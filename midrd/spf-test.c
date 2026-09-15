#include "midr-spf.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
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
	assert(midr_spf_compute(&snapshot, routes, 2, &count) == 0);
	assert(count == 2);
	assert(routes[0].family == MIDR_CORE_AF_IPV4 &&
	       routes[0].originator == 11 && routes[0].metric == 10);
	assert(routes[1].family == MIDR_CORE_AF_IPV6 && routes[1].metric == 20);
	puts("midrd-spf-test: PASS");
	return 0;
}
