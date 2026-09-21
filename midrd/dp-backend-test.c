/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Third-group data-plane tests: Zebra backend, GRE provisioning and the SPF
 * adapter integration.
 *
 * zclient_route_send() is wrapped so the encoded zapi_route can be inspected
 * without a running zebra.  Real ZAPI/Linux-FIB, GRE netdevice and containerlab
 * coverage lives in the shell/harness tests; see doc/midr-doc/dp-doc/.
 */

#define main midrd_program_main
#include "midrd.c"
#undef main

#include <assert.h>
#include <arpa/inet.h>

#include "frrdistance.h"
#include "ipaddr.h"
#include "nexthop.h"
#include "prefix.h"
#include "srv6.h"
#include "zclient.h"

#include "midr-dp-backend.h"
#include "midr-gre.h"
#include "midr-spf-install.h"
#include "midr-zebra.h"

/* ------------------------------------------------------------------ *
 *  Captured ZAPI traffic
 * ------------------------------------------------------------------ */
#define MIDR_TEST_MAX_SENDS 32

struct zapi_capture {
	uint8_t cmd;
	struct zapi_route api;
};

static struct zapi_capture captures[MIDR_TEST_MAX_SENDS];
static size_t capture_count;
static long fail_skip_sends;   /* succeed this many attempts first */
static long fail_next_sends;   /* then fail this many (-1 handled below) */
static bool fail_forever;      /* when true, fail every attempt from now on */

enum zclient_send_status __wrap_zclient_route_send(uint8_t cmd,
						   struct zclient *zc,
						   struct zapi_route *api)
{
	(void)zc;
	if (fail_skip_sends > 0) {
		fail_skip_sends--;
	} else if (fail_forever || fail_next_sends > 0) {
		if (!fail_forever)
			fail_next_sends--;
		return ZCLIENT_SEND_FAILURE;
	}
	if (capture_count < MIDR_TEST_MAX_SENDS) {
		captures[capture_count].cmd = cmd;
		captures[capture_count].api = *api;
		capture_count++;
	}
	return ZCLIENT_SEND_SUCCESS;
}

/* Fail exactly the next send attempt. */
static void fail_one_send(void)
{
	fail_skip_sends = 0;
	fail_next_sends = 1;
	fail_forever = false;
}

/* Succeed @ok send attempts, then fail @bad more; @bad < 0 means forever. */
static void fail_sends_after(long ok, long bad)
{
	fail_skip_sends = ok;
	fail_forever = bad < 0;
	fail_next_sends = bad < 0 ? 0 : bad;
}

/* Let every future send succeed again. */
static void allow_sends(void)
{
	fail_skip_sends = 0;
	fail_next_sends = 0;
	fail_forever = false;
}

static void capture_reset(void)
{
	memset(captures, 0, sizeof(captures));
	capture_count = 0;
	allow_sends();
}

static struct midr_dp_status dp_status(void)
{
	struct midr_dp_status status;

	midr_dp_backend_status_get(&status);
	return status;
}

/*
 * Pretend zebra is connected.  zclient_init() leaves the socket at -1 and the
 * send itself is wrapped, so no real I/O is performed.
 */
static void backend_fake_socket(void)
{
	struct zclient *zc = midr_dp_backend_zclient();

	assert(zc);
	zc->sock = 0;
}

static void backend_fake_reconnect(void)
{
	struct zclient *zc = midr_dp_backend_zclient();

	assert(zc);
	zc->sock = 0;
	zc->zebra_connected(zc);
}

static void ted_create(struct midr_context *ctx)
{
	struct midr_ted_config config = {
		.max_events = 16,
	};

	memset(ctx, 0, sizeof(*ctx));
	ctx->node_id = 1;
	assert(midr_ted_create(&config, &ctx->ted) == 0);
}

static struct prefix prefix_v4(const char *text, uint8_t prefixlen)
{
	struct prefix p = {
		.family = AF_INET,
		.prefixlen = prefixlen,
	};

	(void)inet_pton(AF_INET, text, &p.u.prefix4);
	apply_mask(&p);
	return p;
}

static void path_v4(struct midr_path *path, const char *addr, uint32_t ifindex,
		    uint32_t metric, uint8_t weight)
{
	path->ifindex = ifindex;
	path->metric = metric;
	path->weight = weight;
	path->path_avail_bw = 0.0f;
	SET_IPADDR_V4(&path->nexthop);
	(void)inet_pton(AF_INET, addr, &path->nexthop.ipaddr_v4);
}

static void path_v6(struct midr_path *path, const char *addr, uint32_t ifindex,
		    uint32_t metric)
{
	path->ifindex = ifindex;
	path->metric = metric;
	SET_IPADDR_V6(&path->nexthop);
	(void)inet_pton(AF_INET6, addr, &path->nexthop.ipaddr_v6);
}

/*
 * =========================================================================
 *  ZAPI encoding: BASIC, ECMP, UCMP, SRv6, dual instance
 * =========================================================================
 */
static void test_encoding_modes(void)
{
	struct midr_context ctx;
	struct midr_dp_status dp;
	struct prefix p = prefix_v4("10.10.0.0", 24);
	struct prefix p6 = {
		.family = AF_INET6,
		.prefixlen = 64,
	};
	struct midr_path path = {};
	struct midr_path ecmp[2] = {};
	struct midr_path sid_path = {};
	struct midr_path_result result;
	struct midr_path_result ecmp_result = {
		.paths = ecmp,
		.path_count = 2,
		.instance = MIDR_INSTANCE_SPF,
	};
	struct midr_path_result te_result = {
		.paths = &sid_path,
		.path_count = 1,
		.instance = MIDR_INSTANCE_TE,
	};
	struct in6_addr sid1, sid2;
	struct zapi_route *api;

	ted_create(&ctx);
	ctx.master = event_master_create("midrd-dp-test");
	assert(ctx.master);
	capture_reset();
	assert(midr_dp_backend_start(&ctx, ctx.master, VRF_DEFAULT) == 0);
	backend_fake_socket();

	/* --- BASIC: one IPv4 nexthop carrying an explicit ifindex -------- */
	path_v4(&path, "192.0.2.1", 7, 42, 0);
	result.paths = &path;
	result.path_count = 1;
	result.instance = MIDR_INSTANCE_SPF;

	assert(midr_zebra_route_add(&ctx, &p, &result) == 0);
	dp = dp_status();
	assert(dp.pending == 1 && dp.add_staged == 1);
	assert(midr_zebra_route_flush(&ctx) == 0);
	assert(capture_count == 1);
	api = &captures[0].api;
	assert(captures[0].cmd == ZEBRA_ROUTE_ADD);
	assert(api->type == ZEBRA_ROUTE_BGP_MIDR);
	assert(api->instance == MIDR_INSTANCE_SPF);
	assert(api->metric == 42);
	assert(api->distance == ZEBRA_BGP_MIDR_DISTANCE_DEFAULT);
	assert(CHECK_FLAG(api->flags, ZEBRA_FLAG_ALLOW_RECURSION));
	assert(!CHECK_FLAG(api->flags, ZEBRA_FLAG_USE_RECURSIVE_WEIGHT));
	assert(api->nexthop_num == 1);
	assert(api->nexthops[0].type == NEXTHOP_TYPE_IPV4_IFINDEX);
	assert(api->nexthops[0].ifindex == 7);
	assert(api->nexthops[0].weight == 0);
	assert(prefix_same(&api->prefix, &p));
	dp = dp_status();
	assert(dp.installed == 1 && dp.route_adds == 1);

	/* Identical forwarding state is not re-sent (diff no-op). */
	assert(midr_zebra_route_add(&ctx, &p, &result) == 0);
	assert(midr_zebra_route_flush(&ctx) == 0);
	assert(capture_count == 1);
	assert(dp_status().installed == 1);

	/* A changed nexthop replaces the old route. */
	path_v4(&path, "192.0.2.9", 7, 42, 0);
	assert(midr_zebra_route_add(&ctx, &p, &result) == 0);
	assert(midr_zebra_route_flush(&ctx) == 0);
	assert(capture_count == 3);
	assert(captures[1].cmd == ZEBRA_ROUTE_DELETE);
	assert(captures[2].cmd == ZEBRA_ROUTE_ADD);

	/*
	 * Metric is a zebra tie-breaker and is not part of the forwarding-state
	 * comparison, so a bare re-add is a no-op; the SPF adapter expresses a
	 * metric-only change as an explicit DELETE + ADD, which the backend
	 * then applies verbatim.
	 */
	path_v4(&path, "192.0.2.9", 7, 99, 0);
	assert(midr_zebra_route_add(&ctx, &p, &result) == 0);
	assert(midr_zebra_route_flush(&ctx) == 0);
	assert(capture_count == 3);
	assert(midr_zebra_route_del(&ctx, &p, MIDR_INSTANCE_SPF) == 0);
	assert(midr_zebra_route_add(&ctx, &p, &result) == 0);
	assert(midr_zebra_route_flush(&ctx) == 0);
	assert(capture_count == 5);
	assert(captures[3].cmd == ZEBRA_ROUTE_DELETE);
	assert(captures[4].cmd == ZEBRA_ROUTE_ADD);
	assert(captures[4].api.metric == 99);
	assert(dp_status().installed == 1);

	/* --- ECMP then UCMP over the same prefix ------------------------- */
	path_v4(&ecmp[0], "192.0.2.11", 0, 5, 0);
	path_v4(&ecmp[1], "192.0.2.12", 0, 5, 0);
	assert(midr_zebra_route_add(&ctx, &p, &ecmp_result) == 0);
	assert(midr_zebra_route_flush(&ctx) == 0);
	api = &captures[capture_count - 1].api;
	assert(api->nexthop_num == 2);
	/* ifindex 0 lets zebra resolve the locator recursively. */
	assert(api->nexthops[0].type == NEXTHOP_TYPE_IPV4);
	assert(api->nexthops[1].type == NEXTHOP_TYPE_IPV4);
	assert(!CHECK_FLAG(api->flags, ZEBRA_FLAG_USE_RECURSIVE_WEIGHT));

	ecmp[0].weight = 3;
	ecmp[1].weight = 1;
	assert(midr_zebra_route_add(&ctx, &p, &ecmp_result) == 0);
	assert(midr_zebra_route_flush(&ctx) == 0);
	api = &captures[capture_count - 1].api;
	assert(api->nexthops[0].weight == 3 && api->nexthops[1].weight == 1);
	assert(CHECK_FLAG(api->nexthops[0].flags, ZAPI_NEXTHOP_FLAG_WEIGHT));
	assert(CHECK_FLAG(api->nexthops[1].flags, ZAPI_NEXTHOP_FLAG_WEIGHT));
	assert(CHECK_FLAG(api->flags, ZEBRA_FLAG_USE_RECURSIVE_WEIGHT));

	/* --- SRv6 TE override -------------------------------------------- */
	path_v6(&sid_path, "2001:db8::1", 9, 10);
	(void)inet_pton(AF_INET6, "2001:db8:100::1", &sid1);
	(void)inet_pton(AF_INET6, "2001:db8:100::2", &sid2);
	te_result.explicit.sid_count = 2;
	te_result.explicit.sid_list[0] = sid1;
	te_result.explicit.sid_list[1] = sid2;
	(void)inet_pton(AF_INET6, "2001:db8:200::", &p6.u.prefix6);
	assert(midr_zebra_route_add(&ctx, &p6, &te_result) == 0);
	assert(midr_zebra_route_flush(&ctx) == 0);
	api = &captures[capture_count - 1].api;
	assert(api->instance == MIDR_INSTANCE_TE);
	assert(api->metric == 1);
	assert(api->nexthop_num == 1);
	assert(api->nexthops[0].type == NEXTHOP_TYPE_IPV6_IFINDEX);
	assert(CHECK_FLAG(api->nexthops[0].flags, ZAPI_NEXTHOP_FLAG_SEG6));
	assert(api->nexthops[0].seg_num == 2);
	assert(api->nexthops[0].srv6_encap_behavior ==
	       SRV6_HEADEND_BEHAVIOR_H_INSERT);
	assert(memcmp(api->nexthops[0].seg6_segs, &sid1, sizeof(sid1)) == 0);
	assert(dp_status().installed == 2);

	/* Both instances coexist for one prefix; deleting TE keeps SPF. */
	result.paths = &path;
	result.path_count = 1;
	result.instance = MIDR_INSTANCE_TE;
	assert(midr_zebra_route_add(&ctx, &p, &result) == 0);
	assert(midr_zebra_route_flush(&ctx) == 0);
	assert(captures[capture_count - 1].api.instance == MIDR_INSTANCE_TE);
	assert(dp_status().installed == 3);
	assert(midr_zebra_route_del(&ctx, &p, MIDR_INSTANCE_TE) == 0);
	assert(midr_zebra_route_flush(&ctx) == 0);
	assert(captures[capture_count - 1].cmd == ZEBRA_ROUTE_DELETE);
	assert(captures[capture_count - 1].api.instance == MIDR_INSTANCE_TE);
	assert(dp_status().installed == 2);

	/* Illegal input is rejected before anything is queued. */
	assert(midr_zebra_route_del(&ctx, &p, 7) == -EINVAL);
	result.instance = 7;
	assert(midr_zebra_route_add(&ctx, &p, &result) == -EINVAL);
	result.instance = MIDR_INSTANCE_SPF;

	/* An unusable nexthop fails the submission instead of installing a
	 * blackhole: the previously installed TE route was already deleted, so
	 * the hash truthfully reports it as gone, and the failed operation
	 * stays queued for the caller to abort (or for a resync to rebuild). */
	path_v6(&sid_path, "fe80::1", 0, 10);
	assert(midr_zebra_route_add(&ctx, &p6, &te_result) == 0);
	assert(midr_zebra_route_flush(&ctx) == -EHOSTUNREACH);
	dp = dp_status();
	assert(dp.installed == 1 && dp.pending == 1);
	midr_zebra_route_abort(&ctx);
	assert(dp_status().pending == 0);

	midr_dp_backend_stop();
	assert(midr_dp_backend_get() == NULL);
	midr_ted_destroy(&ctx.ted);
}

/*
 * =========================================================================
 *  Rejected submissions never fake installed state
 * =========================================================================
 */
static void test_send_failure_and_abort(void)
{
	struct midr_context ctx;
	struct midr_dp_status dp;
	struct prefix p = prefix_v4("10.20.0.0", 24);
	struct midr_path path = {};
	struct midr_path_result result;

	ted_create(&ctx);
	ctx.master = event_master_create("midrd-dp-test");
	capture_reset();
	assert(midr_dp_backend_start(&ctx, ctx.master, VRF_DEFAULT) == 0);
	backend_fake_socket();

	path_v4(&path, "192.0.2.1", 5, 10, 0);
	result.paths = &path;
	result.path_count = 1;
	result.instance = MIDR_INSTANCE_SPF;

	/* A rejected send is reported and never recorded as installed. */
	fail_one_send();
	assert(midr_zebra_route_add(&ctx, &p, &result) == 0);
	assert(midr_zebra_route_flush(&ctx) == -EIO);
	dp = dp_status();
	assert(dp.installed == 0 && dp.route_adds == 0 && dp.send_failures == 1);
	/* The failing operation stays queued until the caller aborts. */
	assert(dp.pending == 1);
	midr_zebra_route_abort(&ctx);
	assert(dp_status().pending == 0);

	/* The next submission succeeds and is recorded. */
	assert(midr_zebra_route_add(&ctx, &p, &result) == 0);
	assert(midr_zebra_route_flush(&ctx) == 0);
	assert(dp_status().installed == 1);

	/* A rejected delete keeps the route installed. */
	fail_one_send();
	assert(midr_zebra_route_del(&ctx, &p, MIDR_INSTANCE_SPF) == 0);
	assert(midr_zebra_route_flush(&ctx) == -EIO);
	dp = dp_status();
	assert(dp.installed == 1 && dp.send_failures == 2);
	midr_zebra_route_abort(&ctx);

	/* A disconnected zclient is reported as such.  Remove the still-installed
	 * route first so the next add is a real change. */
	assert(midr_zebra_route_del(&ctx, &p, MIDR_INSTANCE_SPF) == 0);
	assert(midr_zebra_route_flush(&ctx) == 0);
	assert(dp_status().installed == 0);
	midr_dp_backend_zclient()->sock = -1;
	assert(midr_zebra_route_add(&ctx, &p, &result) == 0);
	assert(midr_zebra_route_flush(&ctx) == -ENOTCONN);
	midr_zebra_route_abort(&ctx);
	backend_fake_socket();

	midr_dp_backend_stop();
	midr_ted_destroy(&ctx.ted);
}

/*
 * =========================================================================
 *  Adapter + backend pipeline over the committed TED
 * =========================================================================
 *  Covers READY install, same-generation resync, reconnect replay,
 *  deferred send failure recovery and NOT_READY withdrawal.
 */
static void noop_cb(struct event *t)
{
	(void)t;
}

/* Service the FRR event loop for @ms so deferred/recovery timers can fire. */
static void pump_events(struct event_loop *master, uint32_t ms)
{
	uint64_t deadline = mono_ms() + ms;

	while (mono_ms() < deadline) {
		struct event *wake = NULL;
		struct event ev = {};

		event_add_timer_msec(master, noop_cb, NULL, 20, &wake);
		event_fetch(master, &ev);
		event_call(&ev);
		event_cancel(&wake);
	}
}

static void test_adapter_pipeline_and_recovery(void)
{
	struct midr_context ctx;
	struct midr_dp_status dp;
	struct midr_consumer_event events[2] = {0};
	struct midr_consumer_snapshot snapshot = {
		.generation = 7,
		.count = 2,
		.events = events,
	};

	ted_create(&ctx);
	ctx.master = event_master_create("midrd-dp-test");
	capture_reset();
	assert(midr_dp_backend_start(&ctx, ctx.master, VRF_DEFAULT) == 0);
	backend_fake_socket();

	/* TED READY: the adapter derives SPF results and stages one add. */
	events[0].kind = MIDR_CONSUMER_LINK;
	events[0].generation = 7;
	events[0].originator = 1;
	events[0].remote = 2;
	events[0].link_id = 1;
	events[0].family = MIDR_CORE_AF_IPV4;
	events[0].metric = 5;
	events[0].remote_address[0] = 192;
	events[0].remote_address[1] = 0;
	events[0].remote_address[2] = 2;
	events[0].remote_address[3] = 2;
	events[1].kind = MIDR_CONSUMER_NODE_PREFIX;
	events[1].generation = 7;
	events[1].originator = 2;
	events[1].family = MIDR_CORE_AF_IPV4;
	events[1].prefix_len = 24;
	events[1].prefix[0] = 198;
	events[1].prefix[1] = 51;
	events[1].prefix[2] = 100;
	events[1].metric = 3;
	assert(midr_ted_apply_snapshot(ctx.ted, &snapshot) == 0);

	/*
	 * Cold path: this is the first batch after the backend started, so
	 * update_deferred() submits it at once instead of waiting for the 100 ms
	 * window (capture[0]).
	 */
	dp = dp_status();
	assert(dp.immediate_next == false);
	assert(capture_count == 1);
	assert(captures[0].cmd == ZEBRA_ROUTE_ADD);
	assert(captures[0].api.type == ZEBRA_ROUTE_BGP_MIDR);
	assert(captures[0].api.instance == MIDR_INSTANCE_SPF);
	assert(dp.installed == 1 && dp.pending == 0);

	/* Same generation: resync is a no-op. */
	assert(midr_spf_install_resync(&ctx) == 0);
	assert(capture_count == 1);

	/* zebra reconnect: drop the local hash and replay the same generation
	 * from an empty baseline, submitted immediately again. */
	backend_fake_reconnect();
	dp = dp_status();
	assert(dp.replays == 1);
	assert(capture_count == 2);
	assert(captures[1].cmd == ZEBRA_ROUTE_ADD);
	assert(dp.installed == 1 && dp.pending == 0);

	/*
	 * A new generation whose first send fails inside the deferred timer:
	 * the DELETE half of the metric-change diff (DELETE + ADD) is sent and
	 * rejected, so the incomplete batch is retained and the recovery timer
	 * retries it.  The adapter already advanced its desired generation, so
	 * only the retained-batch retry can converge the FIB; the resync that
	 * follows is the spec-visible reconciliation and is a no-op here.
	 */
	fail_one_send();
	events[0].metric = 6;
	events[1].metric = 4;
	events[0].generation = 8;
	events[1].generation = 8;
	snapshot.generation = 8;
	assert(midr_ted_apply_snapshot(ctx.ted, &snapshot) == 0);

	/* Steady state is still batched: the metric-only change stages DELETE +
	 * ADD and waits for the 100 ms window instead of being submitted at
	 * once. */
	dp = dp_status();
	assert(capture_count == 2 && dp.pending == 2);

	pump_events(ctx.master, 500);
	dp = dp_status();

	/* The metric-only change is expressed as DELETE + ADD; the retained
	 * batch was retried and fully applied: capture[2] deletes the old
	 * metric route, capture[3] adds the new one (capture[0]/[1] are the
	 * earlier READY add and reconnect replay). */
	assert(capture_count == 4);
	assert(captures[2].cmd == ZEBRA_ROUTE_DELETE);
	assert(captures[3].cmd == ZEBRA_ROUTE_ADD);

	/* Only the first submission failed (send_failures counts every failed
	 * submission): the recovery retry succeeded, so nothing is left
	 * pending, the installed hash converged to one route and no residual
	 * error remains. */
	assert(dp.send_failures == 1);
	assert(dp.pending == 0);
	assert(dp.installed == 1);
	assert(dp.last_error == 0);

	/* Exactly one midr_spf_install_resync() call from the recovery cycle;
	 * it is a no-op because the adapter's desired generation already
	 * matches the committed one (that is the defect being covered: the
	 * resync alone could not have fixed the stale FIB). */
	assert(dp.resyncs == 1);

	/* TED NOT_READY withdraws every accepted route. */
	assert(midr_ted_invalidate(ctx.ted, -EIO) == 0);
	assert(midr_zebra_route_flush(&ctx) == 0);
	assert(captures[capture_count - 1].cmd == ZEBRA_ROUTE_DELETE);
	assert(dp_status().installed == 0);

	/* Shutdown order: unregister withdraws and flushes, then the zclient
	 * and the private state are destroyed. */
	assert(midr_zebra_backend_unregister(&ctx) == 0);
	assert(midr_dp_backend_get() != NULL);
	midr_dp_backend_stop();
	assert(midr_dp_backend_get() == NULL);
	assert(!midr_dp_backend_ready());
	midr_ted_destroy(&ctx.ted);
}

/*
 * =========================================================================
 *  Deferred-batch failure consistency (review 2.2)
 * =========================================================================
 *  Partial and whole-batch failures, failure across a zebra reconnect and a
 *  new desired generation arriving while recovery is in flight.  Each case
 *  asserts observable state: the adapter's desired generation, the
 *  installed-hash size, the pending queue and the number of re-sent ZAPI
 *  operations.
 */

/* Fill a two-event TED snapshot (one link + one node prefix) for @generation.
 * A changed link/prefix metric makes the SPF adapter produce a metric-only
 * route change, which it expresses as DELETE + ADD. */
static void snapshot_two_routes(struct midr_consumer_snapshot *snapshot,
				struct midr_consumer_event *events,
				uint64_t generation, uint32_t link_metric,
				uint32_t prefix_metric)
{
	memset(events, 0, sizeof(*events) * 2);
	snapshot->generation = generation;
	snapshot->count = 2;
	snapshot->events = events;

	events[0].kind = MIDR_CONSUMER_LINK;
	events[0].generation = generation;
	events[0].originator = 1;
	events[0].remote = 2;
	events[0].link_id = 1;
	events[0].family = MIDR_CORE_AF_IPV4;
	events[0].metric = link_metric;
	events[0].remote_address[0] = 192;
	events[0].remote_address[1] = 0;
	events[0].remote_address[2] = 2;
	events[0].remote_address[3] = 2;

	events[1].kind = MIDR_CONSUMER_NODE_PREFIX;
	events[1].generation = generation;
	events[1].originator = 2;
	events[1].family = MIDR_CORE_AF_IPV4;
	events[1].prefix_len = 24;
	events[1].prefix[0] = 198;
	events[1].prefix[1] = 51;
	events[1].prefix[2] = 100;
	events[1].metric = prefix_metric;
}

/* Gen 7 is applied on the cold path (immediate submit). */
static void stage_gen7(struct midr_context *ctx,
		       struct midr_consumer_snapshot *snapshot,
		       struct midr_consumer_event *events)
{
	snapshot_two_routes(snapshot, events, 7, 5, 3);
	assert(midr_ted_apply_snapshot(ctx->ted, snapshot) == 0);
}

static void test_deferred_partial_batch_failure(void)
{
	struct midr_context ctx;
	struct midr_dp_status dp;
	struct midr_consumer_event events[2] = {0};
	struct midr_consumer_snapshot snapshot;

	ted_create(&ctx);
	ctx.master = event_master_create("midrd-dp-test");
	capture_reset();
	assert(midr_dp_backend_start(&ctx, ctx.master, VRF_DEFAULT) == 0);
	backend_fake_socket();

	stage_gen7(&ctx, &snapshot, events);
	dp = dp_status();
	assert(dp.installed == 1 && dp.pending == 0 && capture_count == 1);

	/* Gen 8 metric-only change: the DELETE succeeds and the ADD fails, and
	 * sends keep failing so the retained state is observable. */
	fail_sends_after(1, -1);
	snapshot_two_routes(&snapshot, events, 8, 6, 4);
	assert(midr_ted_apply_snapshot(ctx.ted, &snapshot) == 0);
	assert(dp_status().pending == 2);

	pump_events(ctx.master, 300);
	dp = dp_status();
	/* Partial success: the DELETE was applied, the ADD is retained. */
	assert(capture_count == 2);
	assert(captures[1].cmd == ZEBRA_ROUTE_DELETE);
	assert(dp.installed == 0 && dp.pending == 1);
	assert(dp.recovering && !dp.recovery_stalled);
	assert(dp.last_error == -EIO);

	/* Recovery applies the retained remainder and reconciles. */
	allow_sends();
	pump_events(ctx.master, 300);
	dp = dp_status();
	assert(capture_count == 3);
	assert(captures[2].cmd == ZEBRA_ROUTE_ADD);
	assert(dp.installed == 1 && dp.pending == 0);
	assert(!dp.recovering && dp.last_error == 0);
	assert(dp.resyncs == 1);

	midr_dp_backend_stop();
	assert(midr_dp_backend_get() == NULL);
	midr_ted_destroy(&ctx.ted);
}

static void test_deferred_whole_batch_failure(void)
{
	struct midr_context ctx;
	struct midr_dp_status dp;
	struct midr_consumer_event events[2] = {0};
	struct midr_consumer_snapshot snapshot;

	ted_create(&ctx);
	ctx.master = event_master_create("midrd-dp-test");
	capture_reset();
	assert(midr_dp_backend_start(&ctx, ctx.master, VRF_DEFAULT) == 0);
	backend_fake_socket();

	stage_gen7(&ctx, &snapshot, events);
	assert(dp_status().installed == 1 && capture_count == 1);

	/* Every send fails: the whole DELETE + ADD batch is retained. */
	fail_sends_after(0, -1);
	snapshot_two_routes(&snapshot, events, 8, 6, 4);
	assert(midr_ted_apply_snapshot(ctx.ted, &snapshot) == 0);
	assert(dp_status().pending == 2);

	pump_events(ctx.master, 300);
	dp = dp_status();
	/* Nothing was applied; the previous route is still installed. */
	assert(capture_count == 1);
	assert(dp.installed == 1 && dp.pending == 2);
	assert(dp.recovering && dp.last_error == -EIO);

	/* abort_pending() from a later rejected round must not clobber the
	 * retained remainder: it is the only replay of the missing installs. */
	midr_zebra_route_abort(&ctx);
	dp = dp_status();
	assert(dp.installed == 1 && dp.pending == 2 && dp.recovering);

	allow_sends();
	pump_events(ctx.master, 300);
	dp = dp_status();
	assert(capture_count == 3);
	assert(captures[1].cmd == ZEBRA_ROUTE_DELETE);
	assert(captures[2].cmd == ZEBRA_ROUTE_ADD);
	assert(dp.installed == 1 && dp.pending == 0);
	assert(!dp.recovering && dp.last_error == 0);

	midr_dp_backend_stop();
	assert(midr_dp_backend_get() == NULL);
	midr_ted_destroy(&ctx.ted);
}

static void test_deferred_failure_across_reconnect(void)
{
	struct midr_context ctx;
	struct midr_dp_status dp;
	struct midr_consumer_event events[2] = {0};
	struct midr_consumer_snapshot snapshot;

	ted_create(&ctx);
	ctx.master = event_master_create("midrd-dp-test");
	capture_reset();
	assert(midr_dp_backend_start(&ctx, ctx.master, VRF_DEFAULT) == 0);
	backend_fake_socket();

	stage_gen7(&ctx, &snapshot, events);
	assert(dp_status().installed == 1 && capture_count == 1);

	/* Gen 8 fails and is retained; recovery is in flight. */
	fail_sends_after(0, -1);
	snapshot_two_routes(&snapshot, events, 8, 6, 4);
	assert(midr_ted_apply_snapshot(ctx.ted, &snapshot) == 0);
	pump_events(ctx.master, 200);
	dp = dp_status();
	assert(dp.pending == 2 && dp.installed == 1 && dp.recovering);

	/*
	 * zebra reconnects: the backend drops the stale installed hash and
	 * replays the whole desired set; the retained batch is re-sent first
	 * (its DELETE is a no-op now that the local hash is empty).
	 */
	allow_sends();
	backend_fake_reconnect();
	pump_events(ctx.master, 200);
	dp = dp_status();
	assert(dp.replays == 1);
	assert(dp.installed == 1 && dp.pending == 0);
	assert(!dp.recovering && dp.last_error == 0);
	assert(capture_count == 2);
	assert(captures[1].cmd == ZEBRA_ROUTE_ADD);
	assert(dp.resyncs == 1);

	midr_dp_backend_stop();
	assert(midr_dp_backend_get() == NULL);
	midr_ted_destroy(&ctx.ted);
}

/*
 * =========================================================================
 *  Recovery liveness: new desired in flight, and past-cap re-arm (review 2.3)
 * =========================================================================
 */

static void test_recovery_with_new_desired_inflight(void)
{
	struct midr_context ctx;
	struct midr_dp_status dp;
	struct midr_spf_install_status spf;
	uint64_t gen8_desired;
	struct midr_consumer_event events[2] = {0};
	struct midr_consumer_snapshot snapshot;

	ted_create(&ctx);
	ctx.master = event_master_create("midrd-dp-test");
	capture_reset();
	assert(midr_dp_backend_start(&ctx, ctx.master, VRF_DEFAULT) == 0);
	backend_fake_socket();

	stage_gen7(&ctx, &snapshot, events);
	assert(dp_status().installed == 1 && capture_count == 1);

	/* Gen 8 whole batch retained with recovery in flight. */
	fail_sends_after(0, -1);
	snapshot_two_routes(&snapshot, events, 8, 6, 4);
	assert(midr_ted_apply_snapshot(ctx.ted, &snapshot) == 0);
	pump_events(ctx.master, 200);
	dp = dp_status();
	assert(dp.pending == 2 && dp.installed == 1 && dp.recovering);
	assert(midr_spf_install_status_get(&ctx, &spf) == 0);
	assert(spf.desired_generation != 0);
	gen8_desired = spf.desired_generation;

	/* A new desired generation arrives while the retained batch is still
	 * failing: its operations append behind the retained remainder. */
	snapshot_two_routes(&snapshot, events, 9, 7, 5);
	assert(midr_ted_apply_snapshot(ctx.ted, &snapshot) == 0);
	dp = dp_status();
	assert(dp.pending == 4 && dp.recovering);
	assert(midr_spf_install_status_get(&ctx, &spf) == 0);
	assert(spf.desired_generation != gen8_desired);

	/* Let sends through: the retained remainder then the new diff apply in
	 * FIFO order and converge on gen 9. */
	allow_sends();
	pump_events(ctx.master, 400);
	dp = dp_status();
	assert(dp.installed == 1 && dp.pending == 0);
	assert(!dp.recovering && dp.last_error == 0);
	assert(capture_count == 5);
	assert(captures[1].cmd == ZEBRA_ROUTE_DELETE);
	assert(captures[2].cmd == ZEBRA_ROUTE_ADD);
	assert(captures[3].cmd == ZEBRA_ROUTE_DELETE);
	assert(captures[4].cmd == ZEBRA_ROUTE_ADD);

	midr_dp_backend_stop();
	assert(midr_dp_backend_get() == NULL);
	midr_ted_destroy(&ctx.ted);
}

static void test_recovery_past_cap_rearm(void)
{
	struct midr_context ctx;
	struct midr_dp_status dp;
	struct midr_consumer_event events[2] = {0};
	struct midr_consumer_snapshot snapshot;

	ted_create(&ctx);
	ctx.master = event_master_create("midrd-dp-test");
	capture_reset();
	assert(midr_dp_backend_start(&ctx, ctx.master, VRF_DEFAULT) == 0);
	backend_fake_socket();

	stage_gen7(&ctx, &snapshot, events);
	assert(dp_status().installed == 1 && capture_count == 1);

	/* Gen 8 fails and the socket never disconnects; run past the fast-retry
	 * cap (50 x 100 ms) so recovery falls back to the slow heartbeat. */
	fail_sends_after(0, -1);
	snapshot_two_routes(&snapshot, events, 8, 6, 4);
	assert(midr_ted_apply_snapshot(ctx.ted, &snapshot) == 0);

	pump_events(ctx.master, 7000);
	dp = dp_status();
	assert(dp.recovery_stalled);
	assert(dp.recovery_attempts >= 50);
	assert(dp.installed == 1 && dp.pending == 2);
	assert(dp.last_error == -EIO);

	/* No reconnect: a fresh desired generation must re-arm recovery through
	 * the explicit re-arm path and converge the retained batch. */
	allow_sends();
	snapshot_two_routes(&snapshot, events, 9, 7, 5);
	assert(midr_ted_apply_snapshot(ctx.ted, &snapshot) == 0);
	dp = dp_status();
	assert(!dp.recovery_stalled && dp.recovering);
	assert(dp.pending == 4);

	pump_events(ctx.master, 400);
	dp = dp_status();
	assert(dp.installed == 1 && dp.pending == 0);
	assert(!dp.recovering && !dp.recovery_stalled && dp.last_error == 0);
	assert(capture_count == 5);
	assert(captures[1].cmd == ZEBRA_ROUTE_DELETE);
	assert(captures[2].cmd == ZEBRA_ROUTE_ADD);
	assert(captures[3].cmd == ZEBRA_ROUTE_DELETE);
	assert(captures[4].cmd == ZEBRA_ROUTE_ADD);

	midr_dp_backend_stop();
	assert(midr_dp_backend_get() == NULL);
	midr_ted_destroy(&ctx.ted);
}

/*
 * =========================================================================
 *  Installed-hash error path (review 3, bullet 1)
 * =========================================================================
 */

static void test_installed_set_failure(void)
{
	struct midr_context ctx;
	struct midr_dp_status dp;
	struct prefix p = prefix_v4("10.30.0.0", 24);
	struct midr_path path = {};
	struct midr_path_result result = {};

	ted_create(&ctx);
	ctx.master = event_master_create("midrd-dp-test");
	capture_reset();
	assert(midr_dp_backend_start(&ctx, ctx.master, VRF_DEFAULT) == 0);
	backend_fake_socket();

	path_v4(&path, "192.0.2.1", 5, 10, 0);
	result.paths = &path;
	result.path_count = 1;
	result.instance = MIDR_INSTANCE_SPF;

	/* The ZAPI ADD is accepted but recording it fails: the caller must see
	 * a real error and the installed hash must stay empty (no fake state). */
	assert(midr_zebra_route_add(&ctx, &p, &result) == 0);
	midr_dp_backend_test_fail_installed(true);
	assert(midr_zebra_route_flush(&ctx) == -ENOMEM);
	dp = dp_status();
	assert(dp.installed == 0 && dp.pending == 1);
	assert(dp.last_error == -ENOMEM);
	assert(capture_count == 1 && captures[0].cmd == ZEBRA_ROUTE_ADD);

	/* The operation was retained and re-sent; the second attempt records
	 * the real state. */
	assert(midr_zebra_route_flush(&ctx) == 0);
	dp = dp_status();
	assert(dp.installed == 1 && dp.pending == 0);
	assert(capture_count == 2 && captures[1].cmd == ZEBRA_ROUTE_ADD);

	midr_dp_backend_stop();
	assert(midr_dp_backend_get() == NULL);
	midr_ted_destroy(&ctx.ted);
}

/*
 * =========================================================================
 *  GRE provisioning API
 * =========================================================================
 *  Validation paths only: the tunnel itself is created against a real zebra
 *  in the two-container connectivity test (doc/midr-doc/dp-doc).
 */
static void test_gre_api(void)
{
	struct midr_context ctx;
	struct midr_gre_status status = {};
	struct midr_gre_tunnel tun = {};
	struct ipaddr v4 = {}, v6 = {};

	ted_create(&ctx);
	ctx.master = event_master_create("midrd-dp-test");
	midr_gre_init(ctx.master);

	SET_IPADDR_V4(&v4);
	(void)inet_pton(AF_INET, "192.0.2.1", &v4.ipaddr_v4);
	SET_IPADDR_V6(&v6);
	(void)inet_pton(AF_INET6, "2001:db8::1", &v6.ipaddr_v6);

	/* NULL tunnel / unset endpoints are rejected before any zclient use. */
	assert(midr_gre_interface_add(&ctx, NULL, &status) == -1);
	assert(status.state == MIDR_GRE_STATE_FAILED && status.err == EINVAL);
	assert(midr_gre_interface_add(&ctx, &tun, &status) == -1);
	assert(status.err == EINVAL);

	/* Endpoint families must match. */
	tun.vrf_id = VRF_DEFAULT;
	tun.local = v4;
	tun.remote = v6;
	assert(midr_gre_interface_add(&ctx, &tun, &status) == -1);
	assert(status.err == EAFNOSUPPORT);

	/* No data-plane backend (and therefore no zclient) yet. */
	tun.remote = v4;
	assert(midr_gre_interface_add(&ctx, &tun, &status) == -1);
	assert(status.err == ENOTCONN);

	/* The same holds for teardown and lookups. */
	assert(midr_gre_interface_del(&ctx, "", &status) == -1);
	assert(status.err == EINVAL);
	assert(midr_gre_interface_del_by_endpoints(&ctx, VRF_DEFAULT, &v4, &v4,
						   &status) == -1);
	assert(status.err == ENOENT);
	assert(midr_gre_interface_get_state(VRF_DEFAULT, "midr-gre-1",
					    &status) == -1);
	assert(midr_gre_interface_name(VRF_DEFAULT, &v4, &v4) == NULL);
	assert(strcmp(midr_gre_state_str(MIDR_GRE_STATE_UP), "up") == 0);
	assert(strcmp(midr_gre_state_str(MIDR_GRE_STATE_PENDING), "pending") == 0);
	assert(strcmp(midr_gre_state_str(MIDR_GRE_STATE_FAILED), "failed") == 0);

	/* Attached backend without a connected zebra: still refused. */
	assert(midr_dp_backend_start(&ctx, ctx.master, VRF_DEFAULT) == 0);
	assert(midr_gre_interface_add(&ctx, &tun, &status) == -1);
	assert(status.err == ENOTCONN);
	midr_dp_backend_stop();

	midr_gre_fini();
	midr_ted_destroy(&ctx.ted);
}

int main(void)
{
	test_encoding_modes();
	test_send_failure_and_abort();
	test_adapter_pipeline_and_recovery();
	test_deferred_partial_batch_failure();
	test_deferred_whole_batch_failure();
	test_deferred_failure_across_reconnect();
	test_recovery_with_new_desired_inflight();
	test_recovery_past_cap_rearm();
	test_installed_set_failure();
	test_gre_api();
	puts("midrd-dp-test: PASS");
	return 0;
}
