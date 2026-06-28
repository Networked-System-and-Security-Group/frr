// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Netlink protocol 199 (RTPROT_BGP_MIDR) kernel verification.
 *
 * Adds a route directly via netlink socket with RTPROT_BGP_MIDR=199,
 * verifies "ip route show proto midr" shows it, then deletes it.
 *
 * Proves: the kernel recognises protocol 199 as "midr", completing
 * the MIDR DP pipeline verification (DP → ZAPI → zebra → netlink → kernel).
 *
 * Compile: gcc -o /tmp/test_proto199 tests/bgpd/test_midr_proto199.c -lnl-3 -lnl-route-3
 * Run:     /tmp/test_proto199
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netlink/netlink.h>
#include <netlink/route/route.h>
#include <netlink/route/nexthop.h>
#include <netlink/route/link.h>
#include <netlink/addr.h>

#define RTPROT_BGP_MIDR 199

int main(void)
{
	struct nl_sock *sk;
	struct rtnl_route *rt;
	struct nl_addr *dst, *gw;
	struct rtnl_nexthop *nh;
	struct nl_cache *link_cache;
	int ifindex, rc = 0;

	printf("=== MIDR Protocol 199 Netlink Test ===\n");

	/* ---- Step 1: Create dummy interface ---- */
	printf("[Step 1] Create dummy_nl interface\n");
	system("ip link add dummy_nl type dummy 2>/dev/null || true");
	system("ip link set dummy_nl up 2>/dev/null || true");
	system("ip addr add 203.0.113.1/32 dev dummy_nl 2>/dev/null || true");
	sleep(1);

	/* ---- Step 2: Get ifindex ---- */
	sk = nl_socket_alloc();
	if (!sk) { printf("FAIL: nl_socket_alloc\n"); return 1; }
	nl_connect(sk, NETLINK_ROUTE);

	rtnl_link_alloc_cache(sk, AF_UNSPEC, &link_cache);
	ifindex = rtnl_link_name2i(link_cache, "dummy_nl");
	if (ifindex <= 0) { printf("FAIL: dummy_nl not found\n"); rc = 1; goto out; }
	printf("  ifindex = %d\n", ifindex);

	/* ---- Step 3: Add route with proto 199 ---- */
	printf("[Step 3] Add 10.200.200.0/24 via 203.0.113.1 proto 199\n");

	rt = rtnl_route_alloc();
	nl_addr_parse("10.200.200.0/24", AF_INET, &dst);
	rtnl_route_set_dst(rt, dst);
	rtnl_route_set_protocol(rt, RTPROT_BGP_MIDR);  /* key: proto 199 */
	rtnl_route_set_family(rt, AF_INET);
	rtnl_route_set_type(rt, RTN_UNICAST);
	rtnl_route_set_table(rt, RT_TABLE_MAIN);

	nh = rtnl_route_nh_alloc();
	nl_addr_parse("203.0.113.1", AF_INET, &gw);
	rtnl_route_nh_set_gateway(nh, gw);
	rtnl_route_nh_set_ifindex(nh, ifindex);
	rtnl_route_add_nexthop(rt, nh);

	if (rtnl_route_add(sk, rt, NLM_F_CREATE | NLM_F_REPLACE) < 0) {
		printf("FAIL: rtnl_route_add\n"); rc = 1;
		rtnl_route_put(rt);
		goto out;
	}
	rtnl_route_put(rt);
	printf("  Route added via netlink\n");
	sleep(1);

	/* ---- Step 4: Verify via "ip route show proto midr" ---- */
	printf("[Step 4] Verify proto midr visible\n");
	{
		FILE *fp = popen("ip route show proto midr 2>/dev/null", "r");
		char buf[256];
		int found = 0;
		while (fgets(buf, sizeof(buf), fp)) {
			printf("  %s", buf);
			if (strstr(buf, "10.200.200.0/24")) found = 1;
		}
		pclose(fp);
		if (!found) { printf("  FAIL: 10.200.200.0/24 not in proto midr\n"); rc = 1; }
		else printf("  OK: 10.200.200.0/24 shows as proto midr\n");
	}

	/* ---- Step 5: Also check via "ip route show proto 199" ---- */
	printf("[Step 5] Verify proto 199 visible\n");
	{
		FILE *fp = popen("ip route show proto 199 2>/dev/null", "r");
		char buf[256];
		int found = 0;
		while (fgets(buf, sizeof(buf), fp)) {
			if (strstr(buf, "10.200.200.0/24")) found = 1;
		}
		pclose(fp);
		if (!found) { printf("  FAIL: 10.200.200.0/24 not in proto 199\n"); rc = 1; }
		else printf("  OK: 10.200.200.0/24 shows as proto 199\n");
	}

	/* ---- Step 6: Delete route with proto 199 ---- */
	printf("[Step 6] Delete route with proto 199\n");
	rt = rtnl_route_alloc();
	nl_addr_parse("10.200.200.0/24", AF_INET, &dst);
	rtnl_route_set_dst(rt, dst);
	rtnl_route_set_protocol(rt, RTPROT_BGP_MIDR);
	rtnl_route_set_family(rt, AF_INET);
	rtnl_route_set_type(rt, RTN_UNICAST);
	rtnl_route_set_table(rt, RT_TABLE_MAIN);
	/* Must set nexthop to match the route added in Step 3 */
	nh = rtnl_route_nh_alloc();
	nl_addr_parse("203.0.113.1", AF_INET, &gw);
	rtnl_route_nh_set_gateway(nh, gw);
	rtnl_route_nh_set_ifindex(nh, ifindex);
	rtnl_route_add_nexthop(rt, nh);
	if (rtnl_route_delete(sk, rt, 0) < 0) {
		printf("  FAIL: rtnl_route_delete failed\n"); rc = 1;
	}
	rtnl_route_put(rt);
	sleep(1);

	/* ---- Step 7: Verify gone ---- */
	printf("[Step 7] Verify route removed\n");
	{
		FILE *fp = popen("ip route show 10.200.200.0/24 2>/dev/null", "r");
		char buf[256];
		int found = 0;
		while (fgets(buf, sizeof(buf), fp)) found++;
		pclose(fp);
		if (found) { printf("  FAIL: route still present\n"); rc = 1; }
		else printf("  OK: route deleted\n");
	}

	/* ---- Cleanup ---- */
	system("ip link del dummy_nl 2>/dev/null || true");

out:
	nl_close(sk);
	nl_socket_free(sk);
	if (rc == 0) printf("\n=== ALL CHECKS PASSED (proto 199 = midr verified) ===\n");
	else printf("\n=== %d CHECK(S) FAILED ===\n", rc);
	return rc;
}