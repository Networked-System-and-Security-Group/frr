// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR locally originated Membership and Link tests.
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
#include "bgpd/bgp_midr_owned.h"
#include "bgpd/bgp_midr_private.h"
#include "bgpd/bgp_midr_rib.h"
#include "bgpd/bgp_network.h"
#include "bgpd/bgp_vty.h"

struct zebra_privs_t bgpd_privs = {};
struct event_loop *master;

struct memory_sequence_store {
	uint32_t epoch;
	bool found;
	bool fail_save;
};

static struct bgp *bgp;
static struct midr_context *ctx;
static struct memory_sequence_store sequence_store;

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

static int sequence_load(void *arg, uint32_t node_id, uint32_t *epoch,
			 bool *found)
{
	struct memory_sequence_store *store = arg;

	(void)node_id;
	*epoch = store->epoch;
	*found = store->found;
	return 0;
}

static int sequence_save(void *arg, uint32_t node_id, uint32_t epoch)
{
	struct memory_sequence_store *store = arg;

	(void)node_id;
	if (store->fail_save)
		return -EIO;
	store->epoch = epoch;
	store->found = true;
	return 0;
}

static const struct midr_sequence_store_ops sequence_ops = {
	.load_epoch = sequence_load,
	.save_epoch = sequence_save,
};

static struct midr_node_update node_update(uint64_t version,
					   uint32_t group_id)
{
	return (struct midr_node_update){
		.node_id = bgp->router_id.s_addr,
		.group_id = group_id,
		.has_transport_address = true,
		.transport_address = ip_address("192.0.2.1"),
		.cap_flags = 7,
		.policy_state = MIDR_POLICY_ALLOWED,
		.policy_tags = 9,
		.version = version,
	};
}

static struct midr_link_update link_update(uint64_t version,
					   uint32_t rtt_us)
{
	return (struct midr_link_update){
		.key =
			{
				.local_node_id = bgp->router_id.s_addr,
				.remote_node_id = router_id("10.0.0.2"),
				.link_id = 100,
			},
		.local_ifindex = 7,
		.link_local_address = ip_address("198.51.100.1"),
		.link_remote_address = ip_address("198.51.100.2"),
		.metrics =
			{
				.has_rtt_us = true,
				.rtt_us = rtt_us,
				.has_loss_ppm = true,
				.loss_ppm = 100,
				.has_available_bandwidth_kbps = true,
				.available_bandwidth_kbps = 100000,
				.measurement_seqno = version,
				.measurement_timestamp_ms = version * 1000,
			},
		.policy_state = MIDR_POLICY_ALLOWED,
		.policy_tags = 11,
		.version = version,
	};
}

static uint64_t selected_sequence(const struct midr_ls_object_key *key,
				  struct midr_ls_object *selected)
{
	const struct midr_propagation_path *path;
	struct peer *peer;

	assert(midr_rib_selected_get(ctx, key, selected, &path, &peer) == 0);
	assert(peer == bgp->peer_self);
	assert(path->node_count == 1);
	return selected->ls_sequence;
}

static void assert_selected_missing(const struct midr_ls_object_key *key)
{
	const struct midr_propagation_path *path;
	struct midr_ls_object selected;
	struct peer *peer;

	assert(midr_rib_selected_get(ctx, key, &selected, &path, &peer) ==
	       -ENOENT);
}

static void test_origination_suppression_and_withdraw(void)
{
	struct midr_ls_object_key membership_key = {
		.type = MIDR_NLRI_TYPE_MEMBERSHIP,
		.originator_node_id = bgp->router_id.s_addr,
	};
	struct midr_ls_object_key link_key = {
		.type = MIDR_NLRI_TYPE_LINK,
		.originator_node_id = bgp->router_id.s_addr,
		.u.link =
			{
				.remote_node_id = router_id("10.0.0.2"),
				.link_id = 100,
			},
	};
	struct midr_owned_summary summary;
	struct midr_ls_object selected;
	struct midr_node_update node;
	struct midr_link_update link;
	uint64_t membership_sequence;
	uint64_t link_sequence;
	ifindex_t ifindex;

	node = node_update(1, 10);
	link = link_update(1, 1000);
	assert(midr_topology_node_upsert(ctx, &node) == 0);
	assert(midr_topology_link_upsert(ctx, &link) == 0);
	midr_topology_process_pending(ctx);
	membership_sequence = selected_sequence(&membership_key, &selected);
	link_sequence = selected_sequence(&link_key, &selected);
	assert(selected.payload.link.metrics.rtt_us == 1000);

	node.version = 2;
	link.version = 2;
	link.local_ifindex = 17;
	link.metrics.measurement_seqno = 200;
	link.metrics.measurement_timestamp_ms = 200000;
	assert(midr_topology_node_upsert(ctx, &node) == 0);
	assert(midr_topology_link_upsert(ctx, &link) == 0);
	midr_topology_process_pending(ctx);
	assert(selected_sequence(&membership_key, &selected) ==
	       membership_sequence);
	assert(selected_sequence(&link_key, &selected) == link_sequence);
	assert(midr_owned_link_metadata_get(ctx, &link_key, &ifindex) == 0);
	assert(ifindex == 17);

	link = link_update(3, 1010);
	link.local_ifindex = 17;
	assert(midr_topology_link_upsert(ctx, &link) == 0);
	midr_topology_process_pending(ctx);
	assert(selected_sequence(&link_key, &selected) == link_sequence);
	assert(midr_owned_summary_get(ctx, &summary) == 0);
	assert(summary.suppressed_link_count == 1);

	link = link_update(4, 3000);
	link.local_ifindex = 17;
	assert(midr_topology_link_upsert(ctx, &link) == 0);
	midr_topology_process_pending(ctx);
	assert(selected_sequence(&link_key, &selected) == link_sequence);
	assert(midr_owned_summary_get(ctx, &summary) == 0);
	assert(summary.pending_timer_count == 1);
	midr_owned_test_fire_timers(ctx);
	assert(selected_sequence(&link_key, &selected) > link_sequence);
	assert(selected.payload.link.metrics.rtt_us == 3000);

	link.version = 5;
	link.policy_state = MIDR_POLICY_BLOCKED;
	assert(midr_topology_link_upsert(ctx, &link) == 0);
	midr_topology_process_pending(ctx);
	assert_selected_missing(&link_key);

	node = node_update(3, 0);
	assert(midr_topology_node_upsert(ctx, &node) == 0);
	midr_topology_process_pending(ctx);
	assert(midr_owned_summary_get(ctx, &summary) == 0);
	assert(summary.membership_count == 0);
	assert(summary.link_count == 0);
}

static void test_persistence_failure_and_fightback(void)
{
	struct midr_ls_object_key membership_key = {
		.type = MIDR_NLRI_TYPE_MEMBERSHIP,
		.originator_node_id = bgp->router_id.s_addr,
	};
	struct midr_owned_summary summary;
	struct midr_ls_object selected;
	struct midr_node_update node;
	uint64_t observed;

	node = node_update(10, 10);
	assert(midr_topology_node_upsert(ctx, &node) == 0);
	midr_topology_process_pending(ctx);
	(void)selected_sequence(&membership_key, &selected);

	sequence_store.fail_save = true;
	selected.ls_sequence =
		(((selected.ls_sequence >> 32) + 1) << 32);
	assert(midr_owned_observe_self_sequence(ctx, &selected) == -EIO);
	assert(midr_owned_summary_get(ctx, &summary) == 0);
	assert(!summary.ready);
	assert(summary.sequence_failures == 1);
	assert_selected_missing(&membership_key);

	node.version = 11;
	node.group_id = 20;
	assert(midr_topology_node_upsert(ctx, &node) == 0);
	midr_topology_process_pending(ctx);
	assert_selected_missing(&membership_key);

	sequence_store.fail_save = false;
	midr_owned_identity_start(ctx, bgp->router_id.s_addr);
	midr_owned_reconcile(ctx);
	observed = selected_sequence(&membership_key, &selected);
	assert(selected.payload.membership.group_id == 20);

	selected.ls_sequence += (UINT64_C(1) << 32);
	observed = selected.ls_sequence;
	assert(midr_owned_observe_self_sequence(ctx, &selected) == 0);
	assert(selected_sequence(&membership_key, &selected) > observed);
	assert(midr_owned_summary_get(ctx, &summary) == 0);
	assert(summary.fightbacks == 1);
}

int main(void)
{
	as_t asn = 65000;

	qobj_init();
	cmd_init(0);
	bgp_vty_init();
	master = event_master_create("test MIDR owned objects");
	bgp_master_init(master, BGP_SOCKET_SNDBUF_SIZE, list_new());
	vrf_init(NULL, NULL, NULL, NULL);
	bgp_option_set(BGP_OPT_NO_LISTEN);
	bgp_attr_init();
	assert(bgp_get(&bgp, &asn, NULL, BGP_INSTANCE_TYPE_DEFAULT, NULL,
		       ASNOTATION_PLAIN) >= 0);
	ctx = &bgp->midr_info->ctx;
	assert(midr_owned_test_set_sequence_store(ctx, &sequence_ops,
						  &sequence_store) == 0);
	bgp->router_id.s_addr = router_id("10.0.0.1");
	assert(midr_input_router_id_update(bgp, false) == 0);
	midr_input_test_resync_now(ctx);

	test_origination_suppression_and_withdraw();
	test_persistence_failure_and_fightback();
	puts("MIDR owned object tests passed");
	return 0;
}
