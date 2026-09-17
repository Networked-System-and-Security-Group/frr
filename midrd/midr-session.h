/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Native MIDR session control for the in-process discovery module.
 *
 * 第一组新增：midrd 原来只能用 --peer 静态配置邻居，第一组的邻居发现需要在运行时
 * 增删 transport 会话并得知会话起落，故补这组公开接口。函数与状态名按第二组
 * 《midrd 与第一组输入接口交接》第二版的公共 Session 服务命名；结构细节待与
 * 第二组的正式接口文档核对，第二组的实现到位后以其为准替换本文件。
 *
 * A session is keyed by the remote endpoint.  connect() registers the
 * discovery intent and returns; midrd keeps reconnecting while an intent
 * (discovery or a static --peer) exists.  A session becomes ESTABLISHED only
 * after the remote HELLO, whose node id is bound to the session.  Sessions
 * take part in flooding exactly like static --peer sessions.  Once an
 * observer is registered, inbound streams from endpoints without an intent
 * are closed, so the observer's admission policy also applies to the
 * passive side.
 *
 * All calls run on the midrd event thread.  Observers must not connect or
 * disconnect synchronously; schedule such work on the event loop.
 */
#ifndef MIDRD_SESSION_H
#define MIDRD_SESSION_H

#include <zebra.h>

#include <stdbool.h>
#include <stdint.h>

#include "ipaddr.h"
#include "midr-context.h"

struct midr_session_endpoint {
	struct ipaddr address;
	/* 0 selects the local MIDR listen port (the deployment's MIDR port). */
	uint16_t port;
	/* Interface index for an IPv6 link-local address, 0 otherwise. */
	uint32_t scope_id;
};

enum midr_session_close_reason {
	/* Discovery no longer wants this neighbor. */
	MIDR_SESSION_CLOSE_DISCOVERY = 0,
	/* Policy (e.g. Tier1 admission) rejects the neighbor. */
	MIDR_SESSION_CLOSE_POLICY,
	/* An operator removed the session. */
	MIDR_SESSION_CLOSE_ADMIN,
	/* The local node leaves MIDR. */
	MIDR_SESSION_CLOSE_SHUTDOWN,
};

enum midr_session_state {
	MIDR_SESSION_DOWN = 0,
	MIDR_SESSION_CONNECTING,
	MIDR_SESSION_ESTABLISHED,
	/* The remote HELLO named a node other than the one bound earlier. */
	MIDR_SESSION_IDENTITY_MISMATCH,
};

struct midr_session_status {
	enum midr_session_state state;
	/* Bound by the first valid HELLO; 0 until then. */
	uint32_t remote_node_id;
	/* Why the stream last closed: 0 = closed by the remote, otherwise a
	 * negative errno (hold timer, write budget, protocol error, ...). */
	int last_error;
};

struct midr_session_observer {
	void (*state_changed)(struct midr_context *ctx,
			      const struct midr_session_endpoint *remote,
			      const struct midr_session_status *status,
			      void *arg);
};

/* One observer per context.  Passing NULL unregisters it. */
int midr_session_observer_register(struct midr_context *ctx,
				   const struct midr_session_observer *observer,
				   void *arg);

/* Idempotent.  Returns 0 once the discovery intent is registered, -ENOSPC
 * when the session table is full and -EINVAL for an unusable endpoint. */
int midr_session_connect(struct midr_context *ctx,
			 const struct midr_session_endpoint *remote);
/* Clears the discovery intent only; a static --peer intent for the same
 * endpoint keeps its session.  Topology objects are not withdrawn.
 * Returns -ENOENT when no discovery intent exists. */
int midr_session_disconnect(struct midr_context *ctx,
			    const struct midr_session_endpoint *remote,
			    enum midr_session_close_reason reason);
/* Returns -ENOENT when the endpoint has no session. */
int midr_session_status_get(const struct midr_context *ctx,
			    const struct midr_session_endpoint *remote,
			    struct midr_session_status *status);

/* The local identity given on the midrd command line. */
uint32_t midr_context_node_id(const struct midr_context *ctx);
bool midr_context_listen_address(const struct midr_context *ctx,
				 struct ipaddr *address);

#endif /* MIDRD_SESSION_H */
