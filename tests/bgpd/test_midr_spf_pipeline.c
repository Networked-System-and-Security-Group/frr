// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR path-computation pipeline test.
 *
 * This test deliberately stops at the zclient boundary: TED publishes a
 * snapshot, SPF consumes it after debounce, the install adapter computes a
 * diff, and the Zebra adapter emits the resulting ZAPI operations.
 */

#include <zebra.h>

#include "lib/frrevent.h"
#include "lib/privs.h"
#include "lib/zclient.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_midr_private.h"
#include "bgpd/bgp_midr_spf.h"
#include "bgpd/bgp_midr_spf_install.h"
#include "bgpd/bgp_midr_ted_private.h"
#include "bgpd/bgp_midr_zebra.h"

struct zebra_privs_t bgpd_privs = {};
struct event_loop *master;
static struct bgp_master test_bm;
struct bgp_master *bm = &test_bm;
struct zclient *bgp_zclient;

#define SENT_ROUTE_MAX 32

struct sent_route {
	uint8_t command;
	struct zapi_route route;
};

static struct sent_route sent_routes[SENT_ROUTE_MAX];
static size_t sent_route_count;

extern enum zclient_send_status __wrap_zclient_route_send(uint8_t command,
							struct zclient *zclient,
							struct zapi_route *route);

enum zclient_send_status __wrap_zclient_route_send(uint8_t command,
							struct zclient *zclient,
							struct zapi_route *route)
{
	(void)zclient;
	assert(sent_route_count < array_size(sent_routes));
	sent_routes[sent_route_count].command = command;
	sent_routes[sent_route_count].route = *route;
	sent_route_count++;
	return ZCLIENT_SEND_SUCCESS;
}

static uint32_t node_id(const char *text)
{
	struct in_addr address;

	assert(inet_pton(AF_INET, text, &address) == 1);
	return address.s_addr;
}

static struct ipaddr ip_address(const char *text)
{
	struct ipaddr address;

	assert(str2ipaddr(text, &address) == 0);
	return address;
}

static struct midr_ted_prefix_key prefix_key(const char *text)
{
	struct midr_ted_prefix_key key = {.safi = SAFI_UNICAST};

	assert(str2prefix(text, &key.prefix) > 0);
	key.afi = key.prefix.family == AF_INET ? AFI_IP : AFI_IP6;
	return key;
}

static void run_one_event(void)
{
	struct event event;

	assert(event_fetch(master, &event));
	event_call(&event);
}

static void reset_sent_routes(void)
{
	memset(sent_routes, 0, sizeof(sent_routes));
	sent_route_count = 0;
}

static void publish_snapshot(struct midr_context *ctx, uint32_t cost,
				     bool include_prefixes, uint64_t sync_reason_flags)
{
	const uint32_t local = node_id("1.1.1.1");
	const uint32_t remote4 = node_id("2.2.2.2");
	const uint32_t remote6 = node_id("3.3.3.3");
	struct midr_ted_builder *builder = NULL;
	struct midr_ted_node local_node = {.node_id = local, .group_id = 100};
	struct midr_ted_node remote4_node = {.node_id = remote4, .group_id = 100};
	struct midr_ted_node remote6_node = {.node_id = remote6, .group_id = 100};
	struct midr_ted_link_input v4_link = {
		.local_node_id = local,
		.remote_node_id = remote4,
		.link_id = 1,
		.canonical_cost = cost,
		.link_local_address = ip_address("192.0.2.1"),
		.link_remote_address = ip_address("192.0.2.2"),
		.local_ifindex = 7,
	};
	struct midr_ted_link_input v6_link = {
		.local_node_id = local,
		.remote_node_id = remote6,
		.link_id = 2,
		.canonical_cost = cost,
		.link_local_address = ip_address("2001:db8::1"),
		.link_remote_address = ip_address("2001:db8::2"),
		.local_ifindex = 8,
	};
	struct midr_ted_node_prefix v4_prefix = {
		.key = prefix_key("10.10.0.0/16"),
		.node_id = remote4,
	};
	struct midr_ted_node_prefix v6_prefix = {
		.key = prefix_key("2001:db8:100::/64"),
		.node_id = remote6,
	};

	assert(midr_ted_builder_create(local, 100, &builder) == 0);
	assert(midr_ted_builder_add_node(builder, &local_node) == 0);
	assert(midr_ted_builder_add_node(builder, &remote4_node) == 0);
	assert(midr_ted_builder_add_node(builder, &remote6_node) == 0);
	assert(midr_ted_builder_add_link(builder, &v4_link) == 0);
	assert(midr_ted_builder_add_link(builder, &v6_link) == 0);
	if (include_prefixes) {
		assert(midr_ted_builder_add_node_prefix(builder, &v4_prefix) == 0);
		assert(midr_ted_builder_add_node_prefix(builder, &v6_prefix) == 0);
	}
	assert(midr_ted_builder_publish(ctx, builder, sync_reason_flags) == 0);
	midr_ted_builder_destroy(&builder);
}

static void assert_add(size_t index, int family, uint32_t metric, ifindex_t ifindex)
{
	const struct sent_route *sent = &sent_routes[index];

	assert(sent->command == ZEBRA_ROUTE_ADD);
	assert(sent->route.type == ZEBRA_ROUTE_BGP_MIDR);
	assert(sent->route.instance == MIDR_INSTANCE_SPF);
	assert(sent->route.metric == metric);
	assert(sent->route.nexthop_num == 1);
	assert(sent->route.nexthops[0].ifindex == ifindex);
	if (family == AF_INET)
		assert(sent->route.nexthops[0].type == NEXTHOP_TYPE_IPV4_IFINDEX);
	else
		assert(sent->route.nexthops[0].type == NEXTHOP_TYPE_IPV6_IFINDEX);
}

static void test_spf_pipeline(void)
{
	struct bgp bgp = {.vrf_id = VRF_DEFAULT};
	struct midr_context ctx = {.bgp = &bgp};
	struct midr_spf_runtime_status status;
	const struct midr_spf_results *results = NULL;

	master = event_master_create("MIDR SPF pipeline test");
	test_bm.master = master;
	reset_sent_routes();

	assert(midr_ted_context_init(&ctx) == 0);
	midr_zebra_init(&bgp);
	assert(midr_spf_context_init(&ctx) == 0);

	/* The initial runtime event observes that no READY snapshot exists. */
	run_one_event();
	assert(midr_spf_results_get(&ctx, &results) == -EAGAIN);

	/* One TED publication produces one v4 and one v6 ZAPI add. */
	publish_snapshot(&ctx, 10, true, 0);
	run_one_event();
	midr_zebra_route_flush(&bgp);
	assert(midr_spf_runtime_status_get(&ctx, &status) == 0);
	assert(sent_route_count == 2);
	assert_add(0, AF_INET, 10, 7);
	assert_add(1, AF_INET6, 10, 8);
	assert(midr_spf_results_get(&ctx, &results) == 0);
	assert(midr_spf_results_generation(results) == 1);
	assert(midr_spf_results_count(results) == 2);
	midr_spf_results_release(&results);

	/* Two publications before the debounce event collapse to the newest one. */
	reset_sent_routes();
	publish_snapshot(&ctx, 20, true, 0);
	publish_snapshot(&ctx, 7, true, 0);
	assert(midr_spf_runtime_status_get(&ctx, &status) == 0);
	assert(status.recompute_pending);
	assert(status.pending_generation == 3);
	run_one_event();
	midr_zebra_route_flush(&bgp);
	assert(sent_route_count == 4);
	assert(sent_routes[0].command == ZEBRA_ROUTE_DELETE);
	assert_add(1, AF_INET, 7, 7);
	assert(sent_routes[2].command == ZEBRA_ROUTE_DELETE);
	assert_add(3, AF_INET6, 7, 8);

	/* A generation-only sync change has identical forwarding and emits nothing. */
	reset_sent_routes();
	publish_snapshot(&ctx, 7, true, MIDR_TED_SYNC_REASON_EOR_TIMEOUT);
	run_one_event();
	midr_zebra_route_flush(&bgp);
	assert(sent_route_count == 0);

	/* A metric change is reflected as a delete/add pair by the CP adapter. */
	reset_sent_routes();
	publish_snapshot(&ctx, 11, true, MIDR_TED_SYNC_REASON_EOR_TIMEOUT);
	run_one_event();
	midr_zebra_route_flush(&bgp);
	assert(sent_route_count == 4);
	assert(sent_routes[0].command == ZEBRA_ROUTE_DELETE);
	assert(sent_routes[1].command == ZEBRA_ROUTE_ADD);
	assert(sent_routes[2].command == ZEBRA_ROUTE_DELETE);
	assert(sent_routes[3].command == ZEBRA_ROUTE_ADD);
	assert(sent_routes[1].route.metric == 11);
	assert(sent_routes[3].route.metric == 11);

	/* Removing all prefixes withdraws both cached routes. */
	reset_sent_routes();
	publish_snapshot(&ctx, 11, false, 0);
	run_one_event();
	midr_zebra_route_flush(&bgp);
	assert(sent_route_count == 2);
	assert(sent_routes[0].command == ZEBRA_ROUTE_DELETE);
	assert(sent_routes[1].command == ZEBRA_ROUTE_DELETE);

	/* Runtime shutdown also drains the cached routes through Zebra. */
	publish_snapshot(&ctx, 9, true, 0);
	run_one_event();
	midr_zebra_route_flush(&bgp);
	reset_sent_routes();
	midr_spf_context_finish(&ctx);
	assert(sent_route_count == 0);
	midr_zebra_fini(&bgp);
	assert(sent_route_count == 2);
	assert(sent_routes[0].command == ZEBRA_ROUTE_DELETE);
	assert(sent_routes[1].command == ZEBRA_ROUTE_DELETE);

	midr_ted_context_finish(&ctx);
	event_master_free(master);
	master = NULL;
	test_bm.master = NULL;
}

int main(void)
{
	test_spf_pipeline();
	printf("MIDR SPF pipeline tests passed\n");
	return 0;
}
