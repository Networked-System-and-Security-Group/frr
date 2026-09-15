/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MIDRD_SPF_H
#define MIDRD_SPF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "midr-consumer.h"

struct midr_spf_route {
	uint8_t family;
	uint8_t prefix_len;
	uint8_t reserved[2];
	uint8_t prefix[MIDR_CORE_ADDR_BYTES];
	uint32_t originator;
	uint32_t metric;
	uint64_t generation;
	bool reachable;
};

/* Compute prefix paths from local_node_id over the committed Link graph.
 * When a snapshot has no Link events, retain the pre-graph direct-prefix
 * selection semantics so a prefix-only deployment remains usable. */
int midr_spf_compute(const struct midr_consumer_snapshot *snapshot,
			    uint32_t local_node_id,
			    struct midr_spf_route *routes, size_t capacity,
			    size_t *count);

#endif /* MIDRD_SPF_H */
