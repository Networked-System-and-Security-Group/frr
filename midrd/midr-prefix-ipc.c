/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L

#include <zebra.h>

#include "buffer.h"
#include "frrevent.h"
#include "midr-prefix-ipc.h"
#include "network.h"
#include "stream.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define MIDR_PREFIX_IPC_MAGIC 0x4d504658U
#define MIDR_PREFIX_IPC_VERSION 1U
#define MIDR_PREFIX_IPC_RX_CAP (MIDR_PREFIX_IPC_FRAME_LEN * 8U)

struct midr_prefix_ipc {
	bool server;
	bool started;
	int listener_fd;
	int fd;
	struct event_loop *master;
	struct event *accept_event;
	struct event *read_event;
	struct stream *rx_stream;
	uint64_t connection_generation;
	int last_error;
	bool owns_master;
	char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	midr_prefix_ipc_event_cb on_event;
	midr_prefix_ipc_disconnect_cb on_disconnect;
	void *arg;
};

static void prefix_accept_ready(struct event *event);
static void prefix_read_ready(struct event *event);

static int set_no_sigpipe(int fd)
{
#ifdef SO_NOSIGPIPE
	int enabled = 1;

	if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
		       sizeof(enabled)) < 0)
		return -errno;
#else
	(void)fd;
#endif
	return 0;
}

static uint64_t host_to_be64(uint64_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return ((uint64_t)htonl((uint32_t)value) << 32) |
	       htonl((uint32_t)(value >> 32));
#else
	return value;
#endif
}

static uint64_t be64_to_host(uint64_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return ((uint64_t)ntohl((uint32_t)value) << 32) |
	       ntohl((uint32_t)(value >> 32));
#else
	return value;
#endif
}

static int encode(const struct midr_prefix_event *event,
		  uint8_t frame[MIDR_PREFIX_IPC_FRAME_LEN])
{
	uint32_t magic = htonl(MIDR_PREFIX_IPC_MAGIC);
	uint64_t generation;
	uint32_t originator;

	if (midr_prefix_event_validate(event))
		return -EINVAL;
	memset(frame, 0, MIDR_PREFIX_IPC_FRAME_LEN);
	memcpy(frame, &magic, sizeof(magic));
	frame[4] = MIDR_PREFIX_IPC_VERSION;
	frame[5] = (uint8_t)event->kind;
	generation = host_to_be64(event->generation);
	memcpy(frame + 8, &generation, sizeof(generation));
	originator = htonl(event->originator);
	memcpy(frame + 16, &originator, sizeof(originator));
	frame[20] = event->prefix.family;
	frame[21] = event->prefix.prefix_len;
	memcpy(frame + 24, event->prefix.address,
	       sizeof(event->prefix.address));
	{
		uint32_t metric = htonl(event->prefix.metric);

		memcpy(frame + 40, &metric, sizeof(metric));
	}
	return 0;
}

static int decode(const uint8_t frame[MIDR_PREFIX_IPC_FRAME_LEN],
		  struct midr_prefix_event *event)
{
	uint32_t magic, originator;
	uint64_t generation;

	memcpy(&magic, frame, sizeof(magic));
	if (ntohl(magic) != MIDR_PREFIX_IPC_MAGIC ||
	    frame[4] != MIDR_PREFIX_IPC_VERSION)
		return -EBADMSG;
	memset(event, 0, sizeof(*event));
	event->kind = (enum midr_prefix_event_kind)frame[5];
	memcpy(&generation, frame + 8, sizeof(generation));
	event->generation = be64_to_host(generation);
	memcpy(&originator, frame + 16, sizeof(originator));
	event->originator = ntohl(originator);
	event->prefix.family = frame[20];
	event->prefix.prefix_len = frame[21];
	memcpy(event->prefix.address, frame + 24,
	       sizeof(event->prefix.address));
	memcpy(&originator, frame + 40, sizeof(originator));
	event->prefix.metric = ntohl(originator);
	return midr_prefix_event_validate(event);
}

static void close_client(struct midr_prefix_ipc *ipc, int reason)
{
	bool was_open = ipc->fd >= 0;

	event_cancel(&ipc->read_event);
	if (ipc->fd >= 0)
		close(ipc->fd);
	ipc->fd = -1;
	if (ipc->rx_stream)
		stream_reset(ipc->rx_stream);
	if (was_open && ipc->on_disconnect)
		ipc->on_disconnect(ipc->arg, reason);
}

int midr_prefix_ipc_server_create(const struct midr_prefix_ipc_config *config,
				  struct midr_prefix_ipc **out)
{
	struct midr_prefix_ipc *ipc;

	if (!config || !out || *out || !config->path || !config->on_event ||
	    strlen(config->path) >= sizeof(ipc->path))
		return -EINVAL;
	ipc = calloc(1, sizeof(*ipc));
	if (!ipc)
		return -ENOMEM;
	ipc->server = true;
	ipc->listener_fd = -1;
	ipc->fd = -1;
	ipc->master = config->master;
	if (!ipc->master) {
		ipc->master = event_master_create("midr-prefix-ipc-test");
		ipc->owns_master = true;
	}
	ipc->rx_stream = stream_new(MIDR_PREFIX_IPC_RX_CAP);
	if (!ipc->rx_stream) {
		if (ipc->owns_master)
			event_master_free(ipc->master);
		free(ipc);
		return -ENOMEM;
	}
	strcpy(ipc->path, config->path);
	ipc->on_event = config->on_event;
	ipc->on_disconnect = config->on_disconnect;
	ipc->arg = config->arg;
	*out = ipc;
	return 0;
}

int midr_prefix_ipc_server_start(struct midr_prefix_ipc *ipc)
{
	struct sockaddr_un address = {0};

	if (!ipc || !ipc->server || ipc->started)
		return -EINVAL;
	ipc->listener_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (ipc->listener_fd < 0)
		return -errno;
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, ipc->path);
	(void)unlink(ipc->path);
	if (bind(ipc->listener_fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
	    listen(ipc->listener_fd, 1) < 0 ||
	    set_nonblocking(ipc->listener_fd) < 0) {
		int error = errno;

		close(ipc->listener_fd);
		ipc->listener_fd = -1;
		(void)unlink(ipc->path);
		return -error;
	}
	ipc->started = true;
	event_add_read(ipc->master, prefix_accept_ready, ipc,
		       ipc->listener_fd, &ipc->accept_event);
	return 0;
}

static int read_events(struct midr_prefix_ipc *ipc)
{
	for (;;) {
		if (!STREAM_WRITEABLE(ipc->rx_stream)) {
			close_client(ipc, -EMSGSIZE);
			return -EMSGSIZE;
		}
		ssize_t length = stream_read_try(ipc->rx_stream, ipc->fd,
						 STREAM_WRITEABLE(ipc->rx_stream));

		if (length == 0) {
			close_client(ipc, 0);
			return 0;
		}
		if (length < 0) {
			int error;

			if (length == -2)
				break;
			error = errno ? -errno : -EIO;
			close_client(ipc, error);
			return error;
		}
		while (STREAM_READABLE(ipc->rx_stream) >= MIDR_PREFIX_IPC_FRAME_LEN) {
			struct midr_prefix_event event;
			const uint8_t *data = STREAM_DATA(ipc->rx_stream) +
				stream_get_getp(ipc->rx_stream);

			if (decode(data, &event)) {
				close_client(ipc, -EBADMSG);
				return -EBADMSG;
			}
			{
				int ret = ipc->on_event(ipc->arg, &event);

				if (ret) {
					close_client(ipc, ret);
					return ret;
				}
			}
			stream_forward_getp(ipc->rx_stream, MIDR_PREFIX_IPC_FRAME_LEN);
			stream_pulldown(ipc->rx_stream);
		}
	}
	return 0;
}

static void prefix_read_ready(struct event *event)
{
	struct midr_prefix_ipc *ipc = EVENT_ARG(event);
	int fd = EVENT_FD(event);
	uint64_t generation = ipc->connection_generation;
	int ret;

	ipc->read_event = NULL;
	if (!ipc->started || ipc->fd != fd || generation != ipc->connection_generation)
		return;
	ret = read_events(ipc);
	if (ret && !ipc->last_error)
		ipc->last_error = ret;
	if (ipc->started && ipc->fd == fd &&
	    generation == ipc->connection_generation)
		event_add_read(ipc->master, prefix_read_ready, ipc, fd,
			       &ipc->read_event);
	(void)ret;
}

static void prefix_accept_ready(struct event *event)
{
	struct midr_prefix_ipc *ipc = EVENT_ARG(event);
	int listener = EVENT_FD(event);

	ipc->accept_event = NULL;
	if (!ipc->started || ipc->listener_fd != listener)
		return;
	for (;;) {
		int fd = accept(listener, NULL, NULL);

		if (fd < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			if (!ipc->last_error)
				ipc->last_error = -errno;
			break;
		}
		if (set_nonblocking(fd) < 0) {
			close(fd);
			continue;
		}
		if (ipc->fd >= 0)
			close_client(ipc, -ECONNABORTED);
		ipc->fd = fd;
		ipc->connection_generation++;
		stream_reset(ipc->rx_stream);
		event_add_read(ipc->master, prefix_read_ready, ipc, fd,
			       &ipc->read_event);
	}
	if (ipc->started && ipc->listener_fd == listener)
		event_add_read(ipc->master, prefix_accept_ready, ipc, listener,
			       &ipc->accept_event);
}

static void ipc_poll_timeout(struct event *event)
{
	(void)event;
}

int midr_prefix_ipc_server_poll(struct midr_prefix_ipc *ipc, int timeout_ms)
{
	struct event *timeout_event = NULL;
	struct event ready;

	if (!ipc || !ipc->server || !ipc->started || timeout_ms < 0)
		return -EINVAL;
	event_add_timer_msec(ipc->master, ipc_poll_timeout, ipc, timeout_ms,
			     &timeout_event);
	if (event_fetch(ipc->master, &ready))
		event_call(&ready);
	event_cancel(&timeout_event);
	if (ipc->last_error) {
		int ret = ipc->last_error;

		ipc->last_error = 0;
		return ret;
	}
	return 0;
}

int midr_prefix_ipc_server_stop(struct midr_prefix_ipc *ipc)
{
	if (!ipc || !ipc->server)
		return -EINVAL;
	event_cancel(&ipc->accept_event);
	close_client(ipc, 0);
	if (ipc->listener_fd >= 0)
		close(ipc->listener_fd);
	ipc->listener_fd = -1;
	if (ipc->started)
		(void)unlink(ipc->path);
	ipc->started = false;
	return 0;
}

void midr_prefix_ipc_server_destroy(struct midr_prefix_ipc **ipcp)
{
	if (!ipcp || !*ipcp)
		return;
	(void)midr_prefix_ipc_server_stop(*ipcp);
	stream_free((*ipcp)->rx_stream);
	if ((*ipcp)->owns_master)
		event_master_free((*ipcp)->master);
	free(*ipcp);
	*ipcp = NULL;
}

int midr_prefix_ipc_client_connect(const char *path,
				   struct midr_prefix_ipc **out)
{
	struct midr_prefix_ipc *ipc;
	struct sockaddr_un address = {0};

	if (!path || !out || *out || strlen(path) >= sizeof(address.sun_path))
		return -EINVAL;
	ipc = calloc(1, sizeof(*ipc));
	if (!ipc)
		return -ENOMEM;
	ipc->fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (ipc->fd < 0) {
		free(ipc);
		return -errno;
	}
	if (set_no_sigpipe(ipc->fd)) {
		int error = errno;

		close(ipc->fd);
		free(ipc);
		return -error;
	}
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, path);
	if (connect(ipc->fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		int error = errno;

		close(ipc->fd);
		free(ipc);
		return -error;
	}
	if (set_nonblocking(ipc->fd) < 0) {
		int error = errno;

		close(ipc->fd);
		free(ipc);
		return -error;
	}
	ipc->started = true;
	*out = ipc;
	return 0;
}

int midr_prefix_ipc_client_send(struct midr_prefix_ipc *ipc,
				const struct midr_prefix_event *event)
{
	uint8_t frame[MIDR_PREFIX_IPC_FRAME_LEN];
	struct buffer *buffer;
	buffer_status_t status;

	if (!ipc || ipc->server || !ipc->started || encode(event, frame))
		return -EINVAL;
	buffer = buffer_new(sizeof(frame));
	if (!buffer)
		return -ENOMEM;
	buffer_put(buffer, frame, sizeof(frame));
	status = buffer_flush_all(buffer, ipc->fd);
	buffer_free(buffer);
	if (status == BUFFER_EMPTY)
		return 0;
	{
		int error = errno ? -errno : -EAGAIN;

		(void)midr_prefix_ipc_client_close(ipc);
		return error;
	}
}

int midr_prefix_ipc_client_close(struct midr_prefix_ipc *ipc)
{
	if (!ipc || ipc->server)
		return -EINVAL;
	if (ipc->fd >= 0)
		close(ipc->fd);
	ipc->fd = -1;
	ipc->started = false;
	return 0;
}

void midr_prefix_ipc_client_destroy(struct midr_prefix_ipc **ipcp)
{
	if (!ipcp || !*ipcp)
		return;
	(void)midr_prefix_ipc_client_close(*ipcp);
	free(*ipcp);
	*ipcp = NULL;
}
