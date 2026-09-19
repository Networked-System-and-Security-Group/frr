/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <zebra.h>

#include "frrevent.h"
#include "midr-context-private.h"
#include "midr-session-private.h"
#include "midr-wire.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define MIDR_SESSION_MAX_PEERS 128U
#define MIDR_SESSION_SOURCE_STATIC (1U << 0)
#define MIDR_SESSION_SOURCE_DISCOVERY (1U << 1)

struct midr_session_peer {
	struct midr_transport_endpoint endpoint;
	uint32_t sources;
	uint32_t remote_node_id;
	enum midr_session_state state;
	uint64_t generation;
	int last_error;
	bool used;
	/* An accepted stream whose kernel-chosen source does not uniquely
	 * identify a configured peer.  It is not a MIDR neighbour until a
	 * HELLO advertises the peer's listening endpoint (see
	 * hello_identify()).  Provisional peers never carry sources and are
	 * never reported as established. */
	bool provisional;
};

struct midr_session_manager {
	struct midr_context *ctx;
	struct midr_session_manager_config config;
	struct midr_session_protocol_ops protocol;
	struct midr_session_observer_ops observer;
	void *observer_arg;
	struct midr_transport *transport;
	struct midr_session_peer peers[MIDR_SESSION_MAX_PEERS];
	struct event *keepalive_event;
	uint64_t frame_sequence;
	bool observer_registered;
};

static int endpoint_to_transport(
	const struct midr_session_endpoint *endpoint,
	struct midr_transport_endpoint *transport)
{
	int family;

	if (midr_session_endpoint_validate(endpoint) || !transport)
		return -EINVAL;
	memset(transport, 0, sizeof(*transport));
	family = ipaddr_family(&endpoint->address);
	transport->family = family == AF_INET ? MIDR_TRANSPORT_AF_IPV4
					       : MIDR_TRANSPORT_AF_IPV6;
	transport->port = endpoint->port;
	transport->scope_id = endpoint->scope_id;
	memcpy(transport->address, endpoint->address.ip.addrbytes,
	       family == AF_INET ? sizeof(struct in_addr)
				 : sizeof(struct in6_addr));
	return 0;
}

static void endpoint_from_transport(
	const struct midr_transport_endpoint *transport,
	struct midr_session_endpoint *endpoint)
{
	memset(endpoint, 0, sizeof(*endpoint));
	endpoint->port = transport->port;
	endpoint->scope_id = transport->scope_id;
	if (transport->family == MIDR_TRANSPORT_AF_IPV4) {
		SET_IPADDR_V4(&endpoint->address);
		memcpy(&endpoint->address.ipaddr_v4, transport->address,
		       sizeof(endpoint->address.ipaddr_v4));
	} else {
		SET_IPADDR_V6(&endpoint->address);
		memcpy(&endpoint->address.ipaddr_v6, transport->address,
		       sizeof(endpoint->address.ipaddr_v6));
	}
}

int midr_session_endpoint_validate(const struct midr_session_endpoint *endpoint)
{
	int family;

	if (!endpoint || !endpoint->port || ipaddr_is_zero(&endpoint->address))
		return -EINVAL;
	family = ipaddr_family(&endpoint->address);
	if (family == AF_INET)
		return endpoint->scope_id ? -EINVAL : 0;
	if (family != AF_INET6)
		return -EINVAL;
	if (IN6_IS_ADDR_LINKLOCAL(&endpoint->address.ipaddr_v6))
		return endpoint->scope_id ? 0 : -EINVAL;
	return endpoint->scope_id ? -EINVAL : 0;
}

static struct midr_session_peer *peer_find(
	struct midr_session_manager *manager,
	const struct midr_transport_endpoint *endpoint)
{
	for (size_t i = 0; i < array_size(manager->peers); i++)
		if (manager->peers[i].used &&
		    midr_transport_endpoint_equal(&manager->peers[i].endpoint,
						  endpoint))
			return &manager->peers[i];
	return NULL;
}

static const struct midr_session_peer *peer_find_const(
	const struct midr_session_manager *manager,
	const struct midr_transport_endpoint *endpoint)
{
	for (size_t i = 0; i < array_size(manager->peers); i++)
		if (manager->peers[i].used &&
		    midr_transport_endpoint_equal(&manager->peers[i].endpoint,
						  endpoint))
			return &manager->peers[i];
	return NULL;
}

static struct midr_session_peer *peer_ensure(
	struct midr_session_manager *manager,
	const struct midr_transport_endpoint *endpoint)
{
	struct midr_session_peer *peer = peer_find(manager, endpoint);

	if (peer)
		return peer;
	for (size_t i = 0; i < array_size(manager->peers); i++) {
		if (manager->peers[i].used)
			continue;
		peer = &manager->peers[i];
		memset(peer, 0, sizeof(*peer));
		peer->endpoint = *endpoint;
		peer->state = MIDR_SESSION_DOWN;
		peer->used = true;
		return peer;
	}
	return NULL;
}

/* Find a configured (non-provisional) session peer by its listening endpoint.
 * Scope is ignored because HELLO does not carry it and two listeners cannot
 * share an address and port on one host anyway. */
static struct midr_session_peer *peer_find_configured(
	struct midr_session_manager *manager,
	const struct midr_transport_endpoint *endpoint)
{
	for (size_t i = 0; i < array_size(manager->peers); i++) {
		struct midr_session_peer *peer = &manager->peers[i];

		if (!peer->used || peer->provisional)
			continue;
		if (peer->endpoint.family == endpoint->family &&
		    peer->endpoint.port == endpoint->port &&
		    !memcmp(peer->endpoint.address, endpoint->address,
			    sizeof(peer->endpoint.address)))
			return peer;
	}
	return NULL;
}

static struct midr_session_peer *peer_ensure_provisional(
	struct midr_session_manager *manager,
	const struct midr_transport_endpoint *endpoint)
{
	struct midr_session_peer *peer = peer_find(manager, endpoint);

	if (peer)
		return peer;
	peer = peer_ensure(manager, endpoint);
	if (peer)
		peer->provisional = true;
	return peer;
}

static void status_fill(const struct midr_session_peer *peer,
			struct midr_session_status *status)
{
	memset(status, 0, sizeof(*status));
	endpoint_from_transport(&peer->endpoint, &status->remote);
	status->remote_node_id = peer->remote_node_id;
	status->state = peer->state;
	status->generation = peer->generation;
	status->last_error = peer->last_error;
}

static void observer_notify(struct midr_session_manager *manager,
			    const struct midr_session_peer *peer)
{
	struct midr_session_status status;

	if (!manager->observer_registered ||
	    !manager->observer.state_changed || !manager->ctx)
		return;
	status_fill(peer, &status);
	manager->observer.state_changed(manager->ctx, &status,
					manager->observer_arg);
}

static int send_raw(struct midr_session_manager *manager,
		    const struct midr_transport_endpoint *peer, uint8_t type,
		    const uint8_t *payload, size_t payload_len,
		    uint64_t encoded_ns)
{
	struct midr_transport_frame frame = {
		.version = MIDR_WIRE_VERSION,
		.type = type,
		.sequence = ++manager->frame_sequence,
		.encoded_ns = encoded_ns,
		.payload = payload,
		.payload_len = payload_len,
	};

	return midr_transport_send(manager->transport, peer, &frame);
}

static int send_hello(struct midr_session_manager *manager,
		      const struct midr_transport_endpoint *peer)
{
	const struct midr_transport_endpoint *local =
		&manager->config.transport.local;
	uint8_t payload[28] = {0};
	uint32_t node = htonl(manager->config.local_node_id);
	uint32_t hold = htonl(manager->config.transport.hold_time_ms);
	uint16_t port = htons(local->port);

	memcpy(payload, &node, sizeof(node));
	memcpy(payload + 4, &hold, sizeof(hold));
	payload[8] = local->family;
	memcpy(payload + 10, &port, sizeof(port));
	memcpy(payload + 12, local->address, sizeof(local->address));
	return send_raw(manager, peer, MIDR_WIRE_HELLO, payload,
			sizeof(payload), 0);
}

static void transport_established(
	void *arg, const struct midr_transport_endpoint *endpoint)
{
	struct midr_session_manager *manager = arg;
	struct midr_session_peer *peer = peer_find(manager, endpoint);

	/* A stream whose source does not uniquely select a configured peer
	 * (changed for a loopback alias, shared by several peers, ...) is held
	 * as a provisional candidate until its HELLO names the peer. */
	if (!peer) {
		(void)peer_ensure_provisional(manager, endpoint);
		return;
	}
	if (peer->provisional)
		return;
	/* Both endpoints must express an intent.  An arbitrary inbound socket
	 * is not a MIDR neighbor until it belongs to the shared session
	 * registry. */
	if (!peer->sources) {
		(void)midr_transport_disconnect(manager->transport, endpoint);
		return;
	}
	peer->state = MIDR_SESSION_CONNECTING;
	peer->generation = 0;
	peer->last_error = 0;
	observer_notify(manager, peer);
	(void)send_hello(manager, endpoint);
}

static int hello_receive(struct midr_session_manager *manager,
			 struct midr_session_peer *peer,
			 const struct midr_transport_frame *frame)
{
	uint32_t node_id;

	if (frame->payload_len != 28U)
		return -EBADMSG;
	memcpy(&node_id, frame->payload, sizeof(node_id));
	node_id = ntohl(node_id);
	if (!node_id || node_id == manager->config.local_node_id)
		return -EPROTO;
	if (peer->remote_node_id && peer->remote_node_id != node_id) {
		peer->state = MIDR_SESSION_IDENTITY_MISMATCH;
		peer->generation = frame->generation;
		peer->last_error = -EEXIST;
		observer_notify(manager, peer);
		return -EPROTO;
	}
	if (peer->state == MIDR_SESSION_ESTABLISHED &&
	    peer->generation == frame->generation)
		return 0;
	peer->remote_node_id = node_id;
	peer->state = MIDR_SESSION_ESTABLISHED;
	peer->generation = frame->generation;
	peer->last_error = 0;
	observer_notify(manager, peer);
	if (manager->protocol.established)
		manager->protocol.established(manager->protocol.arg,
					      &peer->endpoint, node_id,
					      frame->generation);
	return 0;
}

/* Decode the listening endpoint a HELLO advertises for its sender.  The
 * payload layout mirrors send_hello(): node id, hold time, family, port and
 * the sender's address. */
static int hello_listen_endpoint(const struct midr_transport_frame *frame,
				 struct midr_transport_endpoint *endpoint)
{
	uint16_t port;

	if (frame->payload_len != 28U)
		return -EBADMSG;
	memset(endpoint, 0, sizeof(*endpoint));
	endpoint->family = frame->payload[8];
	memcpy(&port, frame->payload + 10, sizeof(port));
	endpoint->port = ntohs(port);
	memcpy(endpoint->address, frame->payload + 12,
	       sizeof(endpoint->address));
	return midr_transport_endpoint_validate(endpoint) ? -EBADMSG : 0;
}

/* Resolve a provisional accepted stream to the configured peer whose
 * listening endpoint its HELLO advertises, then finish the handshake on that
 * peer.  A HELLO from a node that never expressed an intent is rejected, which
 * closes the stream (the transport reports the error back through
 * transport_closed()).  midr_transport_promote() applies the deterministic
 * simultaneous-connect arbitration: it either keeps this stream (and
 * re-issues on_established for the configured endpoint) or keeps the
 * configured outbound stream and drops this one. */
static int hello_identify(struct midr_session_manager *manager,
			  struct midr_session_peer *provisional,
			  const struct midr_transport_frame *frame)
{
	struct midr_transport_endpoint advertised;
	struct midr_transport_endpoint from = provisional->endpoint;
	struct midr_session_peer *peer;
	int ret;

	if (hello_listen_endpoint(frame, &advertised))
		return -EBADMSG;
	peer = peer_find_configured(manager, &advertised);
	if (!peer)
		return -EPERM;
	ret = midr_transport_promote(manager->transport, &from, &advertised);
	if (ret == -EALREADY)
		return 0;
	if (ret)
		return -EPERM;
	/* The accepted stream now answers to the configured endpoint.  Drop the
	 * provisional candidate and complete the HELLO exchange on the real
	 * peer (on_established() already moved it back to CONNECTING). */
	memset(provisional, 0, sizeof(*provisional));
	peer = peer_find(manager, &advertised);
	if (!peer)
		return -EPERM;
	return hello_receive(manager, peer, frame);
}

static int transport_frame(void *arg,
			   const struct midr_transport_endpoint *endpoint,
			   const struct midr_transport_frame *frame)
{
	struct midr_session_manager *manager = arg;
	struct midr_session_peer *peer = peer_find(manager, endpoint);

	if (peer && peer->provisional) {
		if (frame->type != MIDR_WIRE_HELLO)
			return -EPERM;
		return hello_identify(manager, peer, frame);
	}
	if (!peer || !peer->sources)
		return -EPERM;
	if (frame->type == MIDR_WIRE_HELLO)
		return hello_receive(manager, peer, frame);
	if (peer->state != MIDR_SESSION_ESTABLISHED ||
	    peer->generation != frame->generation)
		return -EPROTO;
	if (frame->type == MIDR_WIRE_KEEPALIVE)
		return frame->payload_len ? -EBADMSG : 0;
	return manager->protocol.frame
		       ? manager->protocol.frame(manager->protocol.arg, endpoint,
					 frame)
		       : 0;
}

static void transport_closed(void *arg,
			     const struct midr_transport_endpoint *endpoint,
			     int reason)
{
	struct midr_session_manager *manager = arg;
	struct midr_session_peer *peer = peer_find(manager, endpoint);
	bool was_established;
	uint64_t generation;
	uint32_t remote_node_id;

	if (!peer)
		return;
	if (peer->provisional) {
		/* A candidate that never named a configured peer: discard it
		 * silently -- it was never a MIDR neighbour. */
		memset(peer, 0, sizeof(*peer));
		return;
	}
	was_established = peer->state == MIDR_SESSION_ESTABLISHED;
	generation = peer->generation;
	remote_node_id = peer->remote_node_id;
	peer->state = MIDR_SESSION_DOWN;
	peer->generation = 0;
	peer->last_error = reason;
	if (manager->protocol.closed && was_established)
		manager->protocol.closed(manager->protocol.arg, endpoint,
					 remote_node_id, generation, reason);
	observer_notify(manager, peer);
}

static void transport_frame_written(
	void *arg, const struct midr_transport_endpoint *endpoint,
	uint64_t generation, uint64_t sequence)
{
	struct midr_session_manager *manager = arg;

	if (manager->protocol.frame_written)
		manager->protocol.frame_written(manager->protocol.arg, endpoint,
						 generation, sequence);
}

static void transport_frame_dropped(
	void *arg, const struct midr_transport_endpoint *endpoint,
	uint64_t generation, uint64_t sequence, int reason)
{
	struct midr_session_manager *manager = arg;

	if (manager->protocol.frame_dropped)
		manager->protocol.frame_dropped(manager->protocol.arg, endpoint,
						 generation, sequence, reason);
}

static void keepalive_timer(struct event *event)
{
	struct midr_session_manager *manager = EVENT_ARG(event);

	manager->keepalive_event = NULL;
	for (size_t i = 0; i < array_size(manager->peers); i++) {
		struct midr_session_peer *peer = &manager->peers[i];

		if (!peer->used || !peer->sources)
			continue;
		if (peer->state == MIDR_SESSION_ESTABLISHED)
			(void)send_raw(manager, &peer->endpoint,
					   MIDR_WIRE_KEEPALIVE, NULL, 0, 0);
		else if (peer->state == MIDR_SESSION_CONNECTING)
			(void)send_hello(manager, &peer->endpoint);
	}
	event_add_timer_msec(midr_transport_master(manager->transport),
			     keepalive_timer, manager,
			     manager->config.hello_interval_ms,
			     &manager->keepalive_event);
}

int midr_session_manager_create(
	const struct midr_session_manager_config *config,
	const struct midr_session_protocol_ops *protocol,
	struct midr_session_manager **out)
{
	struct midr_transport_callbacks callbacks = {
		.on_frame = transport_frame,
		.on_established = transport_established,
		.on_closed = transport_closed,
		.on_frame_written = transport_frame_written,
		.on_frame_dropped = transport_frame_dropped,
	};
	struct midr_session_manager *manager;
	int ret;

	if (!config || !protocol || !out || *out || !config->local_node_id ||
	    !config->hello_interval_ms)
		return -EINVAL;
	manager = calloc(1, sizeof(*manager));
	if (!manager)
		return -ENOMEM;
	manager->config = *config;
	manager->protocol = *protocol;
	callbacks.arg = manager;
	ret = midr_transport_create(&config->transport, &callbacks,
				    &manager->transport);
	if (!ret)
		ret = midr_transport_start(manager->transport);
	if (ret) {
		midr_transport_destroy(&manager->transport);
		free(manager);
		return ret;
	}
	if (midr_transport_master(manager->transport))
		event_add_timer_msec(midr_transport_master(manager->transport),
				     keepalive_timer, manager,
				     config->hello_interval_ms,
				     &manager->keepalive_event);
	*out = manager;
	return 0;
}

void midr_session_manager_destroy(struct midr_session_manager **managerp)
{
	struct midr_session_manager *manager;

	if (!managerp || !(manager = *managerp))
		return;
	event_cancel(&manager->keepalive_event);
	midr_transport_destroy(&manager->transport);
	free(manager);
	*managerp = NULL;
}

static int request_source(struct midr_session_manager *manager,
			  const struct midr_transport_endpoint *endpoint,
			  uint32_t source)
{
	struct midr_session_peer *peer;
	int ret;

	if (!manager || midr_transport_endpoint_validate(endpoint))
		return -EINVAL;
	peer = peer_ensure(manager, endpoint);
	if (!peer)
		return -ENOSPC;
	if (peer->sources & source)
		return 0;
	peer->sources |= source;
	if (peer->state == MIDR_SESSION_DOWN) {
		peer->state = MIDR_SESSION_CONNECTING;
		peer->last_error = 0;
		observer_notify(manager, peer);
	}
	ret = midr_transport_connect(manager->transport, endpoint);
	if (ret == -EINVAL || ret == -ENOSPC) {
		peer->sources &= ~source;
		if (!peer->sources)
			memset(peer, 0, sizeof(*peer));
		return ret;
	}
	/* Once transport accepted the desired peer, transient socket failures are
	 * retried internally and do not reject the discovery intent. */
	return 0;
}

static int release_source(struct midr_session_manager *manager,
			  const struct midr_transport_endpoint *endpoint,
			  uint32_t source)
{
	struct midr_session_peer *peer;
	int ret;

	if (!manager || !endpoint)
		return -EINVAL;
	peer = peer_find(manager, endpoint);
	if (!peer || !(peer->sources & source))
		return 0;
	peer->sources &= ~source;
	if (peer->sources)
		return 0;
	ret = midr_transport_disconnect(manager->transport, endpoint);
	if (ret && ret != -ENOENT) {
		peer->sources |= source;
		return ret;
	}
	if (peer->state != MIDR_SESSION_DOWN) {
		peer->state = MIDR_SESSION_DOWN;
		peer->generation = 0;
		peer->last_error = 0;
		observer_notify(manager, peer);
	}
	memset(peer, 0, sizeof(*peer));
	return 0;
}

int midr_session_manager_request_static(
	struct midr_session_manager *manager,
	const struct midr_transport_endpoint *peer)
{
	return request_source(manager, peer, MIDR_SESSION_SOURCE_STATIC);
}

int midr_session_connect(struct midr_context *ctx,
			 const struct midr_session_endpoint *remote)
{
	struct midr_transport_endpoint endpoint;

	if (!ctx || !ctx->sessions)
		return -ENOENT;
	if (endpoint_to_transport(remote, &endpoint))
		return -EINVAL;
	return request_source(ctx->sessions, &endpoint,
			      MIDR_SESSION_SOURCE_DISCOVERY);
}

int midr_session_disconnect(struct midr_context *ctx,
			    const struct midr_session_endpoint *remote,
			    enum midr_session_close_reason reason)
{
	struct midr_transport_endpoint endpoint;

	if (!ctx || !ctx->sessions)
		return -ENOENT;
	if (reason < MIDR_SESSION_CLOSE_ADMIN ||
	    reason > MIDR_SESSION_CLOSE_POLICY ||
	    endpoint_to_transport(remote, &endpoint))
		return -EINVAL;
	return release_source(ctx->sessions, &endpoint,
			      MIDR_SESSION_SOURCE_DISCOVERY);
}

int midr_session_status_get(struct midr_context *ctx,
			    const struct midr_session_endpoint *remote,
			    struct midr_session_status *status)
{
	struct midr_transport_endpoint endpoint;
	const struct midr_session_peer *peer;

	if (!ctx || !ctx->sessions)
		return -ENOENT;
	if (!status || endpoint_to_transport(remote, &endpoint))
		return -EINVAL;
	peer = peer_find_const(ctx->sessions, &endpoint);
	if (!peer || !(peer->sources & MIDR_SESSION_SOURCE_DISCOVERY))
		return -ENOENT;
	status_fill(peer, status);
	return 0;
}

int midr_session_observer_register(
	struct midr_context *ctx, const struct midr_session_observer_ops *ops,
	void *arg)
{
	struct midr_session_manager *manager;

	if (!ctx || !(manager = ctx->sessions))
		return -ENOENT;
	if (!ops || !ops->state_changed)
		return -EINVAL;
	if (manager->observer_registered)
		return -EALREADY;
	manager->ctx = ctx;
	manager->observer = *ops;
	manager->observer_arg = arg;
	manager->observer_registered = true;
	return 0;
}

void midr_session_observer_unregister(struct midr_context *ctx)
{
	struct midr_session_manager *manager;

	if (!ctx || !(manager = ctx->sessions))
		return;
	memset(&manager->observer, 0, sizeof(manager->observer));
	manager->observer_arg = NULL;
	manager->observer_registered = false;
}

void midr_session_manager_foreach_established(
	struct midr_session_manager *manager, midr_session_peer_cb callback,
	void *arg)
{
	if (!manager || !callback)
		return;
	for (size_t i = 0; i < array_size(manager->peers); i++) {
		const struct midr_session_peer *peer = &manager->peers[i];

		if (peer->used && peer->sources &&
		    peer->state == MIDR_SESSION_ESTABLISHED)
			callback(arg, &peer->endpoint, peer->remote_node_id,
				 peer->generation);
	}
}

uint32_t midr_session_manager_remote_node_id(
	const struct midr_session_manager *manager,
	const struct midr_transport_endpoint *endpoint)
{
	const struct midr_session_peer *peer;

	if (!manager || !endpoint ||
	    !(peer = peer_find_const(manager, endpoint)) ||
	    peer->state != MIDR_SESSION_ESTABLISHED)
		return 0;
	return peer->remote_node_id;
}

int midr_session_manager_send(
	struct midr_session_manager *manager,
	const struct midr_transport_endpoint *endpoint, uint8_t type,
	const uint8_t *payload, size_t payload_len, uint64_t encoded_ns)
{
	const struct midr_session_peer *peer;

	if (!manager || !endpoint ||
	    !(peer = peer_find_const(manager, endpoint)))
		return -ENOENT;
	if (peer->state != MIDR_SESSION_ESTABLISHED)
		return -ENOTCONN;
	if (type == MIDR_WIRE_HELLO || type == MIDR_WIRE_KEEPALIVE)
		return -EINVAL;
	return send_raw(manager, endpoint, type, payload, payload_len,
			encoded_ns);
}

int midr_session_manager_poll(struct midr_session_manager *manager,
			     int timeout_ms)
{
	return manager ? midr_transport_poll(manager->transport, timeout_ms)
		       : -EINVAL;
}

size_t midr_session_manager_pending(
	const struct midr_session_manager *manager)
{
	return manager ? midr_transport_pending(manager->transport) : 0;
}
