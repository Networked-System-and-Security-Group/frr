/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L

#include "midr-transport.c"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct budget_clock {
	uint64_t now_ns;
	uint64_t advance_after_read_ns;
};

struct close_state {
	unsigned int count;
	int reason;
	unsigned int dropped;
	uint64_t dropped_generation;
	uint64_t dropped_sequence;
};

static uint64_t budget_now(void *arg)
{
	struct budget_clock *clock = arg;
	uint64_t now = clock->now_ns;

	clock->now_ns += clock->advance_after_read_ns;
	clock->advance_after_read_ns = 0;
	return now;
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

static void record_drop(void *arg,
			const struct midr_transport_endpoint *endpoint,
			uint64_t generation, uint64_t sequence, int reason)
{
	struct close_state *state = arg;

	assert(endpoint && reason < 0);
	state->dropped++;
	state->dropped_generation = generation;
	state->dropped_sequence = sequence;
}

static int initialize(struct midr_transport *transport,
		      struct midr_transport_peer *peer,
		      struct budget_clock *clock,
		      struct close_state *closed)
{
	int sockets[2];

	memset(transport, 0, sizeof(*transport));
	memset(peer, 0, sizeof(*peer));
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
	assert(transport_set_nonblocking(sockets[0]) == 0);
	assert(transport_set_nonblocking(sockets[1]) == 0);
	transport->config.tx_budget_ms = 1000;
	transport->config.max_frame_size = 65535;
	transport->config.now_ns = budget_now;
	transport->config.clock_arg = clock;
	transport->callbacks.on_closed = record_close;
	transport->callbacks.on_frame_dropped = record_drop;
	transport->callbacks.arg = closed;
	transport->listen_fd = -1;
	transport->master = event_master_create("midr-budget-test");
	transport->owns_master = true;
	transport->started = true;
	peer->transport = transport;
	peer->endpoint.family = MIDR_TRANSPORT_AF_IPV4;
	peer->endpoint.port = 39001;
	peer->endpoint.address[0] = 127;
	peer->endpoint.address[3] = 1;
	peer->fd = sockets[0];
	peer->used = true;
	peer->established = true;
	return sockets[1];
}

static void cleanup(struct midr_transport *transport,
		    struct midr_transport_peer *peer, int receive_fd)
{
	if (peer->used) {
		peer->desired = false;
		close_peer(transport, peer, 0);
	}
	close(receive_fd);
	event_master_free(transport->master);
	transport->master = NULL;
}

static void fill_send_buffer(int fd)
{
	uint8_t bytes[4096] = {0};

	while (write(fd, bytes, sizeof(bytes)) > 0)
		;
	assert(errno == EAGAIN || errno == EWOULDBLOCK);
}

static void drain_receive_buffer(int fd)
{
	uint8_t bytes[4096];

	while (read(fd, bytes, sizeof(bytes)) > 0)
		;
	assert(errno == EAGAIN || errno == EWOULDBLOCK);
}

static void queue_blocked(struct midr_transport *transport,
			  struct midr_transport_peer *peer,
			  uint64_t encoded_ns, uint64_t sequence)
{
	static const uint8_t bytes[4096] = {1, 2, 3, 4};

	fill_send_buffer(peer->fd);
	assert(queue_frame_with_sequence(peer, bytes, sizeof(bytes), encoded_ns,
					 sequence) == 0);
	assert(flush_peer(transport, peer) == 0);
	assert(peer->tx_head);
}

static void test_connecting_frame_generation(void)
{
	struct midr_transport transport;
	struct midr_transport_peer peer;
	struct budget_clock clock = {0};
	struct close_state closed = {0};
	int receive_fd = initialize(&transport, &peer, &clock, &closed);

	peer.established = false;
	peer.connecting = true;
	peer.generation = 0;
	assert(queue_frame_with_sequence(&peer, (const uint8_t *)"queued", 6,
					 0, 7) == 0);
	assert(peer.tx_head->generation == 0);
	establish_peer(&transport, &peer);
	assert(peer.generation == 1);
	assert(peer.tx_head && peer.tx_head->generation == peer.generation);
	close_peer(&transport, &peer, -ECONNRESET);
	close(receive_fd);
	event_master_free(transport.master);
}

int main(void)
{
	struct midr_transport transport;
	struct midr_transport_peer peer;
	struct budget_clock clock = {0};
	struct close_state closed = {0};
	int receive_fd;

	test_connecting_frame_generation();
	receive_fd = initialize(&transport, &peer, &clock, &closed);
	queue_blocked(&transport, &peer, 0, 1);
	clock.now_ns = 1000000000U;
	drain_receive_buffer(receive_fd);
	assert(flush_peer(&transport, &peer) == 0);
	assert(!peer.tx_head && peer.fd >= 0 && closed.count == 0);
	cleanup(&transport, &peer, receive_fd);

	memset(&closed, 0, sizeof(closed));
	clock.now_ns = 0;
	receive_fd = initialize(&transport, &peer, &clock, &closed);
	queue_blocked(&transport, &peer, 0, 1);
	clock.now_ns = 1000000001U;
	assert(flush_peer(&transport, &peer) == -ETIMEDOUT);
	assert(!peer.tx_head && peer.fd == -1);
	assert(closed.count == 1 && closed.reason == -ETIMEDOUT);
	assert(closed.dropped == 1 && closed.dropped_sequence == 1);
	cleanup(&transport, &peer, receive_fd);

	memset(&closed, 0, sizeof(closed));
	clock.now_ns = 0;
	receive_fd = initialize(&transport, &peer, &clock, &closed);
	fill_send_buffer(peer.fd);
	assert(queue_frame_with_sequence(&peer, (const uint8_t *)"blocked", 7,
					 0, 1) == 0);
	clock.advance_after_read_ns = 1000000001U;
	assert(flush_peer(&transport, &peer) == -ETIMEDOUT);
	assert(!peer.tx_head && peer.fd == -1);
	assert(closed.count == 1 && closed.reason == -ETIMEDOUT);
	cleanup(&transport, &peer, receive_fd);

	memset(&closed, 0, sizeof(closed));
	clock.now_ns = 99;
	receive_fd = initialize(&transport, &peer, &clock, &closed);
	assert(queue_frame_with_sequence(&peer, (const uint8_t *)"x", 1, 100,
					 1) == 0);
	assert(flush_peer(&transport, &peer) == -ERANGE);
	assert(!peer.tx_head && peer.fd == -1);
	assert(closed.count == 1 && closed.reason == -ERANGE);
	assert(closed.dropped == 1 && closed.dropped_sequence == 1);
	cleanup(&transport, &peer, receive_fd);

	/* Completing a fresh head must not hide an expired frame behind it. */
	memset(&closed, 0, sizeof(closed));
	clock.now_ns = 1000000001U;
	receive_fd = initialize(&transport, &peer, &clock, &closed);
	assert(queue_frame_with_sequence(&peer, (const uint8_t *)"new", 3,
					 clock.now_ns, 1) == 0);
	assert(queue_frame_with_sequence(&peer, (const uint8_t *)"old", 3, 0,
					 2) == 0);
	assert(flush_peer(&transport, &peer) == -ETIMEDOUT);
	assert(!peer.tx_head && peer.fd == -1);
	assert(closed.count == 1 && closed.reason == -ETIMEDOUT);
	assert(closed.dropped == 1 && closed.dropped_sequence == 2);
	cleanup(&transport, &peer, receive_fd);

	puts("midrd-transport-budget-test: PASS");
	return 0;
}
