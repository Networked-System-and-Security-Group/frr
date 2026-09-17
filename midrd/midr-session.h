/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MIDRD_SESSION_H
#define MIDRD_SESSION_H

#include <stdint.h>

#include "ipaddr.h"
#include "midr-context.h"

/* Public MIDR session service shared by discovery and link-state flooding. */
struct midr_session_endpoint {
	struct ipaddr address;
	uint16_t port;
	uint32_t scope_id;
};

enum midr_session_state {
	MIDR_SESSION_DOWN = 0,
	MIDR_SESSION_CONNECTING,
	MIDR_SESSION_ESTABLISHED,
	MIDR_SESSION_IDENTITY_MISMATCH,
};

enum midr_session_close_reason {
	MIDR_SESSION_CLOSE_ADMIN = 0,
	MIDR_SESSION_CLOSE_NODE_DOWN,
	MIDR_SESSION_CLOSE_POLICY,
};

struct midr_session_status {
	struct midr_session_endpoint remote;
	uint32_t remote_node_id;
	enum midr_session_state state;
	uint64_t generation;
	int last_error;
};

struct midr_session_observer_ops {
	void (*state_changed)(struct midr_context *ctx,
			      const struct midr_session_status *status,
			      void *arg);
};

int midr_session_endpoint_validate(const struct midr_session_endpoint *endpoint);

/* Discovery owns the peering decision.  A successful return records that
 * intent and starts (or reuses) the common MIDR session machinery; it does
 * not mean that HELLO has completed. */
int midr_session_connect(struct midr_context *ctx,
			 const struct midr_session_endpoint *remote);
int midr_session_disconnect(struct midr_context *ctx,
			    const struct midr_session_endpoint *remote,
			    enum midr_session_close_reason reason);

int midr_session_status_get(struct midr_context *ctx,
			    const struct midr_session_endpoint *remote,
			    struct midr_session_status *status);
int midr_session_observer_register(
	struct midr_context *ctx, const struct midr_session_observer_ops *ops,
	void *arg);
void midr_session_observer_unregister(struct midr_context *ctx);

#endif /* MIDRD_SESSION_H */
