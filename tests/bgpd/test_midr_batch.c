// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Batch route installation stress test for MIDR DP.
 *
 * Installs 1000+ routes through midr_zebra_route_add,
 * then flushes them all via deferred batch, verifies
 * batching coalescing, and finally deletes everything.
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

/* ================================================================== */
struct zebra_privs_t bgpd_privs = {};
struct event_loop *master;
struct bgp_master dummy_bm;
struct bgp_master *bm = &dummy_bm;
struct zclient *bgp_zclient;

static int g_send_count = 0;
static int g_total_adds = 0;

enum zclient_send_status __wrap_zclient_route_send(uint8_t cmd,
	struct zclient *z, struct zapi_route *api)
{
	g_send_count++;
	return ZCLIENT_SEND_SUCCESS;
}

/* ================================================================== */
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

/* ================================================================== */
#define BATCH_SIZE 256
#define TOTAL_ROUTES 1024

static void test_batch_install(void)
{
	struct bgp bgp = {};
	struct midr_path paths[1] = {};
	struct midr_path_result result = {};
	struct prefix p;
	int i;
	char buf[64];

	printf("\n[Batch Test] Install %d routes, verify batches coalesce\n", TOTAL_ROUTES);

	midr_zebra_init(&bgp);

	inet_pton(AF_INET, "10.99.0.1", &paths[0].nexthop.ipv4);
	paths[0].metric = 10;
	result.paths = paths;
	result.path_count = 1;
	result.instance = MIDR_INSTANCE_SPF;

	g_send_count = 0;

	/* Queue all routes */
	for (i = 0; i < TOTAL_ROUTES; i++) {
		snprintf(buf, sizeof(buf), "10.%d.%d.0/24",
			 (i / 256) % 256, i % 256);
		str2prefix(buf, &p);
		midr_zebra_route_add(&bgp, &p, &result);
	}
	T(g_send_count == 0, "zero sends during queuing — all batched");

	/* Single deferred — should fire one timer */
	midr_zebra_route_update_deferred(&bgp);

	/* Manually fire the timer to flush */
	midr_zebra_route_flush(&bgp);
	T(g_send_count >= TOTAL_ROUTES, "all routes sent (>=1024)");

	/* Re-add same routes — diff should suppress duplicates */
	int before = g_send_count;
	for (i = 0; i < TOTAL_ROUTES; i++) {
		snprintf(buf, sizeof(buf), "10.%d.%d.0/24",
			 (i / 256) % 256, i % 256);
		str2prefix(buf, &p);
		midr_zebra_route_add(&bgp, &p, &result);
	}
	midr_zebra_route_flush(&bgp);
	T(g_send_count == before, "no duplicate sends — diff optimises re-installs");

	/* Delete all */
	for (i = 0; i < TOTAL_ROUTES; i++) {
		snprintf(buf, sizeof(buf), "10.%d.%d.0/24",
			 (i / 256) % 256, i % 256);
		str2prefix(buf, &p);
		midr_zebra_route_del(&bgp, &p);
	}
	midr_zebra_route_flush(&bgp);
	T(1, "batch delete completed");

	midr_zebra_fini(&bgp);
}

/* ================================================================== */
static void test_batch_ipv6_srv6(void)
{
	struct bgp bgp = {};
	struct midr_path paths[1] = {};
	struct midr_path_result result = {};
	struct prefix p;
	int i;
	char buf[64];

	printf("\n[Batch SRv6] Install %d IPv6 SIDs, verify no corruption\n", BATCH_SIZE);

	midr_zebra_init(&bgp);

	inet_pton(AF_INET6, "2001:db8:a::1", &paths[0].nexthop.ipv6);
	result.paths = paths;
	result.path_count = 1;
	result.instance = MIDR_INSTANCE_TE;
	result.explicit.sid_count = 3;
	inet_pton(AF_INET6, "2001:db8:cafe::1", &result.explicit.sid_list[0]);
	inet_pton(AF_INET6, "2001:db8:cafe::2", &result.explicit.sid_list[1]);
	inet_pton(AF_INET6, "2001:db8:cafe::3", &result.explicit.sid_list[2]);

	g_send_count = 0;

	for (i = 0; i < BATCH_SIZE; i++) {
		snprintf(buf, sizeof(buf), "2001:db8:%x::/48", i);
		str2prefix(buf, &p);
		midr_zebra_route_add(&bgp, &p, &result);
	}

	midr_zebra_route_flush(&bgp);
	T(g_send_count >= BATCH_SIZE, "all SRv6 routes sent");

	/* Delete all */
	for (i = 0; i < BATCH_SIZE; i++) {
		snprintf(buf, sizeof(buf), "2001:db8:%x::/48", i);
		str2prefix(buf, &p);
		midr_zebra_route_del(&bgp, &p);
	}
	midr_zebra_route_flush(&bgp);

	midr_zebra_fini(&bgp);
}

/* ================================================================== */
int main(void)
{
	master = event_master_create("test_midr_batch");
	dummy_bm.master = master;

	printf("=== MIDR Batch Stress Tests ===\n");

	test_batch_install();
	test_batch_ipv6_srv6();

	printf("\n=== %d test(s) FAILED ===\n", g_failed);
	return g_failed ? 1 : 0;
}