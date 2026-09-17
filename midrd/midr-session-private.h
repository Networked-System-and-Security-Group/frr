/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MIDRD_SESSION_PRIVATE_H
#define MIDRD_SESSION_PRIVATE_H

#include <stddef.h>
#include <stdint.h>

#include "midr-session.h"
#include "midr-transport.h"

struct midr_session_manager;

struct midr_session_protocol_ops {
	void (*established)(void *arg,
			    const struct midr_transport_endpoint *peer,
			    uint32_t remote_node_id, uint64_t generation);
	void (*closed)(void *arg, const struct midr_transport_endpoint *peer,
		       uint32_t remote_node_id, uint64_t generation, int reason);
	int (*frame)(void *arg, const struct midr_transport_endpoint *peer,
		     const struct midr_transport_frame *frame);
	void (*frame_written)(void *arg,
			      const struct midr_transport_endpoint *peer,
			      uint64_t generation, uint64_t sequence);
	void (*frame_dropped)(void *arg,
			      const struct midr_transport_endpoint *peer,
			      uint64_t generation, uint64_t sequence,
			      int reason);
	void *arg;
};

struct midr_session_manager_config {
	uint32_t local_node_id;
	uint32_t hello_interval_ms;
	struct midr_transport_config transport;
};

typedef void (*midr_session_peer_cb)(
	void *arg, const struct midr_transport_endpoint *peer,
	uint32_t remote_node_id, uint64_t generation);

int midr_session_manager_create(
	const struct midr_session_manager_config *config,
	const struct midr_session_protocol_ops *protocol,
	struct midr_session_manager **out);
void midr_session_manager_destroy(struct midr_session_manager **manager);

int midr_session_manager_request_static(
	struct midr_session_manager *manager,
	const struct midr_transport_endpoint *peer);
void midr_session_manager_foreach_established(
	struct midr_session_manager *manager, midr_session_peer_cb callback,
	void *arg);
uint32_t midr_session_manager_remote_node_id(
	const struct midr_session_manager *manager,
	const struct midr_transport_endpoint *peer);

int midr_session_manager_send(
	struct midr_session_manager *manager,
	const struct midr_transport_endpoint *peer, uint8_t type,
	const uint8_t *payload, size_t payload_len, uint64_t encoded_ns);
int midr_session_manager_poll(struct midr_session_manager *manager,
			     int timeout_ms);
size_t midr_session_manager_pending(
	const struct midr_session_manager *manager);

#endif /* MIDRD_SESSION_PRIVATE_H */
