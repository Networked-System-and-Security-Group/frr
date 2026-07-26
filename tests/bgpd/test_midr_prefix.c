// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR local prefix contributor tests.
 */

#include <zebra.h>

#include <assert.h>
#include <stdio.h>

#include "command.h"
#include "privs.h"
#include "qobj.h"
#include "routemap.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_attr.h"
#include "bgpd/bgp_midr_owned.h"
#include "bgpd/bgp_midr_prefix.h"
#include "bgpd/bgp_midr_private.h"
#include "bgpd/bgp_midr_rib.h"
#include "bgpd/bgp_midr_lsdb.h"
#include "bgpd/bgp_midr_ted_private.h"
#include "bgpd/bgp_network.h"
#include "bgpd/bgp_table.h"
#include "bgpd/bgp_vty.h"

struct zebra_privs_t bgpd_privs = {};
struct event_loop *master;

static struct bgp *bgp;
static struct midr_context *ctx;

struct memory_sequence_store {
	uint32_t epoch;
	bool found;
};

static struct memory_sequence_store sequence_store;

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

static uint32_t router_id(const char *text)
{
	struct in_addr address;

	assert(inet_pton(AF_INET, text, &address) == 1);
	return address.s_addr;
}

static struct prefix text_prefix(const char *text)
{
	struct prefix prefix;

	assert(str2prefix(text, &prefix) > 0);
	apply_mask(&prefix);
	return prefix;
}

static struct bgp_dest *prefix_dest(const struct prefix *prefix)
{
	afi_t afi = family2afi(prefix->family);

	assert(afi == AFI_IP || afi == AFI_IP6);
	return bgp_node_get(bgp->rib[afi][SAFI_UNICAST], prefix);
}

static struct bgp_path_info path_info(uint8_t type, uint8_t subtype, uint32_t flags)
{
	static struct attr attr;

	return (struct bgp_path_info){
		.peer = bgp->peer_self,
		.attr = &attr,
		.type = type,
		.sub_type = subtype,
		.flags = flags,
	};
}

static void install_path(struct bgp_dest *dest, struct bgp_path_info *path)
{
	path->net = dest;
	bgp_dest_set_bgp_path_info(dest, path);
}

static void remove_paths(struct bgp_dest *dest)
{
	bgp_dest_set_bgp_path_info(dest, NULL);
	bgp_dest_unlock_node(dest);
}

static size_t contributor_count(void)
{
	struct midr_prefix_status status;

	assert(midr_prefix_status_get(ctx, &status) == 0);
	assert(status.state == MIDR_PREFIX_READY);
	return status.contributor_count;
}

static uint64_t contributor_generation(void)
{
	struct midr_prefix_status status;

	assert(midr_prefix_status_get(ctx, &status) == 0);
	return status.generation;
}

static void create_route_map(const char *name, enum route_map_type type)
{
	struct route_map *route_map = route_map_get(name);

	assert(route_map);
	assert(route_map_index_get(route_map, type, 10));
}

static struct midr_ls_object_key node_prefix_key(const struct prefix *prefix)
{
	struct midr_ls_object_key key = {
		.type = MIDR_NLRI_TYPE_NODE_PREFIX,
		.originator_node_id = bgp->router_id.s_addr,
		.u.node_prefix =
			{
				.afi = family2afi(prefix->family),
				.safi = SAFI_UNICAST,
			},
	};

	prefix_copy(&key.u.node_prefix.prefix, prefix);
	apply_mask(&key.u.node_prefix.prefix);
	return key;
}

static struct midr_ls_object_key group_prefix_key(const struct prefix *prefix, uint32_t group_id)
{
	struct midr_ls_object_key key = {
		.type = MIDR_NLRI_TYPE_GROUP_PREFIX,
		.originator_node_id = bgp->router_id.s_addr,
		.u.group_prefix =
			{
				.group_id = group_id,
				.prefix =
					{
						.afi =
							family2afi(
								prefix
									->family),
						.safi = SAFI_UNICAST,
					},
			},
	};

	prefix_copy(&key.u.group_prefix.prefix.prefix, prefix);
	apply_mask(&key.u.group_prefix.prefix.prefix);
	return key;
}

static uint64_t node_prefix_sequence(const struct prefix *prefix)
{
	const struct midr_propagation_path *path;
	struct midr_ls_object_key key = node_prefix_key(prefix);
	struct midr_ls_object object;
	struct peer *peer;

	assert(midr_rib_selected_get(ctx, &key, &object, &path, &peer) == 0);
	assert(peer == bgp->peer_self);
	assert(path->node_count == 1);
	return object.ls_sequence;
}

static void assert_node_prefix_missing(const struct prefix *prefix)
{
	const struct midr_propagation_path *path;
	struct midr_ls_object_key key = node_prefix_key(prefix);
	struct midr_ls_object object;
	struct peer *peer;

	assert(midr_rib_selected_get(ctx, &key, &object, &path, &peer) == -ENOENT);
}

static uint64_t group_prefix_sequence(const struct prefix *prefix, uint32_t group_id)
{
	const struct midr_propagation_path *path;
	struct midr_ls_object_key key = group_prefix_key(prefix, group_id);
	struct midr_ls_object object;
	struct peer *peer;

	assert(midr_rib_selected_get(ctx, &key, &object, &path, &peer) == 0);
	assert(peer == bgp->peer_self);
	return object.ls_sequence;
}

static void assert_group_prefix_missing(const struct prefix *prefix, uint32_t group_id)
{
	const struct midr_propagation_path *path;
	struct midr_ls_object_key key = group_prefix_key(prefix, group_id);
	struct midr_ls_object object;
	struct peer *peer;

	assert(midr_rib_selected_get(ctx, &key, &object, &path, &peer) == -ENOENT);
}

static void install_remote_object(struct peer *peer, const struct midr_ls_object *object)
{
	struct midr_propagation_path path = {};

	assert(midr_propagation_path_init(&path, peer->remote_id.s_addr) == 0);
	assert(midr_rib_path_upsert(ctx, peer, object, &path) == 0);
	midr_propagation_path_fini(&path);
}

static void install_remote_membership(struct peer *peer, uint32_t group_id)
{
	struct midr_ls_object object = {
		.key =
			{
				.type = MIDR_NLRI_TYPE_MEMBERSHIP,
				.originator_node_id = peer->remote_id.s_addr,
			},
		.ls_sequence = 1,
		.payload.membership =
			{
				.group_id = group_id,
		},
	};

	install_remote_object(peer, &object);
}

static struct midr_ls_object remote_node_prefix(struct peer *peer, const struct prefix *prefix)
{
	struct midr_ls_object object = {
		.key =
			{
				.type = MIDR_NLRI_TYPE_NODE_PREFIX,
				.originator_node_id = peer->remote_id.s_addr,
				.u.node_prefix =
					{
						.afi =
							family2afi(
								prefix
									->family),
						.safi = SAFI_UNICAST,
					},
			},
		.ls_sequence = 1,
	};

	prefix_copy(&object.key.u.node_prefix.prefix, prefix);
	apply_mask(&object.key.u.node_prefix.prefix);
	return object;
}

static void test_default_deny_and_network(void)
{
	struct prefix prefix = text_prefix("192.0.2.0/24");
	struct bgp_dest *dest = prefix_dest(&prefix);
	struct bgp_path_info path = path_info(ZEBRA_ROUTE_BGP, BGP_ROUTE_STATIC,
					      BGP_PATH_VALID | BGP_PATH_SELECTED);
	uint64_t generation;
	uint64_t group_sequence;
	uint64_t sequence;
	struct midr_owned_summary owned;
	struct midr_ls_object remote_prefix;
	struct peer *lower_peer;
	struct peer *higher_peer;

	install_path(dest, &path);
	assert(midr_prefix_local_source_set(ctx, AFI_IP, MIDR_PREFIX_SOURCE_NETWORK, true) == 0);
	assert(contributor_count() == 0);

	assert(midr_prefix_route_map_set(ctx, AFI_IP, "MISSING") == 0);
	assert(contributor_count() == 0);

	create_route_map("PERMIT-V4", RMAP_PERMIT);
	assert(midr_prefix_route_map_set(ctx, AFI_IP, "PERMIT-V4") == 0);
	assert(contributor_count() == 1);
	generation = contributor_generation();
	sequence = node_prefix_sequence(&prefix);
	assert(midr_lsdb_test_process(ctx) == 0);
	assert(midr_lsdb_test_process(ctx) == 0);
	group_sequence = group_prefix_sequence(&prefix, 10);

	lower_peer = peer_create_accept(bgp, NULL);
	assert(lower_peer);
	lower_peer->remote_id.s_addr = router_id("9.0.0.1");
	install_remote_membership(lower_peer, 10);
	assert(midr_lsdb_test_process(ctx) == 0);
	assert(midr_lsdb_test_process(ctx) == 0);
	assert_group_prefix_missing(&prefix, 10);

	assert(midr_owned_takeover_delay_set(ctx, 3000) == 0);
	assert(midr_rib_path_withdraw(ctx, lower_peer,
				      &(const struct midr_ls_object_key){
					      .type = MIDR_NLRI_TYPE_MEMBERSHIP,
					      .originator_node_id = lower_peer->remote_id.s_addr,
				      }) == 0);
	assert(midr_lsdb_test_process(ctx) == 0);
	assert_group_prefix_missing(&prefix, 10);
	assert(midr_owned_summary_get(ctx, &owned) == 0);
	assert(owned.takeover_timer_pending);
	midr_owned_test_fire_takeover(ctx);
	assert(midr_lsdb_test_process(ctx) == 0);
	group_sequence = group_prefix_sequence(&prefix, 10);
	assert(midr_owned_takeover_delay_set(ctx, 0) == 0);

	higher_peer = peer_create_accept(bgp, NULL);
	assert(higher_peer);
	higher_peer->remote_id.s_addr = router_id("10.0.0.9");
	install_remote_membership(higher_peer, 10);
	remote_prefix = remote_node_prefix(higher_peer, &prefix);
	install_remote_object(higher_peer, &remote_prefix);
	assert(midr_lsdb_test_process(ctx) == 0);
	assert(group_prefix_sequence(&prefix, 10) == group_sequence);

	path.flags |= BGP_PATH_MULTIPATH;
	midr_prefix_route_changed(ctx, AFI_IP, SAFI_UNICAST, dest, &path, &path);
	assert(contributor_count() == 1);
	assert(contributor_generation() == generation);
	assert(node_prefix_sequence(&prefix) == sequence);
	assert(group_prefix_sequence(&prefix, 10) == group_sequence);
	midr_owned_reconcile(ctx);
	assert(node_prefix_sequence(&prefix) == sequence);

	path.flags |= BGP_PATH_STALE;
	midr_prefix_route_changed(ctx, AFI_IP, SAFI_UNICAST, dest, &path, &path);
	assert(contributor_count() == 0);
	assert(contributor_generation() == generation + 1);
	assert_node_prefix_missing(&prefix);
	assert(midr_lsdb_test_process(ctx) == 0);
	assert(group_prefix_sequence(&prefix, 10) == group_sequence);
	assert(midr_rib_path_withdraw(ctx, higher_peer, &remote_prefix.key) == 0);
	assert(midr_lsdb_test_process(ctx) == 0);
	assert(midr_lsdb_test_process(ctx) == 0);
	assert_group_prefix_missing(&prefix, 10);

	path.flags &= ~BGP_PATH_STALE;
	path.flags &= ~BGP_PATH_VALID;
	midr_prefix_route_changed(ctx, AFI_IP, SAFI_UNICAST, dest, &path, &path);
	assert(contributor_count() == 0);

	remove_paths(dest);
}

static void test_connected_static_and_midr_feedback(void)
{
	struct prefix connected_prefix = text_prefix("198.51.100.0/24");
	struct prefix static_prefix = text_prefix("203.0.113.0/24");
	struct bgp_dest *connected_dest = prefix_dest(&connected_prefix);
	struct bgp_dest *static_dest = prefix_dest(&static_prefix);
	struct bgp_path_info connected = path_info(ZEBRA_ROUTE_CONNECT, BGP_ROUTE_REDISTRIBUTE,
						   BGP_PATH_VALID | BGP_PATH_SELECTED);
	struct bgp_path_info static_route = path_info(ZEBRA_ROUTE_STATIC, BGP_ROUTE_REDISTRIBUTE,
						      BGP_PATH_VALID | BGP_PATH_SELECTED);
	struct midr_prefix_status status;

	install_path(connected_dest, &connected);
	install_path(static_dest, &static_route);
	assert(midr_prefix_local_source_set(ctx, AFI_IP, MIDR_PREFIX_SOURCE_NETWORK, false) == 0);
	assert(midr_prefix_local_source_set(ctx, AFI_IP, MIDR_PREFIX_SOURCE_CONNECTED, true) == 0);
	assert(contributor_count() == 1);
	assert(midr_prefix_local_source_set(ctx, AFI_IP, MIDR_PREFIX_SOURCE_STATIC, true) == 0);
	assert(contributor_count() == 2);

	static_route.type = ZEBRA_ROUTE_MIDR;
	midr_prefix_route_changed(ctx, AFI_IP, SAFI_UNICAST, static_dest, &static_route,
				  &static_route);
	assert(contributor_count() == 1);
	assert(midr_prefix_status_get(ctx, &status) == 0);
	assert(status.rejected_midr > 0);

	create_route_map("DENY-V4", RMAP_DENY);
	assert(midr_prefix_route_map_set(ctx, AFI_IP, "DENY-V4") == 0);
	assert(contributor_count() == 0);

	remove_paths(connected_dest);
	remove_paths(static_dest);
}

static void test_ipv6_and_default_route(void)
{
	struct prefix ipv6 = text_prefix("2001:db8:100::/48");
	struct prefix default_route = text_prefix("0.0.0.0/0");
	struct bgp_dest *ipv6_dest = prefix_dest(&ipv6);
	struct bgp_dest *default_dest = prefix_dest(&default_route);
	struct bgp_path_info ipv6_path = path_info(ZEBRA_ROUTE_BGP, BGP_ROUTE_STATIC,
						   BGP_PATH_VALID | BGP_PATH_SELECTED);
	struct bgp_path_info default_path = path_info(ZEBRA_ROUTE_BGP, BGP_ROUTE_STATIC,
						      BGP_PATH_VALID | BGP_PATH_SELECTED);

	install_path(ipv6_dest, &ipv6_path);
	install_path(default_dest, &default_path);
	create_route_map("PERMIT-V6", RMAP_PERMIT);
	assert(midr_prefix_route_map_set(ctx, AFI_IP6, "PERMIT-V6") == 0);
	assert(midr_prefix_local_source_set(ctx, AFI_IP6, MIDR_PREFIX_SOURCE_NETWORK, true) == 0);
	assert(contributor_count() == 1);

	assert(midr_prefix_route_map_set(ctx, AFI_IP, "PERMIT-V4") == 0);
	assert(midr_prefix_local_source_set(ctx, AFI_IP, MIDR_PREFIX_SOURCE_NETWORK, true) == 0);
	assert(contributor_count() == 2);

	remove_paths(ipv6_dest);
	remove_paths(default_dest);
}

static void test_external_peer_source(void)
{
	struct prefix prefix = text_prefix("172.16.0.0/12");
	struct bgp_dest *dest = prefix_dest(&prefix);
	struct bgp_path_info path = path_info(ZEBRA_ROUTE_BGP, BGP_ROUTE_NORMAL,
					      BGP_PATH_VALID | BGP_PATH_SELECTED);
	struct peer *peer = peer_create_accept(bgp, NULL);

	assert(peer);
	peer->remote_id.s_addr = router_id("10.0.0.9");
	path.peer = peer;
	install_path(dest, &path);
	assert(midr_prefix_route_map_set(ctx, AFI_IP, "PERMIT-V4") == 0);
	assert(midr_prefix_local_source_set(ctx, AFI_IP, MIDR_PREFIX_SOURCE_NETWORK, false) == 0);
	assert(midr_prefix_local_source_set(ctx, AFI_IP, MIDR_PREFIX_SOURCE_CONNECTED, false) == 0);
	assert(midr_prefix_local_source_set(ctx, AFI_IP, MIDR_PREFIX_SOURCE_STATIC, false) == 0);
	assert(contributor_count() == 0);
	assert(midr_prefix_external_peer_set(ctx, peer, AFI_IP, true) == 0);
	assert(contributor_count() == 1);
	assert(midr_prefix_external_peer_set(ctx, peer, AFI_IP, false) == 0);
	assert(contributor_count() == 0);
	remove_paths(dest);
}

static void test_validation_and_lifecycle(void)
{
	struct midr_prefix_status status;

	assert(midr_prefix_route_map_set(NULL, AFI_IP, "X") == -ENOENT);
	assert(midr_prefix_route_map_set(ctx, AFI_UNSPEC, "X") == -EINVAL);
	assert(midr_prefix_route_map_set(ctx, AFI_IP, "") == -EINVAL);
	assert(midr_prefix_local_source_set(ctx, AFI_IP, 0, true) == -EINVAL);
	assert(midr_prefix_local_source_set(ctx, AFI_IP,
					    MIDR_PREFIX_SOURCE_NETWORK |
						    MIDR_PREFIX_SOURCE_CONNECTED,
					    true) == -EINVAL);
	assert(midr_prefix_status_get(ctx, NULL) == -EINVAL);
	assert(midr_prefix_init(ctx) == -EALREADY);
	assert(midr_prefix_rescan(ctx) == 0);
	assert(midr_prefix_status_get(ctx, &status) == 0);
	assert(status.scans > 0);
}

static void test_out_of_sync_and_recovery(void)
{
	struct prefix prefix = text_prefix("100.64.0.0/10");
	struct bgp_dest *dest = prefix_dest(&prefix);
	struct bgp_path_info path = path_info(ZEBRA_ROUTE_BGP, BGP_ROUTE_STATIC,
					      BGP_PATH_VALID | BGP_PATH_SELECTED);
	struct midr_prefix_status prefix_status;
	struct midr_ted_status ted_status;

	install_path(dest, &path);
	assert(midr_prefix_route_map_set(ctx, AFI_IP, "PERMIT-V4") == 0);
	assert(midr_prefix_local_source_set(ctx, AFI_IP, MIDR_PREFIX_SOURCE_NETWORK, true) == 0);
	assert(node_prefix_sequence(&prefix) > 0);
	assert(midr_lsdb_test_process(ctx) == 0);
	assert(midr_lsdb_test_process(ctx) == 0);
	assert(group_prefix_sequence(&prefix, 10) > 0);

	midr_prefix_mark_out_of_sync(ctx);
	assert_node_prefix_missing(&prefix);
	assert_group_prefix_missing(&prefix, 10);
	assert(midr_lsdb_test_process(ctx) == 0);
	assert(midr_ted_status_get(ctx, &ted_status) == 0);
	assert(!ted_status.ready);
	assert(ted_status.sync_reason_flags & MIDR_TED_SYNC_REASON_RESYNC_FAILED);

	assert(midr_prefix_rescan(ctx) == 0);
	assert(midr_prefix_status_get(ctx, &prefix_status) == 0);
	assert(prefix_status.state == MIDR_PREFIX_READY);
	assert(node_prefix_sequence(&prefix) > 0);
	assert(midr_lsdb_test_process(ctx) == 0);
	assert(midr_lsdb_test_process(ctx) == 0);
	assert(group_prefix_sequence(&prefix, 10) > 0);
	assert(midr_ted_status_get(ctx, &ted_status) == 0);
	assert(ted_status.ready);

	remove_paths(dest);
	assert(midr_prefix_rescan(ctx) == 0);
}

int main(void)
{
	as_t asn = 65000;

	qobj_init();
	cmd_init(0);
	bgp_vty_init();
	master = event_master_create("test MIDR prefix contributors");
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
	bgp_route_map_init();
	assert(midr_owned_takeover_delay_set(ctx, 0) == 0);
	assert(midr_topology_node_upsert(ctx, &(const struct midr_node_update){
						      .node_id = bgp->router_id.s_addr,
						      .group_id = 10,
						      .policy_state = MIDR_POLICY_ALLOWED,
						      .version = 1,
					      }) == 0);
	midr_topology_process_pending(ctx);
	assert(midr_lsdb_test_process(ctx) == 0);

	test_default_deny_and_network();
	test_connected_static_and_midr_feedback();
	test_ipv6_and_default_route();
	test_external_peer_source();
	test_validation_and_lifecycle();
	test_out_of_sync_and_recovery();

	puts("MIDR prefix contributor tests passed");
	return 0;
}
