#include "midr-consumer.h"

#include <assert.h>
#include <stdio.h>

static int callback(void *arg, const struct midr_consumer_event *event)
{
	unsigned int *count = arg;
	(void)event;
	(*count)++;
	return 0;
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
	unsigned int count = 0;

	config.on_event = callback;
	config.arg = &count;
	assert(midr_consumer_create(&config, &consumer) == 0);
	assert(midr_consumer_publish(consumer, &event) == 0);
	assert(count == 1 && midr_consumer_pending(consumer) == 1);
	assert(midr_consumer_event_next(consumer, &copied) == 0);
	assert(copied.originator == 42 && midr_consumer_pending(consumer) == 0);
	committed.generation = 8;
	committed.originator = 42;
	assert(midr_consumer_commit_snapshot(consumer, 8, 42, &committed, 1) == 0);
	assert(midr_consumer_snapshot_acquire(consumer, &snapshot) == 0);
	assert(snapshot.generation == 8 && snapshot.count == 1);
	replacement.generation = 9;
	assert(midr_consumer_commit_snapshot(consumer, 9, 42, &replacement, 1) == 0);
	assert(midr_consumer_snapshot_acquire(consumer, &old_snapshot) == 0);
	assert(snapshot.generation == 8 && snapshot.events[0].generation == 8);
	assert(old_snapshot.generation == 9 &&
	       old_snapshot.events[0].generation == 9);
	/* Acquired snapshots own their event arrays and outlive a commit. */
	midr_consumer_snapshot_release(&snapshot);
	midr_consumer_snapshot_release(&old_snapshot);
	midr_consumer_destroy(&consumer);
	puts("midrd-consumer-test: PASS");
	return 0;
}
