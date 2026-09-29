#include "midr-local-ipc.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define TEST_MAGIC 0x4d4c4643U
#define TEST_VERSION 2U

struct event_log {
	struct midr_local_event events[16];
	size_t count;
	int callback_error;
	int reasons[16];
	size_t disconnects;
};

static int record_event(void *arg, const struct midr_local_event *event)
{
	struct event_log *log = arg;

	if (log->callback_error)
		return log->callback_error;
	assert(log->count < 16);
	log->events[log->count++] = *event;
	return 0;
}

static void disconnected(void *arg, int reason)
{
	struct event_log *log = arg;

	assert(log->disconnects < 16);
	log->reasons[log->disconnects++] = reason;
}

static int poll_until_result(struct midr_local_ipc *server)
{
	for (unsigned int i = 0; i < 8; i++) {
		int ret = midr_local_ipc_server_poll(server, 10);

		if (ret)
			return ret;
	}
	return 0;
}

static int raw_connect(const char *path)
{
	struct sockaddr_un address = {0};
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);

	assert(fd >= 0);
	address.sun_family = AF_UNIX;
	assert(strlen(path) < sizeof(address.sun_path));
	strcpy(address.sun_path, path);
	assert(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
	return fd;
}

static void raw_send_all(int fd, const uint8_t *bytes, size_t length)
{
	size_t offset = 0;

	while (offset < length) {
		ssize_t sent = send(fd, bytes + offset, length - offset, 0);

		if (sent < 0 && errno == EINTR)
			continue;
		assert(sent > 0);
		offset += (size_t)sent;
	}
}

static void put_u32(uint8_t *destination, uint32_t value)
{
	value = htonl(value);
	memcpy(destination, &value, sizeof(value));
}

static void put_u64(uint8_t *destination, uint64_t value)
{
	for (unsigned int i = 0; i < 8; i++)
		destination[7U - i] = (uint8_t)(value >> (i * 8U));
}

static struct midr_local_event control_event(enum midr_local_event_kind kind,
					     uint64_t generation)
{
	struct midr_local_event event = {
		.kind = kind,
		.generation = generation,
		.originator = 77,
	};

	return event;
}

static struct midr_local_event link_event(uint64_t generation, uint8_t family,
					  uint8_t suffix)
{
	struct midr_local_event event = control_event(MIDR_LOCAL_LINK,
							generation);

	event.fact.link.remote_node_id = 88U + suffix;
	event.fact.link.local_ifindex = 100U + suffix;
	event.fact.link.family = family;
	event.fact.link.link_id = 12U + suffix;
	event.fact.link.version = 7;
	event.fact.link.rtt_us = 1000;
	event.fact.link.loss_ppm = 2000;
	event.fact.link.available_bandwidth_kbps = 1000000;
	event.fact.link.measurement_sequence = 22;
	event.fact.link.measurement_timestamp_ms = 33;
	if (family == MIDR_CORE_AF_IPV4) {
		event.fact.link.local_address[0] = 192;
		event.fact.link.local_address[1] = 0;
		event.fact.link.local_address[2] = 2;
		event.fact.link.local_address[3] = suffix;
		event.fact.link.remote_address[0] = 198;
		event.fact.link.remote_address[1] = 51;
		event.fact.link.remote_address[2] = 100;
		event.fact.link.remote_address[3] = suffix;
	} else {
		event.fact.link.local_address[0] = 0x20;
		event.fact.link.local_address[1] = 0x01;
		event.fact.link.local_address[2] = 0x0d;
		event.fact.link.local_address[3] = 0xb8;
		event.fact.link.local_address[15] = suffix;
		event.fact.link.remote_address[0] = 0x20;
		event.fact.link.remote_address[1] = 0x01;
		event.fact.link.remote_address[2] = 0x0d;
		event.fact.link.remote_address[3] = 0xb8;
		event.fact.link.remote_address[15] = (uint8_t)(suffix + 1U);
	}
	return event;
}

int main(void)
{
	char path[96];
	struct event_log log = {0};
	struct midr_local_ipc_config config = {
		.path = path,
		.on_event = record_event,
		.on_disconnect = disconnected,
		.arg = &log,
	};
	struct midr_local_ipc *server = NULL;
	struct midr_local_ipc *client = NULL;
	struct midr_local_event events[8];
	uint8_t malformed[MIDR_LOCAL_IPC_FRAME_LEN] = {0};
	int fd;

	assert(snprintf(path, sizeof(path), "/tmp/midrd-local-ipc-%ld.sock",
			(long)getpid()) > 0);
	events[0] = control_event(MIDR_LOCAL_SNAPSHOT_BEGIN, 4);
	events[1] = control_event(MIDR_LOCAL_MEMBERSHIP, 4);
	events[1].fact.membership.group = 9;
	events[1].fact.membership.version = 3;
	events[2] = link_event(4, MIDR_CORE_AF_IPV4, 1);
	events[3] = link_event(4, MIDR_CORE_AF_IPV6, 2);
	events[4] = control_event(MIDR_LOCAL_SNAPSHOT_END, 4);
	events[5] = control_event(MIDR_LOCAL_EOR, 4);
	events[6] = control_event(MIDR_LOCAL_MEMBERSHIP_WITHDRAW, 4);
	events[6].fact.membership.version = 4;
	events[7] = control_event(MIDR_LOCAL_LINK_WITHDRAW, 4);
	events[7].fact.link.remote_node_id = 89;
	events[7].fact.link.link_id = 14;
	events[7].fact.link.version = 8;

	(void)unlink(path);
	assert(midr_local_ipc_server_create(&config, &server) == 0);
	assert(midr_local_ipc_server_start(server) == 0);
	assert(midr_local_ipc_client_connect(path, &client) == 0);
	for (size_t i = 0; i < sizeof(events) / sizeof(events[0]); i++)
		assert(midr_local_ipc_client_send(client, &events[i]) == 0);
	{
		struct midr_local_event invalid = events[2];

		invalid.fact.link.reserved[0] = 1;
		assert(midr_local_ipc_client_send(client, &invalid) == -EINVAL);
	}
	assert(poll_until_result(server) == 0);
	assert(log.count == 8);
	assert(log.events[2].fact.link.family == MIDR_CORE_AF_IPV4);
	assert(log.events[2].fact.link.local_ifindex == 101);
	assert(log.events[2].fact.link.local_address[3] == 1);
	assert(log.events[3].fact.link.family == MIDR_CORE_AF_IPV6);
	assert(log.events[3].fact.link.remote_address[15] == 3);
	assert(log.events[6].kind == MIDR_LOCAL_MEMBERSHIP_WITHDRAW &&
	       log.events[6].fact.membership.group == 0);
	assert(log.events[7].kind == MIDR_LOCAL_LINK_WITHDRAW &&
	       log.events[7].fact.link.remote_node_id == 89 &&
	       log.events[7].fact.link.family == MIDR_CORE_AF_NONE);
	midr_local_ipc_client_destroy(&client);
	assert(poll_until_result(server) == -ECONNRESET);
	assert(log.reasons[log.disconnects - 1U] == -ECONNRESET);

	fd = raw_connect(path);
	assert(midr_local_ipc_server_poll(server, 10) == 0);
	put_u32(malformed, TEST_MAGIC);
	malformed[4] = TEST_VERSION;
	malformed[5] = MIDR_LOCAL_SNAPSHOT_BEGIN;
	malformed[6] = 1;
	put_u64(malformed + 8, 5);
	put_u32(malformed + 16, 77);
	raw_send_all(fd, malformed, sizeof(malformed));
	assert(midr_local_ipc_server_poll(server, 10) == -EBADMSG);
	assert(log.reasons[log.disconnects - 1U] == -EBADMSG);
	(void)close(fd);

	log.callback_error = -ENOSPC;
	assert(midr_local_ipc_client_connect(path, &client) == 0);
	assert(midr_local_ipc_client_send(client, &events[0]) == 0);
	assert(midr_local_ipc_server_poll(server, 10) == 0);
	{
		size_t disconnects = log.disconnects;

		assert(midr_local_ipc_server_poll(server, 10) == -ENOSPC);
		assert(log.reasons[log.disconnects - 1U] == -ENOSPC);
		assert(log.disconnects == disconnects + 1U);
		assert(midr_local_ipc_server_poll(server, 0) == 0);
		assert(log.disconnects == disconnects + 1U);
	}
	midr_local_ipc_client_destroy(&client);
	log.callback_error = 0;

	fd = raw_connect(path);
	assert(midr_local_ipc_server_poll(server, 10) == 0);
	raw_send_all(fd, malformed, 10);
	(void)close(fd);
	assert(midr_local_ipc_server_poll(server, 10) == -ECONNRESET);
	assert(log.reasons[log.disconnects - 1U] == -ECONNRESET);

	/* Replacing a connection discards its partial frame.  The replacement's
	 * first complete frame must not be combined with stale bytes. */
	fd = raw_connect(path);
	assert(midr_local_ipc_server_poll(server, 10) == 0);
	raw_send_all(fd, malformed, 10);
	assert(midr_local_ipc_server_poll(server, 10) == 0);
	{
		size_t disconnects = log.disconnects;

		assert(midr_local_ipc_client_connect(path, &client) == 0);
		assert(midr_local_ipc_server_poll(server, 10) == 0);
		assert(log.reasons[log.disconnects - 1U] == -ECONNABORTED);
		assert(log.disconnects == disconnects + 1U);
	}
	(void)close(fd);
	assert(midr_local_ipc_client_send(client, &events[0]) == 0);
	assert(midr_local_ipc_server_poll(server, 10) == 0);
	assert(log.count == 9);
	assert(log.events[8].kind == MIDR_LOCAL_SNAPSHOT_BEGIN);
	assert(log.events[8].generation == 4);
	midr_local_ipc_client_destroy(&client);
	assert(poll_until_result(server) == -ECONNRESET);
	midr_local_ipc_server_destroy(&server);
	puts("midrd-local-ipc-test: PASS");
	return 0;
}
