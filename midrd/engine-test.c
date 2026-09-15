#include "midr-engine.h"
#include "midr-spf.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void drain(struct midr_consumer *consumer)
{
	struct midr_consumer_event event;

	while (midr_consumer_event_next(consumer, &event) == 0)
		;
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
