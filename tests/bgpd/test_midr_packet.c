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
#include "bgpd/bgp_midr_canonical.h"
#include "bgpd/bgp_midr_codec.h"
#include "bgpd/bgp_midr_packet.h"
#include "bgpd/bgp_midr_private.h"
#include "bgpd/bgp_midr_rib.h"
#include "bgpd/bgp_midr_sync.h"
#include "bgpd/bgp_network.h"
#include "bgpd/bgp_updgrp.h"
#include "bgpd/bgp_vty.h"

struct zebra_privs_t bgpd_privs = {};
struct event_loop *master;

static struct bgp *bgp;
static struct midr_context *ctx;
static struct peer *remote;

#define TEST_BASE_NS 1000000000000000000ULL

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
	struct bpacket_attr_vec_arr *vecarr;
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
	state->result = bgp_midr_packet_attributes(state->stream, bgp, path,
						   state->vecarr);
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
	struct midr_instance newer = membership(21, 31);
	struct stream *nlri = stream_new(128);
	struct stream *encoded = stream_new(256);
	struct stream *reformatted;
	struct stream *wire;
	struct bgp_nlri packet;
	struct attr attr;
	struct attr newer_attr;
	struct bpacket_attr_vec_arr vecarr;
	struct bpacket_queue queue;
	struct bpacket *template;
	struct peer_af paf = {
		.peer = remote,
		.afi = AFI_BGP_LS,
		.safi = SAFI_MIDR_LS,
	};
	const uint64_t base_ns = TEST_BASE_NS;
	struct encode_state state = {
		.key = &active.object.key,
		.stream = encoded,
		.vecarr = &vecarr,
		.result = -1,
	};
	struct attr parsed = {};
	struct midr_instance_attributes wire_attributes;
	size_t length;

	assert(midr_rib_test_set_now_ns(ctx, base_ns) == 0);
	bpacket_attr_vec_arr_reset(&vecarr);
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

	/* The shared template retains the exact canonical instance.  Replacing
	 * the RIB path before peer reformat must not invalidate or retarget it. */
	bpacket_queue_init(&queue);
	bpacket_queue_add(&queue, NULL, NULL);
	template = bpacket_queue_add(&queue, encoded, &vecarr);
	encoded = NULL;
	assert(template && template->arr.midr_instance);
	assert(midr_rib_test_set_now_ns(ctx, base_ns + 100000000ULL) == 0);
	newer_attr = instance_attr(&newer, 44);
	stream_reset(nlri);
	encode_packet(nlri, &newer);
	packet = packet_from_stream(nlri);
	assert(bgp_nlri_parse_midr(remote, &newer_attr, &packet) ==
	       BGP_NLRI_PARSE_OK);
	bgp_attr_unintern_sub(&newer_attr);

	/* MRAI/coalesce/template delay is included at peer-private reformat,
	 * then one B is added for the not-yet-counted delivery interval. */
	assert(midr_rib_test_set_now_ns(ctx, base_ns + 500000000ULL) == 0);
	reformatted = bpacket_reformat_for_peer(template, &paf);
	assert(reformatted);
	length = stream_getw_from(reformatted, 2);
	wire = stream_new(length);
	stream_put(wire, STREAM_DATA(reformatted) + 4, length);
	assert(midr_instance_attribute_decode(wire, length, &wire_attributes) ==
	       MIDR_CODEC_OK);
	assert(wire_attributes.ls.ls_sequence == 20);
	assert(wire_attributes.age_ms ==
	       33 + 500 + MIDR_CANONICAL_FORWARD_BUDGET_MS);
	assert(stream_get_monotime_ns(reformatted) == base_ns + 500000000ULL);
	stream_free(wire);
	stream_free(reformatted);

	/* A template that has itself reached L is discarded, never encoded as
	 * a deceptively young or wrapped instance. */
	assert(midr_rib_test_set_now_ns(
		       ctx, base_ns + (uint64_t)MIDR_CANONICAL_MAX_AGE_MS *
				      1000000ULL) == 0);
	assert(bpacket_reformat_for_peer(template, &paf) == NULL);
	bpacket_queue_cleanup(&queue);

	/* A second NLRI in the same UPDATE is rejected before RIB mutation. */
	encode_packet(nlri, &active);
	packet = packet_from_stream(nlri);
	assert(bgp_nlri_parse_midr(remote, &attr, &packet) == BGP_NLRI_PARSE_ERROR);
	bgp_attr_unintern_sub(&attr);
	stream_free(encoded);
	stream_free(nlri);
}

static void test_receive_fifo_age_and_clock_regression(void)
{
	struct midr_instance received = membership(500, 50);
	struct midr_instance regressed = membership(501, 51);
	struct midr_instance selected;
	struct peer *selected_peer;
	struct midr_rib_summary rib_before, rib_after;
	struct midr_sync_status sync_before, sync_after;
	struct stream *nlri = stream_new(128);
	struct stream *incoming = stream_new(64);
	struct bgp_nlri packet;
	struct attr attr;
	uint32_t age_ms;
	const uint64_t received_ns =
		TEST_BASE_NS + 10000000000ULL +
		(uint64_t)MIDR_CANONICAL_MAX_AGE_MS * 1000000ULL;

	stream_set_monotime_ns(incoming, received_ns);
	remote->connection->curr = incoming;
	assert(midr_rib_test_set_now_ns(ctx, received_ns + 250000000ULL) == 0);
	attr = instance_attr(&received, 100);
	encode_packet(nlri, &received);
	packet = packet_from_stream(nlri);
	assert(bgp_nlri_parse_midr(remote, &attr, &packet) == BGP_NLRI_PARSE_OK);
	assert(midr_rib_selected_instance_get(ctx, &received.object.key,
					      &selected, &age_ms,
					      &selected_peer) == 0);
	assert(age_ms == 350);
	bgp_attr_unintern_sub(&attr);
	remote->connection->curr = NULL;
	stream_free(incoming);

	/* A monotonic regression is an internal time failure, not capacity
	 * pressure: it must not enter the resource-resync/backoff path. */
	assert(midr_rib_summary_get(ctx, &rib_before) == 0);
	assert(midr_sync_status_get(ctx, &sync_before) == 0);
	assert(midr_rib_test_set_now_ns(ctx, received_ns + 249000000ULL) == 0);
	stream_reset(nlri);
	attr = instance_attr(&regressed, 100);
	encode_packet(nlri, &regressed);
	packet = packet_from_stream(nlri);
	assert(bgp_nlri_parse_midr(remote, &attr, &packet) ==
	       BGP_NLRI_PARSE_ERROR);
	assert(midr_rib_summary_get(ctx, &rib_after) == 0);
	assert(midr_sync_status_get(ctx, &sync_after) == 0);
	assert(rib_after.rejected_internal == rib_before.rejected_internal + 1);
	assert(rib_after.rejected_resource == rib_before.rejected_resource);
	assert(sync_after.input_rejected_count == sync_before.input_rejected_count);
	bgp_attr_unintern_sub(&attr);
	assert(midr_rib_test_set_now_ns(ctx, received_ns + 251000000ULL) == 0);
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

static void test_resource_rejection_keeps_session(void)
{
	struct midr_instance third_party = {
		.state = MIDR_INSTANCE_ACTIVE,
		.object = {
			.key = {
				.type = MIDR_NLRI_TYPE_MEMBERSHIP,
				.originator_node_id = router_id("10.0.0.9"),
			},
			.ls_sequence = 30,
			.payload.membership = {
				.group_id = 21,
				.cap_flags = 1,
			},
		},
	};
	struct midr_instance refresh = membership(600, 23);
	struct midr_sync_status status;
	struct midr_instance selected;
	struct peer *selected_peer;
	struct stream *nlri = stream_new(128);
	struct bgp_nlri packet;
	struct attr attr;
	uint32_t age;

	midr_sync_test_session_start(ctx, remote, remote->connection);
	assert(midr_rib_test_set_identity_limit(ctx, 1) == 0);

	/* A new identity beyond the limit is a resource rejection, not a
	 * protocol error: the parse must keep the session up, record the
	 * unadmitted input, and leave the identity unselected. */
	attr = instance_attr(&third_party, 5);
	encode_packet(nlri, &third_party);
	packet = packet_from_stream(nlri);
	assert(bgp_nlri_parse_midr(remote, &attr, &packet) == BGP_NLRI_PARSE_OK);
	assert(midr_sync_status_get(ctx, &status) == 0);
	assert(status.input_rejected_count == 1);
	assert(status.resync_required_count == 1);
	assert(midr_rib_selected_instance_get(ctx, &third_party.object.key,
					      &selected, &age,
					      &selected_peer) == -ENOENT);

	/* An update of an existing identity is still admitted at the limit. */
	{
		struct attr refresh_attr = instance_attr(&refresh, 6);

		stream_reset(nlri);
		encode_packet(nlri, &refresh);
		packet = packet_from_stream(nlri);
		assert(bgp_nlri_parse_midr(remote, &refresh_attr, &packet) ==
		       BGP_NLRI_PARSE_OK);
		assert(midr_rib_selected_instance_get(ctx, &refresh.object.key,
						      &selected, &age,
						      &selected_peer) == 0);
		assert(selected.object.ls_sequence == 600);
		assert(midr_sync_status_get(ctx, &status) == 0);
		assert(status.input_rejected_count == 1);
		bgp_attr_unintern_sub(&refresh_attr);
	}

	/* Once capacity is restored, the same unadmitted input is accepted. */
	assert(midr_rib_test_set_identity_limit(ctx, MIDR_RIB_MAX_IDENTITIES) == 0);
	stream_reset(nlri);
	encode_packet(nlri, &third_party);
	packet = packet_from_stream(nlri);
	assert(bgp_nlri_parse_midr(remote, &attr, &packet) == BGP_NLRI_PARSE_OK);
	assert(midr_rib_selected_instance_get(ctx, &third_party.object.key,
					      &selected, &age,
					      &selected_peer) == 0);
	assert(selected.object.ls_sequence == 30);

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
	assert(midr_rib_test_set_now_ns(ctx, TEST_BASE_NS) == 0);
	remote = peer_create_accept(bgp, NULL);
	assert(remote && remote->connection);
	remote->remote_id.s_addr = router_id("10.0.0.2");
	remote->connection->status = Established;
	remote->afc_nego[AFI_BGP_LS][SAFI_MIDR_LS] = 1;

	test_active_withdraw_and_mp_unreach();
	test_outbound_snapshot_and_single_nlri();
	test_receive_fifo_age_and_clock_regression();
	test_packet_validation();
	test_resource_rejection_keeps_session();
	puts("MIDR packet tests passed");
	return 0;
}
