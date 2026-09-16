#define main midrd_program_main
#include "midrd.c"
#undef main

#include <assert.h>

static struct midr_core_object membership(uint32_t originator,
						 uint32_t group, uint64_t sequence)
{
	struct midr_core_object object = {0};

	object.identity.type = MIDR_CORE_MEMBERSHIP;
	object.identity.originator = originator;
	object.state = MIDR_CORE_ACTIVE;
	object.sequence = sequence;
	object.lifetime_ms = 600000;
	object.group = group;
	return object;
}

static struct midr_core_object prefix(uint32_t originator, uint64_t sequence,
					     uint32_t metric)
{
	struct midr_core_object object = {0};

	object.identity.type = MIDR_CORE_NODE_PREFIX;
	object.identity.family = MIDR_CORE_AF_IPV4;
	object.identity.prefix_len = 24;
	object.identity.originator = originator;
	object.identity.prefix[0] = 192;
	object.identity.prefix[1] = 0;
	object.identity.prefix[2] = 2;
	object.state = MIDR_CORE_ACTIVE;
	object.sequence = sequence;
	object.lifetime_ms = 600000;
	object.metric = metric;
	return object;
}

static void setup(struct midrd *daemon)
{
	struct midr_engine_config engine_config = {
		.node_id = 1,
		.max_objects = 16,
		.lifetime_ms = 600000,
	};
	struct midr_ted_config ted_config = {.max_events = 16};
	struct midr_consumer_config consumer_config = {
		.on_event = on_consumer_event,
		.arg = daemon,
	};

	memset(daemon, 0, sizeof(*daemon));
	daemon->node_id = 1;
	daemon->lifetime_ms = 600000;
	assert(midr_engine_create(&engine_config, &daemon->engine) == 0);
	assert(midr_ted_create(&ted_config, &daemon->ted) == 0);
	assert(midr_consumer_create(&consumer_config, &daemon->consumer) == 0);
	assert(midr_engine_attach_ted(daemon->engine, daemon->ted) == 0);
	assert(midr_engine_attach_consumer(daemon->engine,
					   daemon->consumer) == 0);
}

static void teardown(struct midrd *daemon)
{
	midr_ted_destroy(&daemon->ted);
	midr_consumer_destroy(&daemon->consumer);
	midr_engine_destroy(&daemon->engine);
}

static void fill_consumer_queue(struct midr_consumer *consumer)
{
	const struct midr_consumer_event event = {
		.kind = MIDR_CONSUMER_NODE_PREFIX,
		.generation = 1,
		.originator = 999,
		.family = MIDR_CORE_AF_IPV4,
		.prefix_len = 32,
	};
	int ret;

	while ((ret = midr_consumer_publish(consumer, &event)) == 0)
		;
	assert(ret == -ENOSPC);
}

static void drain_consumer_queue(struct midr_consumer *consumer)
{
	struct midr_consumer_event event;

	while (midr_consumer_event_next(consumer, &event) == 0)
		;
}

static int frame_generation(struct midrd *daemon,
			    const struct midr_transport_endpoint *peer,
			    uint8_t type, uint64_t generation, uint64_t sequence,
			    uint64_t received_ns, const uint8_t *payload,
			    size_t payload_len)
{
	struct midr_transport_frame input = {
		.version = MIDR_WIRE_VERSION,
		.type = type,
		.sequence = sequence,
		.generation = generation,
		.received_ns = received_ns,
		.payload = payload,
		.payload_len = payload_len,
	};

	return on_frame(daemon, peer, &input);
}

static int frame(struct midrd *daemon, const struct midr_transport_endpoint *peer,
		 uint8_t type, uint64_t sequence, uint64_t received_ns,
		 const uint8_t *payload, size_t payload_len)
{
	return frame_generation(daemon, peer, type, 1, sequence, received_ns,
				payload, payload_len);
}

static void test_snapshot_barrier_and_atomic_eor(void)
{
	struct midrd daemon;
	struct midr_transport_endpoint peer = {
		.family = MIDR_TRANSPORT_AF_IPV4,
		.address = {127, 0, 0, 1},
		.port = 39001,
	};
	struct midr_core_object local = membership(1, 7, 1);
	struct midr_core_object remote_membership = membership(2, 7, 1);
	struct midr_core_object remote_prefix = prefix(2, 1, 10);
	struct midr_core_object newer_prefix = prefix(2, 2, 20);
	struct midr_core_object failed_prefix = prefix(2, 3, 30);
	uint8_t payload[MIDR_WIRE_OBJECT_LEN];
	uint64_t now = mono_ns();
	uint64_t ted_generation;
	struct midr_core_object current;
	struct midr_consumer_snapshot snapshot = {0};
	uint64_t snapshot_ns;
	size_t length, count;
	enum midr_core_result result;

	setup(&daemon);
	assert(midr_engine_apply(daemon.engine, &local, mono_ms(), &result) == 0);
	assert(midr_ted_state(daemon.ted) == MIDR_TED_READY);
	ted_generation = midr_ted_generation(daemon.ted);
	assert(frame(&daemon, &peer, MIDR_WIRE_SNAPSHOT_BEGIN, 1, now,
			     NULL, 0) == 0);
	assert(midr_wire_encode_object(&remote_membership, payload,
					       sizeof(payload), &length) == 0);
	assert(frame(&daemon, &peer, MIDR_WIRE_SNAPSHOT_OBJECT, 2, now,
			     payload, length) == 0);
	assert(midr_wire_encode_object(&remote_prefix, payload, sizeof(payload),
					       &length) == 0);
	assert(frame(&daemon, &peer, MIDR_WIRE_SNAPSHOT_OBJECT, 3, now,
			     payload, length) == 0);
	/* UPDATE is queued behind the snapshot and must not change the old view. */
	assert(midr_wire_encode_object(&newer_prefix, payload, sizeof(payload),
					       &length) == 0);
	assert(frame(&daemon, &peer, MIDR_WIRE_UPDATE, 4, now, payload,
			     length) == 0);
	assert(midr_ted_generation(daemon.ted) == ted_generation);
	assert(midr_engine_lookup(daemon.engine, &remote_prefix.identity, 1,
					  &current, NULL) == -ENOENT);
	assert(frame(&daemon, &peer, MIDR_WIRE_SNAPSHOT_END, 5, now, NULL, 0) ==
	       0);
	assert(frame(&daemon, &peer, MIDR_WIRE_EOR, 6, now, NULL, 0) == 0);
	assert(midr_engine_lookup(daemon.engine, &remote_prefix.identity,
					  mono_ms(),
					  &current, NULL) == 0);
	assert(current.sequence == 2 && current.metric == 20);
	assert(midr_ted_state(daemon.ted) == MIDR_TED_READY);
	assert(midr_ted_generation(daemon.ted) > ted_generation);

	/* A downstream derive failure does not reject the committed canonical
	 * batch or tear down the transport; the old TED view is retried later. */
	assert(midr_ted_test_fail_next(daemon.ted, -ENOMEM) == 0);
	assert(frame(&daemon, &peer, MIDR_WIRE_SNAPSHOT_BEGIN, 8, now, NULL, 0) ==
	       0);
	assert(midr_wire_encode_object(&failed_prefix, payload, sizeof(payload),
					       &length) == 0);
	assert(frame(&daemon, &peer, MIDR_WIRE_SNAPSHOT_OBJECT, 9, now, payload,
			     length) == 0);
	assert(frame(&daemon, &peer, MIDR_WIRE_SNAPSHOT_END, 10, now, NULL, 0) ==
	       0);
	assert(frame(&daemon, &peer, MIDR_WIRE_EOR, 11, now, NULL, 0) == 0);
	assert(!daemon.sync_derivation_wait);
	assert(midr_ted_state(daemon.ted) == MIDR_TED_NOT_READY);
	assert(midr_engine_lookup(daemon.engine, &failed_prefix.identity, mono_ms(),
					  &current, NULL) == 0);
	assert(current.sequence == 3 && current.metric == 30);
	assert(rebuild_ted(&daemon) == 0);
	assert(midr_ted_state(daemon.ted) == MIDR_TED_READY);

	/* A second EOR has no stage to commit and is harmless. */
	assert(frame(&daemon, &peer, MIDR_WIRE_EOR, 12, now, NULL, 0) == 0);
	/* EOR before SNAPSHOT_END rejects the transaction without applying it. */
	snapshot_ns = mono_ns();
	assert(frame(&daemon, &peer, MIDR_WIRE_SNAPSHOT_BEGIN, 13, snapshot_ns,
			     NULL, 0) == 0);
	assert(frame(&daemon, &peer, MIDR_WIRE_EOR, 14, snapshot_ns, NULL, 0) ==
	       -EPROTO);
	on_closed(&daemon, &peer, -ECONNRESET);
	assert(stage_for(&daemon, &peer, false) == NULL);
	assert(midr_consumer_snapshot_acquire(daemon.consumer, &snapshot) == 0);
	count = snapshot.count;
	midr_consumer_snapshot_release(&snapshot);
	assert(count == 1);
	teardown(&daemon);
}

static void test_reconnect_generation_isolation(void)
{
	struct midrd daemon;
	struct midr_transport_endpoint peer = {
		.family = MIDR_TRANSPORT_AF_IPV4,
		.address = {127, 0, 0, 1},
		.port = 39002,
	};
	struct midr_core_object remote = prefix(2, 1, 10);
	struct midr_core_object current;
	uint8_t payload[MIDR_WIRE_OBJECT_LEN];
	uint64_t now = mono_ns();
	size_t length;

	setup(&daemon);
	assert(frame_generation(&daemon, &peer, MIDR_WIRE_SNAPSHOT_BEGIN, 10,
				1, now, NULL, 0) == 0);
	assert(midr_wire_encode_object(&remote, payload, sizeof(payload),
					       &length) == 0);
	assert(frame_generation(&daemon, &peer, MIDR_WIRE_SNAPSHOT_OBJECT, 10,
				2, now, payload, length) == 0);
	/* Disconnecting an incomplete snapshot discards only staging; the old
	 * canonical view remains untouched. */
	on_closed(&daemon, &peer, -ECONNRESET);
	assert(stage_for(&daemon, &peer, false) == NULL);
	assert(midr_engine_lookup(daemon.engine, &remote.identity, mono_ms(),
					 &current, NULL) == -ENOENT);

	/* A new connection generation starts a fresh transaction.  A delayed
	 * frame from the old stream is rejected without mutating the new stage. */
	assert(frame_generation(&daemon, &peer, MIDR_WIRE_SNAPSHOT_BEGIN, 11,
				3, now, NULL, 0) == 0);
	assert(frame_generation(&daemon, &peer, MIDR_WIRE_SNAPSHOT_OBJECT, 10,
				4, now, payload, length) == -EPROTO);
	assert(stage_for(&daemon, &peer, false)->count == 0);
	assert(frame_generation(&daemon, &peer, MIDR_WIRE_SNAPSHOT_OBJECT, 11,
				5, now, payload, length) == 0);
	assert(frame_generation(&daemon, &peer, MIDR_WIRE_SNAPSHOT_END, 11,
				6, now, NULL, 0) == 0);
	assert(frame_generation(&daemon, &peer, MIDR_WIRE_EOR, 11, 7, now,
				NULL, 0) == 0);
	assert(midr_engine_lookup(daemon.engine, &remote.identity, mono_ms(),
					 &current, NULL) == 0);
	assert(current.sequence == remote.sequence);
	/* Late duplicate EOR has no stage and therefore no side effect. */
	assert(frame_generation(&daemon, &peer, MIDR_WIRE_EOR, 10, 8, now,
				NULL, 0) == 0);

	/* A stale SNAPSHOT_BEGIN is rejected too; it cannot reset an active
	 * higher-generation transaction. */
	assert(frame_generation(&daemon, &peer, MIDR_WIRE_SNAPSHOT_BEGIN, 20,
				9, now, NULL, 0) == 0);
	assert(frame_generation(&daemon, &peer, MIDR_WIRE_SNAPSHOT_OBJECT, 20,
				10, now, payload, length) == 0);
	assert(frame_generation(&daemon, &peer, MIDR_WIRE_SNAPSHOT_BEGIN, 19,
				11, now, NULL, 0) == -EPROTO);
	assert(stage_for(&daemon, &peer, false)->generation == 20);
	assert(stage_for(&daemon, &peer, false)->count == 1);
	assert(frame_generation(&daemon, &peer, MIDR_WIRE_SNAPSHOT_END, 20,
				12, now, NULL, 0) == 0);
	assert(frame_generation(&daemon, &peer, MIDR_WIRE_EOR, 20, 13, now,
				NULL, 0) == 0);
	teardown(&daemon);
}

static void test_shutdown_result_classification(void)
{
	assert(shutdown_result(0, 0, 0) == MIDRD_SHUTDOWN_COMPLETE);
	assert(shutdown_result(0, 2, 0) == MIDRD_SHUTDOWN_DEGRADED);
	assert(shutdown_result(0, 0, 1) == MIDRD_SHUTDOWN_DEGRADED);
	assert(shutdown_result(1, 0, 0) == MIDRD_SHUTDOWN_GENERATION_FAILED);
	/* A transient generation error that is recovered before the deadline is
	 * still a complete teardown; the cumulative counter records the error. */
	assert(shutdown_result(0, 0, 0) == MIDRD_SHUTDOWN_COMPLETE);
}

static void test_publication_failure_gates_ted_and_recovers(void)
{
	struct midrd daemon;
	struct midr_core_object local = membership(1, 7, 1);
	struct midr_core_object remote_membership = membership(2, 7, 1);
	struct midr_core_object remote_prefix = prefix(2, 1, 10);
	struct midr_core_object current;
	struct midr_consumer_snapshot snapshot = {0};
	enum midr_core_result result;
	bool scope_changed;
	uint64_t now = mono_ms();

	setup(&daemon);
	assert(midr_engine_apply(daemon.engine, &local, now, &result) == 0);
	assert(midr_engine_apply(daemon.engine, &remote_membership, now, &result) ==
	       0);
	drain_consumer_queue(daemon.consumer);
	assert(midr_ted_state(daemon.ted) == MIDR_TED_READY);
	fill_consumer_queue(daemon.consumer);
	assert(apply_update(&daemon, &remote_prefix, now, &result,
			    &scope_changed) == 0);
	assert(result == MIDR_CORE_ACCEPTED && !scope_changed);
	assert(midr_engine_lookup(daemon.engine, &remote_prefix.identity, now,
					 &current, NULL) == 0);
	assert(midr_engine_publication_pending(daemon.engine));
	assert(midr_engine_publication_error(daemon.engine) == -ENOSPC);
	assert(daemon.ted_rebuild_pending);
	assert(midr_ted_state(daemon.ted) == MIDR_TED_NOT_READY);
	assert(midr_ted_snapshot_acquire(daemon.ted, &snapshot) == -EAGAIN);
	drain_consumer_queue(daemon.consumer);
	assert(rebuild_ted(&daemon) == 0);
	assert(!midr_engine_publication_pending(daemon.engine));
	assert(!daemon.ted_rebuild_pending);
	assert(midr_ted_state(daemon.ted) == MIDR_TED_READY);
	assert(midr_ted_snapshot_acquire(daemon.ted, &snapshot) == 0);
	assert(snapshot.count == 1);
	midr_consumer_snapshot_release(&snapshot);
	teardown(&daemon);
}

int main(void)
{
	test_snapshot_barrier_and_atomic_eor();
	test_reconnect_generation_isolation();
	test_shutdown_result_classification();
	test_publication_failure_gates_ted_and_recovers();
	puts("midrd-sync-test: PASS");
	return 0;
}
