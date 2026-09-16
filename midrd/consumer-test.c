#include "midr-consumer.h"

#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>

struct callback_probe {
	struct midr_consumer *consumer;
	uint64_t expected_generation;
	unsigned int count;
	bool saw_committed_snapshot;
};

static void callback(void *arg, const struct midr_consumer_event *event)
{
	struct callback_probe *probe = arg;

	probe->count++;
	if (event->kind == MIDR_CONSUMER_SNAPSHOT_BEGIN &&
	    probe->expected_generation) {
		struct midr_consumer_snapshot snapshot = {0};

		assert(midr_consumer_snapshot_acquire(probe->consumer,
						      &snapshot) == 0);
		assert(snapshot.generation == probe->expected_generation);
		probe->saw_committed_snapshot = true;
		midr_consumer_snapshot_release(&snapshot);
	}
}

int main(void)
{
	struct midr_consumer *consumer = NULL;
	struct midr_consumer_event event = {
		.kind = MIDR_CONSUMER_NODE_PREFIX,
		.generation = 7,
		.originator = 42,
		.family = MIDR_CORE_AF_IPV4,
		.prefix_len = 24,
	};
	struct midr_consumer_config config;
	struct midr_consumer_event copied;
	struct midr_consumer_event committed = event;
	struct midr_consumer_event replacement = event;
	struct midr_consumer_snapshot snapshot = {0};
	struct midr_consumer_snapshot old_snapshot = {0};
	struct midr_consumer_stage *stage = NULL;
	struct midr_consumer_event bad_link = {
		.kind = MIDR_CONSUMER_LINK,
		.generation = 7,
		.originator = 42,
		.remote = 43,
		.family = MIDR_CORE_AF_IPV6,
	};
	struct callback_probe probe = {0};

	config.on_event = callback;
	config.arg = &probe;
	assert(midr_consumer_create(&config, &consumer) == 0);
	probe.consumer = consumer;
	assert(midr_consumer_publish(consumer, &bad_link) == -EINVAL);
	bad_link.metric = 10;
	assert(midr_consumer_publish(consumer, &bad_link) == 0);
	assert(midr_consumer_event_next(consumer, &copied) == 0);
	assert(midr_consumer_publish(consumer, &event) == 0);
	assert(probe.count == 2 && midr_consumer_pending(consumer) == 1);
	assert(midr_consumer_event_next(consumer, &copied) == 0);
	assert(copied.originator == 42 && midr_consumer_pending(consumer) == 0);
	committed.generation = 8;
	committed.originator = 42;
	probe.expected_generation = 8;
	assert(midr_consumer_commit_snapshot(consumer, 8, 42, &committed, 1) == 0);
	assert(probe.saw_committed_snapshot);
	assert(midr_consumer_snapshot_acquire(consumer, &snapshot) == 0);
	assert(snapshot.generation == 8 && snapshot.count == 1);
	replacement.generation = 9;
	probe.expected_generation = 9;
	probe.saw_committed_snapshot = false;
	assert(midr_consumer_commit_snapshot(consumer, 9, 42, &replacement, 1) == 0);
	assert(probe.saw_committed_snapshot);
	assert(midr_consumer_snapshot_acquire(consumer, &old_snapshot) == 0);
	assert(snapshot.generation == 8 && snapshot.events[0].generation == 8);
	assert(old_snapshot.generation == 9 &&
	       old_snapshot.events[0].generation == 9);
	/* Acquired snapshots own their event arrays and outlive a commit. */
	midr_consumer_snapshot_release(&snapshot);
	midr_consumer_snapshot_release(&old_snapshot);

	replacement.generation = 10;
	probe.expected_generation = 10;
	probe.saw_committed_snapshot = false;
	assert(midr_consumer_prepare_snapshot(consumer, 10, 42, &replacement, 1,
					      &stage) == 0);
	assert(!probe.saw_committed_snapshot);
	assert(midr_consumer_snapshot_acquire(consumer, &snapshot) == 0);
	assert(snapshot.generation == 9);
	midr_consumer_snapshot_release(&snapshot);
	midr_consumer_abort_prepared(&stage);
	assert(!stage && !probe.saw_committed_snapshot);
	assert(midr_consumer_prepare_snapshot(consumer, 10, 42, &replacement, 1,
					      &stage) == 0);
	midr_consumer_commit_prepared(consumer, &stage);
	assert(!stage && probe.saw_committed_snapshot);
	assert(midr_consumer_snapshot_acquire(consumer, &snapshot) == 0);
	assert(snapshot.generation == 10);
	midr_consumer_snapshot_release(&snapshot);
	midr_consumer_destroy(&consumer);
	puts("midrd-consumer-test: PASS");
	return 0;
}
