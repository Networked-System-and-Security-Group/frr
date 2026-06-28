// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * E2E (end-to-end) test for MIDR data-plane:
 *   Actually connects to a running zebra daemon, calls the
 *   midr_zebra_route_add / flush / del API, and verifies that
 *   the kernel FIB reflects the expected state via "ip route show".
 *
 * Usage:
 *   ./test_midr_zebra_e2e [zebra_socket_path]
 *   Default socket: /var/run/frr/zserv.api
 *
 * Returns 0 if all checks pass, 1 otherwise.
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

/* ==================================================================
 * Globals needed by libbgp.a / bgp_midr_zebra.o
 * ================================================================== */
struct zebra_privs_t bgpd_privs = {};
struct event_loop *master = NULL;
struct bgp_master *bm;
struct zclient *bgp_zclient;

/* Use default zclient options (no special flags needed) */

/* ------------------------------------------------------------------
 * Test prefix: 10.254.254.0/24 via 192.0.2.1 (dummy nexthop)
 * We use a loopback/dummy interface so the route is installable.
 * ------------------------------------------------------------------ */
static const char *TEST_PREFIX  = "10.254.254.0/24";
static const char *TEST_NEXTHOP = "192.0.2.1";   /* dummy0 address */
static const char *DUMMY_IF     = "dummy0";

/* ------------------------------------------------------------------
 * helpers
 * ------------------------------------------------------------------ */
static int run_cmd(const char *fmt, ...) __attribute__((format(printf,1,2)));
static int run_cmd(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return system(buf);
}

static int ip_route_has(const char *prefix)
{
	char cmd[256];
	snprintf(cmd, sizeof(cmd),
		 "ip route show %s 2>/dev/null | grep -v '^$' | wc -l", prefix);
	FILE *fp = popen(cmd, "r");
	if (!fp) return 0;
	int n = 0;
	fscanf(fp, "%d", &n);
	pclose(fp);
	return n > 0;
}

static int ip_route6_has(const char *prefix)
{
	char cmd[256];
	snprintf(cmd, sizeof(cmd),
		 "ip -6 route show %s 2>/dev/null | grep -v '^$' | wc -l", prefix);
	FILE *fp = popen(cmd, "r");
	if (!fp) return 0;
	int n = 0;
	fscanf(fp, "%d", &n);
	pclose(fp);
	return n > 0;
}

/* ------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
	const char *zebra_sock = "/var/run/frr/zserv.api";
	int rc = 0;
	struct bgp bgp = {};
	struct prefix p;
	struct midr_path paths[1];
	struct midr_path_result result;

	if (argc > 1)
		zebra_sock = argv[1];

	printf("=== MIDR E2E Route Installation Test ===\n");
	printf("zebra socket: %s\n", zebra_sock);

	/* ---- Step 1: Setup dummy interface & nexthop ---- */
	printf("\n[Step 1] Setup dummy0 with %s\n", TEST_NEXTHOP);
	run_cmd("ip link add %s type dummy 2>/dev/null || true", DUMMY_IF);
	run_cmd("ip link set %s up 2>/dev/null || true", DUMMY_IF);
	run_cmd("ip addr add %s/32 dev %s 2>/dev/null || true",
		TEST_NEXTHOP, DUMMY_IF);

	/* ---- Step 2: Create event loop + connect to zebra ---- */
	printf("[Step 2] Connect to zebra\n");
	master = event_master_create("midr_e2e");

	/* Allocate a bgp_master so bm->master is available */
	struct bgp_master dummy_bm = {};
	dummy_bm.master = master;
	bm = &dummy_bm;

	bgp_zclient = zclient_new(master, &zclient_options_default, NULL, 0);
	if (!bgp_zclient) {
		printf("FAIL: zclient_new returned NULL\n");
		return 1;
	}
	zclient_init(bgp_zclient, ZEBRA_ROUTE_BGP_MIDR, 0, &bgpd_privs);
	/* Set the zebra socket path before connecting */
	if (!frr_zclient_addr(&zclient_addr, &zclient_addr_len, zebra_sock)) {
		printf("FAIL: invalid zserv socket path: %s\n", zebra_sock);
		return 1;
	}
	if (zclient_socket_connect(bgp_zclient) < 0) {
		printf("FAIL: could not connect to zebra at %s\n", zebra_sock);
		return 1;
	}
	printf("  Connected to zebra OK\n");

	/* ---- Step 3: Init MIDR DP ---- */
	printf("[Step 3] midr_zebra_init\n");
	midr_zebra_init(&bgp);
	printf("  midr_dp = %p\n", bgp.midr_dp);

	/* ---- Step 4: Add a route ---- */
	printf("[Step 4] midr_zebra_route_add(%s via %s)\n",
	       TEST_PREFIX, TEST_NEXTHOP);
	str2prefix(TEST_PREFIX, &p);
	memset(paths, 0, sizeof(paths));
	inet_pton(AF_INET, TEST_NEXTHOP, &paths[0].nexthop.ipv4);
	paths[0].metric = 100;
	result.paths = paths;
	result.path_count = 1;
	result.explicit.sid_count = 0;

	midr_zebra_route_add(&bgp, &p, &result);

	/* ---- Step 5: Flush immediately ---- */
	printf("[Step 5] midr_zebra_route_flush\n");
	midr_zebra_route_flush(&bgp);

	/* Process ZAPI send (event loop needs to run briefly) */
	{
		struct event t;
		int i;
		for (i = 0; i < 10; i++) {
			event_fetch(master, &t);
			event_call(&t);
		}
	}

	/* Give zebra + kernel a moment */
	sleep(1);

	/* ---- Step 6: Verify route in kernel ---- */
	printf("[Step 6] Check kernel FIB for %s\n", TEST_PREFIX);
	if (ip_route_has(TEST_PREFIX)) {
		printf("  OK: route %s is in kernel FIB\n", TEST_PREFIX);
	} else {
		printf("  FAIL: route %s NOT in kernel FIB\n", TEST_PREFIX);
		rc = 1;
	}

	/* ---- Step 7: Delete the route ---- */
	printf("[Step 7] midr_zebra_route_del(%s)\n", TEST_PREFIX);
	midr_zebra_route_del(&bgp, &p);
	midr_zebra_route_flush(&bgp);

	{
		struct event t;
		int i;
		for (i = 0; i < 10; i++) {
			event_fetch(master, &t);
			event_call(&t);
		}
	}
	sleep(1);

	/* ---- Step 8: Verify route removed from kernel ---- */
	printf("[Step 8] Check kernel FIB for %s after delete\n", TEST_PREFIX);
	if (!ip_route_has(TEST_PREFIX)) {
		printf("  OK: route %s removed from kernel FIB\n", TEST_PREFIX);
	} else {
		printf("  FAIL: route %s still in kernel FIB after delete\n",
		       TEST_PREFIX);
		rc = 1;
	}

	/* ---- Step 9: Cleanup ---- */
	printf("[Step 9] Cleanup\n");
	midr_zebra_fini(&bgp);
	run_cmd("ip link del %s 2>/dev/null || true", DUMMY_IF);

	if (rc == 0)
		printf("\n=== ALL CHECKS PASSED ===\n");
	else
		printf("\n=== %d CHECK(S) FAILED ===\n", rc);

	return rc;
}