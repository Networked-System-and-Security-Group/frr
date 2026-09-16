/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L

#include "midr-local-ipc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
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
	char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	midr_local_ipc_event_cb on_event;
	midr_local_ipc_disconnect_cb on_disconnect;
	void *arg;
	uint8_t rx[MIDR_LOCAL_IPC_RX_CAP];
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

static int set_nonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);

	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return -errno;
	return 0;
}

static void close_client(struct midr_local_ipc *ipc, int reason)
{
	bool was_open;

	if (!ipc)
		return;
	was_open = ipc->fd >= 0;
	if (was_open)
		(void)close(ipc->fd);
	ipc->fd = -1;
	ipc->rx_len = 0;
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
	ret = set_nonblocking(ipc->listener_fd);
	if (ret)
		goto failed;
	ipc->started = true;
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

		if (ipc->rx_len == sizeof(ipc->rx)) {
			close_client(ipc, -EMSGSIZE);
			return -EMSGSIZE;
		}
		length = recv(ipc->fd, ipc->rx + ipc->rx_len,
			      sizeof(ipc->rx) - ipc->rx_len, 0);
		if (!length) {
			close_client(ipc, -ECONNRESET);
			return -ECONNRESET;
		}
		if (length < 0) {
			int error = errno;

			if (error == EINTR)
				continue;
			if (error == EAGAIN || error == EWOULDBLOCK)
				return 0;
			close_client(ipc, -error);
			return -error;
		}
		ipc->rx_len += (size_t)length;
		while (ipc->rx_len >= MIDR_LOCAL_IPC_FRAME_LEN) {
			struct midr_local_event event;
			int ret = decode(ipc->rx, &event);

			if (ret) {
				close_client(ipc, ret);
				return ret;
			}
			ret = ipc->on_event(ipc->arg, &event);
			if (ret) {
				close_client(ipc, ret);
				return ret;
			}
			ipc->rx_len -= MIDR_LOCAL_IPC_FRAME_LEN;
			memmove(ipc->rx, ipc->rx + MIDR_LOCAL_IPC_FRAME_LEN,
				ipc->rx_len);
		}
	}
}

int midr_local_ipc_server_poll(struct midr_local_ipc *ipc, int timeout_ms)
{
	fd_set readfds;
	struct timeval timeout;
	int maxfd;
	int ret;

	if (!ipc || !ipc->server || !ipc->started || timeout_ms < 0)
		return -EINVAL;
	for (;;) {
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
		if (ret >= 0 || errno != EINTR)
			break;
	}
	if (ret < 0)
		return -errno;
	if (!ret)
		return 0;
	if (FD_ISSET(ipc->listener_fd, &readfds)) {
		int fd;

		do {
			fd = accept(ipc->listener_fd, NULL, NULL);
		} while (fd < 0 && errno == EINTR);
		if (fd < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				return -errno;
		} else {
			if (ipc->fd >= 0)
				close_client(ipc, -ECONNABORTED);
			ipc->fd = fd;
			ret = set_nonblocking(ipc->fd);
			if (ret) {
				close_client(ipc, ret);
				return ret;
			}
		}
	}
	if (ipc->fd >= 0 && FD_ISSET(ipc->fd, &readfds))
		return read_events(ipc);
	return 0;
}

int midr_local_ipc_server_stop(struct midr_local_ipc *ipc)
{
	if (!ipc || !ipc->server)
		return -EINVAL;
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
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, path);
	if (connect(ipc->fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
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
	size_t offset = 0;
	int ret;

	if (!ipc || ipc->server || !ipc->started)
		return -EINVAL;
	ret = encode(event, frame);
	if (ret)
		return ret;
	while (offset < sizeof(frame)) {
		ssize_t sent;
		int flags = 0;

#ifdef MSG_NOSIGNAL
		flags = MSG_NOSIGNAL;
#endif
		sent = send(ipc->fd, frame + offset, sizeof(frame) - offset,
			    flags);
		if (sent < 0) {
			int error = errno;

			if (error == EINTR)
				continue;
			(void)midr_local_ipc_client_close(ipc);
			return -error;
		}
		if (!sent) {
			(void)midr_local_ipc_client_close(ipc);
			return -EPIPE;
		}
		offset += (size_t)sent;
	}
	return 0;
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
