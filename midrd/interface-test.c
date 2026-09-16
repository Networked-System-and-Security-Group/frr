#define main midrd_program_main
#include "midrd.c"
#undef main

#include <assert.h>

static struct midr_topology_snapshot provider_snapshot;
static size_t provider_gets;
static size_t provider_releases;
static int provider_error;

struct spf_probe {
	struct midr_context *ctx;
	struct midr_spf_consumer **consumer;
	size_t changes;
	uint64_t generation;
	enum midr_ted_state state;
	int error;
	const struct midr_link_update *reentrant_link;
	int reentrant_result;
	bool unregister_in_callback;
};

static int snapshot_get(struct midr_context *ctx,
			struct midr_topology_snapshot *snapshot)
{
	assert(ctx);
	assert(snapshot);
	provider_gets++;
	if (provider_error)
		return provider_error;
	*snapshot = provider_snapshot;
	return 0;
}

static void snapshot_release(struct midr_context *ctx,
			     struct midr_topology_snapshot *snapshot)
{
	assert(ctx);
	assert(snapshot);
	provider_releases++;
	memset(snapshot, 0, sizeof(*snapshot));
}

static void results_changed(struct midr_context *ctx, uint64_t generation,
			    uint32_t change_flags,
			    enum midr_ted_state state, int error, void *arg)
{
	struct spf_probe *probe = arg;

	assert(ctx == probe->ctx);
	assert(change_flags != MIDR_TED_CHANGE_NONE);
	probe->changes++;
	probe->generation = generation;
	probe->state = state;
	probe->error = error;
	if (probe->reentrant_link) {
		probe->reentrant_result =
			midr_topology_link_upsert(ctx, probe->reentrant_link);
		probe->reentrant_link = NULL;
	}
	if (probe->unregister_in_callback)
		midr_spf_consumer_unregister(ctx, probe->consumer);
}

static void initialize_context(struct midr_context *ctx)
{
	struct midr_engine_config engine_config = {
		.node_id = 77,
		.max_objects = 64,
		.lifetime_ms = 600000,
	};
	struct midr_owned_config owned_config = {
		.originator = 77,
		.max_objects = 64,
		.lifetime_ms = 600000,
	};
	struct midr_ted_config ted_config = {
		.max_events = 64,
		.local_ifindex_lookup = local_ifindex_lookup,
		.local_ifindex_arg = ctx,
	};
	struct midr_consumer_config consumer_config = {
		.on_event = on_consumer_event,
		.arg = ctx,
	};

	memset(ctx, 0, sizeof(*ctx));
	ctx->node_id = 77;
	ctx->lifetime_ms = 600000;
	assert(midr_engine_create(&engine_config, &ctx->engine) == 0);
	assert(midr_ted_create(&ted_config, &ctx->ted) == 0);
	assert(midr_consumer_create(&consumer_config, &ctx->consumer) == 0);
	assert(midr_engine_attach_ted(ctx->engine, ctx->ted) == 0);
	assert(midr_engine_attach_consumer(ctx->engine, ctx->consumer) == 0);
	assert(midr_owned_create(&owned_config, publish_owned, ctx,
				 &ctx->owned) == 0);
}

static struct midr_link_update link_update(uint64_t version)
{
	struct midr_link_update link = {
		.key = {
			.local_node_id = 77,
			.remote_node_id = 88,
			.link_id = 9,
		},
		.local_ifindex = 12,
		.metrics = {
			.has_rtt_us = true,
			.rtt_us = 1000,
			.has_loss_ppm = true,
			.loss_ppm = 100,
			.has_available_bandwidth_kbps = true,
			.available_bandwidth_kbps = 1000000,
			.measurement_seqno = version,
			.measurement_timestamp_ms = version * 10U,
		},
		.policy_state = MIDR_POLICY_ALLOWED,
		.version = version,
	};

	SET_IPADDR_V4(&link.link_local_address);
	SET_IPADDR_V4(&link.link_remote_address);
	assert(inet_pton(AF_INET, "192.0.2.77",
			 &link.link_local_address.ipaddr_v4) == 1);
	assert(inet_pton(AF_INET, "192.0.2.88",
			 &link.link_remote_address.ipaddr_v4) == 1);
	return link;
}

static struct midr_link_update link_update_v6(uint64_t version)
{
	struct midr_link_update link = link_update(version);

	link.key.link_id = 10;
	SET_IPADDR_V6(&link.link_local_address);
	SET_IPADDR_V6(&link.link_remote_address);
	assert(inet_pton(AF_INET6, "2001:db8:77::1",
			 &link.link_local_address.ipaddr_v6) == 1);
	assert(inet_pton(AF_INET6, "2001:db8:88::1",
			 &link.link_remote_address.ipaddr_v6) == 1);
	return link;
}

static void publish_prefix(struct midr_context *ctx)
{
	struct midr_prefix_event events[] = {
		{.kind = MIDR_PREFIX_SNAPSHOT_BEGIN, .generation = 1,
		 .originator = 77},
		{.kind = MIDR_PREFIX_UPSERT, .generation = 1,
		 .originator = 77,
		 .prefix = {.family = MIDR_CORE_AF_IPV4, .prefix_len = 24,
			    .address = {198, 51, 100}, .metric = 5}},
		{.kind = MIDR_PREFIX_UPSERT, .generation = 1,
		 .originator = 77,
		 .prefix = {.family = MIDR_CORE_AF_IPV6, .prefix_len = 64,
			    .address = {0x20, 0x01, 0x0d, 0xb8, 0, 77},
			    .metric = 6}},
		{.kind = MIDR_PREFIX_SNAPSHOT_END, .generation = 1,
		 .originator = 77},
		{.kind = MIDR_PREFIX_EOR, .generation = 1, .originator = 77},
	};

	for (size_t i = 0; i < array_size(events); i++)
		assert(prefix_event(ctx, &events[i]) == 0);
}

int main(void)
{
	static const struct midr_spf_consumer_ops spf_ops = {
		.results_changed = results_changed,
	};
	struct midr_context ctx;
	struct midr_spf_consumer *consumer = NULL;
	struct spf_probe probe = {
		.ctx = &ctx,
		.consumer = &consumer,
	};
	struct midr_node_update node = {
		.node_id = 77,
		.group_id = 9,
		.policy_state = MIDR_POLICY_ALLOWED,
		.version = 1,
	};
	struct midr_link_update link = link_update(1);
	struct midr_link_update link6 = link_update_v6(1);
	const struct midr_spf_results *results = NULL;
	const struct midr_spf_results *held;
	struct midr_ted_prefix_key key = {
		.family = MIDR_CORE_AF_IPV4,
		.prefix_len = 24,
		.prefix = {198, 51, 100},
	};
	struct midr_ted_prefix_key key6 = {
		.family = MIDR_CORE_AF_IPV6,
		.prefix_len = 64,
		.prefix = {0x20, 0x01, 0x0d, 0xb8, 0, 77},
	};

	initialize_context(&ctx);
	assert(midr_context_get_default() == NULL);
	midrd_runtime = &ctx;
	assert(midr_context_get_default() == &ctx);
	assert(midr_spf_results_get(&ctx, &results) == 0);
	assert(midr_spf_results_count(results) == 0);
	midr_spf_results_release(&results);
	probe.reentrant_link = &link;
	probe.unregister_in_callback = true;
	assert(midr_spf_consumer_register(&ctx, &spf_ops, &probe,
					  &consumer) == 0);
	assert(midr_topology_node_upsert(&ctx, &node) == 0);
	assert(consumer == NULL && probe.changes == 1 &&
	       probe.state == MIDR_TED_READY && probe.error == 0);
	assert(probe.reentrant_result == -EBUSY && ctx.link_count == 0);

	probe.unregister_in_callback = false;
	assert(midr_spf_consumer_register(&ctx, &spf_ops, &probe,
					  &consumer) == 0);
	assert(midr_topology_link_upsert(&ctx, &link) == 0);
	assert(midr_topology_link_upsert(&ctx, &link6) == 0);
	assert(ctx.link_count == 2);
	publish_prefix(&ctx);
	assert(probe.changes >= 2 && probe.state == MIDR_TED_READY);
	assert(midr_spf_results_get(&ctx, &results) == 0);
	assert(midr_spf_results_count(results) == 2);
	assert(midr_spf_results_generation(results) == probe.generation);
	assert(midr_spf_results_lookup(results, &key));
	assert(midr_spf_results_lookup(results, &key6));
	assert(midr_spf_results_at(results, 0)->scope == MIDR_SPF_ROUTE_LOCAL);
	held = midr_spf_results_acquire(results);
	assert(held == results);
	midr_spf_results_release(&results);
	assert(midr_spf_results_count(held) == 2);
	midr_spf_results_release(&held);

	assert(midr_topology_provider_register(&ctx, snapshot_get,
					       snapshot_release) == 0);
	node.group_id = 10;
	provider_snapshot.nodes = &node;
	provider_snapshot.node_count = 1;
	provider_snapshot.snapshot_version = 1;
	assert(midr_topology_resync_begin(
		       &ctx, MIDR_TOPOLOGY_RESYNC_PROVIDER_RESTART) == 0);
	assert(provider_gets == 1 && provider_releases == 1);
	assert(ctx.group_id == 10 && ctx.link_count == 0);

	provider_error = -EAGAIN;
	assert(midr_topology_resync_begin(
		       &ctx, MIDR_TOPOLOGY_RESYNC_PROVIDER_RESTART) == -EAGAIN);
	assert(provider_gets == 2 && provider_releases == 1);
	assert(ctx.group_id == 10 && ctx.link_count == 0);
	node.group_id = 11;
	assert(midr_topology_node_upsert(&ctx, &node) == -EAGAIN);
	provider_error = 0;
	node.group_id = 0;
	provider_snapshot.snapshot_version = 2;
	assert(midr_topology_resync_begin(
		       &ctx, MIDR_TOPOLOGY_RESYNC_STATE_INCONSISTENT) == -EINVAL);
	assert(ctx.group_id == 10 && ctx.link_count == 0);
	node.group_id = 11;
	assert(midr_topology_node_upsert(&ctx, &node) == -EAGAIN);
	assert(midr_topology_resync_begin(
		       &ctx, MIDR_TOPOLOGY_RESYNC_STATE_INCONSISTENT) == 0);
	assert(ctx.group_id == 11 && provider_gets == 4 &&
	       provider_releases == 3);

	midr_topology_provider_unregister(&ctx);
	midr_spf_consumer_unregister(&ctx, &consumer);
	assert(consumer == NULL);
	midrd_runtime = NULL;
	midr_context_finish(&ctx);
	puts("midrd-interface-test: PASS");
	return 0;
}
