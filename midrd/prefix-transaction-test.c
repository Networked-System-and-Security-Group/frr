#define main midrd_program_main
#include "midrd.c"
#undef main

#include <assert.h>

static size_t consumer_snapshots;

static void test_lifetime_accounting(void)
{
	struct midr_context daemon = {.lifetime_ms = 10000};
	struct midr_core_object object = {.lifetime_ms = 8000};
	struct midr_core_object before;

	assert(age_object_lifetime(&daemon, &object, 1000000000U,
				   1000000001U, 0) == 0);
	assert(object.lifetime_ms == 7999);

	object.lifetime_ms = 8000;
	assert(age_object_lifetime(&daemon, &object, 1000000001U,
				   1000000000U, 0) == -ERANGE);
	assert(object.lifetime_ms == 8000);

	assert(age_object_lifetime(&daemon, &object, 1000000000U,
				   1000000000U, 1000) == 0);
	assert(object.lifetime_ms == 7000);

	object.lifetime_ms = 1000;
	before = object;
	assert(age_object_lifetime(&daemon, &object, 1000000000U,
				   1000000000U, 1000) == -ESTALE);
	assert(!memcmp(&object, &before, sizeof(object)));

	/* Each hop starts from the remaining lifetime encoded by the previous
	 * hop, then adds its own receive wait and forwarding budget. */
	object.lifetime_ms = 10000;
	assert(age_object_lifetime(&daemon, &object, 1000000000U,
				   1000500000U, 1000) == 0);
	assert(object.lifetime_ms == 8999);
	assert(age_object_lifetime(&daemon, &object, 2000000000U,
				   2001500000U, 1000) == 0);
	assert(object.lifetime_ms == 7997);

	before = object;
	assert(age_object_lifetime(&daemon, &object, 3000000001U,
				   3000000000U, 0) == -ERANGE);
	assert(!memcmp(&object, &before, sizeof(object)));
}

static void record_consumer_event(void *arg,
				  const struct midr_consumer_event *event)
{
	struct midr_context *daemon = arg;

	assert(event);
	on_consumer_event(daemon, event);
	if (event->kind == MIDR_CONSUMER_SNAPSHOT_BEGIN)
		consumer_snapshots++;
}

static struct midr_prefix_event control_event(enum midr_prefix_event_kind kind,
					       uint64_t generation,
					       uint32_t originator)
{
	struct midr_prefix_event event = {
		.kind = kind,
		.generation = generation,
		.originator = originator,
	};

	return event;
}

static struct midr_prefix_event prefix_event_value(
	enum midr_prefix_event_kind kind, uint64_t generation,
	uint32_t originator, uint8_t family, uint8_t suffix, uint32_t metric)
{
	struct midr_prefix_event event = control_event(kind, generation,
						      originator);

	event.prefix.family = family;
	event.prefix.prefix_len = family == MIDR_CORE_AF_IPV4 ? 32U : 128U;
	if (family == MIDR_CORE_AF_IPV4) {
		event.prefix.address[0] = 192;
		event.prefix.address[1] = 0;
		event.prefix.address[2] = 2;
		event.prefix.address[3] = suffix;
	} else {
		event.prefix.address[0] = 0x20;
		event.prefix.address[1] = 0x01;
		event.prefix.address[2] = 0x0d;
		event.prefix.address[3] = 0xb8;
		event.prefix.address[15] = suffix;
	}
	event.prefix.metric = metric;
	return event;
}

static void initialize_daemon(struct midr_context *daemon, size_t capacity)
{
	struct midr_engine_config engine_config = {
		.node_id = 77,
		.max_objects = capacity + 1U,
		.lifetime_ms = 60000,
	};
	struct midr_owned_config owned_config = {
		.originator = 77,
		.max_objects = capacity,
		.lifetime_ms = 60000,
	};
	struct midr_ted_config ted_config = {
		.max_events = capacity + 1U,
	};
	struct midr_consumer_config consumer_config = {
		.on_event = record_consumer_event,
		.arg = daemon,
	};
	struct midr_core_object membership = {
		.identity = {
			.type = MIDR_CORE_MEMBERSHIP,
			.originator = 77,
		},
		.state = MIDR_CORE_ACTIVE,
		.sequence = 1,
		.lifetime_ms = 60000,
		.group = 1,
	};
	enum midr_core_result result;

	memset(daemon, 0, sizeof(*daemon));
	daemon->node_id = 77;
	daemon->lifetime_ms = 60000;
	consumer_snapshots = 0;
	assert(midr_engine_create(&engine_config, &daemon->engine) == 0);
	assert(midr_ted_create(&ted_config, &daemon->ted) == 0);
	assert(midr_consumer_create(&consumer_config, &daemon->consumer) == 0);
	assert(midr_engine_attach_ted(daemon->engine, daemon->ted) == 0);
	assert(midr_engine_attach_consumer(daemon->engine, daemon->consumer) == 0);
	assert(midr_engine_apply(daemon->engine, &membership, mono_ms(), &result) ==
	       0);
	assert(result == MIDR_CORE_ACCEPTED);
	daemon->representative_group = 1;
	daemon->representative_node = 77;
	daemon->representative_committed = true;
	assert(midr_owned_create(&owned_config, publish_owned, daemon,
				 &daemon->owned) == 0);
}

static void destroy_daemon(struct midr_context *daemon)
{
	midr_owned_destroy(&daemon->owned);
	midr_ted_destroy(&daemon->ted);
	midr_consumer_destroy(&daemon->consumer);
	midr_engine_destroy(&daemon->engine);
}

static int deliver(struct midr_context *daemon, struct midr_prefix_event event)
{
	return prefix_event(daemon, &event);
}

static bool owned_contains(struct midr_context *daemon,
			   const struct midr_prefix_event *event,
			   uint32_t metric)
{
	struct midr_core_identity identity;
	struct midr_core_object object;

	prefix_identity(daemon, &event->prefix, &identity);
	return midr_owned_lookup(daemon->owned, &identity, &object) == 0 &&
	       object.state == MIDR_CORE_ACTIVE && object.metric == metric;
}

static uint64_t consumer_generation(struct midr_context *daemon, size_t *count)
{
	struct midr_consumer_snapshot snapshot = {0};
	uint64_t generation;

	assert(midr_consumer_snapshot_acquire(daemon->consumer, &snapshot) == 0);
	generation = snapshot.generation;
	if (count)
		*count = snapshot.count;
	midr_consumer_snapshot_release(&snapshot);
	return generation;
}

int main(void)
{
	struct midr_context daemon;
	struct midr_prefix_event ipv4 = prefix_event_value(
		MIDR_PREFIX_UPSERT, 1, 77, MIDR_CORE_AF_IPV4, 1, 10);
	struct midr_prefix_event ipv6 = prefix_event_value(
		MIDR_PREFIX_UPSERT, 1, 77, MIDR_CORE_AF_IPV6, 1, 20);
	uint64_t engine_generation, view_generation, sequence;
	size_t snapshots_before, view_count, withdrawn = 0;
	struct midr_core_object pending;

	test_lifetime_accounting();
	initialize_daemon(&daemon, 4);
	assert(deliver(&daemon, control_event(MIDR_PREFIX_SNAPSHOT_BEGIN, 1, 88)) ==
	       -EINVAL);
	assert(deliver(&daemon, control_event(MIDR_PREFIX_SNAPSHOT_BEGIN, 1, 77)) ==
	       0);
	assert(deliver(&daemon, ipv4) == 0);
	assert(deliver(&daemon, ipv6) == 0);
	assert(deliver(&daemon, control_event(MIDR_PREFIX_SNAPSHOT_END, 1, 77)) ==
	       0);
	snapshots_before = consumer_snapshots;
	assert(deliver(&daemon, control_event(MIDR_PREFIX_EOR, 1, 77)) == 0);
	assert(consumer_snapshots == snapshots_before + 1U);
	assert(daemon.prefix_generation == 1 && daemon.ipc_prefix_count == 2);
	assert(daemon.group_prefix_count == 2 && midr_owned_count(daemon.owned) == 4);
	assert(owned_contains(&daemon, &ipv4, 10));
	assert(owned_contains(&daemon, &ipv6, 20));
	view_generation = consumer_generation(&daemon, &view_count);
	assert(view_count == 4);

	ipv4.generation = 2;
	ipv4.prefix.metric = 11;
	assert(deliver(&daemon, control_event(MIDR_PREFIX_SNAPSHOT_BEGIN, 2, 77)) ==
	       0);
	assert(deliver(&daemon, ipv4) == 0);
	assert(deliver(&daemon, control_event(MIDR_PREFIX_SNAPSHOT_END, 2, 77)) ==
	       0);
	{
		int ret = deliver(&daemon,
				  control_event(MIDR_PREFIX_EOR, 2, 77));

		if (ret)
			fprintf(stderr, "generation 2 commit failed: %d\n", ret);
		assert(ret == 0);
	}
	assert(daemon.prefix_generation == 2 && daemon.ipc_prefix_count == 1);
	assert(daemon.group_prefix_count == 1 && midr_owned_count(daemon.owned) == 2);
	assert(owned_contains(&daemon, &ipv4, 11));
	assert(!owned_contains(&daemon, &ipv6, 20));
	assert(consumer_generation(&daemon, &view_count) > view_generation);
	assert(view_count == 2);

	assert(deliver(&daemon, control_event(MIDR_PREFIX_SNAPSHOT_BEGIN, 1, 77)) ==
	       0);
	ipv6.generation = 1;
	ipv6.prefix.metric = 21;
	assert(deliver(&daemon, ipv6) == 0);
	assert(deliver(&daemon, control_event(MIDR_PREFIX_SNAPSHOT_END, 1, 77)) ==
	       0);
	assert(deliver(&daemon, control_event(MIDR_PREFIX_EOR, 1, 77)) == 0);
	assert(deliver(&daemon, control_event(MIDR_PREFIX_EOR, 2, 77)) == 0);
	ipv6.generation = 2;
	assert(deliver(&daemon, ipv6) == 0);
	assert(daemon.prefix_generation == 2 && daemon.ipc_prefix_count == 1);
	assert(owned_contains(&daemon, &ipv4, 11));
	assert(!owned_contains(&daemon, &ipv6, 21));

	ipv6.generation = 3;
	ipv6.prefix.metric = 21;
	assert(deliver(&daemon, control_event(MIDR_PREFIX_SNAPSHOT_BEGIN, 3, 77)) ==
	       0);
	assert(deliver(&daemon, ipv6) == 0);
	{
		struct midr_prefix_event invalid = ipv6;

		invalid.prefix.family = MIDR_CORE_AF_IPV4;
		invalid.prefix.prefix_len = 33;
		assert(deliver(&daemon, invalid) == -EINVAL);
	}
	prefix_ipc_disconnect(&daemon, 0);
	assert(!daemon.prefix_stage.active && daemon.prefix_generation == 0);
	assert(daemon.group_prefix_count == 1 && midr_owned_count(daemon.owned) == 2);
	assert(daemon.ipc_prefix_count == 1 && owned_contains(&daemon, &ipv4, 11));
	assert(!owned_contains(&daemon, &ipv6, 21));

	assert(deliver(&daemon, control_event(MIDR_PREFIX_SNAPSHOT_BEGIN, 1, 77)) ==
	       0);
	ipv4.generation = 1;
	ipv4.prefix.metric = 12;
	ipv6.generation = 1;
	ipv6.prefix.metric = 22;
	assert(deliver(&daemon, ipv4) == 0);
	assert(deliver(&daemon, ipv6) == 0);
	assert(deliver(&daemon, control_event(MIDR_PREFIX_SNAPSHOT_END, 1, 77)) ==
	       0);
	assert(deliver(&daemon, control_event(MIDR_PREFIX_EOR, 1, 77)) == 0);
	assert(daemon.prefix_generation == 1 && daemon.ipc_prefix_count == 2);
	assert(daemon.group_prefix_count == 2 && midr_owned_count(daemon.owned) == 4);
	assert(owned_contains(&daemon, &ipv4, 12));
	assert(owned_contains(&daemon, &ipv6, 22));
	assert(consumer_generation(&daemon, &view_count) > view_generation);
	assert(view_count == 4);

	engine_generation = midr_engine_generation(daemon.engine);
	view_generation = consumer_generation(&daemon, &view_count);
	sequence = midr_owned_last_sequence(daemon.owned);
	snapshots_before = consumer_snapshots;
	assert(deliver(&daemon, control_event(MIDR_PREFIX_SNAPSHOT_BEGIN, 2, 77)) ==
	       0);
	for (uint8_t suffix = 3; suffix <= 5; suffix++)
		assert(deliver(&daemon, prefix_event_value(
			MIDR_PREFIX_UPSERT, 2, 77, MIDR_CORE_AF_IPV4, suffix,
			30U + suffix)) == 0);
	assert(deliver(&daemon, control_event(MIDR_PREFIX_SNAPSHOT_END, 2, 77)) ==
	       0);
	assert(deliver(&daemon, control_event(MIDR_PREFIX_EOR, 2, 77)) == -ENOSPC);
	assert(!daemon.prefix_stage.active && daemon.prefix_generation == 1);
	assert(consumer_snapshots == snapshots_before);
	assert(daemon.ipc_prefix_count == 2);
	assert(daemon.group_prefix_count == 2 && midr_owned_count(daemon.owned) == 4);
	assert(owned_contains(&daemon, &ipv4, 12));
	assert(owned_contains(&daemon, &ipv6, 22));
	assert(midr_engine_generation(daemon.engine) == engine_generation);
	assert(consumer_generation(&daemon, &view_count) == view_generation);
	assert(view_count == 4);
	assert(midr_owned_last_sequence(daemon.owned) > sequence);
	assert(midr_engine_event_next(daemon.engine, &pending) != 0);

	sequence = midr_owned_last_sequence(daemon.owned);
	refresh_owned(&daemon);
	assert(midr_owned_last_sequence(daemon.owned) == sequence + 4U);
	assert(owned_contains(&daemon, &ipv4, 12));
	assert(owned_contains(&daemon, &ipv6, 22));
	assert(midr_owned_withdraw_all(daemon.owned, &withdrawn) == 0);
	assert(withdrawn == 4 && midr_owned_count(daemon.owned) == 0);
	(void)consumer_generation(&daemon, &view_count);
	assert(view_count == 0);
	destroy_daemon(&daemon);
	puts("midrd-prefix-transaction-test: PASS");
	return 0;
}
