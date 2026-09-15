#include "midr-consumer.h"

#include <assert.h>
#include <stdio.h>

static int callback(void *arg, const struct midr_consumer_event *event)
{
	unsigned int *count = arg;
	assert(event->generation == 7);
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
	unsigned int count = 0;

	config.on_event = callback;
	config.arg = &count;
	assert(midr_consumer_create(&config, &consumer) == 0);
	assert(midr_consumer_publish(consumer, &event) == 0);
	assert(count == 1 && midr_consumer_pending(consumer) == 1);
	assert(midr_consumer_event_next(consumer, &copied) == 0);
	assert(copied.originator == 42 && midr_consumer_pending(consumer) == 0);
	midr_consumer_destroy(&consumer);
	puts("midrd-consumer-test: PASS");
	return 0;
}
