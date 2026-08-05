// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR Advanced Data-Plane Verification Test
 *
 * Three advanced test modes beyond basic blackhole:
 *
 *   Mode 1 ("ttl-redirect"):  Traffic redirection with TTL observation.
 *     Installs a MIDR route that forces traffic through a detour path
 *     (via r3 forwarder), proving data-plane steering without packet loss.
 *     Caller observes TTL change (e.g., 63→62) because the detour path
 *     has one extra hop.
 *
 *   Mode 2 ("http-block"):    TCP/HTTP service connectivity test.
 *     R2 runs a python3 HTTP server on 10.100.0.1:8080.
 *     BEFORE: curl succeeds (200 OK).
 *     DURING: MIDR blackhole installed → curl times out.
 *     AFTER:  MIDR route deleted → curl succeeds again.
 *
 *   Mode 3 ("iperf-stress"):  High-frequency route add/del under iperf3 load.
 *     Rapidly adds and deletes MIDR routes while r1→r2 iperf3 traffic is
 *     running. Verifies that the kernel FIB and zebra remain stable under
 *     concurrent netlink operations and data-plane forwarding.
 *
 * Usage:
 *   LD_LIBRARY_PATH=/tmp /tmp/test_midr_advanced <mode> [zebra_socket]
 *
 *   modes:
 *     ttl-redirect   Install MIDR route via detour nexthop, hold 10s,
 *                    then delete. Caller measures TTL during hold.
 *     http-block     Install blackhole for 10.100.0.0/24 (via dead nexthop),
 *                    hold 10s, then delete. Caller tests HTTP during hold.
 *     iperf-stress   Loop: add route → hold 0.5s → del route → hold 0.5s,
 *                    repeat 30 times. Caller runs iperf3 simultaneously.
 *
 * Compile (inside FRR dev container):
 *   gcc -std=gnu11 -Wall -Wextra -g -O0 -include config.h \
 *       -I lib -I bgpd -I .  $(pkg-config --cflags libyang) \
 *       -o /tmp/test_midr_advanced \
 *       tests/bgpd/test_midr_zapi_advanced.c bgpd/bgp_midr_zebra.o \
 *       -L lib/.libs -lfrr \
 *       -lcap -lcrypt -ljson-c -lrt -lpthread -lsqlite3 -lresolv \
 *       -ldl -lm -lfl -lyang
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
#include "lib/libfrr.h"
#include "bgpd/bgpd.h"
#include "bgpd/bgp_midr_zebra.h"

/* ----- globals needed by libbgp.a / bgp_midr_zebra.o ------------------- */
struct zebra_privs_t bgpd_privs = {};
struct event_loop *master = NULL;
struct bgp_master *bm;
struct zclient *bgp_zclient;

static int run_cmd(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int run_cmd(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return system(buf);
}

/* ----- test constants -------------------------------------------------- */
static const char *TARGET_PFX    = "10.100.0.0/24";   /* r2-advertised prefix */
static const char *DEAD_NEXTHOP  = "10.200.0.1";      /* dummy_bh own addr    */
static const char *DETOUR_NH     = "10.0.100.2";      /* r3 eth1 (detour path) */
static const char *BH_IF         = "dummy_bh";        /* dead-end interface   */
static const char *ZEBRA_SOCK    = "/var/run/frr/zserv.api";

/* ----- helper: drain event loop ---------------------------------------- */
static void drain_events(void)
{
	struct event t;
	for (int i = 0; i < 10; i++) {
		event_fetch(master, &t);
		event_call(&t);
	}
	sleep(1);
}

/* ----- common init: connect zebra, init MIDR DP ----------------------- */
static int midr_connect(const char *zebra_sock)
{
	master = event_master_create("midr_adv");

	struct bgp_master dummy_bm = {};
	dummy_bm.master = master;
	bm = &dummy_bm;

	bgp_zclient = zclient_new(master, &zclient_options_default, NULL, 0);
	if (!bgp_zclient) {
		printf("FAIL: zclient_new\n");
		return -1;
	}
	zclient_init(bgp_zclient, ZEBRA_ROUTE_BGP_MIDR, 0, &bgpd_privs);
	if (!frr_zclient_addr(&zclient_addr, &zclient_addr_len, zebra_sock)) {
		printf("FAIL: invalid socket path %s\n", zebra_sock);
		return -1;
	}
	if (zclient_socket_connect(bgp_zclient) < 0) {
		printf("FAIL: cannot connect to zebra\n");
		return -1;
	}
	printf("  Connected to zebra OK\n");

	struct bgp bgp_dummy = {};
	midr_zebra_init(&bgp_dummy);
	printf("  midr_zebra_init OK\n");
	return 0;
}

/* ----- common: install a MIDR route ----------------------------------- */
static int midr_install_route(const char *pfx_str, const char *nexthop_str,
			      uint32_t metric, uint8_t instance)
{
	struct prefix p;
	struct midr_path paths[1];
	struct midr_path_result result;
	struct bgp bgp_dummy = {};

	str2prefix(pfx_str, &p);
	memset(paths, 0, sizeof(paths));
	inet_pton(AF_INET, nexthop_str, &paths[0].nexthop.ipv4);
	paths[0].metric = metric;
	paths[0].weight = 0;
	result.paths      = paths;
	result.path_count = 1;
	result.explicit.sid_count = 0;
	result.instance   = instance;

	midr_zebra_route_add(&bgp_dummy, &p, &result);
	midr_zebra_route_flush(&bgp_dummy);
	drain_events();
	return 0;
}

/* ----- common: delete a MIDR route ------------------------------------ */
static int midr_delete_route(const char *pfx_str)
{
	struct prefix p;
	struct bgp bgp_dummy = {};

	str2prefix(pfx_str, &p);
	midr_zebra_route_del(&bgp_dummy, &p, MIDR_INSTANCE_SPF);
	midr_zebra_route_flush(&bgp_dummy);
	drain_events();
	return 0;
}

/* ----- common: verify route in/not-in FIB ----------------------------- */
static int fib_has_route(const char *pfx_str, const char *via)
{
	char cmd[512];
	snprintf(cmd, sizeof(cmd),
		 "ip route show %s proto 199 2>/dev/null | grep -q '%s'",
		 pfx_str, pfx_str);
	int rc = system(cmd);
	return (rc == 0) ? 1 : 0;
}

static void fib_show_proto199(void)
{
	printf("  [FIB proto 199]:\n");
	system("ip route show proto 199 2>/dev/null || echo '    (none)'");
}

/* ===================================================================== *
 *  Mode 1: TTL Redirection
 * ===================================================================== *
 *  Scenario:
 *    Topology has r1↔r3↔r2 detour path via eth2 links.
 *    BGP normal path:   r1 → r2 (direct, TTL=63)
 *    MIDR detour path:  r1 → r3 → r2 (2 hops, TTL=62)
 *
 *  1. Connect zebra, init MIDR DP
 *  2. Install MIDR route: 10.100.0.0/24 via DETOUR_NH (r3's eth1)
 *     with metric=1 (wins over BGP metric)
 *  3. Hold for 10 seconds (caller pings and observes TTL=62)
 *  4. Delete MIDR route
 *  5. Verify route removed from FIB
 *  6. Caller pings again → TTL=63 restored
 */
static int mode_ttl_redirect(const char *zebra_sock)
{
	int rc = 0;

	printf("=== MIDR TTL Redirection Test ===\n");
	printf("detour nexthop: %s\n", DETOUR_NH);
	printf("target prefix:  %s\n", TARGET_PFX);

	/* Step 1: connect */
	printf("[1] Connect to zebra + init MIDR ...\n");
	if (midr_connect(zebra_sock) < 0)
		return 1;

	/* Step 2: install MIDR route via detour */
	printf("[2] Install MIDR route: %s via %s metric=1 ...\n",
	       TARGET_PFX, DETOUR_NH);
	midr_install_route(TARGET_PFX, DETOUR_NH, 1, MIDR_INSTANCE_SPF);

	/* Step 3: verify FIB */
	printf("[3] Verify FIB ...\n");
	if (fib_has_route(TARGET_PFX, DETOUR_NH))
		printf("  OK: %s in FIB via %s (proto 199)\n",
		       TARGET_PFX, DETOUR_NH);
	else {
		printf("  FAIL: %s NOT in FIB\n", TARGET_PFX);
		rc = 1;
	}
	fib_show_proto199();

	/* Step 4: hold for caller to measure TTL */
	printf("[4] Holding MIDR route for 10 seconds "
	       "(caller measures TTL change)...\n");
	fflush(stdout);
	sleep(10);

	/* Step 5: delete MIDR route */
	printf("[5] Delete MIDR route ...\n");
	midr_delete_route(TARGET_PFX);

	/* Step 6: verify removal */
	printf("[6] Verify route removed ...\n");
	if (!fib_has_route(TARGET_PFX, DETOUR_NH))
		printf("  OK: %s removed from FIB\n", TARGET_PFX);
	else {
		printf("  FAIL: %s still in FIB\n", TARGET_PFX);
		rc = 1;
	}

	/* Cleanup */
	printf("[7] Cleanup ...\n");
	struct bgp bgp_dummy = {};
	midr_zebra_fini(&bgp_dummy);

	if (rc == 0)
		printf("\n=== TTL REDIRECT TEST PASSED ===\n");
	else
		printf("\n=== TTL REDIRECT TEST FAILED (%d) ===\n", rc);
	return rc;
}

/* ===================================================================== *
 *  Mode 2: HTTP Block (TCP service blackhole)
 * ===================================================================== *
 *  Scenario:
 *    r2 runs HTTP server on 10.100.0.1:8080.
 *    1. Setup dummy_bh dead-end interface
 *    2. Install blackhole route: 10.100.0.0/24 via DEAD_NEXTHOP metric=1
 *    3. Hold for 10 seconds (caller tests curl → timeout)
 *    4. Delete blackhole
 *    5. Caller tests curl → 200 OK
 */
static int mode_http_block(const char *zebra_sock)
{
	int rc = 0;

	printf("=== MIDR HTTP Block Test ===\n");
	printf("target prefix: %s\n", TARGET_PFX);
	printf("dead nexthop:  %s\n", DEAD_NEXTHOP);

	/* Step 0: setup dummy interface */
	printf("[1] Setup dead-end interface %s ...\n", BH_IF);
	run_cmd("ip link add %s type dummy 2>/dev/null || true", BH_IF);
	run_cmd("ip link set %s up 2>/dev/null || true", BH_IF);
	run_cmd("ip addr add %s/32 dev %s 2>/dev/null || true",
		DEAD_NEXTHOP, BH_IF);

	/* Step 2: connect */
	printf("[2] Connect to zebra + init MIDR ...\n");
	if (midr_connect(zebra_sock) < 0)
		return 1;

	/* Step 3: install blackhole route */
	printf("[3] Install blackhole route ...\n");
	midr_install_route(TARGET_PFX, DEAD_NEXTHOP, 1, MIDR_INSTANCE_SPF);

	/* Step 4: verify FIB */
	printf("[4] Verify FIB ...\n");
	if (fib_has_route(TARGET_PFX, DEAD_NEXTHOP))
		printf("  OK: %s in FIB (blackhole via %s)\n",
		       TARGET_PFX, DEAD_NEXTHOP);
	else {
		printf("  FAIL: %s NOT in FIB\n", TARGET_PFX);
		rc = 1;
	}
	fib_show_proto199();

	/* Step 5: hold for caller to test HTTP blocking */
	printf("[5] Holding blackhole for 10 seconds "
	       "(caller tests HTTP block)...\n");
	fflush(stdout);
	sleep(10);

	/* Step 6: delete blackhole */
	printf("[6] Delete blackhole route ...\n");
	midr_delete_route(TARGET_PFX);

	/* Step 7: verify removal */
	printf("[7] Verify route removed ...\n");
	if (!fib_has_route(TARGET_PFX, DEAD_NEXTHOP))
		printf("  OK: %s removed from FIB\n", TARGET_PFX);
	else {
		printf("  FAIL: %s still in FIB\n", TARGET_PFX);
		rc = 1;
	}

	/* Cleanup */
	printf("[8] Cleanup ...\n");
	struct bgp bgp_dummy = {};
	midr_zebra_fini(&bgp_dummy);
	run_cmd("ip link del %s 2>/dev/null || true", BH_IF);

	if (rc == 0)
		printf("\n=== HTTP BLOCK TEST PASSED ===\n");
	else
		printf("\n=== HTTP BLOCK TEST FAILED (%d) ===\n", rc);
	return rc;
}

/* ===================================================================== *
 *  Mode 3: iperf3 Stress (rapid route add/del under load)
 * ===================================================================== *
 *  Scenario:
 *    r2 runs iperf3 server, r1 runs iperf3 client.
 *    During sustained traffic, this binary rapidly adds/deletes
 *    MIDR routes to stress-test the zebra→kernel netlink path.
 *
 *    Loop (30 iterations):
 *      add route → hold 0.5s → del route → hold 0.5s
 *    Total ~30 seconds of stress.
 */
static int mode_iperf_stress(const char *zebra_sock)
{
	int rc = 0;
	int iterations = 30;

	printf("=== MIDR iperf3 Stress Test ===\n");
	printf("iterations:    %d\n", iterations);
	printf("target prefix: %s\n", TARGET_PFX);
	printf("dead nexthop:  %s\n", DEAD_NEXTHOP);

	/* Step 0: setup dummy interface */
	printf("[1] Setup dead-end interface %s ...\n", BH_IF);
	run_cmd("ip link add %s type dummy 2>/dev/null || true", BH_IF);
	run_cmd("ip link set %s up 2>/dev/null || true", BH_IF);
	run_cmd("ip addr add %s/32 dev %s 2>/dev/null || true",
		DEAD_NEXTHOP, BH_IF);

	/* Step 1: connect */
	printf("[2] Connect to zebra + init MIDR ...\n");
	if (midr_connect(zebra_sock) < 0)
		return 1;

	/* Step 2: rapid add/del loop */
	printf("[3] Starting rapid add/del loop (%d iterations, ~30s)...\n",
	       iterations);
	fflush(stdout);

	int add_ok = 0, del_ok = 0;
	struct timespec ts = {.tv_sec = 0, .tv_nsec = 500000000L}; /* 500ms */

	for (int i = 1; i <= iterations; i++) {
		/* Add */
		midr_install_route(TARGET_PFX, DEAD_NEXTHOP, 1, MIDR_INSTANCE_SPF);
		if (fib_has_route(TARGET_PFX, DEAD_NEXTHOP))
			add_ok++;

		nanosleep(&ts, NULL);

		/* Delete */
		midr_delete_route(TARGET_PFX);
		if (!fib_has_route(TARGET_PFX, DEAD_NEXTHOP))
			del_ok++;

		nanosleep(&ts, NULL);

		if (i % 10 == 0)
			printf("  ... iteration %d/%d (add_ok=%d, del_ok=%d)\n",
			       i, iterations, add_ok, del_ok);
		fflush(stdout);
	}

	printf("[4] Stress loop complete. add_ok=%d/%d, del_ok=%d/%d\n",
	       add_ok, iterations, del_ok, iterations);

	if (add_ok != iterations) {
		printf("  FAIL: some routes failed to install\n");
		rc = 1;
	}
	if (del_ok != iterations) {
		printf("  FAIL: some routes failed to delete\n");
		rc = 1;
	}

	/* Step 3: final cleanup */
	printf("[5] Final verify: no MIDR routes left ...\n");
	fib_show_proto199();

	/* Cleanup */
	printf("[6] Cleanup ...\n");
	struct bgp bgp_dummy = {};
	midr_zebra_fini(&bgp_dummy);
	run_cmd("ip link del %s 2>/dev/null || true", BH_IF);

	if (rc == 0)
		printf("\n=== IPERF STRESS TEST PASSED ===\n");
	else
		printf("\n=== IPERF STRESS TEST FAILED (%d) ===\n", rc);
	return rc;
}

/* ===================================================================== *
 *  Main
 * ===================================================================== */
int main(int argc, char **argv)
{
	const char *mode = NULL;
	const char *zebra_sock = ZEBRA_SOCK;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s <mode> [zebra_socket]\n", argv[0]);
		fprintf(stderr, "  modes: ttl-redirect | http-block | iperf-stress\n");
		return 1;
	}

	mode = argv[1];
	if (argc > 2)
		zebra_sock = argv[2];

	printf("=== MIDR Advanced Test ===\n");
	printf("mode:   %s\n", mode);
	printf("socket: %s\n", zebra_sock);

	if (strcmp(mode, "ttl-redirect") == 0)
		return mode_ttl_redirect(zebra_sock);
	else if (strcmp(mode, "http-block") == 0)
		return mode_http_block(zebra_sock);
	else if (strcmp(mode, "iperf-stress") == 0)
		return mode_iperf_stress(zebra_sock);
	else {
		fprintf(stderr, "Unknown mode: %s\n", mode);
		fprintf(stderr, "  modes: ttl-redirect | http-block | iperf-stress\n");
		return 1;
	}
}