// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Test-only JSON provider for MIDR TED snapshots.
 */

#include <zebra.h>

#include <errno.h>
#include <stdarg.h>

#include <json-c/json.h>

#include "bgpd/bgp_midr_ted_private.h"
#include "tests/bgpd/midr_ted_mock_provider.h"

struct mock_membership {
	uint32_t node_id;
	uint32_t group_id;
};

static int mock_error(char *error, size_t error_size, const char *format, ...)
	__attribute__((format(printf, 3, 4)));

static int mock_error(char *error, size_t error_size, const char *format, ...)
{
	va_list args;

	if (error && error_size) {
		va_start(args, format);
		vsnprintf(error, error_size, format, args);
		va_end(args);
	}
	return -EINVAL;
}

static int mock_require_fields(struct json_object *object, const char *const *fields,
			       size_t field_count, const char *where, char *error,
			       size_t error_size)
{
	struct json_object *value;
	size_t i;

	if (!object || json_object_get_type(object) != json_type_object)
		return mock_error(error, error_size, "%s must be an object", where);
	if ((size_t)json_object_object_length(object) != field_count)
		return mock_error(error, error_size, "%s has missing or unknown fields", where);
	for (i = 0; i < field_count; i++)
		if (!json_object_object_get_ex(object, fields[i], &value))
			return mock_error(error, error_size, "%s is missing field '%s'", where,
					  fields[i]);
	return 0;
}

static int mock_get_value(struct json_object *object, const char *field, enum json_type type,
			  struct json_object **out, const char *where, char *error,
			  size_t error_size)
{
	if (!json_object_object_get_ex(object, field, out) || json_object_get_type(*out) != type)
		return mock_error(error, error_size, "%s.%s has invalid type", where, field);
	return 0;
}

static int mock_get_u64(struct json_object *object, const char *field, uint64_t maximum,
			uint64_t *out, const char *where, char *error, size_t error_size)
{
	struct json_object *value;
	int64_t signed_value;
	uint64_t unsigned_value;
	int ret;

	ret = mock_get_value(object, field, json_type_int, &value, where, error, error_size);
	if (ret)
		return ret;
	signed_value = json_object_get_int64(value);
	if (signed_value < 0)
		return mock_error(error, error_size, "%s.%s must be non-negative", where, field);
	unsigned_value = json_object_get_uint64(value);
	if (unsigned_value > maximum)
		return mock_error(error, error_size, "%s.%s is out of range", where, field);
	*out = unsigned_value;
	return 0;
}

static int mock_get_u32(struct json_object *object, const char *field, uint32_t minimum,
			uint32_t maximum, uint32_t *out, const char *where, char *error,
			size_t error_size)
{
	uint64_t value;
	int ret;

	ret = mock_get_u64(object, field, maximum, &value, where, error, error_size);
	if (ret)
		return ret;
	if (value < minimum)
		return mock_error(error, error_size, "%s.%s is out of range", where, field);
	*out = value;
	return 0;
}

static int mock_get_string(struct json_object *object, const char *field, const char **out,
			   const char *where, char *error, size_t error_size)
{
	struct json_object *value;
	int ret;

	ret = mock_get_value(object, field, json_type_string, &value, where, error, error_size);
	if (ret)
		return ret;
	*out = json_object_get_string(value);
	if (!**out)
		return mock_error(error, error_size, "%s.%s must not be empty", where, field);
	return 0;
}

static int mock_get_node_id(struct json_object *object, const char *field, uint32_t *out,
			    const char *where, char *error, size_t error_size)
{
	const char *text;
	struct in_addr address;
	int ret;

	ret = mock_get_string(object, field, &text, where, error, error_size);
	if (ret)
		return ret;
	if (inet_pton(AF_INET, text, &address) != 1 || !address.s_addr)
		return mock_error(error, error_size, "%s.%s is not a non-zero Router-ID", where,
				  field);
	*out = address.s_addr;
	return 0;
}

static const struct mock_membership *mock_find_node(const struct mock_membership *members,
						    size_t member_count, uint32_t node_id)
{
	size_t i;

	for (i = 0; i < member_count; i++)
		if (members[i].node_id == node_id)
			return &members[i];
	return NULL;
}

static bool mock_group_exists(const struct mock_membership *members, size_t member_count,
			      uint32_t group_id)
{
	size_t i;

	for (i = 0; i < member_count; i++)
		if (members[i].group_id == group_id)
			return true;
	return false;
}

static int mock_parse_prefix_key(struct json_object *object, struct midr_ted_prefix_key *key,
				 const char *where, char *error, size_t error_size)
{
	const char *text;
	int ret;

	ret = mock_get_string(object, "prefix", &text, where, error, error_size);
	if (ret)
		return ret;
	memset(key, 0, sizeof(*key));
	if (str2prefix(text, &key->prefix) <= 0)
		return mock_error(error, error_size, "%s.prefix is invalid", where);
	if (key->prefix.family == AF_INET)
		key->afi = AFI_IP;
	else if (key->prefix.family == AF_INET6)
		key->afi = AFI_IP6;
	else
		return mock_error(error, error_size, "%s.prefix has unsupported AFI", where);
	key->safi = SAFI_UNICAST;
	return 0;
}

static int mock_parse_nodes(struct midr_ted_builder *builder, struct json_object *array,
			    struct mock_membership **members_out, size_t *member_count_out,
			    char *error, size_t error_size)
{
	static const char *const fields[] = {
		"node_id",
		"group_id",
		"cap_flags",
		"policy_tags",
	};
	struct mock_membership *members = NULL;
	size_t count;
	size_t i;
	int ret;

	if (json_object_get_type(array) != json_type_array)
		return mock_error(error, error_size, "nodes must be an array");
	count = json_object_array_length(array);
	if (count > SIZE_MAX / sizeof(*members))
		return -EOVERFLOW;
	members = calloc(count, sizeof(*members));
	if (count && !members)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		struct json_object *object = json_object_array_get_idx(array, i);
		struct midr_ted_node node = {};
		uint64_t value;
		char where[64];

		snprintf(where, sizeof(where), "nodes[%zu]", i);
		ret = mock_require_fields(object, fields, array_size(fields), where, error,
					  error_size);
		if (ret)
			goto fail;
		ret = mock_get_node_id(object, "node_id", &node.node_id, where, error, error_size);
		if (ret)
			goto fail;
		ret = mock_get_u32(object, "group_id", 1, UINT32_MAX, &node.group_id, where, error,
				   error_size);
		if (ret)
			goto fail;
		ret = mock_get_u64(object, "cap_flags", UINT64_MAX, &value, where, error,
				   error_size);
		if (ret)
			goto fail;
		node.cap_flags = value;
		ret = mock_get_u64(object, "policy_tags", UINT64_MAX, &value, where, error,
				   error_size);
		if (ret)
			goto fail;
		node.policy_tags = value;

		ret = midr_ted_builder_add_node(builder, &node);
		if (ret) {
			mock_error(error, error_size, "%s is duplicate or invalid", where);
			goto fail;
		}
		members[i].node_id = node.node_id;
		members[i].group_id = node.group_id;
	}

	*members_out = members;
	*member_count_out = count;
	return 0;

fail:
	free(members);
	return ret;
}

static int mock_parse_links(struct midr_ted_builder *builder, struct json_object *array,
			    const struct mock_membership *members, size_t member_count,
			    uint32_t local_node_id, char *error, size_t error_size)
{
	static const char *const fields[] = {
		"local",
		"remote",
		"link_id",
		"local_address",
		"remote_address",
		"cost",
		"local_ifindex",
		"policy_tags",
	};
	size_t count;
	size_t i;

	if (json_object_get_type(array) != json_type_array)
		return mock_error(error, error_size, "links must be an array");
	count = json_object_array_length(array);
	for (i = 0; i < count; i++) {
		struct json_object *object = json_object_array_get_idx(array, i);
		struct midr_ted_link_input link = {};
		const char *text;
		uint64_t value;
		uint32_t value32;
		char where[64];
		int ret;

		snprintf(where, sizeof(where), "links[%zu]", i);
		ret = mock_require_fields(object, fields, array_size(fields), where, error,
					  error_size);
		if (ret)
			return ret;
		ret = mock_get_node_id(object, "local", &link.local_node_id, where, error,
				       error_size);
		if (ret)
			return ret;
		ret = mock_get_node_id(object, "remote", &link.remote_node_id, where, error,
				       error_size);
		if (ret)
			return ret;
		if (!mock_find_node(members, member_count, link.local_node_id) ||
		    !mock_find_node(members, member_count, link.remote_node_id))
			return mock_error(error, error_size, "%s references an unknown node",
					  where);

		ret = mock_get_u64(object, "link_id", UINT64_MAX, &value, where, error, error_size);
		if (ret)
			return ret;
		link.link_id = value;
		ret = mock_get_string(object, "local_address", &text, where, error, error_size);
		if (ret)
			return ret;
		if (str2ipaddr(text, &link.link_local_address) != 0)
			return mock_error(error, error_size, "%s.local_address is invalid", where);
		ret = mock_get_string(object, "remote_address", &text, where, error, error_size);
		if (ret)
			return ret;
		if (str2ipaddr(text, &link.link_remote_address) != 0)
			return mock_error(error, error_size, "%s.remote_address is invalid", where);
		ret = mock_get_u32(object, "cost", 1, MIDR_TED_LINK_COST_MAX, &link.canonical_cost,
				   where, error, error_size);
		if (ret)
			return ret;
		ret = mock_get_u32(object, "local_ifindex", 0, INT32_MAX, &value32, where, error,
				   error_size);
		if (ret)
			return ret;
		link.local_ifindex = value32;
		if (link.local_node_id != local_node_id && link.local_ifindex)
			return mock_error(error, error_size,
					  "%s.local_ifindex is only valid for the local node",
					  where);
		ret = mock_get_u64(object, "policy_tags", UINT64_MAX, &value, where, error,
				   error_size);
		if (ret)
			return ret;
		link.policy_tags = value;

		ret = midr_ted_builder_add_link(builder, &link);
		if (ret)
			return mock_error(error, error_size, "%s is duplicate or invalid", where);
	}
	return 0;
}

static int mock_parse_node_prefixes(struct midr_ted_builder *builder, struct json_object *array,
				    const struct mock_membership *members, size_t member_count,
				    char *error, size_t error_size)
{
	static const char *const fields[] = {
		"prefix",
		"node_id",
	};
	size_t count;
	size_t i;

	if (json_object_get_type(array) != json_type_array)
		return mock_error(error, error_size, "node_prefixes must be an array");
	count = json_object_array_length(array);
	for (i = 0; i < count; i++) {
		struct json_object *object = json_object_array_get_idx(array, i);
		struct midr_ted_node_prefix prefix = {};
		char where[64];
		int ret;

		snprintf(where, sizeof(where), "node_prefixes[%zu]", i);
		ret = mock_require_fields(object, fields, array_size(fields), where, error,
					  error_size);
		if (ret)
			return ret;
		ret = mock_parse_prefix_key(object, &prefix.key, where, error, error_size);
		if (ret)
			return ret;
		ret = mock_get_node_id(object, "node_id", &prefix.node_id, where, error,
				       error_size);
		if (ret)
			return ret;
		if (!mock_find_node(members, member_count, prefix.node_id))
			return mock_error(error, error_size, "%s references an unknown node",
					  where);
		ret = midr_ted_builder_add_node_prefix(builder, &prefix);
		if (ret)
			return mock_error(error, error_size, "%s is duplicate or invalid", where);
	}
	return 0;
}

static int mock_parse_prefix_groups(struct midr_ted_builder *builder, struct json_object *array,
				    const struct mock_membership *members, size_t member_count,
				    char *error, size_t error_size)
{
	static const char *const fields[] = {
		"prefix",
		"group_id",
	};
	size_t count;
	size_t i;

	if (json_object_get_type(array) != json_type_array)
		return mock_error(error, error_size, "prefix_groups must be an array");
	count = json_object_array_length(array);
	for (i = 0; i < count; i++) {
		struct json_object *object = json_object_array_get_idx(array, i);
		struct midr_ted_prefix_group prefix = {};
		char where[64];
		int ret;

		snprintf(where, sizeof(where), "prefix_groups[%zu]", i);
		ret = mock_require_fields(object, fields, array_size(fields), where, error,
					  error_size);
		if (ret)
			return ret;
		ret = mock_parse_prefix_key(object, &prefix.key, where, error, error_size);
		if (ret)
			return ret;
		ret = mock_get_u32(object, "group_id", 1, UINT32_MAX, &prefix.group_id, where,
				   error, error_size);
		if (ret)
			return ret;
		if (!mock_group_exists(members, member_count, prefix.group_id))
			return mock_error(error, error_size, "%s references an unknown group",
					  where);
		ret = midr_ted_builder_add_prefix_group(builder, &prefix);
		if (ret)
			return mock_error(error, error_size, "%s is duplicate or invalid", where);
	}
	return 0;
}

int midr_ted_mock_provider_publish_file(struct midr_context *ctx, const char *path, char *error,
					size_t error_size)
{
	static const char *const fields[] = {
		"schema_version", "local_node_id", "local_group_id", "nodes",
		"links",	  "node_prefixes", "prefix_groups",
	};
	struct midr_ted_builder *builder = NULL;
	struct mock_membership *members = NULL;
	struct json_object *root = NULL;
	struct json_object *array;
	uint32_t local_node_id = 0;
	uint32_t local_group_id = 0;
	uint32_t schema_version = 0;
	size_t member_count = 0;
	int ret;

	if (error && error_size)
		error[0] = '\0';
	if (!ctx || !path)
		return mock_error(error, error_size, "invalid provider arguments");

	root = json_object_from_file(path);
	if (!root)
		return mock_error(error, error_size, "unable to parse normalized JSON");
	ret = mock_require_fields(root, fields, array_size(fields), "root", error, error_size);
	if (ret)
		goto done;
	ret = mock_get_u32(root, "schema_version", 1, 1, &schema_version, "root", error,
			   error_size);
	if (ret)
		goto done;
	ret = mock_get_node_id(root, "local_node_id", &local_node_id, "root", error, error_size);
	if (ret)
		goto done;
	ret = mock_get_u32(root, "local_group_id", 1, UINT32_MAX, &local_group_id, "root", error,
			   error_size);
	if (ret)
		goto done;

	ret = midr_ted_builder_create(local_node_id, local_group_id, &builder);
	if (ret) {
		ret = mock_error(error, error_size, "unable to create TED builder");
		goto done;
	}

	json_object_object_get_ex(root, "nodes", &array);
	ret = mock_parse_nodes(builder, array, &members, &member_count, error, error_size);
	if (ret)
		goto done;
	{
		const struct mock_membership *local = mock_find_node(members, member_count,
								     local_node_id);

		if (!local || local->group_id != local_group_id) {
			ret = mock_error(error, error_size,
					 "local node membership does not match local_group_id");
			goto done;
		}
	}

	json_object_object_get_ex(root, "links", &array);
	ret = mock_parse_links(builder, array, members, member_count, local_node_id, error,
			       error_size);
	if (ret)
		goto done;
	json_object_object_get_ex(root, "node_prefixes", &array);
	ret = mock_parse_node_prefixes(builder, array, members, member_count, error, error_size);
	if (ret)
		goto done;
	json_object_object_get_ex(root, "prefix_groups", &array);
	ret = mock_parse_prefix_groups(builder, array, members, member_count, error, error_size);
	if (ret)
		goto done;

	ret = midr_ted_builder_publish(ctx, builder, 0);
	if (ret)
		mock_error(error, error_size, "unable to publish TED snapshot");

done:
	free(members);
	midr_ted_builder_destroy(&builder);
	json_object_put(root);
	return ret;
}
