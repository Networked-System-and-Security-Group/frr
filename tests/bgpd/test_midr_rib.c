// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR SAFI RIB identity and path-selection tests.
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
#include "bgpd/bgp_midr_private.h"
#include "bgpd/bgp_midr_rib.h"
#include "bgpd/bgp_network.h"
#include "bgpd/bgp_vty.h"

/* Required variables when linking against bgpd/libbgp.a. */
struct zebra_privs_t bgpd_privs = {};
struct event_loop *master;

static struct bgp *bgp;
static struct midr_context *ctx;
static struct peer *owner_peer;
static struct peer *peer_two;
static struct peer *peer_three;

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

static struct peer *test_peer(const char *id)
{
	struct peer *peer = peer_create_accept(bgp, NULL);

	assert(peer);
	peer->remote_id.s_addr = router_id(id);
	return peer;
}

static struct midr_ls_object membership(uint32_t originator,
					uint64_t sequence,
					uint32_t group_id)
{
	return (struct midr_ls_object){
		.key =
			{
				.type = MIDR_NLRI_TYPE_MEMBERSHIP,
				.originator_node_id = originator,
			},
		.ls_sequence = sequence,
		.payload.membership =
			{
				.group_id = group_id,
				.has_transport_address = true,
				.transport_address =
					ip_address("192.0.2.1"),
			},
	};
}

static struct midr_ls_object link_object(uint32_t originator,
					 uint32_t remote,
					 uint64_t link_id)
{
	return (struct midr_ls_object){
		.key =
			{
				.type = MIDR_NLRI_TYPE_LINK,
				.originator_node_id = originator,
				.u.link =
					{
						.remote_node_id = remote,
						.link_id = link_id,
					},
			},
		.ls_sequence = 1,
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
						.loss_ppm = 10,
						.available_bandwidth_kbps =
							100000,
					},
			},
	};
}

static struct midr_propagation_path propagation(
	uint32_t originator, uint32_t intermediate,
	uint32_t sender)
{
	struct midr_propagation_path path = {};

	assert(midr_propagation_path_init(&path, originator) == 0);
	if (intermediate)
		assert(midr_propagation_path_append(&path,
						    intermediate)
		       == 0);
	if (sender != originator)
		assert(midr_propagation_path_append(&path, sender) == 0);
	return path;
}

static struct peer *selected_peer(
	const struct midr_ls_object_key *key, uint64_t *sequence)
{
	const struct midr_propagation_path *path;
	struct midr_ls_object selected;
	struct peer *peer;

	assert(midr_rib_selected_get(ctx, key, &selected, &path,
				     &peer) == 0);
	assert(path && path->node_count);
	if (sequence)
		*sequence = selected.ls_sequence;
	return peer;
}

static void assert_empty(void)
{
	struct midr_rib_summary summary;

	assert(midr_rib_summary_get(ctx, &summary) == 0);
	assert(summary.identity_count == 0);
	assert(summary.path_count == 0);
	assert(summary.selected_count == 0);
	assert(summary.conflict_count == 0);
}

static void test_direct_owner_and_sequence_fallback(void)
{
	const uint32_t originator = router_id("1.1.1.1");
	struct midr_ls_object direct = membership(originator, 1, 10);
	struct midr_ls_object indirect =
		membership(originator, 100, 10);
	struct midr_propagation_path direct_path =
		propagation(originator, 0, originator);
	struct midr_propagation_path indirect_path =
		propagation(originator, 0,
			    peer_two->remote_id.s_addr);
	uint64_t sequence;

	assert(midr_rib_path_upsert(ctx, owner_peer, &direct,
				    &direct_path) == 0);
	assert(midr_rib_path_upsert(ctx, peer_two, &indirect,
				    &indirect_path) == 0);
	assert(selected_peer(&direct.key, &sequence) == owner_peer);
	assert(sequence == 1);

	assert(midr_rib_path_withdraw(ctx, owner_peer,
				      &direct.key) == 0);
	assert(selected_peer(&direct.key, &sequence) == peer_two);
	assert(sequence == 100);

	indirect.ls_sequence = 1;
	assert(midr_rib_path_upsert(ctx, peer_two, &indirect,
				    &indirect_path) == 0);
	assert(selected_peer(&direct.key, &sequence) == peer_two);
	assert(sequence == 1);
	assert(midr_rib_path_withdraw(ctx, peer_two,
				      &direct.key) == 0);
	assert_empty();

	midr_propagation_path_fini(&direct_path);
	midr_propagation_path_fini(&indirect_path);
}

static void test_stable_tie_break_and_stale(void)
{
	const uint32_t originator = router_id("4.4.4.4");
	struct midr_ls_object object = membership(originator, 20, 40);
	struct midr_propagation_path path_two =
		propagation(originator, 0,
			    peer_two->remote_id.s_addr);
	struct midr_propagation_path path_three =
		propagation(originator, router_id("8.8.8.8"),
			    peer_three->remote_id.s_addr);

	assert(midr_rib_path_upsert(ctx, peer_two, &object,
				    &path_two) == 0);
	assert(midr_rib_path_upsert(ctx, peer_three, &object,
				    &path_three) == 0);
	assert(selected_peer(&object.key, NULL) == peer_two);

	midr_propagation_path_fini(&path_three);
	path_three = propagation(originator, 0,
				 peer_three->remote_id.s_addr);
	assert(midr_rib_path_upsert(ctx, peer_three, &object,
				    &path_three) == 0);
	assert(selected_peer(&object.key, NULL) == peer_two);

	assert(midr_rib_test_set_path_stale(ctx, &object.key,
					    peer_two, true) == 0);
	assert(selected_peer(&object.key, NULL) == peer_three);
	assert(midr_rib_test_set_path_stale(ctx, &object.key,
					    peer_three, true) == 0);
	assert(midr_rib_selected_get(ctx, &object.key,
				     &(struct midr_ls_object){},
				     &(const struct midr_propagation_path *){0},
				     &(struct peer *){0}) == -ENOENT);

	assert(midr_rib_path_withdraw(ctx, peer_two,
				      &object.key) == 0);
	assert(midr_rib_path_withdraw(ctx, peer_three,
				      &object.key) == 0);
	assert_empty();
	midr_propagation_path_fini(&path_two);
	midr_propagation_path_fini(&path_three);
}

static void test_conflict_quarantine_and_resolution(void)
{
	const uint32_t originator = router_id("5.5.5.5");
	struct midr_ls_object first = membership(originator, 10, 50);
	struct midr_ls_object second = membership(originator, 10, 51);
	struct midr_propagation_path path_two =
		propagation(originator, 0,
			    peer_two->remote_id.s_addr);
	struct midr_propagation_path path_three =
		propagation(originator, 0,
			    peer_three->remote_id.s_addr);
	struct midr_rib_summary summary;
	const struct midr_propagation_path *selected_path;
	struct midr_ls_object selected;
	struct peer *selected_source;

	assert(midr_rib_path_upsert(ctx, peer_two, &first,
				    &path_two) == 0);
	assert(midr_rib_path_upsert(ctx, peer_three, &second,
				    &path_three) == 0);
	assert(midr_rib_summary_get(ctx, &summary) == 0);
	assert(summary.conflict_count == 1);
	assert(summary.selected_count == 0);
	assert(midr_rib_selected_get(
		       ctx, &first.key, &selected, &selected_path,
		       &selected_source) == -ENOENT);

	second.ls_sequence = 11;
	assert(midr_rib_path_upsert(ctx, peer_three, &second,
				    &path_three) == 0);
	assert(selected_peer(&first.key, NULL) == peer_three);
	assert(midr_rib_summary_get(ctx, &summary) == 0);
	assert(summary.conflict_count == 0);

	assert(midr_rib_path_withdraw(ctx, peer_two,
				      &first.key) == 0);
	assert(midr_rib_path_withdraw(ctx, peer_three,
				      &first.key) == 0);
	assert_empty();
	midr_propagation_path_fini(&path_two);
	midr_propagation_path_fini(&path_three);
}

static void test_same_peer_payload_conflict(void)
{
	const uint32_t originator = router_id("6.6.6.6");
	struct midr_ls_object first = membership(originator, 7, 60);
	struct midr_ls_object conflicting =
		membership(originator, 7, 61);
	struct midr_propagation_path path =
		propagation(originator, 0,
			    peer_two->remote_id.s_addr);
	struct midr_rib_summary summary;

	assert(midr_rib_path_upsert(ctx, peer_two, &first, &path) == 0);
	assert(midr_rib_path_upsert(ctx, peer_two, &conflicting,
				    &path) == -EINVAL);
	assert(midr_rib_summary_get(ctx, &summary) == 0);
	assert(summary.rejected_payload_conflict == 1);
	assert_empty();
	midr_propagation_path_fini(&path);
}

static void test_four_object_types(void)
{
	const uint32_t local = bgp->router_id.s_addr;
	struct midr_ls_object objects[4] = {
		membership(local, 1, 100),
		link_object(local, router_id("9.9.9.9"), 99),
		{
			.key =
				{
					.type = MIDR_NLRI_TYPE_NODE_PREFIX,
					.originator_node_id = local,
					.u.node_prefix =
						{
							.afi = AFI_IP,
							.safi =
								SAFI_UNICAST,
						},
				},
			.ls_sequence = 1,
		},
		{
			.key =
				{
					.type = MIDR_NLRI_TYPE_GROUP_PREFIX,
					.originator_node_id = local,
					.u.group_prefix =
						{
							.group_id = 100,
							.prefix =
								{
									.afi =
										AFI_IP6,
									.safi =
										SAFI_UNICAST,
								},
						},
				},
			.ls_sequence = 1,
		},
	};
	struct midr_propagation_path path =
		propagation(local, 0, local);
	size_t index;

	assert(str2prefix("198.51.100.0/24",
			  &objects[2].key.u.node_prefix.prefix) > 0);
	assert(str2prefix("2001:db8::/48",
			  &objects[3].key.u.group_prefix.prefix.prefix) > 0);
	for (index = 0; index < array_size(objects); index++) {
		assert(midr_ls_object_validate(&objects[index]) == 0);
		assert(midr_rib_path_upsert(ctx, bgp->peer_self,
					    &objects[index],
					    &path) == 0);
		assert(selected_peer(&objects[index].key, NULL) ==
		       bgp->peer_self);
	}
	for (index = 0; index < array_size(objects); index++)
		assert(midr_rib_path_withdraw(ctx, bgp->peer_self,
					      &objects[index].key) == 0);
	assert_empty();
	midr_propagation_path_fini(&path);
}

static void test_identity_limit(void)
{
	const uint32_t first_id = router_id("7.7.7.7");
	const uint32_t second_id = router_id("8.8.8.8");
	struct midr_ls_object first = membership(first_id, 1, 70);
	struct midr_ls_object second = membership(second_id, 1, 80);
	struct midr_propagation_path first_path =
		propagation(first_id, 0,
			    peer_two->remote_id.s_addr);
	struct midr_propagation_path second_path =
		propagation(second_id, 0,
			    peer_two->remote_id.s_addr);
	struct midr_rib_summary summary;

	assert(midr_rib_test_set_identity_limit(ctx, 1) == 0);
	assert(midr_rib_path_upsert(ctx, peer_two, &first,
				    &first_path) == 0);
	assert(midr_rib_path_upsert(ctx, peer_two, &second,
				    &second_path) == -ENOSPC);
	first.ls_sequence = 2;
	assert(midr_rib_path_upsert(ctx, peer_two, &first,
				    &first_path) == 0);
	assert(midr_rib_path_withdraw(ctx, peer_two,
				      &first.key) == 0);
	assert(midr_rib_path_upsert(ctx, peer_two, &second,
				    &second_path) == 0);
	assert(midr_rib_summary_get(ctx, &summary) == 0);
	assert(summary.rejected_limit == 1);
	assert(midr_rib_path_withdraw(ctx, peer_two,
				      &second.key) == 0);
	assert(midr_rib_test_set_identity_limit(
		       ctx, MIDR_RIB_MAX_IDENTITIES) == 0);
	assert_empty();
	midr_propagation_path_fini(&first_path);
	midr_propagation_path_fini(&second_path);
}

int main(void)
{
	as_t asn = 65000;

	qobj_init();
	cmd_init(0);
	bgp_vty_init();
	master = event_master_create("test MIDR RIB");
	bgp_master_init(master, BGP_SOCKET_SNDBUF_SIZE, list_new());
	vrf_init(NULL, NULL, NULL, NULL);
	bgp_option_set(BGP_OPT_NO_LISTEN);
	bgp_attr_init();
	assert(bgp_get(&bgp, &asn, NULL, BGP_INSTANCE_TYPE_DEFAULT,
		       NULL, ASNOTATION_PLAIN) >= 0);
	bgp->router_id.s_addr = router_id("10.0.0.1");
	ctx = &bgp->midr_info->ctx;
	owner_peer = test_peer("1.1.1.1");
	peer_two = test_peer("2.2.2.2");
	peer_three = test_peer("3.3.3.3");

	test_direct_owner_and_sequence_fallback();
	test_stable_tie_break_and_stale();
	test_conflict_quarantine_and_resolution();
	test_same_peer_payload_conflict();
	test_four_object_types();
	test_identity_limit();

	puts("MIDR RIB tests passed");
	return 0;
}
