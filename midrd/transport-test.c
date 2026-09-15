#include "midr-transport.h"
#include "midr-wire.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct callback_state {
	unsigned int frames;
};

static int on_frame(void *arg, const struct midr_transport_endpoint *peer,
			const struct midr_transport_frame *frame)
{
	struct callback_state *state = arg;

	assert(peer->family == MIDR_TRANSPORT_AF_IPV4);
	assert(frame->type == MIDR_FRAME_KEEPALIVE);
	state->frames++;
	return 0;
}

int main(void)
{
	struct callback_state state = {0};
	struct midr_transport *transport = NULL;
	struct midr_transport_endpoint endpoint = {
		.family = MIDR_TRANSPORT_AF_IPV4,
		.port = 38991,
		.address = {127, 0, 0, 1},
	};
	struct midr_transport_config config = {
		.local = {
			.family = MIDR_TRANSPORT_AF_IPV4,
			.port = 38991,
			.address = {127, 0, 0, 1},
		},
		.max_frame_size = 2048,
	};
	struct midr_transport_callbacks callbacks = {
		.on_frame = on_frame,
		.arg = &state,
	};
	struct midr_transport_frame frame = {
		.version = MIDR_WIRE_VERSION,
		.type = MIDR_FRAME_KEEPALIVE,
		.sequence = 1,
	};

	assert(midr_transport_create(&config, &callbacks, &transport) == 0);
	assert(midr_transport_start(transport) == 0);
	assert(midr_transport_connect(transport, &endpoint) == 0);
	assert(midr_transport_send(transport, &endpoint, &frame) == 0);
	assert(midr_transport_poll(transport, 1000) == 0);
	assert(state.frames == 1);
	midr_transport_destroy(&transport);
	puts("midrd-transport-test: PASS");
	return 0;
}
