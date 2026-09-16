#include "midr-prefix-ipc.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct event_log {
	struct midr_prefix_event events[16];
	size_t count;
	int callback_error;
	int reasons[16];
	size_t disconnects;
};

static int record_event(void *arg, const struct midr_prefix_event *event)
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

static void pump(struct midr_prefix_ipc *server)
{
	for (unsigned int i = 0; i < 8; i++)
		assert(midr_prefix_ipc_server_poll(server, 10) == 0);
}

static int poll_until_result(struct midr_prefix_ipc *server)
{
	for (unsigned int i = 0; i < 8; i++) {
		int ret = midr_prefix_ipc_server_poll(server, 10);

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

int main(void)
{
	char path[96];
	struct event_log log = {0};
	struct midr_prefix_ipc_config config = {
		.path = path,
		.on_event = record_event,
		.on_disconnect = disconnected,
		.arg = &log,
	};
	struct midr_prefix_ipc *server = NULL;
	struct midr_prefix_ipc *client = NULL;
	struct midr_prefix_event begin = {
		.kind = MIDR_PREFIX_SNAPSHOT_BEGIN,
		.generation = 4,
		.originator = 77,
	};
	struct midr_prefix_event upsert = {
		.kind = MIDR_PREFIX_UPSERT,
		.generation = 4,
		.originator = 77,
		.prefix = {
			.family = MIDR_CORE_AF_IPV6,
			.prefix_len = 64,
			.address = {0x20, 1, 0x0d, 0xb8},
			.metric = 12,
		},
	};
	struct midr_prefix_event end = {
		.kind = MIDR_PREFIX_SNAPSHOT_END,
		.generation = 4,
		.originator = 77,
	};
	struct midr_prefix_event eor = end;
	uint8_t malformed[MIDR_PREFIX_IPC_FRAME_LEN] = {0};
	int fd;

	assert(snprintf(path, sizeof(path), "/tmp/midrd-prefix-ipc-%ld.sock",
			(long)getpid()) > 0);
	eor.kind = MIDR_PREFIX_EOR;
	(void)unlink(path);
	assert(midr_prefix_ipc_server_create(&config, &server) == 0);
	assert(midr_prefix_ipc_server_start(server) == 0);
	assert(midr_prefix_ipc_client_connect(path, &client) == 0);
	assert(midr_prefix_ipc_client_send(client, &begin) == 0);
	assert(midr_prefix_ipc_client_send(client, &upsert) == 0);
	assert(midr_prefix_ipc_client_send(client, &end) == 0);
	assert(midr_prefix_ipc_client_send(client, &eor) == 0);
	pump(server);
	assert(log.count == 4 &&
	       log.events[1].prefix.family == MIDR_CORE_AF_IPV6);
	assert(log.events[3].kind == MIDR_PREFIX_EOR);
	midr_prefix_ipc_client_destroy(&client);
	pump(server);
	assert(log.disconnects == 1);

	fd = raw_connect(path);
	assert(midr_prefix_ipc_server_poll(server, 10) == 0);
	raw_send_all(fd, malformed, sizeof(malformed));
	assert(poll_until_result(server) == -EBADMSG);
	assert(log.reasons[log.disconnects - 1U] == -EBADMSG);
	(void)close(fd);

	log.callback_error = -ENOSPC;
	assert(midr_prefix_ipc_client_connect(path, &client) == 0);
	assert(midr_prefix_ipc_client_send(client, &begin) == 0);
	assert(midr_prefix_ipc_server_poll(server, 10) == 0);
	{
		size_t disconnects = log.disconnects;

		assert(poll_until_result(server) == -ENOSPC);
		assert(log.reasons[log.disconnects - 1U] == -ENOSPC);
		assert(log.disconnects == disconnects + 1U);
		assert(midr_prefix_ipc_server_poll(server, 0) == 0);
		assert(log.disconnects == disconnects + 1U);
	}
	midr_prefix_ipc_client_destroy(&client);
	log.callback_error = 0;

	fd = raw_connect(path);
	assert(midr_prefix_ipc_server_poll(server, 10) == 0);
	{
		size_t disconnects = log.disconnects;

		raw_send_all(fd, malformed, 10);
		(void)close(fd);
		pump(server);
		assert(log.disconnects == disconnects + 1U);
		assert(log.reasons[log.disconnects - 1U] == 0);
	}

	/* A replacement connection must not inherit an old partial frame. */
	fd = raw_connect(path);
	assert(midr_prefix_ipc_server_poll(server, 10) == 0);
	raw_send_all(fd, malformed, 10);
	assert(midr_prefix_ipc_server_poll(server, 10) == 0);
	{
		size_t disconnects = log.disconnects;

		assert(midr_prefix_ipc_client_connect(path, &client) == 0);
		assert(midr_prefix_ipc_server_poll(server, 10) == 0);
		assert(log.reasons[log.disconnects - 1U] == -ECONNABORTED);
		assert(log.disconnects == disconnects + 1U);
	}
	(void)close(fd);
	assert(midr_prefix_ipc_client_send(client, &upsert) == 0);
	assert(midr_prefix_ipc_server_poll(server, 10) == 0);
	assert(log.count == 5);
	assert(log.events[4].kind == MIDR_PREFIX_UPSERT);
	midr_prefix_ipc_client_destroy(&client);
	pump(server);
	midr_prefix_ipc_server_destroy(&server);
	puts("midrd-prefix-ipc-test: PASS");
	return 0;
}
