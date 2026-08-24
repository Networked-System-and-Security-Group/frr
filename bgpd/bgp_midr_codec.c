// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR NLRI and path-attribute value codecs.
 */

#include <zebra.h>

#include <errno.h>

#include "iana_afi.h"
#include "memory.h"

#include "bgpd/bgp_memory.h"
#include "bgpd/bgp_midr_codec.h"

DEFINE_MTYPE_STATIC(BGPD, MIDR_PROPAGATION_PATH, "MIDR propagation path");

#define MIDR_NLRI_HEADER_LENGTH 4U
#define MIDR_NLRI_ORIGINATOR_LENGTH 4U
#define MIDR_TLV_HEADER_LENGTH 4U

static bool midr_codec_stream_can_write(const struct stream *stream, size_t length)
{
	return stream && STREAM_WRITEABLE(stream) >= length;
}

static size_t midr_codec_prefix_byte_length(uint16_t prefix_length)
{
	return (prefix_length + 7U) / 8U;
}

static size_t midr_codec_nlri_value_length(const struct midr_ls_object_key *key)
{
	size_t prefix_bytes;

	switch (key->type) {
	case MIDR_NLRI_TYPE_MEMBERSHIP:
		return MIDR_NLRI_ORIGINATOR_LENGTH;
	case MIDR_NLRI_TYPE_LINK:
		return MIDR_NLRI_ORIGINATOR_LENGTH + 4 + 8;
	case MIDR_NLRI_TYPE_NODE_PREFIX:
		prefix_bytes = midr_codec_prefix_byte_length(key->u.node_prefix.prefix.prefixlen);
		return MIDR_NLRI_ORIGINATOR_LENGTH + 2 + 1 + 1 + prefix_bytes;
	case MIDR_NLRI_TYPE_GROUP_PREFIX:
		prefix_bytes =
			midr_codec_prefix_byte_length(key->u.group_prefix.prefix.prefix.prefixlen);
		return MIDR_NLRI_ORIGINATOR_LENGTH + 4 + 2 + 1 + 1 + prefix_bytes;
	case MIDR_NLRI_TYPE_RESERVED:
	default:
		return 0;
	}
}

static void midr_codec_put_prefix(struct stream *stream, const struct midr_ls_prefix_key *key)
{
	size_t bytes = midr_codec_prefix_byte_length(key->prefix.prefixlen);
	uint16_t wire_afi = key->afi == AFI_IP ? IANA_AFI_IPV4 : IANA_AFI_IPV6;

	stream_putw(stream, wire_afi);
	stream_putc(stream, IANA_SAFI_UNICAST);
	stream_putc(stream, key->prefix.prefixlen);
	stream_put(stream, &key->prefix.u.prefix, bytes);
}

enum midr_codec_result midr_nlri_encode(struct stream *stream, const struct midr_ls_object_key *key)
{
	size_t value_length;
	size_t total_length;

	if (midr_ls_object_key_validate(key) != 0)
		return MIDR_CODEC_MALFORMED_NLRI;

	value_length = midr_codec_nlri_value_length(key);
	total_length = MIDR_NLRI_HEADER_LENGTH + value_length;
	if (value_length > UINT16_MAX)
		return MIDR_CODEC_OVERFLOW;
	if (!midr_codec_stream_can_write(stream, total_length))
		return MIDR_CODEC_NO_SPACE;

	stream_putw(stream, key->type);
	stream_putw(stream, value_length);
	stream_putl(stream, ntohl(key->originator_node_id));

	switch (key->type) {
	case MIDR_NLRI_TYPE_MEMBERSHIP:
		break;
	case MIDR_NLRI_TYPE_LINK:
		stream_putl(stream, ntohl(key->u.link.remote_node_id));
		stream_putq(stream, key->u.link.link_id);
		break;
	case MIDR_NLRI_TYPE_NODE_PREFIX:
		midr_codec_put_prefix(stream, &key->u.node_prefix);
		break;
	case MIDR_NLRI_TYPE_GROUP_PREFIX:
		stream_putl(stream, key->u.group_prefix.group_id);
		midr_codec_put_prefix(stream, &key->u.group_prefix.prefix);
		break;
	case MIDR_NLRI_TYPE_RESERVED:
	default:
		return MIDR_CODEC_MALFORMED_NLRI;
	}

	return MIDR_CODEC_OK;
}

static enum midr_codec_result midr_codec_get_prefix(struct stream *stream, size_t offset,
						    size_t length, struct midr_ls_prefix_key *key)
{
	size_t prefix_bytes;
	uint16_t wire_afi;
	uint8_t wire_safi;
	uint8_t prefix_length;

	if (length < 4)
		return MIDR_CODEC_MALFORMED_NLRI;

	wire_afi = stream_getw_from(stream, offset);
	wire_safi = stream_getc_from(stream, offset + 2);
	prefix_length = stream_getc_from(stream, offset + 3);
	if (wire_safi != IANA_SAFI_UNICAST)
		return MIDR_CODEC_MALFORMED_NLRI;

	memset(key, 0, sizeof(*key));
	key->safi = SAFI_UNICAST;
	if (wire_afi == IANA_AFI_IPV4) {
		if (prefix_length > IPV4_MAX_BITLEN)
			return MIDR_CODEC_MALFORMED_NLRI;
		key->afi = AFI_IP;
		key->prefix.family = AF_INET;
	} else if (wire_afi == IANA_AFI_IPV6) {
		if (prefix_length > IPV6_MAX_BITLEN)
			return MIDR_CODEC_MALFORMED_NLRI;
		key->afi = AFI_IP6;
		key->prefix.family = AF_INET6;
	} else {
		return MIDR_CODEC_MALFORMED_NLRI;
	}

	prefix_bytes = midr_codec_prefix_byte_length(prefix_length);
	if (length != 4 + prefix_bytes)
		return MIDR_CODEC_MALFORMED_NLRI;
	key->prefix.prefixlen = prefix_length;
	stream_get_from(&key->prefix.u.prefix, stream, offset + 4, prefix_bytes);
	if (!midr_ls_prefix_key_is_canonical(key))
		return MIDR_CODEC_MALFORMED_NLRI;
	return MIDR_CODEC_OK;
}

enum midr_codec_result midr_nlri_decode(struct stream *stream, struct midr_ls_object_key *key)
{
	struct midr_ls_object_key decoded = {};
	enum midr_codec_result result;
	size_t start;
	size_t total_length;
	size_t value_length;
	uint16_t type;

	if (!key)
		return MIDR_CODEC_MALFORMED_NLRI;
	memset(key, 0, sizeof(*key));
	if (!stream || STREAM_READABLE(stream) < MIDR_NLRI_HEADER_LENGTH)
		return MIDR_CODEC_MALFORMED_NLRI;

	start = stream_get_getp(stream);
	type = stream_getw_from(stream, start);
	value_length = stream_getw_from(stream, start + 2);
	total_length = MIDR_NLRI_HEADER_LENGTH + value_length;
	if (STREAM_READABLE(stream) < total_length)
		return MIDR_CODEC_MALFORMED_NLRI;

	if (type < MIDR_NLRI_TYPE_MEMBERSHIP || type > MIDR_NLRI_TYPE_GROUP_PREFIX) {
		memset(key, 0, sizeof(*key));
		stream_forward_getp(stream, total_length);
		return MIDR_CODEC_UNKNOWN_NLRI_TYPE;
	}
	if (value_length < MIDR_NLRI_ORIGINATOR_LENGTH)
		return MIDR_CODEC_MALFORMED_NLRI;

	decoded.type = type;
	decoded.originator_node_id = htonl(stream_getl_from(stream, start + 4));

	switch (decoded.type) {
	case MIDR_NLRI_TYPE_MEMBERSHIP:
		if (value_length != 4)
			return MIDR_CODEC_MALFORMED_NLRI;
		break;
	case MIDR_NLRI_TYPE_LINK:
		if (value_length != 16)
			return MIDR_CODEC_MALFORMED_NLRI;
		decoded.u.link.remote_node_id = htonl(stream_getl_from(stream, start + 8));
		decoded.u.link.link_id = stream_getq_from(stream, start + 12);
		break;
	case MIDR_NLRI_TYPE_NODE_PREFIX:
		result = midr_codec_get_prefix(stream, start + 8, value_length - 4,
					       &decoded.u.node_prefix);
		if (result != MIDR_CODEC_OK)
			return result;
		break;
	case MIDR_NLRI_TYPE_GROUP_PREFIX:
		if (value_length < 8)
			return MIDR_CODEC_MALFORMED_NLRI;
		decoded.u.group_prefix.group_id = stream_getl_from(stream, start + 8);
		result = midr_codec_get_prefix(stream, start + 12, value_length - 8,
					       &decoded.u.group_prefix.prefix);
		if (result != MIDR_CODEC_OK)
			return result;
		break;
	case MIDR_NLRI_TYPE_RESERVED:
	default:
		return MIDR_CODEC_MALFORMED_NLRI;
	}

	if (midr_ls_object_key_validate(&decoded) != 0)
		return MIDR_CODEC_MALFORMED_NLRI;

	*key = decoded;
	stream_forward_getp(stream, total_length);
	return MIDR_CODEC_OK;
}

static size_t midr_codec_address_value_length(const struct ipaddr *address)
{
	return address->ipa_type == IPADDR_V4 ? 2 + IPV4_MAX_BYTELEN : 2 + IPV6_MAX_BYTELEN;
}

static void midr_codec_put_tlv_header(struct stream *stream, uint16_t type, uint16_t length)
{
	stream_putw(stream, type);
	stream_putw(stream, length);
}

static void midr_codec_put_address(struct stream *stream, uint16_t type,
				   const struct ipaddr *address)
{
	size_t address_length = address->ipa_type == IPADDR_V4 ? IPV4_MAX_BYTELEN
							       : IPV6_MAX_BYTELEN;
	uint16_t wire_afi = address->ipa_type == IPADDR_V4 ? IANA_AFI_IPV4 : IANA_AFI_IPV6;

	midr_codec_put_tlv_header(stream, type, midr_codec_address_value_length(address));
	stream_putw(stream, wire_afi);
	stream_put(stream, address->ip.addrbytes, address_length);
}

static size_t midr_codec_ls_attribute_length(const struct midr_ls_object *object)
{
	size_t length = MIDR_TLV_HEADER_LENGTH + 8;

	if (object->policy_tags)
		length += MIDR_TLV_HEADER_LENGTH + 8;

	switch (object->key.type) {
	case MIDR_NLRI_TYPE_MEMBERSHIP:
		length += MIDR_TLV_HEADER_LENGTH + 4;
		if (object->payload.membership.has_transport_address)
			length += MIDR_TLV_HEADER_LENGTH +
				  midr_codec_address_value_length(
					  &object->payload.membership.transport_address);
		length += MIDR_TLV_HEADER_LENGTH + 8;
		break;
	case MIDR_NLRI_TYPE_LINK:
		length += MIDR_TLV_HEADER_LENGTH +
			  midr_codec_address_value_length(&object->payload.link.link_local_address);
		length +=
			MIDR_TLV_HEADER_LENGTH +
			midr_codec_address_value_length(&object->payload.link.link_remote_address);
		length += MIDR_TLV_HEADER_LENGTH + 4;
		break;
	case MIDR_NLRI_TYPE_NODE_PREFIX:
	case MIDR_NLRI_TYPE_GROUP_PREFIX:
		break;
	case MIDR_NLRI_TYPE_RESERVED:
	default:
		return 0;
	}
	return length;
}

enum midr_codec_result midr_ls_attribute_encode(struct stream *stream,
						const struct midr_ls_object *object)
{
	size_t length;

	if (midr_ls_object_validate(object) != 0)
		return MIDR_CODEC_MALFORMED_ATTRIBUTE;

	length = midr_codec_ls_attribute_length(object);
	if (!midr_codec_stream_can_write(stream, length))
		return MIDR_CODEC_NO_SPACE;

	midr_codec_put_tlv_header(stream, MIDR_LS_TLV_SEQUENCE, 8);
	stream_putq(stream, object->ls_sequence);
	if (object->policy_tags) {
		midr_codec_put_tlv_header(stream, MIDR_LS_TLV_POLICY_TAGS, 8);
		stream_putq(stream, object->policy_tags);
	}

	switch (object->key.type) {
	case MIDR_NLRI_TYPE_MEMBERSHIP:
		midr_codec_put_tlv_header(stream, MIDR_LS_TLV_GROUP_ID, 4);
		stream_putl(stream, object->payload.membership.group_id);
		if (object->payload.membership.has_transport_address)
			midr_codec_put_address(stream, MIDR_LS_TLV_TRANSPORT_ADDRESS,
					       &object->payload.membership.transport_address);
		midr_codec_put_tlv_header(stream, MIDR_LS_TLV_CAP_FLAGS, 8);
		stream_putq(stream, object->payload.membership.cap_flags);
		break;
	case MIDR_NLRI_TYPE_LINK:
		midr_codec_put_address(stream, MIDR_LS_TLV_LINK_LOCAL_ADDRESS,
				       &object->payload.link.link_local_address);
		midr_codec_put_address(stream, MIDR_LS_TLV_LINK_REMOTE_ADDRESS,
				       &object->payload.link.link_remote_address);
		midr_codec_put_tlv_header(
			stream, MIDR_LS_TLV_LINK_CANONICAL_COST, 4);
		stream_putl(stream, object->payload.link.canonical_cost);
		break;
	case MIDR_NLRI_TYPE_NODE_PREFIX:
	case MIDR_NLRI_TYPE_GROUP_PREFIX:
		break;
	case MIDR_NLRI_TYPE_RESERVED:
	default:
		return MIDR_CODEC_MALFORMED_ATTRIBUTE;
	}

	return MIDR_CODEC_OK;
}

static enum midr_codec_result midr_codec_get_address(struct stream *stream, size_t offset,
						     size_t length, struct ipaddr *address)
{
	uint16_t wire_afi;
	size_t address_length;

	if (length < 2)
		return MIDR_CODEC_MALFORMED_ATTRIBUTE;
	wire_afi = stream_getw_from(stream, offset);
	if (wire_afi == IANA_AFI_IPV4)
		address_length = IPV4_MAX_BYTELEN;
	else if (wire_afi == IANA_AFI_IPV6)
		address_length = IPV6_MAX_BYTELEN;
	else
		return MIDR_CODEC_MALFORMED_ATTRIBUTE;
	if (length != 2 + address_length)
		return MIDR_CODEC_MALFORMED_ATTRIBUTE;

	memset(address, 0, sizeof(*address));
	address->ipa_type = wire_afi == IANA_AFI_IPV4 ? IPADDR_V4 : IPADDR_V6;
	stream_get_from(address->ip.addrbytes, stream, offset + 2, address_length);
	return MIDR_CODEC_OK;
}

static bool midr_codec_attribute_is_duplicate(uint32_t present, uint32_t flag)
{
	return (present & flag) != 0;
}

enum midr_codec_result midr_ls_attribute_decode(struct stream *stream, size_t length,
						struct midr_ls_attributes *attributes)
{
	struct midr_ls_attributes decoded = {};
	enum midr_codec_result result;
	size_t start;
	size_t offset = 0;
	size_t value_offset;
	uint16_t type;
	uint16_t previous_type = 0;
	uint16_t value_length;
	uint32_t presence;

	if (!attributes)
		return MIDR_CODEC_MALFORMED_ATTRIBUTE;
	memset(attributes, 0, sizeof(*attributes));
	if (!stream || STREAM_READABLE(stream) < length)
		return MIDR_CODEC_MALFORMED_ATTRIBUTE;
	start = stream_get_getp(stream);

	while (offset < length) {
		if (length - offset < MIDR_TLV_HEADER_LENGTH)
			return MIDR_CODEC_MALFORMED_ATTRIBUTE;
		type = stream_getw_from(stream, start + offset);
		value_length = stream_getw_from(stream, start + offset + 2);
		if (value_length > length - offset - MIDR_TLV_HEADER_LENGTH)
			return MIDR_CODEC_MALFORMED_ATTRIBUTE;
		if (offset && type <= previous_type)
			return MIDR_CODEC_MALFORMED_ATTRIBUTE;
		value_offset = start + offset + MIDR_TLV_HEADER_LENGTH;

		switch (type) {
		case MIDR_LS_TLV_SEQUENCE:
			presence = MIDR_LS_ATTR_HAS_SEQUENCE;
			if (value_length != 8 ||
			    midr_codec_attribute_is_duplicate(decoded.present, presence))
				return MIDR_CODEC_MALFORMED_ATTRIBUTE;
			decoded.ls_sequence = stream_getq_from(stream, value_offset);
			break;
		case MIDR_LS_TLV_POLICY_TAGS:
			presence = MIDR_LS_ATTR_HAS_POLICY_TAGS;
			if (value_length != 8 ||
			    midr_codec_attribute_is_duplicate(decoded.present, presence))
				return MIDR_CODEC_MALFORMED_ATTRIBUTE;
			decoded.policy_tags = stream_getq_from(stream, value_offset);
			break;
		case MIDR_LS_TLV_GROUP_ID:
			presence = MIDR_LS_ATTR_HAS_GROUP_ID;
			if (value_length != 4 ||
			    midr_codec_attribute_is_duplicate(decoded.present, presence))
				return MIDR_CODEC_MALFORMED_ATTRIBUTE;
			decoded.group_id = stream_getl_from(stream, value_offset);
			break;
		case MIDR_LS_TLV_TRANSPORT_ADDRESS:
			presence = MIDR_LS_ATTR_HAS_TRANSPORT_ADDRESS;
			if (midr_codec_attribute_is_duplicate(decoded.present, presence))
				return MIDR_CODEC_MALFORMED_ATTRIBUTE;
			result = midr_codec_get_address(stream, value_offset, value_length,
							&decoded.transport_address);
			if (result != MIDR_CODEC_OK)
				return result;
			break;
		case MIDR_LS_TLV_CAP_FLAGS:
			presence = MIDR_LS_ATTR_HAS_CAP_FLAGS;
			if (value_length != 8 ||
			    midr_codec_attribute_is_duplicate(decoded.present, presence))
				return MIDR_CODEC_MALFORMED_ATTRIBUTE;
			decoded.cap_flags = stream_getq_from(stream, value_offset);
			break;
		case MIDR_LS_TLV_LINK_LOCAL_ADDRESS:
			presence = MIDR_LS_ATTR_HAS_LINK_LOCAL_ADDRESS;
			if (midr_codec_attribute_is_duplicate(decoded.present, presence))
				return MIDR_CODEC_MALFORMED_ATTRIBUTE;
			result = midr_codec_get_address(stream, value_offset, value_length,
							&decoded.link_local_address);
			if (result != MIDR_CODEC_OK)
				return result;
			break;
		case MIDR_LS_TLV_LINK_REMOTE_ADDRESS:
			presence = MIDR_LS_ATTR_HAS_LINK_REMOTE_ADDRESS;
			if (midr_codec_attribute_is_duplicate(decoded.present, presence))
				return MIDR_CODEC_MALFORMED_ATTRIBUTE;
			result = midr_codec_get_address(stream, value_offset, value_length,
							&decoded.link_remote_address);
			if (result != MIDR_CODEC_OK)
				return result;
			break;
		case MIDR_LS_TLV_LINK_CANONICAL_COST:
			presence = MIDR_LS_ATTR_HAS_LINK_CANONICAL_COST;
			if (value_length != 4 ||
			    midr_codec_attribute_is_duplicate(decoded.present, presence))
				return MIDR_CODEC_MALFORMED_ATTRIBUTE;
			decoded.link_canonical_cost =
				stream_getl_from(stream, value_offset);
			break;
		default:
			return MIDR_CODEC_UNKNOWN_TLV;
		}

		decoded.present |= presence;
		previous_type = type;
		offset += MIDR_TLV_HEADER_LENGTH + value_length;
	}

	*attributes = decoded;
	stream_forward_getp(stream, length);
	return MIDR_CODEC_OK;
}

enum midr_codec_result midr_ls_object_from_wire(const struct midr_ls_object_key *key,
						const struct midr_ls_attributes *attributes,
						struct midr_ls_object *object)
{
	struct midr_ls_object decoded = {};
	struct midr_ls_object_key decoded_key;
	uint32_t required;
	uint32_t allowed;

	if (!object)
		return MIDR_CODEC_MALFORMED_ATTRIBUTE;
	if (!key || !attributes) {
		memset(object, 0, sizeof(*object));
		return MIDR_CODEC_MALFORMED_ATTRIBUTE;
	}
	decoded_key = *key;
	memset(object, 0, sizeof(*object));
	if (midr_ls_object_key_validate(&decoded_key) != 0)
		return MIDR_CODEC_MALFORMED_ATTRIBUTE;

	required = MIDR_LS_ATTR_HAS_SEQUENCE;
	allowed = required | MIDR_LS_ATTR_HAS_POLICY_TAGS;
	switch (decoded_key.type) {
	case MIDR_NLRI_TYPE_MEMBERSHIP:
		required |= MIDR_LS_ATTR_HAS_GROUP_ID | MIDR_LS_ATTR_HAS_CAP_FLAGS;
		allowed |= MIDR_LS_ATTR_HAS_GROUP_ID | MIDR_LS_ATTR_HAS_TRANSPORT_ADDRESS |
			   MIDR_LS_ATTR_HAS_CAP_FLAGS;
		break;
	case MIDR_NLRI_TYPE_LINK:
		required |= MIDR_LS_ATTR_HAS_LINK_LOCAL_ADDRESS |
			    MIDR_LS_ATTR_HAS_LINK_REMOTE_ADDRESS |
			    MIDR_LS_ATTR_HAS_LINK_CANONICAL_COST;
		allowed |= MIDR_LS_ATTR_HAS_LINK_LOCAL_ADDRESS |
			   MIDR_LS_ATTR_HAS_LINK_REMOTE_ADDRESS |
			   MIDR_LS_ATTR_HAS_LINK_CANONICAL_COST;
		break;
	case MIDR_NLRI_TYPE_NODE_PREFIX:
	case MIDR_NLRI_TYPE_GROUP_PREFIX:
		break;
	case MIDR_NLRI_TYPE_RESERVED:
	default:
		return MIDR_CODEC_MALFORMED_ATTRIBUTE;
	}

	if ((attributes->present & required) != required || (attributes->present & ~allowed) != 0)
		return MIDR_CODEC_MALFORMED_ATTRIBUTE;

	decoded.key = decoded_key;
	decoded.ls_sequence = attributes->ls_sequence;
	decoded.policy_tags = attributes->policy_tags;
	if (decoded_key.type == MIDR_NLRI_TYPE_MEMBERSHIP) {
		decoded.payload.membership.group_id = attributes->group_id;
		decoded.payload.membership.has_transport_address =
			(attributes->present & MIDR_LS_ATTR_HAS_TRANSPORT_ADDRESS) != 0;
		if (decoded.payload.membership.has_transport_address)
			decoded.payload.membership.transport_address =
				attributes->transport_address;
		else
			SET_IPADDR_NONE(&decoded.payload.membership.transport_address);
		decoded.payload.membership.cap_flags = attributes->cap_flags;
	} else if (decoded_key.type == MIDR_NLRI_TYPE_LINK) {
		decoded.payload.link.link_local_address = attributes->link_local_address;
		decoded.payload.link.link_remote_address = attributes->link_remote_address;
		decoded.payload.link.canonical_cost =
			attributes->link_canonical_cost;
	}

	if (midr_ls_object_validate(&decoded) != 0)
		return MIDR_CODEC_MALFORMED_ATTRIBUTE;
	*object = decoded;
	return MIDR_CODEC_OK;
}

static bool midr_propagation_path_structure_valid(const struct midr_propagation_path *path)
{
	uint16_t left;
	uint16_t right;

	if (!path || !path->nodes || path->node_count < 1 ||
	    path->node_count > MIDR_PROPAGATION_PATH_MAX_NODES || path->capacity < path->node_count)
		return false;

	for (left = 0; left < path->node_count; left++) {
		if (!path->nodes[left])
			return false;
		for (right = left + 1; right < path->node_count; right++)
			if (path->nodes[left] == path->nodes[right])
				return false;
	}
	return true;
}

int midr_propagation_path_init(struct midr_propagation_path *path, uint32_t originator_node_id)
{
	if (!path || !originator_node_id || path->nodes || path->node_count || path->capacity)
		return -EINVAL;

	path->nodes = XCALLOC(MTYPE_MIDR_PROPAGATION_PATH, sizeof(*path->nodes));
	path->nodes[0] = originator_node_id;
	path->node_count = 1;
	path->capacity = 1;
	return 0;
}

void midr_propagation_path_fini(struct midr_propagation_path *path)
{
	if (!path)
		return;
	XFREE(MTYPE_MIDR_PROPAGATION_PATH, path->nodes);
	memset(path, 0, sizeof(*path));
}

bool midr_propagation_path_contains(const struct midr_propagation_path *path, uint32_t node_id)
{
	uint16_t index;

	if (!path || !path->nodes || !node_id || path->node_count < 1 ||
	    path->node_count > MIDR_PROPAGATION_PATH_MAX_NODES || path->capacity < path->node_count)
		return false;
	for (index = 0; index < path->node_count; index++)
		if (path->nodes[index] == node_id)
			return true;
	return false;
}

int midr_propagation_path_append(struct midr_propagation_path *path, uint32_t node_id)
{
	uint16_t capacity;

	if (!midr_propagation_path_structure_valid(path) || !node_id)
		return -EINVAL;
	if (midr_propagation_path_contains(path, node_id))
		return -ELOOP;
	if (path->node_count == MIDR_PROPAGATION_PATH_MAX_NODES)
		return -E2BIG;

	capacity = path->capacity;
	if (capacity == path->node_count) {
		capacity = capacity < 4 ? 4 : capacity * 2;
		if (capacity > MIDR_PROPAGATION_PATH_MAX_NODES)
			capacity = MIDR_PROPAGATION_PATH_MAX_NODES;
		path->nodes = XREALLOC(MTYPE_MIDR_PROPAGATION_PATH, path->nodes,
				       capacity * sizeof(*path->nodes));
		path->capacity = capacity;
	}
	path->nodes[path->node_count++] = node_id;
	return 0;
}

int midr_propagation_path_validate(const struct midr_propagation_path *path,
				   uint32_t originator_node_id, uint32_t sending_peer_node_id,
				   uint32_t local_node_id)
{
	if (!originator_node_id || !sending_peer_node_id || !local_node_id ||
	    !midr_propagation_path_structure_valid(path))
		return -EINVAL;
	if (path->nodes[0] != originator_node_id ||
	    path->nodes[path->node_count - 1] != sending_peer_node_id ||
	    midr_propagation_path_contains(path, local_node_id))
		return -ELOOP;
	return 0;
}

enum midr_codec_result midr_propagation_path_encode(struct stream *stream,
						    const struct midr_propagation_path *path)
{
	size_t length;
	uint16_t index;

	if (!midr_propagation_path_structure_valid(path))
		return MIDR_CODEC_MALFORMED_ATTRIBUTE;
	length = 2 + (size_t)path->node_count * 4;
	if (!midr_codec_stream_can_write(stream, length))
		return MIDR_CODEC_NO_SPACE;

	stream_putw(stream, path->node_count);
	for (index = 0; index < path->node_count; index++)
		stream_putl(stream, ntohl(path->nodes[index]));
	return MIDR_CODEC_OK;
}

enum midr_codec_result midr_propagation_path_decode(struct stream *stream, size_t length,
						    struct midr_propagation_path *path)
{
	struct midr_propagation_path decoded = {};
	size_t start;
	size_t expected_length;
	uint16_t index;
	uint16_t node_count;

	if (!stream || !path || path->nodes || path->node_count || path->capacity ||
	    STREAM_READABLE(stream) < length || length < 2)
		return MIDR_CODEC_MALFORMED_ATTRIBUTE;
	start = stream_get_getp(stream);
	node_count = stream_getw_from(stream, start);
	if (node_count < 1 || node_count > MIDR_PROPAGATION_PATH_MAX_NODES)
		return MIDR_CODEC_MALFORMED_ATTRIBUTE;
	expected_length = 2 + (size_t)node_count * 4;
	if (length != expected_length)
		return MIDR_CODEC_MALFORMED_ATTRIBUTE;

	decoded.nodes = XCALLOC(MTYPE_MIDR_PROPAGATION_PATH, node_count * sizeof(*decoded.nodes));
	decoded.node_count = node_count;
	decoded.capacity = node_count;
	for (index = 0; index < node_count; index++)
		decoded.nodes[index] =
			htonl(stream_getl_from(stream, start + 2 + (size_t)index * 4));

	if (!midr_propagation_path_structure_valid(&decoded)) {
		midr_propagation_path_fini(&decoded);
		return MIDR_CODEC_MALFORMED_ATTRIBUTE;
	}

	*path = decoded;
	stream_forward_getp(stream, length);
	return MIDR_CODEC_OK;
}
