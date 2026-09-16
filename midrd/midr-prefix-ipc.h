/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MIDRD_PREFIX_IPC_H
#define MIDRD_PREFIX_IPC_H

#include <stddef.h>
#include <stdint.h>

#include "midr-prefix-provider.h"

struct event_loop;

#define MIDR_PREFIX_IPC_FRAME_LEN 48U

typedef int (*midr_prefix_ipc_event_cb)(
	void *arg, const struct midr_prefix_event *event);
typedef void (*midr_prefix_ipc_disconnect_cb)(void *arg, int reason);

struct midr_prefix_ipc_config {
	struct event_loop *master;
	const char *path;
	midr_prefix_ipc_event_cb on_event;
	midr_prefix_ipc_disconnect_cb on_disconnect;
	void *arg;
};

struct midr_prefix_ipc;

int midr_prefix_ipc_server_create(const struct midr_prefix_ipc_config *config,
				  struct midr_prefix_ipc **out);
int midr_prefix_ipc_server_start(struct midr_prefix_ipc *ipc);
int midr_prefix_ipc_server_poll(struct midr_prefix_ipc *ipc, int timeout_ms);
int midr_prefix_ipc_server_stop(struct midr_prefix_ipc *ipc);
void midr_prefix_ipc_server_destroy(struct midr_prefix_ipc **ipc);

/* Client calls are synchronous.  The provider adapter owns reconnect policy. */
int midr_prefix_ipc_client_connect(const char *path,
				   struct midr_prefix_ipc **out);
int midr_prefix_ipc_client_send(struct midr_prefix_ipc *ipc,
				const struct midr_prefix_event *event);
int midr_prefix_ipc_client_close(struct midr_prefix_ipc *ipc);
void midr_prefix_ipc_client_destroy(struct midr_prefix_ipc **ipc);

#endif /* MIDRD_PREFIX_IPC_H */
