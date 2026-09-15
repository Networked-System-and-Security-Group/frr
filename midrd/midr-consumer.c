/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "midr-consumer.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define MIDR_CONSUMER_MAX_PENDING 8192U

struct midr_consumer_event_node {
	struct midr_consumer_event_node *next;
	struct midr_consumer_event event;
};

struct midr_consumer {
	struct midr_consumer_config config;
	struct midr_consumer_event_node *head;
	struct midr_consumer_event_node *tail;
	size_t pending;
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
		return event->remote ? 0 : -EINVAL;
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
	free(*consumerp);
	*consumerp = NULL;
}

int midr_consumer_publish(struct midr_consumer *consumer,
			  const struct midr_consumer_event *event)
{
	struct midr_consumer_event_node *node;
	int ret = 0;

	if (!consumer || (ret = midr_consumer_event_validate(event)))
		return ret ? ret : -EINVAL;
	if (consumer->pending == MIDR_CONSUMER_MAX_PENDING)
		return -ENOSPC;
	if (consumer->config.on_event) {
		ret = consumer->config.on_event(consumer->config.arg, event);
		if (ret)
			return ret;
	}
	node = malloc(sizeof(*node));
	if (!node)
		return -ENOMEM;
	node->event = *event;
	node->next = NULL;
	if (consumer->tail)
		consumer->tail->next = node;
	else
		consumer->head = node;
	consumer->tail = node;
	consumer->pending++;
	return 0;
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
