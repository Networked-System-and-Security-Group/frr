#define main midrd_program_main
#include "midrd.c"
#undef main

#include <assert.h>

#include "midr-spf-install.h"
#include "midr-zebra.h"

struct zebra_probe {
	size_t adds;
	size_t deletes;
	size_t deferred;
	size_t flushes;
	size_t aborts;
	size_t add_attempts;
	size_t fail_add_attempt;
	struct prefix last_prefix;
	struct midr_path last_path;
	uint8_t last_instance;
	uint8_t last_sid_count;
};

static int probe_add(void *arg, const struct prefix *prefix,
		     const struct midr_path_result *result)
{
	struct zebra_probe *probe = arg;

	probe->add_attempts++;
	if (probe->fail_add_attempt == probe->add_attempts)
		return -EIO;
	assert(prefix && result && result->paths && result->path_count);
	probe->adds++;
	probe->last_prefix = *prefix;
	probe->last_path = result->paths[0];
	probe->last_instance = result->instance;
	probe->last_sid_count = result->explicit.sid_count;
	return 0;
}

static int probe_delete(void *arg, const struct prefix *prefix,
			uint8_t instance)
{
	struct zebra_probe *probe = arg;

	assert(prefix);
	probe->deletes++;
	probe->last_prefix = *prefix;
	probe->last_instance = instance;
	return 0;
}

static int probe_deferred(void *arg)
{
	struct zebra_probe *probe = arg;

	probe->deferred++;
	return 0;
}

static int probe_flush(void *arg)
{
	struct zebra_probe *probe = arg;

	probe->flushes++;
	return 0;
}

static void probe_abort(void *arg)
{
	struct zebra_probe *probe = arg;

	probe->aborts++;
}

static const struct midr_zebra_backend_ops probe_ops = {
	.route_add = probe_add,
	.route_del = probe_delete,
	.update_deferred = probe_deferred,
	.flush = probe_flush,
	.abort_pending = probe_abort,
};

static void ted_create(struct midr_context *ctx)
{
	struct midr_ted_config config = {
		.max_events = 16,
	};

	memset(ctx, 0, sizeof(*ctx));
	ctx->node_id = 1;
	assert(midr_ted_create(&config, &ctx->ted) == 0);
}

static struct midr_spf_route route_v4(uint8_t marker, uint64_t metric,
				      struct midr_spf_nexthop *nexthop)
{
	struct midr_spf_route route = {
		.family = MIDR_CORE_AF_IPV4,
		.prefix_len = 24,
		.metric = metric,
		.generation = 1,
		.scope = MIDR_SPF_ROUTE_INTRA_GROUP,
		.reachable = true,
		.nexthops = nexthop,
		.nexthop_count = 1,
	};

	route.prefix[0] = 10;
	route.prefix[2] = marker;
	return route;
}

static struct midr_spf_route route_v6(uint8_t marker, uint64_t metric,
				      struct midr_spf_nexthop *nexthop)
{
	struct midr_spf_route route = {
		.family = MIDR_CORE_AF_IPV6,
		.prefix_len = 64,
		.metric = metric,
		.generation = 1,
		.scope = MIDR_SPF_ROUTE_INTER_GROUP,
		.reachable = true,
		.nexthops = nexthop,
		.nexthop_count = 1,
	};

	route.prefix[0] = 0x20;
	route.prefix[1] = 0x01;
	route.prefix[2] = 0x0d;
	route.prefix[3] = 0xb8;
	route.prefix[7] = marker;
	return route;
}

static void test_result_diff_and_compatibility_payload(void)
{
	struct midr_context ctx;
	struct zebra_probe probe = {0};
	struct midr_spf_nexthop nexthops[3] = {
		{.family = MIDR_CORE_AF_IPV4, .address = {192, 0, 2, 1},
		 .ifindex = 11},
		{.family = MIDR_CORE_AF_IPV6, .address = {0x20, 1, 0x0d, 0xb8},
		 .ifindex = 12},
		{.family = MIDR_CORE_AF_IPV4, .address = {192, 0, 2, 3},
		 .ifindex = 13},
	};
	struct midr_spf_route old_routes[2] = {
		route_v4(1, 10, &nexthops[0]),
		route_v6(2, 30, &nexthops[1]),
	};
	struct midr_spf_route new_routes[2] = {
		route_v4(1, 20, &nexthops[0]),
		route_v4(3, 40, &nexthops[2]),
	};
	struct midr_spf_results old_results = {
		.references = 1,
		.generation = 1,
		.routes = old_routes,
		.count = 2,
	};
	struct midr_spf_results new_results = {
		.references = 1,
		.generation = 2,
		.routes = new_routes,
		.count = 2,
	};
	struct prefix te_prefix = {
		.family = AF_INET6,
		.prefixlen = 64,
	};
	struct midr_path te_path = {
		.ifindex = 22,
		.metric = 1,
		.path_avail_bw = 250.0f,
		.weight = 7,
	};
	struct midr_path_result te_result = {
		.paths = &te_path,
		.path_count = 1,
		.instance = MIDR_INSTANCE_TE,
		.explicit.sid_count = 1,
	};

	ted_create(&ctx);
	assert(midr_zebra_backend_register(&ctx, &probe_ops, &probe) == 0);
	assert(midr_spf_install_results(&ctx, &old_results, &new_results) == 0);
	assert(probe.adds == 2 && probe.deletes == 2 && probe.deferred == 1);
	assert(probe.last_instance == MIDR_INSTANCE_SPF);
	assert(probe.last_path.metric == 40 && probe.last_path.weight == 0 &&
	       probe.last_path.path_avail_bw == 0.0f);

	/* UCMP/SRv6 and dual-instance data pass through the compatibility API
	 * without being reduced to the base SPF route shape. */
	te_prefix.u.prefix6.s6_addr[0] = 0x20;
	SET_IPADDR_V6(&te_path.nexthop);
	te_path.nexthop.ipaddr_v6.s6_addr[0] = 0x20;
	te_result.explicit.sid_list[0].s6_addr[0] = 0xfc;
	assert(midr_zebra_route_add(&ctx, &te_prefix, &te_result) == 0);
	assert(probe.last_instance == MIDR_INSTANCE_TE &&
	       probe.last_path.weight == 7 && probe.last_sid_count == 1);
	te_result.explicit.sid_count = MIDR_SRV6_MAX_SEGS + 1;
	assert(midr_zebra_route_add(&ctx, &te_prefix, &te_result) == -E2BIG);
	te_result.explicit.sid_count = 1;

	probe.fail_add_attempt = probe.add_attempts + 2;
	assert(midr_spf_install_results(&ctx, NULL, &new_results) == -EIO);
	assert(probe.aborts == 1);
	assert(midr_zebra_backend_unregister(&ctx) == 0);
	midr_ted_destroy(&ctx.ted);
}

static void test_runtime_ready_and_not_ready(void)
{
	struct midr_context ctx;
	struct zebra_probe probe = {0};
	struct midr_consumer_event events[2] = {0};
	struct midr_consumer_snapshot snapshot = {
		.generation = 7,
		.count = 2,
		.events = events,
	};
	struct midr_spf_install_status status;
	uint64_t first_generation;

	ted_create(&ctx);
	events[0].kind = MIDR_CONSUMER_LINK;
	events[0].generation = 7;
	events[0].originator = 1;
	events[0].remote = 2;
	events[0].link_id = 1;
	events[0].family = MIDR_CORE_AF_IPV4;
	events[0].metric = 5;
	events[0].remote_address[0] = 192;
	events[0].remote_address[1] = 0;
	events[0].remote_address[2] = 2;
	events[0].remote_address[3] = 2;
	events[1].kind = MIDR_CONSUMER_NODE_PREFIX;
	events[1].generation = 7;
	events[1].originator = 2;
	events[1].family = MIDR_CORE_AF_IPV4;
	events[1].prefix_len = 24;
	events[1].prefix[0] = 198;
	events[1].prefix[1] = 51;
	events[1].prefix[2] = 100;
	events[1].metric = 3;
	assert(midr_ted_apply_snapshot(ctx.ted, &snapshot) == 0);
	assert(midr_zebra_backend_register(&ctx, &probe_ops, &probe) == 0);
	assert(probe.adds == 1 && probe.deferred == 1);
	assert(midr_spf_install_status_get(&ctx, &status) == 0);
	assert(status.active && status.desired_generation != 0 &&
	       status.reconcile_count == 1);
	first_generation = status.desired_generation;
	assert(midr_spf_install_resync(&ctx) == 0);
	assert(probe.adds == 1 && probe.deferred == 1);
	assert(midr_spf_install_replay(&ctx) == 0);
	assert(probe.adds == 2 && probe.deferred == 2);
	assert(midr_spf_install_status_get(&ctx, &status) == 0);
	assert(status.desired_generation == first_generation &&
	       status.reconcile_count == 2);

	/* A failed replacement is aborted as one staged batch and does not
	 * advance the adapter's desired generation. */
	for (size_t i = 0; i < snapshot.count; i++)
		events[i].generation = 8;
	snapshot.generation = 8;
	events[0].metric = 6;
	probe.fail_add_attempt = probe.add_attempts + 1;
	assert(midr_ted_apply_snapshot(ctx.ted, &snapshot) == 0);
	assert(midr_spf_install_status_get(&ctx, &status) == 0);
	assert(status.desired_generation == first_generation &&
	       status.last_error == -EIO && probe.aborts == 1);
	probe.fail_add_attempt = 0;
	assert(midr_spf_install_resync(&ctx) == 0);
	assert(midr_spf_install_status_get(&ctx, &status) == 0);
	assert(status.desired_generation != first_generation &&
	       status.reconcile_count == 3 && status.last_error == 0);

	assert(midr_ted_invalidate(ctx.ted, -EIO) == 0);
	assert(probe.deletes == 3 && probe.deferred == 4);
	assert(midr_spf_install_status_get(&ctx, &status) == 0);
	assert(status.desired_generation == 0 && status.withdraw_count == 1 &&
	       status.last_error == -EIO);
	assert(midr_zebra_backend_unregister(&ctx) == 0);
	assert(probe.flushes == 1);
	midr_ted_destroy(&ctx.ted);
}

int main(void)
{
	test_result_diff_and_compatibility_payload();
	test_runtime_ready_and_not_ready();
	puts("midrd-spf-install-test: PASS");
	return 0;
}
