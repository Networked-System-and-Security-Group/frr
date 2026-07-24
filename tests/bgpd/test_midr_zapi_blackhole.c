// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR ZAPI Blackhole Route Test  (Containerlab / integration-test only)
 *
 * Purpose:
 *   Prove that a MIDR route installed via the real midr_zebra_route_add
 *   API can steer (or blackhole) traffic in a live FRR topology.
 *
 * Scenario for 2-node Containerlab (r1,r2):
 *   1. r2 binds 10.100.0.1/32 on lo.
 *   2. BGP installs a reachable path from r1 -> 10.100.0.1.
 *   3. This binary runs INSIDE r1, connects to r1's zebra, and installs
 *      a MIDR route for 10.100.0.0/24 with nexthop=10.0.99.3 (dead host).
 *   4. During the MIDR route lifetime, r1->10.100.0.1 must be unreachable.
 *   5. After deleting the MIDR route, r1->10.100.0.1 is reachable again
 *      via the original BGP path.
 *
 * The caller (shell script) is responsible for verifying connectivity
 * around this binary (before / during / after).
 *
 * Compile (inside FRR dev container):
 *   gcc -std=gnu11 -Wall -Wextra -g -O0 -include config.h \
 *       -I lib -I bgpd -I .  $(pkg-config --cflags libyang) \
 *       -o /tmp/test_midr_blackhole \
 *       tests/bgpd/test_midr_zapi_blackhole.c bgpd/bgp_midr_zebra.o \
 *       -L lib/.libs -lfrr \
 *       -lcap -lcrypt -ljson-c -lrt -lpthread -lsqlite3 -lresolv \
 *       -ldl -lm -lfl -lyang
 *
 * Run (inside clab node r1):
 *   LD_LIBRARY_PATH=/tmp /tmp/test_midr_blackhole /var/run/frr/zserv.api
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
static const char *BLACKHOLE_PFX  = "10.100.0.0/24";  /* prefix we block   */
static const char *DEAD_NEXTHOP   = "10.200.0.1";     /* dummy_bh own addr  */
static const char *BLACKHOLE_IF   = "dummy_bh";       /* dead-end interface */
static const char *TARGET_HOST    = "10.100.0.1";     /* r2 lo address    */

/* ----------------------------------------------------------------------- */
int main(int argc, char **argv)
{
	const char *zebra_sock = "/var/run/frr/zserv.api";
	int rc = 0;
	struct bgp bgp = {};
	struct prefix p_bh;
	struct midr_path  paths_bh[1];
	struct midr_path_result result_bh;

	if (argc > 1)
		zebra_sock = argv[1];

	printf("=== MIDR Blackhole ZAPI Test ===\n");
	printf("zebra socket: %s\n", zebra_sock);
	printf("block prefix: %s  (target host %s)\n", BLACKHOLE_PFX,
	       TARGET_HOST);

	/* ---- setup dead-end interface so nexthop is valid ---- */
	printf("[1] Setup dead-end interface %s with %s/32 ...\n",
	       BLACKHOLE_IF, DEAD_NEXTHOP);
	run_cmd("ip link add %s type dummy 2>/dev/null || true", BLACKHOLE_IF);
	run_cmd("ip link set %s up 2>/dev/null || true", BLACKHOLE_IF);
	run_cmd("ip addr add %s/32 dev %s 2>/dev/null || true",
		DEAD_NEXTHOP, BLACKHOLE_IF);

	/* ---- connect to zebra ---- */
	printf("[2] Connect to zebra ...\n");
	master = event_master_create("midr_bh");

	struct bgp_master dummy_bm = {};
	dummy_bm.master = master;
	bm = &dummy_bm;

	bgp_zclient = zclient_new(master, &zclient_options_default, NULL, 0);
	if (!bgp_zclient) {
		printf("FAIL: zclient_new\n");
		rc = 1; goto out;
	}
	zclient_init(bgp_zclient, ZEBRA_ROUTE_BGP_MIDR, 0, &bgpd_privs);
	if (!frr_zclient_addr(&zclient_addr, &zclient_addr_len, zebra_sock)) {
		printf("FAIL: invalid socket path %s\n", zebra_sock);
		rc = 1; goto out;
	}
	if (zclient_socket_connect(bgp_zclient) < 0) {
		printf("FAIL: cannot connect to zebra\n");
		rc = 1; goto out;
	}
	printf("  Connected OK\n");

	/* ---- init MIDR DP ---- */
	printf("[3] midr_zebra_init ...\n");
	midr_zebra_init(&bgp);

	/* ---- build a blackhole route ---- */
	printf("[4] Build blackhole route: %s via %s metric=1\n",
	       BLACKHOLE_PFX, DEAD_NEXTHOP);
	str2prefix(BLACKHOLE_PFX, &p_bh);
	memset(paths_bh, 0, sizeof(paths_bh));
	inet_pton(AF_INET, DEAD_NEXTHOP, &paths_bh[0].nexthop.ipv4);
	paths_bh[0].metric = 1;          /* lower than BGP → wins */
	paths_bh[0].weight = 0;
	result_bh.paths       = paths_bh;
	result_bh.path_count  = 1;
	result_bh.explicit.sid_count = 0;
	result_bh.instance    = MIDR_INSTANCE_SPF;

	/* ---- install the blackhole ---- */
	printf("[5] midr_zebra_route_add + flush ...\n");
	midr_zebra_route_add(&bgp, &p_bh, &result_bh);
	midr_zebra_route_flush(&bgp);

	/* Let the event loop drain so ZAPI messages reach zebra */
	{
		struct event t;
		for (int i = 0; i < 10; i++) {
			event_fetch(master, &t);
			event_call(&t);
		}
	}
	sleep(1);

	/* ---- verify in kernel FIB ---- */
	printf("[6] Verify %s in kernel FIB (proto 199/midr)\n",
	       BLACKHOLE_PFX);
	if (run_cmd("ip route show %s proto 199 2>/dev/null | grep -q '%s'",
		    BLACKHOLE_PFX, BLACKHOLE_PFX) == 0)
		printf("  OK: %s in FIB as proto 199\n", BLACKHOLE_PFX);
	else {
		printf("  FAIL: %s NOT in FIB\n", BLACKHOLE_PFX);
		rc = 1;
	}

	/* ---- sleep so the caller can test connectivity ---- */
	printf("[7] Holding blackhole for 5 seconds (caller verifies "
	       "blackhole)...\n");
	sleep(5);

	/* ---- delete the blackhole ---- */
	printf("[8] Delete blackhole route ...\n");
	midr_zebra_route_del(&bgp, &p_bh, MIDR_INSTANCE_SPF);
	midr_zebra_route_flush(&bgp);

	{
		struct event t;
		for (int i = 0; i < 10; i++) {
			event_fetch(master, &t);
			event_call(&t);
		}
	}
	sleep(1);

	/* ---- verify it is gone ---- */
	printf("[9] Verify %s removed from FIB ...\n", BLACKHOLE_PFX);
	if (run_cmd("ip route show %s proto 199 2>/dev/null | grep -q '%s'",
		    BLACKHOLE_PFX, BLACKHOLE_PFX) != 0)
		printf("  OK: %s removed from FIB\n", BLACKHOLE_PFX);
	else {
		printf("  FAIL: %s still in FIB\n", BLACKHOLE_PFX);
		rc = 1;
	}

	printf("[10] Final cleanup ...\n");
	midr_zebra_fini(&bgp);
	run_cmd("ip link del %s 2>/dev/null || true", BLACKHOLE_IF);

	if (rc == 0)
		printf("\n=== BLACKHOLE ZAPI TEST PASSED ===\n");
	else
		printf("\n=== %d CHECK(S) FAILED ===\n", rc);

out:
	return rc;
}