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

	events[0] = node_event(1, 77, 1);
	events[1] = link_event(1, 77, 88, 12);
	assert(midr_ted_create(&config, &ted) == 0);
	assert(midr_ted_state(ted) == MIDR_TED_NOT_READY);
	assert(midr_ted_snapshot_acquire(ted, &output) == -EAGAIN);
	assert(midr_ted_apply_snapshot(ted, &input) == 0);
	assert(midr_ted_state(ted) == MIDR_TED_READY);
	assert(midr_ted_generation(ted) == 1);
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
	assert(midr_ted_generation(ted) == 1);
	assert(midr_ted_last_error(ted) == -ENOMEM);
	assert(midr_ted_snapshot_acquire(ted, &output) == -EAGAIN);
	assert(held.generation == 1 && held.count == 2 &&
	       held.events[0].remote == 88);
	midr_consumer_snapshot_release(&held);

	assert(midr_ted_apply_snapshot(ted, &input) == 0);
	assert(midr_ted_state(ted) == MIDR_TED_READY);
	assert(midr_ted_generation(ted) == 2);
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
	assert(midr_ted_generation(ted) == 2);

	/* A delayed older snapshot cannot replace or invalidate the view. */
	input.generation = 1;
	events[0].generation = 1;
	events[1].generation = 1;
	assert(midr_ted_apply_snapshot(ted, &input) == -ESTALE);
	assert(midr_ted_state(ted) == MIDR_TED_READY);
	assert(midr_ted_generation(ted) == 2);

	/* The same generation cannot carry different topology semantics. */
	input.generation = 2;
	events[0].generation = 2;
	events[1].generation = 2;
	events[0].metric++;
	assert(midr_ted_apply_snapshot(ted, &input) == -EPROTO);
	assert(midr_ted_state(ted) == MIDR_TED_NOT_READY);
	assert(midr_ted_generation(ted) == 2);
	events[0].metric--;
	assert(midr_ted_apply_snapshot(ted, &input) == 0);
	assert(midr_ted_state(ted) == MIDR_TED_READY);

	/* A duplicate identity is rejected without publishing a partial view. */
	input.generation = 3;
	events[0].generation = 3;
	events[1].generation = 3;
	input.count = 2;
	events[1] = events[0];
	assert(midr_ted_apply_snapshot(ted, &input) == -EEXIST);
	assert(midr_ted_state(ted) == MIDR_TED_NOT_READY);
	assert(midr_ted_generation(ted) == 2);
	midr_ted_destroy(&ted);
	puts("midrd-ted-test: PASS");
	return 0;
}
