/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L

#include "midr-transport.h"
#include "midr-wire.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MIDR_TRANSPORT_MAX_PEERS 128U
#define MIDR_TRANSPORT_MAX_PACKET 65535U
#define MIDR_TRANSPORT_RECONNECT_MS 100U

struct midr_transport_tx {
	struct midr_transport_tx *next;
	uint8_t *data;
	size_t length;
	size_t offset;
	uint64_t encoded_ns;
};

struct midr_transport_peer {
	struct midr_transport_endpoint endpoint;
	struct sockaddr_storage address;
	socklen_t address_len;
	int fd;
	bool used;
	bool desired;
	bool connecting;
	bool established;
	uint64_t generation;
	uint64_t last_rx_ms;
	uint64_t next_connect_ms;
	uint8_t *rx_buffer;
	size_t rx_length;
	size_t rx_capacity;
	struct midr_transport_tx *tx_head;
	struct midr_transport_tx *tx_tail;
};

struct midr_transport {
	struct midr_transport_config config;
	struct midr_transport_callbacks callbacks;
	struct midr_transport_peer peers[MIDR_TRANSPORT_MAX_PEERS];
	size_t peer_count;
	int listen_fd;
	uint64_t next_generation;
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

static int endpoint_to_sockaddr(const struct midr_transport_endpoint *endpoint,
				struct sockaddr_storage *address,
				socklen_t *length)
{
	if (midr_transport_endpoint_validate(endpoint) || !address || !length)
		return -EINVAL;
	memset(address, 0, sizeof(*address));
	if (endpoint->family == MIDR_TRANSPORT_AF_IPV4) {
		struct sockaddr_in *sin = (struct sockaddr_in *)address;

		sin->sin_family = AF_INET;
		sin->sin_port = htons(endpoint->port);
		memcpy(&sin->sin_addr, endpoint->address, 4);
		*length = sizeof(*sin);
	} else {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)address;

		sin6->sin6_family = AF_INET6;
		sin6->sin6_port = htons(endpoint->port);
		sin6->sin6_scope_id = endpoint->scope_id;
		memcpy(&sin6->sin6_addr, endpoint->address, 16);
		*length = sizeof(*sin6);
	}
	return 0;
}

static void sockaddr_to_endpoint(const struct sockaddr_storage *address,
				 struct midr_transport_endpoint *endpoint)
{
	memset(endpoint, 0, sizeof(*endpoint));
	if (address->ss_family == AF_INET) {
		const struct sockaddr_in *sin = (const struct sockaddr_in *)address;

		endpoint->family = MIDR_TRANSPORT_AF_IPV4;
		endpoint->port = ntohs(sin->sin_port);
		memcpy(endpoint->address, &sin->sin_addr, 4);
	} else if (address->ss_family == AF_INET6) {
		const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)address;

		endpoint->family = MIDR_TRANSPORT_AF_IPV6;
		endpoint->port = ntohs(sin6->sin6_port);
		endpoint->scope_id = sin6->sin6_scope_id;
		memcpy(endpoint->address, &sin6->sin6_addr, 16);
	}
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
	const struct sockaddr_storage *address, socklen_t address_len,
	bool desired)
{
	struct midr_transport_peer *peer = find_peer(transport, endpoint);

	if (peer) {
		if (address) {
			peer->address = *address;
			peer->address_len = address_len;
		}
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
	peer->endpoint = *endpoint;
	if (address) {
		peer->address = *address;
		peer->address_len = address_len;
	}
	peer->fd = -1;
	peer->desired = desired;
	peer->used = true;
	return peer;
}

static void free_tx(struct midr_transport_peer *peer)
{
	struct midr_transport_tx *tx;

	while ((tx = peer->tx_head)) {
		peer->tx_head = tx->next;
		free(tx->data);
		free(tx);
	}
	peer->tx_tail = NULL;
}

static void close_peer(struct midr_transport *transport,
			       struct midr_transport_peer *peer, int reason)
{
	bool notify = peer->established || peer->connecting;
	uint64_t now = transport_now_ms(transport);

	if (peer->fd >= 0)
		close(peer->fd);
	peer->fd = -1;
	peer->connecting = false;
	peer->established = false;
	peer->rx_length = 0;
	free_tx(peer);
	if (notify && transport->callbacks.on_closed)
		transport->callbacks.on_closed(transport->callbacks.arg,
					       &peer->endpoint, reason);
	if (peer->desired)
		peer->next_connect_ms = now + MIDR_TRANSPORT_RECONNECT_MS;
	else {
		free(peer->rx_buffer);
		peer->rx_buffer = NULL;
		peer->rx_capacity = 0;
		peer->used = false;
	}
}

static int set_nonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);

	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return -errno;
	return 0;
}

static void establish_peer(struct midr_transport *transport,
			   struct midr_transport_peer *peer)
{
	peer->connecting = false;
	peer->established = true;
	peer->generation = ++transport->next_generation;
	peer->last_rx_ms = transport_now_ms(transport);
	peer->rx_length = 0;
	if (!peer->rx_buffer) {
		peer->rx_capacity = transport->config.max_frame_size;
		peer->rx_buffer = malloc(peer->rx_capacity);
		if (!peer->rx_buffer) {
			peer->rx_capacity = 0;
			close_peer(transport, peer, -ENOMEM);
			return;
		}
	}
	if (transport->callbacks.on_established)
		transport->callbacks.on_established(transport->callbacks.arg,
						    &peer->endpoint);
}

static int flush_peer(struct midr_transport *transport,
			      struct midr_transport_peer *peer)
{
	while (peer->tx_head) {
		struct midr_transport_tx *tx = peer->tx_head;
		uint64_t now_ns;
		ssize_t sent;
		int flags = 0;
		int ret;

		now_ns = transport_now_ns(transport);
		ret = 0;
		if (transport->config.tx_budget_ms) {
			uint64_t budget_ns =
				(uint64_t)transport->config.tx_budget_ms * 1000000U;

			if (now_ns < tx->encoded_ns)
				ret = -ERANGE;
			else if (now_ns - tx->encoded_ns > budget_ns)
				ret = -ETIMEDOUT;
		}
		if (ret) {
			close_peer(transport, peer, ret);
			return ret;
		}

#ifdef MSG_NOSIGNAL
		flags |= MSG_NOSIGNAL;
#endif
		sent = send(peer->fd, tx->data + tx->offset,
			    tx->length - tx->offset, flags);
		if (sent > 0) {
			tx->offset += (size_t)sent;
			if (tx->offset == tx->length) {
				peer->tx_head = tx->next;
				if (!peer->tx_head)
					peer->tx_tail = NULL;
				free(tx->data);
				free(tx);
			}
			continue;
		}
		if (sent < 0 && errno == EINTR)
			continue;
		if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 0;
		{
			int error = sent < 0 ? -errno : -EPIPE;

			close_peer(transport, peer, error);
			return error;
		}
	}
	return 0;
}

static int queue_frame(struct midr_transport_peer *peer,
			       const uint8_t *data, size_t length,
			       uint64_t encoded_ns)
{
	struct midr_transport_tx *tx = calloc(1, sizeof(*tx));

	if (!tx)
		return -ENOMEM;
	tx->data = malloc(length);
	if (!tx->data) {
		free(tx);
		return -ENOMEM;
	}
	memcpy(tx->data, data, length);
	tx->length = length;
	tx->encoded_ns = encoded_ns;
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
	while (peer->rx_length >= MIDR_WIRE_HEADER_LEN) {
		uint32_t magic, payload_length;
		size_t frame_length;
		struct midr_wire_frame wire_frame;
		struct midr_transport_frame frame = {0};
		int ret;

		memcpy(&magic, peer->rx_buffer, sizeof(magic));
		magic = ntohl(magic);
		if (magic != MIDR_WIRE_MAGIC ||
		    peer->rx_buffer[4] != MIDR_WIRE_VERSION) {
			close_peer(transport, peer, -EBADMSG);
			return -EBADMSG;
		}
		memcpy(&payload_length, peer->rx_buffer + 16,
		       sizeof(payload_length));
		payload_length = ntohl(payload_length);
		if (payload_length > transport->config.max_frame_size -
			    MIDR_WIRE_HEADER_LEN) {
			close_peer(transport, peer, -EMSGSIZE);
			return -EMSGSIZE;
		}
		frame_length = MIDR_WIRE_HEADER_LEN + (size_t)payload_length;
		if (peer->rx_length < frame_length)
			return 0;
		ret = midr_wire_decode_frame(peer->rx_buffer, frame_length,
					     &wire_frame);
		if (ret) {
			close_peer(transport, peer, -EBADMSG);
			return ret;
		}
		frame.version = wire_frame.version;
		frame.type = wire_frame.type;
		frame.flags = wire_frame.flags;
		frame.sequence = wire_frame.sequence;
		frame.received_ns = received_ns;
		frame.payload = wire_frame.payload;
		frame.payload_len = wire_frame.payload_len;
		peer->last_rx_ms = received_ns / 1000000U;
		ret = transport->callbacks.on_frame(transport->callbacks.arg,
						   &peer->endpoint, &frame);
		if (ret) {
			/* A frame callback error means the peer violated the MIDR
			 * contract (or the local engine rejected the frame).  Drop the
			 * connection before returning so the unconsumed frame cannot be
			 * delivered again on the next poll.  The desired peer remains
			 * registered and will follow the normal reconnect path. */
			close_peer(transport, peer, ret);
			return ret;
		}
		peer->rx_length -= frame_length;
		if (peer->rx_length)
			memmove(peer->rx_buffer, peer->rx_buffer + frame_length,
				peer->rx_length);
	}
	return 0;
}

static int read_peer(struct midr_transport *transport,
			     struct midr_transport_peer *peer)
{
	uint8_t buffer[4096];

	for (;;) {
		ssize_t received = recv(peer->fd, buffer, sizeof(buffer), 0);

		if (received > 0) {
			uint64_t received_ns = transport_now_ns(transport);

			if ((size_t)received > peer->rx_capacity - peer->rx_length) {
				close_peer(transport, peer, -EMSGSIZE);
				return -EMSGSIZE;
			}
			memcpy(peer->rx_buffer + peer->rx_length, buffer,
			       (size_t)received);
			peer->rx_length += (size_t)received;
			{
				int ret = parse_rx(transport, peer, received_ns);

				if (ret)
					return ret;
			}
			continue;
		}
		if (received < 0 && errno == EINTR)
			continue;
		if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 0;
		if (received == 0) {
			close_peer(transport, peer, -ECONNRESET);
			return 0;
		}
		{
			int error = -errno;

			close_peer(transport, peer, error);
			return error;
		}
	}
}

static int connect_peer_now(struct midr_transport *transport,
				struct midr_transport_peer *peer, uint64_t now)
{
	int family = peer->endpoint.family == MIDR_TRANSPORT_AF_IPV4 ?
		AF_INET : AF_INET6;
	int fd;
	int ret;

	fd = socket(family, SOCK_STREAM, 0);
	if (fd < 0) {
		peer->next_connect_ms = now + MIDR_TRANSPORT_RECONNECT_MS;
		return -errno;
	}
	ret = set_nonblocking(fd);
	if (ret) {
		close(fd);
		peer->next_connect_ms = now + MIDR_TRANSPORT_RECONNECT_MS;
		return ret;
	}
	if (connect(fd, (struct sockaddr *)&peer->address, peer->address_len) == 0) {
		peer->fd = fd;
		establish_peer(transport, peer);
		return 0;
	}
	if (errno != EINPROGRESS) {
		ret = -errno;
		close(fd);
		peer->next_connect_ms = now + MIDR_TRANSPORT_RECONNECT_MS;
		/* A refused/unreachable peer is a normal reconnect condition. */
		return 0;
	}
	peer->fd = fd;
	peer->connecting = true;
	peer->established = false;
	return 0;
}

static void retry_connections(struct midr_transport *transport, uint64_t now)
{
	for (size_t i = 0; i < transport->peer_count; i++) {
		struct midr_transport_peer *peer = &transport->peers[i];

		if (!peer->used || !peer->desired || peer->fd >= 0 ||
		    now < peer->next_connect_ms)
			continue;
		(void)connect_peer_now(transport, peer, now);
	}
}

static void expire_peers(struct midr_transport *transport, uint64_t now)
{
	if (!transport->config.hold_time_ms)
		return;
	for (size_t i = 0; i < transport->peer_count; i++) {
		struct midr_transport_peer *peer = &transport->peers[i];

		if (peer->used && peer->established &&
		    now >= peer->last_rx_ms &&
		    now - peer->last_rx_ms >= transport->config.hold_time_ms)
			close_peer(transport, peer, -ETIMEDOUT);
	}
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
	*out = transport;
	return 0;
}

void midr_transport_destroy(struct midr_transport **transportp)
{
	if (!transportp || !*transportp)
		return;
	(void)midr_transport_stop(*transportp);
	free(*transportp);
	*transportp = NULL;
}

int midr_transport_start(struct midr_transport *transport)
{
	struct sockaddr_storage address;
	socklen_t address_len;
	int family;
	int one = 1;

	if (!transport || transport->started)
		return -EINVAL;
	if (endpoint_to_sockaddr(&transport->config.local, &address,
					&address_len))
		return -EINVAL;
	family = transport->config.local.family == MIDR_TRANSPORT_AF_IPV4 ?
		AF_INET : AF_INET6;
	transport->listen_fd = socket(family, SOCK_STREAM, 0);
	if (transport->listen_fd < 0)
		return -errno;
	(void)setsockopt(transport->listen_fd, SOL_SOCKET, SO_REUSEADDR,
				 &one, sizeof(one));
	if (family == AF_INET6)
		(void)setsockopt(transport->listen_fd, IPPROTO_IPV6, IPV6_V6ONLY,
				 &one, sizeof(one));
	if (bind(transport->listen_fd, (struct sockaddr *)&address, address_len) < 0) {
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
	if (set_nonblocking(transport->listen_fd)) {
		int error = errno;

		close(transport->listen_fd);
		transport->listen_fd = -1;
		return -error;
	}
	transport->started = true;
	return 0;
}

int midr_transport_stop(struct midr_transport *transport)
{
	if (!transport)
		return -EINVAL;
	if (transport->listen_fd >= 0)
		close(transport->listen_fd);
	transport->listen_fd = -1;
	for (size_t i = 0; i < transport->peer_count; i++) {
		struct midr_transport_peer *peer = &transport->peers[i];

		if (!peer->used)
			continue;
		peer->desired = false;
		close_peer(transport, peer, 0);
		free(peer->rx_buffer);
		peer->rx_buffer = NULL;
		peer->rx_capacity = 0;
		peer->used = false;
	}
	transport->peer_count = 0;
	transport->started = false;
	return 0;
}

int midr_transport_connect(struct midr_transport *transport,
			   const struct midr_transport_endpoint *endpoint)
{
	struct sockaddr_storage address;
	socklen_t address_len;
	struct midr_transport_peer *peer;
	uint64_t now;

	if (!transport || !transport->started ||
	    endpoint_to_sockaddr(endpoint, &address, &address_len))
		return -EINVAL;
	peer = ensure_peer(transport, endpoint, &address, address_len, true);
	if (!peer)
		return -ENOSPC;
	peer->desired = true;
	if (peer->fd >= 0)
		return 0;
	now = transport_now_ms(transport);
	peer->next_connect_ms = now;
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
	ret = queue_frame(peer, packet, length, encoded_ns);
	if (ret)
		return ret;
	return peer->established ? flush_peer(transport, peer) : 0;
}

static int accept_peers(struct midr_transport *transport)
{
	for (;;) {
		struct sockaddr_storage address;
		socklen_t address_len = sizeof(address);
		struct midr_transport_endpoint source, endpoint;
		struct midr_transport_peer *peer;
		int fd = accept(transport->listen_fd, (struct sockaddr *)&address,
					&address_len);

		if (fd < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return 0;
			return -errno;
		}
		if (set_nonblocking(fd)) {
			close(fd);
			continue;
		}
		sockaddr_to_endpoint(&address, &source);
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
				/* Keep an already selected connection.  In particular, do
				 * not replace an outbound connect-in-progress with its
				 * simultaneous inbound duplicate. */
				close(fd);
				continue;
			}
			/* A configured peer retains its destination port for future
			 * reconnects.  Accepted TCP sockets expose the remote's
			 * ephemeral source port, which must not replace that address. */
			if (!peer->desired) {
				peer->address = address;
				peer->address_len = address_len;
			}
			peer->fd = fd;
			establish_peer(transport, peer);
			continue;
		}
		endpoint = source;
		peer = ensure_peer(transport, &endpoint, &address, address_len,
					false);
		if (!peer) {
			close(fd);
			continue;
		}
		peer->fd = fd;
		establish_peer(transport, peer);
	}
}

int midr_transport_poll(struct midr_transport *transport, int timeout_ms)
{
	fd_set readfds, writefds;
	struct timeval timeout;
	uint64_t now;
	int max_fd;
	int ret;

	if (!transport || !transport->started || timeout_ms < 0)
		return -EINVAL;
	now = transport_now_ms(transport);
	expire_peers(transport, now);
	retry_connections(transport, now);
	FD_ZERO(&readfds);
	FD_ZERO(&writefds);
	FD_SET(transport->listen_fd, &readfds);
	max_fd = transport->listen_fd;
	for (size_t i = 0; i < transport->peer_count; i++) {
		struct midr_transport_peer *peer = &transport->peers[i];

		if (!peer->used || peer->fd < 0)
			continue;
		if (peer->established)
			FD_SET(peer->fd, &readfds);
		if (peer->connecting || peer->tx_head)
			FD_SET(peer->fd, &writefds);
		if (peer->fd > max_fd)
			max_fd = peer->fd;
	}
	timeout.tv_sec = timeout_ms / 1000;
	timeout.tv_usec = (timeout_ms % 1000) * 1000;
	ret = select(max_fd + 1, &readfds, &writefds, NULL, &timeout);
	if (ret < 0)
		return errno == EINTR ? 0 : -errno;
	if (FD_ISSET(transport->listen_fd, &readfds)) {
		ret = accept_peers(transport);
		if (ret)
			return ret;
	}
	for (size_t i = 0; i < transport->peer_count; i++) {
		struct midr_transport_peer *peer = &transport->peers[i];
		int fd;

		if (!peer->used || peer->fd < 0)
			continue;
		fd = peer->fd;
		if (peer->connecting && FD_ISSET(fd, &writefds)) {
			int error = 0;
			socklen_t length = sizeof(error);

			if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0)
				error = errno;
			if (error) {
				close_peer(transport, peer, -error);
				continue;
			}
			establish_peer(transport, peer);
		}
		if (peer->used && peer->fd == fd && peer->established &&
		    FD_ISSET(fd, &readfds)) {
			ret = read_peer(transport, peer);
			if (ret)
				return ret;
		}
		if (peer->used && peer->fd == fd && peer->established &&
		    FD_ISSET(fd, &writefds))
			(void)flush_peer(transport, peer);
	}
	now = transport_now_ms(transport);
	expire_peers(transport, now);
	retry_connections(transport, now);
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
