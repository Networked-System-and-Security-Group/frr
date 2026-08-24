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
					.canonical_cost = 250,
			},
	};
}

static void test_ls_attr_decode_and_intern(void)
{
	const uint8_t flags = BGP_ATTR_FLAG_OPTIONAL | BGP_ATTR_FLAG_EXTLEN;
	struct midr_ls_object object = link_object();
	const struct midr_ls_attributes *value;
	struct bgp_midr_ls_attr *first;
	struct bgp_midr_ls_attr *second;
	struct stream *stream = stream_new(256);
	struct attr attr = {};

	assert(midr_ls_attribute_encode(stream, &object) == MIDR_CODEC_OK);
	assert(bgp_midr_attr_decode(&attr, BGP_ATTR_MIDR_LS, flags, STREAM_DATA(stream),
				    stream_get_endp(stream)) == BGP_ATTR_PARSE_PROCEED);
	assert(attr.midr_ls);
	value = bgp_midr_ls_attr_value(attr.midr_ls);
	assert(value->ls_sequence == object.ls_sequence);
	assert(value->policy_tags == object.policy_tags);
	assert(value->link_canonical_cost ==
	       object.payload.link.canonical_cost);

	first = bgp_midr_ls_attr_intern(value);
	second = bgp_midr_ls_attr_intern(value);
	assert(first == second);
	assert(first == attr.midr_ls);
	bgp_midr_ls_attr_unintern(&first);
	bgp_midr_ls_attr_unintern(&second);
	bgp_attr_unintern_sub(&attr);
	assert(!attr.midr_ls);
	stream_free(stream);
}

static void test_propagation_path_decode_and_intern(void)
{
	const uint8_t flags = BGP_ATTR_FLAG_OPTIONAL | BGP_ATTR_FLAG_EXTLEN;
	const struct midr_propagation_path *value;
	struct bgp_midr_propagation_path_attr *second;
	struct midr_propagation_path path = {};
	struct stream *stream = stream_new(64);
	struct attr attr = {};

	assert(midr_propagation_path_init(&path, router_id("1.1.1.1")) == 0);
	assert(midr_propagation_path_append(&path, router_id("2.2.2.2")) == 0);
	assert(midr_propagation_path_encode(stream, &path) == MIDR_CODEC_OK);
	assert(bgp_midr_attr_decode(&attr, BGP_ATTR_MIDR_PROPAGATION_PATH, flags,
				    STREAM_DATA(stream),
				    stream_get_endp(stream)) == BGP_ATTR_PARSE_PROCEED);
	value = bgp_midr_propagation_path_attr_value(attr.midr_propagation_path);
	assert(value && value->node_count == 2);
	assert(value->nodes[0] == router_id("1.1.1.1"));
	assert(value->nodes[1] == router_id("2.2.2.2"));

	second = bgp_midr_propagation_path_attr_intern(value);
	assert(second == attr.midr_propagation_path);
	bgp_midr_propagation_path_attr_unintern(&second);
	bgp_attr_unintern_sub(&attr);
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

	assert(midr_propagation_path_init(&path, router_id("1.1.1.1")) == 0);
	parsed.midr_ls = bgp_midr_ls_attr_new(&attributes);
	parsed.midr_propagation_path = bgp_midr_propagation_path_attr_new(&path);
	assert(parsed.midr_ls && parsed.midr_propagation_path);

	interned = bgp_attr_intern(&parsed);
	assert(interned);
	assert(interned->midr_ls == parsed.midr_ls);
	assert(interned->midr_propagation_path == parsed.midr_propagation_path);
	bgp_attr_flush(&parsed);
	assert(!parsed.midr_ls && !parsed.midr_propagation_path);
	assert(bgp_midr_ls_attr_value(interned->midr_ls)->ls_sequence == 5);
	assert(bgp_midr_propagation_path_attr_value(
		       interned->midr_propagation_path)
		       ->nodes[0] == router_id("1.1.1.1"));
	bgp_attr_unintern(&interned);
	assert(!interned);
	midr_propagation_path_fini(&path);
}

static void test_error_mapping_and_isolation(void)
{
	const uint8_t flags = BGP_ATTR_FLAG_OPTIONAL | BGP_ATTR_FLAG_EXTLEN;
	const uint8_t unknown_tlv[] = { 0x7f, 0xff, 0x00, 0x00 };
	const uint8_t bad_path[] = { 0x00, 0x00 };
	struct attr attr = {};

	assert(bgp_midr_attr_decode(&attr, BGP_ATTR_MIDR_LS, BGP_ATTR_FLAG_OPTIONAL, unknown_tlv,
				    sizeof(unknown_tlv)) == BGP_ATTR_PARSE_WITHDRAW);
	assert(bgp_midr_attr_decode(&attr, BGP_ATTR_MIDR_LS, flags, unknown_tlv,
				    sizeof(unknown_tlv)) == BGP_ATTR_PARSE_WITHDRAW);
	assert(bgp_midr_attr_decode(&attr, BGP_ATTR_MIDR_PROPAGATION_PATH, flags, bad_path,
				    sizeof(bad_path)) == BGP_ATTR_PARSE_WITHDRAW);
	assert(!attr.midr_ls && !attr.midr_propagation_path);

	assert(bgp_midr_attr_codec_result(MIDR_CODEC_UNKNOWN_TLV) == BGP_ATTR_PARSE_WITHDRAW);
	assert(bgp_midr_nlri_codec_result(MIDR_CODEC_UNKNOWN_NLRI_TYPE) == BGP_NLRI_PARSE_OK);
	assert(bgp_midr_nlri_codec_result(MIDR_CODEC_MALFORMED_NLRI) == BGP_NLRI_PARSE_ERROR);

	assert(bgp_midr_attr_family_is_midr(true, AFI_BGP_LS, SAFI_MIDR_LS));
	assert(!bgp_midr_attr_family_is_midr(true, AFI_BGP_LS, SAFI_BGP_LS));
	assert(!bgp_midr_attr_family_is_midr(true, AFI_IP, SAFI_UNICAST));
	assert(!bgp_midr_attr_family_is_midr(false, AFI_BGP_LS, SAFI_MIDR_LS));
}

static void test_container_guards_and_comparisons(void)
{
	struct midr_ls_attributes base = {
		.present = MIDR_LS_ATTR_HAS_SEQUENCE,
		.ls_sequence = 1,
		.policy_tags = 2,
		.group_id = 3,
		.transport_address = ip_address("192.0.2.1"),
		.cap_flags = 4,
		.link_local_address = ip_address("198.51.100.1"),
		.link_remote_address = ip_address("198.51.100.2"),
		.link_canonical_cost = 250,
	};
	struct midr_ls_attributes variants[9];
	struct bgp_midr_ls_attr *base_attr;
	struct midr_propagation_path first = {};
	struct midr_propagation_path second = {};
	struct bgp_midr_propagation_path_attr *first_attr;
	struct bgp_midr_propagation_path_attr *second_attr;
	size_t index;

	for (index = 0; index < array_size(variants); index++)
		variants[index] = base;
	variants[0].present++;
	variants[1].ls_sequence++;
	variants[2].policy_tags++;
	variants[3].group_id++;
	variants[4].transport_address = ip_address("192.0.2.9");
	variants[5].cap_flags++;
	variants[6].link_local_address = ip_address("198.51.100.9");
	variants[7].link_remote_address = ip_address("198.51.100.10");
	variants[8].link_canonical_cost++;

	assert(bgp_midr_ls_attr_hash_key(NULL) == 0);
	assert(bgp_midr_ls_attr_same(NULL, NULL));
	assert(!bgp_midr_ls_attr_same(NULL, (void *)1));
	assert(bgp_midr_ls_attr_intern(NULL) == NULL);
	assert(bgp_midr_ls_attr_value(NULL) == NULL);
	bgp_midr_ls_attr_unintern(NULL);

	base_attr = bgp_midr_ls_attr_intern(&base);
	assert(base_attr);
	assert(bgp_midr_ls_attr_same(base_attr, base_attr));
	bgp_midr_ls_attr_lock(base_attr);
	for (index = 0; index < array_size(variants); index++) {
		struct bgp_midr_ls_attr *variant = bgp_midr_ls_attr_intern(&variants[index]);

		assert(variant);
		assert(!bgp_midr_ls_attr_same(base_attr, variant));
		bgp_midr_ls_attr_unintern(&variant);
	}
	bgp_midr_ls_attr_unintern(&base_attr);
	bgp_midr_ls_attr_unintern(&base_attr);

	assert(bgp_midr_propagation_path_attr_hash_key(NULL) == 0);
	assert(bgp_midr_propagation_path_attr_same(NULL, NULL));
	assert(!bgp_midr_propagation_path_attr_same(NULL, (void *)1));
	assert(bgp_midr_propagation_path_attr_intern(NULL) == NULL);
	assert(bgp_midr_propagation_path_attr_intern(&first) == NULL);
	assert(bgp_midr_propagation_path_attr_value(NULL) == NULL);
	bgp_midr_propagation_path_attr_unintern(NULL);

	assert(midr_propagation_path_init(&first, router_id("1.1.1.1")) == 0);
	assert(midr_propagation_path_init(&second, router_id("1.1.1.1")) == 0);
	assert(midr_propagation_path_append(&second, router_id("2.2.2.2")) == 0);
	first_attr = bgp_midr_propagation_path_attr_intern(&first);
	second_attr = bgp_midr_propagation_path_attr_intern(&second);
	assert(first_attr && second_attr);
	assert(!bgp_midr_propagation_path_attr_same(first_attr, second_attr));
	bgp_midr_propagation_path_attr_lock(first_attr);
	bgp_midr_propagation_path_attr_unintern(&first_attr);
	bgp_midr_propagation_path_attr_unintern(&first_attr);
	bgp_midr_propagation_path_attr_unintern(&second_attr);
	midr_propagation_path_fini(&first);
	midr_propagation_path_fini(&second);
}

static void test_all_parser_result_paths(void)
{
	const uint8_t value[] = { 0 };
	struct attr attr = {};
	enum midr_codec_result result;

	for (result = MIDR_CODEC_OK; result <= MIDR_CODEC_OVERFLOW; result++) {
		enum bgp_attr_parse_ret attr_result = bgp_midr_attr_codec_result(result);
		int nlri_result = bgp_midr_nlri_codec_result(result);

		if (result == MIDR_CODEC_OK) {
			assert(attr_result == BGP_ATTR_PARSE_PROCEED);
			assert(nlri_result == BGP_NLRI_PARSE_OK);
		} else if (result == MIDR_CODEC_UNKNOWN_NLRI_TYPE) {
			assert(attr_result == BGP_ATTR_PARSE_WITHDRAW);
			assert(nlri_result == BGP_NLRI_PARSE_OK);
		} else {
			assert(attr_result == BGP_ATTR_PARSE_WITHDRAW);
			assert(nlri_result == BGP_NLRI_PARSE_ERROR);
		}
	}
	assert(bgp_midr_attr_codec_result((enum midr_codec_result)99) == BGP_ATTR_PARSE_WITHDRAW);
	assert(bgp_midr_nlri_codec_result((enum midr_codec_result)99) == BGP_NLRI_PARSE_ERROR);

	assert(bgp_midr_attr_decode(NULL, BGP_ATTR_MIDR_LS,
				    BGP_ATTR_FLAG_OPTIONAL | BGP_ATTR_FLAG_EXTLEN, value,
				    sizeof(value)) == BGP_ATTR_PARSE_WITHDRAW);
	assert(bgp_midr_attr_decode(&attr, BGP_ATTR_MIDR_LS,
				    BGP_ATTR_FLAG_OPTIONAL | BGP_ATTR_FLAG_EXTLEN, NULL,
				    sizeof(value)) == BGP_ATTR_PARSE_WITHDRAW);
	assert(bgp_midr_attr_decode(&attr, BGP_ATTR_MIDR_LS,
				    BGP_ATTR_FLAG_OPTIONAL | BGP_ATTR_FLAG_EXTLEN |
					    BGP_ATTR_FLAG_TRANS,
				    value, sizeof(value)) == BGP_ATTR_PARSE_WITHDRAW);
	assert(bgp_midr_attr_decode(&attr, 252, BGP_ATTR_FLAG_OPTIONAL | BGP_ATTR_FLAG_EXTLEN,
				    value, sizeof(value)) == BGP_ATTR_PARSE_WITHDRAW);
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
	test_container_guards_and_comparisons();
	test_all_parser_result_paths();
	bgp_attr_finish();
	puts("MIDR attribute tests passed");
	return 0;
}
