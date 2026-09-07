// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR Performance Measurement (PM) module.
 *
 * Implements real UDP round-trip probing:
 *   - One shared UDP socket bound to the local transport_addr (not INADDR_ANY)
 *   - Per-target probe contexts with their own probe/timeout timers
 *   - EWMA RTT (short-term α=0.2, long-term α=0.05)
 *   - Sliding-window loss rate (100 samples): the window itself counts
 *     round-trip (bidirectional) outcomes, since a single-socket probe
 *     can't tell which leg dropped a lost round trip. pm_loss_rate()
 *     converts that measured bidirectional rate L into a one-way estimate
 *     p = 1 - sqrt(1-L), assuming both directions are equally lossy — this
 *     one-way p is what's reported as loss_rate and used below.
 *   - Bandwidth score: sqrt(1.5) / (rtt_s * sqrt(max(loss, MIDR_PM_BW_SCORE_LOSS_FLOOR))),
 *     loss = one-way estimate above, floored at 1% so a clean link doesn't blow the score up
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
#include "monotime.h"
#include "prefix.h"
#include "hash.h"
#include "network.h"
#include "sockopt.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_midr_nds.h"
#include "bgpd/bgp_midr_pm.h"
#include "bgpd/bgp_midr_pm_net.h"

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
static struct midr_probe_ctx *pm_ctx_find(struct bgp_midr_nds *mi,
					  const struct prefix *node_id)
{
	struct midr_probe_ctx key = {};

	prefix_copy(&key.target, node_id);
	return hash_lookup(mi->probe_contexts, &key);
}

/*
 * Estimate the one-way (single-direction) loss rate from the sliding
 * window of probe round-trips.
 *
 * What the sliding window actually counts is round-trip (bidirectional)
 * outcomes: a sample is "received" only if the REQ made it to the
 * responder AND the REP made it back — a loss on either leg looks
 * identical (timeout) to the prober. So the raw window statistic is the
 * *bidirectional* loss rate L, not the one-way loss rate of the link.
 *
 * Assuming both directions have the same one-way loss probability p
 * (documented simplifying assumption — we have no way to distinguish
 * forward/reverse loss from a single-socket round-trip probe):
 *
 *     L = 1 - (1-p)^2   =>   p = 1 - sqrt(1-L)
 *
 * This derived one-way p is what gets reported as loss_rate (short-term
 * and long-term) and fed into bw_score — both are defined in terms of a
 * single link direction, not a round trip.
 */
static double pm_loss_rate(const struct midr_probe_ctx *ctx)
{
	uint32_t total = ctx->loss_win_total < MIDR_PM_LOSS_WINDOW_SIZE
				 ? ctx->loss_win_total
				 : MIDR_PM_LOSS_WINDOW_SIZE;
	uint32_t received = 0;
	uint32_t i;
	double bidir_loss;

	if (total == 0)
		return 0.0;

	for (i = 0; i < total; i++)
		received += ctx->loss_win[i];

	bidir_loss = 1.0 - (double)received / total;

	/* fmax() guards against a negative operand from floating-point
	 * rounding when bidir_loss is exactly 1.0. */
	return 1.0 - sqrt(fmax(0.0, 1.0 - bidir_loss));
}

/* Build and push an I-5 update to NDS. */
static void pm_push_i5(struct midr_probe_ctx *ctx,
		       enum midr_link_status status)
{
	struct bgp *bgp = ctx->bgp;
	struct midr_nds_link_metrics st = {}, lt = {};
	double loss = pm_loss_rate(ctx);
	double rtt_s = 0.0, bw_score = 0.0;

	if (ctx->st_init) {
		st.rtt_us = (uint32_t)ctx->st_rtt_us;
		st.rtt_min_us = ctx->st_rtt_min_us;
		st.rtt_max_us = ctx->st_rtt_max_us;
	}
	st.loss_rate = loss;

	/* Bandwidth score per stage1_design §2.2:
	 *   score = sqrt(1.5) / (rtt_s * sqrt(max(loss, MIDR_PM_BW_SCORE_LOSS_FLOOR)))
	 *
	 * loss is floored at 1% (not a near-zero epsilon) purely for this
	 * division: as loss -> 0 the score would otherwise blow up towards
	 * +inf on a clean link. The reported loss_rate itself (st.loss_rate /
	 * lt.loss_rate below) is NOT clamped — only the value plugged into
	 * this formula's denominator is. */
	if (ctx->st_init && ctx->st_rtt_us > 0) {
		rtt_s = ctx->st_rtt_us / 1e6;
		bw_score = sqrt(1.5)
			   / (rtt_s
			      * sqrt(fmax(loss, MIDR_PM_BW_SCORE_LOSS_FLOOR)));
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
				      * sqrt(fmax(ctx->lt_loss_rate,
						  MIDR_PM_BW_SCORE_LOSS_FLOOR)));
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
	struct bgp_midr_nds *mi = ctx->bgp->midr_nds_info;
	struct midr_probe_pkt pkt = {};
	struct ipaddr local_transport;
	union sockunion dst = {};
	uint64_t now;

	if (mi->pm_sock < 0)
		return;
	if (!midr_nds_local_transport_get(ctx->bgp, &local_transport)
	    || local_transport.ipa_type != ctx->target_addr.ipa_type
	    || !midr_pm_net_socket_matches(mi->pm_sock, &local_transport,
					   MIDR_PM_PROBE_PORT)
	    || !midr_pm_net_transport_to_sockunion(&ctx->target_addr,
					       MIDR_PM_PROBE_PORT, &dst))
		return;

	now = pm_now_us();

	pkt.magic = htonl(MIDR_PM_PROBE_MAGIC);
	pkt.type = MIDR_PM_PROBE_REQ;
	pkt.seqno = htonl(ctx->seqno);
	pkt.sent_us = htobe64(now);

	if (sendto(mi->pm_sock, &pkt, sizeof(pkt), 0,
		   &dst.sa, midr_pm_net_sockaddr_size(&dst)) < 0)
		MIDR_LOG("MIDR PM: sendto %pIA failed: %s",
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
 * O(n) — acceptable for a small peer set.
 *
 * A validated packet (REQ or REP) from a known source is itself a liveness
 * signal — refresh last_update so the entry's observation timestamp reflects
 * reality for "probe-only" responders too (a rep being probed by a joining
 * node never goes through I-5 for the requester's entry).
 *
 * 件④ 起 last_update 只是观测量，不再有老化判死；本刷新保留是为了让
 * show midr nodes 的 Age 列对这类只应答的对端也说实话。
 */
static bool pm_is_known_transport(struct bgp_midr_nds *mi,
				  const struct ipaddr *locator)
{
	struct midr_node_entry *entry;

	frr_each (midr_node_hash, &mi->global_view->nodes, entry) {
		if (entry->is_self)
			continue;
		if (entry->has_transport_addr
		    && midr_ipaddr_same(&entry->transport_addr, locator)) {
			entry->last_update = monotime(NULL);
			return true;
		}
	}
	return false;
}

/* Walk-callback: find a probe context whose target_addr matches a given IP. */
struct pm_ctx_find_by_ip_arg {
	struct ipaddr ip;
	struct midr_probe_ctx *result;
};

static void pm_ctx_find_by_ip_cb(struct hash_bucket *hb, void *arg)
{
	struct midr_probe_ctx *ctx = hb->data;
	struct pm_ctx_find_by_ip_arg *a = arg;

	if (a->result)
		return; /* already found */
	if (midr_ipaddr_same(&ctx->target_addr, &a->ip))
		a->result = ctx;
}

static void midr_pm_recv(struct event *t)
{
	struct bgp *bgp = EVENT_ARG(t);
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct midr_probe_pkt pkt;
	union sockunion src = {};
	struct ipaddr src_addr;
	struct ipaddr local_transport;
	socklen_t srclen = sizeof(src);
	ssize_t n;
	struct midr_probe_ctx *ctx;
	uint64_t rtt_us;

	/* Re-arm read event immediately so we don't miss packets while processing */
	event_add_read(bm->master, midr_pm_recv, bgp, mi->pm_sock,
		       &mi->t_pm_read);

	n = recvfrom(mi->pm_sock, &pkt, sizeof(pkt), MSG_TRUNC, &src.sa,
		     &srclen);
	if (n < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			zlog_warn("MIDR PM: recvfrom failed: %s",
				  safe_strerror(errno));
		return;
	}

	if (!midr_sockunion_to_ipaddr(&src, &src_addr)
	    || !midr_ipaddr_valid_locator(&src_addr)) {
		MIDR_LOG("MIDR PM: packet from invalid transport %pSU, dropped",
			 &src);
		return;
	}

	if (!midr_nds_local_transport_get(bgp, &local_transport)
	    || local_transport.ipa_type != src_addr.ipa_type
	    || !midr_pm_net_socket_matches(mi->pm_sock, &local_transport,
					   MIDR_PM_PROBE_PORT)) {
		MIDR_LOG("MIDR PM: packet from cross-family transport %pIA, dropped",
			 &src_addr);
		return;
	}

	if (!midr_pm_net_source_valid(&local_transport, &src_addr,
				      midr_pm_net_get_port(&src),
				      MIDR_PM_PROBE_PORT)) {
		MIDR_LOG("MIDR PM: packet from %pIA:%u has invalid source port, dropped",
			 &src_addr, midr_pm_net_get_port(&src));
		return;
	}

	if ((size_t)n != sizeof(struct midr_probe_pkt)) {
		MIDR_LOG("MIDR PM: invalid packet length (%zd B) from %pIA, dropped",
			 n, &src_addr);
		return;
	}

	if (ntohl(pkt.magic) != MIDR_PM_PROBE_MAGIC) {
		MIDR_LOG("MIDR PM: bad magic 0x%08x from %pIA, dropped",
			 ntohl(pkt.magic), &src_addr);
		return;
	}

	/*
	 * Security: source must be a transport_addr we recognise in global_view.
	 * Probes travel between transport_addrs (loopback /32s, e.g. 10.99.x.x),
	 * NOT between router-ids, so we cannot use the router-id hash directly —
	 * we need the O(n) transport_addr walk.
	 */
	if (!pm_is_known_transport(mi, &src_addr)) {
		MIDR_LOG("MIDR PM: packet from unknown transport %pIA, dropped",
			 &src_addr);
		return;
	}

	if (pkt.type == MIDR_PM_PROBE_REQ) {
		/* Responder role: echo the packet back with type = REP */
		pkt.type = MIDR_PM_PROBE_REP;
		if (sendto(mi->pm_sock, &pkt, sizeof(pkt), 0,
			   &src.sa, srclen) < 0)
			MIDR_LOG("MIDR PM: echo sendto %pIA failed: %s",
				 &src_addr, safe_strerror(errno));
		return;
	}

	if (pkt.type != MIDR_PM_PROBE_REP)
		return;

	/* Prober role: find the matching context by source transport_addr */
	if (!mi->probe_contexts)
		return;
	{
		struct pm_ctx_find_by_ip_arg arg = {
			.ip = src_addr,
			.result = NULL,
		};

		hash_iterate(mi->probe_contexts, pm_ctx_find_by_ip_cb, &arg);
		ctx = arg.result;
	}

	if (!ctx) {
		MIDR_LOG("MIDR PM: reply from %pIA has no matching probe context, dropped",
			 &src_addr);
		return;
	}

	if (!ctx->probe_outstanding) {
		MIDR_LOG("MIDR PM: late/duplicate reply from %pIA seqno=%u, dropped",
			 &src_addr, ntohl(pkt.seqno));
		return;
	}

	if (ntohl(pkt.seqno) != ctx->pending_seqno) {
		MIDR_LOG("MIDR PM: seqno mismatch from %pIA: got %u expected %u, dropped",
			 &src_addr, ntohl(pkt.seqno), ctx->pending_seqno);
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

	MIDR_LOG("MIDR PM: reply from %pIA rtt=%lluus", &src_addr,
		 (unsigned long long)rtt_us);

	pm_update_stats(ctx, rtt_us);
}

/* -------------------------------------------------------------------------
 * Socket lifecycle
 * ---------------------------------------------------------------------- */

static void midr_pm_open_sock(struct bgp *bgp)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	union sockunion local = {};
	struct ipaddr local_transport;
	int family;
	int sock;

	if (!midr_nds_local_transport_get(bgp, &local_transport)) {
		zlog_warn("MIDR PM: local transport-address not configured; "
			  "PM socket deferred until `midr transport-address` is set");
		return;
	}
	if (!midr_pm_net_transport_to_sockunion(&local_transport,
						MIDR_PM_PROBE_PORT, &local)) {
		zlog_warn("MIDR PM: invalid local transport %pIA",
			  &local_transport);
		return;
	}
	family = sockunion_family(&local);

	sock = socket(family, SOCK_DGRAM, 0);
	if (sock < 0) {
		zlog_warn("MIDR PM: socket() failed: %s", safe_strerror(errno));
		return;
	}

	sockopt_reuseaddr(sock);
	if (midr_pm_net_enable_v6only(family, sock) < 0) {
		zlog_warn("MIDR PM: cannot enable IPV6_V6ONLY: %s",
			  safe_strerror(errno));
		close(sock);
		return;
	}

	/* Bind to local_transport_addr only — NOT INADDR_ANY — to limit
	 * the attack surface to the MIDR loopback interface only. */
	if (bind(sock, &local.sa, midr_pm_net_sockaddr_size(&local)) < 0) {
		zlog_warn("MIDR PM: bind(%pIA:%u) failed: %s",
			  &local_transport, MIDR_PM_PROBE_PORT,
			  safe_strerror(errno));
		close(sock);
		return;
	}

	set_nonblocking(sock);
	mi->pm_sock = sock;
	event_add_read(bm->master, midr_pm_recv, bgp, sock, &mi->t_pm_read);

	MIDR_LOG("MIDR PM: probe socket ready on %pIA:%u", &local_transport,
		 MIDR_PM_PROBE_PORT);
}

static void midr_pm_close_sock(struct bgp *bgp)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;

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
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct midr_node_entry *entry;
	struct ipaddr local_transport;

	if (!midr_nds_local_transport_get(bgp, &local_transport))
		midr_pm_on_transport_addr_unset(bgp);
	else if (!midr_pm_net_socket_matches(mi->pm_sock, &local_transport,
					     MIDR_PM_PROBE_PORT))
		midr_pm_on_transport_addr_set(bgp);

	if (mi->pm_sock < 0)
		goto reschedule;

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
			struct midr_nds_link_metrics m = {};

			midr_nds_on_link_update(bgp, &entry->node_id,
						MIDR_LINK_UP, 0, &m, &m);
		}
	}

reschedule:
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
	struct bgp_midr_nds *mi;
	struct midr_probe_ctx *ctx;
	struct ipaddr local_transport;
	struct ipaddr target_transport;

	if (!bgp || !bgp->midr_nds_info || !node_id)
		return -1;
	mi = bgp->midr_nds_info;

	if (mi->pm_sock < 0) {
		MIDR_LOG("MIDR PM I-1: PM socket not ready, cannot probe %pFX",
			 node_id);
		return -1;
	}

	if (!midr_nds_local_transport_get(bgp, &local_transport)
	    || !midr_pm_net_socket_matches(mi->pm_sock, &local_transport,
					   MIDR_PM_PROBE_PORT)) {
		MIDR_LOG("MIDR PM I-1: local transport socket is not ready");
		return -1;
	}

	if (!midr_nds_node_transport_get(bgp, node_id, &target_transport)) {
		MIDR_LOG("MIDR PM I-1: node %pFX has no transport locator",
			 node_id);
		return -1;
	}
	if (local_transport.ipa_type != target_transport.ipa_type) {
		MIDR_LOG("MIDR PM I-1: node %pFX transport %pIA has a different address family",
			 node_id, &target_transport);
		return -1;
	}

	if (!mi->probe_contexts)
		mi->probe_contexts =
			hash_create(probe_ctx_hash_key, probe_ctx_hash_cmp,
				    "MIDR probe contexts");

	ctx = pm_ctx_find(mi, node_id);
	if (ctx && midr_ipaddr_same(&ctx->target_addr, &target_transport))
		return 0;
	if (ctx)
		midr_pm_remove_target(bgp, node_id,
				      MIDR_STOP_CLUSTER_CHANGE);

	ctx = XCALLOC(MTYPE_MIDR_PROBE_CTX, sizeof(*ctx));
	ctx->bgp = bgp;
	prefix_copy(&ctx->target, node_id);
	ctx->target_addr = target_transport;
	ctx->state = MIDR_PROBE_NORMAL;
	ctx->capabilities = capabilities;
	ctx->source = source;

	hash_get(mi->probe_contexts, ctx, hash_alloc_intern);

	/* Fire first probe immediately (delay = 0 seconds) */
	event_add_timer(bm->master, midr_pm_probe_timer_fn, ctx, 0,
			&ctx->t_probe);

	MIDR_LOG("MIDR PM I-1: start probing %pFX -> %pIA src=%d caps=0x%x",
		 node_id, &ctx->target_addr, source, capabilities);
	return 0;
}

int midr_pm_remove_target(struct bgp *bgp, const struct prefix *node_id,
			  enum midr_stop_reason reason)
{
	struct bgp_midr_nds *mi;
	struct midr_probe_ctx *ctx;

	if (!bgp || !bgp->midr_nds_info || !node_id)
		return -1;
	mi = bgp->midr_nds_info;

	if (!mi->probe_contexts)
		return -1;

	ctx = pm_ctx_find(mi, node_id);
	if (!ctx)
		return -1;

	event_cancel(&ctx->t_probe);
	event_cancel(&ctx->t_timeout);

	/* If a probe was in-flight, push LINK_DOWN so NDS/CL can react */
	if (ctx->probe_outstanding) {
		struct midr_nds_link_metrics zero = {};

		midr_nds_on_link_update(bgp, node_id, MIDR_LINK_DOWN,
					ctx->consecutive_failures, &zero, &zero);
	}

	hash_release(mi->probe_contexts, ctx);
	XFREE(MTYPE_MIDR_PROBE_CTX, ctx);

	MIDR_LOG("MIDR PM I-2: stop probing %pFX reason=%d", node_id, reason);
	return 0;
}

/*
 * I-2 全量版：停掉本实例的**所有**探测目标，返回停掉的个数。
 *
 * 退网（`midr shutdown`）专用。为什么不逐个调 midr_pm_remove_target：探测目标不
 * 都在节点表里——join 期对群代表/成员起的探测用的是占位条目，锚点评估探的是次
 * 优群代表，按节点表遍历会漏，漏掉的就是永远停不下来的探测流。
 *
 * 用 hash_clean 而不是逐条 remove，还顺带避开一个副作用：单条版对"探测在途"的
 * 目标会推一发 LINK_DOWN 回灌 I-5，退网时那是纯噪声（链路事实的作废由退网清表
 * 路径统一发 link withdraw，见 midr_nds_detach_node）。
 * 清空后哈希表本身留着继续用（重入后照常 add_target），与 midr_pm_finish 的
 * hash_clean + hash_free 不同。
 */
int midr_pm_remove_all_targets(struct bgp *bgp, enum midr_stop_reason reason)
{
	struct bgp_midr_nds *mi;
	int n;

	if (!bgp || !bgp->midr_nds_info)
		return 0;
	mi = bgp->midr_nds_info;
	if (!mi->probe_contexts)
		return 0;

	n = (int)hashcount(mi->probe_contexts);
	if (!n)
		return 0;

	hash_clean(mi->probe_contexts, pm_ctx_free_cb);
	MIDR_LOG("MIDR PM I-2: stop probing ALL (%d targets) reason=%d", n,
		 reason);
	return n;
}

/*
 * Called by the VTY `midr transport-address` handler after the address is set.
 * The PM socket cannot be opened during midr_pm_init() because the config file
 * is read AFTER bgp_midr_nds_init() runs.  This function retrofits the open and
 * rescans global_view for neighbors whose I-1 call was silently dropped because
 * the socket was not ready.
 */
void midr_pm_on_transport_addr_set(struct bgp *bgp)
{
	struct bgp_midr_nds *mi;
	struct midr_node_entry *entry;
	struct ipaddr local_transport;

	if (!bgp || !bgp->midr_nds_info)
		return;

	mi = bgp->midr_nds_info;
	if (!midr_nds_local_transport_get(bgp, &local_transport)) {
		midr_pm_on_transport_addr_unset(bgp);
		return;
	}

	if (mi->pm_sock >= 0
	    && !midr_pm_net_socket_matches(mi->pm_sock, &local_transport,
					   MIDR_PM_PROBE_PORT)) {
		MIDR_LOG("MIDR PM: rebinding probe socket to %pIA",
			 &local_transport);
		midr_pm_close_sock(bgp);
		midr_pm_remove_all_targets(bgp, MIDR_STOP_CLUSTER_CHANGE);
	}

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

void midr_pm_on_transport_addr_unset(struct bgp *bgp)
{
	struct bgp_midr_nds *mi;

	if (!bgp || !bgp->midr_nds_info)
		return;

	mi = bgp->midr_nds_info;
	if (mi->pm_sock < 0
	    && (!mi->probe_contexts || hashcount(mi->probe_contexts) == 0))
		return;

	midr_pm_close_sock(bgp);
	midr_pm_remove_all_targets(bgp, MIDR_STOP_CLUSTER_CHANGE);
	MIDR_LOG("MIDR PM: probe socket closed after transport removal");
}

void midr_pm_init(struct bgp *bgp)
{
	struct bgp_midr_nds *mi;

	if (!bgp || !bgp->midr_nds_info)
		return;

	mi = bgp->midr_nds_info;
	mi->pm_sock = -1;

	midr_pm_open_sock(bgp);

	event_add_timer(bm->master, midr_pm_probe_timer, bgp,
			MIDR_PM_PROBE_INTERVAL, &mi->t_pm_probe);
}

void midr_pm_finish(struct bgp *bgp)
{
	struct bgp_midr_nds *mi;

	if (!bgp || !bgp->midr_nds_info)
		return;

	mi = bgp->midr_nds_info;

	event_cancel(&mi->t_pm_probe);
	midr_pm_close_sock(bgp);

	if (mi->probe_contexts) {
		hash_clean(mi->probe_contexts, pm_ctx_free_cb);
		hash_free(mi->probe_contexts);
		mi->probe_contexts = NULL;
	}
}
