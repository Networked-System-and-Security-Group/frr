// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR NLRI and path-attribute value codec tests.
 */

#include <zebra.h>

#include <errno.h>

#include "privs.h"

#include "bgpd/bgp_midr_codec.h"

/* Required variables when linking against bgpd/libbgp.a. */
struct zebra_privs_t bgpd_privs = {};
struct event_loop *master;

static uint32_t router_id(const char *text)
{
	struct in_addr address;

	assert(inet_pton(AF_INET, text, &address) == 1);
	return address.s_addr;
}

static struct ipaddr ip_address(const char *text)
{
	struct ipaddr address;

	assert(str2ipaddr(text, &address) == 0);
	return address;
}

static struct midr_ls_prefix_key prefix_key(const char *text)
{
	struct midr_ls_prefix_key key = {
		.safi = SAFI_UNICAST,
	};

	assert(str2prefix(text, &key.prefix) > 0);
	key.afi = key.prefix.family == AF_INET ? AFI_IP : AFI_IP6;
	assert(midr_ls_prefix_key_normalize(&key, &key) == 0);
	return key;
}

static struct midr_ls_object membership_object(bool transport, uint64_t policy_tags)
{
	struct midr_ls_object object = {
		.key =
			{
				.type = MIDR_NLRI_TYPE_MEMBERSHIP,
				.originator_node_id = router_id("1.1.1.1"),
			},
		.ls_sequence = 1,
		.policy_tags = policy_tags,
		.payload.membership =
			{
				.group_id = 100,
				.has_transport_address = transport,
				.cap_flags = 0,
			},
	};

	if (transport)
		object.payload.membership.transport_address = ip_address("192.0.2.1");
	else
		SET_IPADDR_NONE(&object.payload.membership.transport_address);
	return object;
}

static struct midr_ls_object link_object(void)
{
	return (struct midr_ls_object){
		.key =
			{
				.type = MIDR_NLRI_TYPE_LINK,
				.originator_node_id = router_id("1.1.1.1"),
				.u.link =
					{
						.remote_node_id =
							router_id("2.2.2.2"),
						.link_id =
							0x0102030405060708ULL,
					},
			},
		.ls_sequence = 0x1112131415161718ULL,
		.policy_tags = 0x2122232425262728ULL,
		.payload.link =
			{
				.link_local_address =
					ip_address("2001:db8::1"),
				.link_remote_address =
					ip_address("2001:db8::2"),
				.metrics =
					{
						.present_flags =
							MIDR_METRIC_REQUIRED_MASK,
						.rtt_us = 1000,
						.loss_ppm = 100,
						.available_bandwidth_kbps =
							100000,
					},
			},
	};
}

static struct stream *stream_from_bytes(const uint8_t *bytes, size_t length)
{
	struct stream *stream = stream_new(length ? length : 1);

	stream_put(stream, bytes, length);
	return stream;
}

static void assert_stream_bytes(const struct stream *stream, const uint8_t *expected, size_t length)
{
	assert(stream_get_endp(stream) == length);
	assert(memcmp(STREAM_DATA(stream), expected, length) == 0);
}

static void test_membership_nlri_golden(void)
{
	static const uint8_t golden[] = {
		0x00, 0x01, 0x00, 0x04, 0x01, 0x01, 0x01, 0x01,
	};
	struct midr_ls_object object = membership_object(false, 0);
	struct midr_ls_object_key decoded;
	struct stream *stream = stream_new(sizeof(golden));

	assert(midr_nlri_encode(stream, &object.key) == MIDR_CODEC_OK);
	assert_stream_bytes(stream, golden, sizeof(golden));
	assert(midr_nlri_decode(stream, &decoded) == MIDR_CODEC_OK);
	assert(midr_ls_object_key_same(&object.key, &decoded));
	assert(STREAM_READABLE(stream) == 0);
	stream_free(stream);
}

static void test_other_nlri_round_trip(void)
{
	static const uint8_t link_golden[] = {
		0x00, 0x02, 0x00, 0x10, 0x01, 0x01, 0x01, 0x01, 0x02, 0x02,
		0x02, 0x02, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
	};
	static const uint8_t node_prefix_golden[] = {
		0x00, 0x03, 0x00, 0x0b, 0x01, 0x01, 0x01, 0x01,
		0x00, 0x01, 0x01, 0x18, 0xc0, 0x00, 0x02,
	};
	struct midr_ls_object link = link_object();
	struct midr_ls_object_key keys[] = {
		link.key,
		{
			.type = MIDR_NLRI_TYPE_NODE_PREFIX,
			.originator_node_id = router_id("1.1.1.1"),
			.u.node_prefix = prefix_key("192.0.2.0/24"),
		},
		{
			.type = MIDR_NLRI_TYPE_GROUP_PREFIX,
			.originator_node_id = router_id("3.3.3.3"),
			.u.group_prefix =
				{
					.group_id = 200,
					.prefix =
						prefix_key("2001:db8::/32"),
				},
		},
	};
	struct midr_ls_object_key decoded;
	struct stream *stream;
	size_t index;

	for (index = 0; index < array_size(keys); index++) {
		stream = stream_new(128);
		assert(midr_nlri_encode(stream, &keys[index]) == MIDR_CODEC_OK);
		if (index == 0)
			assert_stream_bytes(stream, link_golden, sizeof(link_golden));
		if (index == 1)
			assert_stream_bytes(stream, node_prefix_golden, sizeof(node_prefix_golden));
		assert(midr_nlri_decode(stream, &decoded) == MIDR_CODEC_OK);
		assert(midr_ls_object_key_same(&keys[index], &decoded));
		stream_free(stream);
	}
}

static void test_nlri_errors_and_atomicity(void)
{
	static const uint8_t unknown[] = {
		0x00, 0x63, 0x00, 0x01, 0xaa,
	};
	static const uint8_t truncated[] = {
		0x00, 0x01, 0x00, 0x04, 0x01,
	};
	static const uint8_t host_bits[] = {
		0x00, 0x03, 0x00, 0x0c, 0x01, 0x01, 0x01, 0x01,
		0x00, 0x01, 0x01, 0x19, 0xc0, 0x00, 0x02, 0x81,
	};
	struct midr_ls_object object = link_object();
	struct midr_ls_object_key decoded = {
		.type = MIDR_NLRI_TYPE_LINK,
	};
	struct stream *stream;

	stream = stream_from_bytes(unknown, sizeof(unknown));
	assert(midr_nlri_decode(stream, &decoded) == MIDR_CODEC_UNKNOWN_NLRI_TYPE);
	assert(decoded.type == MIDR_NLRI_TYPE_RESERVED);
	assert(STREAM_READABLE(stream) == 0);
	stream_free(stream);

	stream = stream_from_bytes(truncated, sizeof(truncated));
	assert(midr_nlri_decode(stream, &decoded) == MIDR_CODEC_MALFORMED_NLRI);
	assert(stream_get_getp(stream) == 0);
	assert(decoded.type == MIDR_NLRI_TYPE_RESERVED);
	stream_free(stream);

	stream = stream_from_bytes(host_bits, sizeof(host_bits));
	assert(midr_nlri_decode(stream, &decoded) == MIDR_CODEC_MALFORMED_NLRI);
	assert(stream_get_getp(stream) == 0);
	stream_free(stream);

	stream = stream_new(19);
	assert(midr_nlri_encode(stream, &object.key) == MIDR_CODEC_NO_SPACE);
	assert(stream_get_endp(stream) == 0);
	stream_free(stream);
}

static void test_nlri_defensive_errors(void)
{
	static const uint8_t short_header[] = { 0x00, 0x01, 0x00 };
	static const uint8_t bad_membership_length[] = {
		0x00, 0x01, 0x00, 0x05, 0x01, 0x01, 0x01, 0x01, 0,
	};
	static const uint8_t zero_originator[] = {
		0x00, 0x01, 0x00, 0x04, 0, 0, 0, 0,
	};
	static const uint8_t bad_prefix_afi[] = {
		0x00, 0x03, 0x00, 0x08, 0x01, 0x01, 0x01, 0x01, 0xff, 0xff, 0x01, 0x00,
	};
	struct midr_ls_object object = membership_object(false, 0);
	struct midr_ls_object_key decoded;
	struct midr_ls_object_key invalid = {};
	struct stream *stream;

	assert(midr_nlri_encode(NULL, &object.key) == MIDR_CODEC_NO_SPACE);
	assert(midr_nlri_encode(NULL, NULL) == MIDR_CODEC_MALFORMED_NLRI);
	assert(midr_nlri_encode(NULL, &invalid) == MIDR_CODEC_MALFORMED_NLRI);
	assert(midr_nlri_decode(NULL, &decoded) == MIDR_CODEC_MALFORMED_NLRI);

	stream = stream_from_bytes(short_header, sizeof(short_header));
	assert(midr_nlri_decode(stream, &decoded) == MIDR_CODEC_MALFORMED_NLRI);
	stream_free(stream);
	stream = stream_from_bytes(bad_membership_length, sizeof(bad_membership_length));
	assert(midr_nlri_decode(stream, &decoded) == MIDR_CODEC_MALFORMED_NLRI);
	stream_free(stream);
	stream = stream_from_bytes(zero_originator, sizeof(zero_originator));
	assert(midr_nlri_decode(stream, &decoded) == MIDR_CODEC_MALFORMED_NLRI);
	stream_free(stream);
	stream = stream_from_bytes(bad_prefix_afi, sizeof(bad_prefix_afi));
	assert(midr_nlri_decode(stream, &decoded) == MIDR_CODEC_MALFORMED_NLRI);
	stream_free(stream);

	stream = stream_new(8);
	assert(midr_nlri_decode(stream, NULL) == MIDR_CODEC_MALFORMED_NLRI);
	stream_free(stream);
}

static void test_membership_attribute_golden(void)
{
	static const uint8_t golden[] = {
		0x00, 0x01, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x01, 0x00, 0x64, 0x00, 0x04, 0x00, 0x00, 0x00, 0x64, 0x00, 0x66,
		0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};
	struct midr_ls_object object = membership_object(false, 0);
	struct midr_ls_object decoded;
	struct midr_ls_attributes attributes;
	struct stream *stream = stream_new(sizeof(golden));

	assert(midr_ls_attribute_encode(stream, &object) == MIDR_CODEC_OK);
	assert_stream_bytes(stream, golden, sizeof(golden));
	assert(midr_ls_attribute_decode(stream, sizeof(golden), &attributes) == MIDR_CODEC_OK);
	assert((attributes.present & MIDR_LS_ATTR_HAS_CAP_FLAGS) != 0);
	assert((attributes.present & MIDR_LS_ATTR_HAS_POLICY_TAGS) == 0);
	assert(midr_ls_object_from_wire(&object.key, &attributes, &decoded) == MIDR_CODEC_OK);
	assert(midr_ls_object_same(&object, &decoded));
	stream_free(stream);
}

static void test_attribute_round_trip(void)
{
	struct midr_ls_object objects[] = {
		membership_object(true, 0x1020304050607080ULL),
		link_object(),
		{
			.key =
				{
					.type = MIDR_NLRI_TYPE_NODE_PREFIX,
					.originator_node_id =
						router_id("1.1.1.1"),
					.u.node_prefix =
						prefix_key("198.51.100.0/24"),
				},
			.ls_sequence = 42,
		},
		{
			.key =
				{
					.type = MIDR_NLRI_TYPE_GROUP_PREFIX,
					.originator_node_id =
						router_id("3.3.3.3"),
					.u.group_prefix =
						{
							.group_id = 200,
							.prefix =
								prefix_key(
									"2001:db8::/32"),
						},
				},
			.ls_sequence = 43,
			.policy_tags = 7,
		},
	};
	struct midr_ls_attributes attributes;
	struct midr_ls_object decoded;
	struct stream *stream;
	size_t length;
	size_t index;

	for (index = 0; index < array_size(objects); index++) {
		stream = stream_new(256);
		assert(midr_ls_attribute_encode(stream, &objects[index]) == MIDR_CODEC_OK);
		length = stream_get_endp(stream);
		assert(midr_ls_attribute_decode(stream, length, &attributes) == MIDR_CODEC_OK);
		assert(midr_ls_object_from_wire(&objects[index].key, &attributes, &decoded) ==
		       MIDR_CODEC_OK);
		assert(midr_ls_object_same(&objects[index], &decoded));
		stream_free(stream);
	}
}

static void test_attribute_errors(void)
{
	static const uint8_t duplicate_sequence[] = {
		0x00, 0x01, 0x00, 0x08, 0, 0, 0, 0, 0, 0, 0, 1,
		0x00, 0x01, 0x00, 0x08, 0, 0, 0, 0, 0, 0, 0, 2,
	};
	static const uint8_t unknown_tlv[] = {
		0x03,
		0xe7,
		0x00,
		0x00,
	};
	static const uint8_t truncated_tlv[] = {
		0x00, 0x01, 0x00, 0x08, 0, 0, 0,
	};
	static const uint8_t out_of_order[] = {
		0x00, 0x03, 0x00, 0x08, 0, 0, 0, 0, 0, 0, 0, 1,
		0x00, 0x01, 0x00, 0x08, 0, 0, 0, 0, 0, 0, 0, 2,
	};
	struct midr_ls_object object = membership_object(false, 0);
	struct midr_ls_attributes attributes = {
		.present = UINT32_MAX,
	};
	struct stream *stream;

	stream = stream_from_bytes(duplicate_sequence, sizeof(duplicate_sequence));
	assert(midr_ls_attribute_decode(stream, sizeof(duplicate_sequence), &attributes) ==
	       MIDR_CODEC_MALFORMED_ATTRIBUTE);
	assert(attributes.present == 0);
	assert(stream_get_getp(stream) == 0);
	stream_free(stream);

	stream = stream_from_bytes(unknown_tlv, sizeof(unknown_tlv));
	assert(midr_ls_attribute_decode(stream, sizeof(unknown_tlv), &attributes) ==
	       MIDR_CODEC_UNKNOWN_TLV);
	assert(stream_get_getp(stream) == 0);
	stream_free(stream);

	stream = stream_from_bytes(truncated_tlv, sizeof(truncated_tlv));
	assert(midr_ls_attribute_decode(stream, sizeof(truncated_tlv), &attributes) ==
	       MIDR_CODEC_MALFORMED_ATTRIBUTE);
	assert(stream_get_getp(stream) == 0);
	stream_free(stream);

	stream = stream_from_bytes(out_of_order, sizeof(out_of_order));
	assert(midr_ls_attribute_decode(stream, sizeof(out_of_order), &attributes) ==
	       MIDR_CODEC_MALFORMED_ATTRIBUTE);
	assert(attributes.present == 0);
	assert(stream_get_getp(stream) == 0);
	stream_free(stream);

	memset(&attributes, 0, sizeof(attributes));
	attributes.present = MIDR_LS_ATTR_HAS_GROUP_ID | MIDR_LS_ATTR_HAS_CAP_FLAGS;
	attributes.group_id = 100;
	assert(midr_ls_object_from_wire(&object.key, &attributes, &object) ==
	       MIDR_CODEC_MALFORMED_ATTRIBUTE);

	stream = stream_new(31);
	object = membership_object(false, 0);
	assert(midr_ls_attribute_encode(stream, &object) == MIDR_CODEC_NO_SPACE);
	assert(stream_get_endp(stream) == 0);
	stream_free(stream);
}

static void test_attribute_type_and_metric_validation(void)
{
	struct midr_ls_object membership = membership_object(false, 0);
	struct midr_ls_object link = link_object();
	struct midr_ls_attributes attributes;
	struct midr_ls_object decoded;
	struct stream *stream = stream_new(256);
	size_t length;

	assert(midr_ls_attribute_encode(stream, &membership) == MIDR_CODEC_OK);
	length = stream_get_endp(stream);
	assert(midr_ls_attribute_decode(stream, length, &attributes) == MIDR_CODEC_OK);
	assert(midr_ls_object_from_wire(&link.key, &attributes, &decoded) ==
	       MIDR_CODEC_MALFORMED_ATTRIBUTE);
	stream_free(stream);

	stream = stream_new(256);
	assert(midr_ls_attribute_encode(stream, &link) == MIDR_CODEC_OK);
	length = stream_get_endp(stream);
	assert(midr_ls_attribute_decode(stream, length, &attributes) == MIDR_CODEC_OK);
	attributes.link_metrics.present_flags |= 0x8;
	assert(midr_ls_object_from_wire(&link.key, &attributes, &decoded) ==
	       MIDR_CODEC_MALFORMED_ATTRIBUTE);
	stream_free(stream);
}

static void test_attribute_defensive_errors(void)
{
	static const uint8_t bad_address_afi[] = {
		0x00, 0x65, 0x00, 0x06, 0xff, 0xff, 0, 0, 0, 0,
	};
	static const uint8_t short_tlv_header[] = { 0x00, 0x01, 0x00 };
	struct midr_ls_object object = membership_object(false, 0);
	struct midr_ls_object_key prefix = {
		.type = MIDR_NLRI_TYPE_NODE_PREFIX,
		.originator_node_id = router_id("1.1.1.1"),
		.u.node_prefix = prefix_key("192.0.2.0/24"),
	};
	struct midr_ls_attributes attributes = {};
	struct stream *stream;

	assert(midr_ls_attribute_encode(NULL, NULL) == MIDR_CODEC_MALFORMED_ATTRIBUTE);
	assert(midr_ls_attribute_encode(NULL, &object) == MIDR_CODEC_NO_SPACE);
	assert(midr_ls_attribute_decode(NULL, 0, &attributes) == MIDR_CODEC_MALFORMED_ATTRIBUTE);

	stream = stream_from_bytes(short_tlv_header, sizeof(short_tlv_header));
	assert(midr_ls_attribute_decode(stream, sizeof(short_tlv_header), &attributes) ==
	       MIDR_CODEC_MALFORMED_ATTRIBUTE);
	assert(midr_ls_attribute_decode(stream, sizeof(short_tlv_header) + 1, &attributes) ==
	       MIDR_CODEC_MALFORMED_ATTRIBUTE);
	stream_free(stream);

	stream = stream_from_bytes(bad_address_afi, sizeof(bad_address_afi));
	assert(midr_ls_attribute_decode(stream, sizeof(bad_address_afi), &attributes) ==
	       MIDR_CODEC_MALFORMED_ATTRIBUTE);
	stream_free(stream);

	stream = stream_new(1);
	assert(midr_ls_attribute_decode(stream, 0, NULL) == MIDR_CODEC_MALFORMED_ATTRIBUTE);
	stream_free(stream);

	assert(midr_ls_object_from_wire(NULL, &attributes, &object) ==
	       MIDR_CODEC_MALFORMED_ATTRIBUTE);
	assert(midr_ls_object_from_wire(&prefix, NULL, &object) == MIDR_CODEC_MALFORMED_ATTRIBUTE);
	assert(midr_ls_object_from_wire(&prefix, &attributes, NULL) ==
	       MIDR_CODEC_MALFORMED_ATTRIBUTE);
	attributes.present = MIDR_LS_ATTR_HAS_SEQUENCE | MIDR_LS_ATTR_HAS_GROUP_ID;
	assert(midr_ls_object_from_wire(&prefix, &attributes, &object) ==
	       MIDR_CODEC_MALFORMED_ATTRIBUTE);
}

static void test_propagation_path(void)
{
	static const uint8_t golden[] = {
		0x00, 0x02, 0x01, 0x01, 0x01, 0x01, 0x02, 0x02, 0x02, 0x02,
	};
	struct midr_propagation_path path = {};
	struct midr_propagation_path decoded = {};
	struct stream *stream = stream_new(sizeof(golden));
	uint32_t originator = router_id("1.1.1.1");
	uint32_t second = router_id("2.2.2.2");
	uint32_t local = router_id("3.3.3.3");

	assert(midr_propagation_path_init(&path, originator) == 0);
	assert(midr_propagation_path_init(&path, originator) == -EINVAL);
	assert(path.node_count == 1);
	assert(path.nodes[0] == originator);
	assert(midr_propagation_path_validate(&path, originator, originator, second) == 0);
	assert(midr_propagation_path_append(&path, second) == 0);
	assert(midr_propagation_path_contains(&path, second));
	assert(midr_propagation_path_append(&path, second) == -ELOOP);
	assert(midr_propagation_path_validate(&path, originator, second, local) == 0);
	assert(midr_propagation_path_validate(&path, originator, local, second) == -ELOOP);

	assert(midr_propagation_path_encode(stream, &path) == MIDR_CODEC_OK);
	assert_stream_bytes(stream, golden, sizeof(golden));
	assert(midr_propagation_path_decode(stream, sizeof(golden), &decoded) == MIDR_CODEC_OK);
	assert(decoded.node_count == 2);
	assert(decoded.nodes[0] == originator);
	assert(decoded.nodes[1] == second);

	midr_propagation_path_fini(&decoded);
	midr_propagation_path_fini(&path);
	stream_free(stream);
}

static void test_propagation_path_errors_and_limit(void)
{
	static const uint8_t zero_count[] = { 0x00, 0x00 };
	static const uint8_t excessive_count[] = { 0x02, 0x01 };
	static const uint8_t duplicate[] = {
		0x00, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	};
	struct midr_propagation_path path = {};
	struct midr_propagation_path occupied;
	struct stream *stream;
	uint32_t node;
	unsigned int index;

	assert(midr_propagation_path_init(NULL, htonl(1)) == -EINVAL);
	assert(midr_propagation_path_init(&path, 0) == -EINVAL);
	assert(midr_propagation_path_append(NULL, htonl(1)) == -EINVAL);
	assert(!midr_propagation_path_contains(NULL, htonl(1)));
	assert(midr_propagation_path_validate(NULL, htonl(1), htonl(1), htonl(2)) == -EINVAL);
	assert(midr_propagation_path_encode(NULL, NULL) == MIDR_CODEC_MALFORMED_ATTRIBUTE);

	stream = stream_from_bytes(zero_count, sizeof(zero_count));
	assert(midr_propagation_path_decode(stream, sizeof(zero_count), &path) ==
	       MIDR_CODEC_MALFORMED_ATTRIBUTE);
	assert(stream_get_getp(stream) == 0);
	stream_free(stream);

	stream = stream_from_bytes(duplicate, sizeof(duplicate));
	assert(midr_propagation_path_decode(stream, sizeof(duplicate), &path) ==
	       MIDR_CODEC_MALFORMED_ATTRIBUTE);
	assert(path.nodes == NULL);
	stream_free(stream);

	stream = stream_from_bytes(excessive_count, sizeof(excessive_count));
	assert(midr_propagation_path_decode(stream, sizeof(excessive_count), &path) ==
	       MIDR_CODEC_MALFORMED_ATTRIBUTE);
	stream_free(stream);

	node = htonl(1);
	occupied = (struct midr_propagation_path){
		.nodes = &node,
		.node_count = 1,
		.capacity = 1,
	};
	stream = stream_from_bytes(zero_count, sizeof(zero_count));
	assert(midr_propagation_path_decode(stream, sizeof(zero_count), &occupied) ==
	       MIDR_CODEC_MALFORMED_ATTRIBUTE);
	stream_free(stream);

	assert(midr_propagation_path_init(&path, htonl(1)) == 0);
	assert(midr_propagation_path_append(&path, 0) == -EINVAL);
	assert(midr_propagation_path_validate(&path, htonl(2), htonl(1), htonl(3)) == -ELOOP);
	assert(midr_propagation_path_validate(&path, htonl(1), htonl(2), htonl(3)) == -ELOOP);
	assert(midr_propagation_path_validate(&path, htonl(1), htonl(1), htonl(1)) == -ELOOP);
	for (index = 2; index <= MIDR_PROPAGATION_PATH_MAX_NODES; index++) {
		node = htonl(index);
		assert(midr_propagation_path_append(&path, node) == 0);
	}
	assert(path.node_count == MIDR_PROPAGATION_PATH_MAX_NODES);
	assert(midr_propagation_path_append(&path, htonl(513)) == -E2BIG);

	stream = stream_new(2 + path.node_count * 4 - 1);
	assert(midr_propagation_path_encode(stream, &path) == MIDR_CODEC_NO_SPACE);
	assert(stream_get_endp(stream) == 0);
	stream_free(stream);
	midr_propagation_path_fini(&path);
}

int main(void)
{
	test_membership_nlri_golden();
	test_other_nlri_round_trip();
	test_nlri_errors_and_atomicity();
	test_nlri_defensive_errors();
	test_membership_attribute_golden();
	test_attribute_round_trip();
	test_attribute_errors();
	test_attribute_type_and_metric_validation();
	test_attribute_defensive_errors();
	test_propagation_path();
	test_propagation_path_errors_and_limit();
	printf("MIDR codec tests passed\n");
	return 0;
}
