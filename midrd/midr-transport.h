/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Native MIDR transport contract.  No BGP session or AFI/SAFI concepts belong
 * here: IPv4/IPv6 endpoints carry MIDR frames over the implementation's native
 * TCP transport.  The TCP stream is only a byte-stream substrate; MIDR keeps
 * its own frame, session and lifetime semantics.
 */
#ifndef MIDRD_TRANSPORT_H
#define MIDRD_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "midr-core.h"

struct event_loop;

enum midr_transport_family {
	MIDR_TRANSPORT_AF_IPV4 = MIDR_CORE_AF_IPV4,
	MIDR_TRANSPORT_AF_IPV6 = MIDR_CORE_AF_IPV6,
};

struct midr_transport_endpoint {
	uint8_t family;
	uint8_t address[MIDR_CORE_ADDR_BYTES];
	uint16_t port;
	uint32_t scope_id;
};

enum midr_transport_frame_type {
	MIDR_FRAME_HELLO = 1,
	MIDR_FRAME_KEEPALIVE = 2,
	MIDR_FRAME_SNAPSHOT_BEGIN = 3,
	MIDR_FRAME_SNAPSHOT_OBJECT = 4,
	MIDR_FRAME_SNAPSHOT_END = 5,
	MIDR_FRAME_EOR = 6,
	MIDR_FRAME_UPDATE = 7,
	MIDR_FRAME_WITHDRAW = 8,
};

struct midr_transport_frame {
	uint8_t version;
	uint8_t type;
	uint16_t flags;
	uint64_t sequence;
	/* Connection generation assigned when the stream was established. */
	uint64_t generation;
	/* Optional timestamp at which an object's remaining lifetime was encoded.
	 * Zero lets the transport record the queue insertion time. */
	uint64_t encoded_ns;
	/* Local timestamp when the complete inbound frame entered the RX FIFO. */
	uint64_t received_ns;
	const uint8_t *payload;
	size_t payload_len;
};

typedef int (*midr_transport_frame_cb)(
	void *arg, const struct midr_transport_endpoint *peer,
	const struct midr_transport_frame *frame);

/* Called exactly once after a queued frame has been completely written. */
typedef void (*midr_transport_written_cb)(
	void *arg, const struct midr_transport_endpoint *peer,
	uint64_t generation, uint64_t sequence);

/* Called once for each queued frame discarded when a stream is closed. */
typedef void (*midr_transport_dropped_cb)(
	void *arg, const struct midr_transport_endpoint *peer,
	uint64_t generation, uint64_t sequence, int reason);

struct midr_transport_callbacks {
	midr_transport_frame_cb on_frame;
	void (*on_established)(void *arg,
			       const struct midr_transport_endpoint *peer);
	void (*on_closed)(void *arg, const struct midr_transport_endpoint *peer,
			  int reason);
	midr_transport_written_cb on_frame_written;
	midr_transport_dropped_cb on_frame_dropped;
	void *arg;
};

typedef uint64_t (*midr_transport_clock_cb)(void *arg);

struct midr_transport_config {
	/* The daemon event loop.  Tests may leave this NULL to use a private
	 * transport loop driven by midr_transport_poll(). */
	struct event_loop *master;
	struct midr_transport_endpoint local;
	uint32_t hello_interval_ms;
	uint32_t hold_time_ms;
	/* A queued frame older than this budget closes the session.  Zero
	 * disables the write-age check. */
	uint32_t tx_budget_ms;
	size_t max_frame_size;
	/* Optional monotonic clock injection for deterministic boundary tests. */
	midr_transport_clock_cb now_ns;
	void *clock_arg;
};

struct midr_transport;

int midr_transport_endpoint_validate(
	const struct midr_transport_endpoint *endpoint);
bool midr_transport_endpoint_equal(
	const struct midr_transport_endpoint *a,
	const struct midr_transport_endpoint *b);

int midr_transport_create(const struct midr_transport_config *config,
			  const struct midr_transport_callbacks *callbacks,
			  struct midr_transport **out);
void midr_transport_destroy(struct midr_transport **transport);
int midr_transport_start(struct midr_transport *transport);
int midr_transport_stop(struct midr_transport *transport);
int midr_transport_connect(struct midr_transport *transport,
			   const struct midr_transport_endpoint *peer);
int midr_transport_disconnect(struct midr_transport *transport,
			      const struct midr_transport_endpoint *peer);
int midr_transport_reset(struct midr_transport *transport,
			 const struct midr_transport_endpoint *peer, int reason);
int midr_transport_send(struct midr_transport *transport,
			const struct midr_transport_endpoint *peer,
			const struct midr_transport_frame *frame);
int midr_transport_poll(struct midr_transport *transport, int timeout_ms);
size_t midr_transport_peer_count(const struct midr_transport *transport);
/* Number of frames still queued or partially written on all peers. */
size_t midr_transport_pending(const struct midr_transport *transport);

#endif /* MIDRD_TRANSPORT_H */
