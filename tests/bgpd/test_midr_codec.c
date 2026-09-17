// SPDX-License-Identifier: GPL-2.0-or-later
/* MIDR single-instance NLRI and attribute codec tests. */

#include <zebra.h>

#include <assert.h>
#include <errno.h>
#include <string.h>

#include "privs.h"

#include "bgpd/bgp_midr_codec.h"

struct zebra_privs_t bgpd_privs = {};
struct event_loop *master;

static uint32_t router_id(const char *text)
{
	struct in_addr address;

	assert(inet_pton(AF_INET, text, &address) == 1);
	return address.s_addr;
}

static struct midr_ls_prefix_key prefix_key(const char *text)
{
	struct midr_ls_prefix_key key = {.safi = SAFI_UNICAST};

	assert(str2prefix(text, &key.prefix) > 0);
	key.afi = key.prefix.family == AF_INET ? AFI_IP : AFI_IP6;
	assert(midr_ls_prefix_key_normalize(&key, &key) == 0);
	return key;
}

static struct midr_instance instance(enum midr_nlri_type type, bool ipv6)
{
	struct midr_instance value = {
		.state = MIDR_INSTANCE_ACTIVE,
		.object = {
			.key = {
				.type = type,
				.originator_node_id = router_id("1.1.1.1"),
			},
			.ls_sequence = 100,
		},
	};

	switch (type) {
	case MIDR_NLRI_TYPE_MEMBERSHIP:
		value.object.payload.membership.group_id = 7;
		value.object.payload.membership.cap_flags = 3;
		break;
	case MIDR_NLRI_TYPE_LINK:
		value.object.key.u.link.remote_node_id = router_id("2.2.2.2");
		value.object.key.u.link.link_id = 9;
		value.object.payload.link.link_local_address.ipa_type =
			ipv6 ? IPADDR_V6 : IPADDR_V4;
		value.object.payload.link.link_remote_address.ipa_type =
			ipv6 ? IPADDR_V6 : IPADDR_V4;
		assert(inet_pton(ipv6 ? AF_INET6 : AF_INET,
				 ipv6 ? "2001:db8::1" : "192.0.2.1",
				 value.object.payload.link.link_local_address.ip.addrbytes) == 1);
		assert(inet_pton(ipv6 ? AF_INET6 : AF_INET,
				 ipv6 ? "2001:db8::2" : "192.0.2.2",
				 value.object.payload.link.link_remote_address.ip.addrbytes) == 1);
		value.object.payload.link.canonical_cost = 250;
		break;
	case MIDR_NLRI_TYPE_NODE_PREFIX:
		value.object.key.u.node_prefix =
			prefix_key(ipv6 ? "2001:db8:1::/65" : "192.0.2.0/24");
		break;
	case MIDR_NLRI_TYPE_GROUP_PREFIX:
		value.object.key.u.group_prefix.group_id = 7;
		value.object.key.u.group_prefix.prefix =
			prefix_key(ipv6 ? "2001:db8:2::/73" : "198.51.100.0/24");
		break;
	case MIDR_NLRI_TYPE_RESERVED:
	default:
		assert(0);
	}
	assert(midr_instance_validate(&value) == 0);
	return value;
}

static struct midr_instance withdrawn(struct midr_instance value)
{
	value.state = MIDR_INSTANCE_WITHDRAWN;
	value.object.policy_tags = 0;
	memset(&value.object.payload, 0, sizeof(value.object.payload));
	return value;
}

static struct stream *bytes(const uint8_t *data, size_t length)
{
	struct stream *stream = stream_new(length ? length : 1);

	stream_put(stream, data, length);
	return stream;
}

static void roundtrip(struct midr_instance *value)
{
	struct stream *nlri = stream_new(256);
	struct stream *attributes = stream_new(512);
	struct midr_ls_object_key key;
	struct midr_instance_attributes decoded_attributes;
	struct midr_instance decoded;

	assert(midr_nlri_encode(nlri, &value->object.key) == MIDR_CODEC_OK);
	assert(midr_nlri_decode(nlri, &key) == MIDR_CODEC_OK);
	assert(!STREAM_READABLE(nlri));
	assert(midr_ls_object_key_same(&key, &value->object.key));
	assert(midr_instance_attribute_encode(attributes, value, 123) == MIDR_CODEC_OK);
	assert(midr_instance_attribute_decode(attributes, stream_get_endp(attributes),
						 &decoded_attributes) == MIDR_CODEC_OK);
	assert(decoded_attributes.age_ms == 123);
	assert(midr_instance_from_wire(&key, &decoded_attributes, &decoded) ==
	       MIDR_CODEC_OK);
	assert(midr_instance_same(value, &decoded));
	stream_free(attributes);
	stream_free(nlri);
}

static void test_nlri_and_families(void)
{
	static const uint8_t membership_golden[] = {
		0x00, 0x01, 0x00, 0x04, 0x01, 0x01, 0x01, 0x01,
	};
	struct midr_instance value = instance(MIDR_NLRI_TYPE_MEMBERSHIP, false);
	struct stream *stream = stream_new(sizeof(membership_golden));
	struct midr_ls_object_key decoded;

	assert(midr_nlri_encode(stream, &value.object.key) == MIDR_CODEC_OK);
	assert(stream_get_endp(stream) == sizeof(membership_golden));
	assert(!memcmp(STREAM_DATA(stream), membership_golden,
			       sizeof(membership_golden)));
	assert(midr_nlri_decode(stream, &decoded) == MIDR_CODEC_OK);
	assert(midr_ls_object_key_same(&decoded, &value.object.key));
	stream_free(stream);

	for (int type = MIDR_NLRI_TYPE_MEMBERSHIP;
	     type <= MIDR_NLRI_TYPE_GROUP_PREFIX; type++)
		for (int family = 0; family < 2; family++) {
			struct midr_instance current = instance(type, family);
			struct midr_instance old = withdrawn(current);

			current.object.policy_tags = 0x1020304050607080ULL;
			roundtrip(&current);
			roundtrip(&old);
		}
}

static void test_malformed_nlri(void)
{
	static const uint8_t unknown[] = {0, 99, 0, 1, 0xaa};
	static const uint8_t truncated[] = {0, 1, 0, 4, 1};
	static const uint8_t host_bits[] = {
		0, 3, 0, 0x0c, 1, 1, 1, 1, 0, 1, 1, 0x19, 0xc0, 0, 2, 0x81,
	};
	struct midr_ls_object_key key;
	struct stream *stream;

	stream = bytes(unknown, sizeof(unknown));
	assert(midr_nlri_decode(stream, &key) == MIDR_CODEC_UNKNOWN_NLRI_TYPE);
	assert(!STREAM_READABLE(stream));
	stream_free(stream);
	stream = bytes(truncated, sizeof(truncated));
	assert(midr_nlri_decode(stream, &key) == MIDR_CODEC_MALFORMED_NLRI);
	assert(stream_get_getp(stream) == 0);
	stream_free(stream);
	stream = bytes(host_bits, sizeof(host_bits));
	assert(midr_nlri_decode(stream, &key) == MIDR_CODEC_MALFORMED_NLRI);
	assert(stream_get_getp(stream) == 0);
	stream_free(stream);
}

static void test_attribute_validation(void)
{
	static const uint8_t duplicate_sequence[] = {
		0, 1, 0, 8, 0, 0, 0, 0, 0, 0, 0, 1,
		0, 1, 0, 8, 0, 0, 0, 0, 0, 0, 0, 2,
	};
	static const uint8_t duplicate_state[] = {
		0, 1, 0, 8, 0, 0, 0, 0, 0, 0, 0, 1,
		0, 4, 0, 1, 1, 0, 4, 0, 1, 1,
		0, 5, 0, 4, 0, 0, 0, 0,
	};
	static const uint8_t duplicate_age[] = {
		0, 1, 0, 8, 0, 0, 0, 0, 0, 0, 0, 1,
		0, 4, 0, 1, 1, 0, 5, 0, 4, 0, 0, 0, 0,
		0, 5, 0, 4, 0, 0, 0, 0,
	};
	static const uint8_t unknown[] = {0x7f, 0xff, 0, 0};
	static const uint8_t truncated[] = {0, 1, 0, 8, 0, 0, 0};
	struct midr_instance value = instance(MIDR_NLRI_TYPE_NODE_PREFIX, false);
	struct midr_instance_attributes attributes;
	struct stream *stream;

	stream = bytes(duplicate_sequence, sizeof(duplicate_sequence));
	assert(midr_instance_attribute_decode(stream, sizeof(duplicate_sequence),
						     &attributes) == MIDR_CODEC_MALFORMED_ATTRIBUTE);
	assert(stream_get_getp(stream) == 0);
	stream_free(stream);
	stream = bytes(duplicate_state, sizeof(duplicate_state));
	assert(midr_instance_attribute_decode(stream, sizeof(duplicate_state),
						     &attributes) == MIDR_CODEC_MALFORMED_ATTRIBUTE);
	stream_free(stream);
	stream = bytes(duplicate_age, sizeof(duplicate_age));
	assert(midr_instance_attribute_decode(stream, sizeof(duplicate_age),
						     &attributes) == MIDR_CODEC_MALFORMED_ATTRIBUTE);
	stream_free(stream);
	stream = bytes(unknown, sizeof(unknown));
	assert(midr_instance_attribute_decode(stream, sizeof(unknown), &attributes) ==
	       MIDR_CODEC_UNKNOWN_TLV);
	stream_free(stream);
	stream = bytes(truncated, sizeof(truncated));
	assert(midr_instance_attribute_decode(stream, sizeof(truncated), &attributes) ==
	       MIDR_CODEC_MALFORMED_ATTRIBUTE);
	stream_free(stream);

	stream = stream_new(128);
	assert(midr_instance_attribute_encode(stream, &value, 0) == MIDR_CODEC_OK);
	assert(midr_instance_attribute_decode(stream, stream_get_endp(stream), &attributes) ==
	       MIDR_CODEC_OK);
	assert(midr_instance_from_wire(&value.object.key, &attributes, &value) ==
	       MIDR_CODEC_OK);
	stream_free(stream);
}

static void test_defensive_guards(void)
{
	struct midr_instance value = instance(MIDR_NLRI_TYPE_LINK, true);
	struct midr_instance_attributes attributes;
	struct stream *stream = stream_new(1);

	assert(midr_instance_attribute_encode(NULL, NULL, 0) ==
	       MIDR_CODEC_MALFORMED_ATTRIBUTE);
	assert(midr_instance_attribute_decode(NULL, 0, &attributes) ==
	       MIDR_CODEC_MALFORMED_ATTRIBUTE);
	assert(midr_instance_attribute_decode(stream, 0, NULL) ==
	       MIDR_CODEC_MALFORMED_ATTRIBUTE);
	stream_free(stream);

	value.object.ls_sequence = 0;
	stream = stream_new(64);
	assert(midr_instance_attribute_encode(stream, &value, 0) ==
	       MIDR_CODEC_MALFORMED_ATTRIBUTE);
	stream_free(stream);
}

int main(void)
{
	test_nlri_and_families();
	test_malformed_nlri();
	test_attribute_validation();
	test_defensive_guards();
	puts("MIDR codec tests passed");
	return 0;
}
