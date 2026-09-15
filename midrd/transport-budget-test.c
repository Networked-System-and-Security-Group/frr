/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static unsigned int mock_send_calls;

static ssize_t mock_send(int fd, const void *buffer, size_t length, int flags)
{
	(void)fd;
	(void)buffer;
	(void)flags;
	if (mock_send_calls++ == 0)
		return length > 1U ? 1 : (ssize_t)length;
	if (mock_send_calls == 2) {
		errno = EAGAIN;
		return -1;
	}
	return (ssize_t)length;
}

#define send mock_send
#include "midr-transport.c"
#undef send

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct budget_clock {
	uint64_t now_ns;
};

struct close_state {
	unsigned int count;
	int reason;
};

static uint64_t budget_now(void *arg)
{
	return ((struct budget_clock *)arg)->now_ns;
}

static void record_close(void *arg,
			 const struct midr_transport_endpoint *endpoint,
			 int reason)
{
	struct close_state *state = arg;

	assert(endpoint);
	state->count++;
	state->reason = reason;
}

static void initialize(struct midr_transport *transport,
		       struct midr_transport_peer *peer,
		       struct budget_clock *clock,
		       struct close_state *closed)
{
	memset(transport, 0, sizeof(*transport));
	memset(peer, 0, sizeof(*peer));
	transport->config.tx_budget_ms = 1000;
	transport->config.now_ns = budget_now;
	transport->config.clock_arg = clock;
	transport->callbacks.on_closed = record_close;
	transport->callbacks.arg = closed;
	transport->listen_fd = -1;
	peer->endpoint.family = MIDR_TRANSPORT_AF_IPV4;
	peer->endpoint.port = 39001;
	peer->endpoint.address[0] = 127;
	peer->endpoint.address[3] = 1;
	peer->fd = open("/dev/null", O_RDWR);
	assert(peer->fd >= 0);
	peer->used = true;
	peer->established = true;
}

static void queue_partial(struct midr_transport *transport,
			  struct midr_transport_peer *peer,
			  uint64_t encoded_ns)
{
	static const uint8_t bytes[] = {1, 2, 3, 4};

	mock_send_calls = 0;
	assert(queue_frame(peer, bytes, sizeof(bytes), encoded_ns) == 0);
	assert(flush_peer(transport, peer) == 0);
	assert(peer->tx_head && peer->tx_head->offset == 1U);
}

int main(void)
{
	struct midr_transport transport;
	struct midr_transport_peer peer;
	struct budget_clock clock = {0};
	struct close_state closed = {0};

	initialize(&transport, &peer, &clock, &closed);
	queue_partial(&transport, &peer, 0);
	clock.now_ns = 1000000000U;
	assert(flush_peer(&transport, &peer) == 0);
	assert(!peer.tx_head && peer.fd >= 0 && closed.count == 0);
	close(peer.fd);

	memset(&closed, 0, sizeof(closed));
	clock.now_ns = 0;
	initialize(&transport, &peer, &clock, &closed);
	queue_partial(&transport, &peer, 0);
	clock.now_ns = 1000000001U;
	assert(flush_peer(&transport, &peer) == -ETIMEDOUT);
	assert(!peer.tx_head && peer.fd == -1);
	assert(closed.count == 1 && closed.reason == -ETIMEDOUT);

	memset(&closed, 0, sizeof(closed));
	clock.now_ns = 99;
	initialize(&transport, &peer, &clock, &closed);
	assert(queue_frame(&peer, (const uint8_t *)"x", 1, 100) == 0);
	assert(flush_peer(&transport, &peer) == -ERANGE);
	assert(!peer.tx_head && peer.fd == -1);
	assert(closed.count == 1 && closed.reason == -ERANGE);

	puts("midrd-transport-budget-test: PASS");
	return 0;
}
