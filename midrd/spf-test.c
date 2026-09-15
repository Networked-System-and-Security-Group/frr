#include "midr-spf.h"

#include <assert.h>
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
	events[1].originator = 2;
	events[1].remote = 3;
	events[2].originator = 1;
	events[2].remote = 3;
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
	       routes[0].reachable);
	assert(routes[1].originator == 1 && routes[1].metric == 4 &&
	       routes[1].reachable);
}

int main(void)
{
	test_direct_prefix_selection();
	test_link_graph_costs();
	puts("midrd-spf-test: PASS");
	return 0;
}
