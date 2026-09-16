/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MIDRD_SPF_H
#define MIDRD_SPF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "midr-consumer.h"
#include "midr-ted.h"

enum midr_spf_route_scope {
	MIDR_SPF_ROUTE_UNREACHABLE = 0,
	MIDR_SPF_ROUTE_LOCAL,
	MIDR_SPF_ROUTE_INTRA_GROUP,
	MIDR_SPF_ROUTE_INTER_GROUP,
};

struct midr_spf_nexthop {
	uint8_t family;
	uint8_t reserved[3];
	uint8_t address[MIDR_CORE_ADDR_BYTES];
	uint32_t ifindex;
	uint32_t local_node_id;
	uint32_t remote_node_id;
	uint32_t local_group_id;
	uint32_t remote_group_id;
	uint32_t destination_group_id;
	uint32_t next_group_id;
	uint64_t link_id;
};

struct midr_spf_route {
	uint8_t family;
	uint8_t prefix_len;
	uint8_t reserved[2];
	uint8_t prefix[MIDR_CORE_ADDR_BYTES];
	uint32_t originator;
	uint64_t metric;
	uint64_t generation;
	enum midr_spf_route_scope scope;
	uint64_t group_score;
	uint64_t local_cost;
	bool reachable;
	bool local_destination;
	struct midr_spf_nexthop *nexthops;
	size_t nexthop_count;
};

/* Compute prefix paths from local_node_id over the committed Link graph.
 * When a snapshot has no Link events, retain the pre-graph direct-prefix
 * selection semantics so a prefix-only deployment remains usable. */
int midr_spf_compute(const struct midr_consumer_snapshot *snapshot,
			    uint32_t local_node_id,
			    struct midr_spf_route *routes, size_t capacity,
			    size_t *count);

/* Compute over the formal layered TED view. */
int midr_spf_compute_ted(const struct midr_ted_view *view,
			 struct midr_spf_route *routes, size_t capacity,
			 size_t *count);

void midr_spf_routes_clear(struct midr_spf_route *routes, size_t count);

#endif /* MIDRD_SPF_H */
