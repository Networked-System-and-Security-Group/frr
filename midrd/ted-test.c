/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "midr-ted.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static struct midr_consumer_event node_event(uint64_t generation,
					     uint32_t originator,
					     uint8_t suffix)
{
	struct midr_consumer_event event = {
		.kind = MIDR_CONSUMER_NODE_PREFIX,
		.generation = generation,
		.originator = originator,
		.family = MIDR_CORE_AF_IPV6,
		.prefix_len = 128,
	};

	event.prefix[0] = 0x20;
	event.prefix[1] = 0x01;
	event.prefix[2] = 0x0d;
	event.prefix[3] = 0xb8;
	event.prefix[15] = suffix;
	return event;
}

static struct midr_consumer_event link_event(uint64_t generation,
					     uint32_t originator,
					     uint32_t remote,
					     uint64_t link_id)
{
	struct midr_consumer_event event = {
		.kind = MIDR_CONSUMER_LINK,
		.generation = generation,
		.originator = originator,
		.remote = remote,
		.link_id = link_id,
		.family = MIDR_CORE_AF_IPV4,
		.metric = 10,
	};

	event.local_address[0] = 192;
	event.local_address[1] = 0;
	event.local_address[2] = 2;
	event.local_address[3] = 1;
	event.remote_address[0] = 198;
	event.remote_address[1] = 51;
	event.remote_address[2] = 100;
	event.remote_address[3] = (uint8_t)remote;
	return event;
}

struct callback_state {
	size_t count;
	uint64_t generation;
	uint32_t flags;
};

static void snapshot_changed(struct midr_ted *ted, uint64_t generation,
				     uint32_t flags, void *arg)
{
	struct callback_state *state = arg;

	assert(ted);
	state->count++;
	state->generation = generation;
	state->flags = flags;
}

struct reentrant_callback_state {
	struct midr_ted_consumer **self;
	struct midr_ted_consumer **added;
	struct callback_state *added_state;
	size_t count;
};

static void reentrant_snapshot_changed(struct midr_ted *ted,
				       uint64_t generation, uint32_t flags,
				       void *arg)
{
	static const struct midr_ted_consumer_ops added_ops = {
		.snapshot_changed = snapshot_changed,
	};
	struct reentrant_callback_state *state = arg;

	(void)generation;
	(void)flags;
	state->count++;
	midr_ted_consumer_unregister(ted, state->self);
	assert(midr_ted_consumer_register(ted, &added_ops, state->added_state,
					  state->added) == 0);
}

static void test_consumer_callback_reentrancy(void)
{
	struct midr_ted_config config = {.max_events = 2};
	struct midr_ted *ted = NULL;
	struct midr_ted_consumer *self = NULL;
	struct midr_ted_consumer *survivor = NULL;
	struct midr_ted_consumer *added = NULL;
	struct callback_state survivor_state = {0};
	struct callback_state added_state = {0};
	struct reentrant_callback_state reentrant = {
		.self = &self,
		.added = &added,
		.added_state = &added_state,
	};
	const struct midr_ted_consumer_ops count_ops = {
		.snapshot_changed = snapshot_changed,
	};
	const struct midr_ted_consumer_ops reentrant_ops = {
		.snapshot_changed = reentrant_snapshot_changed,
	};
	struct midr_consumer_event event = node_event(1, 77, 1);
	struct midr_consumer_snapshot snapshot = {
		.generation = 1,
		.count = 1,
		.events = &event,
	};

	assert(midr_ted_create(&config, &ted) == 0);
	assert(midr_ted_consumer_register(ted, &count_ops, &survivor_state,
					  &survivor) == 0);
	assert(midr_ted_consumer_register(ted, &reentrant_ops, &reentrant,
					  &self) == 0);
	assert(midr_ted_apply_snapshot(ted, &snapshot) == 0);
	assert(reentrant.count == 1 && !self);
	assert(survivor_state.count == 1);
	assert(added && added_state.count == 0);

	snapshot.generation = 2;
	event = node_event(2, 77, 2);
	assert(midr_ted_apply_snapshot(ted, &snapshot) == 0);
	assert(reentrant.count == 1);
	assert(survivor_state.count == 2);
	assert(added_state.count == 1);
	midr_ted_consumer_unregister(ted, &survivor);
	midr_ted_consumer_unregister(ted, &added);
	midr_ted_destroy(&ted);
}

int main(void)
{
	struct midr_ted_config config = {.max_events = 8};
	struct midr_ted *ted = NULL;
	struct midr_consumer_event events[2];
	struct midr_consumer_snapshot input = {
		.generation = 1,
		.count = 2,
		.events = events,
	};
	struct midr_consumer_snapshot output = {0};
	struct midr_consumer_snapshot held = {0};
	struct midr_ted_stage *stage = NULL;
	struct midr_ted_consumer *consumer = NULL;
	struct callback_state callback = {0};
	const struct midr_ted_consumer_ops consumer_ops = {
		.snapshot_changed = snapshot_changed,
	};

	events[0] = node_event(1, 77, 1);
	events[1] = link_event(1, 77, 88, 12);
	assert(midr_ted_create(&config, &ted) == 0);
	assert(midr_ted_consumer_register(ted, &consumer_ops, &callback,
					 &consumer) == 0);
	assert(midr_ted_state(ted) == MIDR_TED_NOT_READY);
	assert(!midr_ted_generation_is_current(ted, 1));
	assert(midr_ted_snapshot_acquire(ted, &output) == -EAGAIN);
	assert(midr_ted_apply_snapshot(ted, &input) == 0);
	assert(midr_ted_state(ted) == MIDR_TED_READY);
	assert(midr_ted_generation(ted) == 1);
	assert(midr_ted_source_generation(ted) == 1);
	assert(callback.count == 1 && callback.generation == 1 &&
	       (callback.flags & MIDR_TED_CHANGE_SYNC));
	assert(midr_ted_generation_is_current(ted, 1));
	assert(midr_ted_snapshot_acquire(ted, &output) == 0);
	assert(output.count == 2 && output.events[0].remote == 88);
	midr_consumer_snapshot_release(&output);
	assert(midr_ted_snapshot_acquire(ted, &held) == 0);

	assert(midr_ted_test_fail_next(ted, -ENOMEM) == 0);
	input.generation = 2;
	events[0] = node_event(2, 77, 2);
	events[1] = link_event(2, 77, 89, 13);
	assert(midr_ted_apply_snapshot(ted, &input) == -ENOMEM);
	assert(midr_ted_state(ted) == MIDR_TED_NOT_READY);
	assert(midr_ted_generation(ted) == 2);
	assert(midr_ted_source_generation(ted) == 1);
	assert(midr_ted_last_error(ted) == -ENOMEM);
	assert(callback.count == 2 && callback.generation == 2 &&
	       callback.flags == MIDR_TED_CHANGE_SYNC);
	assert(!midr_ted_generation_is_current(ted, 2));
	assert(midr_ted_snapshot_acquire(ted, &output) == -EAGAIN);
	assert(held.generation == 1 && held.count == 2 &&
	       held.events[0].remote == 88);
	midr_consumer_snapshot_release(&held);

	assert(midr_ted_apply_snapshot(ted, &input) == 0);
	assert(midr_ted_state(ted) == MIDR_TED_READY);
	assert(midr_ted_generation(ted) == 3);
	assert(midr_ted_source_generation(ted) == 2);
	assert(callback.count == 3 && callback.generation == 3 &&
	       (callback.flags & MIDR_TED_CHANGE_SYNC));
	assert(midr_ted_snapshot_acquire(ted, &output) == 0);
	assert(output.events[1].prefix[15] == 2);
	midr_consumer_snapshot_release(&output);

	/* Reordering a repeated generation is a semantic no-op. */
	{
		struct midr_consumer_event swap = events[0];

		events[0] = events[1];
		events[1] = swap;
	}
	assert(midr_ted_apply_snapshot(ted, &input) == 0);
	assert(midr_ted_state(ted) == MIDR_TED_READY);
	assert(midr_ted_generation(ted) == 3);
	assert(callback.count == 3);

	/* A delayed older snapshot cannot replace or invalidate the view. */
	input.generation = 1;
	events[0].generation = 1;
	events[1].generation = 1;
	assert(midr_ted_apply_snapshot(ted, &input) == -ESTALE);
	assert(midr_ted_state(ted) == MIDR_TED_READY);
	assert(midr_ted_generation(ted) == 3);

	/* The same generation cannot carry different topology semantics. */
	input.generation = 2;
	events[0].generation = 2;
	events[1].generation = 2;
	events[0].metric++;
	assert(midr_ted_apply_snapshot(ted, &input) == -EPROTO);
	assert(midr_ted_state(ted) == MIDR_TED_NOT_READY);
	assert(midr_ted_generation(ted) == 4);
	assert(callback.count == 4 && callback.generation == 4 &&
	       callback.flags == MIDR_TED_CHANGE_SYNC);
	events[0].metric--;
	assert(midr_ted_apply_snapshot(ted, &input) == 0);
	assert(midr_ted_state(ted) == MIDR_TED_READY);
	assert(midr_ted_generation(ted) == 5);
	assert(callback.count == 5);

	input.generation = 3;
	events[0].generation = 3;
	events[1].generation = 3;
	assert(midr_ted_prepare_snapshot(ted, &input, &stage) == 0);
	assert(midr_ted_generation(ted) == 5);
	assert(midr_ted_source_generation(ted) == 2);
	midr_ted_abort_prepared(&stage);
	assert(!stage && midr_ted_generation(ted) == 5);

	/* A duplicate identity is rejected without publishing a partial view. */
	input.count = 2;
	events[1] = events[0];
	assert(midr_ted_apply_snapshot(ted, &input) == -EEXIST);
	assert(midr_ted_state(ted) == MIDR_TED_NOT_READY);
	assert(midr_ted_generation(ted) == 6);
	assert(callback.count == 6);
	midr_ted_consumer_unregister(ted, &consumer);
	assert(!consumer);
	midr_ted_destroy(&ted);
	test_consumer_callback_reentrancy();
	puts("midrd-ted-test: PASS");
	return 0;
}
