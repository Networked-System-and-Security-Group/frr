// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unit tests for MIDR data-plane (bgp_midr_zebra.c)
 *
 * Links against libbgp.a; tests the public DP API:
 *   midr_zebra_init / fini / route_add / route_del / route_flush /
 *   route_update_deferred
 *
 * Internal structs bgp_midr_dp / midr_installed_entry are duplicated
 * here for test introspection (matching the definitions in bp_midr_zebra.c).
 */

#include <zebra.h>
#include "vty.h"
#include "stream.h"
#include "privs.h"
#include "queue.h"
#include "filter.h"
#include "frr_pthread.h"
#include "lib/frrevent.h"
#include "lib/hash.h"
#include "lib/zclient.h"
#include "lib/frrdistance.h"
#include "bgpd/bgpd.h"
#include "bgpd/bgp_midr_zebra.h"

/* ==================================================================
 * Internal struct copies (mirror bp_midr_zebra.c) for test access
 * ================================================================== */
struct midr_installed_entry {
	struct prefix		prefix;
	struct midr_path	*paths;
	uint8_t			path_count;
	uint8_t			sid_count;
	struct in6_addr		sid_list[SRV6_MAX_SEGS];
};

struct bgp_midr_dp {
	struct event		*t_deferred;
	struct list		*pending_ops;
	struct hash		*installed;
	bool			flush_pending;
};

/* ==================================================================
 * Mock globals (libbgp.a symbols we must provide)
 * ================================================================== */
struct zebra_privs_t bgpd_privs = {};
/* dynamically allocated so event_loop (opaque struct) works */
struct event_loop *master;
struct bgp_master dummy_bm;
struct bgp_master *bm = &dummy_bm;
struct zclient *bgp_zclient;

/* weak wrappers: used via -Wl,--wrap (linker replaces the libfrr symbols) */
enum zclient_send_status __wrap_zclient_route_send(uint8_t cmd,
	struct zclient *z, struct zapi_route *api) {
	return ZCLIENT_SEND_SUCCESS;
}

/* ==================================================================
 * Test harness
 * ================================================================== */
#define VT100_RED    "\x1b[31m"
#define VT100_GREEN  "\x1b[32m"
#define VT100_RESET  "\x1b[0m"
#define OK   VT100_GREEN "OK" VT100_RESET
#define FAIL VT100_RED "FAIL" VT100_RESET

static int g_failed = 0;

#define T(cond, msg) do { \
	if (!(cond)) { printf("  " FAIL ": %s\n", msg); g_failed++; } \
	else          printf("  " OK   ": %s\n", msg); \
} while (0)

/* helpers */
static struct bgp_midr_dp *get_dp(struct bgp *bgp) {
	return (struct bgp_midr_dp *)bgp->midr_dp;
}
static int pending_count(struct bgp *bgp) {
	struct bgp_midr_dp *dp = get_dp(bgp);
	return (dp && dp->pending_ops) ? listcount(dp->pending_ops) : -1;
}
static int installed_has(struct bgp *bgp, struct prefix *p) {
	struct bgp_midr_dp *dp = get_dp(bgp);
	if (!dp || !dp->installed) return 0;
	struct midr_installed_entry key = {};
	prefix_copy(&key.prefix, p);
	return hash_lookup(dp->installed, &key) != NULL;
}

/* ==================================================================
 * Test 1: init / fini lifecycle
 * ================================================================== */
static void test_lifecycle(void)
{
	struct bgp bgp = {};
	struct bgp_midr_dp *dp;

	printf("\n[Test 1] init/fini lifecycle\n");

	midr_zebra_fini(&bgp);
	T(1, "fini on NULL dp safe");

	midr_zebra_init(&bgp);
	T(bgp.midr_dp != NULL, "midr_dp allocated");

	dp = get_dp(&bgp);
	T(dp->pending_ops != NULL, "pending_ops list created");
	T(dp->installed != NULL, "installed hash created");
	T(pending_count(&bgp) == 0, "pending_ops empty initially");
	T(!dp->flush_pending, "flush_pending=false initially");

	midr_zebra_fini(&bgp);
	T(bgp.midr_dp == NULL, "midr_dp NULL after fini");
}

/* ==================================================================
 * Test 2: add / flush / diff
 * ================================================================== */
static void test_add_flush_diff(void)
{
	struct bgp bgp = {};
	struct midr_path paths[2] = {};
	struct midr_path_result result = {};
	struct prefix p;

	printf("\n[Test 2] add/flush/diff\n");

	midr_zebra_init(&bgp);

	str2prefix("10.1.0.0/16", &p);
	inet_pton(AF_INET, "10.0.0.1", &paths[0].nexthop.ipv4);
	inet_pton(AF_INET, "10.0.0.2", &paths[1].nexthop.ipv4);
	result.paths = paths; result.path_count = 2;

	midr_zebra_route_add(&bgp, &p, &result);
	T(pending_count(&bgp) == 1, "1 pending after add");

	midr_zebra_route_add(&bgp, NULL, &result);
	midr_zebra_route_add(&bgp, &p, NULL);
	T(pending_count(&bgp) == 1, "nil args ignored");

	midr_zebra_route_flush(&bgp);
	T(pending_count(&bgp) == 0, "queue empty after flush");
	T(installed_has(&bgp, &p), "installed hash has prefix");

	/* no-op reinstall */
	midr_zebra_route_add(&bgp, &p, &result);
	midr_zebra_route_flush(&bgp);
	T(installed_has(&bgp, &p), "reinstall preserves entry");

	/* change nexthop → diff detects change */
	inet_pton(AF_INET, "10.0.0.99", &paths[0].nexthop.ipv4);
	midr_zebra_route_add(&bgp, &p, &result);
	midr_zebra_route_flush(&bgp);
	T(installed_has(&bgp, &p), "entry still exists after change");

	/* delete */
	midr_zebra_route_del(&bgp, &p);
	midr_zebra_route_flush(&bgp);
	T(!installed_has(&bgp, &p), "gone after delete");

	midr_zebra_fini(&bgp);
}

/* ==================================================================
 * Test 3: add-then-delete in one batch
 * ================================================================== */
static void test_add_then_delete(void)
{
	struct bgp bgp = {};
	struct midr_path paths[1] = {};
	struct midr_path_result result = {};
	struct prefix p;

	printf("\n[Test 3] add-then-delete\n");

	midr_zebra_init(&bgp);

	str2prefix("192.168.0.0/16", &p);
	inet_pton(AF_INET, "10.0.0.1", &paths[0].nexthop.ipv4);
	result.paths = paths; result.path_count = 1;

	midr_zebra_route_add(&bgp, &p, &result);
	midr_zebra_route_del(&bgp, &p);
	T(pending_count(&bgp) == 2, "ADD+DEL queued");

	midr_zebra_route_flush(&bgp);
	T(pending_count(&bgp) == 0, "queue empty after flush");
	T(!installed_has(&bgp, &p), "not installed after ADD→DEL");

	midr_zebra_route_del(&bgp, &p);
	midr_zebra_route_flush(&bgp);
	T(1, "delete non-existent safe");

	midr_zebra_fini(&bgp);
}

/* ==================================================================
 * Test 4: deferred timer coalescing
 * ================================================================== */
static void test_deferred(void)
{
	struct bgp bgp = {};
	struct midr_path paths[1] = {};
	struct midr_path_result result = {};
	struct prefix p;
	struct bgp_midr_dp *dp;

	printf("\n[Test 4] deferred timer coalescing\n");

	midr_zebra_init(&bgp);
	dp = get_dp(&bgp);

	str2prefix("10.99.0.0/16", &p);
	inet_pton(AF_INET, "10.0.0.1", &paths[0].nexthop.ipv4);
	result.paths = paths; result.path_count = 1;

	midr_zebra_route_add(&bgp, &p, &result);

	/* first deferred call arms the timer */
	midr_zebra_route_update_deferred(&bgp);
	T(dp->flush_pending, "flush_pending=true after deferred");
	T(dp->t_deferred != NULL, "deferred timer armed");

	/* second call coalesced */
	midr_zebra_route_update_deferred(&bgp);
	T(dp->flush_pending, "coalesced (still true)");

	/* flush cancels timer and drains queue */
	midr_zebra_route_flush(&bgp);
	T(!dp->flush_pending, "flush_pending reset");
	T(dp->t_deferred == NULL, "timer cancelled by flush");
	T(pending_count(&bgp) == 0, "queue drained");

	midr_zebra_route_flush(&bgp);
	T(1, "empty flush safe");

	midr_zebra_fini(&bgp);
}

/* ==================================================================
 * Test 5: SRv6 path
 * ================================================================== */
static void test_srv6_path(void)
{
	struct bgp bgp = {};
	struct midr_path paths[1] = {};
	struct midr_path_result result = {};
	struct prefix p;

	printf("\n[Test 5] SRv6 path\n");

	midr_zebra_init(&bgp);

	str2prefix("2001:db8:1::/48", &p);
	inet_pton(AF_INET6, "2001:db8:a::1", &paths[0].nexthop.ipv6);
	result.paths = paths; result.path_count = 1;
	result.explicit.sid_count = 3;
	inet_pton(AF_INET6, "2001:db8:1::1", &result.explicit.sid_list[0]);
	inet_pton(AF_INET6, "2001:db8:2::1", &result.explicit.sid_list[1]);
	inet_pton(AF_INET6, "2001:db8:3::1", &result.explicit.sid_list[2]);

	midr_zebra_route_add(&bgp, &p, &result);
	T(pending_count(&bgp) == 1, "SRv6 queued");

	midr_zebra_route_flush(&bgp);
	T(installed_has(&bgp, &p), "SRv6 installed");

	midr_zebra_fini(&bgp);
}

/* ==================================================================
 * Test 6: multiple prefixes
 * ================================================================== */
static void test_multiple_prefixes(void)
{
	struct bgp bgp = {};
	struct midr_path paths[1] = {};
	struct midr_path_result result = {};
	struct prefix p1, p2, p3;

	printf("\n[Test 6] multiple prefixes\n");

	midr_zebra_init(&bgp);

	inet_pton(AF_INET, "10.0.0.1", &paths[0].nexthop.ipv4);
	result.paths = paths; result.path_count = 1;

	str2prefix("10.1.0.0/16", &p1);
	str2prefix("10.2.0.0/16", &p2);
	str2prefix("10.3.0.0/16", &p3);

	midr_zebra_route_add(&bgp, &p1, &result);
	midr_zebra_route_add(&bgp, &p2, &result);
	midr_zebra_route_add(&bgp, &p3, &result);
	T(pending_count(&bgp) == 3, "3 pending ops");

	midr_zebra_route_flush(&bgp);
	T(installed_has(&bgp, &p1), "p1");
	T(installed_has(&bgp, &p2), "p2");
	T(installed_has(&bgp, &p3), "p3");

	midr_zebra_route_del(&bgp, &p2);
	midr_zebra_route_flush(&bgp);
	T(installed_has(&bgp, &p1), "p1 still");
	T(!installed_has(&bgp, &p2), "p2 gone");
	T(installed_has(&bgp, &p3), "p3 still");

	midr_zebra_fini(&bgp);
}

/* ==================================================================
 * Test 7: UCMP weights
 * ================================================================== */
static void test_ucmp(void)
{
	struct bgp bgp = {};
	struct midr_path paths[3] = {};
	struct midr_path_result result = {};
	struct prefix p;

	printf("\n[Test 7] UCMP weights\n");

	midr_zebra_init(&bgp);

	str2prefix("10.5.0.0/16", &p);
	inet_pton(AF_INET, "10.0.0.1", &paths[0].nexthop.ipv4);
	paths[0].weight = 204;
	inet_pton(AF_INET, "10.0.0.2", &paths[1].nexthop.ipv4);
	paths[1].weight = 51;
	inet_pton(AF_INET, "10.0.0.3", &paths[2].nexthop.ipv4);
	paths[2].weight = 0;
	result.paths = paths; result.path_count = 3;

	midr_zebra_route_add(&bgp, &p, &result);
	T(pending_count(&bgp) == 1, "UCMP queued");
	midr_zebra_route_flush(&bgp);
	T(installed_has(&bgp, &p), "UCMP installed");
	midr_zebra_fini(&bgp);
}

/* ==================================================================
 * Test 8: empty flush / empty deferred
 * ================================================================== */
static void test_empty_ops(void)
{
	struct bgp bgp = {};

	printf("\n[Test 8] empty operations\n");

	midr_zebra_init(&bgp);

	midr_zebra_route_flush(&bgp);
	T(1, "flush on empty safe");

	midr_zebra_route_update_deferred(&bgp);
	T(1, "deferred on empty safe");

	midr_zebra_fini(&bgp);
}

/* ==================================================================
 * Test 9: struct layout sanity
 * ================================================================== */
static void test_struct_layout(void)
{
	printf("\n[Test 9] struct layout\n");

	T(sizeof(struct midr_path) > 0, "midr_path sizeof > 0");
	T(sizeof(struct midr_path_result) >
	  sizeof(struct in6_addr) * SRV6_MAX_SEGS,
	  "midr_path_result has room for SID list");

	struct midr_path_result r = {};
	T(r.explicit.sid_count == 0, "zero-inited sid_count=0 (pure IP)");
	T(r.path_count == 0, "zero-inited path_count=0");
}

/* ==================================================================
 * main
 * ================================================================== */
int main(void)
{
	/* Allocate a real event_loop (needed by event_add_timer_msec) */
	master = event_master_create("test_midr");
	dummy_bm.master = master;

	printf("=== MIDR Data-Plane Unit Tests ===\n");

	test_lifecycle();
	test_add_flush_diff();
	test_add_then_delete();
	test_deferred();
	test_srv6_path();
	test_multiple_prefixes();
	test_ucmp();
	test_empty_ops();
	test_struct_layout();

	printf("\n=== %d test(s) FAILED ===\n", g_failed);
	return g_failed ? 1 : 0;
}
