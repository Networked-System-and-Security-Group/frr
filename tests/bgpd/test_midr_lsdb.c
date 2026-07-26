// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR selected-object database and production TED tests.
 */

#include <zebra.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#include "command.h"
#include "privs.h"
#include "qobj.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_attr.h"
#include "bgpd/bgp_midr_cost.h"
#include "bgpd/bgp_midr_lsdb.h"
#include "bgpd/bgp_midr_owned.h"
#include "bgpd/bgp_midr_private.h"
#include "bgpd/bgp_midr_rib.h"
#include "bgpd/bgp_midr_ted_private.h"
#include "bgpd/bgp_network.h"
#include "bgpd/bgp_vty.h"
#include "tests/bgpd/midr_ted_mock_provider.h"

struct zebra_privs_t bgpd_privs = {};
struct event_loop *master;

struct memory_sequence_store {
	uint32_t epoch;
	bool found;
};

struct remote_callback_state {
	size_t node_updates;
	size_t node_withdraws;
	size_t link_updates;
	size_t link_withdraws;
};

static struct bgp *bgp;
static struct midr_context *ctx;
static struct peer *remote_peer;
static struct memory_sequence_store sequence_store;
static struct remote_callback_state callbacks;

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

static struct prefix ipv4_prefix(const char *text)
{
	struct prefix prefix;

	assert(str2prefix(text, &prefix) > 0);
	return prefix;
}

static int sequence_load(void *arg, uint32_t node_id, uint32_t *epoch, bool *found)
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
	store->epoch = epoch;
	store->found = true;
	return 0;
}

static const struct midr_sequence_store_ops sequence_ops = {
	.load_epoch = sequence_load,
	.save_epoch = sequence_save,
};

static struct midr_node_update local_node(uint64_t version, uint32_t group_id)
{
	return (struct midr_node_update){
		.node_id = bgp->router_id.s_addr,
		.group_id = group_id,
		.has_transport_address = true,
		.transport_address = ip_address("192.0.2.1"),
		.policy_state = MIDR_POLICY_ALLOWED,
		.version = version,
	};
}

static struct midr_link_update local_link(uint64_t version)
{
	return (struct midr_link_update){
		.key =
			{
				.local_node_id = bgp->router_id.s_addr,
				.remote_node_id = remote_peer->remote_id.s_addr,
				.link_id = 1,
			},
		.local_ifindex = 9,
		.link_local_address = ip_address("198.51.100.1"),
		.link_remote_address = ip_address("198.51.100.2"),
		.metrics =
			{
				.has_rtt_us = true,
				.rtt_us = 1000,
				.has_loss_ppm = true,
				.loss_ppm = 100,
				.has_available_bandwidth_kbps = true,
				.available_bandwidth_kbps = 100000,
			},
		.policy_state = MIDR_POLICY_ALLOWED,
		.version = version,
	};
}

static struct midr_ls_object remote_membership(uint64_t sequence, uint32_t group_id)
{
	return (struct midr_ls_object){
		.key =
			{
				.type = MIDR_NLRI_TYPE_MEMBERSHIP,
				.originator_node_id =
					remote_peer->remote_id.s_addr,
			},
		.ls_sequence = sequence,
		.payload.membership =
			{
				.group_id = group_id,
				.has_transport_address = true,
				.transport_address =
					ip_address("192.0.2.2"),
			},
	};
}

static struct midr_ls_object remote_link(uint64_t sequence)
{
	return (struct midr_ls_object){
		.key =
			{
				.type = MIDR_NLRI_TYPE_LINK,
				.originator_node_id =
					remote_peer->remote_id.s_addr,
				.u.link =
					{
						.remote_node_id =
							bgp->router_id.s_addr,
						.link_id = 2,
					},
			},
		.ls_sequence = sequence,
		.payload.link =
			{
				.link_local_address =
					ip_address("198.51.100.2"),
				.link_remote_address =
					ip_address("198.51.100.1"),
				.metrics =
					{
						.present_flags =
							MIDR_METRIC_REQUIRED_MASK,
						.rtt_us = 1200,
						.loss_ppm = 200,
						.available_bandwidth_kbps =
							90000,
					},
			},
	};
}

static struct midr_ls_object remote_node_prefix(uint64_t sequence)
{
	return (struct midr_ls_object){
		.key =
			{
				.type = MIDR_NLRI_TYPE_NODE_PREFIX,
				.originator_node_id =
					remote_peer->remote_id.s_addr,
				.u.node_prefix =
					{
						.afi = AFI_IP,
						.safi = SAFI_UNICAST,
						.prefix =
							ipv4_prefix(
								"203.0.113.0/24"),
					},
			},
		.ls_sequence = sequence,
	};
}

static struct midr_ls_object remote_group_prefix(uint64_t sequence, uint32_t group_id)
{
	return (struct midr_ls_object){
		.key =
			{
				.type = MIDR_NLRI_TYPE_GROUP_PREFIX,
				.originator_node_id =
					remote_peer->remote_id.s_addr,
				.u.group_prefix =
					{
						.group_id = group_id,
						.prefix =
							{
								.afi = AFI_IP,
								.safi =
									SAFI_UNICAST,
								.prefix =
									ipv4_prefix(
										"198.18.0.0/15"),
							},
					},
			},
		.ls_sequence = sequence,
	};
}

static void remote_node_update(const struct midr_remote_node_info *node)
{
	assert(node->node_id == remote_peer->remote_id.s_addr);
	callbacks.node_updates++;
}

static void remote_node_withdraw(uint32_t node_id, uint64_t sequence)
{
	assert(node_id == remote_peer->remote_id.s_addr);
	assert(sequence);
	callbacks.node_withdraws++;
}

static void remote_link_update(const struct midr_remote_link_info *link)
{
	assert(link->key.local_node_id == remote_peer->remote_id.s_addr);
	callbacks.link_updates++;
}

static void remote_link_withdraw(const struct midr_link_key *key, uint64_t sequence)
{
	assert(key->local_node_id == remote_peer->remote_id.s_addr);
	assert(sequence);
	callbacks.link_withdraws++;
}

static void install_remote(const struct midr_ls_object *object)
{
	struct midr_propagation_path path = {};

	assert(midr_propagation_path_init(&path, remote_peer->remote_id.s_addr) == 0);
	assert(midr_rib_path_upsert(ctx, remote_peer, object, &path) == 0);
	midr_propagation_path_fini(&path);
}

static void assert_mock_matches_real(const struct midr_ted_snapshot *real,
				     const struct midr_ls_object *remote_link_object)
{
	char path[] = "/tmp/midr-m4-mock-XXXXXX";
	char error[256];
	struct midr_ls_metrics local_metrics = {
		.present_flags = MIDR_METRIC_REQUIRED_MASK,
		.rtt_us = 1000,
		.loss_ppm = 100,
		.available_bandwidth_kbps = 100000,
	};
	const struct midr_ted_snapshot *mock = NULL;
	FILE *file;
	uint32_t local_cost;
	uint32_t remote_cost;
	int fd;

	assert(midr_cost_from_metrics(&local_metrics, &local_cost) == 0);
	assert(midr_cost_from_metrics(&remote_link_object->payload.link.metrics, &remote_cost) ==
	       0);
	fd = mkstemp(path);
	assert(fd >= 0);
	file = fdopen(fd, "w");
	assert(file);
	assert(fprintf(file,
		       "{"
		       "\"schema_version\":1,"
		       "\"local_node_id\":\"10.0.0.1\","
		       "\"local_group_id\":10,"
		       "\"nodes\":["
		       "{\"node_id\":\"10.0.0.1\",\"group_id\":10,"
		       "\"cap_flags\":0,\"policy_tags\":0},"
		       "{\"node_id\":\"10.0.0.2\",\"group_id\":20,"
		       "\"cap_flags\":0,\"policy_tags\":0}],"
		       "\"links\":["
		       "{\"local\":\"10.0.0.1\",\"remote\":\"10.0.0.2\","
		       "\"link_id\":1,\"local_address\":\"198.51.100.1\","
		       "\"remote_address\":\"198.51.100.2\",\"cost\":%u,"
		       "\"available_bandwidth_kbps\":100000,"
		       "\"local_ifindex\":9,\"policy_tags\":0},"
		       "{\"local\":\"10.0.0.2\",\"remote\":\"10.0.0.1\","
		       "\"link_id\":2,\"local_address\":\"198.51.100.2\","
		       "\"remote_address\":\"198.51.100.1\",\"cost\":%u,"
		       "\"available_bandwidth_kbps\":90000,"
		       "\"local_ifindex\":0,\"policy_tags\":0}],"
		       "\"node_prefixes\":["
		       "{\"prefix\":\"203.0.113.0/24\","
		       "\"node_id\":\"10.0.0.2\"}],"
		       "\"prefix_groups\":["
		       "{\"prefix\":\"198.18.0.0/15\",\"group_id\":20}]"
		       "}",
		       local_cost, remote_cost) > 0);
	assert(fclose(file) == 0);
	assert(midr_ted_mock_provider_publish_file(ctx, path, error, sizeof(error)) == 0);
	assert(unlink(path) == 0);
	assert(midr_ted_snapshot_get(ctx, &mock) == 0);
	assert(mock->generation == real->generation);
	assert(mock->node_count == real->node_count);
	assert(mock->intra_link_count == real->intra_link_count);
	assert(mock->egress_link_count == real->egress_link_count);
	assert(mock->group_edge_count == real->group_edge_count);
	assert(mock->node_prefix_count == real->node_prefix_count);
	assert(mock->prefix_group_count == real->prefix_group_count);
	assert(mock->egress_links[0].canonical_cost == real->egress_links[0].canonical_cost);
	midr_ted_snapshot_release(&mock);
}

static void test_initial_state_and_api_validation(void)
{
	struct midr_remote_view_snapshot remote = {};
	struct midr_lsdb_summary summary;
	struct midr_ted_status status;
	struct bgp empty_bgp = {};
	struct midr_context no_ted = {
		.bgp = &empty_bgp,
	};
	struct midr_context standalone = {
		.bgp = &empty_bgp,
		.ted_store = (void *)1,
	};

	assert(midr_lsdb_init(NULL) == -EINVAL);
	assert(midr_lsdb_init(&(struct midr_context){}) == -EINVAL);
	assert(midr_lsdb_init(&no_ted) == -EINVAL);
	assert(midr_lsdb_init(&standalone) == 0);
	assert(midr_lsdb_init(&standalone) == -EALREADY);
	assert(midr_lsdb_summary_get(&standalone, &summary) == 0);
	assert(summary.object_count == 0);
	assert(midr_lsdb_test_process(&standalone) == 0);
	midr_lsdb_finish(&standalone);
	midr_lsdb_finish(&standalone);
	assert(midr_lsdb_init(ctx) == -EALREADY);
	midr_lsdb_finish(NULL);
	midr_lsdb_route_changed(NULL, NULL, NULL, NULL);
	midr_lsdb_route_changed(&standalone, NULL, NULL, NULL);
	midr_lsdb_route_changed(ctx, NULL, NULL, NULL);
	midr_lsdb_local_metadata_changed(NULL);
	midr_lsdb_local_metadata_changed(&standalone);
	midr_lsdb_input_state_changed(NULL);
	midr_lsdb_input_state_changed(&standalone);
	assert(midr_lsdb_summary_get(NULL, &summary) == -ENOENT);
	assert(midr_lsdb_summary_get(ctx, NULL) == -EINVAL);
	assert(midr_lsdb_summary_get(ctx, &summary) == 0);
	assert(summary.generation == 0);
	assert(summary.object_count == 0);
	assert(midr_lsdb_remote_snapshot_get(NULL, &remote) == -ENOENT);
	assert(midr_lsdb_remote_snapshot_get(&standalone, &remote) == -ENOENT);
	assert(midr_lsdb_remote_snapshot_get(ctx, NULL) == -EINVAL);
	assert(midr_lsdb_remote_snapshot_get(ctx, &remote) == -EAGAIN);
	assert(remote.node_count == 0);
	assert(remote.link_count == 0);
	assert(midr_lsdb_test_process(NULL) == -ENOENT);
	midr_lsdb_test_fail_next_prepare(NULL);
	midr_lsdb_remote_snapshot_release(NULL);
	midr_lsdb_remote_snapshot_release(&remote);
	midr_lsdb_remote_snapshot_release(&remote);

	assert(midr_lsdb_test_process(ctx) == 0);
	assert(midr_lsdb_test_process(ctx) == 0);
	assert(midr_lsdb_summary_get(ctx, &summary) == 0);
	assert(summary.object_count == 0);
	assert(summary.commit_count == 1);
	assert(midr_ted_status_get(ctx, &status) == 0);
	assert(!status.ready);
	assert(status.generation == 1);
	assert(midr_lsdb_remote_snapshot_get(ctx, &remote) == -EAGAIN);
}

static void test_pending_activation_and_four_objects(void)
{
	const struct midr_remote_view_callbacks empty_callbacks = {};
	const struct midr_ted_snapshot *held = NULL;
	const struct midr_ted_snapshot *snapshot = NULL;
	struct midr_remote_view_snapshot remote = {};
	struct midr_ls_object membership = remote_membership(1, 20);
	struct midr_ls_object link = remote_link(1);
	struct midr_ls_object node_prefix = remote_node_prefix(1);
	struct midr_ls_object group_prefix = remote_group_prefix(1, 20);
	struct midr_lsdb_summary lsdb;
	struct midr_ted_status status;
	struct midr_node_update node = local_node(1, 10);
	struct midr_link_update local = local_link(1);
	uint64_t held_generation;
	uint64_t ready_generation;

	assert(midr_topology_node_upsert(ctx, &node) == 0);
	assert(midr_topology_link_upsert(ctx, &local) == 0);
	midr_topology_process_pending(ctx);
	assert(midr_lsdb_test_process(ctx) == 0);
	assert(midr_lsdb_summary_get(ctx, &lsdb) == 0);
	assert(lsdb.object_count == 2);
	assert(lsdb.usable_count == 1);
	assert(lsdb.pending_count == 1);
	assert(midr_ted_snapshot_get(ctx, &snapshot) == 0);
	assert(snapshot->node_count == 1);
	assert(snapshot->egress_link_count == 0);
	midr_ted_snapshot_release(&snapshot);

	install_remote(&membership);
	install_remote(&link);
	install_remote(&node_prefix);
	install_remote(&group_prefix);
	assert(midr_lsdb_test_process(ctx) == 0);
	assert(midr_lsdb_summary_get(ctx, &lsdb) == 0);
	assert(lsdb.object_count == 6);
	assert(lsdb.usable_count == 6);
	assert(lsdb.pending_count == 0);
	assert(lsdb.membership_count == 2);
	assert(lsdb.link_count == 2);
	assert(lsdb.node_prefix_count == 1);
	assert(lsdb.group_prefix_count == 1);

	assert(midr_ted_snapshot_get(ctx, &held) == 0);
	assert(held->node_count == 1);
	assert(held->egress_link_count == 1);
	assert(held->group_edge_count == 2);
	assert(held->prefix_group_count == 1);
	assert(held->egress_links[0].local_ifindex == 9);
	ready_generation = held->generation;
	held_generation = held->generation;
	assert_mock_matches_real(held, &link);

	assert(midr_remote_view_snapshot_get(ctx, &remote) == 0);
	assert(remote.node_count == 1);
	assert(remote.link_count == 1);
	assert(remote.nodes[0].group_id == 20);
	midr_remote_view_snapshot_release(ctx, &remote);
	assert(callbacks.node_updates == 1);
	assert(callbacks.link_updates == 1);

	assert(midr_lsdb_summary_get(ctx, &lsdb) == 0);
	ready_generation = lsdb.generation;
	midr_lsdb_local_metadata_changed(ctx);
	assert(midr_lsdb_test_process(ctx) == 0);
	assert(midr_lsdb_summary_get(ctx, &lsdb) == 0);
	assert(lsdb.generation == ready_generation);
	assert(callbacks.node_updates == 1);
	assert(callbacks.link_updates == 1);

	local.version = 2;
	local.local_ifindex = 19;
	assert(midr_topology_link_upsert(ctx, &local) == 0);
	midr_topology_process_pending(ctx);
	assert(midr_lsdb_test_process(ctx) == 0);
	assert(midr_lsdb_summary_get(ctx, &lsdb) == 0);
	assert(lsdb.generation > ready_generation);
	ready_generation = lsdb.generation;
	assert(midr_ted_snapshot_get(ctx, &snapshot) == 0);
	assert(snapshot->egress_links[0].local_ifindex == 19);
	midr_ted_snapshot_release(&snapshot);
	assert(held->egress_links[0].local_ifindex == 9);

	membership = remote_membership(2, 30);
	install_remote(&membership);
	midr_lsdb_test_fail_next_prepare(ctx);
	assert(midr_lsdb_test_process(ctx) == -ENOMEM);
	assert(midr_ted_status_get(ctx, &status) == 0);
	assert(status.generation == ready_generation);
	assert(midr_remote_view_snapshot_get(ctx, &remote) == 0);
	assert(remote.nodes[0].group_id == 20);
	midr_remote_view_snapshot_release(ctx, &remote);

	assert(midr_lsdb_test_process(ctx) == 0);
	assert(midr_remote_view_snapshot_get(ctx, &remote) == 0);
	assert(remote.nodes[0].group_id == 30);
	midr_remote_view_snapshot_release(ctx, &remote);
	assert(callbacks.node_updates == 2);

	ctx->midr->remote_callbacks_registered = false;
	membership = remote_membership(3, 31);
	install_remote(&membership);
	assert(midr_lsdb_test_process(ctx) == 0);
	assert(callbacks.node_updates == 2);
	ctx->midr->remote_callbacks_registered = true;
	membership = remote_membership(4, 32);
	install_remote(&membership);
	assert(midr_lsdb_test_process(ctx) == 0);
	assert(callbacks.node_updates == 3);

	assert(midr_remote_view_callbacks_register(ctx, &empty_callbacks) == 0);
	membership = remote_membership(5, 33);
	install_remote(&membership);
	assert(midr_lsdb_test_process(ctx) == 0);
	assert(midr_rib_path_withdraw(ctx, remote_peer, &membership.key) == 0);
	assert(midr_lsdb_test_process(ctx) == 0);
	assert(callbacks.node_updates == 3);
	assert(callbacks.node_withdraws == 0);
	assert(callbacks.link_withdraws == 0);
	assert(midr_remote_view_callbacks_register(ctx,
						   &(const struct midr_remote_view_callbacks){
							   .remote_node_update = remote_node_update,
							   .remote_node_withdraw =
								   remote_node_withdraw,
							   .remote_link_update = remote_link_update,
							   .remote_link_withdraw =
								   remote_link_withdraw,
						   }) == 0);
	membership = remote_membership(6, 34);
	install_remote(&membership);
	assert(midr_lsdb_test_process(ctx) == 0);
	assert(callbacks.node_updates == 4);
	assert(callbacks.link_updates == 2);

	assert(midr_rib_path_withdraw(ctx, remote_peer, &membership.key) == 0);
	assert(midr_lsdb_test_process(ctx) == 0);
	assert(midr_lsdb_summary_get(ctx, &lsdb) == 0);
	assert(lsdb.usable_count == 1);
	assert(lsdb.pending_count == 4);
	assert(midr_remote_view_snapshot_get(ctx, &remote) == 0);
	assert(remote.node_count == 0);
	assert(remote.link_count == 0);
	midr_remote_view_snapshot_release(ctx, &remote);
	assert(callbacks.node_withdraws == 1);
	assert(callbacks.link_withdraws == 1);

	node = local_node(2, 0);
	assert(midr_topology_node_upsert(ctx, &node) == 0);
	midr_topology_process_pending(ctx);
	assert(midr_lsdb_test_process(ctx) == 0);
	assert(midr_ted_status_get(ctx, &status) == 0);
	assert(!status.ready);
	assert(status.generation > ready_generation);
	assert(midr_ted_snapshot_get(ctx, &snapshot) == -EAGAIN);
	assert(held->generation == held_generation);
	assert(held->egress_link_count == 1);
	midr_ted_snapshot_release(&held);
}

static void test_out_of_sync_reason(void)
{
	const struct midr_ted_snapshot *snapshot = NULL;
	struct midr_node_update node = local_node(10, 10);
	struct midr_link_update link = local_link(10);
	struct midr_ted_status status;

	assert(midr_topology_node_upsert(ctx, &node) == 0);
	assert(midr_topology_link_upsert(ctx, &link) == 0);
	midr_topology_process_pending(ctx);
	assert(midr_lsdb_test_process(ctx) == 0);
	assert(midr_ted_snapshot_get(ctx, &snapshot) == 0);
	midr_ted_snapshot_release(&snapshot);

	assert(midr_input_test_set_queue_limits(ctx, 1, 1) == 0);
	node.version = 11;
	assert(midr_topology_node_upsert(ctx, &node) == 0);
	link.version = 11;
	assert(midr_topology_link_upsert(ctx, &link) == -ENOSPC);
	assert(midr_lsdb_test_process(ctx) == 0);
	assert(midr_ted_status_get(ctx, &status) == 0);
	assert(status.ready);
	assert(status.sync_reason_flags == MIDR_TED_SYNC_REASON_RESYNC_FAILED);
}

int main(void)
{
	const struct midr_remote_view_callbacks remote_callbacks = {
		.remote_node_update = remote_node_update,
		.remote_node_withdraw = remote_node_withdraw,
		.remote_link_update = remote_link_update,
		.remote_link_withdraw = remote_link_withdraw,
	};
	as_t asn = 65000;

	qobj_init();
	cmd_init(0);
	bgp_vty_init();
	master = event_master_create("test MIDR LSDB");
	bgp_master_init(master, BGP_SOCKET_SNDBUF_SIZE, list_new());
	vrf_init(NULL, NULL, NULL, NULL);
	bgp_option_set(BGP_OPT_NO_LISTEN);
	bgp_attr_init();
	assert(bgp_get(&bgp, &asn, NULL, BGP_INSTANCE_TYPE_DEFAULT, NULL, ASNOTATION_PLAIN) >= 0);
	ctx = &bgp->midr_info->ctx;
	assert(midr_owned_test_set_sequence_store(ctx, &sequence_ops, &sequence_store) == 0);
	bgp->router_id.s_addr = router_id("10.0.0.1");
	assert(midr_input_router_id_update(bgp, false) == 0);
	midr_input_test_resync_now(ctx);
	remote_peer = peer_create_accept(bgp, NULL);
	assert(remote_peer);
	remote_peer->remote_id.s_addr = router_id("10.0.0.2");
	assert(midr_remote_view_callbacks_register(ctx, &remote_callbacks) == 0);

	test_initial_state_and_api_validation();
	test_pending_activation_and_four_objects();
	test_out_of_sync_reason();
	midr_lsdb_finish(ctx);
	assert(midr_lsdb_summary_get(ctx, &(struct midr_lsdb_summary){}) == -ENOENT);
	puts("MIDR LSDB and production TED tests passed");
	return 0;
}
