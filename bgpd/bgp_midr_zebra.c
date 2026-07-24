// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR Data-Plane Route Installation (bgp_midr_zebra.c)
 *
 * =========================================================================
 *  Purpose
 * =========================================================================
 * This module is the DRIVER that sits between the MIDR control-plane
 * path-computation engines (standard SPF and TE CSPF) and the FRR
 * zebra daemon.  It receives midr_path_result structures from the CP
 * and encodes them as zapi_route / zapi_nexthop messages.
 *
 * =========================================================================
 *  Mode auto-detection (driven purely by midr_path_result fields)
 * =========================================================================
 *   BASIC   : path_count == 1 && sid_count == 0
 *   ECMP    : path_count >  1 && all weights == 0 && sid_count == 0
 *   UCMP    : path_count >  1 && any weight   >  0 && sid_count == 0
 *   SRv6    : sid_count > 0
 *
 * =========================================================================
 *  Batch semantics
 * =========================================================================
 *   midr_zebra_route_add()  ─┐
 *   midr_zebra_route_del()  ─┤  stage operations in per-BGP pending queue
 *                             │  (NO ZAPI message is sent yet)
 *                             │
 *   midr_zebra_route_update_deferred()  ─┐
 *                                        ├  arm/cancel 100 ms timer
 *   midr_zebra_route_flush()            ─┘
 *                             │
 *                             ▼  timer fires or flush() called
 *                      midr_flush_pending()
 *                             │
 *                             ├── iterate pending ops in order
 *                             ├── diff each against installed-hash
 *                             │    (same → skip / different → DEL old + ADD new)
 *                             └── zclient_route_send() → zebra
 *
 *  Why batch?  Topology churn produces many SPF runs in a short
 *  interval.  Batching avoids flooding zebra and the kernel with
 *  intermediate states.  A single atomic diff is emitted at the end
 *  of the 100 ms deferral window.
 *
 * =========================================================================
 *  Dual-Instance TE model (TE overrides SPF without destroying it)
 * =========================================================================
 *  Standard SPF routes use zapi_route.instance=0, metric=IGP_value.
 *  TE SRv6 routes use zapi_route.instance=1, metric=1.
 *
 *  Both entries coexist in zebra's RIB for the SAME prefix.
 *  rib_choose_best() picks the lower metric (1 < IGP) → TE wins.
 *  When the TE route is deleted (admin removes policy / congestion
 *  clears), zebra automatically promotes the SPF entry back to best
 *  and re-installs it in the kernel FIB — no CP re-computation
 *  required.
 */

#include <zebra.h>

#include "lib/frrdistance.h"
#include "lib/jhash.h"
#include "lib/linklist.h"
#include "lib/nexthop.h"
#include "lib/srv6.h"
#include "lib/zclient.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_midr_zebra.h"

/* Only need the zclient pointer from bgp_zebra; avoid pulling in
 * bgp_path_info / BGP_ROUTE_* which are not relevant to the DP. */
extern struct zclient *bgp_zclient;

/*
 * =========================================================================
 *  Per-BGP deferred-batch state  (private to this TU)
 * =========================================================================
 *
 * Every BGP instance has a `bgp->midr_dp` pointer (opaque void*) that
 * points to a `struct bgp_midr_dp`.  This state is allocated in
 * midr_zebra_init() and torn down in midr_zebra_fini().
 *
 * State components:
 *
 *   pending_ops  – FIFO list of midr_pending_op entries.
 *                  add/del calls push here; flush drains.
 *
 *   installed    – prefix-keyed hash of midr_installed_entry.
 *                  tracks what is CURRENTLY installed in zebra.
 *                  used as the diff baseline on each flush.
 *
 *   t_deferred   – 100 ms batch timer event.  coalesces multiple
 *                  update_deferred() calls into one flush.
 *
 *   flush_pending – boolean guard to prevent re-arming the timer.
 */

/*
 * The batch window.  100 ms is chosen because it is long enough to
 * absorb a burst of topology changes (e.g. a link flap causing
 * multiple LSA updates) yet short enough to be negligible for
 * convergence time.
 */
#define MIDR_BATCH_INTERVAL_MS 100

/* Type of a pending operation. */
enum midr_op_type {
	MIDR_OP_ADD,	/* install or update a route */
	MIDR_OP_DEL,	/* withdraw a route */
};

/*
 * A single queued operation.
 *
 * For MIDR_OP_ADD all path/SID fields are deep-copied from the
 * caller's midr_path_result so the CP can free/reuse its buffer
 * immediately after midr_zebra_route_add() returns.
 */
struct midr_pending_op {
	enum midr_op_type	op;
	struct prefix		prefix;
	uint8_t			instance;  /* MIDR_INSTANCE_SPF or MIDR_INSTANCE_TE */

	/* ---- valid only for MIDR_OP_ADD ---- */
	struct midr_path	*paths;
	uint8_t			path_count;
	uint8_t			sid_count;
	struct in6_addr		sid_list[SRV6_MAX_SEGS];
};

/*
 * A snapshot of what is currently installed in zebra for a given
 * prefix.  Used for diff: if an incoming ADD matches what's already
 * installed it is silently dropped (no ZAPI message sent).
 */
struct midr_installed_entry {
	struct prefix		prefix;
	uint8_t			instance;  /* MIDR_INSTANCE_SPF or MIDR_INSTANCE_TE */

	/* copies of the path + SID data that were last sent */
	struct midr_path	*paths;
	uint8_t			path_count;
	uint8_t			sid_count;
	struct in6_addr		sid_list[SRV6_MAX_SEGS];
};

/* Per-BGP DP state (hung off bgp->midr_dp). */
struct bgp_midr_dp {
	struct event		*t_deferred;	/* 100 ms batch timer */
	struct list		*pending_ops;	/* struct midr_pending_op */
	struct hash		*installed;	/* struct midr_installed_entry */

	bool			flush_pending;	/* guard against re-arming */
};

/*
 * =========================================================================
 *  Deep copy / free helpers
 * =========================================================================
 */

/*
 * Duplicate an array of midr_path entries.
 * Returns a freshly-allocated array or NULL if the source is empty.
 */
static struct midr_path *midr_paths_dup(const struct midr_path *src,
					 uint8_t count)
{
	struct midr_path *dst;

	if (!src || !count)
		return NULL;

	dst = XCALLOC(MTYPE_TMP, sizeof(*dst) * count);
	memcpy(dst, src, sizeof(*dst) * count);
	return dst;
}

/*
 * Free a pending_op and its embedded paths array.
 * Registered as the list->del callback.
 */
static void midr_pending_op_free(struct midr_pending_op *op)
{
	XFREE(MTYPE_TMP, op->paths);
	XFREE(MTYPE_TMP, op);
}

/*
 * Hash function for midr_installed_entry — key is the prefix.
 */
static unsigned int midr_prefix_hash_key(const void *data)
{
	const struct midr_installed_entry *e = data;

	return prefix_hash_key(&e->prefix) ^ (e->instance * 31);
}

/*
 * Equality comparison for midr_installed_entry — two entries are
 * considered the same if their prefixes are identical.
 */
static bool midr_prefix_cmp(const void *d1, const void *d2)
{
	const struct midr_installed_entry *e1 = d1;
	const struct midr_installed_entry *e2 = d2;

	return prefix_same(&e1->prefix, &e2->prefix) &&
	       (e1->instance == e2->instance);
}

/*
 * Compare two path-result sets for EQUALITY.
 *
 * Returns true only if the installed entry and the pending op describe
 * IDENTICAL forwarding state (same number of paths, same nexthops,
 * same weights, same SID list).  If they match, no ZAPI message is
 * sent (no-op optimisation).
 *
 * Note: we deliberately do NOT compare metric — metric changes do
 * not affect the forwarding state of a route (they are zebra
 * tie-breakers), so we skip them to avoid unnecessary churn.
 */
static bool midr_path_result_eq(const struct midr_installed_entry *a,
				const struct midr_pending_op *b)
{
	uint8_t i;

	/* Mismatched instance or counts → definitely different */
	if (a->instance != b->instance ||
	    a->path_count != b->path_count ||
	    a->sid_count != b->sid_count)
		return false;

	/* Compare SID lists byte-for-byte */
	for (i = 0; i < a->sid_count; i++) {
		if (!IPV6_ADDR_SAME(&a->sid_list[i], &b->sid_list[i]))
			return false;
	}

	/* Compare each path: nexthop (only relevant AF), ifindex, weight */
	for (i = 0; i < a->path_count; i++) {
		const struct midr_path *pa = &a->paths[i];
		const struct midr_path *pb = &b->paths[i];

		/* Compare only the address family that the prefix uses.
		 * The union g_addr contains both ipv4 and ipv6; comparing both
		 * unconditionally is fragile when the unused field is zeroed.
		 * We check the family stored in the prefix to decide which
		 * address field matters. For IPv6 SID routes, the nexthop is
		 * always IPv6; for pure-IP routes it follows p->family. */
		switch (b->prefix.family) {
		case AF_INET:
			if (!IPV4_ADDR_SAME(&pa->nexthop.ipv4,
					    &pb->nexthop.ipv4))
				return false;
			break;
		case AF_INET6:
			if (!IPV6_ADDR_SAME(&pa->nexthop.ipv6,
					    &pb->nexthop.ipv6))
				return false;
			break;
		default:
			return false; /* unknown family – cannot compare */
		}
		if (pa->ifindex != pb->ifindex)
			return false;
		if (pa->weight != pb->weight)
			return false;
	}

	return true;
}

/*
 * =========================================================================
 *  ZAPI encoding helpers
 * =========================================================================
 */

/*
 * Populate a single zapi_nexthop from a midr_path entry.
 *
 * Maps AF_INET  → NEXTHOP_TYPE_IPV4[ _IFINDEX ]
 *       AF_INET6 → NEXTHOP_TYPE_IPV6[ _IFINDEX ]
 *
 * Returns true if the nexthop address is valid (non-zero),
 * false otherwise (the caller should skip this nexthop).
 */
static bool midr_path_fill_zapi_nh(const struct prefix *p,
				   const struct midr_path *mpath,
				   struct zapi_nexthop *znh,
				   vrf_id_t vrf_id)
{
	zapi_nexthop_init(znh);
	znh->vrf_id = vrf_id;

	switch (p->family) {
	case AF_INET:
		znh->type = NEXTHOP_TYPE_IPV4;
		znh->gate.ipv4 = mpath->nexthop.ipv4;
		if (mpath->ifindex != IFINDEX_INTERNAL) {
			znh->type = NEXTHOP_TYPE_IPV4_IFINDEX;
			znh->ifindex = mpath->ifindex;
		}
		return mpath->nexthop.ipv4.s_addr != INADDR_ANY;

	case AF_INET6:
		znh->type = NEXTHOP_TYPE_IPV6;
		znh->gate.ipv6 = mpath->nexthop.ipv6;
		if (mpath->ifindex != IFINDEX_INTERNAL) {
			znh->type = NEXTHOP_TYPE_IPV6_IFINDEX;
			znh->ifindex = mpath->ifindex;
		}
		return !IN6_IS_ADDR_UNSPECIFIED(&mpath->nexthop.ipv6);

	default:
		return false;
	}
}

/*
 * Encode a full midr_path_result into a zapi_route.
 *
 * This function performs MODE SELECTION implicitly based on the data:
 *
 *   sid_count == 0  →  pure IP  (BASIC / ECMP / UCMP)
 *     • path_count=1            → single-nexthop
 *     • path_count>1, weight=0  → ECMP (equal-cost multipath)
 *     • path_count>1, weight>0  → UCMP (unequal-cost, ZAPI_NEXTHOP_FLAG_WEIGHT)
 *
 *   sid_count  > 0  →  SRv6 (Segment Routing over IPv6)
 *     • SID list is applied to the FIRST nexthop only
 *     • ZAPI_NEXTHOP_FLAG_SEG6 is set
 *     • encap behaviour = SRV6_HEADEND_BEHAVIOR_H_INSERT (inline)
 *     • SRv6 paths are single-path; CP should not set path_count>1 with sid_count>0
 *
 * Returns 0 on success, -1 if no valid nexthop could be constructed.
 */
static int midr_result_to_zapi(struct bgp *bgp, const struct prefix *p,
			       const struct midr_path *paths,
			       uint8_t path_count, uint8_t sid_count,
			       const struct in6_addr *sid_list,
			       uint8_t instance, struct zapi_route *api)
{
	uint8_t i;
	bool any_weight = false;

	zapi_route_init(api);
	api->vrf_id = bgp->vrf_id;

	/*
	 * Use the dedicated ZEBRA_ROUTE_BGP_MIDR type so zebra can
	 * distinguish MIDR routes from ordinary BGP NLRI routes.
	 * This prevents SPF-computed routes and BGP-learned routes
	 * from overwriting each other in the RIB.
	 */
	api->type = ZEBRA_ROUTE_BGP_MIDR;

	/*
	 * instance field enables the dual-instance TE model:
	 *   - Standard SPF uses 0.
	 *   - TE CSPF uses 1 with metric=1 to override.
	 * See the file-header comment for details.
	 */
	api->instance = instance;
	api->safi = SAFI_UNICAST;
	api->prefix = *p;

	/*
	 * Dual-instance metric strategy:
	 *   instance==MIDR_INSTANCE_SPF (0) → metric = first-path metric
	 *   instance==MIDR_INSTANCE_TE  (1) → metric = 1
	 *   This ensures TE always wins rib_choose_best() (1 < IGP metric).
	 */
	SET_FLAG(api->message, ZAPI_MESSAGE_METRIC);
	if (instance == MIDR_INSTANCE_TE)
		api->metric = 1;
	else
		api->metric = (path_count > 0) ? paths[0].metric : 0;

	/*
	 * Administrative distance: 115 (same as IS-IS).
	 *   - BGP EBGP = 20 (lower – preferred in standard BGP)
	 *   - IS-IS = 115
	 *   - MIDR = 115 (SPF-computed link-state routes, not BGP)
	 *   - BGP IBGP = 200 (higher – less preferred)
	 * This ensures regular EBGP routes are preferred over MIDR
	 * when both are present for the same prefix.
	 */
	SET_FLAG(api->message, ZAPI_MESSAGE_DISTANCE);
	api->distance = ZEBRA_BGP_MIDR_DISTANCE_DEFAULT;

	SET_FLAG(api->message, ZAPI_MESSAGE_NEXTHOP);

	/*
	 * Fill nexthops.  Iterate up to path_count or MULTIPATH_NUM
	 * (whichever is smaller – FRR's zapi_route has a fixed-size
	 * nexthop array).
	 */
	for (i = 0; i < path_count && api->nexthop_num < MULTIPATH_NUM; i++) {
		struct zapi_nexthop *znh = &api->nexthops[api->nexthop_num];

		/* Skip invalid nexthops (e.g. zero address). */
		if (!midr_path_fill_zapi_nh(p, &paths[i], znh, bgp->vrf_id))
			continue;

		/*
		 * UCMP weight.
		 * weight==0 → equal-cost (no flag) – kernel does per-flow hash.
		 * weight>0  → set ZAPI_NEXTHOP_FLAG_WEIGHT so zebra/nhg
		 *             knows to apply proportional weighting.
		 */
		if (paths[i].weight > 0) {
			znh->weight = paths[i].weight;
			SET_FLAG(znh->flags, ZAPI_NEXTHOP_FLAG_WEIGHT);
			any_weight = true;
		}

		/*
		 * SRv6 headend behaviour.
		 *
		 * Applied to the FIRST nexthop ONLY.  All paths in an
		 * SRv6 result share the same SID list; the physical
		 * nexthop address is the first-hop router of the SID
		 * chain.
		 *
		 * Encapsulation mode: H.Insert (inline).
		 * This means the kernel inserts an SRH between the
		 * existing IP header and the payload.  No new outer
		 * IPv6 header is created (that would be H.Encaps).
		 * H.Insert is sufficient for intra-domain SRv6 where
		 * the IPv6 DA is already routable.
		 */
		if (sid_count > 0 && api->nexthop_num == 0) {
			SET_FLAG(znh->flags, ZAPI_NEXTHOP_FLAG_SEG6);
			znh->seg_num = sid_count;
			memcpy(znh->seg6_segs, sid_list,
			       sizeof(struct in6_addr) * sid_count);
			znh->srv6_encap_behavior =
				SRV6_HEADEND_BEHAVIOR_H_INSERT;
		}

		api->nexthop_num++;
	}

	/* If every nexthop was invalid, fail. */
	if (!api->nexthop_num)
		return -1;

	/*
	 * ZEBRA_FLAG_USE_RECURSIVE_WEIGHT tells zebra that the
	 * weight should survive recursive nexthop resolution.
	 * Without this flag, zebra discards weight after resolving
	 * the nexthop through the RIB, defeating UCMP.
	 */
	if (any_weight)
		SET_FLAG(api->flags, ZEBRA_FLAG_USE_RECURSIVE_WEIGHT);

	return 0;
}

/*
 * Send a single route ADD or DEL to zebra via the shared zclient.
 *
 * bgp_zclient is the global singleton zclient instance for bgpd.
 * MIDR shares this client with standard BGP (bgp_zebra.c); no
 * separate connection to zebra is needed.
 *
 * Returns the zclient send status (ZCLIENT_SEND_SUCCESS / FAILURE).
 */
static enum zclient_send_status
midr_zapi_send(struct bgp *bgp, struct zapi_route *api,
	       uint8_t cmd)
{
	return zclient_route_send(cmd, bgp_zclient, api);
}

/*
 * =========================================================================
 *  Diff & commit
 * =========================================================================
 *
 * The diff mechanism ensures we never send redundant ZAPI messages:
 * if a newly-computed route is identical to what's already installed,
 * we skip it.  This significantly reduces zebra/kernel churn during
 * SPF recomputation where most routes do not change.
 *
 * The installed-hash uses struct midr_installed_entry keyed by prefix.
 */

/*
 * Look up an installed entry by prefix.
 * Returns NULL if no route for this prefix has been installed.
 */
static struct midr_installed_entry *
midr_installed_lookup(struct hash *h, const struct prefix *p, uint8_t instance)
{
	struct midr_installed_entry key = {
		.prefix = *p,
		.instance = instance,
	};

	return hash_lookup(h, &key);
}

/*
 * Record a route as installed.  If an entry for the same prefix
 * already exists, it is replaced (DEL old implied).  Data is
 * deep-copied so the caller retains ownership.
 */
static void midr_installed_set(struct hash *h, const struct prefix *p,
			       const struct midr_path *paths,
			       uint8_t path_count, uint8_t sid_count,
			       const struct in6_addr *sid_list,
			       uint8_t instance)
{
	struct midr_installed_entry *e;

	/* Remove and free any existing entry for this (prefix, instance). */
	e = midr_installed_lookup(h, p, instance);
	if (e) {
		hash_release(h, e);
		XFREE(MTYPE_TMP, e->paths);
		XFREE(MTYPE_TMP, e);
	}

	e = XCALLOC(MTYPE_TMP, sizeof(*e));
	prefix_copy(&e->prefix, p);
	e->instance = instance;
	e->path_count = path_count;
	e->sid_count = sid_count;
	memcpy(e->sid_list, sid_list, sizeof(*sid_list) * sid_count);
	e->paths = midr_paths_dup(paths, path_count);

	hash_get(h, e, hash_alloc_intern);
}

/*
 * Remove a route from the installed hash and free its memory.
 */
static void midr_installed_unset(struct hash *h, const struct prefix *p,
				 uint8_t instance)
{
	struct midr_installed_entry *e;

	e = midr_installed_lookup(h, p, instance);
	if (!e)
		return;

	hash_release(h, e);
	XFREE(MTYPE_TMP, e->paths);
	XFREE(MTYPE_TMP, e);
}

/*
 * Process ONE pending operation against the installed-hash.
 *
 * ADD logic:
 *   1. Look up the prefix in the installed hash.
 *   2. If already installed AND identical → no-op (return early).
 *      This is the key optimisation: identical routes are not
 *      re-sent to zebra.
 *   3. If installed but DIFFERENT → send DEL for the old route,
 *      then ADD for the new one.
 *   4. If not installed → send ADD.
 *   5. Update the installed hash with the new data.
 *
 * DEL logic:
 *   1. Look up the prefix in the installed hash.
 *   2. If not installed → no-op (nothing to delete).
 *   3. If installed → send DEL, remove from hash.
 *
 * For DEL operations, a zapi_route with only the prefix filled in
 * is sufficient — zebra deletes routes by (prefix, type, instance,
 * table) match and does not require nexthops on deletion.
 */
static void midr_process_one_op(struct bgp *bgp, struct bgp_midr_dp *dp,
				struct midr_pending_op *op)
{
	struct midr_installed_entry *installed;
	struct zapi_route api;

	switch (op->op) {
	case MIDR_OP_ADD:
		installed = midr_installed_lookup(dp->installed, &op->prefix,
						  op->instance);

		/* No-op: the route hasn't changed. */
		if (installed && midr_path_result_eq(installed, op))
			return;

		/* Route changed → delete old version first. */
		if (installed) {
			zapi_route_init(&api);
			api.vrf_id = bgp->vrf_id;
			api.type = ZEBRA_ROUTE_BGP_MIDR;
			api.instance = installed->instance;
			api.safi = SAFI_UNICAST;
			api.prefix = op->prefix;

			midr_zapi_send(bgp, &api, ZEBRA_ROUTE_DELETE);
		}

		/* Install the new route. */
		if (midr_result_to_zapi(bgp, &op->prefix,
					op->paths, op->path_count,
					op->sid_count, op->sid_list,
					op->instance, &api) == 0)
			midr_zapi_send(bgp, &api, ZEBRA_ROUTE_ADD);

		/* Update installed-state snapshot. */
		midr_installed_set(dp->installed, &op->prefix,
				   op->paths, op->path_count,
				   op->sid_count, op->sid_list,
				   op->instance);
		break;

	case MIDR_OP_DEL:
		installed = midr_installed_lookup(dp->installed, &op->prefix,
						  op->instance);
		if (!installed)
			return; /* already absent — no-op */

		zapi_route_init(&api);
		api.vrf_id = bgp->vrf_id;
		api.type = ZEBRA_ROUTE_BGP_MIDR;
		api.instance = op->instance;
		api.safi = SAFI_UNICAST;
		api.prefix = op->prefix;

		midr_zapi_send(bgp, &api, ZEBRA_ROUTE_DELETE);

		midr_installed_unset(dp->installed, &op->prefix, op->instance);
		break;
	}
}

/*
 * Drain the entire pending-ops queue.
 *
 * Operations are processed in FIFO order (the order they were
 * queued).  After processing, each op is freed and removed from
 * the list.
 *
 * This function is called from TWO contexts:
 *   1. The 100 ms deferred timer callback (normal path).
 *   2. midr_zebra_route_flush() (emergency path — no delay).
 *      The caller is responsible for cancelling the timer before
 *      calling this function directly.
 */
static void midr_flush_pending(struct bgp *bgp, struct bgp_midr_dp *dp)
{
	struct listnode *node, *nnode;
	struct midr_pending_op *op;

	for (ALL_LIST_ELEMENTS(dp->pending_ops, node, nnode, op)) {
		midr_process_one_op(bgp, dp, op);
		list_delete_node(dp->pending_ops, node);
		midr_pending_op_free(op);
	}

	dp->flush_pending = false;
}

/*
 * =========================================================================
 *  Deferred-batch timer callback
 * =========================================================================
 *
 * Called by the FRR event loop 100 ms after
 * midr_zebra_route_update_deferred() was invoked.
 *
 * The timer is a ONE-SHOT: it fires once and NULL-s out t_deferred.
 * Multiple calls to _update_deferred() during the 100 ms window are
 * coalesced (the flush_pending guard prevents re-arming).
 */
static void midr_flush_timer_cb(struct event *t)
{
	struct bgp *bgp = EVENT_ARG(t);
	struct bgp_midr_dp *dp = bgp->midr_dp;

	dp->t_deferred = NULL;
	dp->flush_pending = false;

	if (listcount(dp->pending_ops))
		midr_flush_pending(bgp, dp);
}

/*
 * =========================================================================
 *  Public API  (see bgp_midr_zebra.h for full Doxygen-style docs)
 * =========================================================================
 */

void midr_zebra_init(struct bgp *bgp)
{
	struct bgp_midr_dp *dp;

	assert(bgp);

	dp = XCALLOC(MTYPE_TMP, sizeof(*dp));

	/*
	 * pending_ops is a linked list with a custom free-function.
	 * When an element is deleted from the list, midr_pending_op_free
	 * is called automatically to release its paths[] array.
	 */
	dp->pending_ops = list_new();
	dp->pending_ops->del = (void (*)(void *))midr_pending_op_free;

	/*
	 * installed hash uses prefix_hash_key / midr_prefix_cmp as
	 * the hash/compare functions.  Entries are interned (managed
	 * by the hash itself — hash_alloc_intern).
	 */
	dp->installed = hash_create(midr_prefix_hash_key, midr_prefix_cmp,
				    "MIDR installed routes");

	bgp->midr_dp = dp;
}

void midr_zebra_fini(struct bgp *bgp)
{
	struct bgp_midr_dp *dp;

	if (!bgp || !bgp->midr_dp)
		return;

	dp = bgp->midr_dp;

	/* Cancel outstanding timer (if any). */
	event_cancel(&dp->t_deferred);

	/* Flush any remaining pending ops before shutdown. */
	midr_flush_pending(bgp, dp);

	/*
	 * Free all installed entries first, then clean the hash.
	 * We use hash_iterate to walk entries safely.
	 */
	{
		struct hash_bucket *hb;
		unsigned int i;

		for (i = 0; i < dp->installed->size; i++) {
			for (hb = dp->installed->index[i]; hb;
			     hb = hb->next) {
				struct midr_installed_entry *e = hb->data;

				if (e) {
					XFREE(MTYPE_TMP, e->paths);
					XFREE(MTYPE_TMP, e);
				}
			}
		}
	}
	hash_clean_and_free(&dp->installed, NULL);

	list_delete(&dp->pending_ops);
	XFREE(MTYPE_TMP, dp);
	bgp->midr_dp = NULL;
}

void midr_zebra_route_add(struct bgp *bgp, struct prefix *p,
			  struct midr_path_result *result)
{
	struct bgp_midr_dp *dp;
	struct midr_pending_op *op;

	assert(bgp && bgp->midr_dp);
	dp = bgp->midr_dp;

	/* Sanity checks. */
	if (!p || !result || !result->paths || !result->path_count)
		return;

	op = XCALLOC(MTYPE_TMP, sizeof(*op));
	op->op = MIDR_OP_ADD;
	prefix_copy(&op->prefix, p);

	/*
	 * Deep-copy the path array.  The CP may reuse or free its
	 * midr_path_result immediately after this call returns.
	 */
	op->path_count = result->path_count;
	op->paths = midr_paths_dup(result->paths, result->path_count);

	/* Copy instance (SPF vs TE) */
	op->instance = result->instance;

	/* Copy SID list if present. */
	op->sid_count = result->explicit.sid_count;
	if (op->sid_count)
		memcpy(op->sid_list, result->explicit.sid_list,
		       sizeof(result->explicit.sid_list[0]) * op->sid_count);

	listnode_add(dp->pending_ops, op);
}

void midr_zebra_route_del(struct bgp *bgp, struct prefix *p, uint8_t instance)
{
	struct bgp_midr_dp *dp;
	struct midr_pending_op *op;

	assert(bgp && bgp->midr_dp);
	dp = bgp->midr_dp;

	if (!p)
		return;

	op = XCALLOC(MTYPE_TMP, sizeof(*op));
	op->op = MIDR_OP_DEL;
	op->instance = instance;
	prefix_copy(&op->prefix, p);

	listnode_add(dp->pending_ops, op);
}

void midr_zebra_route_update_deferred(struct bgp *bgp)
{
	struct bgp_midr_dp *dp;

	assert(bgp && bgp->midr_dp);
	dp = bgp->midr_dp;

	/*
	 * Coalescing guard: if a timer is already armed, this call
	 * is a no-op.  Any add/del calls since the timer was armed
	 * are already in pending_ops and will be flushed together.
	 */
	if (dp->flush_pending)
		return;

	dp->flush_pending = true;

	/*
	 * event_add_timer_msec creates a ONE-SHOT timer.
	 * MIDR_BATCH_INTERVAL_MS = 100 ms.
	 * bm->master is the bgpd event loop; bgp is passed as the
	 * callback argument via EVENT_ARG.
	 */
	event_add_timer_msec(bm->master, midr_flush_timer_cb, bgp,
			     MIDR_BATCH_INTERVAL_MS, &dp->t_deferred);
}

void midr_zebra_route_flush(struct bgp *bgp)
{
	struct bgp_midr_dp *dp;

	assert(bgp && bgp->midr_dp);
	dp = bgp->midr_dp;

	/*
	 * Cancel the deferred timer (if running) and flush all
	 * pending operations immediately.  Used in emergency
	 * scenarios (e.g. local interface down) where 100 ms of
	 * additional delay is unacceptable.
	 */
	event_cancel(&dp->t_deferred);
	dp->flush_pending = false;

	if (listcount(dp->pending_ops))
		midr_flush_pending(bgp, dp);
}