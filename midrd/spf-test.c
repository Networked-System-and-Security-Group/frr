#include "midr-spf.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_direct_prefix_selection(void)
{
	struct midr_consumer_event events[4] = {0};
	struct midr_consumer_snapshot snapshot = {
		.generation = 9,
		.count = 4,
		.events = events,
	};
	struct midr_spf_route routes[2] = {0};
	size_t count = 0;

	events[0].kind = MIDR_CONSUMER_NODE_PREFIX;
	events[0].generation = 9;
	events[0].originator = 10;
	events[0].family = MIDR_CORE_AF_IPV4;
	events[0].prefix_len = 24;
	events[0].prefix[0] = 192;
	events[0].prefix[1] = 0;
	events[0].prefix[2] = 2;
	events[0].metric = 30;
	events[1] = events[0];
	events[1].originator = 11;
	events[1].metric = 10;
	events[2] = events[0];
	events[2].family = MIDR_CORE_AF_IPV6;
	events[2].prefix_len = 64;
	events[2].prefix[0] = 0x20;
	events[2].prefix[1] = 0x01;
	events[2].prefix[2] = 0x0d;
	events[2].metric = 20;
	events[3] = events[0];
	events[3].kind = MIDR_CONSUMER_LINK;
	assert(midr_spf_compute(&snapshot, 1, routes, 2, &count) == 0);
	assert(count == 2);
	assert(routes[0].family == MIDR_CORE_AF_IPV4 &&
	       routes[0].originator == 11 && routes[0].metric == 10 &&
	       routes[0].reachable);
	assert(routes[1].family == MIDR_CORE_AF_IPV6 && routes[1].metric == 20 &&
	       routes[1].reachable);
	midr_spf_routes_clear(routes, count);
}

static void test_link_graph_costs(void)
{
	struct midr_consumer_event events[6] = {0};
	struct midr_consumer_snapshot snapshot = {
		.generation = 10,
		.count = 6,
		.events = events,
	};
	struct midr_spf_route routes[2] = {0};
	size_t count = 0;

	/* 1 -> 2 -> 3 (5 + 7) beats the direct 1 -> 3 (30) path. */
	for (size_t i = 0; i < 3; i++) {
		events[i].kind = MIDR_CONSUMER_LINK;
		events[i].generation = 10;
		events[i].metric = i == 0 ? 5 : (i == 1 ? 7 : 30);
	}
	events[0].originator = 1;
	events[0].remote = 2;
	events[0].family = MIDR_CORE_AF_IPV4;
	events[0].link_id = 1;
	events[0].remote_address[0] = 192;
	events[0].remote_address[1] = 0;
	events[0].remote_address[2] = 2;
	events[0].remote_address[3] = 2;
	events[1].originator = 2;
	events[1].remote = 3;
	events[1].family = MIDR_CORE_AF_IPV4;
	events[1].link_id = 2;
	events[2].originator = 1;
	events[2].remote = 3;
	events[2].family = MIDR_CORE_AF_IPV4;
	events[2].link_id = 3;
	events[3].kind = MIDR_CONSUMER_NODE_PREFIX;
	events[3].generation = 10;
	events[3].originator = 3;
	events[3].family = MIDR_CORE_AF_IPV4;
	events[3].prefix_len = 24;
	events[3].prefix[0] = 198;
	events[3].prefix[1] = 18;
	events[3].prefix[2] = 0;
	events[3].metric = 10;
	/* A local prefix does not pay a link cost. */
	events[4] = events[3];
	events[4].originator = 1;
	events[4].prefix[2] = 1;
	events[4].metric = 4;
	/* Invalid zero-cost links are ignored rather than creating a free path. */
	events[5] = events[0];
	events[5].originator = 3;
	events[5].remote = 4;
	events[5].metric = 0;

	assert(midr_spf_compute(&snapshot, 1, routes, 2, &count) == 0);
	assert(count == 2);
	assert(routes[0].originator == 3 && routes[0].metric == 22 &&
	       routes[0].reachable &&
	       routes[0].scope == MIDR_SPF_ROUTE_INTRA_GROUP &&
	       routes[0].nexthop_count == 1 &&
	       routes[0].nexthops[0].family == MIDR_CORE_AF_IPV4 &&
	       routes[0].nexthops[0].address[3] == 2);
	assert(routes[1].originator == 1 && routes[1].metric == 4 &&
	       routes[1].reachable && routes[1].local_destination &&
	       routes[1].scope == MIDR_SPF_ROUTE_LOCAL &&
	       routes[1].nexthop_count == 0);
	midr_spf_routes_clear(routes, count);
}

static void test_ipv6_ecmp_nexthops(void)
{
	struct midr_consumer_event events[5] = {0};
	struct midr_consumer_snapshot snapshot = {
		.generation = 11,
		.count = 5,
		.events = events,
	};
	struct midr_spf_route route = {0};
	size_t count = 0;

	for (size_t i = 0; i < 4; i++) {
		events[i].kind = MIDR_CONSUMER_LINK;
		events[i].generation = 11;
		events[i].family = MIDR_CORE_AF_IPV6;
		events[i].metric = 5;
		events[i].link_id = i + 1U;
	}
	events[0].originator = 1;
	events[0].remote = 2;
	events[0].remote_address[15] = 2;
	events[1].originator = 1;
	events[1].remote = 3;
	events[1].remote_address[15] = 3;
	events[2].originator = 2;
	events[2].remote = 4;
	events[2].remote_address[15] = 4;
	events[3].originator = 3;
	events[3].remote = 4;
	events[3].remote_address[15] = 4;
	events[4].kind = MIDR_CONSUMER_NODE_PREFIX;
	events[4].generation = 11;
	events[4].originator = 4;
	events[4].family = MIDR_CORE_AF_IPV6;
	events[4].prefix_len = 64;
	events[4].prefix[0] = 0x20;
	events[4].prefix[1] = 0x01;

	assert(midr_spf_compute(&snapshot, 1, &route, 1, &count) == 0);
	assert(count == 1 && route.reachable && route.metric == 10 &&
	       route.nexthop_count == 2);
	assert(route.nexthops[0].family == MIDR_CORE_AF_IPV6 &&
	       route.nexthops[0].address[15] == 2);
	assert(route.nexthops[1].family == MIDR_CORE_AF_IPV6 &&
	       route.nexthops[1].address[15] == 3);
	midr_spf_routes_clear(&route, count);
}

static struct midr_ted_prefix_key ted_prefix(uint8_t family, uint8_t length,
					     uint8_t marker)
{
	struct midr_ted_prefix_key key = {
		.family = family,
		.prefix_len = length,
	};

	key.prefix[0] = marker;
	return key;
}

static const struct midr_spf_route *find_route(
	const struct midr_spf_route *routes, size_t count,
	const struct midr_ted_prefix_key *key)
{
	for (size_t i = 0; i < count; i++)
		if (routes[i].family == key->family &&
		    routes[i].prefix_len == key->prefix_len &&
		    !memcmp(routes[i].prefix, key->prefix, sizeof(routes[i].prefix)))
			return &routes[i];
	return NULL;
}

static void test_layered_ted_spf(void)
{
	const struct midr_ted_prefix_key local_v4 =
		ted_prefix(MIDR_CORE_AF_IPV4, 24, 10);
	const struct midr_ted_prefix_key local_v6 =
		ted_prefix(MIDR_CORE_AF_IPV6, 64, 0x20);
	const struct midr_ted_prefix_key remote_v4 =
		ted_prefix(MIDR_CORE_AF_IPV4, 24, 20);
	const struct midr_ted_prefix_key unreachable_v6 =
		ted_prefix(MIDR_CORE_AF_IPV6, 64, 0x30);
	struct midr_ted_node nodes[] = {
		{.node_id = 1, .group_id = 10},
		{.node_id = 2, .group_id = 10},
		{.node_id = 3, .group_id = 10},
	};
	struct midr_ted_link intra_links[] = {
		{
			.local_node_id = 1,
			.remote_node_id = 2,
			.local_group_id = 10,
			.remote_group_id = 10,
			.canonical_cost = 4,
			.local_ifindex = 12,
			.family = MIDR_CORE_AF_IPV4,
			.remote_address = {192, 0, 2, 2},
		},
		{
			.local_node_id = 2,
			.remote_node_id = 3,
			.local_group_id = 10,
			.remote_group_id = 10,
			.canonical_cost = 4,
			.family = MIDR_CORE_AF_IPV4,
		},
		{
			.local_node_id = 1,
			.remote_node_id = 2,
			.local_group_id = 10,
			.remote_group_id = 10,
			.link_id = 3,
			.canonical_cost = 4,
			.local_ifindex = 13,
			.family = MIDR_CORE_AF_IPV6,
			.remote_address = {[15] = 2},
		},
	};
	/* The first egress is intentionally more expensive than the second. */
	struct midr_ted_link egress_links[] = {
		{
			.local_node_id = 2,
			.remote_node_id = 21,
			.local_group_id = 10,
			.remote_group_id = 20,
			.canonical_cost = 30,
		},
		{
			.local_node_id = 3,
			.remote_node_id = 31,
			.local_group_id = 10,
			.remote_group_id = 20,
			.canonical_cost = 7,
		},
	};
	struct midr_ted_node_prefix node_prefixes[] = {
		{.key = local_v4, .node_id = 3},
		{.key = local_v6, .node_id = 2},
	};
	struct midr_ted_group_edge group_edges[] = {
		/* Equal group paths must choose the lower next group, 20. */
		{.source_group_id = 10, .target_group_id = 30,
		 .aggregate_cost = 9},
		{.source_group_id = 30, .target_group_id = 40,
		 .aggregate_cost = 7},
		{.source_group_id = 10, .target_group_id = 20,
		 .aggregate_cost = 9},
		{.source_group_id = 20, .target_group_id = 40,
		 .aggregate_cost = 7},
		/* This edge is directed away from the source and cannot help. */
		{.source_group_id = 50, .target_group_id = 40,
		 .aggregate_cost = 1},
	};
	struct midr_ted_prefix_group prefix_groups[] = {
		{.key = remote_v4, .group_id = 40},
		{.key = unreachable_v6, .group_id = 50},
	};
	struct midr_ted_view view = {
		.generation = 42,
		.local_node_id = 1,
		.local_group_id = 10,
		.nodes = nodes,
		.node_count = sizeof(nodes) / sizeof(nodes[0]),
		.intra_links = intra_links,
		.intra_link_count = sizeof(intra_links) / sizeof(intra_links[0]),
		.egress_links = egress_links,
		.egress_link_count = sizeof(egress_links) / sizeof(egress_links[0]),
		.node_prefixes = node_prefixes,
		.node_prefix_count = sizeof(node_prefixes) / sizeof(node_prefixes[0]),
		.group_edges = group_edges,
		.group_edge_count = sizeof(group_edges) / sizeof(group_edges[0]),
		.prefix_groups = prefix_groups,
		.prefix_group_count = sizeof(prefix_groups) / sizeof(prefix_groups[0]),
	};
	struct midr_spf_route routes[4] = {0};
	size_t count = 0;
	const struct midr_spf_route *route;

	assert(midr_spf_compute_ted(&view, routes, 4, &count) == 0);
	assert(count == 4);
	route = find_route(routes, count, &local_v4);
	assert(route && route->reachable && route->originator == 3 &&
	       route->metric == 8 && route->generation == 42 &&
	       route->scope == MIDR_SPF_ROUTE_INTRA_GROUP &&
	       route->nexthop_count == 1 &&
	       route->nexthops[0].family == MIDR_CORE_AF_IPV4 &&
	       route->nexthops[0].ifindex == 12 &&
	       route->nexthops[0].address[3] == 2);
	route = find_route(routes, count, &local_v6);
	assert(route && route->reachable && route->originator == 2 &&
	       route->metric == 4 && route->nexthop_count == 1 &&
	       route->nexthops[0].family == MIDR_CORE_AF_IPV6 &&
	       route->nexthops[0].ifindex == 13 &&
	       route->nexthops[0].address[15] == 2);
	/* 1->2->3 costs 8, egress 3->31 costs 7, and 20->40 costs 7. */
	route = find_route(routes, count, &remote_v4);
	assert(route && route->reachable && route->originator == 3 &&
	       route->metric == 22 &&
	       route->scope == MIDR_SPF_ROUTE_INTER_GROUP &&
	       route->group_score == 16 && route->local_cost == 15 &&
	       route->nexthop_count == 1 &&
	       route->nexthops[0].ifindex == 12 &&
	       route->nexthops[0].destination_group_id == 40 &&
	       route->nexthops[0].next_group_id == 20);
	route = find_route(routes, count, &unreachable_v6);
	assert(route && !route->reachable && route->originator == 0 &&
	       route->metric == UINT64_MAX &&
	       route->scope == MIDR_SPF_ROUTE_UNREACHABLE &&
	       route->nexthop_count == 0);
	midr_spf_routes_clear(routes, count);
}

int main(void)
{
	test_direct_prefix_selection();
	test_link_graph_costs();
	test_ipv6_ecmp_nexthops();
	test_layered_ted_spf();
	puts("midrd-spf-test: PASS");
	return 0;
}
