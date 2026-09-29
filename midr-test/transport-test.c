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
	/* Endpoint reported by the most recent on_established().  For an
	 * accepted stream that the transport could not attribute to a
	 * configured peer this is the kernel-chosen source endpoint. */
	struct midr_transport_endpoint last_peer;
	bool have_peer;
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
	state->last_peer = *peer;
	state->have_peer = true;
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

/* Two configured peers share one address, so the kernel-chosen source of an
 * accepted stream cannot identify which one dialled in.  The transport must
 * not guess: the stream stays unbound until its identity is supplied through
 * midr_transport_promote().  Before the fix the inbound was bound to the
 * first address match (peer1) and the identity was silently wrong. */
static void test_shared_address_promote(void)
{
	struct callback_state server_state = {0}, client_state = {0};
	struct midr_transport *server = NULL, *client = NULL;
	struct midr_transport_endpoint server_endpoint = {
		.family = MIDR_TRANSPORT_AF_IPV4,
		.port = 39101,
		.address = {127, 0, 0, 1},
	};
	struct midr_transport_endpoint peer1_endpoint = {
		.family = MIDR_TRANSPORT_AF_IPV4,
		.port = 39102,
		.address = {127, 0, 0, 1},
	};
	struct midr_transport_endpoint peer2_endpoint = {
		.family = MIDR_TRANSPORT_AF_IPV4,
		.port = 39103,
		.address = {127, 0, 0, 1},
	};
	struct midr_transport_config server_config = {
		.local = server_endpoint,
		.hold_time_ms = 2000,
		.tx_budget_ms = 1000,
		.max_frame_size = 4096,
	};
	struct midr_transport_config client_config = {
		.local = {
			.family = MIDR_TRANSPORT_AF_IPV4,
			.port = 39104,
			.address = {127, 0, 0, 1},
		},
		.hold_time_ms = 2000,
		.tx_budget_ms = 1000,
		.max_frame_size = 4096,
	};
	struct midr_transport_frame frame = {
		.version = MIDR_WIRE_VERSION,
		.type = MIDR_FRAME_KEEPALIVE,
		.sequence = 1,
	};
	struct midr_transport_callbacks server_callbacks =
		callbacks(&server_state);
	struct midr_transport_callbacks client_callbacks =
		callbacks(&client_state);

	assert(midr_transport_create(&server_config, &server_callbacks,
				     &server) == 0);
	assert(midr_transport_create(&client_config, &client_callbacks,
				     &client) == 0);
	assert(midr_transport_start(server) == 0);
	assert(midr_transport_start(client) == 0);
	assert(midr_transport_connect(server, &peer1_endpoint) == 0);
	assert(midr_transport_connect(server, &peer2_endpoint) == 0);
	/* The third node dials in from 127.0.0.1, which matches both peers. */
	assert(midr_transport_connect(client, &server_endpoint) == 0);
	for (unsigned int i = 0; i < 100 && !server_state.established; i++)
		poll_pair(server, client);
	assert(server_state.established == 1);
	assert(server_state.have_peer);
	/* The stream was left unbound: neither configured peer may claim it. */
	assert(midr_transport_send(server, &peer1_endpoint, &frame) ==
	       -ENOTCONN);
	assert(midr_transport_send(server, &peer2_endpoint, &frame) ==
	       -ENOTCONN);
	/* HELLO identified the stream as peer2; re-key it and use it. */
	assert(midr_transport_promote(server, &server_state.last_peer,
				      &peer2_endpoint) == 0);
	assert(server_state.established == 2);
	assert(midr_transport_send(server, &peer1_endpoint, &frame) ==
	       -ENOTCONN);
	frame.sequence = 2;
	assert(midr_transport_send(server, &peer2_endpoint, &frame) == 0);
	for (unsigned int i = 0; i < 50 && !client_state.frames; i++)
		poll_pair(server, client);
	assert(client_state.frames == 1);
	midr_transport_destroy(&server);
	midr_transport_destroy(&client);
}

/* A configured peer must always have an outbound connect initiated, even when
 * several peers share one address. */
static void test_outbound_connect_shared_address(void)
{
	struct callback_state server_state = {0};
	struct callback_state client1_state = {0}, client2_state = {0};
	struct midr_transport *server = NULL, *client1 = NULL, *client2 = NULL;
	struct midr_transport_endpoint peer1_endpoint = {
		.family = MIDR_TRANSPORT_AF_IPV4,
		.port = 39112,
		.address = {127, 0, 0, 1},
	};
	struct midr_transport_endpoint peer2_endpoint = {
		.family = MIDR_TRANSPORT_AF_IPV4,
		.port = 39113,
		.address = {127, 0, 0, 1},
	};
	struct midr_transport_config server_config = {
		.local = {
			.family = MIDR_TRANSPORT_AF_IPV4,
			.port = 39111,
			.address = {127, 0, 0, 1},
		},
		.hold_time_ms = 2000,
		.tx_budget_ms = 1000,
		.max_frame_size = 4096,
	};
	struct midr_transport_config client1_config = {
		.local = peer1_endpoint,
		.hold_time_ms = 2000,
		.tx_budget_ms = 1000,
		.max_frame_size = 4096,
	};
	struct midr_transport_config client2_config = {
		.local = peer2_endpoint,
		.hold_time_ms = 2000,
		.tx_budget_ms = 1000,
		.max_frame_size = 4096,
	};
	struct midr_transport_frame frame = {
		.version = MIDR_WIRE_VERSION,
		.type = MIDR_FRAME_KEEPALIVE,
		.sequence = 1,
	};
	struct midr_transport_callbacks server_callbacks =
		callbacks(&server_state);
	struct midr_transport_callbacks client1_callbacks =
		callbacks(&client1_state);
	struct midr_transport_callbacks client2_callbacks =
		callbacks(&client2_state);

	assert(midr_transport_create(&server_config, &server_callbacks,
				     &server) == 0);
	assert(midr_transport_create(&client1_config, &client1_callbacks,
				     &client1) == 0);
	assert(midr_transport_create(&client2_config, &client2_callbacks,
				     &client2) == 0);
	assert(midr_transport_start(server) == 0);
	assert(midr_transport_start(client1) == 0);
	assert(midr_transport_start(client2) == 0);
	assert(midr_transport_connect(server, &peer1_endpoint) == 0);
	assert(midr_transport_connect(server, &peer2_endpoint) == 0);
	for (unsigned int i = 0; i < 100 && server_state.established < 2; i++) {
		poll_pair(server, client1);
		poll_pair(server, client2);
	}
	assert(server_state.established == 2);
	assert(midr_transport_peer_count(server) == 2);
	assert(midr_transport_send(server, &peer1_endpoint, &frame) == 0);
	frame.sequence = 2;
	assert(midr_transport_send(server, &peer2_endpoint, &frame) == 0);
	for (unsigned int i = 0;
	     i < 50 && (!client1_state.frames || !client2_state.frames); i++) {
		poll_pair(server, client1);
		poll_pair(server, client2);
	}
	assert(client1_state.frames == 1 && client2_state.frames == 1);
	midr_transport_destroy(&server);
	midr_transport_destroy(&client1);
	midr_transport_destroy(&client2);
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
	test_shared_address_promote();
	test_outbound_connect_shared_address();
	puts("midrd-transport-test: PASS");
	return 0;
}
