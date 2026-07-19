// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR Control-Plane to Data-Plane Route Installation Interface
 *
 * This is the UNIDIRECTIONAL (CP -> DP) interface through which the
 * control plane (SPF/CSPF engine) delivers path computation results
 * to the data plane for zebra route installation.
 *
 * Design principles:
 *   - CP outputs raw path results (nexthop, weight, SID list)
 *   - CP does NOT know zapi encoding, batching, or debounce details
 *   - DP selects kernel route format based on sid_count:
 *       sid_count=0 -> pure IP (BASIC/UCMP)
 *       sid_count>0 -> SRv6
 *   - DP batches: 100ms window aggregates, diffs, then atomically installs
 */

#ifndef _FRR_BGP_MIDR_ZEBRA_H
#define _FRR_BGP_MIDR_ZEBRA_H

#include <zebra.h>
#include "lib/prefix.h"
#include "lib/zclient.h"
#include "lib/srv6.h"

struct bgp;

/*
 * =========================================================================
 *  Data structures (defined by CP spec, consumed by DP)
 * =========================================================================
 *
 *  design principle:
 *    CP (SPF/CSPF engine) outputs raw computation results.
 *    DP (bgp_midr_zebra.c) maps those results into zapi_route messages.
 *    The midr_path_result is the ONLY CP→DP data carrier – both the
 *    standard-SPF path and the TE CSPF SRv6 path use the same struct.
 *
 *  dual-instance TE model:
 *    instance == 0 (MIDR_INSTANCE_SPF) → standard SPF route
 *    instance == 1 (MIDR_INSTANCE_TE)  → TE SRv6 override route
 *    Both instances coexist in zebra's RIB for the same prefix;
 *    the TE instance uses metric=1 to win rib_choose_best().
 *    Deleting the TE instance automatically restores the SPF instance.
 *
 *  zero-value semantics:
 *    sid_count == 0  →  pure IP forwarding  (BASIC / ECMP / UCMP)
 *    sid_count  > 0  →  SRv6 Segment Routing Header encapsulation
 *    weight   == 0  →  equal-cost ECMP
 *    weight    > 0  →  unequal-cost UCMP (proportional to weight)
 */

/* Dual-instance identifiers */
#define MIDR_INSTANCE_SPF  0
#define MIDR_INSTANCE_TE   1

/* ------------------------------------------------------------------ *
 *  single path entry
 * ------------------------------------------------------------------ */
struct midr_path {
	/*
	 * physical next-hop – the IPv4 / IPv6 address of the directly
	 * connected neighbour that this path should be forwarded to.
	 * for an SRv6 path this is the first-hop router of the SID list.
	 */
	union g_addr	nexthop;

	/*
	 * local egress interface index.
	 * 0 = let zebra resolve the interface via its own RIB.
	 * non-zero = force this specific interface (e.g. for link-local
	 *            IPv6  nexthops where the interface is mandatory).
	 */
	uint32_t	ifindex;

	/*
	 * IGP-style path metric (sum of link metrics along the path).
	 * used by zebra as a tie-breaker when multiple protocol sources
	 * (standard SPF vs TE) install routes for the same prefix.
	 */
	uint32_t	metric;

	/*
	 * bottleneck available bandwidth along this path (Mbps).
	 * this is the minimum ava_bw across all links on the path.
	 * used as the source value for UCMP weight computation.
	 */
	float		path_avail_bw;

	/*
	 * UCMP weight (1-255).  0 = equal-weight ECMP (no UCMP flag).
	 * the CP computes this value based on path_avail_bw or TE metrics;
	 * the DP simply attaches it to the nexthop when non-zero.
	 */
	uint8_t		weight;
};

/* ------------------------------------------------------------------ *
 *  complete path-computation result  (sole CP→DP data carrier)
 * ------------------------------------------------------------------ */
struct midr_path_result {
	/*
	 * array of path entries.  path_count=1 → single-path route;
	 * path_count>1 → ECMP or UCMP route.  caller must keep the
	 * memory alive until midr_zebra_route_add() returns (the DP
	 * deep-copies the data internally).
	 */
	struct midr_path	*paths;
	uint8_t			path_count;

	/*
	 * SRv6 explicit-path segment list.
	 *
	 * sid_count == 0  →  pure IP forwarding (default)
	 * sid_count  > 0  →  the DP encodes a Seg6 encap route via
	 *                     ZAPI_NEXTHOP_FLAG_SEG6.
	 *
	 * the SID list is applied to the FIRST zapi_nexthop only.
	 * SRv6 strict paths are single-path (TE doesn't do ECMP);
	 * if path_count>1 with sid_count>0 the behaviour is
	 * implementation-defined and should be avoided by the CP.
	 */
	struct {
		struct in6_addr	sid_list[SRV6_MAX_SEGS];
		uint8_t		sid_count;
	} explicit;

	/*
	 * dual-instance identifier (MIDR_INSTANCE_SPF or MIDR_INSTANCE_TE).
	 *
	 * instance == MIDR_INSTANCE_SPF (0)  → standard SPF route
	 *         uses zapi_route.instance=0, metric=IGP path metric,
	 *         distance=115.  This is the default for non-SRv6 routes.
	 *
	 * instance == MIDR_INSTANCE_TE  (1)  → TE SRv6 override route
	 *         uses zapi_route.instance=1, metric=1 (always beats SPF),
	 *         distance=115.  When deleted, the SPF route (instance=0)
	 *         is automatically re-promoted by zebra's rib_choose_best().
	 *
	 * zero-initialization (instance=0) defaults to SPF mode.
	 */
	uint8_t		instance;
};

/*
 * =========================================================================
 *  Data-plane public API
 * =========================================================================
 *
 *  batch semantics overview:
 *
 *   midr_zebra_route_add()  ─┐
 *   midr_zebra_route_del()  ─┤  stage operations in per-BGP pending queue
 *                             │  (no ZAPI message sent yet)
 *                             │
 *   midr_zebra_route_update_deferred()  ─┐
 *                                        ├  arm/cancel 100 ms timer
 *   midr_zebra_route_flush()            ─┘
 *                             │
 *                             ▼  timer fires or flush() called
 *                      midr_flush_pending()
 *                             │
 *                             ├── diff against installed-hash
 *                             │    (same → skip / different → DEL old + ADD new)
 *                             │
 *                             └── zclient_route_send() → zebra
 *
 *  design rationale:
 *    topology churn can produce many SPF runs in a short interval.
 *    batching avoids flooding zebra/kernel with intermediate states
 *    and replaces them with a single atomic diff at the end of the
 *    100 ms window.
 */

/*
 * Stage a route add/update.
 *
 * the route is deep-copied and queued for deferred batch processing.
 * the caller may free/reuse the midr_path_result immediately after
 * this call returns.
 *
 * @param bgp     BGP instance
 * @param p       destination prefix
 * @param result  path computation result (paths/weights/SID list)
 */
extern void midr_zebra_route_add(struct bgp *bgp, struct prefix *p,
				 struct midr_path_result *result);

/*
 * Stage a route withdrawal.
 *
 * @param bgp  BGP instance
 * @param p    prefix to be withdrawn
 */
extern void midr_zebra_route_del(struct bgp *bgp, struct prefix *p);

/*
 * Activate the 100 ms deferred-batch timer.
 *
 * if a timer is already running this call is a no-op (coalescing).
 * the caller should invoke this once after a series of add/del calls
 * (typically at the end of an SPF batch).
 *
 * @param bgp  BGP instance
 */
extern void midr_zebra_route_update_deferred(struct bgp *bgp);

/*
 * Flush the pending batch immediately, cancelling any outstanding
 * timer.  use this for emergency scenarios (e.g. local interface
 * down, rapid convergence required) where the 100 ms deferral is
 * unacceptable.
 *
 * @param bgp  BGP instance
 */
extern void midr_zebra_route_flush(struct bgp *bgp);

/*
 * Allocate and initialise the per-BGP-instance DP state.
 * hung off bgp->midr_dp (opaque pointer).
 */
extern void midr_zebra_init(struct bgp *bgp);

/*
 * Tear down DP state: cancel timer, drain pending queue, free
 * installed-hash contents, and NULL out bgp->midr_dp.
 */
extern void midr_zebra_fini(struct bgp *bgp);

#endif /* _FRR_BGP_MIDR_ZEBRA_H */
