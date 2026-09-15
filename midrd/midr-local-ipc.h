/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MIDRD_LOCAL_IPC_H
#define MIDRD_LOCAL_IPC_H

#include "midr-local-provider.h"

#define MIDR_LOCAL_IPC_FRAME_LEN 128U

typedef int (*midr_local_ipc_event_cb)(
	void *arg, const struct midr_local_event *event);
typedef void (*midr_local_ipc_disconnect_cb)(void *arg, int reason);

struct midr_local_ipc_config {
	const char *path;
	midr_local_ipc_event_cb on_event;
	midr_local_ipc_disconnect_cb on_disconnect;
	void *arg;
};

struct midr_local_ipc;

int midr_local_ipc_server_create(const struct midr_local_ipc_config *config,
				 struct midr_local_ipc **out);
int midr_local_ipc_server_start(struct midr_local_ipc *ipc);
int midr_local_ipc_server_poll(struct midr_local_ipc *ipc, int timeout_ms);
int midr_local_ipc_server_stop(struct midr_local_ipc *ipc);
void midr_local_ipc_server_destroy(struct midr_local_ipc **ipc);

int midr_local_ipc_client_connect(const char *path,
				  struct midr_local_ipc **out);
int midr_local_ipc_client_send(struct midr_local_ipc *ipc,
			       const struct midr_local_event *event);
int midr_local_ipc_client_close(struct midr_local_ipc *ipc);
void midr_local_ipc_client_destroy(struct midr_local_ipc **ipc);

#endif /* MIDRD_LOCAL_IPC_H */
