#include "midr-transport.h"
#include "midr-wire.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct callback_state {
	struct midr_transport *transport;
	unsigned int established;
	unsigned int closed;
	unsigned int frames;
	unsigned int written;
	uint64_t last_written_sequence;
	uint64_t last_written_generation;
	size_t payload_bytes;
	uint64_t received_ns;
	uint64_t received_generation;
	bool disconnect_on_frame;
};

struct test_clock {
	uint64_t now_ns;
	uint64_t advance_after_read_ns;
};

static uint64_t clock_now(void *arg)
{
	struct test_clock *clock = arg;
	uint64_t now = clock->now_ns;

	clock->now_ns += clock->advance_after_read_ns;
	clock->advance_after_read_ns = 0;
	return now;
}

static void on_established(void *arg,
			   const struct midr_transport_endpoint *peer)
{
	struct callback_state *state = arg;

	assert(peer && (peer->family == MIDR_TRANSPORT_AF_IPV4 ||
			peer->family == MIDR_TRANSPORT_AF_IPV6));
	state->established++;
}

static void on_closed(void *arg, const struct midr_transport_endpoint *peer,
		      int reason)
{
	struct callback_state *state = arg;

	assert(peer && reason <= 0);
	state->closed++;
}

static int on_frame(void *arg, const struct midr_transport_endpoint *peer,
			const struct midr_transport_frame *frame)
{
	struct callback_state *state = arg;

	assert(peer && (peer->family == MIDR_TRANSPORT_AF_IPV4 ||
			peer->family == MIDR_TRANSPORT_AF_IPV6));
	assert(frame && frame->type == MIDR_FRAME_KEEPALIVE);
	state->frames++;
	state->payload_bytes += frame->payload_len;
	state->received_ns = frame->received_ns;
	state->received_generation = frame->generation;
	if (state->disconnect_on_frame) {
		state->disconnect_on_frame = false;
		assert(state->transport);
		assert(midr_transport_disconnect(state->transport, peer) == 0);
	}
	return 0;
}

static void on_frame_written(void *arg,
			     const struct midr_transport_endpoint *peer,
			     uint64_t generation, uint64_t sequence)
{
	struct callback_state *state = arg;

	assert(peer && (peer->family == MIDR_TRANSPORT_AF_IPV4 ||
			peer->family == MIDR_TRANSPORT_AF_IPV6));
	state->written++;
	state->last_written_generation = generation;
	state->last_written_sequence = sequence;
}

static struct midr_transport_callbacks callbacks(struct callback_state *state)
{
	return (struct midr_transport_callbacks){
		.on_frame = on_frame,
		.on_established = on_established,
		.on_closed = on_closed,
		.on_frame_written = on_frame_written,
		.arg = state,
	};
}

static void poll_pair(struct midr_transport *left,
			      struct midr_transport *right)
{
	assert(midr_transport_poll(left, 20) >= 0);
	assert(midr_transport_poll(right, 20) >= 0);
}

static void test_simultaneous_connect(void)
{
	struct callback_state left_state = {0}, right_state = {0};
	struct midr_transport *left = NULL, *right = NULL;
	struct midr_transport_endpoint left_endpoint = {
		.family = MIDR_TRANSPORT_AF_IPV4,
		.port = 38993,
		.address = {127, 0, 0, 1},
	};
	struct midr_transport_endpoint right_endpoint = {
		.family = MIDR_TRANSPORT_AF_IPV4,
		.port = 38994,
		.address = {127, 0, 0, 1},
	};
	struct midr_transport_config left_config = {
		.local = left_endpoint,
		.hold_time_ms = 2000,
		.tx_budget_ms = 1000,
		.max_frame_size = 4096,
	};
	struct midr_transport_config right_config = {
		.local = right_endpoint,
		.hold_time_ms = 2000,
		.tx_budget_ms = 1000,
		.max_frame_size = 4096,
	};
	struct midr_transport_frame frame = {
		.version = MIDR_WIRE_VERSION,
		.type = MIDR_FRAME_KEEPALIVE,
		.sequence = 1,
	};
	struct midr_transport_callbacks left_callbacks = callbacks(&left_state);
	struct midr_transport_callbacks right_callbacks = callbacks(&right_state);
	unsigned int left_established;
	unsigned int right_established;

	assert(midr_transport_create(&left_config, &left_callbacks, &left) == 0);
	assert(midr_transport_create(&right_config, &right_callbacks, &right) == 0);
	assert(midr_transport_start(left) == 0);
	assert(midr_transport_start(right) == 0);
	assert(midr_transport_connect(left, &right_endpoint) == 0);
	assert(midr_transport_connect(right, &left_endpoint) == 0);
	for (unsigned int i = 0;
	     i < 100 && (!left_state.established || !right_state.established);
	     i++)
		poll_pair(left, right);
	assert(left_state.established && right_state.established);
	assert(midr_transport_peer_count(left) == 1);
	assert(midr_transport_peer_count(right) == 1);
	for (unsigned int i = 0; i < 10; i++)
		poll_pair(left, right);
	left_established = left_state.established;
	right_established = right_state.established;
	for (unsigned int i = 0; i < 10; i++)
		poll_pair(left, right);
	assert(left_state.established == left_established);
	assert(right_state.established == right_established);
	assert(midr_transport_send(left, &right_endpoint, &frame) == 0);
	frame.sequence = 2;
	assert(midr_transport_send(right, &left_endpoint, &frame) == 0);
	for (unsigned int i = 0;
	     i < 100 && (!left_state.frames || !right_state.frames); i++)
		poll_pair(left, right);
	assert(left_state.frames == 1 && right_state.frames == 1);
	for (unsigned int i = 0; i < 20; i++)
		poll_pair(left, right);
	assert(left_state.established == left_established);
	assert(right_state.established == right_established);
	midr_transport_destroy(&left);
	midr_transport_destroy(&right);
}

static void test_callback_disconnect(void)
{
	struct callback_state left_state = {0}, right_state = {0};
	struct midr_transport *left = NULL, *right = NULL;
	struct midr_transport_endpoint right_endpoint = {
		.family = MIDR_TRANSPORT_AF_IPV4,
		.port = 38996,
		.address = {127, 0, 0, 1},
	};
	struct midr_transport_config left_config = {
		.local = {
			.family = MIDR_TRANSPORT_AF_IPV4,
			.port = 38995,
			.address = {127, 0, 0, 1},
		},
		.hold_time_ms = 2000,
		.tx_budget_ms = 1000,
		.max_frame_size = 4096,
	};
	struct midr_transport_config right_config = {
		.local = right_endpoint,
		.hold_time_ms = 2000,
		.tx_budget_ms = 1000,
		.max_frame_size = 4096,
	};
	struct midr_transport_frame frame = {
		.version = MIDR_WIRE_VERSION,
		.type = MIDR_FRAME_KEEPALIVE,
		.sequence = 1,
	};
	struct midr_transport_callbacks left_callbacks = callbacks(&left_state);
	struct midr_transport_callbacks right_callbacks = callbacks(&right_state);

	assert(midr_transport_create(&left_config, &left_callbacks, &left) == 0);
	assert(midr_transport_create(&right_config, &right_callbacks, &right) == 0);
	left_state.transport = left;
	right_state.transport = right;
	assert(midr_transport_start(left) == 0);
	assert(midr_transport_start(right) == 0);
	assert(midr_transport_connect(left, &right_endpoint) == 0);
	for (unsigned int i = 0;
	     i < 50 && (!left_state.established || !right_state.established); i++)
		poll_pair(left, right);
	assert(left_state.established == 1 && right_state.established == 1);
	right_state.disconnect_on_frame = true;
	assert(midr_transport_send(left, &right_endpoint, &frame) == 0);
	assert(midr_transport_poll(right, 20) >= 0);
	assert(right_state.frames == 1);
	assert(right_state.closed == 1);
	midr_transport_destroy(&left);
	midr_transport_destroy(&right);
}

static void test_ipv6(void)
{
	struct callback_state left_state = {0}, right_state = {0};
	struct midr_transport *left = NULL, *right = NULL;
	struct midr_transport_endpoint left_endpoint = {
		.family = MIDR_TRANSPORT_AF_IPV6,
		.port = 38997,
		.address = {[15] = 1},
	};
	struct midr_transport_endpoint right_endpoint = {
		.family = MIDR_TRANSPORT_AF_IPV6,
		.port = 38998,
		.address = {[15] = 1},
	};
	struct midr_transport_config left_config = {
		.local = left_endpoint,
		.hold_time_ms = 2000,
		.tx_budget_ms = 1000,
		.max_frame_size = 4096,
	};
	struct midr_transport_config right_config = {
		.local = right_endpoint,
		.hold_time_ms = 2000,
		.tx_budget_ms = 1000,
		.max_frame_size = 4096,
	};
	struct midr_transport_frame frame = {
		.version = MIDR_WIRE_VERSION,
		.type = MIDR_FRAME_KEEPALIVE,
		.sequence = 11,
	};
	struct midr_transport_callbacks left_callbacks = callbacks(&left_state);
	struct midr_transport_callbacks right_callbacks = callbacks(&right_state);

	assert(midr_transport_create(&left_config, &left_callbacks, &left) == 0);
	assert(midr_transport_create(&right_config, &right_callbacks, &right) == 0);
	assert(midr_transport_start(left) == 0);
	assert(midr_transport_start(right) == 0);
	assert(midr_transport_connect(left, &right_endpoint) == 0);
	for (unsigned int i = 0;
	     i < 100 && (!left_state.established || !right_state.established);
	     i++)
		poll_pair(left, right);
	assert(left_state.established == 1 && right_state.established == 1);
	assert(midr_transport_send(left, &right_endpoint, &frame) == 0);
	for (unsigned int i = 0; i < 100 && !right_state.frames; i++)
		poll_pair(left, right);
	assert(right_state.frames == 1 && left_state.written == 1);
	midr_transport_destroy(&left);
	midr_transport_destroy(&right);
}

static void test_hold_timer(void)
{
	struct callback_state left_state = {0}, right_state = {0};
	struct midr_transport *left = NULL, *right = NULL;
	struct midr_transport_endpoint right_endpoint = {
		.family = MIDR_TRANSPORT_AF_IPV4,
		.port = 39000,
		.address = {127, 0, 0, 1},
	};
	struct midr_transport_config left_config = {
		.local = {
			.family = MIDR_TRANSPORT_AF_IPV4,
			.port = 38999,
			.address = {127, 0, 0, 1},
		},
		.hold_time_ms = 40,
		.tx_budget_ms = 1000,
		.max_frame_size = 4096,
	};
	struct midr_transport_config right_config = {
		.local = right_endpoint,
		.hold_time_ms = 40,
		.tx_budget_ms = 1000,
		.max_frame_size = 4096,
	};
	struct midr_transport_callbacks left_callbacks = callbacks(&left_state);
	struct midr_transport_callbacks right_callbacks = callbacks(&right_state);

	assert(midr_transport_create(&left_config, &left_callbacks, &left) == 0);
	assert(midr_transport_create(&right_config, &right_callbacks, &right) == 0);
	assert(midr_transport_start(left) == 0);
	assert(midr_transport_start(right) == 0);
	assert(midr_transport_connect(left, &right_endpoint) == 0);
	for (unsigned int i = 0;
	     i < 100 && (!left_state.established || !right_state.established);
	     i++)
		poll_pair(left, right);
	assert(left_state.established && right_state.established);
	for (unsigned int i = 0;
	     i < 100 && (!left_state.closed || !right_state.closed); i++)
		poll_pair(left, right);
	assert(left_state.closed && right_state.closed);
	midr_transport_destroy(&left);
	midr_transport_destroy(&right);
}

int main(void)
{
	struct callback_state left_state = {0}, right_state = {0};
	struct test_clock left_clock = {.now_ns = 1000000000U};
	struct test_clock right_clock = {.now_ns = 2000000000U};
	struct midr_transport *left = NULL, *right = NULL;
	struct midr_transport_endpoint right_endpoint = {
		.family = MIDR_TRANSPORT_AF_IPV4,
		.port = 38992,
		.address = {127, 0, 0, 1},
	};
	struct midr_transport_config left_config = {
		.local = {
			.family = MIDR_TRANSPORT_AF_IPV4,
			.port = 38991,
			.address = {127, 0, 0, 1},
		},
		.hold_time_ms = 2000,
		.tx_budget_ms = 1000,
		.max_frame_size = 4096,
		.now_ns = clock_now,
		.clock_arg = &left_clock,
	};
	struct midr_transport_config right_config = {
		.local = {
			.family = MIDR_TRANSPORT_AF_IPV4,
			.port = 38992,
			.address = {127, 0, 0, 1},
		},
		.hold_time_ms = 2000,
		.tx_budget_ms = 1000,
		.max_frame_size = 4096,
		.now_ns = clock_now,
		.clock_arg = &right_clock,
	};
	struct midr_transport_frame frame = {
		.version = MIDR_WIRE_VERSION,
		.type = MIDR_FRAME_KEEPALIVE,
		.sequence = 1,
	};
	struct midr_transport_callbacks left_callbacks = callbacks(&left_state);
	struct midr_transport_callbacks right_callbacks = callbacks(&right_state);
	uint8_t payload[3000];
	uint64_t old_generation;

	memset(payload, 0xa5, sizeof(payload));
	frame.payload = payload;
	frame.payload_len = sizeof(payload);
	assert(midr_transport_create(&left_config, &left_callbacks, &left) == 0);
	assert(midr_transport_create(&right_config, &right_callbacks, &right) == 0);
	assert(midr_transport_start(left) == 0);
	assert(midr_transport_start(right) == 0);
	assert(midr_transport_connect(left, &right_endpoint) == 0);
	for (unsigned int i = 0; i < 50 && !right_state.established; i++)
		poll_pair(left, right);
	assert(left_state.established == 1 && right_state.established == 1);
	assert(midr_transport_peer_count(left) == 1);
	assert(midr_transport_peer_count(right) == 1);
	assert(midr_transport_send(left, &right_endpoint, &frame) == 0);
	assert(midr_transport_pending(left) == 0);
	assert(left_state.written == 1 && left_state.last_written_sequence == 1);
	assert(left_state.last_written_generation != 0);
	for (unsigned int i = 0; i < 50 && !right_state.frames; i++)
		poll_pair(left, right);
	assert(right_state.frames == 1);
	assert(right_state.payload_bytes == sizeof(payload));
	assert(right_state.received_ns == right_clock.now_ns);
	assert(right_state.received_generation != 0);
	/* Several frames exercise stream coalescing and parser boundaries. */
	frame.payload = NULL;
	frame.payload_len = 0;
	for (unsigned int i = 0; i < 16; i++)
		assert(midr_transport_send(left, &right_endpoint, &frame) == 0);
	for (unsigned int i = 0; i < 50 && right_state.frames < 17; i++)
		poll_pair(left, right);
	assert(right_state.frames == 17);
	assert(left_state.written == 17);
	/* A frame may be written at exactly B, but not one nanosecond later. */
	left_clock.advance_after_read_ns = 1000000000U;
	assert(midr_transport_send(left, &right_endpoint, &frame) == 0);
	for (unsigned int i = 0; i < 50 && right_state.frames < 18; i++)
		poll_pair(left, right);
	assert(right_state.frames == 18);
	assert(left_state.written == 18);
	old_generation = left_state.last_written_generation;
	assert(midr_transport_disconnect(left, &right_endpoint) == 0);
	assert(left_state.closed == 1);
	/* A configured peer must reconnect to its destination port after an
	 * accepted duplicate used an ephemeral source port. */
	assert(midr_transport_poll(right, 20) >= 0);
	assert(midr_transport_connect(left, &right_endpoint) == 0);
	for (unsigned int i = 0;
	     i < 100 && (left_state.established < 2 || right_state.established < 2);
	     i++)
		poll_pair(left, right);
	assert(left_state.established == 2 && right_state.established == 2);
	/* A completion from the new stream carries a new generation. */
	left_clock.advance_after_read_ns = 0;
	frame.sequence = 2;
	assert(midr_transport_send(left, &right_endpoint, &frame) == 0);
	assert(left_state.written == 19 &&
	       left_state.last_written_generation != old_generation);
	left_clock.advance_after_read_ns = 1000000001U;
	frame.sequence = 3;
	assert(midr_transport_send(left, &right_endpoint, &frame) == -ETIMEDOUT);
	assert(left_state.closed == 2);
	/* B+1 closes the stale byte stream, but the desired peer remains and
	 * must reconnect through the normal snapshot/resync entry point. */
	left_clock.now_ns += 1000000000U;
	for (unsigned int i = 0;
	     i < 100 && (left_state.established < 3 || right_state.established < 3);
	     i++)
		poll_pair(left, right);
	assert(left_state.established == 3 && right_state.established == 3);
	midr_transport_destroy(&left);
	midr_transport_destroy(&right);
	test_simultaneous_connect();
	test_callback_disconnect();
	test_ipv6();
	test_hold_timer();
	puts("midrd-transport-test: PASS");
	return 0;
}
