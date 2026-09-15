#include "midr-prefix-ipc.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct event_log {
	struct midr_prefix_event events[8];
	size_t count;
	size_t disconnects;
};

static int record_event(void *arg, const struct midr_prefix_event *event)
{
	struct event_log *log = arg;

	assert(log->count < 8);
	log->events[log->count++] = *event;
	return 0;
}

static void disconnected(void *arg, int reason)
{
	struct event_log *log = arg;

	(void)reason;
	log->disconnects++;
}

static void pump(struct midr_prefix_ipc *server)
{
	for (unsigned int i = 0; i < 4; i++)
		assert(midr_prefix_ipc_server_poll(server, 10) >= 0);
}

int main(void)
{
	const char *path = "/tmp/midrd-prefix-ipc-test.sock";
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
	assert(log.count == 4 && log.events[1].prefix.family == MIDR_CORE_AF_IPV6);
	assert(log.events[3].kind == MIDR_PREFIX_EOR);
	midr_prefix_ipc_client_destroy(&client);
	pump(server);
	assert(log.disconnects == 1);
	assert(midr_prefix_ipc_client_connect(path, &client) == 0);
	assert(midr_prefix_ipc_client_send(client, &upsert) == 0);
	pump(server);
	assert(log.count == 5);
	midr_prefix_ipc_client_destroy(&client);
	midr_prefix_ipc_server_destroy(&server);
	puts("midrd-prefix-ipc-test: PASS");
	return 0;
}
