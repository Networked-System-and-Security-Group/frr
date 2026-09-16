/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "midr-consumer.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define MIDR_CONSUMER_MAX_PENDING 8192U
#define MIDR_CONSUMER_MAX_SNAPSHOT 8192U

struct midr_consumer_event_node {
	struct midr_consumer_event_node *next;
	struct midr_consumer_event event;
};

struct midr_consumer {
	struct midr_consumer_config config;
	struct midr_consumer_event_node *head;
	struct midr_consumer_event_node *tail;
	size_t pending;
	struct midr_consumer_event *snapshot;
	size_t snapshot_count;
	uint64_t snapshot_generation;
};

struct midr_consumer_stage {
	struct midr_consumer_event_node *head;
	struct midr_consumer_event_node *tail;
	struct midr_consumer_event *snapshot;
	size_t snapshot_count;
	size_t pending;
	uint64_t generation;
	uint32_t originator;
};

static bool ipv4_padding_is_zero(
	const uint8_t address[MIDR_CORE_ADDR_BYTES])
{
	for (size_t i = 4; i < MIDR_CORE_ADDR_BYTES; i++)
		if (address[i])
			return false;
	return true;
}

int midr_consumer_event_validate(const struct midr_consumer_event *event)
{
	if (!event || !event->generation)
		return -EINVAL;
	if (event->kind == MIDR_CONSUMER_SNAPSHOT_BEGIN ||
	    event->kind == MIDR_CONSUMER_SNAPSHOT_END)
		return event->originator ? 0 : -EINVAL;
	if (event->kind != MIDR_CONSUMER_LINK &&
	    event->kind != MIDR_CONSUMER_NODE_PREFIX &&
	    event->kind != MIDR_CONSUMER_GROUP_PREFIX)
		return -EINVAL;
	if (!event->originator)
		return -EINVAL;
	if (event->kind == MIDR_CONSUMER_LINK) {
		if (!event->remote || event->originator == event->remote ||
		    !event->metric || event->metric == UINT32_MAX ||
		    (event->family != MIDR_CORE_AF_IPV4 &&
		     event->family != MIDR_CORE_AF_IPV6))
			return -EINVAL;
		if (event->family == MIDR_CORE_AF_IPV4 &&
		    (!ipv4_padding_is_zero(event->local_address) ||
		     !ipv4_padding_is_zero(event->remote_address)))
			return -EINVAL;
		return 0;
	}
	if ((event->family != MIDR_CORE_AF_IPV4 &&
	     event->family != MIDR_CORE_AF_IPV6) ||
	    (event->family == MIDR_CORE_AF_IPV4 && event->prefix_len > 32U) ||
	    (event->family == MIDR_CORE_AF_IPV6 && event->prefix_len > 128U))
		return -EINVAL;
	return 0;
}

int midr_consumer_create(const struct midr_consumer_config *config,
			 struct midr_consumer **out)
{
	struct midr_consumer *consumer;

	if (!config || !out || *out)
		return -EINVAL;
	consumer = calloc(1, sizeof(*consumer));
	if (!consumer)
		return -ENOMEM;
	consumer->config = *config;
	*out = consumer;
	return 0;
}

void midr_consumer_destroy(struct midr_consumer **consumerp)
{
	struct midr_consumer_event_node *node;

	if (!consumerp || !*consumerp)
		return;
	while ((node = (*consumerp)->head)) {
		(*consumerp)->head = node->next;
		free(node);
	}
	free((*consumerp)->snapshot);
	free(*consumerp);
	*consumerp = NULL;
}

static int append_pending(struct midr_consumer *consumer,
			  struct midr_consumer_event_node *node)
{
	if (consumer->tail)
		consumer->tail->next = node;
	else
		consumer->head = node;
	consumer->tail = node;
	consumer->pending++;
	return 0;
}

static int publish_pending(struct midr_consumer *consumer,
			   const struct midr_consumer_event *event)
{
	struct midr_consumer_event_node *node;

	if (consumer->pending == MIDR_CONSUMER_MAX_PENDING)
		return -ENOSPC;
	node = malloc(sizeof(*node));
	if (!node)
		return -ENOMEM;
	node->event = *event;
	node->next = NULL;
	append_pending(consumer, node);
	if (consumer->config.on_event)
		consumer->config.on_event(consumer->config.arg, event);
	return 0;
}

int midr_consumer_publish(struct midr_consumer *consumer,
			  const struct midr_consumer_event *event)
{
	int ret = 0;

	if (!consumer || (ret = midr_consumer_event_validate(event)))
		return ret ? ret : -EINVAL;
	return publish_pending(consumer, event);
}

void midr_consumer_abort_prepared(struct midr_consumer_stage **stagep)
{
	struct midr_consumer_event_node *node;

	if (!stagep || !*stagep)
		return;
	while ((node = (*stagep)->head)) {
		(*stagep)->head = node->next;
		free(node);
	}
	free((*stagep)->snapshot);
	free(*stagep);
	*stagep = NULL;
}

int midr_consumer_prepare_snapshot(struct midr_consumer *consumer,
				   uint64_t generation,
				   uint32_t originator,
				   const struct midr_consumer_event *events,
				   size_t count,
				   struct midr_consumer_stage **stagep)
{
	struct midr_consumer_stage *stage = NULL;
	struct midr_consumer_event marker = {0};
	size_t total;

	if (!consumer || !generation || !originator || !stagep || *stagep ||
	    (count && !events) || count > MIDR_CONSUMER_MAX_SNAPSHOT)
		return -EINVAL;
	if (count > SIZE_MAX - 2U)
		return -EOVERFLOW;
	total = count + 2U;
	if (consumer->pending > MIDR_CONSUMER_MAX_PENDING - total)
		return -ENOSPC;
	stage = calloc(1, sizeof(*stage));
	if (!stage)
		return -ENOMEM;
	if (count) {
		stage->snapshot = calloc(count, sizeof(*stage->snapshot));
		if (!stage->snapshot) {
			midr_consumer_abort_prepared(&stage);
			return -ENOMEM;
		}
	}
	for (size_t i = 0; i < count; i++) {
		if (events[i].generation != generation ||
		    midr_consumer_event_validate(&events[i]) ||
		    events[i].kind == MIDR_CONSUMER_SNAPSHOT_BEGIN ||
		    events[i].kind == MIDR_CONSUMER_SNAPSHOT_END) {
			midr_consumer_abort_prepared(&stage);
			return -EINVAL;
		}
		stage->snapshot[i] = events[i];
	}
	marker.kind = MIDR_CONSUMER_SNAPSHOT_BEGIN;
	marker.generation = generation;
	marker.originator = originator;
	for (size_t i = 0; i < total; i++) {
		struct midr_consumer_event_node *node;
		const struct midr_consumer_event *source;

		if (i == 0)
			source = &marker;
		else if (i == total - 1) {
			marker.kind = MIDR_CONSUMER_SNAPSHOT_END;
			source = &marker;
		} else
			source = &stage->snapshot[i - 1];
		node = malloc(sizeof(*node));
		if (!node) {
			midr_consumer_abort_prepared(&stage);
			return -ENOMEM;
		}
		node->event = *source;
		node->next = NULL;
		if (stage->tail)
			stage->tail->next = node;
		else
			stage->head = node;
		stage->tail = node;
		stage->pending++;
	}
	stage->snapshot_count = count;
	stage->generation = generation;
	stage->originator = originator;
	*stagep = stage;
	return 0;
}

void midr_consumer_commit_prepared(struct midr_consumer *consumer,
				   struct midr_consumer_stage **stagep)
{
	struct midr_consumer_stage *stage;
	struct midr_consumer_event marker = {0};

	if (!consumer || !stagep || !*stagep)
		return;
	stage = *stagep;
	if (consumer->tail)
		consumer->tail->next = stage->head;
	else
		consumer->head = stage->head;
	consumer->tail = stage->tail;
	consumer->pending += stage->pending;
	stage->head = NULL;
	stage->tail = NULL;
	stage->pending = 0;
	free(consumer->snapshot);
	consumer->snapshot = stage->snapshot;
	consumer->snapshot_count = stage->snapshot_count;
	consumer->snapshot_generation = stage->generation;
	stage->snapshot = NULL;
	if (consumer->config.on_event) {
		marker.kind = MIDR_CONSUMER_SNAPSHOT_BEGIN;
		marker.generation = stage->generation;
		marker.originator = stage->originator;
		consumer->config.on_event(consumer->config.arg, &marker);
		for (size_t i = 0; i < consumer->snapshot_count; i++)
			consumer->config.on_event(consumer->config.arg,
						  &consumer->snapshot[i]);
		marker.kind = MIDR_CONSUMER_SNAPSHOT_END;
		consumer->config.on_event(consumer->config.arg, &marker);
	}
	free(stage);
	*stagep = NULL;
}

int midr_consumer_commit_snapshot(struct midr_consumer *consumer,
				  uint64_t generation,
				  uint32_t originator,
				  const struct midr_consumer_event *events,
				  size_t count)
{
	struct midr_consumer_stage *stage = NULL;
	int ret;

	ret = midr_consumer_prepare_snapshot(consumer, generation, originator,
					     events, count, &stage);
	if (ret)
		return ret;
	midr_consumer_commit_prepared(consumer, &stage);
	return 0;
}

int midr_consumer_snapshot_acquire(const struct midr_consumer *consumer,
				   struct midr_consumer_snapshot *snapshot)
{
	if (!consumer || !snapshot)
		return -EINVAL;
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->generation = consumer->snapshot_generation;
	snapshot->count = consumer->snapshot_count;
	if (!snapshot->count)
		return 0;
	snapshot->events = calloc(snapshot->count, sizeof(*snapshot->events));
	if (!snapshot->events)
		return -ENOMEM;
	memcpy(snapshot->events, consumer->snapshot,
	       snapshot->count * sizeof(*snapshot->events));
	return 0;
}

void midr_consumer_snapshot_release(struct midr_consumer_snapshot *snapshot)
{
	if (!snapshot)
		return;
	free(snapshot->events);
	memset(snapshot, 0, sizeof(*snapshot));
}

int midr_consumer_event_next(struct midr_consumer *consumer,
			     struct midr_consumer_event *event)
{
	struct midr_consumer_event_node *node;

	if (!consumer || !event)
		return -EINVAL;
	node = consumer->head;
	if (!node)
		return -ENOENT;
	consumer->head = node->next;
	if (!consumer->head)
		consumer->tail = NULL;
	*event = node->event;
	free(node);
	consumer->pending--;
	return 0;
}

size_t midr_consumer_pending(const struct midr_consumer *consumer)
{
	return consumer ? consumer->pending : 0;
}
