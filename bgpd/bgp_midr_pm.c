// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR Performance Measurement (PM) module.
 *
 * Implements real UDP round-trip probing:
 *   - One shared UDP socket bound to the local transport_addr (not INADDR_ANY)
 *   - Per-target probe contexts with their own probe/timeout timers
 *   - EWMA RTT (short-term α=0.2, long-term α=0.05)
 *   - Sliding-window loss rate (100 samples)
 *   - Bandwidth score: sqrt(1.5) / (rtt_s * sqrt(max(loss, 1e-6)))
 *   - Fast-probe mode (200 ms) after 3 consecutive failures; normal (1 s) otherwise
 *   - Security: source IP validated against global_view before processing any packet
 *
 * Feeds results to NDS via midr_nds_on_link_update() (I-5).
 *
 * The steady-state sweep timer (midr_pm_probe_timer) is kept for established
 * neighbours that NDS has not explicitly registered via I-1; it skips any node
 * that already has a dedicated probe_ctx to avoid double-probing.
 */

#include <zebra.h>
#include <math.h>
#include <sys/time.h>

#include "log.h"
#include "memory.h"
#include "prefix.h"
#include "hash.h"
#include "network.h"
#include "sockopt.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_midr.h"
#include "bgpd/bgp_midr_pm.h"

DEFINE_MTYPE_STATIC(BGPD, MIDR_PROBE_CTX, "MIDR probe context");

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/* Monotonic microsecond clock. */
static uint64_t pm_now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

/* Hash key: the probe target prefix (node_id). */
static unsigned int probe_ctx_hash_key(const void *data)
{
	const struct midr_probe_ctx *ctx = data;

	return prefix_hash_key(&ctx->target);
}

/* Hash comparison. */
static bool probe_ctx_hash_cmp(const void *a, const void *b)
{
	const struct midr_probe_ctx *ca = a;
	const struct midr_probe_ctx *cb = b;

	return !!prefix_same(&ca->target, &cb->target);
}

/* Look up a probe context by node_id prefix. */
static struct midr_probe_ctx *pm_ctx_find(struct bgp_midr *mi,
					  const struct prefix *node_id)
{
	struct midr_probe_ctx key = {};

	prefix_copy(&key.target, node_id);
	return hash_lookup(mi->probe_contexts, &key);
}

/* Compute the current loss rate from the sliding window. */
static double pm_loss_rate(const struct midr_probe_ctx *ctx)
{
	uint32_t total = ctx->loss_win_total < MIDR_PM_LOSS_WINDOW_SIZE
				 ? ctx->loss_win_total
				 : MIDR_PM_LOSS_WINDOW_SIZE;
	uint32_t received = 0;
	uint32_t i;

	if (total == 0)
		return 0.0;

	for (i = 0; i < total; i++)
		received += ctx->loss_win[i];
	return 1.0 - (double)received / total;
}

/* Build and push an I-5 update to NDS. */
static void pm_push_i5(struct midr_probe_ctx *ctx,
		       enum midr_link_status status)
{
	struct bgp *bgp = ctx->bgp;
	struct midr_link_metrics st = {}, lt = {};
	double loss = pm_loss_rate(ctx);
	double rtt_s = 0.0, bw_score = 0.0;

	if (ctx->st_init) {
		st.rtt_us = (uint32_t)ctx->st_rtt_us;
		st.rtt_min_us = ctx->st_rtt_min_us;
		st.rtt_max_us = ctx->st_rtt_max_us;
	}
	st.loss_rate = loss;

	/* Bandwidth score per stage1_design §2.2:
	 *   score = sqrt(1.5) / (rtt_s * sqrt(max(loss, 1e-6))) */
	if (ctx->st_init && ctx->st_rtt_us > 0) {
		rtt_s = ctx->st_rtt_us / 1e6;
		bw_score = sqrt(1.5) / (rtt_s * sqrt(fmax(loss, 1e-6)));
		st.bw_score = (uint32_t)fmin(bw_score, (double)UINT32_MAX);
	}

	if (ctx->lt_init) {
		lt.rtt_us = (uint32_t)ctx->lt_rtt_us;
		lt.rtt_min_us = (uint32_t)ctx->lt_rtt_us; /* approx */
		lt.rtt_max_us = (uint32_t)ctx->lt_rtt_us;
		lt.loss_rate = ctx->lt_loss_rate;
		if (ctx->lt_rtt_us > 0) {
			rtt_s = ctx->lt_rtt_us / 1e6;
			bw_score = sqrt(1.5)
				   / (rtt_s
				      * sqrt(fmax(ctx->lt_loss_rate, 1e-6)));
			lt.bw_score =
				(uint32_t)fmin(bw_score, (double)UINT32_MAX);
		}
	}

	/* Structured log for external metric collection / plotting.
	 * Grep pattern: "MIDR PM I-5:" */
	MIDR_LOG("MIDR PM I-5: node=%pFX status=%d failures=%u "
		 "st_rtt_us=%u st_loss=%.6f st_bw=%u "
		 "lt_rtt_us=%u lt_loss=%.6f lt_bw=%u",
		 &ctx->target, (int)status, ctx->consecutive_failures,
		 st.rtt_us, st.loss_rate, st.bw_score,
		 lt.rtt_us, lt.loss_rate, lt.bw_score);

	midr_nds_on_link_update(bgp, &ctx->target, status,
				ctx->consecutive_failures, &st, &lt);
}

/* Update EWMA + loss statistics after a successful probe, then push I-5. */
static void pm_update_stats(struct midr_probe_ctx *ctx, uint64_t rtt_us)
{
	double loss;

	/* Short-term EWMA */
	if (!ctx->st_init) {
		ctx->st_rtt_us = (double)rtt_us;
		ctx->st_rtt_min_us = (uint32_t)rtt_us;
		ctx->st_rtt_max_us = (uint32_t)rtt_us;
		ctx->st_init = true;
	} else {
		ctx->st_rtt_us = MIDR_PM_ALPHA_SHORT * (double)rtt_us
				 + (1.0 - MIDR_PM_ALPHA_SHORT) * ctx->st_rtt_us;
		if (rtt_us < ctx->st_rtt_min_us)
			ctx->st_rtt_min_us = (uint32_t)rtt_us;
		if (rtt_us > ctx->st_rtt_max_us)
			ctx->st_rtt_max_us = (uint32_t)rtt_us;
	}

	/* Current loss from sliding window */
	loss = pm_loss_rate(ctx);

	/* Long-term EWMA */
	if (!ctx->lt_init) {
		ctx->lt_rtt_us = (double)rtt_us;
		ctx->lt_loss_rate = loss;
		ctx->lt_init = true;
	} else {
		ctx->lt_rtt_us = MIDR_PM_ALPHA_LONG * (double)rtt_us
				 + (1.0 - MIDR_PM_ALPHA_LONG) * ctx->lt_rtt_us;
		ctx->lt_loss_rate =
			MIDR_PM_ALPHA_LONG * loss
			+ (1.0 - MIDR_PM_ALPHA_LONG) * ctx->lt_loss_rate;
	}

	pm_push_i5(ctx, MIDR_LINK_UP);
}

/* -------------------------------------------------------------------------
 * Per-target probe send and timeout
 * ---------------------------------------------------------------------- */

static void midr_pm_probe_timer_fn(struct event *t); /* forward */
static void midr_pm_timeout(struct event *t);	     /* forward */

/* Send one probe packet and arm the per-probe timeout. */
static void pm_send_probe(struct midr_probe_ctx *ctx)
{
	struct bgp_midr *mi = ctx->bgp->midr_info;
	struct midr_probe_pkt pkt = {};
	struct sockaddr_in dst = {};
	uint64_t now;

	if (mi->pm_sock < 0)
		return;

	now = pm_now_us();

	pkt.magic = htonl(MIDR_PM_PROBE_MAGIC);
	pkt.type = MIDR_PM_PROBE_REQ;
	pkt.seqno = htonl(ctx->seqno);
	pkt.sent_us = htobe64(now);

	dst.sin_family = AF_INET;
	dst.sin_port = htons(MIDR_PM_PROBE_PORT);
	dst.sin_addr = ctx->target_addr;

	if (sendto(mi->pm_sock, &pkt, sizeof(pkt), 0,
		   (struct sockaddr *)&dst, sizeof(dst)) < 0)
		MIDR_LOG("MIDR PM: sendto %pI4 failed: %s",
			 &ctx->target_addr, safe_strerror(errno));

	ctx->pending_seqno = ctx->seqno;
	ctx->seqno++;
	ctx->sent_us = now;
	ctx->probe_outstanding = true;

	event_add_timer_msec(bm->master, midr_pm_timeout, ctx,
			     MIDR_PM_PROBE_TIMEOUT_MS, &ctx->t_timeout);
}

/* Probe reply timed out — count loss, maybe switch to fast mode, push I-5. */
static void midr_pm_timeout(struct event *t)
{
	struct midr_probe_ctx *ctx = EVENT_ARG(t);

	ctx->probe_outstanding = false;

	/* Record as lost in sliding window */
	ctx->loss_win[ctx->loss_win_idx % MIDR_PM_LOSS_WINDOW_SIZE] = 0;
	ctx->loss_win_idx++;
	ctx->loss_win_total++;

	ctx->consecutive_failures++;
	if (ctx->consecutive_failures >= MIDR_PM_CONSECUTIVE_FAIL_FAST
	    && ctx->state != MIDR_PROBE_FAST) {
		ctx->state = MIDR_PROBE_FAST;
		MIDR_LOG("MIDR PM: %pFX entered fast-probe mode after %u failures",
			 &ctx->target, ctx->consecutive_failures);
	}

	pm_push_i5(ctx,
		   (ctx->consecutive_failures >= MIDR_PM_CONSECUTIVE_FAIL_FAST)
			   ? MIDR_LINK_DEGRADED
			   : MIDR_LINK_UP);
}

/* Per-target probe timer: send one probe then reschedule. */
static void midr_pm_probe_timer_fn(struct event *t)
{
	struct midr_probe_ctx *ctx = EVENT_ARG(t);

	if (!ctx->probe_outstanding)
		pm_send_probe(ctx);

	if (ctx->state == MIDR_PROBE_FAST)
		event_add_timer_msec(bm->master, midr_pm_probe_timer_fn, ctx,
				     MIDR_PM_PROBE_INTERVAL_FAST_MS,
				     &ctx->t_probe);
	else
		event_add_timer(bm->master, midr_pm_probe_timer_fn, ctx,
				MIDR_PM_PROBE_INTERVAL_NORMAL, &ctx->t_probe);
}

/* -------------------------------------------------------------------------
 * Receive handler (prober + responder roles, security filtering)
 * ---------------------------------------------------------------------- */

/*
 * Find a global_view node entry by transport_addr (not node_id / router-id).
 * Probes travel between transport_addrs (loopback IPs), not router-ids.
 * Falls back to comparing against node_id if no transport_addr is set.
 * O(n) — acceptable for a small peer set.
 */
static bool pm_is_known_transport(struct bgp_midr *mi, struct in_addr addr)
{
	struct midr_node_entry *entry;

	frr_each (midr_node_hash, &mi->global_view->nodes, entry) {
		if (entry->is_self)
			continue;
		if (entry->has_transport_addr) {
			if (entry->transport_addr.s_addr == addr.s_addr)
				return true;
		} else {
			if (entry->node_id.family == AF_INET
			    && entry->node_id.u.prefix4.s_addr == addr.s_addr)
				return true;
		}
	}
	return false;
}

/* Walk-callback: find a probe context whose target_addr matches a given IP. */
struct pm_ctx_find_by_ip_arg {
	struct in_addr ip;
	struct midr_probe_ctx *result;
};

static void pm_ctx_find_by_ip_cb(struct hash_bucket *hb, void *arg)
{
	struct midr_probe_ctx *ctx = hb->data;
	struct pm_ctx_find_by_ip_arg *a = arg;

	if (a->result)
		return; /* already found */
	if (ctx->target_addr.s_addr == a->ip.s_addr)
		a->result = ctx;
}

static void midr_pm_recv(struct event *t)
{
	struct bgp *bgp = EVENT_ARG(t);
	struct bgp_midr *mi = bgp->midr_info;
	struct midr_probe_pkt pkt;
	struct sockaddr_in src = {};
	socklen_t srclen = sizeof(src);
	ssize_t n;
	struct midr_probe_ctx *ctx;
	uint64_t rtt_us;

	/* Re-arm read event immediately so we don't miss packets while processing */
	event_add_read(bm->master, midr_pm_recv, bgp, mi->pm_sock,
		       &mi->t_pm_read);

	n = recvfrom(mi->pm_sock, &pkt, sizeof(pkt), 0,
		     (struct sockaddr *)&src, &srclen);
	if (n < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			zlog_warn("MIDR PM: recvfrom failed: %s",
				  safe_strerror(errno));
		return;
	}

	if ((size_t)n < sizeof(struct midr_probe_pkt)) {
		MIDR_LOG("MIDR PM: short packet (%zd B) from %pI4, dropped",
			 n, &src.sin_addr);
		return;
	}

	if (ntohl(pkt.magic) != MIDR_PM_PROBE_MAGIC) {
		MIDR_LOG("MIDR PM: bad magic 0x%08x from %pI4, dropped",
			 ntohl(pkt.magic), &src.sin_addr);
		return;
	}

	/*
	 * Security: source must be a transport_addr we recognise in global_view.
	 * Probes travel between transport_addrs (loopback /32s, e.g. 10.99.x.x),
	 * NOT between router-ids, so we cannot use the router-id hash directly —
	 * we need the O(n) transport_addr walk.
	 */
	if (!pm_is_known_transport(mi, src.sin_addr)) {
		MIDR_LOG("MIDR PM: packet from unknown transport %pI4, dropped",
			 &src.sin_addr);
		return;
	}

	if (pkt.type == MIDR_PM_PROBE_REQ) {
		/* Responder role: echo the packet back with type = REP */
		pkt.type = MIDR_PM_PROBE_REP;
		if (sendto(mi->pm_sock, &pkt, sizeof(pkt), 0,
			   (struct sockaddr *)&src, sizeof(src)) < 0)
			MIDR_LOG("MIDR PM: echo sendto %pI4 failed: %s",
				 &src.sin_addr, safe_strerror(errno));
		return;
	}

	if (pkt.type != MIDR_PM_PROBE_REP)
		return;

	/* Prober role: find the matching context by source transport_addr */
	if (!mi->probe_contexts)
		return;
	{
		struct pm_ctx_find_by_ip_arg arg = { src.sin_addr, NULL };

		hash_iterate(mi->probe_contexts, pm_ctx_find_by_ip_cb, &arg);
		ctx = arg.result;
	}

	if (!ctx) {
		MIDR_LOG("MIDR PM: reply from %pI4 has no matching probe context, dropped",
			 &src.sin_addr);
		return;
	}

	if (!ctx->probe_outstanding) {
		MIDR_LOG("MIDR PM: late/duplicate reply from %pI4 seqno=%u, dropped",
			 &src.sin_addr, ntohl(pkt.seqno));
		return;
	}

	if (ntohl(pkt.seqno) != ctx->pending_seqno) {
		MIDR_LOG("MIDR PM: seqno mismatch from %pI4: got %u expected %u, dropped",
			 &src.sin_addr, ntohl(pkt.seqno), ctx->pending_seqno);
		return;
	}

	/* Valid reply */
	rtt_us = pm_now_us() - ctx->sent_us;

	event_cancel(&ctx->t_timeout);
	ctx->probe_outstanding = false;
	ctx->consecutive_failures = 0;

	/* Record as received in sliding window */
	ctx->loss_win[ctx->loss_win_idx % MIDR_PM_LOSS_WINDOW_SIZE] = 1;
	ctx->loss_win_idx++;
	ctx->loss_win_total++;

	if (ctx->state == MIDR_PROBE_FAST) {
		ctx->state = MIDR_PROBE_NORMAL;
		MIDR_LOG("MIDR PM: %pFX recovered, returning to normal probe rate",
			 &ctx->target);
	}

	MIDR_LOG("MIDR PM: reply from %pI4 rtt=%lluus",
		 &src.sin_addr, (unsigned long long)rtt_us);

	pm_update_stats(ctx, rtt_us);
}

/* -------------------------------------------------------------------------
 * Socket lifecycle
 * ---------------------------------------------------------------------- */

static void midr_pm_open_sock(struct bgp *bgp)
{
	struct bgp_midr *mi = bgp->midr_info;
	struct sockaddr_in sa = {};
	int sock;

	if (!mi->transport_addr_set) {
		zlog_warn("MIDR PM: local transport-address not configured; "
			  "PM socket deferred until `midr transport-address` is set");
		return;
	}

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		zlog_warn("MIDR PM: socket() failed: %s", safe_strerror(errno));
		return;
	}

	sockopt_reuseaddr(sock);

	/* Bind to local_transport_addr only — NOT INADDR_ANY — to limit
	 * the attack surface to the MIDR loopback interface only. */
	sa.sin_family = AF_INET;
	sa.sin_port = htons(MIDR_PM_PROBE_PORT);
	sa.sin_addr = mi->local_transport_addr;
	if (bind(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		zlog_warn("MIDR PM: bind(%pI4:%u) failed: %s",
			  &mi->local_transport_addr, MIDR_PM_PROBE_PORT,
			  safe_strerror(errno));
		close(sock);
		return;
	}

	set_nonblocking(sock);
	mi->pm_sock = sock;
	event_add_read(bm->master, midr_pm_recv, bgp, sock, &mi->t_pm_read);

	MIDR_LOG("MIDR PM: probe socket ready on %pI4:%u",
		 &mi->local_transport_addr, MIDR_PM_PROBE_PORT);
}

static void midr_pm_close_sock(struct bgp *bgp)
{
	struct bgp_midr *mi = bgp->midr_info;

	event_cancel(&mi->t_pm_read);
	if (mi->pm_sock >= 0) {
		close(mi->pm_sock);
		mi->pm_sock = -1;
	}
}

/* -------------------------------------------------------------------------
 * Steady-state sweep timer (covers established neighbours not registered
 * via I-1; skips nodes that already have a dedicated probe_ctx).
 * ---------------------------------------------------------------------- */

static void midr_pm_probe_timer(struct event *t)
{
	struct bgp *bgp = EVENT_ARG(t);
	struct bgp_midr *mi = bgp->midr_info;
	struct midr_node_entry *entry;

	frr_each (midr_node_hash, &mi->global_view->nodes, entry) {
		if (entry->is_self)
			continue;
		if (!entry->is_adjacent)
			continue;
		if (!midr_node_established_peer(bgp, &entry->node_id))
			continue;

		/* Skip nodes already handled by a per-target probe_ctx */
		if (mi->probe_contexts
		    && pm_ctx_find(mi, &entry->node_id))
			continue;

		/* Push a minimal UP update to keep the link entry alive in NDS
		 * for nodes without explicit probe_ctx registration. */
		{
			struct midr_link_metrics m = {};

			midr_nds_on_link_update(bgp, &entry->node_id,
						MIDR_LINK_UP, 0, &m, &m);
		}
	}

	event_add_timer(bm->master, midr_pm_probe_timer, bgp,
			MIDR_PM_PROBE_INTERVAL, &mi->t_pm_probe);
}

/* -------------------------------------------------------------------------
 * Cleanup helper: cancel timers and free a probe context.
 * Called via hash_clean() — receives the data pointer directly.
 * ---------------------------------------------------------------------- */

static void pm_ctx_free_cb(void *data)
{
	struct midr_probe_ctx *ctx = data;

	event_cancel(&ctx->t_probe);
	event_cancel(&ctx->t_timeout);
	XFREE(MTYPE_MIDR_PROBE_CTX, ctx);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

int midr_pm_add_target(struct bgp *bgp, const struct prefix *node_id,
		       enum midr_node_source source, uint32_t capabilities)
{
	struct bgp_midr *mi = bgp->midr_info;
	struct midr_node_entry key = {};
	struct midr_node_entry *entry;
	struct midr_probe_ctx *ctx;

	if (!bgp || !mi || !node_id)
		return -1;

	if (mi->pm_sock < 0) {
		MIDR_LOG("MIDR PM I-1: PM socket not ready, cannot probe %pFX",
			 node_id);
		return -1;
	}

	if (!mi->probe_contexts)
		mi->probe_contexts =
			hash_create(probe_ctx_hash_key, probe_ctx_hash_cmp,
				    "MIDR probe contexts");

	/* Idempotent */
	if (pm_ctx_find(mi, node_id))
		return 0;

	/* Resolve transport_addr from the node table */
	prefix_copy(&key.node_id, node_id);
	entry = midr_node_hash_find(&mi->global_view->nodes, &key);
	if (!entry) {
		MIDR_LOG("MIDR PM I-1: node %pFX not in global_view, skipped",
			 node_id);
		return -1;
	}

	ctx = XCALLOC(MTYPE_MIDR_PROBE_CTX, sizeof(*ctx));
	ctx->bgp = bgp;
	prefix_copy(&ctx->target, node_id);
	ctx->target_addr = entry->has_transport_addr ? entry->transport_addr
						     : entry->node_id.u.prefix4;
	ctx->state = MIDR_PROBE_NORMAL;
	ctx->capabilities = capabilities;
	ctx->source = source;

	hash_get(mi->probe_contexts, ctx, hash_alloc_intern);

	/* Fire first probe immediately (delay = 0 seconds) */
	event_add_timer(bm->master, midr_pm_probe_timer_fn, ctx, 0,
			&ctx->t_probe);

	MIDR_LOG("MIDR PM I-1: start probing %pFX -> %pI4 src=%d caps=0x%x",
		 node_id, &ctx->target_addr, source, capabilities);
	return 0;
}

int midr_pm_remove_target(struct bgp *bgp, const struct prefix *node_id,
			  enum midr_stop_reason reason)
{
	struct bgp_midr *mi = bgp->midr_info;
	struct midr_probe_ctx *ctx;

	if (!bgp || !mi || !node_id)
		return -1;

	if (!mi->probe_contexts)
		return -1;

	ctx = pm_ctx_find(mi, node_id);
	if (!ctx)
		return -1;

	event_cancel(&ctx->t_probe);
	event_cancel(&ctx->t_timeout);

	/* If a probe was in-flight, push LINK_DOWN so NDS/CL can react */
	if (ctx->probe_outstanding) {
		struct midr_link_metrics zero = {};

		midr_nds_on_link_update(bgp, node_id, MIDR_LINK_DOWN,
					ctx->consecutive_failures, &zero, &zero);
	}

	hash_release(mi->probe_contexts, ctx);
	XFREE(MTYPE_MIDR_PROBE_CTX, ctx);

	MIDR_LOG("MIDR PM I-2: stop probing %pFX reason=%d", node_id, reason);
	return 0;
}

/*
 * Called by the VTY `midr transport-address` handler after the address is set.
 * The PM socket cannot be opened during midr_pm_init() because the config file
 * is read AFTER bgp_midr_init() runs.  This function retrofits the open and
 * rescans global_view for neighbors whose I-1 call was silently dropped because
 * the socket was not ready.
 */
void midr_pm_on_transport_addr_set(struct bgp *bgp)
{
	struct bgp_midr *mi;
	struct midr_node_entry *entry;

	if (!bgp || !bgp->midr_info)
		return;

	mi = bgp->midr_info;

	if (mi->pm_sock < 0)
		midr_pm_open_sock(bgp);

	if (mi->pm_sock < 0)
		return;

	/* Re-issue I-1 for any adjacent peers that were already established
	 * when the socket wasn't ready yet.  midr_pm_add_target() is idempotent. */
	if (!mi->global_view)
		return;

	frr_each (midr_node_hash, &mi->global_view->nodes, entry) {
		if (entry->is_self)
			continue;
		if (!entry->is_adjacent)
			continue;
		if (!midr_node_established_peer(bgp, &entry->node_id))
			continue;
		midr_pm_add_target(bgp, &entry->node_id, MIDR_SRC_GOSSIP,
				   entry->capabilities);
	}
}

void midr_pm_init(struct bgp *bgp)
{
	struct bgp_midr *mi;

	if (!bgp || !bgp->midr_info)
		return;

	mi = bgp->midr_info;
	mi->pm_sock = -1;

	midr_pm_open_sock(bgp);

	event_add_timer(bm->master, midr_pm_probe_timer, bgp,
			MIDR_PM_PROBE_INTERVAL, &mi->t_pm_probe);
}

void midr_pm_finish(struct bgp *bgp)
{
	struct bgp_midr *mi;

	if (!bgp || !bgp->midr_info)
		return;

	mi = bgp->midr_info;

	event_cancel(&mi->t_pm_probe);
	midr_pm_close_sock(bgp);

	if (mi->probe_contexts) {
		hash_clean(mi->probe_contexts, pm_ctx_free_cb);
		hash_free(mi->probe_contexts);
		mi->probe_contexts = NULL;
	}
}
