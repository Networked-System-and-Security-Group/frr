#include "midr-engine.h"
#include "midr-spf.h"
#include "midr-ted.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static struct midr_core_object prefix(uint32_t originator, uint8_t family);

static struct midr_core_object membership(uint32_t originator, uint32_t group)
{
	struct midr_core_object object = {0};

	object.identity.type = MIDR_CORE_MEMBERSHIP;
	object.identity.originator = originator;
	object.state = MIDR_CORE_ACTIVE;
	object.sequence = 1;
	object.group = group;
	return object;
}

static struct midr_core_object link_object(uint32_t originator,
					   uint32_t remote)
{
	struct midr_core_object object = {0};

	object.identity.type = MIDR_CORE_LINK;
	object.identity.originator = originator;
	object.identity.remote = remote;
	object.identity.link_id = 1;
	object.state = MIDR_CORE_ACTIVE;
	object.address_family = MIDR_CORE_AF_IPV4;
	object.sequence = 1;
	object.lifetime_ms = 1000;
	object.metric = 1;
	return object;
}

static void drain(struct midr_consumer *consumer)
{
	struct midr_consumer_event event;

	while (midr_consumer_event_next(consumer, &event) == 0)
		;
}

struct ted_bridge {
	struct midr_consumer *consumer;
	struct midr_ted *ted;
	int last_error;
};

static void apply_ted_snapshot(void *arg,
			       const struct midr_consumer_event *event)
{
	struct ted_bridge *bridge = arg;
	struct midr_consumer_snapshot snapshot = {0};

	if (event->kind != MIDR_CONSUMER_SNAPSHOT_END)
		return;
	bridge->last_error = midr_consumer_snapshot_acquire(bridge->consumer,
							    &snapshot);
	if (!bridge->last_error) {
		bridge->last_error = midr_ted_apply_snapshot(bridge->ted,
							     &snapshot);
		midr_consumer_snapshot_release(&snapshot);
	}
}

static void test_ted_failure_preserves_old_view(void)
{
	struct midr_engine_config engine_config = {
		.node_id = 300,
		.max_objects = 8,
		.lifetime_ms = 1000,
	};
	struct midr_ted_config ted_config = {.max_events = 8};
	struct ted_bridge bridge = {0};
	struct midr_consumer_config consumer_config = {
		.on_event = apply_ted_snapshot,
		.arg = &bridge,
	};
	struct midr_engine *engine = NULL;
	struct midr_consumer *consumer = NULL;
	struct midr_ted *ted = NULL;
	struct midr_core_object local = membership(300, 1);
	struct midr_core_object object = prefix(300, MIDR_CORE_AF_IPV4);
	struct midr_consumer_snapshot snapshot = {0};
	enum midr_core_result result;
	uint64_t old_generation;

	assert(midr_engine_create(&engine_config, &engine) == 0);
	assert(midr_ted_create(&ted_config, &ted) == 0);
	bridge.ted = ted;
	assert(midr_consumer_create(&consumer_config, &consumer) == 0);
	bridge.consumer = consumer;
	assert(midr_engine_attach_consumer(engine, consumer) == 0);
	assert(bridge.last_error == 0 && midr_ted_state(ted) == MIDR_TED_READY);
	assert(midr_engine_apply(engine, &local, 1, &result) == 0);
	assert(midr_engine_apply(engine, &object, 1, &result) == 0);
	old_generation = midr_ted_generation(ted);
	assert(midr_ted_test_fail_next(ted, -ENOMEM) == 0);
	object.sequence++;
	object.metric++;
	assert(midr_engine_apply(engine, &object, 2, &result) == 0);
	assert(result == MIDR_CORE_ACCEPTED && bridge.last_error == -ENOMEM);
	assert(midr_consumer_snapshot_acquire(consumer, &snapshot) == 0);
	assert(snapshot.generation > old_generation && snapshot.count == 1);
	midr_consumer_snapshot_release(&snapshot);
	assert(midr_ted_generation(ted) == old_generation);
	assert(midr_ted_state(ted) == MIDR_TED_NOT_READY);
	assert(midr_ted_snapshot_acquire(ted, &snapshot) == -EAGAIN);

	object.sequence++;
	object.metric++;
	assert(midr_engine_apply(engine, &object, 3, &result) == 0);
	assert(bridge.last_error == 0 && midr_ted_state(ted) == MIDR_TED_READY);
	assert(midr_consumer_snapshot_acquire(consumer, &snapshot) == 0);
	assert(midr_ted_generation(ted) == snapshot.generation);
	midr_consumer_snapshot_release(&snapshot);
	midr_ted_destroy(&ted);
	midr_consumer_destroy(&consumer);
	midr_engine_destroy(&engine);
}

static void test_batch_abort_is_atomic(void)
{
	struct midr_engine_config config = {
		.node_id = 200,
		.max_objects = 4,
		.lifetime_ms = 1000,
	};
	struct midr_engine *engine = NULL;
	struct midr_consumer *consumer = NULL;
	struct midr_consumer_config consumer_config = {0};
	struct midr_core_object object = prefix(20, MIDR_CORE_AF_IPV4);
	struct midr_core_object objects[1];
	size_t count = 0;

	assert(midr_consumer_create(&consumer_config, &consumer) == 0);
	assert(midr_engine_create(&config, &engine) == 0);
	assert(midr_engine_attach_consumer(engine, consumer) == 0);
	drain(consumer);
	assert(midr_engine_begin_batch(engine) == 0);
	assert(midr_engine_apply(engine, &object, 1,
				&(enum midr_core_result){0}) == 0);
	assert(midr_engine_count(engine) == 0);
	assert(midr_engine_snapshot(engine, 1, objects, 1, &count) == 0 &&
	       count == 0);
	assert(midr_engine_batch_snapshot(engine, 1, objects, 1, &count) == 0 &&
	       count == 1);
	assert(midr_engine_generation(engine) == 2);
	assert(midr_consumer_pending(consumer) == 0);
	assert(midr_engine_abort_batch(engine) == 0);
	assert(midr_engine_count(engine) == 0);
	assert(midr_engine_generation(engine) == 1);
	assert(midr_consumer_pending(consumer) == 0);
	midr_engine_destroy(&engine);
	midr_consumer_destroy(&consumer);
}

static void test_batch_failure_does_not_publish(void)
{
	struct midr_engine_config config = {
		.node_id = 201,
		.max_objects = 1,
		.lifetime_ms = 1000,
	};
	struct midr_engine *engine = NULL;
	struct midr_consumer *consumer = NULL;
	struct midr_consumer_config consumer_config = {0};
	struct midr_core_object first = prefix(21, MIDR_CORE_AF_IPV4);
	struct midr_core_object second = prefix(22, MIDR_CORE_AF_IPV4);
	enum midr_core_result result;

	assert(midr_consumer_create(&consumer_config, &consumer) == 0);
	assert(midr_engine_create(&config, &engine) == 0);
	assert(midr_engine_attach_consumer(engine, consumer) == 0);
	drain(consumer);
	assert(midr_engine_begin_batch(engine) == 0);
	assert(midr_engine_apply(engine, &first, 1, &result) == 0);
	assert(midr_engine_apply(engine, &second, 1, &result) == -ENOSPC);
	assert(midr_engine_end_batch(engine, 1) == -ENOSPC);
	assert(midr_engine_count(engine) == 0);
	assert(midr_engine_generation(engine) == 1);
	assert(midr_consumer_pending(consumer) == 0);
	/* The failed transaction was discarded by end_batch(). */
	assert(midr_engine_begin_batch(engine) == 0);
	assert(midr_engine_apply(engine, &first, 2, &result) == 0);
	assert(midr_engine_end_batch(engine, 2) == 0);
	assert(midr_engine_count(engine) == 1);
	assert(midr_engine_generation(engine) == 2);
	midr_engine_destroy(&engine);
	midr_consumer_destroy(&consumer);
}

static void test_batch_invalid_is_atomic(void)
{
	struct midr_engine_config config = {
		.node_id = 202,
		.max_objects = 4,
		.lifetime_ms = 1000,
	};
	struct midr_engine *engine = NULL;
	struct midr_consumer *consumer = NULL;
	struct midr_consumer_config consumer_config = {0};
	struct midr_core_object invalid = prefix(23, MIDR_CORE_AF_IPV4);
	enum midr_core_result result;

	invalid.identity.originator = 0;
	assert(midr_consumer_create(&consumer_config, &consumer) == 0);
	assert(midr_engine_create(&config, &engine) == 0);
	assert(midr_engine_attach_consumer(engine, consumer) == 0);
	drain(consumer);
	assert(midr_engine_begin_batch(engine) == 0);
	assert(midr_engine_apply(engine, &invalid, 1, &result) == -EINVAL);
	assert(midr_engine_end_batch(engine, 1) == -EINVAL);
	assert(midr_engine_count(engine) == 0);
	assert(midr_engine_generation(engine) == 1);
	assert(midr_consumer_pending(consumer) == 0);
	midr_engine_destroy(&engine);
	midr_consumer_destroy(&consumer);
}

static size_t snapshot_count(struct midr_consumer *consumer)
{
	struct midr_consumer_snapshot snapshot = {0};
	size_t count;

	assert(midr_consumer_snapshot_acquire(consumer, &snapshot) == 0);
	count = snapshot.count;
	midr_consumer_snapshot_release(&snapshot);
	return count;
}

static void test_membership_scope_lifecycle(void)
{
	struct midr_engine_config config = {
		.node_id = 1,
		.max_objects = 16,
		.lifetime_ms = 1000,
	};
	struct midr_consumer_config consumer_config = {0};
	struct midr_engine *engine = NULL;
	struct midr_consumer *consumer = NULL;
	struct midr_core_object m1 = membership(1, 10);
	struct midr_core_object m2 = membership(2, 10);
	struct midr_core_object m3 = membership(3, 20);
	struct midr_core_object p2 = prefix(2, MIDR_CORE_AF_IPV4);
	struct midr_core_object p3 = prefix(3, MIDR_CORE_AF_IPV4);
	struct midr_core_object l12 = link_object(1, 2);
	struct midr_core_object l13 = link_object(1, 3);
	struct midr_core_object m2_moved = m2;
	struct midr_core_object current;
	struct midr_core_identity key = m2.identity;
	struct midr_core_object memberships[] = {m1, m2, m3};
	enum midr_core_result result;
	size_t expired = 0;

	assert(midr_consumer_create(&consumer_config, &consumer) == 0);
	assert(midr_engine_create(&config, &engine) == 0);
	assert(midr_engine_attach_consumer(engine, consumer) == 0);
	drain(consumer);
	for (size_t i = 0; i < sizeof(memberships) / sizeof(memberships[0]); i++) {
		assert(midr_engine_apply(engine, &memberships[i], 1, &result) == 0);
		assert(result == MIDR_CORE_ACCEPTED);
		drain(consumer);
	}
	assert(midr_engine_apply(engine, &p2, 2, &result) == 0);
	assert(midr_engine_apply(engine, &p3, 2, &result) == 0);
	assert(midr_engine_apply(engine, &l12, 2, &result) == 0);
	assert(midr_engine_apply(engine, &l13, 2, &result) == 0);
	assert(snapshot_count(consumer) == 3);
	drain(consumer);

	/* A same-identity Membership update changes scope atomically. */
	m2_moved.sequence = 2;
	m2_moved.group = 20;
	assert(midr_engine_apply(engine, &m2_moved, 3, &result) == 0);
	assert(midr_engine_membership(engine, 2, &current.group) == 0 &&
	       current.group == 20);
	assert(snapshot_count(consumer) == 2);
	drain(consumer);

	/* Withdrawal removes the peer's local-only objects but leaves the global
	 * link usable; the canonical store still retains the withdrawn identity. */
	assert(midr_engine_withdraw(engine, &key, 4) == 0);
	assert(snapshot_count(consumer) == 1);
	assert(midr_engine_count(engine) == 7);
	drain(consumer);

	/* Membership expiry rebuilds scope and immediately withdraws dependent
	 * objects from the committed usable snapshot. */
	assert(midr_engine_expire(engine, 2000, &expired) == 0);
	assert(expired == 7);
	assert(snapshot_count(consumer) == 0);
	assert(midr_engine_membership(engine, 1, &current.group) == -ENOENT);
	midr_engine_destroy(&engine);
	midr_consumer_destroy(&consumer);
}

static void test_scope_rollback_is_atomic(void)
{
	struct midr_engine_config config = {
		.node_id = 10,
		.max_objects = 2,
		.lifetime_ms = 1000,
	};
	struct midr_consumer_config consumer_config = {0};
	struct midr_engine *engine = NULL;
	struct midr_consumer *consumer = NULL;
	struct midr_core_object m = membership(10, 1);
	struct midr_core_object p1 = prefix(11, MIDR_CORE_AF_IPV4);
	struct midr_core_object p2 = prefix(12, MIDR_CORE_AF_IPV4);
	enum midr_core_result result;

	assert(midr_consumer_create(&consumer_config, &consumer) == 0);
	assert(midr_engine_create(&config, &engine) == 0);
	assert(midr_engine_attach_consumer(engine, consumer) == 0);
	drain(consumer);
	assert(midr_engine_begin_batch(engine) == 0);
	assert(midr_engine_apply(engine, &m, 1, &result) == 0);
	assert(midr_engine_apply(engine, &p1, 1, &result) == 0);
	assert(midr_engine_apply(engine, &p2, 1, &result) == -ENOSPC);
	assert(midr_engine_end_batch(engine, 1) == -ENOSPC);
	assert(midr_engine_count(engine) == 0);
	assert(midr_engine_membership(engine, 10, &(uint32_t){0}) == -ENOENT);
	assert(snapshot_count(consumer) == 0);
	midr_engine_destroy(&engine);
	midr_consumer_destroy(&consumer);
}

static void test_snapshot_rebuilds_expired_scope(void)
{
	struct midr_engine_config config = {
		.node_id = 1,
		.max_objects = 8,
		.lifetime_ms = 100,
	};
	struct midr_consumer_config consumer_config = {0};
	struct midr_engine *engine = NULL;
	struct midr_consumer *consumer = NULL;
	struct midr_core_object m = membership(1, 10);
	struct midr_core_object p = prefix(1, MIDR_CORE_AF_IPV4);
	struct midr_core_object later = prefix(2, MIDR_CORE_AF_IPV4);
	enum midr_core_result result;

	assert(midr_consumer_create(&consumer_config, &consumer) == 0);
	assert(midr_engine_create(&config, &engine) == 0);
	assert(midr_engine_attach_consumer(engine, consumer) == 0);
	drain(consumer);
	assert(midr_engine_apply(engine, &m, 1, &result) == 0);
	assert(midr_engine_apply(engine, &p, 1, &result) == 0);
	assert(snapshot_count(consumer) == 1);
	drain(consumer);
	/* The read-side expiry must rebuild the scope before publishing the new
	 * object, so the expired local Membership cannot authorize it. */
	assert(midr_engine_apply(engine, &later, 200, &result) == 0);
	assert(snapshot_count(consumer) == 0);
	assert(midr_engine_membership(engine, 1, &(uint32_t){0}) == -ENOENT);
	midr_engine_destroy(&engine);
	midr_consumer_destroy(&consumer);
}

static struct midr_core_object prefix(uint32_t originator, uint8_t family)
{
	struct midr_core_object object = {0};

	object.identity.type = MIDR_CORE_NODE_PREFIX;
	object.identity.family = family;
	object.identity.prefix_len = family == MIDR_CORE_AF_IPV4 ? 24 : 64;
	object.identity.originator = originator;
	object.identity.prefix[0] = family == MIDR_CORE_AF_IPV4 ? 192 : 0x20;
	object.identity.prefix[1] = family == MIDR_CORE_AF_IPV4 ? 0 : 1;
	object.identity.prefix[2] = family == MIDR_CORE_AF_IPV4 ? 2 : 0x0d;
	object.sequence = 1;
	object.state = MIDR_CORE_ACTIVE;
	object.lifetime_ms = 1000;
	object.metric = originator;
	return object;
}

int main(void)
{
	test_batch_abort_is_atomic();
	test_batch_failure_does_not_publish();
	test_batch_invalid_is_atomic();
	test_membership_scope_lifecycle();
	test_scope_rollback_is_atomic();
	test_snapshot_rebuilds_expired_scope();
	test_ted_failure_preserves_old_view();
	struct midr_engine_config engine_config = {
		.node_id = 100,
		.max_objects = 16,
		.lifetime_ms = 1000,
	};
	struct midr_consumer_config consumer_config = {0};
	struct midr_engine *engine = NULL;
	struct midr_consumer *consumer = NULL;
	struct midr_consumer_snapshot snapshot = {0};
	struct midr_spf_route routes[4] = {0};
	struct midr_core_object v4 = prefix(10, MIDR_CORE_AF_IPV4);
	struct midr_core_object v6 = prefix(11, MIDR_CORE_AF_IPV6);
	struct midr_core_object m_local = membership(100, 10);
	struct midr_core_object m_v4 = membership(10, 10);
	struct midr_core_object m_v6 = membership(11, 10);
	size_t count = 0;

	assert(midr_consumer_create(&consumer_config, &consumer) == 0);
	assert(midr_engine_create(&engine_config, &engine) == 0);
	assert(midr_engine_attach_consumer(engine, consumer) == 0);
	drain(consumer);
	assert(midr_engine_apply(engine, &m_local, 1,
				&(enum midr_core_result){0}) == 0);
	drain(consumer);
	assert(midr_engine_apply(engine, &m_v4, 1,
				&(enum midr_core_result){0}) == 0);
	drain(consumer);
	assert(midr_engine_apply(engine, &m_v6, 1,
				&(enum midr_core_result){0}) == 0);
	drain(consumer);
	assert(midr_engine_begin_batch(engine) == 0);
	assert(midr_engine_apply(engine, &v4, 1, &(enum midr_core_result){0}) == 0);
	assert(midr_engine_apply(engine, &v6, 1, &(enum midr_core_result){0}) == 0);
	assert(midr_consumer_pending(consumer) == 0);
	assert(midr_engine_end_batch(engine, 1) == 0);
	assert(midr_consumer_pending(consumer) == 4);
	assert(midr_consumer_snapshot_acquire(consumer, &snapshot) == 0);
	assert(snapshot.count == 2 && snapshot.generation == 6);
	assert(midr_spf_compute(&snapshot, engine_config.node_id, routes, 4,
				       &count) == 0 && count == 2);
	midr_consumer_snapshot_release(&snapshot);
	drain(consumer);
	assert(midr_engine_withdraw(engine, &v4.identity, 2) == 0);
	assert(midr_consumer_snapshot_acquire(consumer, &snapshot) == 0);
	assert(snapshot.count == 1 && snapshot.events[0].family == MIDR_CORE_AF_IPV6);
	midr_consumer_snapshot_release(&snapshot);
	midr_engine_destroy(&engine);
	midr_consumer_destroy(&consumer);
	puts("midrd-engine-test: PASS");
	return 0;
}
