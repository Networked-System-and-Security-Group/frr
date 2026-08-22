// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR hierarchical shortest path computation tests
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
#include "bgpd/bgp_midr_ted_private.h"

struct zebra_privs_t bgpd_privs = {};
struct event_loop *master;
static struct bgp_master test_bm;
struct bgp_master *bm = &test_bm;
struct zclient *bgp_zclient;

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

static struct midr_ted_link ted_link(uint32_t local_node_id, uint32_t remote_node_id,
				     uint32_t local_group_id, uint32_t remote_group_id,
				     uint64_t link_id, uint32_t cost, uint32_t bandwidth,
				     const char *remote_address, ifindex_t ifindex)
{
	struct ipaddr remote = ip_address(remote_address);
	struct ipaddr local;

	if (remote.ipa_type == IPADDR_V6)
		local = ip_address("2001:db8::1");
	else
		local = ip_address("192.0.2.1");

	return (struct midr_ted_link){
		.local_node_id = local_node_id,
		.remote_node_id = remote_node_id,
		.local_group_id = local_group_id,
		.remote_group_id = remote_group_id,
		.link_id = link_id,
		.canonical_cost = cost,
		.available_bandwidth_kbps = bandwidth,
		.link_local_address = local,
		.link_remote_address = remote,
		.local_ifindex = ifindex,
	};
}

static void test_intra_group_ecmp_and_owned_result(void)
{
	const uint32_t a = node_id("1.1.1.1");
	const uint32_t b = node_id("2.2.2.2");
	const uint32_t c = node_id("3.3.3.3");
	const uint32_t d = node_id("4.4.4.4");
	struct midr_ted_node nodes[] = {
		{ .node_id = a, .group_id = 100 },
		{ .node_id = b, .group_id = 100 },
		{ .node_id = c, .group_id = 100 },
		{ .node_id = d, .group_id = 100 },
	};
	struct midr_ted_link links[] = {
		ted_link(a, b, 100, 100, 1, 2, 100000, "10.0.12.2", 12),
		ted_link(a, c, 100, 100, 2, 2, 90000, "10.0.13.3", 13),
		ted_link(a, b, 100, 100, 6, 2, 95000, "10.0.22.2", 22),
		ted_link(b, d, 100, 100, 3, 3, 80000, "10.0.24.4", 24),
		ted_link(c, d, 100, 100, 4, 3, 70000, "10.0.34.4", 34),
		ted_link(a, d, 100, 100, 5, 10, 110000, "10.0.14.4", 14),
	};
	struct midr_ted_node_prefix node_prefixes[] = {
		{ .key = prefix_key("203.0.113.0/24"), .node_id = d },
	};
	struct midr_ted_snapshot snapshot = {
		.generation = 7,
		.local_node_id = a,
		.local_group_id = 100,
		.ready = true,
		.sync_reason_flags = MIDR_TED_SYNC_REASON_EOR_TIMEOUT,
		.nodes = nodes,
		.node_count = array_size(nodes),
		.intra_links = links,
		.intra_link_count = array_size(links),
		.node_prefixes = node_prefixes,
		.node_prefix_count = array_size(node_prefixes),
	};
	struct midr_spf_route *route = NULL;

	assert(midr_compute_path(&snapshot, &node_prefixes[0].key, &route) == 0);
	assert(route);
	assert(route->generation == 7);
	assert(route->sync_reason_flags == MIDR_TED_SYNC_REASON_EOR_TIMEOUT);
	assert(route->reachable);
	assert(!route->local_destination);
	assert(route->scope == MIDR_SPF_ROUTE_INTRA_GROUP);
	assert(route->group_score == 0);
	assert(route->local_cost == 5);
	assert(route->nexthop_count == 3);
	assert(ipaddr_cmp(&route->nexthops[0].address, &links[0].link_remote_address) == 0);
	assert(route->nexthops[0].ifindex == 12);
	assert(route->nexthops[0].available_bandwidth_kbps == 80000);
	assert(ipaddr_cmp(&route->nexthops[1].address, &links[1].link_remote_address) == 0);
	assert(route->nexthops[1].available_bandwidth_kbps == 70000);
	assert(ipaddr_cmp(&route->nexthops[2].address, &links[2].link_remote_address) == 0);
	assert(route->nexthops[2].available_bandwidth_kbps == 80000);

	links[0].link_remote_address = ip_address("198.51.100.1");
	links[0].available_bandwidth_kbps = 1;
	assert(ipaddr_cmp(&route->nexthops[0].address, &links[0].link_remote_address) != 0);
	assert(route->nexthops[0].available_bandwidth_kbps == 80000);

	midr_spf_route_free(&route);
	assert(!route);
}

static void test_ipv6_prefix_and_nexthops(void)
{
	const uint32_t a = node_id("1.1.1.1");
	const uint32_t b = node_id("2.2.2.2");
	struct midr_ted_node nodes[] = {
		{ .node_id = a, .group_id = 100 },
		{ .node_id = b, .group_id = 100 },
	};
	struct midr_ted_link links[] = {
		ted_link(a, b, 100, 100, 1, 7, 100000, "2001:db8:12::2", 12),
		ted_link(a, b, 100, 100, 2, 7, 90000, "2001:db8:13::2", 13),
	};
	struct midr_ted_node_prefix node_prefixes[] = {
		{ .key = prefix_key("192.0.2.0/24"), .node_id = b },
		{ .key = prefix_key("2001:db8:100::/64"), .node_id = b },
	};
	struct midr_ted_snapshot snapshot = {
		.generation = 8,
		.local_node_id = a,
		.local_group_id = 100,
		.ready = true,
		.nodes = nodes,
		.node_count = array_size(nodes),
		.intra_links = links,
		.intra_link_count = array_size(links),
		.node_prefixes = node_prefixes,
		.node_prefix_count = array_size(node_prefixes),
	};
	struct midr_ted_prefix_key ipv6_lookup = prefix_key("2001:db8:100::ffff/64");
	struct midr_ted_prefix_key ipv6_network = prefix_key("2001:db8:100::/64");
	const struct midr_spf_results *results = NULL;
	const struct midr_spf_route *route;

	assert(midr_spf_compute_all(&snapshot, &results) == 0);
	assert(midr_spf_results_count(results) == 2);

	route = midr_spf_results_lookup(results, &ipv6_lookup);
	assert(route && route->reachable && !route->local_destination);
	assert(route->prefix.afi == AFI_IP6);
	assert(route->prefix.prefix.family == AF_INET6);
	assert(prefix_same(&route->prefix.prefix, &ipv6_network.prefix));
	assert(route->scope == MIDR_SPF_ROUTE_INTRA_GROUP);
	assert(route->group_score == 0);
	assert(route->local_cost == 7);
	assert(route->nexthop_count == 2);
	assert(route->nexthops[0].address.ipa_type == IPADDR_V6);
	assert(ipaddr_cmp(&route->nexthops[0].address, &links[0].link_remote_address) == 0);
	assert(route->nexthops[0].ifindex == 12);
	assert(route->nexthops[1].address.ipa_type == IPADDR_V6);
	assert(ipaddr_cmp(&route->nexthops[1].address, &links[1].link_remote_address) == 0);
	assert(route->nexthops[1].ifindex == 13);

	/*
	 * Prefix and nexthop address families are independent.  This also
	 * protects the valid IPv4-prefix/IPv6-nexthop combination needed by
	 * the data-plane adapter.
	 */
	route = midr_spf_results_lookup(results, &node_prefixes[0].key);
	assert(route && route->reachable);
	assert(route->prefix.afi == AFI_IP);
	assert(route->nexthop_count == 2);
	assert(route->nexthops[0].address.ipa_type == IPADDR_V6);
	assert(route->nexthops[1].address.ipa_type == IPADDR_V6);

	midr_spf_results_release(&results);
	assert(!results);
}

static void test_local_unreachable_and_result_set(void)
{
	const uint32_t a = node_id("1.1.1.1");
	const uint32_t b = node_id("2.2.2.2");
	struct midr_ted_node nodes[] = {
		{ .node_id = a, .group_id = 100 },
		{ .node_id = b, .group_id = 100 },
	};
	struct midr_ted_node_prefix node_prefixes[] = {
		{ .key = prefix_key("10.0.0.0/24"), .node_id = a },
		{ .key = prefix_key("10.0.1.0/24"), .node_id = b },
	};
	struct midr_ted_prefix_group prefix_groups[] = {
		{ .key = prefix_key("10.0.2.0/24"), .group_id = 200 },
		{ .key = prefix_key("10.0.2.0/24"), .group_id = 300 },
	};
	struct midr_ted_snapshot snapshot = {
		.generation = 9,
		.local_node_id = a,
		.local_group_id = 100,
		.ready = true,
		.nodes = nodes,
		.node_count = array_size(nodes),
		.node_prefixes = node_prefixes,
		.node_prefix_count = array_size(node_prefixes),
		.prefix_groups = prefix_groups,
		.prefix_group_count = array_size(prefix_groups),
	};
	const struct midr_spf_results *results = NULL;
	const struct midr_spf_route *route;

	assert(midr_spf_compute_all(&snapshot, &results) == 0);
	assert(midr_spf_results_generation(results) == 9);
	assert(midr_spf_results_count(results) == 3);

	route = midr_spf_results_lookup(results, &node_prefixes[0].key);
	assert(route && route->reachable && route->local_destination);
	assert(route->scope == MIDR_SPF_ROUTE_LOCAL);
	assert(route->group_score == 0 && route->local_cost == 0);
	assert(route->nexthop_count == 0);

	route = midr_spf_results_lookup(results, &node_prefixes[1].key);
	assert(route && !route->reachable);
	assert(route->scope == MIDR_SPF_ROUTE_UNREACHABLE);
	assert(route->group_score == UINT64_MAX);
	assert(route->local_cost == UINT64_MAX);

	route = midr_spf_results_lookup(results, &prefix_groups[0].key);
	assert(route && !route->reachable);
	assert(midr_spf_results_at(results, 3) == NULL);

	midr_spf_results_release(&results);
	assert(!results);
}

static void test_cross_group_hierarchy_and_ecmp(void)
{
	const uint32_t a = node_id("1.1.1.1");
	const uint32_t b = node_id("2.2.2.2");
	const uint32_t c = node_id("3.3.3.3");
	const uint32_t d = node_id("4.4.4.4");
	const uint32_t x = node_id("20.0.0.1");
	const uint32_t y = node_id("20.0.0.2");
	const uint32_t z = node_id("40.0.0.1");
	struct midr_ted_node nodes[] = {
		{ .node_id = a, .group_id = 100 },
		{ .node_id = b, .group_id = 100 },
		{ .node_id = c, .group_id = 100 },
		{ .node_id = d, .group_id = 100 },
	};
	struct midr_ted_link intra[] = {
		ted_link(a, b, 100, 100, 1, 5, 100000, "10.0.12.2", 12),
		ted_link(a, c, 100, 100, 2, 2, 100000, "10.0.13.3", 13),
	};
	struct midr_ted_link egress[] = {
		ted_link(b, x, 100, 200, 10, 10, 90000, "10.0.20.1", 20),
		ted_link(c, y, 100, 200, 11, 13, 80000, "10.0.20.2", 21),
		ted_link(a, z, 100, 400, 12, 22, 70000, "10.0.40.1", 40),
	};
	struct midr_ted_group_edge group_edges[] = {
		{ .source_group_id = 100, .target_group_id = 200, .aggregate_cost = 10 },
		{ .source_group_id = 200, .target_group_id = 300, .aggregate_cost = 7 },
		{ .source_group_id = 100, .target_group_id = 300, .aggregate_cost = 30 },
		{ .source_group_id = 100, .target_group_id = 400, .aggregate_cost = 22 },
	};
	struct midr_ted_prefix_group prefix_groups[] = {
		{ .key = prefix_key("198.51.100.0/24"), .group_id = 300 },
		{ .key = prefix_key("198.51.100.0/24"), .group_id = 400 },
	};
	struct midr_ted_node_prefix node_prefix = {
		.key = prefix_key("198.51.100.0/24"),
		.node_id = d,
	};
	struct midr_ted_snapshot snapshot = {
		.generation = 11,
		.local_node_id = a,
		.local_group_id = 100,
		.ready = true,
		.nodes = nodes,
		.node_count = array_size(nodes),
		.intra_links = intra,
		.intra_link_count = array_size(intra),
		.egress_links = egress,
		.egress_link_count = array_size(egress),
		.node_prefixes = &node_prefix,
		.node_prefix_count = 1,
		.group_edges = group_edges,
		.group_edge_count = array_size(group_edges),
		.prefix_groups = prefix_groups,
		.prefix_group_count = array_size(prefix_groups),
	};
	struct midr_spf_route *route = NULL;

	assert(midr_compute_path(&snapshot, &prefix_groups[0].key, &route) == 0);
	assert(route && route->reachable && !route->local_destination);
	assert(route->scope == MIDR_SPF_ROUTE_INTER_GROUP);
	assert(route->group_score == 17);
	assert(route->local_cost == 15);
	assert(route->nexthop_count == 2);

	assert(route->nexthops[0].available_bandwidth_kbps == 0);
	assert(route->nexthops[1].available_bandwidth_kbps == 0);

	assert(route->nexthops[0].next_group_id == 200);
	assert(route->nexthops[0].destination_group_id == 300);
	assert(route->nexthops[1].next_group_id == 200);
	assert(route->nexthops[1].destination_group_id == 300);

	midr_spf_route_free(&route);
}

static void test_cross_group_lexicographic_selection(void)
{
	const uint32_t a = node_id("1.1.1.1");
	const uint32_t x = node_id("20.0.0.1");
	const uint32_t y = node_id("30.0.0.1");
	struct midr_ted_node node = {
		.node_id = a,
		.group_id = 100,
	};
	struct midr_ted_link egress[] = {
		ted_link(a, x, 100, 200, 1, 100, 100000, "10.0.20.1", 20),
		ted_link(a, y, 100, 300, 2, 1, 100000, "10.0.30.1", 30),
	};
	struct midr_ted_group_edge group_edges[] = {
		{ .source_group_id = 100, .target_group_id = 200, .aggregate_cost = 4 },
		{ .source_group_id = 200, .target_group_id = 500, .aggregate_cost = 5 },
		{ .source_group_id = 100, .target_group_id = 300, .aggregate_cost = 5 },
		{ .source_group_id = 300, .target_group_id = 500, .aggregate_cost = 5 },
	};
	struct midr_ted_prefix_group mapping = {
		.key = prefix_key("198.18.1.0/24"),
		.group_id = 500,
	};
	struct midr_ted_snapshot snapshot = {
		.generation = 13,
		.local_node_id = a,
		.local_group_id = 100,
		.ready = true,
		.nodes = &node,
		.node_count = 1,
		.egress_links = egress,
		.egress_link_count = array_size(egress),
		.group_edges = group_edges,
		.group_edge_count = array_size(group_edges),
		.prefix_groups = &mapping,
		.prefix_group_count = 1,
	};
	struct midr_spf_route *route = NULL;

	assert(midr_compute_path(&snapshot, &mapping.key, &route) == 0);
	assert(route && route->reachable);
	assert(route->scope == MIDR_SPF_ROUTE_INTER_GROUP);
	assert(route->group_score == 9);
	assert(route->local_cost == 100);
	assert(route->nexthop_count == 1);
	assert(route->nexthops[0].next_group_id == 200);
	assert(route->nexthops[0].destination_group_id == 500);
	assert(route->nexthops[0].ifindex == 20);
	midr_spf_route_free(&route);
}

static void test_cross_group_local_tiebreak_and_ecmp(void)
{
	const uint32_t a = node_id("1.1.1.1");
	const uint32_t x = node_id("20.0.0.1");
	const uint32_t y = node_id("30.0.0.1");
	struct midr_ted_node node = {
		.node_id = a,
		.group_id = 100,
	};
	struct midr_ted_link egress[] = {
		ted_link(a, x, 100, 200, 1, 20, 100000, "10.0.20.1", 20),
		ted_link(a, y, 100, 300, 2, 5, 100000, "10.0.30.1", 30),
	};
	struct midr_ted_group_edge group_edges[] = {
		{ .source_group_id = 100, .target_group_id = 200, .aggregate_cost = 4 },
		{ .source_group_id = 200, .target_group_id = 500, .aggregate_cost = 6 },
		{ .source_group_id = 100, .target_group_id = 300, .aggregate_cost = 5 },
		{ .source_group_id = 300, .target_group_id = 500, .aggregate_cost = 5 },
	};
	struct midr_ted_prefix_group mapping = {
		.key = prefix_key("198.18.2.0/24"),
		.group_id = 500,
	};
	struct midr_ted_snapshot snapshot = {
		.generation = 14,
		.local_node_id = a,
		.local_group_id = 100,
		.ready = true,
		.nodes = &node,
		.node_count = 1,
		.egress_links = egress,
		.egress_link_count = array_size(egress),
		.group_edges = group_edges,
		.group_edge_count = array_size(group_edges),
		.prefix_groups = &mapping,
		.prefix_group_count = 1,
	};
	struct midr_spf_route *route = NULL;

	assert(midr_compute_path(&snapshot, &mapping.key, &route) == 0);
	assert(route && route->reachable);
	assert(route->group_score == 10);
	assert(route->local_cost == 5);
	assert(route->nexthop_count == 1);
	assert(route->nexthops[0].next_group_id == 300);
	midr_spf_route_free(&route);

	egress[0].canonical_cost = 5;
	assert(midr_compute_path(&snapshot, &mapping.key, &route) == 0);
	assert(route && route->reachable);
	assert(route->group_score == 10);
	assert(route->local_cost == 5);
	assert(route->nexthop_count == 2);
	assert(route->nexthops[0].next_group_id == 200);
	assert(route->nexthops[1].next_group_id == 300);
	midr_spf_route_free(&route);
}

static void test_unexecutable_group_first_hop_filtered(void)
{
	const uint32_t a = node_id("1.1.1.1");
	const uint32_t y = node_id("30.0.0.1");
	struct midr_ted_node node = {
		.node_id = a,
		.group_id = 100,
	};
	struct midr_ted_link egress = ted_link(a, y, 100, 300, 1, 7, 100000, "10.0.30.1", 30);
	struct midr_ted_group_edge group_edges[] = {
		{ .source_group_id = 100, .target_group_id = 200, .aggregate_cost = 1 },
		{ .source_group_id = 200, .target_group_id = 500, .aggregate_cost = 1 },
		{ .source_group_id = 100, .target_group_id = 300, .aggregate_cost = 2 },
		{ .source_group_id = 300, .target_group_id = 500, .aggregate_cost = 2 },
	};
	struct midr_ted_prefix_group mapping = {
		.key = prefix_key("198.18.3.0/24"),
		.group_id = 500,
	};
	struct midr_ted_snapshot snapshot = {
		.generation = 15,
		.local_node_id = a,
		.local_group_id = 100,
		.ready = true,
		.nodes = &node,
		.node_count = 1,
		.egress_links = &egress,
		.egress_link_count = 1,
		.group_edges = group_edges,
		.group_edge_count = array_size(group_edges),
		.prefix_groups = &mapping,
		.prefix_group_count = 1,
	};
	struct midr_spf_route *route = NULL;

	assert(midr_compute_path(&snapshot, &mapping.key, &route) == 0);
	assert(route && route->reachable);
	assert(route->scope == MIDR_SPF_ROUTE_INTER_GROUP);
	assert(route->group_score == 4);
	assert(route->local_cost == 7);
	assert(route->nexthop_count == 1);
	assert(route->nexthops[0].next_group_id == 300);
	midr_spf_route_free(&route);
}

static void test_invalid_edge_and_overflow_skip(void)
{
	const uint32_t a = node_id("1.1.1.1");
	const uint32_t b = node_id("2.2.2.2");
	struct midr_ted_node nodes[] = {
		{ .node_id = a, .group_id = 100 },
		{ .node_id = b, .group_id = 100 },
	};
	struct midr_ted_link intra[] = {
		ted_link(a, b, 100, 100, 1, 0, 1000, "10.0.0.2", 2),
	};
	struct midr_ted_group_edge group_edges[] = {
		{ .source_group_id = 100, .target_group_id = 200, .aggregate_cost = UINT64_MAX },
		{ .source_group_id = 200, .target_group_id = 300, .aggregate_cost = 1 },
	};
	struct midr_ted_node_prefix local_prefix = {
		.key = prefix_key("192.0.2.0/24"),
		.node_id = b,
	};
	struct midr_ted_prefix_group remote_prefix = {
		.key = prefix_key("203.0.113.0/24"),
		.group_id = 300,
	};
	struct midr_ted_snapshot snapshot = {
		.generation = 12,
		.local_node_id = a,
		.local_group_id = 100,
		.ready = true,
		.nodes = nodes,
		.node_count = array_size(nodes),
		.intra_links = intra,
		.intra_link_count = array_size(intra),
		.node_prefixes = &local_prefix,
		.node_prefix_count = 1,
		.group_edges = group_edges,
		.group_edge_count = array_size(group_edges),
		.prefix_groups = &remote_prefix,
		.prefix_group_count = 1,
	};
	struct midr_spf_route *route = NULL;

	assert(midr_compute_path(&snapshot, &local_prefix.key, &route) == 0);
	assert(route && !route->reachable);
	midr_spf_route_free(&route);

	assert(midr_compute_path(&snapshot, &remote_prefix.key, &route) == 0);
	assert(route && !route->reachable);
	midr_spf_route_free(&route);
}

static void run_one_event(void)
{
	struct event event;

	assert(event_fetch(master, &event));
	event_call(&event);
}

static void publish_runtime_snapshot(struct midr_context *ctx, uint32_t link_cost,
				     bool include_prefix)
{
	const uint32_t a = node_id("1.1.1.1");
	const uint32_t b = node_id("2.2.2.2");
	struct midr_ted_builder *builder = NULL;
	struct midr_ted_node node_a = {
		.node_id = a,
		.group_id = 100,
	};
	struct midr_ted_node node_b = {
		.node_id = b,
		.group_id = 100,
	};
	struct midr_ted_link_input input = {
		.local_node_id = a,
		.remote_node_id = b,
		.link_id = 1,
		.canonical_cost = link_cost,
		.available_bandwidth_kbps = 50000,
		.link_local_address = ip_address("10.0.0.1"),
		.link_remote_address = ip_address("10.0.0.2"),
		.local_ifindex = 2,
	};
	struct midr_ted_node_prefix mapping = {
		.key = prefix_key("10.10.0.0/16"),
		.node_id = b,
	};

	assert(midr_ted_builder_create(a, 100, &builder) == 0);
	assert(midr_ted_builder_add_node(builder, &node_a) == 0);
	assert(midr_ted_builder_add_node(builder, &node_b) == 0);
	assert(midr_ted_builder_add_link(builder, &input) == 0);
	if (include_prefix)
		assert(midr_ted_builder_add_node_prefix(builder, &mapping) == 0);
	assert(midr_ted_builder_publish(ctx, builder, 0) == 0);
	midr_ted_builder_destroy(&builder);
}

static void test_runtime_recompute_and_cache_lifetime(void)
{
	const struct midr_spf_results *generation_two = NULL;
	const struct midr_spf_results *current = NULL;
	struct midr_spf_runtime_status status;
	struct midr_context ctx = {};

	master = event_master_create("MIDR SPF test");
	test_bm.master = master;
	assert(midr_ted_context_init(&ctx) == 0);
	assert(midr_spf_context_init(&ctx) == 0);

	run_one_event();
	assert(midr_spf_results_get(&ctx, &current) == -EAGAIN);

	publish_runtime_snapshot(&ctx, 20, true);
	publish_runtime_snapshot(&ctx, 10, true);
	assert(midr_spf_runtime_status_get(&ctx, &status) == 0);
	assert(status.recompute_pending);
	assert(status.pending_generation == 2);
	run_one_event();

	assert(midr_spf_results_get(&ctx, &generation_two) == 0);
	assert(midr_spf_results_generation(generation_two) == 2);
	assert(midr_spf_results_count(generation_two) == 1);
	assert(midr_spf_results_at(generation_two, 0)->local_cost == 10);

	publish_runtime_snapshot(&ctx, 5, true);
	run_one_event();
	assert(midr_spf_results_get(&ctx, &current) == 0);
	assert(midr_spf_results_generation(current) == 3);
	assert(midr_spf_results_at(current, 0)->local_cost == 5);
	assert(midr_spf_results_generation(generation_two) == 2);
	assert(midr_spf_results_at(generation_two, 0)->local_cost == 10);
	midr_spf_results_release(&current);

	publish_runtime_snapshot(&ctx, 5, false);
	run_one_event();
	assert(midr_spf_results_get(&ctx, &current) == 0);
	assert(midr_spf_results_generation(current) == 4);
	assert(midr_spf_results_count(current) == 0);
	assert(midr_spf_results_generation(generation_two) == 2);

	midr_spf_results_release(&current);
	midr_spf_results_release(&generation_two);
	midr_spf_context_finish(&ctx);
	midr_ted_context_finish(&ctx);
	event_master_free(master);
	master = NULL;
	test_bm.master = NULL;
}

int main(void)
{
	test_intra_group_ecmp_and_owned_result();
	test_ipv6_prefix_and_nexthops();
	test_local_unreachable_and_result_set();
	test_cross_group_hierarchy_and_ecmp();
	test_cross_group_lexicographic_selection();
	test_cross_group_local_tiebreak_and_ecmp();
	test_unexecutable_group_first_hop_filtered();
	test_invalid_edge_and_overflow_skip();
	test_runtime_recompute_and_cache_lifetime();
	printf("MIDR hierarchical SPF tests passed\n");
	return 0;
}
