// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR test-only normalized JSON fixture runner.
 */

#include <zebra.h>

#include <json-c/json.h>

#include "bgpd/bgp_midr_ted_private.h"
#include "tests/bgpd/midr_ted_mock_provider.h"

/* Required variables when linking against bgpd/libbgp.a. */
struct zebra_privs_t bgpd_privs = {};
struct event_loop *master;

static const char *node_id_text(uint32_t node_id, char *buffer, size_t buffer_size)
{
	assert(inet_ntop(AF_INET, &node_id, buffer, buffer_size));
	return buffer;
}

static const char *prefix_text(const struct midr_ted_prefix_key *key, char *buffer,
			       size_t buffer_size)
{
	assert(prefix2str(&key->prefix, buffer, buffer_size) != NULL);
	return buffer;
}

static struct json_object *snapshot_node_json(const struct midr_ted_node *node)
{
	struct json_object *object = json_object_new_object();
	char id[INET_ADDRSTRLEN];

	json_object_object_add(object, "node_id",
			       json_object_new_string(node_id_text(node->node_id, id, sizeof(id))));
	json_object_object_add(object, "group_id", json_object_new_uint64(node->group_id));
	json_object_object_add(object, "cap_flags", json_object_new_uint64(node->cap_flags));
	json_object_object_add(object, "policy_tags", json_object_new_uint64(node->policy_tags));
	return object;
}

static struct json_object *snapshot_link_json(const struct midr_ted_link *link)
{
	struct json_object *object = json_object_new_object();
	char local_id[INET_ADDRSTRLEN];
	char remote_id[INET_ADDRSTRLEN];
	char local_address[INET6_ADDRSTRLEN];
	char remote_address[INET6_ADDRSTRLEN];

	json_object_object_add(object, "local",
			       json_object_new_string(node_id_text(link->local_node_id, local_id,
								   sizeof(local_id))));
	json_object_object_add(object, "remote",
			       json_object_new_string(node_id_text(link->remote_node_id, remote_id,
								   sizeof(remote_id))));
	json_object_object_add(object, "local_group_id",
			       json_object_new_uint64(link->local_group_id));
	json_object_object_add(object, "remote_group_id",
			       json_object_new_uint64(link->remote_group_id));
	json_object_object_add(object, "link_id", json_object_new_uint64(link->link_id));
	json_object_object_add(object, "cost", json_object_new_uint64(link->canonical_cost));
	json_object_object_add(object, "available_bandwidth_kbps",
			       json_object_new_uint64(link->available_bandwidth_kbps));
	json_object_object_add(object, "policy_tags", json_object_new_uint64(link->policy_tags));
	json_object_object_add(object, "local_address",
			       json_object_new_string(ipaddr2str(&link->link_local_address,
								 local_address,
								 sizeof(local_address))));
	json_object_object_add(object, "remote_address",
			       json_object_new_string(ipaddr2str(&link->link_remote_address,
								 remote_address,
								 sizeof(remote_address))));
	json_object_object_add(object, "local_ifindex", json_object_new_int64(link->local_ifindex));
	return object;
}

static struct json_object *snapshot_node_prefix_json(const struct midr_ted_node_prefix *attachment)
{
	struct json_object *object = json_object_new_object();
	char prefix[PREFIX_STRLEN];
	char id[INET_ADDRSTRLEN];

	json_object_object_add(object, "prefix",
			       json_object_new_string(
				       prefix_text(&attachment->key, prefix, sizeof(prefix))));
	json_object_object_add(object, "node_id",
			       json_object_new_string(
				       node_id_text(attachment->node_id, id, sizeof(id))));
	return object;
}

static struct json_object *snapshot_group_edge_json(const struct midr_ted_group_edge *edge)
{
	struct json_object *object = json_object_new_object();

	json_object_object_add(object, "source_group_id",
			       json_object_new_uint64(edge->source_group_id));
	json_object_object_add(object, "target_group_id",
			       json_object_new_uint64(edge->target_group_id));
	json_object_object_add(object, "aggregate_cost",
			       json_object_new_uint64(edge->aggregate_cost));
	return object;
}

static struct json_object *snapshot_prefix_group_json(const struct midr_ted_prefix_group *mapping)
{
	struct json_object *object = json_object_new_object();
	char prefix[PREFIX_STRLEN];

	json_object_object_add(object, "prefix",
			       json_object_new_string(
				       prefix_text(&mapping->key, prefix, sizeof(prefix))));
	json_object_object_add(object, "group_id", json_object_new_uint64(mapping->group_id));
	return object;
}

static struct json_object *snapshot_json(const struct midr_ted_snapshot *snapshot,
					 const struct midr_ted_status *status)
{
	struct json_object *root = json_object_new_object();
	struct json_object *array;
	char local_id[INET_ADDRSTRLEN];
	size_t i;

	json_object_object_add(root, "generation", json_object_new_uint64(snapshot->generation));
	json_object_object_add(root, "local_node_id",
			       json_object_new_string(node_id_text(snapshot->local_node_id,
								   local_id, sizeof(local_id))));
	json_object_object_add(root, "local_group_id",
			       json_object_new_uint64(snapshot->local_group_id));
	json_object_object_add(root, "ready", json_object_new_boolean(snapshot->ready));
	json_object_object_add(root, "sync_reason_flags",
			       json_object_new_uint64(snapshot->sync_reason_flags));

	array = json_object_new_array();
	for (i = 0; i < snapshot->node_count; i++)
		json_object_array_add(array, snapshot_node_json(&snapshot->nodes[i]));
	json_object_object_add(root, "nodes", array);

	array = json_object_new_array();
	for (i = 0; i < snapshot->intra_link_count; i++)
		json_object_array_add(array, snapshot_link_json(&snapshot->intra_links[i]));
	json_object_object_add(root, "intra_links", array);

	array = json_object_new_array();
	for (i = 0; i < snapshot->egress_link_count; i++)
		json_object_array_add(array, snapshot_link_json(&snapshot->egress_links[i]));
	json_object_object_add(root, "egress_links", array);

	array = json_object_new_array();
	for (i = 0; i < snapshot->node_prefix_count; i++)
		json_object_array_add(array,
				      snapshot_node_prefix_json(&snapshot->node_prefixes[i]));
	json_object_object_add(root, "node_prefixes", array);

	array = json_object_new_array();
	for (i = 0; i < snapshot->group_edge_count; i++)
		json_object_array_add(array, snapshot_group_edge_json(&snapshot->group_edges[i]));
	json_object_object_add(root, "group_edges", array);

	array = json_object_new_array();
	for (i = 0; i < snapshot->prefix_group_count; i++)
		json_object_array_add(array,
				      snapshot_prefix_group_json(&snapshot->prefix_groups[i]));
	json_object_object_add(root, "prefix_groups", array);

	{
		struct json_object *pending = json_object_new_object();

		json_object_object_add(pending, "links",
				       json_object_new_uint64(status->pending_link_count));
		json_object_object_add(pending, "node_prefixes",
				       json_object_new_uint64(status->pending_node_prefix_count));
		json_object_object_add(pending, "prefix_groups",
				       json_object_new_uint64(status->pending_prefix_group_count));
		json_object_object_add(root, "pending", pending);
	}
	return root;
}

int main(int argc, char **argv)
{
	struct midr_context ctx = {};
	const struct midr_ted_snapshot *snapshot = NULL;
	struct midr_ted_status status;
	struct json_object *output = NULL;
	char error[256];
	int ret;

	if (argc != 2) {
		fprintf(stderr, "usage: %s NORMALIZED_JSON\n", argv[0]);
		return 2;
	}

	ret = midr_ted_context_init(&ctx);
	assert(ret == 0);
	ret = midr_ted_mock_provider_publish_file(&ctx, argv[1], error, sizeof(error));
	if (ret) {
		fprintf(stderr, "mock provider: %s (%d)\n", error, ret);
		midr_ted_context_finish(&ctx);
		return 1;
	}

	assert(midr_ted_snapshot_get(&ctx, &snapshot) == 0);
	assert(midr_ted_status_get(&ctx, &status) == 0);
	output = snapshot_json(snapshot, &status);
	printf("%s\n", json_object_to_json_string_ext(output, JSON_C_TO_STRING_PRETTY));

	json_object_put(output);
	midr_ted_snapshot_release(&snapshot);
	midr_ted_context_finish(&ctx);
	return 0;
}
