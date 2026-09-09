// SPDX-License-Identifier: GPL-2.0-or-later
/* MIDR export eligibility with canonical instances. */

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

static struct prefix prefix4(const char *text)
{
	struct prefix prefix;

	assert(str2prefix(text, &prefix) > 0);
	return prefix;
}

static struct peer *test_peer(const char *text)
{
	struct peer *peer = peer_create_accept(bgp, NULL);

	assert(peer);
	peer->remote_id.s_addr = router_id(text);
	return peer;
}

static struct midr_instance membership(uint32_t originator, uint32_t group_id,
					uint64_t sequence)
{
	return (struct midr_instance){
		.state = MIDR_INSTANCE_ACTIVE,
		.object = {
			.key = {
				.type = MIDR_NLRI_TYPE_MEMBERSHIP,
				.originator_node_id = originator,
			},
			.ls_sequence = sequence,
			.payload.membership = {
				.group_id = group_id,
				.has_transport_address = true,
				.transport_address = ip_address("192.0.2.1"),
				.cap_flags = 1,
			},
		},
	};
}

static struct midr_instance node_prefix(uint32_t originator)
{
	return (struct midr_instance){
		.state = MIDR_INSTANCE_ACTIVE,
		.object = {
			.key = {
				.type = MIDR_NLRI_TYPE_NODE_PREFIX,
				.originator_node_id = originator,
				.u.node_prefix = {
					.afi = AFI_IP,
					.safi = SAFI_UNICAST,
					.prefix = prefix4("203.0.113.0/24"),
				},
			},
			.ls_sequence = 1,
		},
	};
}

static struct midr_instance link_instance(uint32_t originator, uint32_t remote_id)
{
	return (struct midr_instance){
		.state = MIDR_INSTANCE_ACTIVE,
		.object = {
			.key = {
				.type = MIDR_NLRI_TYPE_LINK,
				.originator_node_id = originator,
				.u.link = {
					.remote_node_id = remote_id,
					.link_id = 1,
				},
			},
			.ls_sequence = 1,
			.payload.link = {
				.link_local_address = ip_address("198.51.100.1"),
				.link_remote_address = ip_address("198.51.100.2"),
				.canonical_cost = 250,
			},
		},
	};
}

static void install(struct peer *peer, const struct midr_instance *instance)
{
	assert(midr_rib_instance_upsert(ctx, peer, instance, 0) == 0);
}

struct selected_ref {
	const struct midr_ls_object_key *key;
	struct bgp_dest *dest;
	struct bgp_path_info *path;
};

static int find_selected(const struct midr_instance *instance, struct peer *peer,
				 struct bgp_dest *dest, struct bgp_path_info *path,
				 void *arg)
{
	struct selected_ref *ref = arg;

	(void)peer;
	if (midr_ls_object_key_same(&instance->object.key, ref->key)) {
		ref->dest = dest;
		ref->path = path;
	}
	return 0;
}

static struct selected_ref selected(const struct midr_ls_object_key *key)
{
	struct selected_ref ref = {.key = key};

	assert(midr_rib_selected_entry_foreach(ctx, find_selected, &ref) == 0);
	assert(ref.dest && ref.path);
	return ref;
}

static void test_advertisement_relationship(void)
{
	struct peer *same_group = test_peer("10.0.0.2");
	struct peer *other_group = test_peer("10.0.0.3");
	struct peer *relay = test_peer("10.0.0.4");
	struct midr_instance local = membership(bgp->router_id.s_addr, 10, 1);
	struct midr_instance same = membership(same_group->remote_id.s_addr, 10, 1);
	struct midr_instance other = membership(other_group->remote_id.s_addr, 20, 1);
	struct midr_instance link = link_instance(same_group->remote_id.s_addr,
							other_group->remote_id.s_addr);
	struct selected_ref ref;

	install(bgp->peer_self, &local);
	install(same_group, &same);
	install(other_group, &other);
	install(same_group, &link);
	assert(midr_lsdb_test_process(ctx) == 0);

	ref = selected(&link.object.key);
	assert(!midr_lsdb_export_eligible(ctx, ref.dest, ref.path, same_group));
	assert(midr_lsdb_export_eligible(ctx, ref.dest, ref.path, other_group));
	assert(!midr_lsdb_export_eligible(ctx, ref.dest, ref.path, relay));

	/* MP_UNREACH removes only the relationship; canonical remains selected. */
	assert(midr_rib_peer_withdraw(ctx, same_group, &link.object.key) == 0);
	assert(midr_lsdb_test_process(ctx) == 0);
	assert(midr_lsdb_export_eligible(ctx, ref.dest, ref.path, same_group));
}

static void test_scope_gate(void)
{
	struct peer *same_group = test_peer("10.0.0.5");
	struct peer *other_group = test_peer("10.0.0.6");
	struct midr_instance local = membership(bgp->router_id.s_addr, 10, 2);
	struct midr_instance same = membership(same_group->remote_id.s_addr, 10, 2);
	struct midr_instance other = membership(other_group->remote_id.s_addr, 20, 2);
	struct midr_instance prefix = node_prefix(same_group->remote_id.s_addr);
	struct selected_ref ref;

	install(bgp->peer_self, &local);
	install(same_group, &same);
	install(other_group, &other);
	install(same_group, &prefix);
	assert(midr_lsdb_test_process(ctx) == 0);
	ref = selected(&prefix.object.key);
	assert(!midr_lsdb_export_eligible(ctx, ref.dest, ref.path, same_group));
	assert(!midr_lsdb_export_eligible(ctx, ref.dest, ref.path, other_group));
	assert(midr_rib_peer_withdraw(ctx, same_group, &prefix.object.key) == 0);
	assert(midr_lsdb_test_process(ctx) == 0);
	assert(midr_lsdb_export_eligible(ctx, ref.dest, ref.path, same_group));
}

int main(void)
{
	as_t asn = 65000;

	qobj_init();
	cmd_init(0);
	bgp_vty_init();
	master = event_master_create("test MIDR scope");
	bgp_master_init(master, BGP_SOCKET_SNDBUF_SIZE, list_new());
	vrf_init(NULL, NULL, NULL, NULL);
	bgp_option_set(BGP_OPT_NO_LISTEN);
	bgp_attr_init();
	assert(bgp_get(&bgp, &asn, NULL, BGP_INSTANCE_TYPE_DEFAULT, NULL,
		       ASNOTATION_PLAIN) >= 0);
	bgp->router_id.s_addr = router_id("10.0.0.1");
	ctx = &bgp->midr_info->ctx;

	test_advertisement_relationship();
	test_scope_gate();
	puts("MIDR scope tests passed");
	return 0;
}
