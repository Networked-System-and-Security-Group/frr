// SPDX-License-Identifier: GPL-2.0-or-later
/* Real FRR events and engine, deterministic transport, no IP traffic. */
#include <zebra.h>
#include "network.h"
#include "privs.h"
#include "bgpd/midr_trace_engine.h"
#include "bgpd/midr_trace_udp.h"

struct zebra_privs_t bgpd_privs = {};
struct event_loop *master;
static int peer_fd;
static uint8_t sent_ttl;
static uint16_t sent_port, previous_port;
static unsigned int sends, callbacks;
static bool skip_second, fail_send;
static struct midr_trace_job_result delivered;

bool __wrap_midr_trace_udp_supported(int family);
int __wrap_midr_trace_udp_open(int family, int *fd);
int __wrap_midr_trace_udp_send(int fd, const struct prefix *target, uint16_t port,
			     uint8_t ttl, const void *payload, size_t len);
int __wrap_midr_trace_udp_receive(int fd, const struct prefix *target, uint16_t port,
	const void *payload, size_t len, struct midr_trace_udp_reply *reply);

bool __wrap_midr_trace_udp_supported(int family)
{
	return family == AF_INET || family == AF_INET6;
}

int __wrap_midr_trace_udp_open(int family, int *fd)
{
	int pair[2];

	(void)family;
	assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) == 0);
	assert(set_nonblocking(pair[0]) == 0);
	*fd = pair[0];
	peer_fd = pair[1];
	return 0;
}

int __wrap_midr_trace_udp_send(int fd, const struct prefix *target, uint16_t port,
			     uint8_t ttl, const void *payload, size_t len)
{
	(void)fd;
	(void)target;
	(void)payload;
	assert(len == 32 && port && port != previous_port);
	previous_port = sent_port = port;
	sent_ttl = ttl;
	sends++;
	if (fail_send)
		return EACCES;
	if (!(skip_second && ttl == 2))
		assert(send(peer_fd, "x", 1, 0) == 1);
	return 0;
}

int __wrap_midr_trace_udp_receive(int fd, const struct prefix *target, uint16_t port,
	const void *payload, size_t len, struct midr_trace_udp_reply *reply)
{
	char byte;

	(void)payload;
	(void)len;
	if (recv(fd, &byte, 1, MSG_DONTWAIT) < 0)
		return -1;
	assert(port == sent_port);
	memset(reply, 0, sizeof(*reply));
	reply->visible = true;
	reply->offender = *target;
	reply->type = target->family == AF_INET ? 11 : 3;
	if (sent_ttl == 3) {
		reply->terminal = reply->reached = true;
		reply->type = target->family == AF_INET ? 3 : 1;
		reply->code = target->family == AF_INET ? 3 : 4;
	}
	return 1;
}

static void done(const struct midr_trace_job_result *result, void *arg)
{
	(void)arg;
	delivered = *result;
	callbacks++;
}

static void watchdog(struct event *event)
{
	(void)event;
	assert(!"engine test timed out");
}

static void run(const char *address, bool missing_hop, bool failure)
{
	struct prefix target;
	struct midr_trace_engine *engine = NULL;
	struct event *timeout = NULL;
	struct event event;

	skip_second = missing_hop;
	fail_send = failure;
	sends = callbacks = 0;
	assert(str2prefix(address, &target));
	assert(midr_trace_engine_start(master, &target, done, NULL, &engine) == 0);
	assert(callbacks == 0 && sends == 0);
	event_add_timer(master, watchdog, NULL, 5, &timeout);
	while (!callbacks) {
		assert(event_fetch(master, &event));
		event_call(&event);
	}
	event_cancel(&timeout);
	assert(callbacks == 1);
	if (failure) {
		assert(delivered.status == MIDR_TRACE_ERR_SEND);
		assert(delivered.system_errno == EACCES);
	} else {
		assert(delivered.status == MIDR_TRACE_OK && delivered.target_reached);
		assert(delivered.raw_path.hop_count == 3 && sends == 3);
		assert(delivered.raw_path.hops[1].visible == !missing_hop);
		assert(delivered.raw_path.hops[2].ttl == 3);
	}
	midr_trace_engine_destroy(&engine);
	assert(!engine);
	midr_trace_engine_destroy(&engine);
	close(peer_fd);
}

int main(void)
{
	struct prefix target;
	struct midr_trace_engine *engine = NULL;

	master = event_master_create("MIDR engine test");
	run("192.0.2.10", false, false);
	run("2001:db8::10", true, false);
	run("192.0.2.10", false, true);
	assert(str2prefix("fe80::1", &target));
	assert(!midr_trace_engine_target_valid(&target));
	assert(str2prefix("192.0.2.20", &target));
	assert(midr_trace_engine_start(master, &target, done, NULL, &engine) == 0);
	midr_trace_engine_destroy(&engine); /* cancel before initial send */
	close(peer_fd);
	event_master_free(master);
	puts("MIDR engine tests passed");
	return 0;
}
