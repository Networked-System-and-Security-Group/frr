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

#define MIDR_TRANSPORT_MAX_PEERS 128U
#define MIDR_TRANSPORT_MAX_PACKET 65535U

struct midr_transport_peer {
	struct midr_transport_endpoint endpoint;
	struct sockaddr_storage address;
	socklen_t address_len;
	bool active;
};

struct midr_transport {
	struct midr_transport_config config;
	struct midr_transport_callbacks callbacks;
	struct midr_transport_peer peers[MIDR_TRANSPORT_MAX_PEERS];
	size_t peer_count;
	int fd;
	bool started;
};

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
		if (transport->peers[i].active &&
		    midr_transport_endpoint_equal(&transport->peers[i].endpoint,
						  endpoint))
			return &transport->peers[i];
	return NULL;
}

static struct midr_transport_peer *ensure_peer(
	struct midr_transport *transport,
	const struct midr_transport_endpoint *endpoint,
	const struct sockaddr_storage *address, socklen_t address_len)
{
	struct midr_transport_peer *peer = find_peer(transport, endpoint);

	if (peer)
		return peer;
	if (transport->peer_count == MIDR_TRANSPORT_MAX_PEERS)
		return NULL;
	peer = &transport->peers[transport->peer_count++];
	memset(peer, 0, sizeof(*peer));
	peer->endpoint = *endpoint;
	peer->address = *address;
	peer->address_len = address_len;
	peer->active = true;
	if (transport->callbacks.on_established)
		transport->callbacks.on_established(transport->callbacks.arg,
						    endpoint);
	return peer;
}

int midr_transport_create(const struct midr_transport_config *config,
			  const struct midr_transport_callbacks *callbacks,
			  struct midr_transport **out)
{
	struct midr_transport *transport;

	if (!config || !callbacks || !callbacks->on_frame || !out || *out ||
	    midr_transport_endpoint_validate(&config->local) ||
	    !config->max_frame_size)
		return -EINVAL;
	transport = calloc(1, sizeof(*transport));
	if (!transport)
		return -ENOMEM;
	transport->config = *config;
	transport->callbacks = *callbacks;
	transport->fd = -1;
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

	if (!transport || transport->started)
		return -EINVAL;
	if (endpoint_to_sockaddr(&transport->config.local, &address, &address_len))
		return -EINVAL;
	family = transport->config.local.family == MIDR_TRANSPORT_AF_IPV4
		 ? AF_INET : AF_INET6;
	transport->fd = socket(family, SOCK_DGRAM, 0);
	if (transport->fd < 0)
		return -errno;
	if (family == AF_INET6) {
		int only = 1;
		(void)setsockopt(transport->fd, IPPROTO_IPV6, IPV6_V6ONLY, &only,
				 sizeof(only));
	}
	if (bind(transport->fd, (struct sockaddr *)&address, address_len) < 0) {
		int error = errno;
		close(transport->fd);
		transport->fd = -1;
		return -error;
	}
	(void)fcntl(transport->fd, F_SETFL, O_NONBLOCK);
	transport->started = true;
	return 0;
}

int midr_transport_stop(struct midr_transport *transport)
{
	if (!transport)
		return -EINVAL;
	if (transport->started) {
		for (size_t i = 0; i < transport->peer_count; i++)
			if (transport->peers[i].active && transport->callbacks.on_closed)
				transport->callbacks.on_closed(transport->callbacks.arg,
						       &transport->peers[i].endpoint, 0);
		close(transport->fd);
		transport->fd = -1;
		transport->started = false;
	}
	transport->peer_count = 0;
	return 0;
}

int midr_transport_connect(struct midr_transport *transport,
			   const struct midr_transport_endpoint *endpoint)
{
	struct sockaddr_storage address;
	socklen_t address_len;

	if (!transport || !transport->started ||
	    endpoint_to_sockaddr(endpoint, &address, &address_len))
		return -EINVAL;
	return ensure_peer(transport, endpoint, &address, address_len) ? 0 : -ENOSPC;
}

int midr_transport_disconnect(struct midr_transport *transport,
			      const struct midr_transport_endpoint *endpoint)
{
	struct midr_transport_peer *peer;

	if (!transport || !endpoint || !(peer = find_peer(transport, endpoint)))
		return -ENOENT;
	peer->active = false;
	if (transport->callbacks.on_closed)
		transport->callbacks.on_closed(transport->callbacks.arg, endpoint, 0);
	return 0;
}

int midr_transport_send(struct midr_transport *transport,
			const struct midr_transport_endpoint *endpoint,
			const struct midr_transport_frame *frame)
{
	struct midr_transport_peer *peer;
	struct midr_wire_frame wire_frame;
	uint8_t packet[MIDR_TRANSPORT_MAX_PACKET];
	size_t length;
	ssize_t sent;

	if (!transport || !transport->started || !frame || !endpoint ||
	    !(peer = find_peer(transport, endpoint)))
		return -EINVAL;
	wire_frame.version = frame->version;
	wire_frame.type = frame->type;
	wire_frame.flags = frame->flags;
	wire_frame.sequence = frame->sequence;
	wire_frame.payload = frame->payload;
	wire_frame.payload_len = frame->payload_len;
	if (midr_wire_encode_frame(&wire_frame, packet, sizeof(packet), &length) ||
	    length > transport->config.max_frame_size)
		return -EMSGSIZE;
	sent = sendto(transport->fd, packet, length, 0,
		      (struct sockaddr *)&peer->address, peer->address_len);
	return sent == (ssize_t)length ? 0 : -errno;
}

int midr_transport_poll(struct midr_transport *transport, int timeout_ms)
{
	uint8_t packet[MIDR_TRANSPORT_MAX_PACKET];
	struct sockaddr_storage source;
	socklen_t source_len;
	struct midr_transport_endpoint endpoint;
	struct midr_wire_frame wire_frame;
	struct midr_transport_peer *peer;
	fd_set readfds;
	struct timeval timeout;
	ssize_t length;
	int ret;

	if (!transport || !transport->started || timeout_ms < 0)
		return -EINVAL;
	FD_ZERO(&readfds);
	FD_SET(transport->fd, &readfds);
	timeout.tv_sec = timeout_ms / 1000;
	timeout.tv_usec = (timeout_ms % 1000) * 1000;
	ret = select(transport->fd + 1, &readfds, NULL, NULL, &timeout);
	if (ret <= 0)
		return ret;
	source_len = sizeof(source);
	length = recvfrom(transport->fd, packet, sizeof(packet), 0,
			  (struct sockaddr *)&source, &source_len);
	if (length < 0)
		return -errno;
	if (midr_wire_decode_frame(packet, (size_t)length, &wire_frame))
		return -EBADMSG;
	sockaddr_to_endpoint(&source, &endpoint);
	if (midr_transport_endpoint_validate(&endpoint))
		return -EAFNOSUPPORT;
	peer = ensure_peer(transport, &endpoint, &source, source_len);
	if (!peer)
		return -ENOSPC;
	struct midr_transport_frame frame = {
		.version = wire_frame.version,
		.type = wire_frame.type,
		.flags = wire_frame.flags,
		.sequence = wire_frame.sequence,
		.payload = wire_frame.payload,
		.payload_len = wire_frame.payload_len,
	};
	return transport->callbacks.on_frame(transport->callbacks.arg,
					    &peer->endpoint, &frame);
}

size_t midr_transport_peer_count(const struct midr_transport *transport)
{
	size_t count = 0;

	if (!transport)
		return 0;
	for (size_t i = 0; i < transport->peer_count; i++)
		if (transport->peers[i].active)
			count++;
	return count;
}
