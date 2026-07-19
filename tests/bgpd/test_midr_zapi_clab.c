// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR ZAPI Containerlab Connectivity Test v2
 * Uses the SAME working pattern as test_midr_zapi_batch.c
 * (proven to install 64 routes successfully via ZAPI→zebra→kernel FIB)
 *
 * Compile:
 *   gcc -std=gnu11 -Wall -Wextra -g -O0 -include config.h \
 *       -I lib -I bgpd -I . $(pkg-config --cflags libyang 2>/dev/null) \
 *       -o /tmp/test_midr_zapi_clab tests/bgpd/test_midr_zapi_clab.c \
 *       bgpd/bgp_midr_zebra.o -L lib/.libs -lfrr \
 *       -lcap -lcrypt -ljson-c -lrt -lpthread -lsqlite3 -lresolv -ldl -lm -lfl -lyang
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

static int fib_has(const char *pfx) {
	char cmd[512];
	snprintf(cmd, sizeof(cmd), "ip route show %s proto 199 2>/dev/null | grep -q '%s'", pfx, pfx);
	return (system(cmd) == 0);
}

static void install_one(struct bgp *bgp, const char *pfx, const char *nh, uint32_t metric) {
	struct prefix p; struct midr_path paths[1]; struct midr_path_result r;
	str2prefix(pfx, &p);
	memset(paths, 0, sizeof(paths));
	inet_pton(AF_INET, nh, &paths[0].nexthop.ipv4);
	paths[0].metric = metric;
	r.paths = paths; r.path_count = 1;
	r.explicit.sid_count = 0; r.instance = MIDR_INSTANCE_SPF;
	midr_zebra_route_add(bgp, &p, &r);
}

static void delete_one(struct bgp *bgp, const char *pfx) {
	struct prefix p; str2prefix(pfx, &p);
	midr_zebra_route_del(bgp, &p);
}

static int mode_blackhole(const char *sock, const char *pfx, const char *nh, int hold) {
	struct bgp bgp = {};
	int fail = 0;

	printf("=== MIDR Blackhole (zebra-only) ===\n");

	/* Setup dummy_bh */
	printf("[1] Setup dummy_bh with %s/32...\n", nh);
	run_cmd("ip link add dummy_bh type dummy 2>/dev/null || true");
	run_cmd("ip link set dummy_bh up");
	run_cmd("ip addr add %s/32 dev dummy_bh 2>/dev/null || true", nh);

	/* Connect zebra - exact same pattern as working batch test */
	printf("[2] Connect zebra...\n");
	master = event_master_create("midr_clab");
	struct bgp_master dmb = {}; dmb.master = master; bm = &dmb;
	bgp_zclient = zclient_new(master, &zclient_options_default, NULL, 0);
	if (!bgp_zclient) { printf("FAIL: zclient_new\n"); return 1; }
	zclient_init(bgp_zclient, ZEBRA_ROUTE_BGP_MIDR, 0, &bgpd_privs);
	if (!frr_zclient_addr(&zclient_addr, &zclient_addr_len, sock)) {
		printf("FAIL: socket\n"); return 1;
	}
	if (zclient_socket_connect(bgp_zclient) < 0) {
		printf("FAIL: connect zebra\n"); return 1;
	}
	printf("  Connected OK\n");

	midr_zebra_init(&bgp);

	/* Install blackhole */
	printf("[3] Install blackhole...\n");
	install_one(&bgp, pfx, nh, 1);
	midr_zebra_route_flush(&bgp);
	sleep(3);

	/* Verify FIB */
	printf("[4] Verify FIB...\n");
	if (fib_has(pfx))
		printf("  OK: %s in FIB (proto 199)\n", pfx);
	else {
		printf("  FAIL: %s NOT in FIB\n", pfx);
		run_cmd("ip route show proto 199 2>/dev/null | head -5 || echo none");
		fail++;
	}

	/* Hold */
	printf("[5] Holding %ds (caller verifies blackhole)...\n", hold);
	fflush(stdout); sleep(hold);

	/* Delete */
	printf("[6] Delete blackhole...\n");
	delete_one(&bgp, pfx);
	midr_zebra_route_flush(&bgp);
	sleep(2);

	/* Verify removed */
	printf("[7] Verify removed...\n");
	if (!fib_has(pfx)) printf("  OK: removed\n");
	else { printf("  FAIL: still in FIB\n"); fail++; }

	/* Cleanup */
	midr_zebra_fini(&bgp);
	run_cmd("ip link del dummy_bh 2>/dev/null || true");

	if (fail == 0) printf("\n=== BLACKHOLE PASSED ===\n");
	else printf("\n=== BLACKHOLE FAILED (%d) ===\n", fail);
	return fail;
}

static int mode_stress(const char *sock, const char *pfx, const char *nh, int iters) {
	struct bgp bgp = {};
	int add_ok = 0, del_ok = 0, fail = 0;

	printf("=== MIDR Stress (zebra-only, %d iters) ===\n", iters);

	/* Setup dummy_bh */
	run_cmd("ip link add dummy_bh type dummy 2>/dev/null || true");
	run_cmd("ip link set dummy_bh up");
	run_cmd("ip addr add %s/32 dev dummy_bh 2>/dev/null || true", nh);

	/* Connect */
	printf("[1] Connect zebra...\n");
	master = event_master_create("midr_clab");
	struct bgp_master dmb = {}; dmb.master = master; bm = &dmb;
	bgp_zclient = zclient_new(master, &zclient_options_default, NULL, 0);
	if (!bgp_zclient) { printf("FAIL: zclient_new\n"); fail = 99; goto out; }
	zclient_init(bgp_zclient, ZEBRA_ROUTE_BGP_MIDR, 0, &bgpd_privs);
	if (!frr_zclient_addr(&zclient_addr, &zclient_addr_len, sock) || zclient_socket_connect(bgp_zclient) < 0) {
		printf("FAIL: connect zebra\n"); fail = 99; goto out;
	}
	printf("  Connected OK\n");

	midr_zebra_init(&bgp);

	/* Rapid add/del loop */
	printf("[2] Running %d iterations (~%ds)...\n", iters, iters);
	for (int i = 1; i <= iters; i++) {
		install_one(&bgp, pfx, nh, 1);
		midr_zebra_route_flush(&bgp);
		usleep(300000);
		if (fib_has(pfx)) add_ok++;
		delete_one(&bgp, pfx);
		midr_zebra_route_flush(&bgp);
		usleep(300000);
		if (!fib_has(pfx)) del_ok++;
		if (i % 10 == 0)
			printf("  iter %d/%d (add_ok=%d del_ok=%d)\n", i, iters, add_ok, del_ok);
	}

	printf("[3] Done. add_ok=%d/%d del_ok=%d/%d\n", add_ok, iters, del_ok, iters);
	if (add_ok != iters) { printf("  FAIL: installs\n"); fail++; }
	if (del_ok != iters) { printf("  FAIL: deletions\n"); fail++; }

out:
	midr_zebra_fini(&bgp);
	run_cmd("ip link del dummy_bh 2>/dev/null || true");
	if (fail == 0) printf("\n=== STRESS PASSED ===\n");
	else printf("\n=== STRESS FAILED ===\n");
	return fail;
}

int main(int argc, char **argv) {
	if (argc < 4) {
		fprintf(stderr, "Usage: %s <mode> <prefix> <nexthop> [param] [zebra_sock]\n"
			"  %s blackhole 10.100.0.0/24 10.200.0.1 5\n"
			"  %s stress    10.100.0.0/24 10.200.0.1 20\n", argv[0], argv[0], argv[0]);
		return 1;
	}
	const char *mode = argv[1], *pfx = argv[2], *nh = argv[3];
	int param = (argc > 4) ? atoi(argv[4]) : 5;
	const char *sock = (argc > 5) ? argv[5] : "/var/run/frr/zserv.api";

	if (strcmp(mode, "blackhole") == 0) return mode_blackhole(sock, pfx, nh, param);
	if (strcmp(mode, "stress") == 0) return mode_stress(sock, pfx, nh, param);
	fprintf(stderr, "Unknown mode: %s\n", mode);
	return 1;
}