/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MIDR data-plane backend (third group).
 *
 * Migrated from the former bgpd implementation (bgpd/bgp_midr_zebra.c) with
 * the host parameter changed from the former BGP context type to
 * struct midr_context and the completion rule hardened: the installed-route
 * hash is only updated after zebra accepted the corresponding ZAPI message, so
 * a failed send never pretends a route is installed.
 *
 * Mode selection stays driven purely by the midr_path_result fields:
 *   BASIC : path_count == 1 && sid_count == 0
 *   ECMP  : path_count >  1 && all weights == 0 && sid_count == 0
 *   UCMP  : path_count >  1 && any weight   >  0 && sid_count == 0
 *   SRv6  : sid_count > 0
 *
 * Dual instance: ZEBRA_ROUTE_BGP_MIDR instance 0 is the SPF route and
 * instance 1 the TE override.  Both coexist in zebra's RIB for the same
 * prefix; the TE entry uses metric 1 so it wins rib_choose_best().
 *
 * Deferred-batch failure recovery is two-step.  A deferred batch can fail
 * after update_deferred() already returned success, so the adapter never saw
 * the error and has already advanced its desired generation.  The operations
 * left in the queue are exactly the un-applied remainder of the diff between
 * the last accepted result and the current one; they are therefore RETAINED
 * (marked as recovery operations) instead of being dropped.  dp_recovery_cb()
 * (1) retries that retained batch and only once it was applied calls (2)
 * midr_spf_install_resync() as the spec-visible reconciliation, which is a
 * no-op when the adapter already reached the desired generation.
 *
 * Pending operations are never cleared while the adapter's desired generation
 * is ahead of the installed hash: dropping them would leave the FIB missing
 * operations the adapter will never re-generate, a permanent divergence.  The
 * retained batch is itself the replay that re-creates every missing install,
 * so it is the only safe thing to keep.  abort_pending() still discards the
 * operations staged by the current round, preserving the backend-rejection
 * contract that the previous desired generation is kept; it deliberately
 * keeps retained recovery operations, which belong to an earlier round whose
 * desired generation the adapter already committed.
 *
 * Retries are bounded (MIDR_DP_RECOVERY_MAX_ATTEMPTS, 100 ms apart); if zebra
 * is unreachable the recovery returns without rescheduling and leaves
 * convergence to the zebra-reconnect path (clear installed hash +
 * midr_spf_install_replay()).
 */

#include <zebra.h>

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "frrdistance.h"
#include "frrevent.h"
#include "hash.h"
#include "if.h"
#include "linklist.h"
#include "log.h"
#include "libfrr.h"
#include "nexthop.h"
#include "privs.h"
#include "srv6.h"
#include "zclient.h"

#include "midr-context.h"
#include "midr-dp-backend.h"
#include "midr-spf-install.h"
#include "midr-zebra.h"

/* Batch window: long enough to absorb a topology-change burst, short enough
 * to be negligible for convergence. */
#define MIDR_BATCH_INTERVAL_MS 100

/* Send-failure recovery: fast retries for the first
 * MIDR_DP_RECOVERY_MAX_ATTEMPTS attempts, then a slow heartbeat so a socket
 * that stays up but keeps failing sends still re-arms recovery instead of
 * stalling forever. */
#define MIDR_DP_RECOVERY_MS 100
#define MIDR_DP_RECOVERY_MAX_ATTEMPTS 50
#define MIDR_DP_RECOVERY_HEARTBEAT_MS 2000

/*
 * Recovery state machine (review 2.3).  A deferred batch that fails leaves the
 * adapter's desired generation ahead of the FIB, so the retained batch (see the
 * file header) must be driven to convergence.  The state is explicit and is
 * re-armed by a timer, a connection-state change or fresh adapter work, never
 * by a reconnect alone:
 *
 *   IDLE    - nothing to recover.
 *   ACTIVE  - fast retries, MIDR_DP_RECOVERY_MS apart, below the attempt cap.
 *   STALLED - attempt cap reached or zebra unreachable: a slow heartbeat,
 *             MIDR_DP_RECOVERY_HEARTBEAT_MS apart, keeps re-arming so the
 *             batch is retried until it succeeds, a reconnect happens or a
 *             new desired generation re-kicks recovery back to ACTIVE.  The
 *             heartbeat is slow on purpose: recovery must not spin at 100 ms
 *             forever.
 */
enum midr_dp_recovery_state {
	MIDR_DP_RECOVERY_IDLE = 0,
	MIDR_DP_RECOVERY_ACTIVE,
	MIDR_DP_RECOVERY_STALLED,
};

/* Keep the historical distance so regular EBGP routes stay preferred over
 * MIDR routes. */
#define MIDR_DP_DISTANCE ZEBRA_BGP_MIDR_DISTANCE_DEFAULT

enum midr_op_type {
	MIDR_OP_ADD,
	MIDR_OP_DEL,
};

/* A queued operation.  For MIDR_OP_ADD the paths and SID list are deep-copied
 * so the producer may free its result as soon as route_add() returns.
 *
 * @recovery marks an operation retained from a deferred batch that already
 * failed: the adapter has advanced its desired generation, so this operation
 * is part of the only replay that can re-create the missing install and must
 * survive abort_pending() of a later round. */
struct midr_pending_op {
	enum midr_op_type op;
	struct prefix prefix;
	uint8_t instance;
	struct midr_path *paths;
	uint8_t path_count;
	uint8_t sid_count;
	struct in6_addr sid_list[MIDR_SRV6_MAX_SEGS];
	bool recovery;
};

/* Last state accepted by zebra for one (prefix, instance) identity. */
struct midr_installed_entry {
	struct prefix prefix;
	uint8_t instance;
	struct midr_path *paths;
	uint8_t path_count;
	uint8_t sid_count;
	struct in6_addr sid_list[MIDR_SRV6_MAX_SEGS];
};

struct midr_dp_backend {
	struct midr_context *ctx;
	struct event_loop *master;
	struct zclient *zc;
	vrf_id_t vrf_id;

	struct event *t_deferred;
	struct list *pending_ops;
	struct hash *installed;
	struct event *t_recovery;
	enum midr_dp_recovery_state recovery_state;
	uint32_t recovery_attempts;

	bool flush_pending;
	bool zebra_up;

	/* Set on the cold path (daemon start and every zebra reconnect): the
	 * next batch is submitted at once instead of waiting for the 100 ms
	 * window, so the first route does not pay the batching delay.  The
	 * steady-state path keeps the deferred window that absorbs topology
	 * churn. */
	bool immediate_next;

	/* Diagnostics. */
	uint64_t add_staged;
	uint64_t del_staged;
	uint64_t batches;
	uint64_t route_adds;
	uint64_t route_deletes;
	uint64_t send_failures;
	uint64_t replays;
	uint64_t resyncs;
	int last_error;
};

/* midrd runs a single MIDR runtime per process; the zclient callbacks only
 * receive the zclient, so keep the owning backend reachable. */
static struct midr_dp_backend *midr_dp_current;

/* Zero-initialised privileges: midrd runs with FRR_NO_PRIVSEP and never
 * changes uid/gid, which matches the former bgpd test harness setup. */
static struct zebra_privs_t midr_dp_privs;

/*
 * =========================================================================
 *  Deep copy / hash helpers
 * =========================================================================
 */

static struct midr_path *dp_paths_dup(const struct midr_path *src,
				      uint8_t count)
{
	struct midr_path *dst;

	if (!src || !count)
		return NULL;
	dst = calloc(count, sizeof(*dst));
	if (!dst)
		return NULL;
	memcpy(dst, src, sizeof(*dst) * count);
	return dst;
}

static void dp_pending_op_free(struct midr_pending_op *op)
{
	if (!op)
		return;
	free(op->paths);
	free(op);
}

static unsigned int dp_prefix_hash_key(const void *data)
{
	const struct midr_installed_entry *e = data;

	return prefix_hash_key(&e->prefix) ^ (e->instance * 31U);
}

static bool dp_prefix_cmp(const void *d1, const void *d2)
{
	const struct midr_installed_entry *e1 = d1;
	const struct midr_installed_entry *e2 = d2;

	return prefix_same(&e1->prefix, &e2->prefix) &&
	       e1->instance == e2->instance;
}

static struct midr_installed_entry *
dp_installed_lookup(struct hash *h, const struct prefix *p, uint8_t instance)
{
	struct midr_installed_entry key = {
		.instance = instance,
	};

	prefix_copy(&key.prefix, p);
	return hash_lookup(h, &key);
}

/* Test-only fault injection for the installed-hash error path (see
 * midr_dp_backend_test_fail_installed()). */
static bool dp_test_fail_installed;

/*
 * Record the state zebra accepted for one (prefix, instance).  This runs only
 * after dp_zapi_send() succeeded, so a failure here must not leave a
 * half-built entry in the hash: the whole entry is built first and published
 * only once every allocation succeeded.  The caller propagates the error and
 * keeps the operation queued (see dp_process_one_op()), so recovery re-sends
 * it and converges once the allocation succeeds -- the same retained-batch
 * semantic as a deferred send failure.
 */
static int dp_installed_set(struct hash *h, const struct prefix *p,
			    const struct midr_path *paths, uint8_t path_count,
			    uint8_t sid_count,
			    const struct in6_addr *sid_list, uint8_t instance)
{
	struct midr_installed_entry *e;
	struct midr_installed_entry *existing;

	e = calloc(1, sizeof(*e));
	if (!e)
		return -ENOMEM;

	prefix_copy(&e->prefix, p);
	e->instance = instance;
	e->path_count = path_count;
	e->sid_count = sid_count;
	if (sid_count)
		memcpy(e->sid_list, sid_list, sizeof(e->sid_list[0]) * sid_count);

	if (path_count) {
		e->paths = dp_paths_dup(paths, path_count);
		if (!e->paths) {
			free(e);
			return -ENOMEM;
		}
	}

	/* Simulate a hash insert failure so the partial-free path is testable. */
	existing = dp_test_fail_installed ? NULL
					  : hash_get(h, e, hash_alloc_intern);
	dp_test_fail_installed = false;
	if (!existing) {
		free(e->paths);
		free(e);
		return -ENOMEM;
	}

	if (existing != e) {
		/* The key was already present: replace its payload in place and
		 * drop the duplicate.  The old payload is never released before
		 * the new one is complete, so a failure above leaves it intact. */
		free(existing->paths);
		existing->paths = e->paths;
		existing->path_count = e->path_count;
		existing->sid_count = e->sid_count;
		memcpy(existing->sid_list, e->sid_list,
		       sizeof(existing->sid_list));
		free(e);
	}
	return 0;
}

/* Fault injection hook used by dp-backend-test.c; not for production use. */
void midr_dp_backend_test_fail_installed(bool enable)
{
	dp_test_fail_installed = enable;
}

/* Cold-path batch submit performed right after the backend registers; defined
 * with the submit/recovery machinery further down. */
static void dp_cold_submit(struct midr_dp_backend *b);

/*
 * Test-only entry point for the cold-path submit that
 * midr_dp_backend_start() performs after the registration-time
 * reconciliation.  In production that path needs the zclient to be connected
 * while a batch is staged, which the start sequence cannot produce today, so
 * the failure handling (retain + arm recovery) is exercised through this hook.
 */
void midr_dp_backend_test_cold_submit(void)
{
	if (midr_dp_current)
		dp_cold_submit(midr_dp_current);
}

static void dp_installed_unset(struct hash *h, const struct prefix *p,
			       uint8_t instance)
{
	struct midr_installed_entry *e = dp_installed_lookup(h, p, instance);

	if (!e)
		return;
	hash_release(h, e);
	free(e->paths);
	free(e);
}

/* Free one installed entry (hash_clean callback). */
static void dp_installed_entry_free(void *data)
{
	struct midr_installed_entry *e = data;

	if (!e)
		return;
	free(e->paths);
	free(e);
}

/* Drop every entry without touching zebra: used after a zebra reconnect,
 * where the previous client state is gone and a replay rebuilds it. */
static void dp_installed_clear(struct midr_dp_backend *b)
{
	if (b->installed)
		hash_clean(b->installed, dp_installed_entry_free);
}

static size_t dp_installed_count(const struct midr_dp_backend *b)
{
	return b->installed ? hashcount(b->installed) : 0;
}

/*
 * =========================================================================
 *  Forwarding-state comparison
 * =========================================================================
 *
 * Two path sets are equal when they describe identical forwarding state:
 * same nexthop (per the tagged address family), ifindex, weight and SID
 * list.  Metric is deliberately excluded because it is only a zebra
 * tie-breaker; the SPF adapter already turns a metric-only change into an
 * explicit DELETE + ADD.
 */
static bool dp_path_same(const struct midr_path *a, const struct midr_path *b)
{
	if (a->ifindex != b->ifindex || a->weight != b->weight ||
	    a->nexthop.ipa_type != b->nexthop.ipa_type)
		return false;

	switch (a->nexthop.ipa_type) {
	case IPADDR_V4:
		return IPV4_ADDR_SAME(&a->nexthop.ipaddr_v4,
				      &b->nexthop.ipaddr_v4);
	case IPADDR_V6:
		return IPV6_ADDR_SAME(&a->nexthop.ipaddr_v6,
				      &b->nexthop.ipaddr_v6);
	case IPADDR_NONE:
	default:
		return false;
	}
}

static bool dp_result_same(const struct midr_installed_entry *a,
			   const struct midr_pending_op *b)
{
	uint8_t i;

	if (a->instance != b->instance || a->path_count != b->path_count ||
	    a->sid_count != b->sid_count)
		return false;

	for (i = 0; i < a->sid_count; i++)
		if (!IPV6_ADDR_SAME(&a->sid_list[i], &b->sid_list[i]))
			return false;

	for (i = 0; i < a->path_count; i++)
		if (!dp_path_same(&a->paths[i], &b->paths[i]))
			return false;

	return true;
}

/*
 * =========================================================================
 *  ZAPI encoding
 * =========================================================================
 */

/*
 * Populate one zapi_nexthop from a midr_path.
 *
 * The nexthop address family comes from the tagged struct ipaddr, never from
 * the destination prefix.  ifindex 0 (IFINDEX_INTERNAL) lets zebra resolve
 * the egress interface recursively, which is the normal case for MIDR
 * transport locators; link-local IPv6 nexthops must carry an explicit index.
 *
 * Returns true when the nexthop is usable.
 */
static bool dp_path_fill_zapi_nh(vrf_id_t vrf_id, const struct midr_path *mpath,
				 struct zapi_nexthop *znh)
{
	zapi_nexthop_init(znh);
	znh->vrf_id = vrf_id;

	switch (mpath->nexthop.ipa_type) {
	case IPADDR_V4:
		znh->type = NEXTHOP_TYPE_IPV4;
		znh->gate.ipv4 = mpath->nexthop.ipaddr_v4;
		if (mpath->ifindex != IFINDEX_INTERNAL) {
			znh->type = NEXTHOP_TYPE_IPV4_IFINDEX;
			znh->ifindex = mpath->ifindex;
		}
		return znh->gate.ipv4.s_addr != INADDR_ANY;

	case IPADDR_V6:
		if (IN6_IS_ADDR_LINKLOCAL(&mpath->nexthop.ipaddr_v6) &&
		    mpath->ifindex == IFINDEX_INTERNAL)
			return false;
		znh->type = NEXTHOP_TYPE_IPV6;
		znh->gate.ipv6 = mpath->nexthop.ipaddr_v6;
		if (mpath->ifindex != IFINDEX_INTERNAL) {
			znh->type = NEXTHOP_TYPE_IPV6_IFINDEX;
			znh->ifindex = mpath->ifindex;
		}
		return !IN6_IS_ADDR_UNSPECIFIED(&znh->gate.ipv6);

	case IPADDR_NONE:
	default:
		return false;
	}
}

/*
 * Encode a full path result into a zapi_route.
 *
 * Dual-instance metric strategy: instance SPF uses the first path metric, the
 * TE override uses metric 1 so that it always wins rib_choose_best().
 *
 * Returns 0 on success, a negative errno when the result cannot be encoded.
 */
static int dp_result_to_zapi(struct midr_dp_backend *b, const struct prefix *p,
			     const struct midr_path *paths, uint8_t path_count,
			     uint8_t sid_count, const struct in6_addr *sid_list,
			     uint8_t instance, struct zapi_route *api)
{
	uint8_t i;
	bool any_weight = false;

	if (!paths || !path_count)
		return -EINVAL;
	if (instance != MIDR_INSTANCE_SPF && instance != MIDR_INSTANCE_TE)
		return -EINVAL;
	if (sid_count > MIDR_SRV6_MAX_SEGS)
		return -E2BIG;
	if (p->family != AF_INET && p->family != AF_INET6)
		return -EAFNOSUPPORT;

	zapi_route_init(api);
	api->vrf_id = b->vrf_id;
	api->type = ZEBRA_ROUTE_BGP_MIDR;
	api->instance = instance;
	api->safi = SAFI_UNICAST;
	api->prefix = *p;

	SET_FLAG(api->message, ZAPI_MESSAGE_METRIC);
	api->metric = instance == MIDR_INSTANCE_TE ? 1 : paths[0].metric;

	SET_FLAG(api->message, ZAPI_MESSAGE_DISTANCE);
	api->distance = MIDR_DP_DISTANCE;

	SET_FLAG(api->flags, ZEBRA_FLAG_ALLOW_RECURSION);
	SET_FLAG(api->message, ZAPI_MESSAGE_NEXTHOP);

	for (i = 0; i < path_count && api->nexthop_num < MULTIPATH_NUM; i++) {
		struct zapi_nexthop *znh = &api->nexthops[api->nexthop_num];

		if (!dp_path_fill_zapi_nh(b->vrf_id, &paths[i], znh))
			continue;

		if (paths[i].weight > 0) {
			znh->weight = paths[i].weight;
			SET_FLAG(znh->flags, ZAPI_NEXTHOP_FLAG_WEIGHT);
			any_weight = true;
		}

		/* SRv6: the SID list rides on the first nexthop only, using
		 * H.Insert so the kernel splices an SRH into the existing
		 * IPv6 header. */
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

	if (!api->nexthop_num)
		return -EHOSTUNREACH;

	/* Keep UCMP weights across zebra's recursive nexthop resolution. */
	if (any_weight)
		SET_FLAG(api->flags, ZEBRA_FLAG_USE_RECURSIVE_WEIGHT);

	return 0;
}

/*
 * =========================================================================
 *  Send and commit
 * =========================================================================
 */

static int dp_zapi_send(struct midr_dp_backend *b, struct zapi_route *api,
			uint8_t cmd)
{
	if (!b->zc || b->zc->sock < 0)
		return -ENOTCONN;
	if (zclient_route_send(cmd, b->zc, api) != ZCLIENT_SEND_SUCCESS)
		return -EIO;
	return 0;
}

/*
 * DELETE carries only the identity: route type, VRF, table and instance must
 * match the ADD so zebra removes exactly the route we installed. */
static int dp_send_delete(struct midr_dp_backend *b, const struct prefix *p,
			  uint8_t instance)
{
	struct zapi_route api;

	zapi_route_init(&api);
	api.vrf_id = b->vrf_id;
	api.type = ZEBRA_ROUTE_BGP_MIDR;
	api.instance = instance;
	api.safi = SAFI_UNICAST;
	api.prefix = *p;

	return dp_zapi_send(b, &api, ZEBRA_ROUTE_DELETE);
}

/*
 * Process one pending operation against the installed hash.
 *
 * The installed entry is only updated after zebra accepted the message.  If
 * the old route was already deleted before a new ADD failed to encode or
 * send, the entry is dropped instead, so the hash always mirrors zebra.
 */
static int dp_process_one_op(struct midr_dp_backend *b,
			     struct midr_pending_op *op)
{
	struct midr_installed_entry *installed;
	struct zapi_route api;
	int ret;

	switch (op->op) {
	case MIDR_OP_ADD:
		installed = dp_installed_lookup(b->installed, &op->prefix,
						op->instance);
		if (installed && dp_result_same(installed, op))
			return 0;

		if (installed) {
			ret = dp_send_delete(b, &op->prefix, installed->instance);
			if (ret)
				return ret;
			dp_installed_unset(b->installed, &op->prefix,
					   installed->instance);
			b->route_deletes++;
		}

		ret = dp_result_to_zapi(b, &op->prefix, op->paths,
					op->path_count, op->sid_count,
					op->sid_list, op->instance, &api);
		if (ret)
			return ret;

		ret = dp_zapi_send(b, &api, ZEBRA_ROUTE_ADD);
		if (ret)
			return ret;

		ret = dp_installed_set(b->installed, &op->prefix, op->paths,
				       op->path_count, op->sid_count,
				       op->sid_list, op->instance);
		if (ret)
			/* The ADD was accepted by zebra but we could not record
			 * it.  Leave the operation queued so recovery re-sends
			 * and re-records it; never pretend it is installed. */
			return ret;
		b->route_adds++;
		return 0;

	case MIDR_OP_DEL:
		installed = dp_installed_lookup(b->installed, &op->prefix,
						op->instance);
		if (!installed)
			return 0;

		ret = dp_send_delete(b, &op->prefix, op->instance);
		if (ret)
			return ret;

		dp_installed_unset(b->installed, &op->prefix, op->instance);
		b->route_deletes++;
		return 0;
	}

	return -EINVAL;
}

/*
 * Drain the pending queue in FIFO order.
 *
 * On a send failure the failing operation and everything after it stay
 * queued, so the caller (or the recovery timer) can rebuild the batch.  The
 * adapter responds to the error by calling abort_pending(), which matches the
 * contract: abort, keep the previous desired generation, wait for resync.
 */
static int dp_flush_pending(struct midr_dp_backend *b)
{
	struct listnode *node, *nnode;
	struct midr_pending_op *op;
	uint64_t flushed = 0;
	int ret = 0;

	b->flush_pending = false;
	for (ALL_LIST_ELEMENTS(b->pending_ops, node, nnode, op)) {
		ret = dp_process_one_op(b, op);
		if (ret) {
			/* Counted on both the immediate-flush and the deferred
			 * path so the status is complete. */
			b->send_failures++;
			b->last_error = ret;
			break;
		}
		list_delete_node(b->pending_ops, node);
		dp_pending_op_free(op);
		flushed++;
	}
	if (flushed)
		b->batches++;
	return ret;
}

/*
 * Keep the queue remainder as the retained recovery batch.  dp_flush_pending()
 * has just failed, so every operation still queued is un-applied and is part
 * of the diff the adapter will not re-create; mark all of them as recovery
 * operations so a later abort_pending() of a new round cannot drop them.
 */
static void dp_retain_remainder(struct midr_dp_backend *b)
{
	struct listnode *node;
	struct midr_pending_op *op;

	if (!b->pending_ops)
		return;
	for (ALL_LIST_ELEMENTS_RO(b->pending_ops, node, op))
		op->recovery = true;
}

/*
 * Discard the operations staged by the current round only.  The adapter calls
 * abort_pending() when it rejects the current diff and it keeps its previous
 * desired generation, so the current round's operations are safe to drop.
 * Retained recovery operations belong to an earlier, already-committed round
 * and are the only replay of their missing installs, so they are kept.
 */
static void dp_abort_pending_ops(struct midr_dp_backend *b)
{
	struct listnode *node, *nnode;
	struct midr_pending_op *op;

	if (b->t_deferred)
		event_cancel(&b->t_deferred);
	b->flush_pending = false;
	if (!b->pending_ops)
		return;
	for (ALL_LIST_ELEMENTS(b->pending_ops, node, nnode, op)) {
		if (op->recovery)
			continue;
		list_delete_node(b->pending_ops, node);
		dp_pending_op_free(op);
	}
}

static void dp_flush_timer_cb(struct event *t);

static void dp_recovery_cb(struct event *t);

/*
 * A deferred batch failed after update_deferred() already returned success,
 * so the adapter never saw the error and has already advanced its desired
 * generation.  The ops left in the queue (the failing one and everything
 * after it) are exactly the un-applied remainder of the diff between the last
 * accepted SPF result and the current one, so they are kept as the retained
 * batch instead of being dropped.  Recovery is then two-step, driven by
 * dp_recovery_cb():
 *
 *   1. retry the retained batch; re-submitting the remainder converges the
 *      FIB without the adapter having to recreate it;
 *   2. once the batch was applied (or there was nothing pending), call
 *      midr_spf_install_resync() as the spec-visible reconciliation, which is
 *      a no-op when the adapter already reached the desired generation.
 *
 * Retries are bounded (MIDR_DP_RECOVERY_MAX_ATTEMPTS, 100 ms apart).  If
 * zebra is unreachable the recovery returns without rescheduling and leaves
 * convergence to the zebra-reconnect path (clear installed hash +
 * midr_spf_install_replay()).
 */
static void dp_schedule_recovery(struct midr_dp_backend *b, int error)
{
	b->last_error = error;
	if (!b->master || b->t_recovery)
		return;
	b->recovery_state = MIDR_DP_RECOVERY_ACTIVE;
	b->recovery_attempts = 0;
	event_add_timer_msec(b->master, dp_recovery_cb, b, MIDR_DP_RECOVERY_MS,
			     &b->t_recovery);
}

/* Arm the recovery timer; the caller owns state/attempt updates. */
static void dp_recovery_arm(struct midr_dp_backend *b, uint32_t delay_ms)
{
	if (!b->master)
		return;
	event_add_timer_msec(b->master, dp_recovery_cb, b, delay_ms,
			     &b->t_recovery);
}

/* One-shot 100 ms batch timer: fires once and coalesces further arming. */
static void dp_flush_timer_cb(struct event *t)
{
	struct midr_dp_backend *b = EVENT_ARG(t);
	int ret;

	b->t_deferred = NULL;
	b->flush_pending = false;
	if (!listcount(b->pending_ops))
		return;

	ret = dp_flush_pending(b);
	if (!ret)
		return;

	/* The adapter was already told this batch succeeded, so its desired
	 * generation is ahead of the FIB and it will not re-create the missing
	 * operations.  Retain the un-applied remainder and drive recovery; it
	 * is never dropped while the desired generation is ahead. */
	dp_retain_remainder(b);
	zlog_warn("MIDR data plane: batch submission failed (%d), retained %u ops"
		  " for recovery", ret, listcount(b->pending_ops));
	dp_schedule_recovery(b, ret);
}

/* Two-step deferred-batch recovery (see the recovery state machine above):
 *
 *   1. retry the retained pending batch; on continued failure re-arm the
 *      timer while MIDR_DP_RECOVERY_MAX_ATTEMPTS is not exhausted, then fall
 *      back to the slow heartbeat;
 *   2. once the batch was applied (or was empty), call
 *      midr_spf_install_resync() as the spec-visible reconciliation -- a
 *      no-op when generation already matches.
 *
 * If zebra is unreachable the callback keeps the needs-recovery state and
 * re-arms the slow heartbeat, so a socket that stays up without ever
 * disconnecting cannot stall recovery permanently; a reconnect or a fresh
 * desired generation re-kicks the fast path.
 */
static void dp_recovery_cb(struct event *t)
{
	struct midr_dp_backend *b = EVENT_ARG(t);
	int ret;

	b->t_recovery = NULL;
	if (!b->ctx) {
		b->recovery_state = MIDR_DP_RECOVERY_IDLE;
		return;
	}

	/* Cannot talk to zebra right now.  Keep the retained batch and the
	 * needs-recovery state, but only the slow heartbeat can re-arm it.  The
	 * zclient socket is the authoritative reachability signal. */
	if (!b->zc || b->zc->sock < 0) {
		b->recovery_state = MIDR_DP_RECOVERY_STALLED;
		dp_recovery_arm(b, MIDR_DP_RECOVERY_HEARTBEAT_MS);
		return;
	}

	/* Step 1: retry the retained batch.  The queued ops are the
	 * un-applied remainder of the diff, so this is idempotent.  A failure
	 * never drops the batch -- that would be a permanent divergence -- it
	 * is retried while the budget lasts. */
	if (listcount(b->pending_ops)) {
		ret = dp_flush_pending(b);
		if (ret) {
			b->last_error = ret;
			b->recovery_attempts++;
			if (b->recovery_attempts <
			    MIDR_DP_RECOVERY_MAX_ATTEMPTS) {
				b->recovery_state = MIDR_DP_RECOVERY_ACTIVE;
				dp_recovery_arm(b, MIDR_DP_RECOVERY_MS);
				return;
			}
			/* Budget exhausted: switch to the slow heartbeat so the
			 * socket staying up without a disconnect cannot stall
			 * recovery forever. */
			b->recovery_state = MIDR_DP_RECOVERY_STALLED;
			zlog_warn("MIDR data plane: retained batch still failing after"
				  " %u retries (%d), slow heartbeat armed",
				  b->recovery_attempts, ret);
			dp_recovery_arm(b, MIDR_DP_RECOVERY_HEARTBEAT_MS);
			return;
		}
	}

	/* Step 2: spec-visible reconciliation. */
	ret = midr_spf_install_resync(b->ctx);
	b->resyncs++;
	if (ret && ret != -ENOENT) {
		b->last_error = ret;
		b->recovery_attempts++;
		if (b->recovery_attempts < MIDR_DP_RECOVERY_MAX_ATTEMPTS) {
			b->recovery_state = MIDR_DP_RECOVERY_ACTIVE;
			dp_recovery_arm(b, MIDR_DP_RECOVERY_MS);
			return;
		}
		b->recovery_state = MIDR_DP_RECOVERY_STALLED;
		zlog_warn("MIDR data plane: resync still failing after %u retries"
			  " (%d), slow heartbeat armed",
			  b->recovery_attempts, ret);
		dp_recovery_arm(b, MIDR_DP_RECOVERY_HEARTBEAT_MS);
		return;
	}
	b->recovery_state = MIDR_DP_RECOVERY_IDLE;
	b->recovery_attempts = 0;
	b->last_error = 0;
	zlog_info("MIDR data plane: recovery complete (installed=%zu)",
		  dp_installed_count(b));
}

/*
 * Re-arm recovery from an external trigger (a fresh desired generation or a
 * zebra reconnect): reset the fast-retry budget and bring the timer forward,
 * so a stalled batch does not have to wait for the slow heartbeat.  A no-op
 * when there is nothing to recover.
 */
static void dp_recovery_kick(struct midr_dp_backend *b)
{
	if (!b || b->recovery_state == MIDR_DP_RECOVERY_IDLE)
		return;
	if (b->t_recovery)
		event_cancel(&b->t_recovery);
	b->recovery_state = MIDR_DP_RECOVERY_ACTIVE;
	b->recovery_attempts = 0;
	dp_recovery_arm(b, MIDR_DP_RECOVERY_MS);
}

/*
 * zebra (re)connected: the previous client state is gone, so drop the local
 * installed hash and replay the complete current result.  replay ignores the
 * generation and starts from an empty baseline by contract.
 */
static void dp_zebra_connected(struct zclient *zc)
{
	struct midr_dp_backend *b = midr_dp_current;
	int ret;

	if (!b || zc != b->zc)
		return;

	b->zebra_up = true;
	b->last_error = 0;
	dp_installed_clear(b);
	zlog_info("MIDR data plane: zebra connected, replaying SPF routes");

	if (!b->ctx)
		return;

	/* The replay stages the complete result set; submit it at once instead
	 * of waiting for the deferred window. */
	b->immediate_next = true;
	ret = midr_spf_install_replay(b->ctx);
	b->immediate_next = false;
	b->replays++;
	if (ret && ret != -ENOENT) {
		b->last_error = ret;
		zlog_warn("MIDR data plane: replay deferred (%d)", ret);
	}

	/* A reconnect is a connection-state change: re-arm any outstanding
	 * recovery so it does not depend on the slow heartbeat. */
	dp_recovery_kick(b);
}

/*
 * =========================================================================
 *  struct midr_zebra_backend_ops implementation
 * =========================================================================
 */

/* Stage a route.  The input is deep-copied before returning, as required by
 * the contract, so the SPF adapter may release its result immediately. */
static int dp_route_add(void *arg, const struct prefix *prefix,
			const struct midr_path_result *result)
{
	struct midr_dp_backend *b = arg;
	struct midr_pending_op *op;

	if (!b || !prefix || !result || !result->paths || !result->path_count)
		return -EINVAL;
	if (result->explicit.sid_count > MIDR_SRV6_MAX_SEGS)
		return -E2BIG;

	op = calloc(1, sizeof(*op));
	if (!op)
		return -ENOMEM;

	op->op = MIDR_OP_ADD;
	prefix_copy(&op->prefix, prefix);
	op->instance = result->instance;
	op->path_count = result->path_count;
	op->paths = dp_paths_dup(result->paths, result->path_count);
	if (!op->paths) {
		free(op);
		return -ENOMEM;
	}
	op->sid_count = result->explicit.sid_count;
	if (op->sid_count)
		memcpy(op->sid_list, result->explicit.sid_list,
		       sizeof(op->sid_list[0]) * op->sid_count);

	listnode_add(b->pending_ops, op);
	b->add_staged++;
	return 0;
}

static int dp_route_del(void *arg, const struct prefix *prefix,
			uint8_t instance)
{
	struct midr_dp_backend *b = arg;
	struct midr_pending_op *op;

	if (!b || !prefix)
		return -EINVAL;
	if (instance != MIDR_INSTANCE_SPF && instance != MIDR_INSTANCE_TE)
		return -EINVAL;

	op = calloc(1, sizeof(*op));
	if (!op)
		return -ENOMEM;

	op->op = MIDR_OP_DEL;
	op->instance = instance;
	prefix_copy(&op->prefix, prefix);

	listnode_add(b->pending_ops, op);
	b->del_staged++;
	return 0;
}

/* Arm the 100 ms batch timer; repeated calls coalesce.  On the cold path (the
 * first batch after start or after a zebra reconnect) the batch is submitted
 * immediately instead: there is no churn to absorb and the first route should
 * not wait for the window. */
static int dp_route_update_deferred(void *arg)
{
	struct midr_dp_backend *b = arg;

	if (!b || !b->master)
		return -EINVAL;

	/* Fresh adapter work is an explicit re-arm: a new desired generation
	 * must not have to wait for the slow heartbeat. */
	dp_recovery_kick(b);

	if (b->immediate_next) {
		b->immediate_next = false;
		zlog_info("MIDR data plane: cold-path submit (%u ops)",
			  listcount(b->pending_ops));
		return dp_flush_pending(b);
	}

	if (b->flush_pending)
		return 0;

	b->flush_pending = true;
	event_add_timer_msec(b->master, dp_flush_timer_cb, b,
			     MIDR_BATCH_INTERVAL_MS, &b->t_deferred);
	return 0;
}

/* Submit the pending batch immediately, cancelling the timer. */
static int dp_route_flush(void *arg)
{
	struct midr_dp_backend *b = arg;

	if (!b)
		return -EINVAL;
	if (b->t_deferred)
		event_cancel(&b->t_deferred);
	return dp_flush_pending(b);
}

static void dp_route_abort_pending(void *arg)
{
	struct midr_dp_backend *b = arg;

	if (!b)
		return;
	dp_abort_pending_ops(b);
}

static const struct midr_zebra_backend_ops midr_dp_ops = {
	.route_add = dp_route_add,
	.route_del = dp_route_del,
	.update_deferred = dp_route_update_deferred,
	.flush = dp_route_flush,
	.abort_pending = dp_route_abort_pending,
};

/*
 * =========================================================================
 *  Lifecycle and diagnostics
 * =========================================================================
 */

static void dp_backend_destroy(struct midr_dp_backend *b)
{
	if (!b)
		return;
	if (b->t_deferred)
		event_cancel(&b->t_deferred);
	if (b->t_recovery)
		event_cancel(&b->t_recovery);
	b->recovery_state = MIDR_DP_RECOVERY_IDLE;
	dp_abort_pending_ops(b);
	if (b->installed)
		hash_clean_and_free(&b->installed, dp_installed_entry_free);
	if (b->pending_ops)
		list_delete(&b->pending_ops);
	if (b->zc) {
		zclient_stop(b->zc);
		zclient_free(b->zc);
	}
	free(b);
}

/*
 * Deliver a batch left over from the registration-time reconciliation.
 *
 * update_deferred() already reported success for this batch, so the adapter's
 * desired generation is ahead of the FIB and it will not re-create these
 * operations.  A failure here therefore has the same consequence as a failed
 * deferred batch: the queued remainder is the only replay of the missing
 * installs, so it is retained and handed to the recovery state machine.
 * Leaving it queued without arming recovery would strand it forever when the
 * socket never disconnects (review 3, D5).
 */
static void dp_cold_submit(struct midr_dp_backend *b)
{
	int ret;

	if (!b->zc || b->zc->sock < 0 || !listcount(b->pending_ops))
		return;

	b->immediate_next = false;
	ret = dp_flush_pending(b);
	if (!ret)
		return;

	dp_retain_remainder(b);
	zlog_warn("MIDR data plane: cold-path batch failed (%d), retained %u ops"
		  " for recovery", ret, listcount(b->pending_ops));
	dp_schedule_recovery(b, ret);
}

int midr_dp_backend_start(struct midr_context *ctx, struct event_loop *master,
			  vrf_id_t vrf_id)
{
	struct midr_dp_backend *b;
	int ret;

	if (!ctx || !master)
		return -EINVAL;
	if (midr_dp_current)
		return -EALREADY;

	b = calloc(1, sizeof(*b));
	if (!b)
		return -ENOMEM;
	b->ctx = ctx;
	b->master = master;
	b->vrf_id = vrf_id;
	b->pending_ops = list_new();
	b->pending_ops->del = (void (*)(void *))dp_pending_op_free;
	b->installed = hash_create(dp_prefix_hash_key, dp_prefix_cmp,
				   "MIDR data-plane installed routes");
	midr_dp_current = b;

	/* zclient_init() schedules the connect event; zebra_connected performs
	 * the replay once the socket is up. */
	b->zc = zclient_new(master, &zclient_options_default, NULL, 0);
	if (!b->zc) {
		midr_dp_current = NULL;
		dp_backend_destroy(b);
		return -ENOMEM;
	}
	zclient_init(b->zc, ZEBRA_ROUTE_BGP_MIDR, 0, &midr_dp_privs);
	b->zc->zebra_connected = dp_zebra_connected;
	b->immediate_next = true;

	/* Registration starts the SPF installation adapter, subscribes to the
	 * committed TED and immediately reconciles the current READY result. */
	ret = midr_zebra_backend_register(ctx, &midr_dp_ops, b);
	if (ret) {
		midr_dp_current = NULL;
		dp_backend_destroy(b);
		return ret;
	}

	/* Deliver the batch the registration-time reconciliation may have
	 * staged.  A failure is handled exactly like a failed deferred batch:
	 * retain the remainder and let the recovery state machine drive it. */
	dp_cold_submit(b);

	zlog_info("MIDR data plane backend started (vrf %u, zserv %s)", vrf_id,
		  frr_zclientpath);
	return 0;
}

void midr_dp_backend_stop(void)
{
	struct midr_dp_backend *b = midr_dp_current;
	int ret;

	if (!b)
		return;

	/* Stop new work, then unregister: the adapter withdraws the accepted
	 * SPF routes and flushes immediately while the zclient is still
	 * connected.  Only afterwards is the zclient destroyed. */
	if (b->t_deferred)
		event_cancel(&b->t_deferred);
	if (b->t_recovery)
		event_cancel(&b->t_recovery);
	ret = midr_zebra_backend_unregister(b->ctx);
	if (ret)
		zlog_warn("MIDR data plane: unregister returned %d", ret);
	midr_dp_backend_log_status("stop");
	midr_dp_current = NULL;
	dp_backend_destroy(b);
}

struct midr_dp_backend *midr_dp_backend_get(void)
{
	return midr_dp_current;
}

struct zclient *midr_dp_backend_zclient(void)
{
	return midr_dp_current ? midr_dp_current->zc : NULL;
}

vrf_id_t midr_dp_backend_vrf_id(void)
{
	return midr_dp_current ? midr_dp_current->vrf_id : VRF_DEFAULT;
}

bool midr_dp_backend_ready(void)
{
	return midr_dp_current && midr_dp_current->zc &&
	       midr_dp_current->zc->sock >= 0;
}

void midr_dp_backend_status_get(struct midr_dp_status *status)
{
	struct midr_dp_backend *b = midr_dp_current;

	if (!status)
		return;
	memset(status, 0, sizeof(*status));
	if (!b)
		return;

	status->zebra_up = b->zebra_up && b->zc && b->zc->sock >= 0;
	status->immediate_next = b->immediate_next;
	status->add_staged = b->add_staged;
	status->del_staged = b->del_staged;
	status->batches = b->batches;
	status->route_adds = b->route_adds;
	status->route_deletes = b->route_deletes;
	status->send_failures = b->send_failures;
	status->replays = b->replays;
	status->resyncs = b->resyncs;
	status->recovering = b->recovery_state != MIDR_DP_RECOVERY_IDLE;
	status->recovery_stalled =
		b->recovery_state == MIDR_DP_RECOVERY_STALLED;
	status->recovery_attempts = b->recovery_attempts;
	status->pending = b->pending_ops ? listcount(b->pending_ops) : 0;
	status->installed = dp_installed_count(b);
	status->last_error = b->last_error;
}

void midr_dp_backend_log_status(const char *tag)
{
	struct midr_dp_status st;

	midr_dp_backend_status_get(&st);
	printf("dp backend=%s zebra=%s installed=%zu pending=%zu route-adds=%" PRIu64
	       " route-deletes=%" PRIu64 " send-failures=%" PRIu64
	       " replays=%" PRIu64 " resyncs=%" PRIu64 " recovery=%s last-error=%d\n",
	       tag ? tag : "status", st.zebra_up ? "up" : "down", st.installed,
	       st.pending, st.route_adds, st.route_deletes, st.send_failures,
	       st.replays, st.resyncs,
	       st.recovery_stalled ? "stalled"
				   : (st.recovering ? "active" : "idle"),
	       st.last_error);
	(void)fflush(stdout);
}
