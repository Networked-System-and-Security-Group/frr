/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L

#include <zebra.h>

#include "buffer.h"
#include "frrevent.h"
#include "midr-local-ipc.h"
#include "network.h"
#include "stream.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define MIDR_LOCAL_IPC_MAGIC 0x4d4c4643U
#define MIDR_LOCAL_IPC_VERSION 2U
#define MIDR_LOCAL_IPC_RX_CAP (MIDR_LOCAL_IPC_FRAME_LEN * 8U)

struct midr_local_ipc {
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
	midr_local_ipc_event_cb on_event;
	midr_local_ipc_disconnect_cb on_disconnect;
	void *arg;
};

static void local_accept_ready(struct event *event);
static void local_read_ready(struct event *event);

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

static void put_u32(uint8_t *destination, uint32_t value)
{
	value = htonl(value);
	memcpy(destination, &value, sizeof(value));
}

static uint32_t get_u32(const uint8_t *source)
{
	uint32_t value;

	memcpy(&value, source, sizeof(value));
	return ntohl(value);
}

static void put_u64(uint8_t *destination, uint64_t value)
{
	value = host_to_be64(value);
	memcpy(destination, &value, sizeof(value));
}

static uint64_t get_u64(const uint8_t *source)
{
	uint64_t value;

	memcpy(&value, source, sizeof(value));
	return be64_to_host(value);
}

static bool bytes_are_zero(const uint8_t *bytes, size_t length)
{
	for (size_t i = 0; i < length; i++)
		if (bytes[i])
			return false;
	return true;
}

static int encode(const struct midr_local_event *event,
		  uint8_t frame[MIDR_LOCAL_IPC_FRAME_LEN])
{
	const struct midr_local_link *link;

	if (midr_local_event_validate(event))
		return -EINVAL;
	memset(frame, 0, MIDR_LOCAL_IPC_FRAME_LEN);
	put_u32(frame, MIDR_LOCAL_IPC_MAGIC);
	frame[4] = MIDR_LOCAL_IPC_VERSION;
	frame[5] = (uint8_t)event->kind;
	put_u64(frame + 8, event->generation);
	put_u32(frame + 16, event->originator);
	if (event->kind == MIDR_LOCAL_MEMBERSHIP ||
	    event->kind == MIDR_LOCAL_MEMBERSHIP_WITHDRAW) {
		put_u32(frame + 20, event->fact.membership.group);
		put_u64(frame + 24, event->fact.membership.version);
		return 0;
	}
	if (event->kind != MIDR_LOCAL_LINK &&
	    event->kind != MIDR_LOCAL_LINK_WITHDRAW)
		return 0;
	link = &event->fact.link;
	put_u32(frame + 32, link->remote_node_id);
	put_u32(frame + 120, link->local_ifindex);
	frame[36] = link->family;
	put_u64(frame + 40, link->link_id);
	put_u64(frame + 48, link->version);
	put_u32(frame + 56, link->rtt_us);
	put_u32(frame + 60, link->loss_ppm);
	put_u32(frame + 64, link->available_bandwidth_kbps);
	put_u64(frame + 72, link->measurement_sequence);
	put_u64(frame + 80, link->measurement_timestamp_ms);
	memcpy(frame + 88, link->local_address, sizeof(link->local_address));
	memcpy(frame + 104, link->remote_address,
	       sizeof(link->remote_address));
	return 0;
}

static int decode(const uint8_t frame[MIDR_LOCAL_IPC_FRAME_LEN],
		  struct midr_local_event *event)
{
	struct midr_local_link *link;

	if (!frame || !event || get_u32(frame) != MIDR_LOCAL_IPC_MAGIC ||
	    frame[4] != MIDR_LOCAL_IPC_VERSION ||
	    !bytes_are_zero(frame + 6, 2))
		return -EBADMSG;
	memset(event, 0, sizeof(*event));
	event->kind = (enum midr_local_event_kind)frame[5];
	event->generation = get_u64(frame + 8);
	event->originator = get_u32(frame + 16);
	if (event->kind == MIDR_LOCAL_MEMBERSHIP ||
	    event->kind == MIDR_LOCAL_MEMBERSHIP_WITHDRAW) {
		if (!bytes_are_zero(frame + 32, 96))
			return -EBADMSG;
		event->fact.membership.group = get_u32(frame + 20);
		event->fact.membership.version = get_u64(frame + 24);
	} else if (event->kind == MIDR_LOCAL_LINK ||
		   event->kind == MIDR_LOCAL_LINK_WITHDRAW) {
		if (!bytes_are_zero(frame + 20, 12) ||
		    !bytes_are_zero(frame + 37, 3) ||
		    !bytes_are_zero(frame + 68, 4) ||
		    !bytes_are_zero(frame + 124, 4))
			return -EBADMSG;
		link = &event->fact.link;
		link->remote_node_id = get_u32(frame + 32);
		link->local_ifindex = get_u32(frame + 120);
		link->family = frame[36];
		link->link_id = get_u64(frame + 40);
		link->version = get_u64(frame + 48);
		link->rtt_us = get_u32(frame + 56);
		link->loss_ppm = get_u32(frame + 60);
		link->available_bandwidth_kbps = get_u32(frame + 64);
		link->measurement_sequence = get_u64(frame + 72);
		link->measurement_timestamp_ms = get_u64(frame + 80);
		memcpy(link->local_address, frame + 88,
		       sizeof(link->local_address));
		memcpy(link->remote_address, frame + 104,
		       sizeof(link->remote_address));
	} else if (!bytes_are_zero(frame + 20, 108)) {
		return -EBADMSG;
	}
	return midr_local_event_validate(event) ? -EBADMSG : 0;
}

static void close_client(struct midr_local_ipc *ipc, int reason)
{
	bool was_open;

	if (!ipc)
		return;
	event_cancel(&ipc->read_event);
	was_open = ipc->fd >= 0;
	if (was_open)
		(void)close(ipc->fd);
	ipc->fd = -1;
	if (ipc->rx_stream)
		stream_reset(ipc->rx_stream);
	if (was_open && ipc->on_disconnect)
		ipc->on_disconnect(ipc->arg, reason);
}

int midr_local_ipc_server_create(const struct midr_local_ipc_config *config,
				 struct midr_local_ipc **out)
{
	struct midr_local_ipc *ipc;

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
		ipc->master = event_master_create("midr-local-ipc-test");
		ipc->owns_master = true;
	}
	ipc->rx_stream = stream_new(MIDR_LOCAL_IPC_RX_CAP);
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

int midr_local_ipc_server_start(struct midr_local_ipc *ipc)
{
	struct sockaddr_un address = {0};
	int ret;

	if (!ipc || !ipc->server || ipc->started)
		return -EINVAL;
	ipc->listener_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (ipc->listener_fd < 0)
		return -errno;
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, ipc->path);
	(void)unlink(ipc->path);
	if (bind(ipc->listener_fd, (struct sockaddr *)&address,
		 sizeof(address)) < 0 || listen(ipc->listener_fd, 1) < 0) {
		ret = -errno;
		goto failed;
	}
	if (set_nonblocking(ipc->listener_fd) < 0) {
		ret = -errno;
		goto failed;
	}
	ipc->started = true;
	event_add_read(ipc->master, local_accept_ready, ipc,
		       ipc->listener_fd, &ipc->accept_event);
	return 0;
failed:
	(void)close(ipc->listener_fd);
	ipc->listener_fd = -1;
	(void)unlink(ipc->path);
	return ret;
}

static int read_events(struct midr_local_ipc *ipc)
{
	for (;;) {
		ssize_t length;

		if (!STREAM_WRITEABLE(ipc->rx_stream)) {
			close_client(ipc, -EMSGSIZE);
			return -EMSGSIZE;
		}
		length = stream_read_try(ipc->rx_stream, ipc->fd,
					 STREAM_WRITEABLE(ipc->rx_stream));
		if (!length) {
			close_client(ipc, -ECONNRESET);
			return -ECONNRESET;
		}
		if (length < 0) {
			int error;

			if (length == -2)
				return 0;
			error = errno ? -errno : -EIO;
			close_client(ipc, error);
			return error;
		}
		while (STREAM_READABLE(ipc->rx_stream) >= MIDR_LOCAL_IPC_FRAME_LEN) {
			struct midr_local_event event;
			const uint8_t *data = STREAM_DATA(ipc->rx_stream) +
				stream_get_getp(ipc->rx_stream);
			int ret = decode(data, &event);

			if (ret) {
				close_client(ipc, ret);
				return ret;
			}
			ret = ipc->on_event(ipc->arg, &event);
			if (ret) {
				close_client(ipc, ret);
				return ret;
			}
			stream_forward_getp(ipc->rx_stream, MIDR_LOCAL_IPC_FRAME_LEN);
			stream_pulldown(ipc->rx_stream);
		}
	}
}

static void local_read_ready(struct event *event)
{
	struct midr_local_ipc *ipc = EVENT_ARG(event);
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
		event_add_read(ipc->master, local_read_ready, ipc, fd,
			       &ipc->read_event);
}

static void local_accept_ready(struct event *event)
{
	struct midr_local_ipc *ipc = EVENT_ARG(event);
	int listener = EVENT_FD(event);

	ipc->accept_event = NULL;
	if (!ipc->started || ipc->listener_fd != listener)
		return;
	for (;;) {
		int fd = accept(listener, NULL, NULL);

		if (fd < 0) {
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN && errno != EWOULDBLOCK &&
			    !ipc->last_error)
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
		event_add_read(ipc->master, local_read_ready, ipc, fd,
			       &ipc->read_event);
	}
	if (ipc->started && ipc->listener_fd == listener)
		event_add_read(ipc->master, local_accept_ready, ipc, listener,
			       &ipc->accept_event);
}

static void ipc_poll_timeout(struct event *event)
{
	(void)event;
}

int midr_local_ipc_server_poll(struct midr_local_ipc *ipc, int timeout_ms)
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

int midr_local_ipc_server_stop(struct midr_local_ipc *ipc)
{
	if (!ipc || !ipc->server)
		return -EINVAL;
	event_cancel(&ipc->accept_event);
	close_client(ipc, 0);
	if (ipc->listener_fd >= 0)
		(void)close(ipc->listener_fd);
	ipc->listener_fd = -1;
	if (ipc->started)
		(void)unlink(ipc->path);
	ipc->started = false;
	return 0;
}

void midr_local_ipc_server_destroy(struct midr_local_ipc **ipcp)
{
	if (!ipcp || !*ipcp)
		return;
	(void)midr_local_ipc_server_stop(*ipcp);
	stream_free((*ipcp)->rx_stream);
	if ((*ipcp)->owns_master)
		event_master_free((*ipcp)->master);
	free(*ipcp);
	*ipcp = NULL;
}

int midr_local_ipc_client_connect(const char *path,
				  struct midr_local_ipc **out)
{
	struct midr_local_ipc *ipc;
	struct sockaddr_un address = {0};

	if (!path || !out || *out || strlen(path) >= sizeof(address.sun_path))
		return -EINVAL;
	ipc = calloc(1, sizeof(*ipc));
	if (!ipc)
		return -ENOMEM;
	ipc->listener_fd = -1;
	ipc->fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (ipc->fd < 0) {
		int error = errno;

		free(ipc);
		return -error;
	}
	if (set_no_sigpipe(ipc->fd)) {
		int error = errno;

		(void)close(ipc->fd);
		free(ipc);
		return -error;
	}
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, path);
	if (connect(ipc->fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		int error = errno;

		(void)close(ipc->fd);
		free(ipc);
		return -error;
	}
	if (set_nonblocking(ipc->fd) < 0) {
		int error = errno;

		(void)close(ipc->fd);
		free(ipc);
		return -error;
	}
	ipc->started = true;
	*out = ipc;
	return 0;
}

int midr_local_ipc_client_send(struct midr_local_ipc *ipc,
			       const struct midr_local_event *event)
{
	uint8_t frame[MIDR_LOCAL_IPC_FRAME_LEN];
	struct buffer *buffer;
	buffer_status_t status;
	int ret;

	if (!ipc || ipc->server || !ipc->started)
		return -EINVAL;
	ret = encode(event, frame);
	if (ret)
		return ret;
	buffer = buffer_new(sizeof(frame));
	if (!buffer)
		return -ENOMEM;
	buffer_put(buffer, frame, sizeof(frame));
	status = buffer_flush_all(buffer, ipc->fd);
	buffer_free(buffer);
	if (status == BUFFER_EMPTY)
		return 0;
	ret = errno ? -errno : -EAGAIN;
	(void)midr_local_ipc_client_close(ipc);
	return ret;
}

int midr_local_ipc_client_close(struct midr_local_ipc *ipc)
{
	if (!ipc || ipc->server)
		return -EINVAL;
	if (ipc->fd >= 0)
		(void)close(ipc->fd);
	ipc->fd = -1;
	ipc->started = false;
	return 0;
}

void midr_local_ipc_client_destroy(struct midr_local_ipc **ipcp)
{
	if (!ipcp || !*ipcp)
		return;
	(void)midr_local_ipc_client_close(*ipcp);
	free(*ipcp);
	*ipcp = NULL;
}
