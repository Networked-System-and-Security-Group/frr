// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Minimal path-computation consumer used to verify the public TED contract.
 */

#include <zebra.h>

#include <errno.h>

#include "tests/bgpd/test_midr_ted_consumer.h"

static void path_consumer_snapshot_changed(struct midr_context *ctx, uint64_t generation,
					   uint32_t change_flags, void *arg)
{
	struct midr_ted_path_consumer_stub *consumer = arg;

	(void)ctx;
	consumer->last_generation = generation;
	consumer->last_change_flags = change_flags;
	consumer->notification_count++;
}

int midr_ted_path_consumer_stub_start(struct midr_context *ctx,
				      struct midr_ted_path_consumer_stub *consumer)
{
	const struct midr_ted_consumer_ops ops = {
		.snapshot_changed = path_consumer_snapshot_changed,
	};

	if (!consumer || consumer->registration)
		return -EINVAL;

	consumer->last_generation = 0;
	consumer->last_change_flags = MIDR_TED_CHANGE_NONE;
	consumer->notification_count = 0;
	return midr_ted_consumer_register(ctx, &ops, consumer, &consumer->registration);
}

void midr_ted_path_consumer_stub_stop(struct midr_context *ctx,
				      struct midr_ted_path_consumer_stub *consumer)
{
	if (!consumer)
		return;

	midr_ted_consumer_unregister(ctx, &consumer->registration);
}

int midr_ted_path_consumer_stub_snapshot_get(struct midr_context *ctx,
					     const struct midr_ted_path_consumer_stub *consumer,
					     const struct midr_ted_snapshot **snapshot)
{
	if (!consumer || !consumer->registration)
		return -ENOENT;

	return midr_ted_snapshot_get(ctx, snapshot);
}

bool midr_ted_path_consumer_stub_result_is_current(
	struct midr_context *ctx, const struct midr_ted_path_consumer_stub *consumer,
	uint64_t generation)
{
	if (!consumer || !consumer->registration)
		return false;

	return midr_ted_generation_is_current(ctx, generation);
}
