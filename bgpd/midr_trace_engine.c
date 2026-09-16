// SPDX-License-Identifier: GPL-2.0-or-later
/* One outstanding UDP probe per job; all ownership is on bgpd's event loop. */
#include <zebra.h>
#include "memory.h"
#include "monotime.h"
#include "bgpd/bgp_memory.h"
#include "bgpd/midr_trace_engine.h"
#include "bgpd/midr_trace_udp.h"

DEFINE_MTYPE_STATIC(BGPD, MIDR_TRACE_ENGINE, "MIDR UDP traceroute engine");

#define MIDR_TRACE_PORT_FIRST 33434U
#define MIDR_TRACE_PORT_COUNT (65536U - MIDR_TRACE_PORT_FIRST)
#define MIDR_TRACE_RX_BUDGET 32U

/* Bound delayed ICMP aliasing across fd/source-port reuse. Never reset these
 * process-lifetime reservations when the scheduler is reinitialized.
 */
static int64_t port_available[2][MIDR_TRACE_PORT_COUNT];
static unsigned int port_cursor[2];
static int64_t next_send_usec;
static uint64_t next_probe_id = 1;

struct midr_trace_engine {
	struct event_loop *master;
	int fd;
	uint8_t ttl;
	unsigned int probes;
	uint16_t port;
	bool waiting;
	bool finished;
	int64_t sent_usec;
	unsigned char payload[32];
	struct midr_trace_job_result result;
	midr_trace_engine_done_cb done;
	void *arg;
	struct event *read_event;
	struct event *timer;
	struct event *completion;
};

static int64_t midr_trace_now(void)
{
	struct timeval now;

	monotime(&now);
	return (int64_t)now.tv_sec * 1000000 + now.tv_usec;
}

bool midr_trace_engine_supported(int family)
{
	return midr_trace_udp_supported(family);
}

bool midr_trace_engine_target_valid(const struct prefix *target)
{
	if (!target)
		return false;
	if (target->family == AF_INET) {
		uint32_t addr = ntohl(target->u.prefix4.s_addr);

		return addr != 0 && addr != UINT32_MAX && !IN_MULTICAST(addr);
	}
	if (target->family == AF_INET6)
		return !IN6_IS_ADDR_UNSPECIFIED(&target->u.prefix6)
			&& !IN6_IS_ADDR_MULTICAST(&target->u.prefix6)
			&& !IN6_IS_ADDR_LINKLOCAL(&target->u.prefix6)
			&& !IN6_IS_ADDR_V4MAPPED(&target->u.prefix6);
	return false;
}

static void midr_trace_engine_close(struct midr_trace_engine *engine)
{
	event_cancel(&engine->read_event);
	event_cancel(&engine->timer);
	if (engine->fd >= 0) {
		close(engine->fd);
		engine->fd = -1;
	}
}

static void midr_trace_engine_deliver(struct event *event)
{
	struct midr_trace_engine *engine = EVENT_ARG(event);

	engine->done(&engine->result, engine->arg);
}

static void midr_trace_engine_finish(struct midr_trace_engine *engine,
				     enum midr_trace_status status,
				     enum midr_trace_stop_reason reason, int error)
{
	if (engine->finished)
		return;
	engine->finished = true;
	engine->waiting = false;
	engine->result.status = status;
	engine->result.stop_reason = reason;
	engine->result.system_errno = error;
	midr_trace_engine_close(engine);
	event_add_event(engine->master, midr_trace_engine_deliver, engine, 0,
			&engine->completion);
}

static void midr_trace_engine_send(struct event *event);
static void midr_trace_engine_read(struct event *event);

static void midr_trace_engine_next(struct midr_trace_engine *engine)
{
	engine->waiting = false;
	engine->port = 0;
	event_cancel(&engine->timer);
	if (engine->ttl == MIDR_TRACE_MAX_TTL) {
		midr_trace_engine_finish(engine, MIDR_TRACE_OK,
					 MIDR_TRACE_STOP_MAX_HOPS, 0);
		return;
	}
	engine->ttl++;
	engine->probes = 0;
	event_add_timer_msec(engine->master, midr_trace_engine_send, engine,
			     MIDR_TRACE_SEND_INTERVAL_MSEC, &engine->timer);
}

static void midr_trace_engine_hop_timeout(struct event *event)
{
	struct midr_trace_engine *engine = EVENT_ARG(event);
	struct midr_trace_raw_hop *hop;

	if (!engine->waiting || engine->finished)
		return;
	hop = &engine->result.raw_path.hops[engine->ttl - 1];
	hop->ttl = engine->ttl;
	engine->result.raw_path.hop_count = engine->ttl;
	if (engine->probes < MIDR_TRACE_PROBES_PER_HOP) {
		engine->waiting = false;
		/* A retry has a new port/token so late replies cannot match it. */
		engine->port = 0;
		event_add_timer_msec(engine->master, midr_trace_engine_send, engine,
				     MIDR_TRACE_SEND_INTERVAL_MSEC, &engine->timer);
		return;
	}
	midr_trace_engine_next(engine);
}

static uint16_t midr_trace_port(int family, int64_t now)
{
	unsigned int af = family == AF_INET6;
	unsigned int i;

	for (i = 0; i < MIDR_TRACE_PORT_COUNT; i++) {
		unsigned int slot = port_cursor[af]++ % MIDR_TRACE_PORT_COUNT;

		if (port_available[af][slot] > now)
			continue;
		/* Max job deadline is 120s; retain another 120s after that. */
		port_available[af][slot] = now + 240000000;
		return MIDR_TRACE_PORT_FIRST + slot;
	}
	return 0;
}

static void midr_trace_engine_send(struct event *event)
{
	struct midr_trace_engine *engine = EVENT_ARG(event);
	int64_t now = midr_trace_now();
	int rc;
	unsigned int i;
	uint64_t id;

	if (engine->finished)
		return;
	if (now < next_send_usec) {
		event_add_timer_msec(engine->master, midr_trace_engine_send, engine,
				     (next_send_usec - now + 999) / 1000, &engine->timer);
		return;
	}
	next_send_usec = now + MIDR_TRACE_SEND_INTERVAL_MSEC * 1000;
	if (!engine->port) {
		engine->port = midr_trace_port(engine->result.target.family, now);
		if (!engine->port || !next_probe_id) {
			midr_trace_engine_finish(engine, MIDR_TRACE_ERR_RESOURCE,
						 MIDR_TRACE_STOP_ERROR, ENOSPC);
			return;
		}
		id = next_probe_id++;
		memcpy(engine->payload, "MIDR-UDP", 8);
		for (i = 0; i < 8; i++)
			engine->payload[8 + i] = (id >> (56 - 8 * i)) & 0xff;
	}
	rc = midr_trace_udp_send(engine->fd, &engine->result.target, engine->port,
				 engine->ttl, engine->payload, sizeof(engine->payload));
	if (rc == EAGAIN || rc == EWOULDBLOCK || rc == EINTR) {
		event_add_timer_msec(engine->master, midr_trace_engine_send, engine,
				     MIDR_TRACE_SEND_INTERVAL_MSEC, &engine->timer);
		return;
	}
	if (rc) {
		midr_trace_engine_finish(engine, MIDR_TRACE_ERR_SEND,
					 MIDR_TRACE_STOP_ERROR, rc);
		return;
	}
	engine->probes++;
	engine->sent_usec = midr_trace_now();
	engine->waiting = true;
	event_add_timer_msec(engine->master, midr_trace_engine_hop_timeout, engine,
			     MIDR_TRACE_PROBE_WAIT_MSEC, &engine->timer);
}

static void midr_trace_engine_read(struct event *event)
{
	struct midr_trace_engine *engine = EVENT_ARG(event);
	unsigned int i;
	unsigned char discard[128];

	if (engine->finished)
		return;
	for (i = 0; i < MIDR_TRACE_RX_BUDGET; i++) {
		struct midr_trace_udp_reply reply;
		int rc = midr_trace_udp_receive(engine->fd, &engine->result.target,
			engine->port, engine->payload, sizeof(engine->payload), &reply);
		int64_t elapsed = midr_trace_now() - engine->sent_usec;
		struct midr_trace_raw_hop *hop;

		if (rc < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			if (errno == EINTR)
				continue;
			midr_trace_engine_finish(engine, MIDR_TRACE_ERR_RECEIVE,
						 MIDR_TRACE_STOP_ERROR, errno);
			return;
		}
		if (!rc || !engine->waiting || elapsed >= MIDR_TRACE_PROBE_WAIT_MSEC * 1000)
			continue;
		if (reply.local_errno) {
			midr_trace_engine_finish(engine, MIDR_TRACE_ERR_SEND,
						 MIDR_TRACE_STOP_ERROR, reply.local_errno);
			return;
		}
		/* An unidentifiable non-terminal reply still represents a '*' hop.
		 * Leave the current timeout armed so the bounded retry path handles it. */
		if (!reply.visible && !reply.terminal)
			continue;
		hop = &engine->result.raw_path.hops[engine->ttl - 1];
		hop->ttl = engine->ttl;
		hop->visible = reply.visible;
		hop->address = reply.offender;
		hop->has_rtt = true;
		hop->rtt_usec = (uint32_t)MAX(elapsed, 0);
		hop->has_icmp = true;
		hop->icmp_type = reply.type;
		hop->icmp_code = reply.code;
		engine->result.raw_path.hop_count = engine->ttl;
		if (reply.terminal) {
			engine->result.target_reached = reply.reached;
			midr_trace_engine_finish(engine, MIDR_TRACE_OK,
				reply.reached ? MIDR_TRACE_STOP_REACHED : MIDR_TRACE_STOP_UNREACHABLE, 0);
			return;
		}
		midr_trace_engine_next(engine);
		if (engine->finished)
			return;
	}
	/* Ordinary UDP responses are not proof of ICMP port-unreachable arrival. */
	for (i = 0; i < MIDR_TRACE_RX_BUDGET; i++) {
		if (recv(engine->fd, discard, sizeof(discard), MSG_DONTWAIT) < 0)
			break;
	}
	/* FRR removes the epoll registration on EPOLLERR: always re-arm. */
	event_add_read(engine->master, midr_trace_engine_read, engine, engine->fd,
			&engine->read_event);
}

int midr_trace_engine_start(struct event_loop *master, const struct prefix *target,
			   const struct midr_trace_net_context *context,
			   midr_trace_engine_done_cb done, void *arg,
			   struct midr_trace_engine **out)
{
	struct midr_trace_engine *engine;
	int fd, rc;

	if (!out)
		return EINVAL;
	*out = NULL;
	if (!master || !done || !midr_trace_engine_target_valid(target))
		return EINVAL;
	if (context && context->source.family &&
	    (!midr_trace_engine_target_valid(&context->source) ||
	     context->source.family != target->family))
		return EINVAL;
	rc = midr_trace_udp_open(target->family, context, &fd);
	if (rc)
		return rc;
	engine = XCALLOC(MTYPE_MIDR_TRACE_ENGINE, sizeof(*engine));
	engine->master = master;
	engine->fd = fd;
	engine->ttl = 1;
	engine->done = done;
	engine->arg = arg;
	engine->result.target = *target;
	engine->result.target.prefixlen = target->family == AF_INET ? 32 : 128;
	*out = engine;
	event_add_read(master, midr_trace_engine_read, engine, fd, &engine->read_event);
	event_add_event(master, midr_trace_engine_send, engine, 0, &engine->timer);
	return 0;
}

void midr_trace_engine_snapshot(const struct midr_trace_engine *engine,
			       struct midr_trace_job_result *out)
{
	if (engine && out)
		*out = engine->result;
}

void midr_trace_engine_destroy(struct midr_trace_engine **enginep)
{
	struct midr_trace_engine *engine;

	if (!enginep || !*enginep)
		return;
	engine = *enginep;
	midr_trace_engine_close(engine);
	event_cancel(&engine->completion);
	event_cancel_event(engine->master, engine);
	XFREE(MTYPE_MIDR_TRACE_ENGINE, engine);
	*enginep = NULL;
}
