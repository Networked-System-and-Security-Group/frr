/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Native MIDR session control for the in-process discovery module.
 *
 * 第一组新增：midrd 原来只能用 --peer 静态配置邻居，第一组的邻居发现需要在运行时
 * 增删 transport 会话并得知会话起落，故补这组公开接口。
 *
 * A session is keyed by the remote transport address.  The remote port is the
 * local listen port: every midrd instance of one deployment listens on the
 * same MIDR port.  Requested sessions take part in flooding exactly like
 * statically configured --peer sessions.  Once a session owner is registered,
 * inbound streams from addresses that were not requested are closed, so the
 * owner's admission policy also applies to the passive side.
 *
 * All calls run on the midrd event thread.  Callbacks must not request or
 * release sessions synchronously; schedule such work on the event loop.
 */
#ifndef MIDRD_SESSION_H
#define MIDRD_SESSION_H

#include <zebra.h>

#include <stdbool.h>
#include <stdint.h>

#include "ipaddr.h"
#include "midr-context.h"

struct midr_session_ops {
	/* The stream to remote is up.  node_id is the identity the owner gave
	 * in midr_session_request(). */
	void (*session_up)(struct midr_context *ctx, uint32_t node_id,
			   const struct ipaddr *remote, void *arg);
	/* The stream to remote closed; midrd keeps reconnecting while the
	 * session remains requested. */
	void (*session_down)(struct midr_context *ctx,
			     const struct ipaddr *remote, int reason,
			     void *arg);
};

/* One owner per context.  Passing NULL ops unregisters it. */
int midr_session_owner_register(struct midr_context *ctx,
				const struct midr_session_ops *ops, void *arg);

/* Returns 0 when the session is (already) requested, -ENOSPC when the peer
 * table is full and -EINVAL for an address of the wrong family.  node_id may
 * be 0 when the remote identity is not known yet; the remote HELLO sets it. */
int midr_session_request(struct midr_context *ctx, uint32_t node_id,
			 const struct ipaddr *remote);
/* Returns 0 after the session was removed, -ENOENT if it was not requested. */
int midr_session_release(struct midr_context *ctx,
			 const struct ipaddr *remote);
bool midr_session_is_up(const struct midr_context *ctx,
			const struct ipaddr *remote);

/* The local identity given on the midrd command line. */
uint32_t midr_context_node_id(const struct midr_context *ctx);
bool midr_context_listen_address(const struct midr_context *ctx,
				 struct ipaddr *address);

#endif /* MIDRD_SESSION_H */
