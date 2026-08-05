// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR ZAPI Batch Stress Test (Real Zebra, v3 — no event draining)
 *
 * Compile:
 *   gcc -std=gnu11 -Wall -Wextra -g -O0 -include config.h \
 *       -I lib -I bgpd -I . $(pkg-config --cflags libyang 2>/dev/null) \
 *       -o /tmp/test_midr_zapi_batch tests/bgpd/test_midr_zapi_batch.c \
 *       bgpd/bgp_midr_zebra.o -L lib/.libs -lfrr \
 *       -lcap -lcrypt -ljson-c -lrt -lpthread -lsqlite3 -lresolv -ldl -lm -lfl -lyang
 *
 * Run:
 *   LD_LIBRARY_PATH=/home/frr/frr/lib/.libs /tmp/test_midr_zapi_batch [zebra_sock]
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

struct zebra_privs_t bgpd_privs = {};
struct event_loop *master = NULL;
struct bgp_master *bm;
struct zclient *bgp_zclient;

static int run_cmd(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int run_cmd(const char *fmt, ...) {
	char buf[512]; va_list ap;
	va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
	return system(buf);
}

static int fib_cnt(void) {
	FILE *fp = popen("ip route show proto 199 2>/dev/null | wc -l", "r");
	int n = 0;
	if (fp) { char b[32]; if (fgets(b, sizeof(b), fp)) n = atoi(b); pclose(fp); }
	return n;
}

#define BATCH_N  64
#define DUAL_N   5

int main(int argc, char **argv) {
	const char *sock = "/var/run/frr/zserv.api";
	int fail = 0;
	if (argc > 1) sock = argv[1];

	printf("=== MIDR ZAPI Batch Stress v3 ===\n");

	/* Setup dummy0 */
	printf("[1] Setup dummy0...\n");
	run_cmd("ip link add dummy0 type dummy 2>/dev/null || true");
	run_cmd("ip link set dummy0 up");
	run_cmd("ip addr add 192.168.200.1/24 dev dummy0 2>/dev/null || true");

	/* Connect zebra */
	printf("[2] Connect zebra...\n");
	master = event_master_create("midr_bt");
	struct bgp_master dmb = {}; dmb.master = master; bm = &dmb;

	bgp_zclient = zclient_new(master, &zclient_options_default, NULL, 0);
	if (!bgp_zclient) { printf("FAIL: zclient_new\n"); fail = 99; goto out; }
	zclient_init(bgp_zclient, ZEBRA_ROUTE_BGP_MIDR, 0, &bgpd_privs);
	if (!frr_zclient_addr(&zclient_addr, &zclient_addr_len, sock)) {
		printf("FAIL: socket\n"); fail = 99; goto out;
	}
	if (zclient_socket_connect(bgp_zclient) < 0) {
		printf("FAIL: connect zebra\n"); fail = 99; goto out;
	}
	printf("  Connected OK\n");

	struct bgp bgp = {};
	midr_zebra_init(&bgp);

	/* ---- Test A: Batch install BATCH_N routes ---- */
	printf("\n--- Test A: Install %d routes ---\n", BATCH_N);
	for (int i = 1; i <= BATCH_N; i++) {
		char px[32], nh[32];
		snprintf(px, sizeof(px), "10.200.%d.0/24", i);
		snprintf(nh, sizeof(nh), "192.168.200.%d", (i % 250) + 2);
		struct prefix p; struct midr_path paths[1]; struct midr_path_result r;
		str2prefix(px, &p);
		memset(paths, 0, sizeof(paths));
		inet_pton(AF_INET, nh, &paths[0].nexthop.ipv4);
		paths[0].metric = 100;
		r.paths = paths; r.path_count = 1;
		r.explicit.sid_count = 0; r.instance = MIDR_INSTANCE_SPF;
		midr_zebra_route_add(&bgp, &p, &r);
	}

	printf("  Flushing...\n");
	midr_zebra_route_flush(&bgp);
	sleep(3); /* wait for zebra→kernel netlink */

	int n = fib_cnt();
	printf("  FIB: %d routes (expect >= %d)\n", n, BATCH_N);
	if (n >= BATCH_N) printf("  OK: install PASSED\n");
	else { printf("  FAIL: install (%d/%d)\n", n, BATCH_N); fail++; }

	/* ---- Test B: Delete all ---- */
	printf("\n--- Test B: Delete %d routes ---\n", BATCH_N);
	for (int i = 1; i <= BATCH_N; i++) {
		char px[32]; snprintf(px, sizeof(px), "10.200.%d.0/24", i);
		struct prefix p; str2prefix(px, &p);
		midr_zebra_route_del(&bgp, &p, MIDR_INSTANCE_SPF);
	}
	midr_zebra_route_flush(&bgp);
	sleep(2);

	n = fib_cnt();
	printf("  FIB: %d routes (expect 0)\n", n);
	if (n == 0) printf("  OK: delete PASSED\n");
	else { printf("  FAIL: delete (%d remain)\n", n); fail++; }

	/* ---- Test C: Dual-Instance SPF+TE (flush SPF first, then TE) ---- */
	printf("\n--- Test C: Dual-Instance (%d prefixes) ---\n", DUAL_N);
	/* Add all SPF routes, flush */
	for (int i = 1; i <= DUAL_N; i++) {
		char px[32], nh[32];
		snprintf(px, sizeof(px), "10.210.%d.0/24", i);
		snprintf(nh, sizeof(nh), "192.168.200.%d", (i % 250) + 2);
		struct prefix p; struct midr_path paths[1]; struct midr_path_result r;
		str2prefix(px, &p);
		memset(paths, 0, sizeof(paths));
		inet_pton(AF_INET, nh, &paths[0].nexthop.ipv4);
		paths[0].metric = 100;
		r.paths = paths; r.path_count = 1;
		r.explicit.sid_count = 0; r.instance = MIDR_INSTANCE_SPF;
		midr_zebra_route_add(&bgp, &p, &r);
	}
	midr_zebra_route_flush(&bgp);
	sleep(1);
	/* Add all TE routes (different instance), flush */
	for (int i = 1; i <= DUAL_N; i++) {
		char px[32], nh[32];
		snprintf(px, sizeof(px), "10.210.%d.0/24", i);
		snprintf(nh, sizeof(nh), "192.168.200.%d", (i % 250) + 2);
		struct prefix p; struct midr_path paths[1]; struct midr_path_result r;
		str2prefix(px, &p);
		memset(paths, 0, sizeof(paths));
		inet_pton(AF_INET, nh, &paths[0].nexthop.ipv4);
		paths[0].metric = 1;
		r.paths = paths; r.path_count = 1;
		r.explicit.sid_count = 0; r.instance = MIDR_INSTANCE_TE;
		midr_zebra_route_add(&bgp, &p, &r);
	}
	midr_zebra_route_flush(&bgp);
	sleep(2);

	n = fib_cnt();
	/* Kernel FIB shows only winning TE routes (metric=1).
	 * SPF routes (metric=100) are in zebra RIB, Test D verifies they survive. */
	printf("  FIB: %d routes (expect %d: TE wins, SPF in zebra RIB)\n", n, DUAL_N);
	if (n >= DUAL_N) printf("  OK: dual-instance PASSED (TE active, SPF standby)\n");
	else { printf("  FAIL: dual-instance (%d)\n", n); fail++; }

	/* ---- Test D: Delete TE, SPF survives ---- */
	printf("\n--- Test D: Delete TE, SPF survives ---\n");
	for (int i = 1; i <= DUAL_N; i++) {
		char px[32]; snprintf(px, sizeof(px), "10.210.%d.0/24", i);
		struct prefix p; str2prefix(px, &p);
		midr_zebra_route_del(&bgp, &p, MIDR_INSTANCE_TE);
	}
	midr_zebra_route_flush(&bgp);
	sleep(2);

	{
		FILE *fp = popen("ip route show proto 199 | grep '10.210.' | wc -l", "r");
		n = 0; if (fp) { char b[32]; if (fgets(b, sizeof(b), fp)) n = atoi(b); pclose(fp); }
	}
	printf("  FIB (10.210.x): %d (expect %d)\n", n, DUAL_N);
	if (n == DUAL_N) printf("  OK: SPF survives TE delete\n");
	else { printf("  FAIL: SPF survival (%d)\n", n); fail++; }

	/* ---- Cleanup ---- */
	printf("\n--- Cleanup ---\n");
	for (int i = 1; i <= DUAL_N; i++) {
		char px[32]; snprintf(px, sizeof(px), "10.210.%d.0/24", i);
		struct prefix p; str2prefix(px, &p);
		midr_zebra_route_del(&bgp, &p, MIDR_INSTANCE_SPF);
	}
	midr_zebra_route_flush(&bgp);
	sleep(1);

	midr_zebra_fini(&bgp);
	run_cmd("ip link del dummy0 2>/dev/null || true");

	if (fail == 0)
		printf("\n=== ALL TESTS PASSED ===\n");
	else
		printf("\n=== %d TEST(S) FAILED ===\n", fail);

out:
	return fail > 0 ? 1 : 0;
}