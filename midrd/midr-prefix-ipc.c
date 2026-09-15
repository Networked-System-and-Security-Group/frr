/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L

#include "midr-prefix-ipc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/select.h>
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
	char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	midr_prefix_ipc_event_cb on_event;
	void *arg;
	uint8_t rx[MIDR_PREFIX_IPC_RX_CAP];
	size_t rx_len;
};

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

static int set_nonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);

	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return -errno;
	return 0;
}

static void close_client(struct midr_prefix_ipc *ipc)
{
	if (ipc->fd >= 0)
		close(ipc->fd);
	ipc->fd = -1;
	ipc->rx_len = 0;
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
	strcpy(ipc->path, config->path);
	ipc->on_event = config->on_event;
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
	    listen(ipc->listener_fd, 1) < 0 || set_nonblocking(ipc->listener_fd)) {
		int error = errno;

		close(ipc->listener_fd);
		ipc->listener_fd = -1;
		(void)unlink(ipc->path);
		return -error;
	}
	ipc->started = true;
	return 0;
}

static int read_events(struct midr_prefix_ipc *ipc)
{
	ssize_t length;

	for (;;) {
		if (ipc->rx_len == sizeof(ipc->rx))
			return -EMSGSIZE;
		length = recv(ipc->fd, ipc->rx + ipc->rx_len,
				      sizeof(ipc->rx) - ipc->rx_len, 0);
		if (length == 0) {
			close_client(ipc);
			return 0;
		}
		if (length < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			return -errno;
		}
		ipc->rx_len += (size_t)length;
		while (ipc->rx_len >= MIDR_PREFIX_IPC_FRAME_LEN) {
			struct midr_prefix_event event;

			if (decode(ipc->rx, &event)) {
				close_client(ipc);
				return -EBADMSG;
			}
			if (ipc->on_event(ipc->arg, &event)) {
				close_client(ipc);
				return -EIO;
			}
			ipc->rx_len -= MIDR_PREFIX_IPC_FRAME_LEN;
			memmove(ipc->rx, ipc->rx + MIDR_PREFIX_IPC_FRAME_LEN,
				ipc->rx_len);
		}
	}
	return 0;
}

int midr_prefix_ipc_server_poll(struct midr_prefix_ipc *ipc, int timeout_ms)
{
	fd_set readfds;
	struct timeval timeout;
	int maxfd, ret;

	if (!ipc || !ipc->server || !ipc->started || timeout_ms < 0)
		return -EINVAL;
	FD_ZERO(&readfds);
	FD_SET(ipc->listener_fd, &readfds);
	maxfd = ipc->listener_fd;
	if (ipc->fd >= 0) {
		FD_SET(ipc->fd, &readfds);
		if (ipc->fd > maxfd)
			maxfd = ipc->fd;
	}
	timeout.tv_sec = timeout_ms / 1000;
	timeout.tv_usec = (timeout_ms % 1000) * 1000;
	ret = select(maxfd + 1, &readfds, NULL, NULL, &timeout);
	if (ret <= 0)
		return ret;
	if (FD_ISSET(ipc->listener_fd, &readfds)) {
		int fd = accept(ipc->listener_fd, NULL, NULL);

		if (fd >= 0) {
			if (ipc->fd >= 0)
				close_client(ipc);
			ipc->fd = fd;
			(void)set_nonblocking(ipc->fd);
		}
	}
	if (ipc->fd >= 0 && FD_ISSET(ipc->fd, &readfds))
		return read_events(ipc);
	return 0;
}

int midr_prefix_ipc_server_stop(struct midr_prefix_ipc *ipc)
{
	if (!ipc || !ipc->server)
		return -EINVAL;
	close_client(ipc);
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
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, path);
	if (connect(ipc->fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
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
	size_t offset = 0;
	ssize_t sent;

	if (!ipc || ipc->server || !ipc->started || encode(event, frame))
		return -EINVAL;
	while (offset < sizeof(frame)) {
		sent = send(ipc->fd, frame + offset, sizeof(frame) - offset, 0);
		if (sent < 0)
			return -errno;
		if (!sent)
			return -EPIPE;
		offset += (size_t)sent;
	}
	return 0;
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
