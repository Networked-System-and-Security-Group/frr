// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR hierarchical shortest path computation
 *
 * Copyright (C) 2026
 */

#ifndef _FRR_BGP_MIDR_SPF_H
#define _FRR_BGP_MIDR_SPF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bgpd/bgp_midr_ted.h"

#ifdef __cplusplus
extern "C" {
#endif

struct midr_context;
struct midr_spf_results;

enum midr_spf_route_scope {
	MIDR_SPF_ROUTE_UNREACHABLE = 0,
	MIDR_SPF_ROUTE_LOCAL,
	MIDR_SPF_ROUTE_INTRA_GROUP,
	MIDR_SPF_ROUTE_INTER_GROUP,
};

/*
 * One installable physical next hop.  All fields are copied from the input
 * snapshot, so the object remains valid after midr_ted_snapshot_release().
 */
struct midr_spf_nexthop {
	struct ipaddr address;
	ifindex_t ifindex;
	uint32_t local_node_id;
	uint32_t remote_node_id;
	uint32_t local_group_id;
	uint32_t remote_group_id;
	uint32_t destination_group_id;
	uint32_t next_group_id;
	uint64_t link_id;
	uint32_t available_bandwidth_kbps;
};

/*
 * Result for one normalized unicast prefix.  The nexthops array is owned by
 * this route.  Available bandwidth is zero (unknown) because the TED snapshot
 * exposes owner-computed canonical cost rather than raw link metrics.
 * Intra-group routes use group_score 0 and an exact local_cost.  Inter-group
 * routes keep the aggregate Group SPF score and exact local egress cost
 * separate; callers must not add them.
 */
struct midr_spf_route {
	struct midr_ted_prefix_key prefix;
	uint64_t generation;
	uint64_t sync_reason_flags;
	enum midr_spf_route_scope scope;
	uint64_t group_score;
	uint64_t local_cost;
	bool reachable;
	bool local_destination;
	struct midr_spf_nexthop *nexthops;
	size_t nexthop_count;
};

/*
 * Compute one prefix from an immutable TED snapshot.  Unreachable is a valid
 * route result, not a function failure.  The caller owns the returned route.
 */
extern int midr_compute_path(const struct midr_ted_snapshot *snapshot,
			     const struct midr_ted_prefix_key *prefix, struct midr_spf_route **out);
extern void midr_spf_route_free(struct midr_spf_route **route);

/*
 * Compute every unique node-prefix and prefix-group key in the snapshot.
 * Result sets are immutable and reference counted.
 */
extern int midr_spf_compute_all(const struct midr_ted_snapshot *snapshot,
				const struct midr_spf_results **out);
extern uint64_t midr_spf_results_generation(const struct midr_spf_results *results);
extern uint64_t midr_spf_results_sync_reason_flags(const struct midr_spf_results *results);
extern size_t midr_spf_results_count(const struct midr_spf_results *results);
extern const struct midr_spf_route *midr_spf_results_at(const struct midr_spf_results *results,
							size_t index);
extern const struct midr_spf_route *
midr_spf_results_lookup(const struct midr_spf_results *results,
			const struct midr_ted_prefix_key *prefix);
extern const struct midr_spf_results *
midr_spf_results_acquire(const struct midr_spf_results *results);
extern void midr_spf_results_release(const struct midr_spf_results **results);

/*
 * Per-MIDR-instance runtime.  It subscribes to TED changes, coalesces updates,
 * and atomically caches the latest complete result set.
 */
extern int midr_spf_context_init(struct midr_context *ctx);
extern void midr_spf_context_finish(struct midr_context *ctx);
extern int midr_spf_results_get(struct midr_context *ctx, const struct midr_spf_results **out);

struct midr_spf_runtime_status {
	uint64_t cached_generation;
	uint64_t pending_generation;
	uint32_t pending_change_flags;
	uint64_t recompute_count;
	int last_error;
	bool recompute_pending;
};

extern int midr_spf_runtime_status_get(struct midr_context *ctx,
				       struct midr_spf_runtime_status *status);

#ifdef __cplusplus
}
#endif

#endif /* _FRR_BGP_MIDR_SPF_H */
