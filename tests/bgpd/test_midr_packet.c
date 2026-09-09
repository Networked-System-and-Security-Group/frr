// SPDX-License-Identifier: GPL-2.0-or-later
/* MIDR MP_REACH/MP_UNREACH and single-instance UPDATE tests. */

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
#include "bgpd/bgp_midr_codec.h"
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

static struct midr_instance membership(uint64_t sequence, uint32_t group_id)
{
	return (struct midr_instance){
		.state = MIDR_INSTANCE_ACTIVE,
		.object = {
			.key = {
				.type = MIDR_NLRI_TYPE_MEMBERSHIP,
				.originator_node_id = remote->remote_id.s_addr,
			},
			.ls_sequence = sequence,
			.payload.membership = {
				.group_id = group_id,
				.cap_flags = 1,
			},
		},
	};
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

static struct attr instance_attr(const struct midr_instance *instance,
				 uint32_t age_ms)
{
	struct attr attr = {};

	attr.midr_ls = bgp_midr_instance_attr_intern(instance, age_ms);
	assert(attr.midr_ls);
	return attr;
}

static void encode_packet(struct stream *nlri, const struct midr_instance *instance)
{
	assert(midr_nlri_encode(nlri, &instance->object.key) == MIDR_CODEC_OK);
}

struct encode_state {
	const struct midr_ls_object_key *key;
	struct stream *stream;
	struct bgp_dest *dest;
	struct bgp_path_info *path;
	int result;
};

static int encode_selected(const struct midr_instance *instance, struct peer *peer,
				   struct bgp_dest *dest, struct bgp_path_info *path,
				   void *arg)
{
	struct encode_state *state = arg;

	(void)peer;
	if (!midr_ls_object_key_same(&instance->object.key, state->key))
		return 0;
	state->dest = dest;
	state->path = path;
	state->result = bgp_midr_packet_attributes(state->stream, bgp, path);
	return 0;
}

static void test_active_withdraw_and_mp_unreach(void)
{
	struct midr_instance active = membership(7, 20);
	struct midr_instance withdrawn = active;
	struct midr_instance selected;
	struct peer *selected_peer;
	struct stream *nlri = stream_new(128);
	struct bgp_nlri packet;
	struct attr attr;

	withdrawn.state = MIDR_INSTANCE_WITHDRAWN;
	withdrawn.object.ls_sequence = 8;
	memset(&withdrawn.object.payload, 0, sizeof(withdrawn.object.payload));

	attr = instance_attr(&active, 12);
	encode_packet(nlri, &active);
	packet = packet_from_stream(nlri);
	assert(bgp_nlri_parse_midr(remote, &attr, &packet) == BGP_NLRI_PARSE_OK);
	assert(midr_rib_selected_instance_get(ctx, &active.object.key, &selected,
						     NULL, &selected_peer) == 0);
	assert(selected.state == MIDR_INSTANCE_ACTIVE);
	assert(selected.object.ls_sequence == 7);
	assert(selected_peer == remote);

	/* MP_UNREACH removes only the sender advertisement relationship. */
	assert(bgp_nlri_parse_midr(remote, NULL, &packet) == BGP_NLRI_PARSE_OK);
	assert(midr_rib_selected_instance_get(ctx, &active.object.key, &selected,
						     NULL, &selected_peer) == 0);
	assert(selected.state == MIDR_INSTANCE_ACTIVE);
	assert(!midr_rib_peer_advertisement_has(ctx, &active.object.key, remote));

	bgp_attr_unintern_sub(&attr);
	stream_reset(nlri);
	attr = instance_attr(&withdrawn, 20);
	encode_packet(nlri, &withdrawn);
	packet = packet_from_stream(nlri);
	assert(bgp_nlri_parse_midr(remote, &attr, &packet) == BGP_NLRI_PARSE_OK);
	/* The withdrawn instance remains selected for flooding, but the
	 * active-view accessor must not expose it as usable state. */
	assert(midr_rib_selected_instance_get(ctx, &active.object.key, &selected,
						     NULL, &selected_peer) == -ENOENT);

	bgp_attr_unintern_sub(&attr);
	stream_free(nlri);
}

static void test_outbound_snapshot_and_single_nlri(void)
{
	struct midr_instance active = membership(20, 30);
	struct stream *nlri = stream_new(128);
	struct stream *encoded = stream_new(256);
	struct bgp_nlri packet;
	struct attr attr;
	struct encode_state state = {
		.key = &active.object.key,
		.stream = encoded,
		.result = -1,
	};
	struct attr parsed = {};
	size_t length;

	attr = instance_attr(&active, 33);
	encode_packet(nlri, &active);
	packet = packet_from_stream(nlri);
	assert(bgp_nlri_parse_midr(remote, &attr, &packet) == BGP_NLRI_PARSE_OK);
	assert(midr_rib_selected_entry_foreach(ctx, encode_selected, &state) == 0);
	assert(state.result > 0 && state.dest && state.path);
	assert(stream_getc_from(encoded, 0) ==
	       (BGP_ATTR_FLAG_OPTIONAL | BGP_ATTR_FLAG_EXTLEN));
	assert(stream_getc_from(encoded, 1) == BGP_ATTR_MIDR_LS);
	length = stream_getw_from(encoded, 2);
	assert(length == stream_get_endp(encoded) - 4);
	assert(bgp_midr_attr_decode(&parsed, BGP_ATTR_MIDR_LS,
					   BGP_ATTR_FLAG_OPTIONAL | BGP_ATTR_FLAG_EXTLEN,
					   STREAM_DATA(encoded) + 4, length) ==
		       BGP_ATTR_PARSE_PROCEED);
	assert(bgp_midr_ls_attr_state(parsed.midr_ls) == MIDR_INSTANCE_ACTIVE);
	assert(bgp_midr_ls_attr_age(parsed.midr_ls) == 33);
	bgp_attr_unintern_sub(&parsed);

	/* A second NLRI in the same UPDATE is rejected before RIB mutation. */
	encode_packet(nlri, &active);
	packet = packet_from_stream(nlri);
	assert(bgp_nlri_parse_midr(remote, &attr, &packet) == BGP_NLRI_PARSE_ERROR);
	bgp_attr_unintern_sub(&attr);
	stream_free(encoded);
	stream_free(nlri);
}

static void test_packet_validation(void)
{
	struct midr_instance active = membership(40, 40);
	struct stream *nlri = stream_new(128);
	struct bgp_nlri packet;
	struct attr attr;

	attr = instance_attr(&active, 0);
	encode_packet(nlri, &active);
	stream_putw(nlri, 0xffff);
	stream_putw(nlri, 0);
	packet = packet_from_stream(nlri);
	assert(bgp_nlri_parse_midr(remote, &attr, &packet) == BGP_NLRI_PARSE_ERROR);
	bgp_attr_unintern_sub(&attr);
	stream_free(nlri);
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
	assert(bgp_get(&bgp, &asn, NULL, BGP_INSTANCE_TYPE_DEFAULT, NULL,
		       ASNOTATION_PLAIN) >= 0);
	bgp->router_id.s_addr = router_id("10.0.0.1");
	ctx = &bgp->midr_info->ctx;
	remote = peer_create_accept(bgp, NULL);
	assert(remote && remote->connection);
	remote->remote_id.s_addr = router_id("10.0.0.2");
	remote->connection->status = Established;
	remote->afc_nego[AFI_BGP_LS][SAFI_MIDR_LS] = 1;

	test_active_withdraw_and_mp_unreach();
	test_outbound_snapshot_and_single_nlri();
	test_packet_validation();
	puts("MIDR packet tests passed");
	return 0;
}
