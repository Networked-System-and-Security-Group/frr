// SPDX-License-Identifier: GPL-2.0-or-later
/* MIDR single-instance attribute container and parser tests. */

#include <zebra.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "privs.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_attr.h"
#include "bgpd/bgp_midr_attr.h"
#include "bgpd/bgp_midr_codec.h"
#include "bgpd/bgp_network.h"
#include "bgpd/bgp_route.h"

struct zebra_privs_t bgpd_privs = {};
struct event_loop *master;

static struct ipaddr address(const char *text)
{
	struct ipaddr value;

	assert(str2ipaddr(text, &value) == 0);
	return value;
}

static struct midr_instance link_instance(void)
{
	return (struct midr_instance){
		.state = MIDR_INSTANCE_ACTIVE,
		.object = {
			.key = {
				.type = MIDR_NLRI_TYPE_LINK,
				.originator_node_id = htonl(0x01010101),
				.u.link = {
					.remote_node_id = htonl(0x02020202),
					.link_id = 7,
				},
			},
			.ls_sequence = 42,
			.policy_tags = 9,
			.payload.link = {
				.link_local_address = address("2001:db8::1"),
				.link_remote_address = address("2001:db8::2"),
				.canonical_cost = 250,
			},
		},
	};
}

static void test_decode_and_intern(void)
{
	const uint8_t flags = BGP_ATTR_FLAG_OPTIONAL | BGP_ATTR_FLAG_EXTLEN;
	struct midr_instance instance = link_instance();
	struct stream *stream = stream_new(256);
	struct attr parsed = {};
	struct bgp_midr_ls_attr *first;
	struct bgp_midr_ls_attr *second;
	const struct midr_ls_attributes *value;

	assert(midr_instance_attribute_encode(stream, &instance, 123) == MIDR_CODEC_OK);
	assert(bgp_midr_attr_decode(&parsed, BGP_ATTR_MIDR_LS, flags,
				    STREAM_DATA(stream), stream_get_endp(stream)) ==
	       BGP_ATTR_PARSE_PROCEED);
	assert(parsed.midr_ls);
	value = bgp_midr_ls_attr_value(parsed.midr_ls);
	assert(value && value->ls_sequence == instance.object.ls_sequence);
	assert(value->policy_tags == instance.object.policy_tags);
	assert(value->link_canonical_cost == instance.object.payload.link.canonical_cost);
	assert(bgp_midr_ls_attr_state(parsed.midr_ls) == MIDR_INSTANCE_ACTIVE);
	assert(bgp_midr_ls_attr_age(parsed.midr_ls) == 123);

	first = bgp_midr_instance_attr_intern(&instance, 123);
	second = bgp_midr_instance_attr_intern(&instance, 123);
	assert(first && first == second && first == parsed.midr_ls);
	bgp_midr_ls_attr_unintern(&first);
	bgp_midr_ls_attr_unintern(&second);
	bgp_attr_unintern_sub(&parsed);
	stream_free(stream);
}

static void test_withdrawn_container(void)
{
	const uint8_t flags = BGP_ATTR_FLAG_OPTIONAL | BGP_ATTR_FLAG_EXTLEN;
	struct midr_instance instance = link_instance();
	struct midr_instance_attributes decoded;
	struct stream *stream = stream_new(128);
	struct attr parsed = {};

	instance.state = MIDR_INSTANCE_WITHDRAWN;
	instance.object.policy_tags = 0;
	memset(&instance.object.payload, 0, sizeof(instance.object.payload));
	assert(midr_instance_attribute_encode(stream, &instance, UINT32_MAX) == MIDR_CODEC_OK);
	assert(bgp_midr_attr_decode(&parsed, BGP_ATTR_MIDR_LS, flags,
				    STREAM_DATA(stream), stream_get_endp(stream)) ==
	       BGP_ATTR_PARSE_PROCEED);
	assert(bgp_midr_ls_attr_state(parsed.midr_ls) == MIDR_INSTANCE_WITHDRAWN);
	assert(bgp_midr_ls_attr_age(parsed.midr_ls) == UINT32_MAX);
	assert(midr_instance_attribute_decode(stream, stream_get_endp(stream), &decoded) ==
	       MIDR_CODEC_OK);
	assert(decoded.ls.present == MIDR_LS_ATTR_HAS_SEQUENCE);
	bgp_attr_unintern_sub(&parsed);
	stream_free(stream);
}

static void test_parser_rejection_and_family(void)
{
	const uint8_t flags = BGP_ATTR_FLAG_OPTIONAL | BGP_ATTR_FLAG_EXTLEN;
	const uint8_t malformed[] = {0, 1, 0, 8, 0, 0, 0, 0, 0, 0, 0};
	struct attr attr = {};

	assert(bgp_midr_attr_decode(&attr, BGP_ATTR_MIDR_LS, flags, malformed,
				    sizeof(malformed)) == BGP_ATTR_PARSE_WITHDRAW);
	assert(!attr.midr_ls);
	assert(bgp_midr_attr_decode(&attr, 254, flags, malformed, sizeof(malformed)) ==
	       BGP_ATTR_PARSE_WITHDRAW);
	assert(bgp_midr_attr_decode(&attr, BGP_ATTR_MIDR_LS, BGP_ATTR_FLAG_OPTIONAL,
				    malformed, sizeof(malformed)) == BGP_ATTR_PARSE_WITHDRAW);
	assert(bgp_midr_attr_family_is_midr(true, AFI_BGP_LS, SAFI_MIDR_LS));
	assert(!bgp_midr_attr_family_is_midr(true, AFI_IP6, SAFI_UNICAST));
	assert(!bgp_midr_attr_family_is_midr(false, AFI_BGP_LS, SAFI_MIDR_LS));
	assert(bgp_midr_attr_codec_result(MIDR_CODEC_UNKNOWN_TLV) ==
	       BGP_ATTR_PARSE_WITHDRAW);
}

int main(void)
{
	master = event_master_create("test MIDR attributes");
	bgp_master_init(master, BGP_SOCKET_SNDBUF_SIZE, list_new());
	bgp_attr_init();
	test_decode_and_intern();
	test_withdrawn_container();
	test_parser_rejection_and_family();
	bgp_attr_finish();
	puts("MIDR attribute tests passed");
	return 0;
}
