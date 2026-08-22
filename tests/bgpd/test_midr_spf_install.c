// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR SPF result to Zebra installation adapter tests
 *
 * Copyright (C) 2026
 */

#include <zebra.h>

#include "lib/frrevent.h"
#include "lib/privs.h"
#include "lib/zclient.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_midr_private.h"
#include "bgpd/bgp_midr_spf.h"
#include "bgpd/bgp_midr_spf_install.h"
#include "bgpd/bgp_midr_zebra.h"

struct zebra_privs_t bgpd_privs = {};
struct event_loop *master;
static struct bgp_master test_bm;
struct bgp_master *bm = &test_bm;
struct zclient *bgp_zclient;

#define SENT_ROUTE_MAX 16

struct sent_route {
	uint8_t command;
	struct zapi_route route;
};

static struct sent_route sent_routes[SENT_ROUTE_MAX];
static size_t sent_route_count;

extern enum zclient_send_status __wrap_zclient_route_send(uint8_t command, struct zclient *zclient,
							  struct zapi_route *route);

enum zclient_send_status __wrap_zclient_route_send(uint8_t command, struct zclient *zclient,
						   struct zapi_route *route)
{
	(void)zclient;
	assert(sent_route_count < array_size(sent_routes));
	sent_routes[sent_route_count].command = command;
	sent_routes[sent_route_count].route = *route;
	sent_route_count++;
	return ZCLIENT_SEND_SUCCESS;
}

static uint32_t node_id(const char *text)
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

static struct midr_ted_prefix_key prefix_key(const char *text)
{
	struct midr_ted_prefix_key key = {
		.safi = SAFI_UNICAST,
	};

	assert(str2prefix(text, &key.prefix) > 0);
	key.afi = key.prefix.family == AF_INET ? AFI_IP : AFI_IP6;
	return key;
}

static const struct midr_spf_results *compute_results(const char *prefix_text,
						      const char *nexthop_text, uint32_t cost,
						      uint32_t bandwidth_kbps, ifindex_t ifindex,
						      bool local_destination)
{
	const uint32_t local = node_id("1.1.1.1");
	const uint32_t remote = node_id("2.2.2.2");
	struct midr_ted_node nodes[] = {
		{ .node_id = local, .group_id = 100 },
		{ .node_id = remote, .group_id = 100 },
	};
	struct midr_ted_link link = {
		.local_node_id = local,
		.remote_node_id = remote,
		.local_group_id = 100,
		.remote_group_id = 100,
		.link_id = 1,
		.canonical_cost = cost,
		.available_bandwidth_kbps = bandwidth_kbps,
		.link_local_address = ip_address(strchr(nexthop_text, ':') ? "2001:db8::1"
									   : "192.0.2.1"),
		.link_remote_address = ip_address(nexthop_text),
		.local_ifindex = ifindex,
	};
	struct midr_ted_node_prefix mapping = {
		.key = prefix_key(prefix_text),
		.node_id = local_destination ? local : remote,
	};
	struct midr_ted_snapshot snapshot = {
		.generation = cost,
		.local_node_id = local,
		.local_group_id = 100,
		.ready = true,
		.nodes = nodes,
		.node_count = array_size(nodes),
		.intra_links = &link,
		.intra_link_count = 1,
		.node_prefixes = &mapping,
		.node_prefix_count = 1,
	};
	const struct midr_spf_results *results = NULL;

	assert(midr_spf_compute_all(&snapshot, &results) == 0);
	assert(results);
	return results;
}

static const struct midr_spf_results *compute_ecmp_results(void)
{
	const uint32_t local = node_id("1.1.1.1");
	const uint32_t left = node_id("2.2.2.2");
	const uint32_t right = node_id("3.3.3.3");
	const uint32_t destination = node_id("4.4.4.4");
	struct midr_ted_node nodes[] = {
		{ .node_id = local, .group_id = 100 },
		{ .node_id = left, .group_id = 100 },
		{ .node_id = right, .group_id = 100 },
		{ .node_id = destination, .group_id = 100 },
	};
	struct midr_ted_link links[] = {
		{
			.local_node_id = local,
			.remote_node_id = left,
			.local_group_id = 100,
			.remote_group_id = 100,
			.link_id = 1,
			.canonical_cost = 2,
			.available_bandwidth_kbps = 64000,
			.link_local_address = ip_address("192.0.2.1"),
			.link_remote_address = ip_address("192.0.2.2"),
			.local_ifindex = 7,
		},
		{
			.local_node_id = local,
			.remote_node_id = right,
			.local_group_id = 100,
			.remote_group_id = 100,
			.link_id = 2,
			.canonical_cost = 2,
			.available_bandwidth_kbps = 32000,
			.link_local_address = ip_address("192.0.2.1"),
			.link_remote_address = ip_address("192.0.2.3"),
			.local_ifindex = 8,
		},
		{
			.local_node_id = left,
			.remote_node_id = destination,
			.local_group_id = 100,
			.remote_group_id = 100,
			.link_id = 3,
			.canonical_cost = MIDR_TED_LINK_COST_MAX,
		},
		{
			.local_node_id = right,
			.remote_node_id = destination,
			.local_group_id = 100,
			.remote_group_id = 100,
			.link_id = 4,
			.canonical_cost = MIDR_TED_LINK_COST_MAX,
		},
	};
	struct midr_ted_node_prefix mapping = {
		.key = prefix_key("198.18.0.0/15"),
		.node_id = destination,
	};
	struct midr_ted_snapshot snapshot = {
		.generation = 100,
		.local_node_id = local,
		.local_group_id = 100,
		.ready = true,
		.nodes = nodes,
		.node_count = array_size(nodes),
		.intra_links = links,
		.intra_link_count = array_size(links),
		.node_prefixes = &mapping,
		.node_prefix_count = 1,
	};
	const struct midr_spf_results *results = NULL;

	assert(midr_spf_compute_all(&snapshot, &results) == 0);
	assert(results);
	return results;
}

static const struct midr_spf_results *
compute_cross_results(uint64_t generation, uint64_t group_score, uint32_t local_cost)
{
	const uint32_t local = node_id("1.1.1.1");
	const uint32_t remote = node_id("2.2.2.2");
	struct midr_ted_node node = {
		.node_id = local,
		.group_id = 100,
	};
	struct midr_ted_link egress = {
		.local_node_id = local,
		.remote_node_id = remote,
		.local_group_id = 100,
		.remote_group_id = 200,
		.link_id = 1,
		.canonical_cost = local_cost,
		.available_bandwidth_kbps = 64000,
		.link_local_address = ip_address("192.0.2.1"),
		.link_remote_address = ip_address("192.0.2.2"),
		.local_ifindex = 7,
	};
	struct midr_ted_group_edge group_edge = {
		.source_group_id = 100,
		.target_group_id = 200,
		.aggregate_cost = group_score,
	};
	struct midr_ted_prefix_group mapping = {
		.key = prefix_key("203.0.113.0/24"),
		.group_id = 200,
	};
	struct midr_ted_snapshot snapshot = {
		.generation = generation,
		.local_node_id = local,
		.local_group_id = 100,
		.ready = true,
		.nodes = &node,
		.node_count = 1,
		.egress_links = &egress,
		.egress_link_count = 1,
		.group_edges = &group_edge,
		.group_edge_count = 1,
		.prefix_groups = &mapping,
		.prefix_group_count = 1,
	};
	const struct midr_spf_results *results = NULL;

	assert(midr_spf_compute_all(&snapshot, &results) == 0);
	assert(results);
	return results;
}

static void reset_sent_routes(void)
{
	memset(sent_routes, 0, sizeof(sent_routes));
	sent_route_count = 0;
}

static void test_spf_install_adapter(void)
{
	const struct midr_spf_results *initial;
	const struct midr_spf_results *same;
	const struct midr_spf_results *changed;
	const struct midr_spf_results *cross_group_changed;
	const struct midr_spf_results *cross_initial;
	const struct midr_spf_results *cross_local_changed;
	const struct midr_spf_results *local;
	const struct midr_spf_results *ecmp;
	const struct midr_spf_results *ipv6;
	const struct midr_spf_results *mixed;
	struct midr_context ctx = {};
	struct bgp bgp = {
		.vrf_id = VRF_DEFAULT,
	};
	struct in_addr expected_v4;
	struct in6_addr expected_v6;

	ctx.bgp = &bgp;
	midr_zebra_init(&bgp);

	initial = compute_results("203.0.113.0/24", "192.0.2.2", 5, 64000, 7, false);
	midr_spf_install_results(&ctx, NULL, initial);
	midr_zebra_route_flush(&bgp);
	assert(sent_route_count == 1);
	assert(sent_routes[0].command == ZEBRA_ROUTE_ADD);
	assert(sent_routes[0].route.type == ZEBRA_ROUTE_MIDR);
	assert(sent_routes[0].route.instance == MIDR_INSTANCE_SPF);
	assert(sent_routes[0].route.metric == 5);
	assert(sent_routes[0].route.nexthop_num == 1);
	assert(sent_routes[0].route.nexthops[0].type == NEXTHOP_TYPE_IPV4_IFINDEX);
	assert(sent_routes[0].route.nexthops[0].ifindex == 7);
	assert(inet_pton(AF_INET, "192.0.2.2", &expected_v4) == 1);
	assert(IPV4_ADDR_SAME(&sent_routes[0].route.nexthops[0].gate.ipv4, &expected_v4));

	same = compute_results("203.0.113.0/24", "192.0.2.2", 5, 64000, 7, false);
	midr_spf_install_results(&ctx, initial, same);
	midr_zebra_route_flush(&bgp);
	assert(sent_route_count == 1);

	changed = compute_results("203.0.113.0/24", "192.0.2.2", 9, 64000, 7, false);
	midr_spf_install_results(&ctx, same, changed);
	midr_zebra_route_flush(&bgp);
	assert(sent_route_count == 3);
	assert(sent_routes[1].command == ZEBRA_ROUTE_DELETE);
	assert(sent_routes[2].command == ZEBRA_ROUTE_ADD);
	assert(sent_routes[2].route.metric == 9);

	local = compute_results("203.0.113.0/24", "192.0.2.2", 9, 64000, 7, true);
	midr_spf_install_results(&ctx, changed, local);
	midr_zebra_route_flush(&bgp);
	assert(sent_route_count == 4);
	assert(sent_routes[3].command == ZEBRA_ROUTE_DELETE);

	ecmp = compute_ecmp_results();
	midr_spf_install_results(&ctx, NULL, ecmp);
	midr_zebra_route_flush(&bgp);
	assert(sent_route_count == 5);
	assert(sent_routes[4].command == ZEBRA_ROUTE_ADD);
	assert(sent_routes[4].route.metric == UINT32_MAX);
	assert(sent_routes[4].route.nexthop_num == 2);
	assert(sent_routes[4].route.nexthops[0].ifindex == 7);
	assert(sent_routes[4].route.nexthops[1].ifindex == 8);

	ipv6 = compute_results("2001:db8:100::/64", "2001:db8::2", 11, 32000, 8, false);
	midr_spf_install_results(&ctx, NULL, ipv6);
	midr_zebra_route_flush(&bgp);
	assert(sent_route_count == 6);
	assert(sent_routes[5].command == ZEBRA_ROUTE_ADD);
	assert(sent_routes[5].route.metric == 11);
	assert(sent_routes[5].route.nexthop_num == 1);
	assert(sent_routes[5].route.nexthops[0].type == NEXTHOP_TYPE_IPV6_IFINDEX);
	assert(sent_routes[5].route.nexthops[0].ifindex == 8);
	assert(inet_pton(AF_INET6, "2001:db8::2", &expected_v6) == 1);
	assert(IPV6_ADDR_SAME(&sent_routes[5].route.nexthops[0].gate.ipv6, &expected_v6));

	/*
	 * The current data-plane API does not carry a nexthop address family.
	 * The adapter must not silently reinterpret an IPv6 nexthop as IPv4.
	 */
	mixed = compute_results("198.51.100.0/24", "2001:db8::3", 3, 1000, 9, false);
	midr_spf_install_results(&ctx, NULL, mixed);
	midr_zebra_route_flush(&bgp);
	assert(sent_route_count == 6);

	midr_spf_install_results(&ctx, ipv6, NULL);
	midr_zebra_route_flush(&bgp);
	assert(sent_route_count == 7);
	assert(sent_routes[6].command == ZEBRA_ROUTE_DELETE);

	cross_initial = compute_cross_results(1, 50, 5);
	midr_spf_install_results(&ctx, NULL, cross_initial);
	midr_zebra_route_flush(&bgp);
	assert(sent_route_count == 8);
	assert(sent_routes[7].command == ZEBRA_ROUTE_ADD);
	assert(sent_routes[7].route.metric == 50);

	cross_local_changed = compute_cross_results(2, 50, 9);
	midr_spf_install_results(&ctx, cross_initial, cross_local_changed);
	midr_zebra_route_flush(&bgp);
	assert(sent_route_count == 8);

	cross_group_changed = compute_cross_results(3, 70, 9);
	midr_spf_install_results(&ctx, cross_local_changed, cross_group_changed);
	midr_zebra_route_flush(&bgp);
	assert(sent_route_count == 10);
	assert(sent_routes[8].command == ZEBRA_ROUTE_DELETE);
	assert(sent_routes[9].command == ZEBRA_ROUTE_ADD);
	assert(sent_routes[9].route.metric == 70);

	midr_spf_results_release(&cross_group_changed);
	midr_spf_results_release(&cross_local_changed);
	midr_spf_results_release(&cross_initial);
	midr_spf_results_release(&mixed);
	midr_spf_results_release(&ipv6);
	midr_spf_results_release(&ecmp);
	midr_spf_results_release(&local);
	midr_spf_results_release(&changed);
	midr_spf_results_release(&same);
	midr_spf_results_release(&initial);
	midr_zebra_fini(&bgp);
}

int main(void)
{
	master = event_master_create("test_midr_spf_install");
	test_bm.master = master;
	reset_sent_routes();
	test_spf_install_adapter();
	event_master_free(master);
	return 0;
}
