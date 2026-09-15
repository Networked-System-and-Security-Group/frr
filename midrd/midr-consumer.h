/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MIDRD_CONSUMER_H
#define MIDRD_CONSUMER_H

#include <stddef.h>
#include <stdint.h>

#include "midr-core.h"

enum midr_consumer_event_kind {
	MIDR_CONSUMER_SNAPSHOT_BEGIN = 1,
	MIDR_CONSUMER_LINK = 2,
	MIDR_CONSUMER_NODE_PREFIX = 3,
	MIDR_CONSUMER_GROUP_PREFIX = 4,
	MIDR_CONSUMER_SNAPSHOT_END = 5,
};

struct midr_consumer_event {
	enum midr_consumer_event_kind kind;
	uint64_t generation;
	uint32_t originator;
	uint32_t remote;
	uint32_t group;
	uint64_t link_id;
	uint8_t family;
	uint8_t prefix_len;
	uint8_t reserved[2];
	uint8_t prefix[MIDR_CORE_ADDR_BYTES];
	uint32_t metric;
	uint8_t local_address[MIDR_CORE_ADDR_BYTES];
	uint8_t remote_address[MIDR_CORE_ADDR_BYTES];
};

/* Post-commit notification.  The callback observes an already committed
 * snapshot/event and cannot reject or roll it back.  Consumers that need a
 * transactional downstream update must acquire the committed snapshot and
 * stage that update independently. */
typedef void (*midr_consumer_event_cb)(
	void *arg, const struct midr_consumer_event *event);

struct midr_consumer_config {
	midr_consumer_event_cb on_event;
	void *arg;
};

struct midr_consumer_snapshot {
	uint64_t generation;
	size_t count;
	struct midr_consumer_event *events;
};

struct midr_consumer;

int midr_consumer_create(const struct midr_consumer_config *config,
			 struct midr_consumer **out);
void midr_consumer_destroy(struct midr_consumer **consumer);
int midr_consumer_publish(struct midr_consumer *consumer,
			  const struct midr_consumer_event *event);
int midr_consumer_commit_snapshot(struct midr_consumer *consumer,
				  uint64_t generation,
				  uint32_t originator,
				  const struct midr_consumer_event *events,
				  size_t count);
int midr_consumer_snapshot_acquire(const struct midr_consumer *consumer,
				   struct midr_consumer_snapshot *snapshot);
void midr_consumer_snapshot_release(struct midr_consumer_snapshot *snapshot);
int midr_consumer_event_validate(const struct midr_consumer_event *event);
int midr_consumer_event_next(struct midr_consumer *consumer,
			     struct midr_consumer_event *event);
size_t midr_consumer_pending(const struct midr_consumer *consumer);

#endif /* MIDRD_CONSUMER_H */
