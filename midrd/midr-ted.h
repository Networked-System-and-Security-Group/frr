/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MIDRD_TED_H
#define MIDRD_TED_H

#include <stddef.h>
#include <stdint.h>

#include "midr-consumer.h"

enum midr_ted_state {
	MIDR_TED_NOT_READY = 0,
	MIDR_TED_READY = 1,
};

struct midr_ted_config {
	size_t max_events;
};

struct midr_ted;

int midr_ted_create(const struct midr_ted_config *config,
		    struct midr_ted **out);
void midr_ted_destroy(struct midr_ted **ted);

/* Build a complete candidate view and publish it with one pointer swap. */
int midr_ted_apply_snapshot(struct midr_ted *ted,
			    const struct midr_consumer_snapshot *snapshot);

/* Hide the active view after an upstream staging failure. */
int midr_ted_invalidate(struct midr_ted *ted, int error);

/* Returns -EAGAIN while the current derived view is not usable. */
int midr_ted_snapshot_acquire(const struct midr_ted *ted,
			      struct midr_consumer_snapshot *snapshot);

enum midr_ted_state midr_ted_state(const struct midr_ted *ted);
uint64_t midr_ted_generation(const struct midr_ted *ted);
int midr_ted_last_error(const struct midr_ted *ted);

/* Test-only fault injection.  The next apply fails and consumes the error. */
int midr_ted_test_fail_next(struct midr_ted *ted, int error);

#endif /* MIDRD_TED_H */
