// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR path-attribute integration tests.
 */

#include <zebra.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "privs.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_attr.h"
#include "bgpd/bgp_midr_attr.h"
#include "bgpd/bgp_network.h"
#include "bgpd/bgp_route.h"

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

static struct midr_ls_object link_object(void)
{
	return (struct midr_ls_object){
		.key =
			{
				.type = MIDR_NLRI_TYPE_LINK,
				.originator_node_id =
					router_id("1.1.1.1"),
				.u.link =
					{
						.remote_node_id =
							router_id(
								"2.2.2.2"),
						.link_id = 7,
					},
			},
		.ls_sequence = 42,
		.policy_tags = 9,
		.payload.link =
			{
				.link_local_address =
					ip_address("192.0.2.1"),
				.link_remote_address =
					ip_address("192.0.2.2"),
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

static void test_ls_attr_decode_and_intern(void)
{
	const uint8_t flags =
		BGP_ATTR_FLAG_OPTIONAL | BGP_ATTR_FLAG_EXTLEN;
	struct midr_ls_object object = link_object();
	const struct midr_ls_attributes *value;
	struct bgp_midr_ls_attr *first;
	struct bgp_midr_ls_attr *second;
	struct stream *stream = stream_new(256);
	struct attr attr = {};

	assert(midr_ls_attribute_encode(stream, &object) == MIDR_CODEC_OK);
	assert(bgp_midr_attr_decode(&attr, BGP_ATTR_MIDR_LS, flags,
				    STREAM_DATA(stream),
				    stream_get_endp(stream))
	       == BGP_ATTR_PARSE_PROCEED);
	assert(attr.midr_ls);
	value = bgp_midr_ls_attr_value(attr.midr_ls);
	assert(value->ls_sequence == object.ls_sequence);
	assert(value->policy_tags == object.policy_tags);
	assert(value->link_metrics.rtt_us ==
	       object.payload.link.metrics.rtt_us);

	first = bgp_midr_ls_attr_intern(value);
	second = bgp_midr_ls_attr_intern(value);
	assert(first == second);
	assert(first == attr.midr_ls);
	bgp_midr_ls_attr_unintern(&first);
	bgp_midr_ls_attr_unintern(&second);
	bgp_attr_flush(&attr);
	assert(!attr.midr_ls);
	stream_free(stream);
}

static void test_propagation_path_decode_and_intern(void)
{
	const uint8_t flags =
		BGP_ATTR_FLAG_OPTIONAL | BGP_ATTR_FLAG_EXTLEN;
	const struct midr_propagation_path *value;
	struct bgp_midr_propagation_path_attr *second;
	struct midr_propagation_path path = {};
	struct stream *stream = stream_new(64);
	struct attr attr = {};

	assert(midr_propagation_path_init(&path,
					  router_id("1.1.1.1"))
	       == 0);
	assert(midr_propagation_path_append(&path,
					    router_id("2.2.2.2"))
	       == 0);
	assert(midr_propagation_path_encode(stream, &path)
	       == MIDR_CODEC_OK);
	assert(bgp_midr_attr_decode(
		       &attr, BGP_ATTR_MIDR_PROPAGATION_PATH, flags,
		       STREAM_DATA(stream), stream_get_endp(stream))
	       == BGP_ATTR_PARSE_PROCEED);
	value = bgp_midr_propagation_path_attr_value(
		attr.midr_propagation_path);
	assert(value && value->node_count == 2);
	assert(value->nodes[0] == router_id("1.1.1.1"));
	assert(value->nodes[1] == router_id("2.2.2.2"));

	second = bgp_midr_propagation_path_attr_intern(value);
	assert(second == attr.midr_propagation_path);
	bgp_midr_propagation_path_attr_unintern(&second);
	bgp_attr_flush(&attr);
	midr_propagation_path_fini(&path);
	stream_free(stream);
}

static void test_outer_attr_lifecycle(void)
{
	struct midr_ls_attributes attributes = {
		.present = MIDR_LS_ATTR_HAS_SEQUENCE,
		.ls_sequence = 5,
	};
	struct midr_propagation_path path = {};
	struct attr parsed = {};
	struct attr *interned;

	assert(midr_propagation_path_init(&path,
					  router_id("1.1.1.1"))
	       == 0);
	parsed.midr_ls = bgp_midr_ls_attr_intern(&attributes);
	parsed.midr_propagation_path =
		bgp_midr_propagation_path_attr_intern(&path);
	assert(parsed.midr_ls && parsed.midr_propagation_path);

	interned = bgp_attr_intern(&parsed);
	assert(interned);
	assert(interned->midr_ls == parsed.midr_ls);
	assert(interned->midr_propagation_path ==
	       parsed.midr_propagation_path);
	bgp_attr_unintern_sub(&parsed);
	assert(!parsed.midr_ls && !parsed.midr_propagation_path);
	bgp_attr_unintern(&interned);
	assert(!interned);
	midr_propagation_path_fini(&path);
}

static void test_error_mapping_and_isolation(void)
{
	const uint8_t flags =
		BGP_ATTR_FLAG_OPTIONAL | BGP_ATTR_FLAG_EXTLEN;
	const uint8_t unknown_tlv[] = {0x7f, 0xff, 0x00, 0x00};
	const uint8_t bad_path[] = {0x00, 0x00};
	struct attr attr = {};

	assert(bgp_midr_attr_decode(&attr, BGP_ATTR_MIDR_LS,
				    BGP_ATTR_FLAG_OPTIONAL,
				    unknown_tlv, sizeof(unknown_tlv))
	       == BGP_ATTR_PARSE_WITHDRAW);
	assert(bgp_midr_attr_decode(&attr, BGP_ATTR_MIDR_LS, flags,
				    unknown_tlv, sizeof(unknown_tlv))
	       == BGP_ATTR_PARSE_WITHDRAW);
	assert(bgp_midr_attr_decode(
		       &attr, BGP_ATTR_MIDR_PROPAGATION_PATH, flags,
		       bad_path, sizeof(bad_path))
	       == BGP_ATTR_PARSE_WITHDRAW);
	assert(!attr.midr_ls && !attr.midr_propagation_path);

	assert(bgp_midr_attr_codec_result(MIDR_CODEC_UNKNOWN_TLV)
	       == BGP_ATTR_PARSE_WITHDRAW);
	assert(bgp_midr_nlri_codec_result(
		       MIDR_CODEC_UNKNOWN_NLRI_TYPE)
	       == BGP_NLRI_PARSE_OK);
	assert(bgp_midr_nlri_codec_result(
		       MIDR_CODEC_MALFORMED_NLRI)
	       == BGP_NLRI_PARSE_ERROR);

	assert(bgp_midr_attr_family_is_midr(
		true, AFI_BGP_LS, SAFI_MIDR_LS));
	assert(!bgp_midr_attr_family_is_midr(
		true, AFI_BGP_LS, SAFI_BGP_LS));
	assert(!bgp_midr_attr_family_is_midr(
		true, AFI_IP, SAFI_UNICAST));
	assert(!bgp_midr_attr_family_is_midr(
		false, AFI_BGP_LS, SAFI_MIDR_LS));
}

int main(void)
{
	master = event_master_create("test MIDR attributes");
	bgp_master_init(master, BGP_SOCKET_SNDBUF_SIZE, list_new());
	bgp_attr_init();
	test_ls_attr_decode_and_intern();
	test_propagation_path_decode_and_intern();
	test_outer_attr_lifecycle();
	test_error_mapping_and_isolation();
	bgp_attr_finish();
	puts("MIDR attribute tests passed");
	return 0;
}
