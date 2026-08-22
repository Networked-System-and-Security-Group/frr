// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Stable, read-only MIDR TED interface for path computation.
 */

#ifndef _FRR_BGP_MIDR_TED_H
#define _FRR_BGP_MIDR_TED_H

#include <zebra.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "if.h"
#include "ipaddr.h"
#include "prefix.h"

struct midr_context;
struct midr_ted_consumer;

#define MIDR_TED_LINK_COST_MAX (UINT32_MAX - 1U)

enum midr_ted_sync_reason {
	MIDR_TED_SYNC_REASON_NONE = 0,
	MIDR_TED_SYNC_REASON_EOR_TIMEOUT = (1U << 0),
	MIDR_TED_SYNC_REASON_RESYNC_FAILED = (1U << 1),
};

enum midr_ted_change {
	MIDR_TED_CHANGE_NONE = 0,
	MIDR_TED_CHANGE_LOCAL = (1U << 0),
	MIDR_TED_CHANGE_NODES = (1U << 1),
	MIDR_TED_CHANGE_LINKS = (1U << 2),
	MIDR_TED_CHANGE_GROUP_EDGES = (1U << 3),
	MIDR_TED_CHANGE_PREFIXES = (1U << 4),
	MIDR_TED_CHANGE_SYNC = (1U << 5),
	MIDR_TED_CHANGE_ALL = MIDR_TED_CHANGE_LOCAL | MIDR_TED_CHANGE_NODES |
			      MIDR_TED_CHANGE_LINKS | MIDR_TED_CHANGE_GROUP_EDGES |
			      MIDR_TED_CHANGE_PREFIXES | MIDR_TED_CHANGE_SYNC,
};

struct midr_ted_prefix_key {
	afi_t afi;
	safi_t safi;
	struct prefix prefix;
};

struct midr_ted_node {
	uint32_t node_id;
	uint32_t group_id;
	uint64_t cap_flags;
	uint64_t policy_tags;
};

struct midr_ted_link {
	uint32_t local_node_id;
	uint32_t remote_node_id;
	uint32_t local_group_id;
	uint32_t remote_group_id;
	uint64_t link_id;
	uint32_t canonical_cost;
	uint64_t policy_tags;
	struct ipaddr link_local_address;
	struct ipaddr link_remote_address;
	ifindex_t local_ifindex;
};

struct midr_ted_node_prefix {
	struct midr_ted_prefix_key key;
	uint32_t node_id;
};

struct midr_ted_group_edge {
	uint32_t source_group_id;
	uint32_t target_group_id;
	uint64_t aggregate_cost;
};

struct midr_ted_prefix_group {
	struct midr_ted_prefix_key key;
	uint32_t group_id;
};

struct midr_ted_snapshot {
	uint64_t generation;
	uint32_t local_node_id;
	uint32_t local_group_id;
	bool ready;
	uint64_t sync_reason_flags;

	const struct midr_ted_node *nodes;
	size_t node_count;
	const struct midr_ted_link *intra_links;
	size_t intra_link_count;
	const struct midr_ted_link *egress_links;
	size_t egress_link_count;
	const struct midr_ted_node_prefix *node_prefixes;
	size_t node_prefix_count;
	const struct midr_ted_group_edge *group_edges;
	size_t group_edge_count;
	const struct midr_ted_prefix_group *prefix_groups;
	size_t prefix_group_count;
};

struct midr_ted_consumer_ops {
	void (*snapshot_changed)(struct midr_context *ctx, uint64_t generation,
				 uint32_t change_flags, void *arg);
};

extern int midr_ted_snapshot_get(struct midr_context *ctx, const struct midr_ted_snapshot **out);
extern void midr_ted_snapshot_release(const struct midr_ted_snapshot **snapshot);
extern bool midr_ted_generation_is_current(struct midr_context *ctx, uint64_t generation);
extern int midr_ted_consumer_register(struct midr_context *ctx,
				      const struct midr_ted_consumer_ops *ops, void *arg,
				      struct midr_ted_consumer **out);
extern void midr_ted_consumer_unregister(struct midr_context *ctx,
					 struct midr_ted_consumer **consumer);

#endif /* _FRR_BGP_MIDR_TED_H */
