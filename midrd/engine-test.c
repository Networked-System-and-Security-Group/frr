#include "midr-engine.h"
#include "midr-spf.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static struct midr_core_object prefix(uint32_t originator, uint8_t family);

static void drain(struct midr_consumer *consumer)
{
	struct midr_consumer_event event;

	while (midr_consumer_event_next(consumer, &event) == 0)
		;
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

	assert(midr_consumer_create(&consumer_config, &consumer) == 0);
	assert(midr_engine_create(&config, &engine) == 0);
	assert(midr_engine_attach_consumer(engine, consumer) == 0);
	drain(consumer);
	assert(midr_engine_begin_batch(engine) == 0);
	assert(midr_engine_apply(engine, &object, 1,
				&(enum midr_core_result){0}) == 0);
	assert(midr_engine_count(engine) == 0);
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
	size_t count = 0;

	assert(midr_consumer_create(&consumer_config, &consumer) == 0);
	assert(midr_engine_create(&engine_config, &engine) == 0);
	assert(midr_engine_attach_consumer(engine, consumer) == 0);
	drain(consumer);
	assert(midr_engine_begin_batch(engine) == 0);
	assert(midr_engine_apply(engine, &v4, 1, &(enum midr_core_result){0}) == 0);
	assert(midr_engine_apply(engine, &v6, 1, &(enum midr_core_result){0}) == 0);
	assert(midr_consumer_pending(consumer) == 0);
	assert(midr_engine_end_batch(engine, 1) == 0);
	assert(midr_consumer_pending(consumer) == 4);
	assert(midr_consumer_snapshot_acquire(consumer, &snapshot) == 0);
	assert(snapshot.count == 2 && snapshot.generation == 3);
	assert(midr_spf_compute(&snapshot, routes, 4, &count) == 0 && count == 2);
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
