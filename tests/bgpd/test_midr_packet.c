// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR MP_REACH/MP_UNREACH adaptation tests.
 */

#include <zebra.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>

#include "command.h"
#include "privs.h"
#include "qobj.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_attr.h"
#include "bgpd/bgp_midr_attr.h"
#include "bgpd/bgp_midr_packet.h"
#include "bgpd/bgp_midr_private.h"
#include "bgpd/bgp_midr_rib.h"
#include "bgpd/bgp_network.h"
#include "bgpd/bgp_vty.h"

struct zebra_privs_t bgpd_privs = {};
struct event_loop *master;

static struct bgp *bgp;
static struct midr_context *ctx;
static struct peer *remote;

static uint32_t router_id(const char *text)
{
	struct in_addr address;

	assert(inet_pton(AF_INET, text, &address) == 1);
	return address.s_addr;
}

static struct midr_ls_prefix_key prefix_key(const char *text)
{
	struct midr_ls_prefix_key key = {
		.safi = SAFI_UNICAST,
	};

	assert(str2prefix(text, &key.prefix) > 0);
	key.afi = family2afi(key.prefix.family);
	assert(key.afi == AFI_IP || key.afi == AFI_IP6);
	apply_mask(&key.prefix);
	return key;
}

static struct midr_ls_object_key node_prefix_key(const char *text)
{
	return (struct midr_ls_object_key){
		.type = MIDR_NLRI_TYPE_NODE_PREFIX,
		.originator_node_id = remote->remote_id.s_addr,
		.u.node_prefix = prefix_key(text),
	};
}

static struct midr_ls_object_key group_prefix_key(const char *text, uint32_t group_id)
{
	return (struct midr_ls_object_key){
		.type = MIDR_NLRI_TYPE_GROUP_PREFIX,
		.originator_node_id = remote->remote_id.s_addr,
		.u.group_prefix = {
			.group_id = group_id,
			.prefix = prefix_key(text),
		},
	};
}

static struct midr_ls_attributes membership_attributes(uint64_t sequence, uint32_t group_id)
{
	return (struct midr_ls_attributes){
		.present = MIDR_LS_ATTR_HAS_SEQUENCE | MIDR_LS_ATTR_HAS_GROUP_ID |
			   MIDR_LS_ATTR_HAS_CAP_FLAGS,
		.ls_sequence = sequence,
		.group_id = group_id,
	};
}

static struct midr_ls_attributes prefix_attributes(uint64_t sequence)
{
	return (struct midr_ls_attributes){
		.present = MIDR_LS_ATTR_HAS_SEQUENCE,
		.ls_sequence = sequence,
	};
}

static struct attr wire_attr(const struct midr_ls_attributes *attributes,
			     const struct midr_propagation_path *path)
{
	struct attr attr = {};

	attr.midr_ls = bgp_midr_ls_attr_intern(attributes);
	attr.midr_propagation_path = bgp_midr_propagation_path_attr_intern(path);
	assert(attr.midr_ls && attr.midr_propagation_path);
	return attr;
}

static struct bgp_nlri packet_from_stream(struct stream *stream)
{
	return (struct bgp_nlri){
		.afi = AFI_BGP_LS,
		.safi = SAFI_MIDR_LS,
		.nlri = STREAM_DATA(stream),
		.length = stream_get_endp(stream),
	};
}

struct encode_state {
	struct stream *stream;
	const struct midr_ls_object_key *key;
	int result;
};

static int encode_selected(const struct midr_ls_object *object,
			   const struct midr_propagation_path *path, struct peer *peer,
			   struct bgp_dest *dest, struct bgp_path_info *selected, void *arg)
{
	struct encode_state *state = arg;

	(void)path;
	(void)peer;
	if (!midr_ls_object_key_same(&object->key, state->key))
		return 0;
	assert(selected->attr->aspath);
	assert(bgp_attr_exists(selected->attr, BGP_ATTR_ORIGIN));
	assert(bgp_attr_exists(selected->attr, BGP_ATTR_AS_PATH));
	state->result = bgp_midr_packet_attributes(state->stream, bgp, selected);
	assert(bgp_midr_packet_nlri_size(dest) > 0);
	return 0;
}

static void test_update_withdraw_and_outbound_path(void)
{
	uint32_t originator = remote->remote_id.s_addr;
	struct midr_ls_object_key key = {
		.type = MIDR_NLRI_TYPE_MEMBERSHIP,
		.originator_node_id = originator,
	};
	struct midr_ls_attributes attributes = membership_attributes(7, 20);
	struct midr_propagation_path path = {};
	const struct midr_propagation_path *selected_path;
	struct midr_ls_object selected;
	struct peer *selected_peer;
	struct stream *nlri = stream_new(128);
	struct stream *encoded = stream_new(512);
	struct bgp_nlri packet;
	struct attr attr;
	struct encode_state state = {
		.stream = encoded,
		.key = &key,
		.result = -1,
	};
	size_t offset;
	uint16_t length;
	struct midr_propagation_path propagated = {};

	assert(midr_propagation_path_init(&path, originator) == 0);
	attr = wire_attr(&attributes, &path);
	assert(midr_nlri_encode(nlri, &key) == MIDR_CODEC_OK);
	packet = packet_from_stream(nlri);
	assert(bgp_nlri_parse_midr(remote, &attr, &packet) == BGP_NLRI_PARSE_OK);
	assert(midr_rib_selected_get(ctx, &key, &selected, &selected_path, &selected_peer) == 0);
	assert(selected.ls_sequence == 7);
	assert(selected.payload.membership.group_id == 20);
	assert(selected_peer == remote);

	assert(midr_rib_selected_entry_foreach(ctx, encode_selected, &state) == 0);
	assert(state.result > 0);
	offset = 0;
	assert(stream_getc_from(encoded, offset + 1) == BGP_ATTR_MIDR_LS);
	length = stream_getw_from(encoded, offset + 2);
	offset += 4 + length;
	assert(stream_getc_from(encoded, offset + 1) == BGP_ATTR_MIDR_PROPAGATION_PATH);
	length = stream_getw_from(encoded, offset + 2);
	stream_set_getp(encoded, offset + 4);
	assert(midr_propagation_path_decode(encoded, length, &propagated) == MIDR_CODEC_OK);
	assert(propagated.node_count == 2);
	assert(propagated.nodes[0] == originator);
	assert(propagated.nodes[1] == bgp->router_id.s_addr);

	assert(bgp_nlri_parse_midr(remote, NULL, &packet) == BGP_NLRI_PARSE_OK);
	assert(midr_rib_selected_get(ctx, &key, &selected, &selected_path, &selected_peer) ==
	       -ENOENT);

	midr_propagation_path_fini(&propagated);
	bgp_attr_unintern_sub(&attr);
	midr_propagation_path_fini(&path);
	stream_free(encoded);
	stream_free(nlri);
}

static void test_unknown_and_malformed_nlri(void)
{
	struct midr_ls_object_key key = {
		.type = MIDR_NLRI_TYPE_MEMBERSHIP,
		.originator_node_id = remote->remote_id.s_addr,
	};
	struct midr_ls_attributes attributes = membership_attributes(8, 30);
	struct midr_propagation_path path = {};
	struct stream *stream = stream_new(128);
	struct bgp_nlri packet;
	struct attr attr;

	assert(midr_propagation_path_init(&path, remote->remote_id.s_addr) == 0);
	attr = wire_attr(&attributes, &path);
	stream_putw(stream, 999);
	stream_putw(stream, 1);
	stream_putc(stream, 0);
	assert(midr_nlri_encode(stream, &key) == MIDR_CODEC_OK);
	packet = packet_from_stream(stream);
	assert(bgp_nlri_parse_midr(remote, &attr, &packet) == BGP_NLRI_PARSE_OK);
	assert(bgp_nlri_parse_midr(remote, &(struct attr){}, &packet) == BGP_NLRI_PARSE_ERROR);

	stream_reset(stream);
	stream_putw(stream, MIDR_NLRI_TYPE_MEMBERSHIP);
	stream_putw(stream, 5);
	stream_putl(stream, ntohl(key.originator_node_id));
	stream_putc(stream, 0);
	packet = packet_from_stream(stream);
	assert(bgp_nlri_parse_midr(remote, &attr, &packet) == BGP_NLRI_PARSE_ERROR);

	bgp_attr_unintern_sub(&attr);
	midr_propagation_path_fini(&path);
	stream_free(stream);
}

static void assert_selected_sequence(const struct midr_ls_object_key *key, uint64_t sequence)
{
	const struct midr_propagation_path *selected_path;
	struct midr_ls_object selected;
	struct peer *selected_peer;

	assert(midr_rib_selected_get(ctx, key, &selected, &selected_path, &selected_peer) == 0);
	assert(midr_ls_object_key_same(&selected.key, key));
	assert(selected.ls_sequence == sequence);
	assert(selected_path->node_count == 1);
	assert(selected_path->nodes[0] == remote->remote_id.s_addr);
	assert(selected_peer == remote);
}

static void assert_selected_missing(const struct midr_ls_object_key *key)
{
	const struct midr_propagation_path *selected_path;
	struct midr_ls_object selected;
	struct peer *selected_peer;

	assert(midr_rib_selected_get(ctx, key, &selected, &selected_path, &selected_peer) ==
	       -ENOENT);
}

static void test_ipv6_prefix_update_and_withdraw(void)
{
	struct midr_ls_object_key keys[] = {
		node_prefix_key("::/0"),
		node_prefix_key("2001:db8:abcd:ef01:8000::/73"),
		group_prefix_key("2001:db8:ffff::1/128", 20),
	};
	struct midr_propagation_path path = {};
	struct stream *nlri = stream_new(512);
	struct bgp_nlri packet;
	size_t index;

	assert(midr_propagation_path_init(&path, remote->remote_id.s_addr) == 0);
	for (index = 0; index < array_size(keys); index++) {
		struct midr_ls_attributes attributes = prefix_attributes(100 + index);
		struct attr attr = wire_attr(&attributes, &path);

		stream_reset(nlri);
		assert(midr_nlri_encode(nlri, &keys[index]) == MIDR_CODEC_OK);
		packet = packet_from_stream(nlri);
		assert(packet.afi == AFI_BGP_LS);
		assert(bgp_nlri_parse_midr(remote, &attr, &packet) == BGP_NLRI_PARSE_OK);
		assert_selected_sequence(&keys[index], 100 + index);
		assert(bgp_nlri_parse_midr(remote, NULL, &packet) == BGP_NLRI_PARSE_OK);
		assert_selected_missing(&keys[index]);
		bgp_attr_unintern_sub(&attr);
	}

	stream_reset(nlri);
	assert(midr_nlri_encode(nlri, &keys[0]) == MIDR_CODEC_OK);
	packet = packet_from_stream(nlri);
	packet.afi = AFI_IP6;
	assert(bgp_nlri_parse_midr(remote, NULL, &packet) == BGP_NLRI_PARSE_ERROR);

	midr_propagation_path_fini(&path);
	stream_free(nlri);
}

static void test_ipv4_ipv6_identity_isolation(void)
{
	struct midr_ls_object_key ipv4 = node_prefix_key("192.0.2.0/24");
	struct midr_ls_object_key ipv6 = node_prefix_key("::ffff:192.0.2.0/120");
	struct midr_ls_attributes attributes = prefix_attributes(200);
	struct midr_propagation_path path = {};
	struct stream *updates = stream_new(256);
	struct stream *withdraw = stream_new(128);
	struct bgp_nlri packet;
	struct attr attr;

	assert(midr_propagation_path_init(&path, remote->remote_id.s_addr) == 0);
	attr = wire_attr(&attributes, &path);
	assert(midr_nlri_encode(updates, &ipv4) == MIDR_CODEC_OK);
	assert(midr_nlri_encode(updates, &ipv6) == MIDR_CODEC_OK);
	packet = packet_from_stream(updates);
	assert(bgp_nlri_parse_midr(remote, &attr, &packet) == BGP_NLRI_PARSE_OK);
	assert_selected_sequence(&ipv4, 200);
	assert_selected_sequence(&ipv6, 200);

	assert(midr_nlri_encode(withdraw, &ipv6) == MIDR_CODEC_OK);
	packet = packet_from_stream(withdraw);
	assert(bgp_nlri_parse_midr(remote, NULL, &packet) == BGP_NLRI_PARSE_OK);
	assert_selected_sequence(&ipv4, 200);
	assert_selected_missing(&ipv6);

	stream_reset(withdraw);
	assert(midr_nlri_encode(withdraw, &ipv4) == MIDR_CODEC_OK);
	packet = packet_from_stream(withdraw);
	assert(bgp_nlri_parse_midr(remote, NULL, &packet) == BGP_NLRI_PARSE_OK);
	assert_selected_missing(&ipv4);

	bgp_attr_unintern_sub(&attr);
	midr_propagation_path_fini(&path);
	stream_free(withdraw);
	stream_free(updates);
}

int main(void)
{
	as_t asn = 65000;

	qobj_init();
	cmd_init(0);
	bgp_vty_init();
	master = event_master_create("test MIDR packet");
	bgp_master_init(master, BGP_SOCKET_SNDBUF_SIZE, list_new());
	vrf_init(NULL, NULL, NULL, NULL);
	bgp_option_set(BGP_OPT_NO_LISTEN);
	bgp_attr_init();
	assert(bgp_get(&bgp, &asn, NULL, BGP_INSTANCE_TYPE_DEFAULT, NULL, ASNOTATION_PLAIN) >= 0);
	bgp->router_id.s_addr = router_id("10.0.0.1");
	ctx = &bgp->midr_info->ctx;
	remote = peer_create_accept(bgp, NULL);
	assert(remote && remote->connection);
	remote->remote_id.s_addr = router_id("10.0.0.2");
	remote->connection->status = Established;
	remote->afc_nego[AFI_BGP_LS][SAFI_MIDR_LS] = 1;

	test_update_withdraw_and_outbound_path();
	test_ipv6_prefix_update_and_withdraw();
	test_ipv4_ipv6_identity_isolation();
	test_unknown_and_malformed_nlri();
	puts("MIDR packet tests passed");
	return 0;
}
