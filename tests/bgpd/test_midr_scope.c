// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR propagation scope and export eligibility tests.
 */

#include <zebra.h>

#include <assert.h>
#include <stdio.h>

#include "command.h"
#include "privs.h"
#include "qobj.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_attr.h"
#include "bgpd/bgp_midr_lsdb.h"
#include "bgpd/bgp_midr_private.h"
#include "bgpd/bgp_midr_rib.h"
#include "bgpd/bgp_network.h"
#include "bgpd/bgp_vty.h"

struct zebra_privs_t bgpd_privs = {};
struct event_loop *master;

static struct bgp *bgp;
static struct midr_context *ctx;

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

static struct peer *test_peer(const char *id)
{
	struct peer *peer = peer_create_accept(bgp, NULL);

	assert(peer);
	peer->remote_id.s_addr = router_id(id);
	return peer;
}

static struct midr_ls_object membership(uint32_t node_id, uint32_t group_id, uint64_t sequence)
{
	return (struct midr_ls_object){
		.key =
			{
				.type = MIDR_NLRI_TYPE_MEMBERSHIP,
				.originator_node_id = node_id,
			},
		.ls_sequence = sequence,
		.payload.membership =
			{
				.group_id = group_id,
				.has_transport_address = true,
				.transport_address = ip_address("192.0.2.1"),
			},
	};
}

static struct midr_ls_object link_object(uint32_t local_node_id, uint32_t remote_node_id,
					 uint64_t link_id)
{
	return (struct midr_ls_object){
		.key =
			{
				.type = MIDR_NLRI_TYPE_LINK,
				.originator_node_id = local_node_id,
				.u.link =
					{
						.remote_node_id =
							remote_node_id,
						.link_id = link_id,
					},
			},
		.ls_sequence = 1,
		.payload.link =
			{
				.link_local_address =
					ip_address("198.51.100.1"),
				.link_remote_address =
					ip_address("198.51.100.2"),
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

static struct midr_ls_object node_prefix(uint32_t node_id)
{
	return (struct midr_ls_object){
		.key =
			{
				.type = MIDR_NLRI_TYPE_NODE_PREFIX,
				.originator_node_id = node_id,
				.u.node_prefix =
					{
						.afi = AFI_IP,
						.safi = SAFI_UNICAST,
						.prefix =
							ipv4_prefix(
								"203.0.113.0/24"),
					},
			},
		.ls_sequence = 1,
	};
}

static void install_direct(struct peer *peer, const struct midr_ls_object *object)
{
	struct midr_propagation_path path = {};

	assert(midr_propagation_path_init(&path, object->key.originator_node_id) == 0);
	assert(midr_rib_path_upsert(ctx, peer, object, &path) == 0);
	midr_propagation_path_fini(&path);
}

static void install_relayed(struct peer *peer, const struct midr_ls_object *object,
			    uint32_t intermediate)
{
	struct midr_propagation_path path = {};

	assert(midr_propagation_path_init(&path, object->key.originator_node_id) == 0);
	assert(midr_propagation_path_append(&path, intermediate) == 0);
	assert(midr_propagation_path_append(&path, peer->remote_id.s_addr) == 0);
	assert(midr_rib_path_upsert(ctx, peer, object, &path) == 0);
	midr_propagation_path_fini(&path);
}

struct selected_ref {
	const struct midr_ls_object_key *key;
	struct bgp_dest *dest;
	struct bgp_path_info *path;
};

static int find_selected(const struct midr_ls_object *object,
			 const struct midr_propagation_path *path, struct peer *peer,
			 struct bgp_dest *dest, struct bgp_path_info *selected, void *arg)
{
	struct selected_ref *ref = arg;

	(void)path;
	(void)peer;
	if (midr_ls_object_key_same(&object->key, ref->key)) {
		ref->dest = dest;
		ref->path = selected;
	}
	return 0;
}

static struct selected_ref selected_ref(const struct midr_ls_object_key *key)
{
	struct selected_ref ref = {
		.key = key,
	};

	assert(midr_rib_selected_entry_foreach(ctx, find_selected, &ref) == 0);
	assert(ref.dest && ref.path);
	return ref;
}

int main(void)
{
	struct peer *owner_same;
	struct peer *target_same;
	struct peer *target_other;
	struct peer *relay;
	struct midr_ls_object local_membership;
	struct midr_ls_object owner_membership;
	struct midr_ls_object same_membership;
	struct midr_ls_object other_membership;
	struct midr_ls_object same_link;
	struct midr_ls_object cross_link;
	struct midr_ls_object loop_path_link;
	struct midr_ls_object prefix;
	struct selected_ref ref;
	as_t asn = 65000;

	qobj_init();
	cmd_init(0);
	bgp_vty_init();
	master = event_master_create("test MIDR scope");
	bgp_master_init(master, BGP_SOCKET_SNDBUF_SIZE, list_new());
	vrf_init(NULL, NULL, NULL, NULL);
	bgp_option_set(BGP_OPT_NO_LISTEN);
	bgp_attr_init();
	assert(bgp_get(&bgp, &asn, NULL, BGP_INSTANCE_TYPE_DEFAULT, NULL, ASNOTATION_PLAIN) >= 0);
	bgp->router_id.s_addr = router_id("10.0.0.1");
	ctx = &bgp->midr_info->ctx;

	owner_same = test_peer("10.0.0.2");
	target_same = test_peer("10.0.0.3");
	target_other = test_peer("10.0.0.4");
	relay = test_peer("10.0.0.5");

	local_membership = membership(bgp->router_id.s_addr, 10, 1);
	owner_membership = membership(owner_same->remote_id.s_addr, 10, 1);
	same_membership = membership(target_same->remote_id.s_addr, 10, 1);
	other_membership = membership(target_other->remote_id.s_addr, 20, 1);
	install_direct(bgp->peer_self, &local_membership);
	install_direct(owner_same, &owner_membership);
	install_direct(target_same, &same_membership);
	install_direct(target_other, &other_membership);

	same_link = link_object(owner_same->remote_id.s_addr, target_same->remote_id.s_addr, 1);
	cross_link = link_object(owner_same->remote_id.s_addr, target_other->remote_id.s_addr, 2);
	loop_path_link = link_object(owner_same->remote_id.s_addr, target_other->remote_id.s_addr,
				     3);
	prefix = node_prefix(owner_same->remote_id.s_addr);
	install_direct(owner_same, &same_link);
	install_direct(owner_same, &cross_link);
	install_relayed(relay, &loop_path_link, target_same->remote_id.s_addr);
	install_direct(owner_same, &prefix);
	assert(midr_lsdb_test_process(ctx) == 0);

	ref = selected_ref(&same_link.key);
	assert(midr_lsdb_export_eligible(ctx, ref.dest, ref.path, target_same));
	assert(!midr_lsdb_export_eligible(ctx, ref.dest, ref.path, target_other));
	assert(!midr_lsdb_export_eligible(ctx, ref.dest, ref.path, owner_same));

	ref = selected_ref(&cross_link.key);
	assert(midr_lsdb_export_eligible(ctx, ref.dest, ref.path, target_same));
	assert(midr_lsdb_export_eligible(ctx, ref.dest, ref.path, target_other));

	ref = selected_ref(&loop_path_link.key);
	assert(!midr_lsdb_export_eligible(ctx, ref.dest, ref.path, target_same));
	assert(midr_lsdb_export_eligible(ctx, ref.dest, ref.path, target_other));

	ref = selected_ref(&prefix.key);
	assert(midr_lsdb_export_eligible(ctx, ref.dest, ref.path, target_same));
	assert(!midr_lsdb_export_eligible(ctx, ref.dest, ref.path, target_other));

	same_membership = membership(target_same->remote_id.s_addr, 20, 2);
	install_direct(target_same, &same_membership);
	assert(midr_lsdb_test_process(ctx) == 0);
	ref = selected_ref(&prefix.key);
	assert(!midr_lsdb_export_eligible(ctx, ref.dest, ref.path, target_same));

	puts("MIDR propagation scope tests passed");
	return 0;
}
