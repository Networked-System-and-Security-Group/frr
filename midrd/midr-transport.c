/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L

#include <zebra.h>

#include "buffer.h"
#include "frrevent.h"
#include "midr-transport.h"
#include "midr-wire.h"
#include "network.h"
#include "sockunion.h"
#include "stream.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MIDR_TRANSPORT_MAX_PEERS 128U
#define MIDR_TRANSPORT_MAX_PACKET 65535U
#define MIDR_TRANSPORT_RECONNECT_MS 100U

struct midr_transport_tx {
	struct midr_transport_tx *next;
	struct buffer *buffer;
	uint64_t encoded_ns;
	uint64_t generation;
	uint64_t sequence;
};

struct midr_transport;

struct midr_transport_peer {
	struct midr_transport *transport;
	struct midr_transport_endpoint endpoint;
	union sockunion address;
	int fd;
	bool used;
	bool desired;
	bool connecting;
	bool established;
	uint64_t generation;
	uint64_t last_rx_ms;
	struct stream *rx;
	struct midr_transport_tx *tx_head;
	struct midr_transport_tx *tx_tail;
	struct event *read_event;
	struct event *write_event;
	struct event *reconnect_event;
	struct event *hold_event;
	struct event *tx_budget_event;
};

struct midr_transport {
	struct midr_transport_config config;
	struct midr_transport_callbacks callbacks;
	struct midr_transport_peer peers[MIDR_TRANSPORT_MAX_PEERS];
	size_t peer_count;
	struct event_loop *master;
	struct event *accept_event;
	int listen_fd;
	uint64_t next_generation;
	int last_error;
	bool owns_master;
	bool started;
};

static uint64_t monotonic_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000000U + (uint64_t)ts.tv_nsec;
}

static uint64_t transport_now_ns(const struct midr_transport *transport)
{
	return transport->config.now_ns
		       ? transport->config.now_ns(transport->config.clock_arg)
		       : monotonic_ns();
}

static uint64_t transport_now_ms(const struct midr_transport *transport)
{
	return transport_now_ns(transport) / 1000000U;
}

int midr_transport_endpoint_validate(
	const struct midr_transport_endpoint *endpoint)
{
	if (!endpoint || (endpoint->family != MIDR_TRANSPORT_AF_IPV4 &&
			 endpoint->family != MIDR_TRANSPORT_AF_IPV6) ||
	    !endpoint->port)
		return -EINVAL;
	return 0;
}

bool midr_transport_endpoint_equal(
	const struct midr_transport_endpoint *a,
	const struct midr_transport_endpoint *b)
{
	if (!a || !b)
		return false;
	return a->family == b->family && a->port == b->port &&
	       a->scope_id == b->scope_id &&
	       !memcmp(a->address, b->address, sizeof(a->address));
}

static bool endpoint_address_equal(const struct midr_transport_endpoint *a,
				   const struct midr_transport_endpoint *b)
{
	if (!a || !b)
		return false;
	return a->family == b->family && a->scope_id == b->scope_id &&
	       !memcmp(a->address, b->address, sizeof(a->address));
}

static int endpoint_compare(const struct midr_transport_endpoint *a,
			    const struct midr_transport_endpoint *b)
{
	int ret;

	if (a->family != b->family)
		return a->family < b->family ? -1 : 1;
	ret = memcmp(a->address, b->address, sizeof(a->address));
	if (ret)
		return ret;
	if (a->scope_id != b->scope_id)
		return a->scope_id < b->scope_id ? -1 : 1;
	if (a->port != b->port)
		return a->port < b->port ? -1 : 1;
	return 0;
}

static int endpoint_to_sockunion(const struct midr_transport_endpoint *endpoint,
				 union sockunion *address)
{
	if (midr_transport_endpoint_validate(endpoint) || !address)
		return -EINVAL;
	memset(address, 0, sizeof(*address));
	if (endpoint->family == MIDR_TRANSPORT_AF_IPV4) {
		address->sin.sin_family = AF_INET;
		address->sin.sin_port = htons(endpoint->port);
		memcpy(&address->sin.sin_addr, endpoint->address, 4);
	} else {
		address->sin6.sin6_family = AF_INET6;
		address->sin6.sin6_port = htons(endpoint->port);
		address->sin6.sin6_scope_id = endpoint->scope_id;
		memcpy(&address->sin6.sin6_addr, endpoint->address, 16);
	}
	return 0;
}

static void sockunion_to_endpoint(const union sockunion *address,
				  struct midr_transport_endpoint *endpoint)
{
	memset(endpoint, 0, sizeof(*endpoint));
	if (address->sa.sa_family == AF_INET) {
		endpoint->family = MIDR_TRANSPORT_AF_IPV4;
		endpoint->port = ntohs(address->sin.sin_port);
		memcpy(endpoint->address, &address->sin.sin_addr, 4);
	} else if (address->sa.sa_family == AF_INET6) {
		endpoint->family = MIDR_TRANSPORT_AF_IPV6;
		endpoint->port = ntohs(address->sin6.sin6_port);
		endpoint->scope_id = address->sin6.sin6_scope_id;
		memcpy(endpoint->address, &address->sin6.sin6_addr, 16);
	}
}

static bool local_keeps_outbound(const struct midr_transport *transport,
				 int accepted_fd,
				 const struct midr_transport_peer *peer)
{
	union sockunion address;
	struct midr_transport_endpoint local = transport->config.local;
	socklen_t address_len = sizeof(address);

	/* getsockname() resolves a wildcard listener to the concrete address used
	 * by this connection.  Both endpoints can therefore make the same choice:
	 * the lower listening endpoint keeps its outbound stream and the higher
	 * endpoint keeps the matching inbound stream. */
	if (getsockname(accepted_fd, (struct sockaddr *)&address,
			&address_len) == 0)
		sockunion_to_endpoint(&address, &local);
	return endpoint_compare(&local, &peer->endpoint) <= 0;
}

static struct midr_transport_peer *find_peer(
	struct midr_transport *transport,
	const struct midr_transport_endpoint *endpoint)
{
	for (size_t i = 0; i < transport->peer_count; i++)
		if (transport->peers[i].used &&
		    midr_transport_endpoint_equal(&transport->peers[i].endpoint,
						  endpoint))
			return &transport->peers[i];
	return NULL;
}

static struct midr_transport_peer *find_peer_by_address(
	struct midr_transport *transport,
	const struct midr_transport_endpoint *endpoint)
{
	for (size_t i = 0; i < transport->peer_count; i++)
		if (transport->peers[i].used && transport->peers[i].desired &&
		    endpoint_address_equal(&transport->peers[i].endpoint, endpoint))
			return &transport->peers[i];
	return NULL;
}

static struct midr_transport_peer *ensure_peer(
	struct midr_transport *transport,
	const struct midr_transport_endpoint *endpoint,
	const union sockunion *address, bool desired)
{
	struct midr_transport_peer *peer = find_peer(transport, endpoint);

	if (peer) {
		if (address)
			peer->address = *address;
		if (desired)
			peer->desired = true;
		return peer;
	}
	for (size_t i = 0; i < transport->peer_count; i++) {
		if (!transport->peers[i].used) {
			peer = &transport->peers[i];
			goto initialize;
		}
	}
	if (transport->peer_count == MIDR_TRANSPORT_MAX_PEERS)
		return NULL;
	peer = &transport->peers[transport->peer_count++];
initialize:
	memset(peer, 0, sizeof(*peer));
	peer->transport = transport;
	peer->endpoint = *endpoint;
	if (address)
		peer->address = *address;
	peer->fd = -1;
	peer->desired = desired;
	peer->used = true;
	return peer;
}

static void peer_read_ready(struct event *event);
static void peer_write_ready(struct event *event);
static void peer_reconnect(struct event *event);
static void peer_hold_expired(struct event *event);
static void peer_tx_budget_expired(struct event *event);
static void accept_ready(struct event *event);
static void close_peer(struct midr_transport *transport,
			       struct midr_transport_peer *peer, int reason);

static void remember_error(struct midr_transport *transport, int error)
{
	if (error && !transport->last_error)
		transport->last_error = error;
}

static void fail_tx_budget(struct midr_transport_peer *peer, int error)
{
	close_peer(peer->transport, peer, error);
}

static void schedule_read(struct midr_transport_peer *peer)
{
	if (peer->used && peer->established && peer->fd >= 0 &&
	    !peer->read_event)
		event_add_read(peer->transport->master, peer_read_ready, peer,
			       peer->fd, &peer->read_event);
}

static void schedule_write(struct midr_transport_peer *peer)
{
	if (peer->used && peer->fd >= 0 &&
	    (peer->connecting || peer->tx_head) && !peer->write_event)
		event_add_write(peer->transport->master, peer_write_ready, peer,
				peer->fd, &peer->write_event);
}

static void schedule_reconnect(struct midr_transport_peer *peer,
			       uint32_t delay_ms)
{
	event_cancel(&peer->reconnect_event);
	if (peer->used && peer->desired && peer->fd < 0 &&
	    peer->transport->started)
		event_add_timer_msec(peer->transport->master, peer_reconnect, peer,
				     delay_ms, &peer->reconnect_event);
}

static void schedule_hold(struct midr_transport_peer *peer)
{
	struct midr_transport *transport = peer->transport;
	uint64_t elapsed;
	uint64_t now;
	uint32_t delay;

	event_cancel(&peer->hold_event);
	if (!peer->used || !peer->established || !transport->config.hold_time_ms)
		return;
	now = transport_now_ms(transport);
	elapsed = now >= peer->last_rx_ms ? now - peer->last_rx_ms : 0;
	delay = elapsed >= transport->config.hold_time_ms
			? 1U
			: transport->config.hold_time_ms - (uint32_t)elapsed;
	event_add_timer_msec(transport->master, peer_hold_expired, peer, delay,
			     &peer->hold_event);
}

static int frame_budget_error(const struct midr_transport *transport,
			      const struct midr_transport_tx *tx,
			      uint64_t now_ns)
{
	uint64_t budget_ns;

	if (!transport->config.tx_budget_ms)
		return 0;
	budget_ns = (uint64_t)transport->config.tx_budget_ms * 1000000U;
	if (now_ns < tx->encoded_ns)
		return -ERANGE;
	if (now_ns - tx->encoded_ns > budget_ns)
		return -ETIMEDOUT;
	return 0;
}

static int schedule_tx_budget(struct midr_transport_peer *peer)
{
	struct midr_transport *transport = peer->transport;
	struct midr_transport_tx *tx = peer->tx_head;
	uint64_t budget_ns;
	uint64_t elapsed_ns;
	uint64_t now_ns;
	uint64_t remaining_ns;
	uint64_t delay_ms;

	event_cancel(&peer->tx_budget_event);
	if (!peer->used || !peer->established || !tx ||
	    !transport->config.tx_budget_ms)
		return 0;
	now_ns = transport_now_ns(transport);
	if (now_ns < tx->encoded_ns) {
		fail_tx_budget(peer, -ERANGE);
		return -ERANGE;
	}
	budget_ns = (uint64_t)transport->config.tx_budget_ms * 1000000U;
	elapsed_ns = now_ns - tx->encoded_ns;
	if (elapsed_ns > budget_ns) {
		fail_tx_budget(peer, -ETIMEDOUT);
		return -ETIMEDOUT;
	}
	remaining_ns = budget_ns - elapsed_ns;
	delay_ms = remaining_ns / 1000000U + 1U;
	if (delay_ms > UINT32_MAX)
		delay_ms = UINT32_MAX;
	event_add_timer_msec(transport->master, peer_tx_budget_expired, peer,
			     (uint32_t)delay_ms, &peer->tx_budget_event);
	return 0;
}

static void free_tx(struct midr_transport *transport,
			struct midr_transport_peer *peer, int reason)
{
	struct midr_transport_endpoint endpoint = peer->endpoint;
	struct midr_transport_tx *tx = peer->tx_head;

	event_cancel(&peer->tx_budget_event);
	peer->tx_head = NULL;
	peer->tx_tail = NULL;
	while (tx) {
		struct midr_transport_tx *next = tx->next;

		if (transport->callbacks.on_frame_dropped)
			transport->callbacks.on_frame_dropped(
				transport->callbacks.arg, &endpoint,
				tx->generation, tx->sequence, reason);
		buffer_free(tx->buffer);
		free(tx);
		tx = next;
	}
}

static void close_peer(struct midr_transport *transport,
			       struct midr_transport_peer *peer, int reason)
{
	struct midr_transport_endpoint endpoint = peer->endpoint;
	bool notify = peer->established || peer->connecting;

	event_cancel(&peer->read_event);
	event_cancel(&peer->write_event);
	event_cancel(&peer->hold_event);
	event_cancel(&peer->tx_budget_event);
	if (peer->fd >= 0)
		close(peer->fd);
	peer->fd = -1;
	peer->connecting = false;
	peer->established = false;
	if (peer->rx)
		stream_reset(peer->rx);
	free_tx(transport, peer, reason);
	if (!peer->desired) {
		event_cancel(&peer->reconnect_event);
		stream_free(peer->rx);
		peer->rx = NULL;
		peer->used = false;
	}
	if (notify && transport->callbacks.on_closed)
		transport->callbacks.on_closed(transport->callbacks.arg,
					       &endpoint, reason);
	if (peer->used && peer->desired && peer->fd < 0) {
		schedule_reconnect(peer, MIDR_TRANSPORT_RECONNECT_MS);
	}
}

static int transport_set_nonblocking(int fd)
{
	if (set_nonblocking(fd) < 0)
		return -errno;
	return 0;
}

/* Frames queued while connect is in progress acquire the new stream's
 * generation before any byte is written. */
static void retag_queued_frames(struct midr_transport_peer *peer,
				uint64_t generation)
{
	for (struct midr_transport_tx *tx = peer->tx_head; tx; tx = tx->next)
		tx->generation = generation;
}

static void establish_peer(struct midr_transport *transport,
			   struct midr_transport_peer *peer)
{
	event_cancel(&peer->reconnect_event);
	peer->connecting = false;
	peer->established = true;
	peer->generation = ++transport->next_generation;
	retag_queued_frames(peer, peer->generation);
	peer->last_rx_ms = transport_now_ms(transport);
	if (!peer->rx)
		peer->rx = stream_new(transport->config.max_frame_size);
	else
		stream_reset(peer->rx);
	if (!peer->rx) {
		close_peer(transport, peer, -ENOMEM);
		return;
	}
	schedule_read(peer);
	schedule_hold(peer);
	schedule_write(peer);
	if (transport->callbacks.on_established)
		transport->callbacks.on_established(transport->callbacks.arg,
						    &peer->endpoint);
}

static int flush_peer(struct midr_transport *transport,
			      struct midr_transport_peer *peer)
{
	while (peer->tx_head) {
		struct midr_transport_tx *tx = peer->tx_head;
		uint64_t generation = peer->generation;
		int fd = peer->fd;
		int ret = frame_budget_error(transport, tx,
					     transport_now_ns(transport));
		buffer_status_t status;

		if (ret) {
			close_peer(transport, peer, ret);
			return ret;
		}
		status = buffer_flush_available(tx->buffer, peer->fd);
		if (status == BUFFER_ERROR) {
			ret = errno ? -errno : -EPIPE;
			close_peer(transport, peer, ret);
			return ret;
		}
		if (status == BUFFER_PENDING) {
			schedule_write(peer);
			return schedule_tx_budget(peer);
		}
		peer->tx_head = tx->next;
		if (!peer->tx_head)
			peer->tx_tail = NULL;
		generation = tx->generation;
		uint64_t sequence = tx->sequence;

		buffer_free(tx->buffer);
		free(tx);
		event_cancel(&peer->tx_budget_event);
		if (transport->callbacks.on_frame_written)
			transport->callbacks.on_frame_written(
				transport->callbacks.arg, &peer->endpoint,
				generation, sequence);
		if (!peer->used || !peer->established || peer->fd != fd ||
		    peer->generation != generation)
			return 0;
	}
	event_cancel(&peer->write_event);
	event_cancel(&peer->tx_budget_event);
	return 0;
}

static int queue_frame_with_sequence(struct midr_transport_peer *peer,
				      const uint8_t *data, size_t length,
				      uint64_t encoded_ns, uint64_t sequence)
{
	struct midr_transport_tx *tx = calloc(1, sizeof(*tx));

	if (!tx)
		return -ENOMEM;
	tx->buffer = buffer_new(length);
	if (!tx->buffer) {
		free(tx);
		return -ENOMEM;
	}
	buffer_put(tx->buffer, data, length);
	tx->encoded_ns = encoded_ns;
	tx->generation = peer->generation;
	tx->sequence = sequence;
	if (peer->tx_tail)
		peer->tx_tail->next = tx;
	else
		peer->tx_head = tx;
	peer->tx_tail = tx;
	return 0;
}

static int parse_rx(struct midr_transport *transport,
			    struct midr_transport_peer *peer,
			    uint64_t received_ns)
{
	while (STREAM_READABLE(peer->rx) >= MIDR_WIRE_HEADER_LEN) {
		const uint8_t *data = STREAM_DATA(peer->rx) +
				      stream_get_getp(peer->rx);
		uint32_t magic, payload_length;
		size_t frame_length;
		struct midr_wire_frame wire_frame;
		struct midr_transport_frame frame = {0};
		uint64_t generation;
		int fd;
		int ret;

		memcpy(&magic, data, sizeof(magic));
		magic = ntohl(magic);
		if (magic != MIDR_WIRE_MAGIC || data[4] != MIDR_WIRE_VERSION) {
			close_peer(transport, peer, -EBADMSG);
			return -EBADMSG;
		}
		memcpy(&payload_length, data + 16, sizeof(payload_length));
		payload_length = ntohl(payload_length);
		if (payload_length > transport->config.max_frame_size -
			    MIDR_WIRE_HEADER_LEN) {
			close_peer(transport, peer, -EMSGSIZE);
			return -EMSGSIZE;
		}
		frame_length = MIDR_WIRE_HEADER_LEN + (size_t)payload_length;
		if (STREAM_READABLE(peer->rx) < frame_length)
			return 0;
		ret = midr_wire_decode_frame(data, frame_length, &wire_frame);
		if (ret) {
			close_peer(transport, peer, -EBADMSG);
			return ret;
		}
		frame.version = wire_frame.version;
		frame.type = wire_frame.type;
		frame.flags = wire_frame.flags;
		frame.sequence = wire_frame.sequence;
		frame.generation = peer->generation;
		frame.received_ns = received_ns;
		frame.payload = wire_frame.payload;
		frame.payload_len = wire_frame.payload_len;
		peer->last_rx_ms = received_ns / 1000000U;
		schedule_hold(peer);
		fd = peer->fd;
		generation = peer->generation;
		ret = transport->callbacks.on_frame(transport->callbacks.arg,
						   &peer->endpoint, &frame);
		if (ret) {
			if (peer->used && peer->fd == fd &&
			    peer->generation == generation)
				close_peer(transport, peer, ret);
			return ret;
		}
		if (!peer->used || !peer->established || peer->fd != fd ||
		    peer->generation != generation)
			return 0;
		stream_forward_getp(peer->rx, frame_length);
		stream_pulldown(peer->rx);
	}
	return 0;
}

static int read_peer(struct midr_transport *transport,
			     struct midr_transport_peer *peer)
{
	for (;;) {
		int fd = peer->fd;
		ssize_t received;

		if (!STREAM_WRITEABLE(peer->rx)) {
			close_peer(transport, peer, -EMSGSIZE);
			return -EMSGSIZE;
		}
		received = stream_read_try(peer->rx, fd,
					   STREAM_WRITEABLE(peer->rx));
		if (received > 0) {
			int ret = parse_rx(transport, peer,
					   transport_now_ns(transport));

			if (ret)
				return ret;
			if (!peer->used || !peer->established || peer->fd != fd)
				return 0;
			continue;
		}
		if (received == -2)
			return 0;
		if (received == 0) {
			close_peer(transport, peer, -ECONNRESET);
			return 0;
		}
		{
			int error = errno ? -errno : -EIO;

			close_peer(transport, peer, error);
			return error;
		}
	}
}

static int connect_peer_now(struct midr_transport *transport,
				struct midr_transport_peer *peer, uint64_t now)
{
	enum connect_result result;
	int fd;
	int ret;

	event_cancel(&peer->reconnect_event);
	fd = sockunion_socket(&peer->address);
	if (fd < 0) {
		schedule_reconnect(peer, MIDR_TRANSPORT_RECONNECT_MS);
		return -errno;
	}
	ret = transport_set_nonblocking(fd);
	if (ret) {
		close(fd);
		schedule_reconnect(peer, MIDR_TRANSPORT_RECONNECT_MS);
		return ret;
	}
	/* 第一组修改：overlay 会话是多跳的，不绑定监听地址时内核会选出口链路地址作源，
	 * 对端就认不出这是它请求过的会话（BGP 里对应 update-source）。 */
	{
		union sockunion local;

		if (!endpoint_to_sockunion(&transport->config.local, &local) &&
		    !sockunion_is_null(&local) &&
		    sockunion_bind(fd, &local, 0, &local) < 0) {
			ret = -errno;
			close(fd);
			schedule_reconnect(peer, MIDR_TRANSPORT_RECONNECT_MS);
			return ret;
		}
	}
	result = sockunion_connect(fd, &peer->address,
				   htons(peer->endpoint.port));
	if (result == connect_success) {
		peer->fd = fd;
		establish_peer(transport, peer);
		return 0;
	}
	if (result == connect_error) {
		close(fd);
		schedule_reconnect(peer, MIDR_TRANSPORT_RECONNECT_MS);
		return 0;
	}
	peer->fd = fd;
	peer->connecting = true;
	peer->established = false;
	schedule_write(peer);
	return 0;
}

static void peer_reconnect(struct event *event)
{
	struct midr_transport_peer *peer = EVENT_ARG(event);

	peer->reconnect_event = NULL;
	if (!peer->used || !peer->desired || peer->fd >= 0)
		return;
	(void)connect_peer_now(peer->transport, peer,
				   transport_now_ms(peer->transport));
}

static void peer_hold_expired(struct event *event)
{
	struct midr_transport_peer *peer = EVENT_ARG(event);
	struct midr_transport *transport = peer->transport;
	uint64_t now;

	peer->hold_event = NULL;
	if (!peer->used || !peer->established || !transport->config.hold_time_ms)
		return;
	now = transport_now_ms(transport);
	if (now >= peer->last_rx_ms &&
	    now - peer->last_rx_ms >= transport->config.hold_time_ms) {
		close_peer(transport, peer, -ETIMEDOUT);
		return;
	}
	schedule_hold(peer);
}

static void peer_tx_budget_expired(struct event *event)
{
	struct midr_transport_peer *peer = event ? EVENT_ARG(event) : NULL;
	struct midr_transport *transport;
	int ret;

	if (!peer)
		return;
	peer->tx_budget_event = NULL;
	transport = peer->transport;
	if (!peer->used || !peer->established || !peer->tx_head)
		return;
	ret = frame_budget_error(transport, peer->tx_head,
				 transport_now_ns(transport));
	if (ret) {
		fail_tx_budget(peer, ret);
		remember_error(transport, ret);
		return;
	}
	(void)schedule_tx_budget(peer);
}

static void peer_read_ready(struct event *event)
{
	struct midr_transport_peer *peer = EVENT_ARG(event);
	struct midr_transport *transport = peer->transport;
	int fd = EVENT_FD(event);
	int ret;

	peer->read_event = NULL;
	if (!peer->used || !peer->established || peer->fd != fd)
		return;
	ret = read_peer(transport, peer);
	remember_error(transport, ret);
	if (peer->used && peer->established && peer->fd == fd)
		schedule_read(peer);
}

static void peer_write_ready(struct event *event)
{
	struct midr_transport_peer *peer = EVENT_ARG(event);
	struct midr_transport *transport = peer->transport;
	int fd = EVENT_FD(event);

	peer->write_event = NULL;
	if (!peer->used || peer->fd != fd)
		return;
	if (peer->connecting) {
		int error = 0;
		socklen_t length = sizeof(error);

		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0)
			error = errno;
		if (error) {
			close_peer(transport, peer, -error);
			return;
		}
		establish_peer(transport, peer);
	}
	if (peer->used && peer->established && peer->fd == fd)
		(void)flush_peer(transport, peer);
	if (peer->used && peer->fd == fd &&
	    (peer->connecting || peer->tx_head))
		schedule_write(peer);
}

int midr_transport_create(const struct midr_transport_config *config,
			  const struct midr_transport_callbacks *callbacks,
			  struct midr_transport **out)
{
	struct midr_transport *transport;

	if (!config || !callbacks || !callbacks->on_frame || !out || *out ||
	    midr_transport_endpoint_validate(&config->local) ||
	    config->max_frame_size < MIDR_WIRE_HEADER_LEN ||
	    config->max_frame_size > MIDR_TRANSPORT_MAX_PACKET)
		return -EINVAL;
	transport = calloc(1, sizeof(*transport));
	if (!transport)
		return -ENOMEM;
	transport->config = *config;
	transport->callbacks = *callbacks;
	transport->listen_fd = -1;
	transport->master = config->master;
	if (!transport->master) {
		transport->master = event_master_create("midr-transport-test");
		transport->owns_master = true;
	}
	*out = transport;
	return 0;
}

void midr_transport_destroy(struct midr_transport **transportp)
{
	if (!transportp || !*transportp)
		return;
	(void)midr_transport_stop(*transportp);
	if ((*transportp)->owns_master)
		event_master_free((*transportp)->master);
	free(*transportp);
	*transportp = NULL;
}

int midr_transport_start(struct midr_transport *transport)
{
	union sockunion address;
	int one = 1;
	int ret;

	if (!transport || transport->started)
		return -EINVAL;
	if (endpoint_to_sockunion(&transport->config.local, &address))
		return -EINVAL;
	transport->listen_fd = sockunion_socket(&address);
	if (transport->listen_fd < 0)
		return -errno;
	(void)setsockopt(transport->listen_fd, SOL_SOCKET, SO_REUSEADDR,
				 &one, sizeof(one));
	if (address.sa.sa_family == AF_INET6)
		(void)setsockopt(transport->listen_fd, IPPROTO_IPV6, IPV6_V6ONLY,
				 &one, sizeof(one));
	if (sockunion_bind(transport->listen_fd, &address,
			   transport->config.local.port, &address) < 0) {
		int error = errno;

		close(transport->listen_fd);
		transport->listen_fd = -1;
		return -error;
	}
	if (listen(transport->listen_fd, SOMAXCONN) < 0) {
		int error = errno;

		close(transport->listen_fd);
		transport->listen_fd = -1;
		return -error;
	}
	ret = transport_set_nonblocking(transport->listen_fd);
	if (ret) {

		close(transport->listen_fd);
		transport->listen_fd = -1;
		return ret;
	}
	transport->started = true;
	event_add_read(transport->master, accept_ready, transport,
		       transport->listen_fd, &transport->accept_event);
	return 0;
}

int midr_transport_stop(struct midr_transport *transport)
{
	if (!transport)
		return -EINVAL;
	event_cancel(&transport->accept_event);
	if (transport->listen_fd >= 0)
		close(transport->listen_fd);
	transport->listen_fd = -1;
	for (size_t i = 0; i < transport->peer_count; i++) {
		struct midr_transport_peer *peer = &transport->peers[i];

		if (!peer->used)
			continue;
		peer->desired = false;
		close_peer(transport, peer, 0);
	}
	transport->peer_count = 0;
	transport->started = false;
	return 0;
}

int midr_transport_connect(struct midr_transport *transport,
			   const struct midr_transport_endpoint *endpoint)
{
	union sockunion address;
	struct midr_transport_peer *peer;
	uint64_t now;

	if (!transport || !transport->started ||
	    endpoint_to_sockunion(endpoint, &address))
		return -EINVAL;
	peer = ensure_peer(transport, endpoint, &address, true);
	if (!peer)
		return -ENOSPC;
	peer->desired = true;
	if (peer->fd >= 0)
		return 0;
	now = transport_now_ms(transport);
	return connect_peer_now(transport, peer, now);
}

int midr_transport_disconnect(struct midr_transport *transport,
			      const struct midr_transport_endpoint *endpoint)
{
	struct midr_transport_peer *peer;

	if (!transport || !endpoint || !(peer = find_peer(transport, endpoint)))
		return -ENOENT;
	peer->desired = false;
	close_peer(transport, peer, 0);
	return 0;
}

int midr_transport_reset(struct midr_transport *transport,
			 const struct midr_transport_endpoint *endpoint, int reason)
{
	struct midr_transport_peer *peer;

	if (!transport || !endpoint || reason >= 0)
		return -EINVAL;
	peer = find_peer(transport, endpoint);
	if (!peer || !peer->desired)
		return -ENOENT;
	close_peer(transport, peer, reason);
	return 0;
}

int midr_transport_send(struct midr_transport *transport,
			const struct midr_transport_endpoint *endpoint,
			const struct midr_transport_frame *frame)
{
	struct midr_transport_peer *peer;
	struct midr_wire_frame wire_frame;
	uint8_t packet[MIDR_TRANSPORT_MAX_PACKET];
	uint64_t encoded_ns;
	size_t length;
	int ret;

	if (!transport || !transport->started || !frame || !endpoint ||
	    !(peer = find_peer(transport, endpoint)) || peer->fd < 0)
		return -ENOTCONN;
	wire_frame.version = frame->version;
	wire_frame.type = frame->type;
	wire_frame.flags = frame->flags;
	wire_frame.sequence = frame->sequence;
	wire_frame.payload = frame->payload;
	wire_frame.payload_len = frame->payload_len;
	if (midr_wire_encode_frame(&wire_frame, packet, sizeof(packet), &length) ||
	    length > transport->config.max_frame_size)
		return -EMSGSIZE;
	encoded_ns = frame->encoded_ns ? frame->encoded_ns :
		transport_now_ns(transport);
	ret = queue_frame_with_sequence(peer, packet, length, encoded_ns,
				       frame->sequence);
	if (ret)
		return ret;
	return peer->established ? flush_peer(transport, peer) : 0;
}

static int accept_peers(struct midr_transport *transport)
{
	for (;;) {
		union sockunion address;
		struct midr_transport_endpoint source, endpoint;
		struct midr_transport_peer *peer;
		int fd = sockunion_accept(transport->listen_fd, &address);

		if (fd < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return 0;
			return -errno;
		}
		if (transport_set_nonblocking(fd)) {
			close(fd);
			continue;
		}
		sockunion_to_endpoint(&address, &source);
		if (midr_transport_endpoint_validate(&source)) {
			close(fd);
			continue;
		}
		peer = find_peer(transport, &source);
		if (!peer)
			peer = find_peer_by_address(transport, &source);
		if (peer) {
			endpoint = peer->endpoint;
			if (peer->fd >= 0) {
				if (local_keeps_outbound(transport, fd, peer)) {
					close(fd);
					continue;
				}
				/* Both endpoints may initiate at the same time.  Replace the
				 * losing outbound stream so both sides retain the same TCP
				 * connection instead of repeatedly closing each other. */
				close_peer(transport, peer, -ECONNABORTED);
			}
			/* A configured peer retains its destination port for future
			 * reconnects.  Accepted TCP sockets expose the remote's
			 * ephemeral source port, which must not replace that address. */
			if (!peer->desired) {
				peer->address = address;
			}
			peer->fd = fd;
			establish_peer(transport, peer);
			continue;
		}
		endpoint = source;
		peer = ensure_peer(transport, &endpoint, &address, false);
		if (!peer) {
			close(fd);
			continue;
		}
		peer->fd = fd;
		establish_peer(transport, peer);
	}
}

static void accept_ready(struct event *event)
{
	struct midr_transport *transport = EVENT_ARG(event);
	int fd = EVENT_FD(event);
	int ret;

	transport->accept_event = NULL;
	if (!transport->started || transport->listen_fd != fd)
		return;
	ret = accept_peers(transport);
	remember_error(transport, ret);
	if (transport->started && transport->listen_fd == fd)
		event_add_read(transport->master, accept_ready, transport, fd,
			       &transport->accept_event);
}

static void poll_timeout(struct event *event)
{
	(void)event;
}

int midr_transport_poll(struct midr_transport *transport, int timeout_ms)
{
	struct event *timeout_event = NULL;
	struct event ready;
	int ret = 0;

	if (!transport || !transport->started || timeout_ms < 0)
		return -EINVAL;
	event_add_timer_msec(transport->master, poll_timeout, transport,
			     timeout_ms, &timeout_event);
	if (event_fetch(transport->master, &ready))
		event_call(&ready);
	event_cancel(&timeout_event);
	if (transport->last_error) {
		ret = transport->last_error;
		transport->last_error = 0;
	}
	return ret;
}

size_t midr_transport_peer_count(const struct midr_transport *transport)
{
	size_t count = 0;

	if (!transport)
		return 0;
	for (size_t i = 0; i < transport->peer_count; i++)
		if (transport->peers[i].used &&
		    (transport->peers[i].desired || transport->peers[i].fd >= 0))
			count++;
	return count;
}

size_t midr_transport_pending(const struct midr_transport *transport)
{
	size_t count = 0;

	if (!transport)
		return 0;
	for (size_t i = 0; i < transport->peer_count; i++) {
		const struct midr_transport_peer *peer = &transport->peers[i];

		for (const struct midr_transport_tx *tx = peer->tx_head; tx;
		     tx = tx->next)
			count++;
	}
	return count;
}
