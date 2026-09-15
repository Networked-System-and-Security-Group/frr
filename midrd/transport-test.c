#include "midr-transport.h"
#include "midr-wire.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct callback_state {
	unsigned int established;
	unsigned int closed;
	unsigned int frames;
	size_t payload_bytes;
};

static void on_established(void *arg,
			   const struct midr_transport_endpoint *peer)
{
	struct callback_state *state = arg;

	assert(peer && peer->family == MIDR_TRANSPORT_AF_IPV4);
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

	assert(peer && peer->family == MIDR_TRANSPORT_AF_IPV4);
	assert(frame && frame->type == MIDR_FRAME_KEEPALIVE);
	state->frames++;
	state->payload_bytes += frame->payload_len;
	return 0;
}

static struct midr_transport_callbacks callbacks(struct callback_state *state)
{
	return (struct midr_transport_callbacks){
		.on_frame = on_frame,
		.on_established = on_established,
		.on_closed = on_closed,
		.arg = state,
	};
}

static void poll_pair(struct midr_transport *left,
			      struct midr_transport *right)
{
	assert(midr_transport_poll(left, 20) >= 0);
	assert(midr_transport_poll(right, 20) >= 0);
}

int main(void)
{
	struct callback_state left_state = {0}, right_state = {0};
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
		.max_frame_size = 4096,
	};
	struct midr_transport_config right_config = {
		.local = {
			.family = MIDR_TRANSPORT_AF_IPV4,
			.port = 38992,
			.address = {127, 0, 0, 1},
		},
		.hold_time_ms = 2000,
		.max_frame_size = 4096,
	};
	struct midr_transport_frame frame = {
		.version = MIDR_WIRE_VERSION,
		.type = MIDR_FRAME_KEEPALIVE,
		.sequence = 1,
	};
	struct midr_transport_callbacks left_callbacks = callbacks(&left_state);
	struct midr_transport_callbacks right_callbacks = callbacks(&right_state);
	uint8_t payload[3000];

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
	for (unsigned int i = 0; i < 50 && !right_state.frames; i++)
		poll_pair(left, right);
	assert(right_state.frames == 1);
	assert(right_state.payload_bytes == sizeof(payload));
	/* Several frames exercise stream coalescing and parser boundaries. */
	frame.payload = NULL;
	frame.payload_len = 0;
	for (unsigned int i = 0; i < 16; i++)
		assert(midr_transport_send(left, &right_endpoint, &frame) == 0);
	for (unsigned int i = 0; i < 50 && right_state.frames < 17; i++)
		poll_pair(left, right);
	assert(right_state.frames == 17);
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
	midr_transport_destroy(&left);
	midr_transport_destroy(&right);
	puts("midrd-transport-test: PASS");
	return 0;
}
