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
		return event->remote && event->originator != event->remote &&
		       event->metric && event->metric != UINT32_MAX ? 0 : -EINVAL;
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
	if (consumer->config.on_event) {
		int ret = consumer->config.on_event(consumer->config.arg, event);

		if (ret)
			return ret;
	}
	node = malloc(sizeof(*node));
	if (!node)
		return -ENOMEM;
	node->event = *event;
	node->next = NULL;
	return append_pending(consumer, node);
}

int midr_consumer_publish(struct midr_consumer *consumer,
			  const struct midr_consumer_event *event)
{
	int ret = 0;

	if (!consumer || (ret = midr_consumer_event_validate(event)))
		return ret ? ret : -EINVAL;
	return publish_pending(consumer, event);
}

int midr_consumer_commit_snapshot(struct midr_consumer *consumer,
				  uint64_t generation,
				  uint32_t originator,
				  const struct midr_consumer_event *events,
				  size_t count)
{
	struct midr_consumer_event *copy = NULL;
	struct midr_consumer_event_node **nodes = NULL;
	struct midr_consumer_event marker = {0};
	size_t total;
	int ret = 0;

	if (!consumer || !generation || !originator || (count && !events) ||
	    count > MIDR_CONSUMER_MAX_SNAPSHOT)
		return -EINVAL;
	if (count > SIZE_MAX - 2U)
		return -EOVERFLOW;
	total = count + 2U;
	if (consumer->pending > MIDR_CONSUMER_MAX_PENDING - total)
		return -ENOSPC;
	if (count) {
		copy = calloc(count, sizeof(*copy));
		if (!copy)
			return -ENOMEM;
	}
	for (size_t i = 0; i < count; i++) {
		if (events[i].generation != generation ||
		    midr_consumer_event_validate(&events[i]) ||
		    events[i].kind == MIDR_CONSUMER_SNAPSHOT_BEGIN ||
		    events[i].kind == MIDR_CONSUMER_SNAPSHOT_END) {
			free(copy);
			return -EINVAL;
		}
		copy[i] = events[i];
	}
	marker.kind = MIDR_CONSUMER_SNAPSHOT_BEGIN;
	marker.generation = generation;
	marker.originator = originator;
	nodes = calloc(total, sizeof(*nodes));
	if (!nodes) {
		free(copy);
		return -ENOMEM;
	}
	for (size_t i = 0; i < total; i++) {
		const struct midr_consumer_event *source = i == 0 ? &marker :
			(i == total - 1 ? &marker : &copy[i - 1]);

		if (i == total - 1)
			marker.kind = MIDR_CONSUMER_SNAPSHOT_END;
		if (consumer->config.on_event) {
			ret = consumer->config.on_event(consumer->config.arg, source);
			if (ret)
				break;
		}
		nodes[i] = malloc(sizeof(*nodes[i]));
		if (!nodes[i]) {
			ret = -ENOMEM;
			break;
		}
		nodes[i]->event = *source;
		nodes[i]->next = NULL;
	}
	if (ret) {
		for (size_t i = 0; i < total; i++)
			free(nodes[i]);
		free(nodes);
		free(copy);
		return ret;
	}
	for (size_t i = 0; i < total; i++)
		append_pending(consumer, nodes[i]);
	free(nodes);
	free(consumer->snapshot);
	consumer->snapshot = copy;
	consumer->snapshot_count = count;
	consumer->snapshot_generation = generation;
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
