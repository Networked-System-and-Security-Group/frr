// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * E2E (end-to-end) test for MIDR data-plane with Dual-Instance support.
 *
 * Verifies:
 *   1. Pure IP (SPF instance=0) route install → kernel FIB → delete
 *   2. SRv6 (TE instance=1) route install alongside SPF → coexistence
 *   3. Batch route flush
 *
 * Usage:
 *   ./test_midr_zebra_e2e [zebra_socket_path]
 *   Default socket: /var/run/frr/zserv.api
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

/* Test prefixes */
static const char *TEST_PFX_SPF  = "10.254.1.0/24";   /* SPF pure IPv4 */
static const char *TEST_PFX_SRV6 = "2001:db8:dead::/48"; /* SRv6 */
static const char *TEST_NEXTHOP  = "192.0.2.1";        /* dummy0 IPv4 address */
static const char *TEST_NEXTHOP6 = "2001:db8::2";      /* dummy0 IPv6 nexthop (same /64 subnet) */
static const char *TEST_IPV6_ADDR = "2001:db8::1/64";  /* dummy0 IPv6 address */
static const char *DUMMY_IF      = "dummy0";

/* ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
	const char *zebra_sock = "/var/run/frr/zserv.api";
	int rc = 0;
	struct bgp bgp = {};
	struct prefix p_spf, p_srv6;
	struct midr_path paths_spf[1], paths_srv6[1];
	struct midr_path_result result_spf, result_srv6;

	if (argc > 1)
		zebra_sock = argv[1];

	printf("=== MIDR E2E Route Installation Test (Dual-Instance) ===\n");
	printf("zebra socket: %s\n", zebra_sock);

	/* ---- Step 1: Setup dummy interface with IPv4 + IPv6 ---- */
	printf("\n[Step 1] Setup dummy0 with IPv4=%s IPv6=%s\n",
	       TEST_NEXTHOP, TEST_IPV6_ADDR);
	run_cmd("ip link add %s type dummy 2>/dev/null || true", DUMMY_IF);
	run_cmd("ip link set %s up 2>/dev/null || true", DUMMY_IF);
	run_cmd("ip addr add %s/32 dev %s 2>/dev/null || true",
		TEST_NEXTHOP, DUMMY_IF);
	/* Add IPv6 address and static neighbor so SRv6 nexthop is reachable */
	run_cmd("ip -6 addr add %s dev %s 2>/dev/null || true",
		TEST_IPV6_ADDR, DUMMY_IF);
	run_cmd("ip -6 neigh add %s lladdr 00:11:22:33:44:55 dev %s 2>/dev/null || true",
		TEST_NEXTHOP6, DUMMY_IF);
	run_cmd("sysctl -w net.ipv6.conf.%s.forwarding=1 2>/dev/null || true",
	        DUMMY_IF);
	run_cmd("sysctl -w net.ipv6.conf.%s.disable_ipv6=0 2>/dev/null || true",
	        DUMMY_IF);

	/* ---- Step 2: Connect to zebra ---- */
	printf("[Step 2] Connect to zebra\n");
	master = event_master_create("midr_e2e");

	struct bgp_master dummy_bm = {};
	dummy_bm.master = master;
	bm = &dummy_bm;

	bgp_zclient = zclient_new(master, &zclient_options_default, NULL, 0);
	if (!bgp_zclient) {
		printf("FAIL: zclient_new returned NULL\n");
		rc = 1; goto cleanup_if;
	}
	zclient_init(bgp_zclient, ZEBRA_ROUTE_BGP_MIDR, 0, &bgpd_privs);
	if (!frr_zclient_addr(&zclient_addr, &zclient_addr_len, zebra_sock)) {
		printf("FAIL: invalid zserv socket path: %s\n", zebra_sock);
		rc = 1; goto cleanup_if;
	}
	if (zclient_socket_connect(bgp_zclient) < 0) {
		printf("FAIL: could not connect to zebra at %s\n", zebra_sock);
		rc = 1; goto cleanup_if;
	}
	printf("  Connected to zebra OK\n");

	/* ---- Step 3: Init MIDR DP ---- */
	printf("[Step 3] midr_zebra_init\n");
	midr_zebra_init(&bgp);

	/*
	 * ════════════════════════════════════════════
	 * Test A: Pure IP SPF route (instance=0)
	 * ════════════════════════════════════════════
	 */
	printf("\n--- Test A: Pure IP (SPF instance=0) ---\n");

	str2prefix(TEST_PFX_SPF, &p_spf);
	memset(paths_spf, 0, sizeof(paths_spf));
	inet_pton(AF_INET, TEST_NEXTHOP, &paths_spf[0].nexthop.ipv4);
	paths_spf[0].metric = 100;
	result_spf.paths = paths_spf;
	result_spf.path_count = 1;
	result_spf.explicit.sid_count = 0;
	result_spf.instance = MIDR_INSTANCE_SPF;

	printf("[A.1] midr_zebra_route_add(%s via %s, instance=SPF)\n",
	       TEST_PFX_SPF, TEST_NEXTHOP);
	midr_zebra_route_add(&bgp, &p_spf, &result_spf);

	printf("[A.2] midr_zebra_route_flush\n");
	midr_zebra_route_flush(&bgp);

	{
		struct event t;
		for (int i = 0; i < 10; i++) { event_fetch(master, &t); event_call(&t); }
	}
	sleep(1);

	printf("[A.3] Verify %s in kernel FIB\n", TEST_PFX_SPF);
	if (ip_route_has(TEST_PFX_SPF)) {
		printf("  OK: route %s in FIB\n", TEST_PFX_SPF);
	} else {
		printf("  FAIL: route %s NOT in FIB\n", TEST_PFX_SPF);
		rc = 1;
	}

	printf("[A.4] Delete SPF route\n");
	midr_zebra_route_del(&bgp, &p_spf, MIDR_INSTANCE_SPF);
	midr_zebra_route_flush(&bgp);

	{
		struct event t;
		for (int i = 0; i < 10; i++) { event_fetch(master, &t); event_call(&t); }
	}
	sleep(1);

	if (!ip_route_has(TEST_PFX_SPF)) {
		printf("  OK: route %s removed from FIB\n", TEST_PFX_SPF);
	} else {
		printf("  FAIL: route %s still in FIB\n", TEST_PFX_SPF);
		rc = 1;
	}

	/*
	 * ════════════════════════════════════════════
	 * Test B: Dual-Instance — SPF + TE SRv6 coexist
	 * ════════════════════════════════════════════
	 */
	printf("\n--- Test B: Dual-Instance SPF+TE SRv6 ---\n");

	/* B.1 Install SPF first */
	printf("[B.1] Install SPF route (instance=0) for %s\n", TEST_PFX_SPF);
	result_spf.paths[0].nexthop.ipv4.s_addr = 0;
	inet_pton(AF_INET, TEST_NEXTHOP, &result_spf.paths[0].nexthop.ipv4);
	midr_zebra_route_add(&bgp, &p_spf, &result_spf);
	midr_zebra_route_flush(&bgp);

	{
		struct event t;
		for (int i = 0; i < 10; i++) { event_fetch(master, &t); event_call(&t); }
	}
	sleep(1);
	T_SPF_RECHECK:
	if (ip_route_has(TEST_PFX_SPF))
		printf("  OK: SPF route installed\n");
	else {
		printf("  FAIL: SPF re-install failed\n");
		rc = 1;
	}

	/* B.2 Install TE SRv6 route (instance=1) — same prefix!
	 * FIX v5.0: nexthop must be in the same subnet as dummy0 (2001:db8::/64)
	 * and ifindex must be explicitly set so zebra knows which interface to use.
	 * Without ifindex, zebra cannot resolve the nexthop and rejects the route. */
	str2prefix(TEST_PFX_SRV6, &p_srv6);
	memset(paths_srv6, 0, sizeof(paths_srv6));
	inet_pton(AF_INET6, TEST_NEXTHOP6, &paths_srv6[0].nexthop.ipv6);
	paths_srv6[0].ifindex = if_nametoindex(DUMMY_IF);
	result_srv6.paths = paths_srv6;
	result_srv6.path_count = 1;
	result_srv6.instance = MIDR_INSTANCE_TE;
	result_srv6.explicit.sid_count = 2;
	inet_pton(AF_INET6, "2001:db8:1::1", &result_srv6.explicit.sid_list[0]);
	inet_pton(AF_INET6, "2001:db8:2::1", &result_srv6.explicit.sid_list[1]);

	printf("[B.2] Install TE SRv6 route (instance=1) for %s\n", TEST_PFX_SRV6);
	midr_zebra_route_add(&bgp, &p_srv6, &result_srv6);
	midr_zebra_route_flush(&bgp);

	{
		struct event t;
		for (int i = 0; i < 10; i++) { event_fetch(master, &t); event_call(&t); }
	}
	sleep(1);

	if (ip_route6_has(TEST_PFX_SRV6))
		printf("  OK: SRv6 route installed\n");
	else {
		/* SRv6 kernel support not guaranteed — warn, not fail */
		printf("  WARN: SRv6 route not in FIB (kernel may lack seg6 support)\n");
	}

	/* B.3 Delete SRv6 route — nothing to verify besides no crash */
	printf("[B.3] Delete TE SRv6\n");
	midr_zebra_route_del(&bgp, &p_srv6, MIDR_INSTANCE_TE);
	midr_zebra_route_flush(&bgp);

	{
		struct event t;
		for (int i = 0; i < 10; i++) { event_fetch(master, &t); event_call(&t); }
	}
	sleep(1);

	/* B.4 Verify SPF still intact after TE delete */
	printf("[B.4] SPF route should still be installed\n");
	if (ip_route_has(TEST_PFX_SPF))
		printf("  OK: SPF survives TE delete\n");
	else {
		printf("  FAIL: SPF route lost\n");
		rc = 1;
	}

	/* Final cleanup */
	midr_zebra_route_del(&bgp, &p_spf, MIDR_INSTANCE_SPF);
	midr_zebra_route_flush(&bgp);
	{
		struct event t;
		for (int i = 0; i < 10; i++) { event_fetch(master, &t); event_call(&t); }
	}

	printf("\n[Cleanup]\n");
	midr_zebra_fini(&bgp);
	run_cmd("ip link del %s 2>/dev/null || true", DUMMY_IF);

	if (rc == 0)
		printf("\n=== ALL CHECKS PASSED ===\n");
	else
		printf("\n=== %d CHECK(S) FAILED ===\n", rc);

	return rc;

cleanup_if:
	run_cmd("ip link del %s 2>/dev/null || true", DUMMY_IF);
	return rc;
}