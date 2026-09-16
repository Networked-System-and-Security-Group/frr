/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L

#include <zebra.h>

#include "frrevent.h"
#include "getopt.h"
#include "libfrr.h"
#include "log.h"
#include "sigevent.h"
#include <lib/version.h>

#include "midr-cost.h"
#include "midr-context-private.h"
#include "midr-engine.h"
#include "midr-local-ipc.h"
#include "midr-local-provider.h"
#include "midr-prefix-provider.h"
#include "midr-prefix-ipc.h"
#include "midr-spf.h"
#include "midr-ted.h"
#include "midr-owned.h"
#include "midr-transport.h"
#include "midr-wire.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

static struct midr_context *midrd_runtime;

static FRR_NORETURN void midrd_terminate(int status);

static void midrd_sighup(void)
{
	zlog_info("SIGHUP received and ignored");
}

static void midrd_sigusr1(void)
{
	zlog_rotate();
}

static FRR_NORETURN void midrd_sigint(void)
{
	zlog_notice("Terminating on signal");
	midrd_terminate(0);
}

static struct frr_signal_t midrd_signals[] = {
	{
		.signal = SIGHUP,
		.handler = &midrd_sighup,
	},
	{
		.signal = SIGUSR1,
		.handler = &midrd_sigusr1,
	},
	{
		.signal = SIGINT,
		.handler = &midrd_sigint,
	},
	{
		.signal = SIGTERM,
		.handler = &midrd_sigint,
	},
};

static struct zebra_privs_t midrd_privs;

static const char midrd_help[] =
	"  --node-id N              Stable MIDR node identifier\n"
	"  --listen HOST:PORT       Native MIDR listener\n"
	"  --peer HOST:PORT         Native MIDR peer (repeatable)\n"
	"  --group ID               Development-time local group\n"
	"  --prefix ADDRESS/LEN     Development-time local Prefix\n"
	"  --link NODE:COST         Development-time local Link\n"
	"  --local-fact-socket PATH External Local Fact Provider\n"
	"  --prefix-socket PATH     External Prefix Provider\n"
	"  --sequence-file PATH     Persistent owner sequence file\n"
	"  --lifetime MS            Object lifetime\n"
	"  --hold-time MS           Native session Hold Timer\n"
	"  --runtime SEC            Stop after a bounded runtime\n"
	"  --takeover-delay MS      Representative takeover delay\n"
	"  --pidfile PATH           Compatibility alias for --pid_file\n";

/* clang-format off */
FRR_DAEMON_INFO(midrd, MIDR,
	.vty_port = 0,
	.proghelp = "Standalone MIDR link-state daemon.",
	.signals = midrd_signals,
	.n_signals = array_size(midrd_signals),
	.privs = &midrd_privs,
	.flags = FRR_NO_PRIVSEP | FRR_NO_TCPVTY | FRR_NO_SPLIT_CONFIG |
		 FRR_NO_ZCLIENT,
);
/* clang-format on */

enum midrd_option {
	/* libfrr reserves 1000-1009 for its own long-only options. */
	MIDRD_OPT_NODE_ID = 2000,
	MIDRD_OPT_LISTEN,
	MIDRD_OPT_PEER,
	MIDRD_OPT_GROUP,
	MIDRD_OPT_PREFIX,
	MIDRD_OPT_LINK,
	MIDRD_OPT_LOCAL_FACT_SOCKET,
	MIDRD_OPT_PREFIX_SOCKET,
	MIDRD_OPT_SEQUENCE_FILE,
	MIDRD_OPT_LIFETIME,
	MIDRD_OPT_HOLD_TIME,
	MIDRD_OPT_RUNTIME,
	MIDRD_OPT_TAKEOVER_DELAY,
	MIDRD_OPT_PIDFILE,
};

static const struct option midrd_longopts[] = {
	{"node-id", required_argument, NULL, MIDRD_OPT_NODE_ID},
	{"listen", required_argument, NULL, MIDRD_OPT_LISTEN},
	{"peer", required_argument, NULL, MIDRD_OPT_PEER},
	{"group", required_argument, NULL, MIDRD_OPT_GROUP},
	{"prefix", required_argument, NULL, MIDRD_OPT_PREFIX},
	{"link", required_argument, NULL, MIDRD_OPT_LINK},
	{"local-fact-socket", required_argument, NULL,
	 MIDRD_OPT_LOCAL_FACT_SOCKET},
	{"prefix-socket", required_argument, NULL, MIDRD_OPT_PREFIX_SOCKET},
	{"sequence-file", required_argument, NULL, MIDRD_OPT_SEQUENCE_FILE},
	{"lifetime", required_argument, NULL, MIDRD_OPT_LIFETIME},
	{"hold-time", required_argument, NULL, MIDRD_OPT_HOLD_TIME},
	{"runtime", required_argument, NULL, MIDRD_OPT_RUNTIME},
	{"takeover-delay", required_argument, NULL, MIDRD_OPT_TAKEOVER_DELAY},
	{"pidfile", required_argument, NULL, MIDRD_OPT_PIDFILE},
	{0},
};

static int reconcile_group_prefixes(struct midr_context *daemon, uint64_t now_ms);

static uint32_t peer_node_id(const struct midr_context *daemon,
			     const struct midr_transport_endpoint *peer)
{
	if (!daemon || !peer)
		return 0;
	for (size_t i = 0; i < daemon->peer_count; i++)
		if (midr_transport_endpoint_equal(&daemon->peers[i].endpoint, peer))
			return daemon->peers[i].node_id;
	return 0;
}

static struct midrd_snapshot_stage *stage_for(struct midr_context *daemon,
					      const struct midr_transport_endpoint *peer,
					      bool create)
{
	struct midrd_snapshot_stage *free_stage = NULL;

	for (size_t i = 0; i < MIDRD_MAX_PEERS; i++) {
		if (daemon->stages[i].active &&
		    midr_transport_endpoint_equal(&daemon->stages[i].peer, peer))
			return &daemon->stages[i];
		if (!daemon->stages[i].active && !free_stage)
			free_stage = &daemon->stages[i];
	}
	if (!create || !free_stage)
		return NULL;
	free_stage->peer = *peer;
	free_stage->capacity = MIDRD_MAX_SNAPSHOT;
	free_stage->objects = calloc(free_stage->capacity,
					      sizeof(*free_stage->objects));
	free_stage->received_ns = calloc(free_stage->capacity,
					  sizeof(*free_stage->received_ns));
	free_stage->updates = calloc(free_stage->capacity,
					 sizeof(*free_stage->updates));
	free_stage->update_received_ns = calloc(free_stage->capacity,
						 sizeof(*free_stage->update_received_ns));
	if (!free_stage->objects || !free_stage->received_ns ||
	    !free_stage->updates || !free_stage->update_received_ns) {
		free(free_stage->received_ns);
		free(free_stage->update_received_ns);
		free(free_stage->updates);
		free(free_stage->objects);
		memset(free_stage, 0, sizeof(*free_stage));
		return NULL;
	}
	free_stage->count = 0;
	free_stage->active = true;
	return free_stage;
}

static void stage_release(struct midrd_snapshot_stage *stage)
{
	if (!stage)
		return;
	free(stage->received_ns);
	free(stage->update_received_ns);
	free(stage->updates);
	free(stage->objects);
	memset(stage, 0, sizeof(*stage));
}

static uint64_t mono_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000000U + (uint64_t)ts.tv_nsec;
}

static uint64_t mono_ms(void)
{
	return mono_ns() / 1000000U;
}

static int age_object_lifetime(const struct midr_context *daemon,
			       struct midr_core_object *object,
			       uint64_t received_ns, uint64_t now_ns,
			       uint32_t budget_ms)
{
	uint32_t remaining_ms;
	int ret;

	if (!daemon || !object)
		return -EINVAL;
	ret = midr_core_lifetime_remaining(object->lifetime_ms, received_ns,
					   now_ns, budget_ms,
					   daemon->lifetime_ms,
					   &remaining_ms);
	if (ret)
		return ret;
	if (!remaining_ms)
		return -ESTALE;
	object->lifetime_ms = remaining_ms;
	return 0;
}

static int split_endpoint(const char *text, char *host, size_t host_len,
			  char *service, size_t service_len)
{
	const char *colon;
	size_t len;

	if (!text || !host || !service)
		return -EINVAL;
	if (text[0] == '[') {
		const char *end = strchr(text, ']');

		if (!end || end[1] != ':')
			return -EINVAL;
		len = (size_t)(end - text - 1);
		if (!len || len >= host_len)
			return -EINVAL;
		memcpy(host, text + 1, len);
		host[len] = '\0';
		if (snprintf(service, service_len, "%s", end + 2) < 1)
			return -EINVAL;
		return 0;
	}
	colon = strrchr(text, ':');
	if (!colon || strchr(text, ':') != colon)
		return -EINVAL;
	len = (size_t)(colon - text);
	if (!len || len >= host_len)
		return -EINVAL;
	memcpy(host, text, len);
	host[len] = '\0';
	if (snprintf(service, service_len, "%s", colon + 1) < 1)
		return -EINVAL;
	return 0;
}

static int parse_endpoint(const char *text,
			  struct midr_transport_endpoint *endpoint)
{
	char host[128], service[32];
	struct addrinfo hints = {0}, *result = NULL;
	int ret;

	if (!endpoint || split_endpoint(text, host, sizeof(host), service,
					 sizeof(service)))
		return -EINVAL;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	ret = getaddrinfo(!strcmp(host, "*") ? NULL : host, service,
				  &hints, &result);
	if (ret || !result)
		return -EINVAL;
	if (result->ai_family != AF_INET && result->ai_family != AF_INET6) {
		freeaddrinfo(result);
		return -EAFNOSUPPORT;
	}
	memset(endpoint, 0, sizeof(*endpoint));
	endpoint->family = result->ai_family == AF_INET ?
		MIDR_TRANSPORT_AF_IPV4 : MIDR_TRANSPORT_AF_IPV6;
	if (result->ai_family == AF_INET) {
		const struct sockaddr_in *sin =
			(const struct sockaddr_in *)result->ai_addr;

		endpoint->port = ntohs(sin->sin_port);
		memcpy(endpoint->address, &sin->sin_addr, 4);
	} else {
		const struct sockaddr_in6 *sin6 =
			(const struct sockaddr_in6 *)result->ai_addr;

		endpoint->port = ntohs(sin6->sin6_port);
		endpoint->scope_id = sin6->sin6_scope_id;
		memcpy(endpoint->address, &sin6->sin6_addr, 16);
	}
	freeaddrinfo(result);
	return 0;
}

static int parse_link(const char *text, struct midrd_link_config *link)
{
	char copy[128], *separator, *end;
	unsigned long remote, metric;

	if (!text || !link || strlen(text) >= sizeof(copy))
		return -EINVAL;
	strcpy(copy, text);
	separator = strchr(copy, ':');
	if (!separator || strchr(separator + 1, ':'))
		return -EINVAL;
	*separator++ = '\0';
	errno = 0;
	remote = strtoul(copy, &end, 10);
	if (errno || *end || !remote || remote > UINT32_MAX)
		return -EINVAL;
	errno = 0;
	metric = strtoul(separator, &end, 10);
	if (errno || *end || !metric || metric >= UINT32_MAX)
		return -EINVAL;
	link->remote = (uint32_t)remote;
	link->metric = (uint32_t)metric;
	link->candidate_metric = link->metric;
	/* A single static link per remote is sufficient for development input.
	 * The identity remains stable across refreshes and restarts. */
	link->link_id = link->remote;
	return 0;
}

static int send_frame_at(struct midr_context *daemon,
			 const struct midr_transport_endpoint *peer,
			 uint8_t type, const uint8_t *payload,
			 size_t payload_len, uint64_t encoded_ns)
{
	struct midr_transport_frame frame = {
		.version = MIDR_WIRE_VERSION,
		.type = type,
		.sequence = ++daemon->frame_sequence,
		.encoded_ns = encoded_ns,
		.payload = payload,
		.payload_len = payload_len,
	};

	return midr_transport_send(daemon->transport, peer, &frame);
}

static int send_frame(struct midr_context *daemon,
		      const struct midr_transport_endpoint *peer,
		      uint8_t type, const uint8_t *payload, size_t payload_len)
{
	return send_frame_at(daemon, peer, type, payload, payload_len, 0);
}

static int send_hello(struct midr_context *daemon,
			      const struct midr_transport_endpoint *peer)
{
	uint8_t payload[28] = {0};

	uint32_t node = htonl(daemon->node_id);
	uint32_t lifetime = htonl(daemon->lifetime_ms);
	uint16_t port = htons(peer->port);

	memcpy(payload, &node, sizeof(node));
	memcpy(payload + 4, &lifetime, sizeof(lifetime));
	payload[8] = peer->family;
	memcpy(payload + 10, &port, sizeof(port));
	memcpy(payload + 12, peer->address, 16);
	return send_frame(daemon, peer, MIDR_WIRE_HELLO, payload, sizeof(payload));
}

static int send_object(struct midr_context *daemon,
			       const struct midr_transport_endpoint *peer,
			       const struct midr_core_object *object,
			       uint64_t observed_ns)
{
	struct midr_core_object wire_object = *object;
	uint8_t payload[MIDR_WIRE_OBJECT_LEN];
	uint64_t encoded_ns;
	size_t length;
	uint8_t type = object->state == MIDR_CORE_WITHDRAWN ?
		MIDR_WIRE_WITHDRAW : MIDR_WIRE_UPDATE;
	int ret;

	encoded_ns = mono_ns();
	ret = age_object_lifetime(daemon, &wire_object, observed_ns, encoded_ns,
				  MIDRD_FORWARD_BUDGET_MS);
	if (ret)
		return ret;
	if (midr_wire_encode_object(&wire_object, payload, sizeof(payload),
				    &length))
		return -EINVAL;
	return send_frame_at(daemon, peer, type, payload, length, encoded_ns);
}

static int send_snapshot_object(struct midr_context *daemon,
				const struct midr_transport_endpoint *peer,
				const struct midr_core_object *object,
				uint64_t observed_ns)
{
	struct midr_core_object wire_object = *object;
	uint8_t payload[MIDR_WIRE_OBJECT_LEN];
	uint64_t encoded_ns;
	size_t length;
	int ret;

	encoded_ns = mono_ns();
	ret = age_object_lifetime(daemon, &wire_object, observed_ns, encoded_ns,
				  MIDRD_FORWARD_BUDGET_MS);
	if (ret)
		return ret;
	if (midr_wire_encode_object(&wire_object, payload, sizeof(payload),
				    &length))
		return -EINVAL;
	return send_frame_at(daemon, peer, MIDR_WIRE_SNAPSHOT_OBJECT, payload,
			     length, encoded_ns);
}

static void flood_object(struct midr_context *daemon,
			 const struct midr_core_object *object,
			 uint64_t observed_ns,
			 const struct midr_transport_endpoint *except)
{
	for (size_t i = 0; i < daemon->peer_count; i++) {
		if (except && midr_transport_endpoint_equal(
				&daemon->peers[i].endpoint, except))
			continue;
		if (!midr_engine_export(daemon->engine, object,
					daemon->peers[i].node_id))
			continue;
		(void)send_object(daemon, &daemon->peers[i].endpoint, object,
				  observed_ns);
	}
}

static void reflood_scope(struct midr_context *daemon,
			  const struct midr_transport_endpoint *except)
{
	struct midr_core_object *objects;
	uint64_t snapshot_ns;
	size_t count = 0;

	objects = calloc(MIDRD_MAX_SNAPSHOT, sizeof(*objects));
	if (!objects)
		return;
	snapshot_ns = mono_ns();
	if (!midr_engine_snapshot(daemon->engine, snapshot_ns / 1000000U, objects,
				  MIDRD_MAX_SNAPSHOT, &count)) {
		for (size_t i = 0; i < count; i++)
			if (objects[i].state == MIDR_CORE_ACTIVE)
				flood_object(daemon, &objects[i], snapshot_ns, except);
	}
	free(objects);
}

static void note_engine_publication(struct midr_context *daemon)
{
	int error;
	enum midr_ted_state previous_state;
	int previous_error;

	if (!daemon || !daemon->engine || !daemon->ted ||
	    !midr_engine_publication_pending(daemon->engine))
		return;
	error = midr_engine_publication_error(daemon->engine);
	if (!error)
		error = -EIO;
	previous_state = midr_ted_state(daemon->ted);
	previous_error = midr_ted_last_error(daemon->ted);
	(void)midr_ted_invalidate(daemon->ted, error);
	daemon->ted_rebuild_pending = true;
	if (previous_state != MIDR_TED_NOT_READY || previous_error != error)
		fprintf(stderr,
			"node=%" PRIu32 " ted-state=NOT_READY error=%d\n",
			daemon->node_id, error);
}

static int apply_update(struct midr_context *daemon,
			const struct midr_core_object *object,
			uint64_t now_ms, enum midr_core_result *result,
			bool *scope_changed)
{
	int ret;

	if (!daemon || !object || !result || !scope_changed)
		return -EINVAL;
	*scope_changed = false;
	ret = midr_engine_apply(daemon->engine, object, now_ms, result);
	if (!ret)
		note_engine_publication(daemon);
	if (!ret && *result == MIDR_CORE_ACCEPTED &&
	    object->identity.type == MIDR_CORE_MEMBERSHIP)
		*scope_changed = true;
	return ret;
}

static int rebuild_ted(struct midr_context *daemon)
{
	struct midr_consumer_snapshot snapshot = {0};
	enum midr_ted_state previous_state;
	int previous_error;
	int ret;

	if (!daemon || !daemon->consumer || !daemon->ted)
		return -EINVAL;
	previous_state = midr_ted_state(daemon->ted);
	previous_error = midr_ted_last_error(daemon->ted);
	ret = midr_engine_retry_publication(daemon->engine, mono_ms());
	if (!ret)
		ret = midr_consumer_snapshot_acquire(daemon->consumer, &snapshot);
	if (ret)
		(void)midr_ted_invalidate(daemon->ted, ret);
	else if (midr_ted_state(daemon->ted) == MIDR_TED_READY &&
		 midr_ted_source_generation(daemon->ted) == snapshot.generation) {
		midr_consumer_snapshot_release(&snapshot);
	} else {
		ret = midr_ted_apply_snapshot(daemon->ted, &snapshot);
		midr_consumer_snapshot_release(&snapshot);
		if (ret)
			(void)midr_ted_invalidate(daemon->ted, ret);
	}
	daemon->ted_rebuild_pending = ret != 0;
	if (ret) {
		if (previous_state != MIDR_TED_NOT_READY ||
		    previous_error != ret)
			fprintf(stderr,
				"node=%" PRIu32 " ted-state=NOT_READY error=%d\n",
				daemon->node_id, ret);
		return ret;
	}
	if (previous_state != MIDR_TED_READY)
		printf("node=%" PRIu32 " ted-state=READY generation=%" PRIu64
		       "\n", daemon->node_id, midr_ted_generation(daemon->ted));
	return 0;
}

static void drain_consumer(struct midr_context *daemon)
{
	struct midr_consumer_event event;
	struct midr_ted_view view = {0};
	struct midr_spf_route routes[MIDRD_MAX_SNAPSHOT];
	size_t route_count = 0;

	while (midr_consumer_event_next(daemon->consumer, &event) == 0)
		printf("node=%" PRIu32 " ted-event kind=%u generation=%" PRIu64 "\n",
		       daemon->node_id, event.kind, event.generation);
	if (midr_ted_view_acquire(daemon->ted, &view) == 0) {
		if (view.local_group_id) {
			if (view.generation == daemon->last_spf_generation) {
				midr_ted_view_release(&view);
				return;
			}
			if (midr_spf_compute_ted(&view, routes, MIDRD_MAX_SNAPSHOT,
						 &route_count) == 0) {
				daemon->last_spf_generation = view.generation;
				printf("node=%" PRIu32 " spf generation=%" PRIu64
				       " routes=%zu\n", daemon->node_id, view.generation,
				       route_count);
				for (size_t i = 0; i < route_count; i++)
					printf("node=%" PRIu32 " route originator=%" PRIu32
					       " metric=%" PRIu64
					       " reachable=%u nexthops=%zu\n",
					       daemon->node_id, routes[i].originator,
					       routes[i].metric,
					       routes[i].reachable ? 1U : 0U,
					       routes[i].nexthop_count);
				midr_spf_routes_clear(routes, route_count);
			}
			midr_ted_view_release(&view);
			return;
		}
		midr_ted_view_release(&view);
	}
	/* A local prefix-only deployment has no group-level TED yet. */
	{
		struct midr_consumer_snapshot snapshot = {0};

		if (midr_ted_snapshot_acquire(daemon->ted, &snapshot) == 0) {
			if (snapshot.generation == daemon->last_spf_generation) {
				midr_consumer_snapshot_release(&snapshot);
				return;
			}
			if (midr_spf_compute(&snapshot, daemon->node_id, routes,
					     MIDRD_MAX_SNAPSHOT,
					     &route_count) == 0) {
				daemon->last_spf_generation = snapshot.generation;
				printf("node=%" PRIu32 " spf generation=%" PRIu64
				       " routes=%zu\n", daemon->node_id,
				       snapshot.generation, route_count);
				for (size_t i = 0; i < route_count; i++)
					printf("node=%" PRIu32 " route originator=%" PRIu32
					       " metric=%" PRIu64
					       " reachable=%u nexthops=%zu\n",
					       daemon->node_id, routes[i].originator,
					       routes[i].metric,
					       routes[i].reachable ? 1U : 0U,
					       routes[i].nexthop_count);
				midr_spf_routes_clear(routes, route_count);
			}
			midr_consumer_snapshot_release(&snapshot);
		}
	}
}

static void drain_events(struct midr_context *daemon,
			 const struct midr_transport_endpoint *except)
{
	struct midr_core_object event_object;

	while (midr_engine_event_next(daemon->engine, &event_object) == 0) {
		struct midr_core_object current;
		uint64_t observed_ns = mono_ns();

		if (midr_engine_lookup(daemon->engine, &event_object.identity,
				       observed_ns / 1000000U, &current, NULL) ||
		    current.sequence != event_object.sequence ||
		    current.state != event_object.state)
			continue;
		flood_object(daemon, &current, observed_ns, except);
		printf("node=%" PRIu32 " event state=%u type=%u originator=%" PRIu32
		       " group=%" PRIu32 " seq=%" PRIu64 "\n",
		       daemon->node_id, current.state, current.identity.type,
		       current.identity.originator, current.identity.group,
		       current.sequence);
	}
	drain_consumer(daemon);
}

static void send_snapshot(struct midr_context *daemon,
			  const struct midr_transport_endpoint *peer)
{
	struct midr_core_object *objects;
	uint64_t snapshot_ns;
	size_t count = 0;

	(void)send_frame(daemon, peer, MIDR_WIRE_SNAPSHOT_BEGIN, NULL, 0);
	objects = calloc(MIDRD_MAX_SNAPSHOT, sizeof(*objects));
	snapshot_ns = mono_ns();
	if (objects && midr_engine_snapshot(daemon->engine,
					    snapshot_ns / 1000000U, objects,
					    MIDRD_MAX_SNAPSHOT, &count) == 0)
		for (size_t i = 0; i < count; i++)
			if (objects[i].state == MIDR_CORE_ACTIVE &&
			    midr_engine_export(daemon->engine, &objects[i],
					       peer_node_id(daemon, peer)))
				(void)send_snapshot_object(daemon, peer, &objects[i],
						   snapshot_ns);
	free(objects);
	(void)send_frame(daemon, peer, MIDR_WIRE_SNAPSHOT_END, NULL, 0);
	(void)send_frame(daemon, peer, MIDR_WIRE_EOR, NULL, 0);
}

static void on_consumer_event(void *arg,
			      const struct midr_consumer_event *event)
{
	struct midr_context *daemon = arg;

	if (!daemon || !event || event->kind != MIDR_CONSUMER_SNAPSHOT_END)
		return;
	if (daemon->ted && midr_ted_state(daemon->ted) == MIDR_TED_READY &&
	    midr_ted_source_generation(daemon->ted) == event->generation)
		return;
	if (daemon->sync_derivation_wait) {
		daemon->ted_rebuild_pending = true;
		return;
	}
	(void)rebuild_ted(daemon);
}

static void on_established(void *arg,
			   const struct midr_transport_endpoint *peer)
{
	struct midr_context *daemon = arg;

	printf("node=%" PRIu32 " established family=%u port=%u\n",
	       daemon->node_id, peer->family, peer->port);
	(void)send_hello(daemon, peer);
	send_snapshot(daemon, peer);
}

static int stage_queue_update(struct midrd_snapshot_stage *stage,
			      const struct midr_core_object *object,
			      uint64_t received_ns)
{
	if (!stage || !stage->active || stage->update_count == stage->capacity)
		return -ENOSPC;
	stage->updates[stage->update_count] = *object;
	stage->update_received_ns[stage->update_count] = received_ns;
	stage->update_count++;
	return 0;
}

static int stage_generation_check(const struct midrd_snapshot_stage *stage,
				  const struct midr_transport_frame *frame)
{
	return stage && frame && stage->generation == frame->generation ? 0 :
		-EPROTO;
}

static void on_closed(void *arg, const struct midr_transport_endpoint *peer,
			      int reason)
{
	struct midr_context *daemon = arg;
	struct midrd_snapshot_stage *stage = stage_for(daemon, peer, false);

	stage_release(stage);
	printf("node=%" PRIu32 " closed family=%u port=%u reason=%d\n",
	       daemon->node_id, peer->family, peer->port, reason);
}

static void on_frame_written(void *arg,
			     const struct midr_transport_endpoint *peer,
			     uint64_t generation, uint64_t sequence)
{
	struct midr_context *daemon = arg;

	if (!daemon || !peer)
		return;
	printf("node=%" PRIu32 " tx-complete family=%u port=%u gen=%" PRIu64
	       " seq=%" PRIu64 "\n", daemon->node_id, peer->family,
	       peer->port, generation, sequence);
}

static void on_frame_dropped(void *arg,
			     const struct midr_transport_endpoint *peer,
			     uint64_t generation, uint64_t sequence, int reason)
{
	struct midr_context *daemon = arg;

	if (!daemon || !peer)
		return;
	if (daemon->shutdown_active)
		daemon->shutdown_write_failures++;
	fprintf(stderr,
		"node=%" PRIu32 " tx-dropped family=%u port=%u gen=%" PRIu64
		" seq=%" PRIu64 " reason=%d\n", daemon->node_id, peer->family,
		peer->port, generation, sequence, reason);
}

static int on_frame(void *arg, const struct midr_transport_endpoint *peer,
			const struct midr_transport_frame *frame)
{
	struct midr_context *daemon = arg;
	struct midr_core_object object;
	enum midr_core_result result;
	int ret;

	printf("node=%" PRIu32 " rx family=%u port=%u type=%u\n",
	       daemon->node_id, peer->family, peer->port, frame->type);
	switch (frame->type) {
	case MIDR_WIRE_HELLO:
		{
			uint32_t node_id;

		if (frame->payload_len != 28U)
			return -EINVAL;
		memcpy(&node_id, frame->payload, sizeof(node_id));
		node_id = ntohl(node_id);
		for (size_t i = 0; i < daemon->peer_count; i++)
			if (midr_transport_endpoint_equal(
					&daemon->peers[i].endpoint, peer)) {
				daemon->peers[i].node_id = node_id;
				break;
			}
		/* The initial snapshot may have been sent before the peer's HELLO
		 * arrived.  Re-send now that export eligibility is known. */
		send_snapshot(daemon, peer);
		return 0;
		}
	case MIDR_WIRE_KEEPALIVE:
		return frame->payload_len ? -EINVAL : 0;
	case MIDR_WIRE_SNAPSHOT_OBJECT:
		{
			struct midrd_snapshot_stage *stage = stage_for(daemon, peer,
								false);
			uint64_t now_ns;

			if (!stage || !stage->active)
				return -EPROTO;
			if (stage_generation_check(stage, frame))
				return -EPROTO;
			if (stage->ended)
				return -EPROTO;
			if (stage->count == stage->capacity)
				return -ENOSPC;
			if (midr_wire_decode_object(frame->payload, frame->payload_len,
						    &stage->objects[stage->count]))
				return -EINVAL;
			now_ns = mono_ns();
			ret = age_object_lifetime(daemon,
						 &stage->objects[stage->count],
						 frame->received_ns, now_ns, 0);
			if (ret)
				return ret;
			stage->received_ns[stage->count] = now_ns;
			stage->count++;
			return 0;
		}
	case MIDR_WIRE_UPDATE:
	case MIDR_WIRE_WITHDRAW:
		{
			struct midrd_snapshot_stage *stage = stage_for(daemon, peer,
								false);
			bool scope_changed;
			uint64_t now_ns;

			if (midr_wire_decode_object(frame->payload, frame->payload_len,
					    &object))
				return -EINVAL;
			if (frame->type == MIDR_WIRE_WITHDRAW)
				object.state = MIDR_CORE_WITHDRAWN;
			now_ns = mono_ns();
			ret = age_object_lifetime(daemon, &object, frame->received_ns,
						 now_ns, 0);
			if (ret)
				return ret;
			/* Incremental objects received before EoR belong to the same
			 * snapshot transaction.  Queue them so a reconnect cannot expose
			 * a half-applied full view or publish an intermediate TED. */
			if (stage && stage->active) {
				if (stage_generation_check(stage, frame))
					return -EPROTO;
				return stage_queue_update(stage, &object, now_ns);
			}
			ret = apply_update(daemon, &object, now_ns / 1000000U, &result,
					   &scope_changed);
			if (ret)
				return ret;
			if (result == MIDR_CORE_ACCEPTED) {
				drain_events(daemon, peer);
				if (scope_changed)
					reflood_scope(daemon, peer);
				(void)reconcile_group_prefixes(daemon, mono_ms());
			}
			return 0;
		}
	case MIDR_WIRE_SNAPSHOT_BEGIN:
		{
			struct midrd_snapshot_stage *stage = stage_for(daemon, peer, true);

			if (frame->payload_len)
				return -EINVAL;
			if (!stage)
				return -ENOSPC;
			/* A delayed begin from an older connection must not reset a
			 * snapshot already being assembled for a newer generation. */
			if (stage->generation && frame->generation < stage->generation)
				return -EPROTO;
			/* A repeated begin starts a fresh snapshot generation. */
			stage->count = 0;
			stage->update_count = 0;
			stage->ended = false;
			stage->generation = frame->generation;
		}
		printf("node=%" PRIu32 " sync type=%u\n", daemon->node_id,
		       frame->type);
		return 0;
	case MIDR_WIRE_SNAPSHOT_END:
		{
			struct midrd_snapshot_stage *stage = stage_for(daemon, peer,
								false);

			printf("node=%" PRIu32 " sync type=%u\n", daemon->node_id,
			       frame->type);
			if (frame->payload_len)
				return -EINVAL;
			if (!stage)
				return -EPROTO;
			if (stage_generation_check(stage, frame))
				return -EPROTO;
			if (stage->ended)
				return -EPROTO;
			stage->ended = true;
			return 0;
		}
	case MIDR_WIRE_EOR:
		{
			struct midrd_snapshot_stage *stage = stage_for(daemon, peer,
								false);
			bool scope_changed = false;
			uint64_t now_ns;

			printf("node=%" PRIu32 " sync type=%u\n", daemon->node_id,
			       frame->type);
			if (frame->payload_len)
				return -EINVAL;
			if (!stage)
				return 0;
			if (stage_generation_check(stage, frame))
				return -EPROTO;
			if (!stage->ended)
				return -EPROTO;
			now_ns = mono_ns();
			/* Keep the previous derived view available during the transaction;
			 * the callback from engine_end_batch() is serviced below, after EoR
			 * has established a complete canonical view. */
			daemon->sync_derivation_wait = true;
			ret = midr_engine_begin_batch(daemon->engine);
			if (!ret)
				for (size_t i = 0; i < stage->count; i++) {
					ret = age_object_lifetime(
						daemon, &stage->objects[i],
						stage->received_ns[i], now_ns, 0);
					if (ret)
						break;
					ret = midr_engine_apply(daemon->engine,
								&stage->objects[i],
								now_ns / 1000000U,
								&result);
						if (ret)
							break;
					if (result == MIDR_CORE_ACCEPTED &&
						    stage->objects[i].identity.type ==
							    MIDR_CORE_MEMBERSHIP) {
						scope_changed = true;
					}
				}
			if (!ret)
				for (size_t i = 0; i < stage->update_count; i++) {
					ret = age_object_lifetime(
						daemon, &stage->updates[i],
						stage->update_received_ns[i], now_ns, 0);
					if (ret)
						break;
					ret = midr_engine_apply(daemon->engine,
								&stage->updates[i],
								now_ns / 1000000U,
								&result);
					if (ret)
						break;
					if (result == MIDR_CORE_ACCEPTED &&
						    stage->updates[i].identity.type ==
							    MIDR_CORE_MEMBERSHIP)
						scope_changed = true;
				}
			if (!ret)
				ret = midr_engine_end_batch(daemon->engine,
						    now_ns / 1000000U);
			daemon->sync_derivation_wait = false;
			if (!ret)
				note_engine_publication(daemon);
			if (ret)
				(void)midr_engine_abort_batch(daemon->engine);
			else if (daemon->ted_rebuild_pending &&
				 !midr_engine_publication_pending(daemon->engine))
				(void)rebuild_ted(daemon);
			stage_release(stage);
			if (!ret) {
				drain_events(daemon, peer);
				if (scope_changed)
					reflood_scope(daemon, peer);
				(void)reconcile_group_prefixes(daemon, mono_ms());
			}
			return ret;
		}
	default:
		return -EINVAL;
	}
}

static int publish_owned(void *arg, const struct midr_core_object *object)
{
	struct midr_context *daemon = arg;
	enum midr_core_result result;
	uint64_t now_ms;
	int ret;

	now_ms = daemon->owned_commit_active ? daemon->owned_commit_now_ms :
		mono_ms();
	ret = midr_engine_apply(daemon->engine, object, now_ms, &result);
	if (ret)
		return ret;
	note_engine_publication(daemon);
	return result == MIDR_CORE_ACCEPTED ? 0 : -EAGAIN;
}

static void set_local_identity(struct midr_context *daemon,
			       const struct midr_core_identity *identity)
{
	daemon->local_identity = *identity;
	daemon->have_local_identity = true;
}

static int local_provider_event(void *arg,
				const struct midr_prefix_event *event)
{
	struct midr_context *daemon = arg;
	struct midr_core_object object = {0};
	int ret;

	if (!event)
		return -EINVAL;
	if (event->kind == MIDR_PREFIX_SNAPSHOT_BEGIN ||
	    event->kind == MIDR_PREFIX_SNAPSHOT_END ||
	    event->kind == MIDR_PREFIX_EOR)
		return 0;
	object.identity.type = MIDR_CORE_NODE_PREFIX;
	object.identity.family = event->prefix.family;
	object.identity.prefix_len = event->prefix.prefix_len;
	object.identity.originator = event->originator;
	memcpy(object.identity.prefix, event->prefix.address,
	       sizeof(object.identity.prefix));
	object.state = event->kind == MIDR_PREFIX_WITHDRAW
			       ? MIDR_CORE_WITHDRAWN : MIDR_CORE_ACTIVE;
	object.metric = event->prefix.metric;
	if (object.state == MIDR_CORE_WITHDRAWN)
		ret = midr_owned_withdraw(daemon->owned, &object.identity);
	else
		ret = midr_owned_upsert(daemon->owned, &object);
	if (!ret) {
		if (object.state == MIDR_CORE_ACTIVE)
			set_local_identity(daemon, &object.identity);
		drain_events(daemon, NULL);
		(void)reconcile_group_prefixes(daemon, mono_ms());
	}
	return ret;
}

static bool prefix_key_equal(const struct midr_prefix *left,
			     const struct midr_prefix *right)
{
	return left->family == right->family &&
	       left->prefix_len == right->prefix_len &&
	       !memcmp(left->address, right->address, sizeof(left->address));
}

static int prefix_find(const struct midr_prefix *prefixes, size_t count,
		       const struct midr_prefix *prefix)
{
	for (size_t i = 0; i < count; i++)
		if (prefix_key_equal(&prefixes[i], prefix))
			return (int)i;
	return -1;
}

static void prefix_identity(struct midr_context *daemon,
			    const struct midr_prefix *prefix,
			    struct midr_core_identity *identity)
{
	memset(identity, 0, sizeof(*identity));
	identity->type = MIDR_CORE_NODE_PREFIX;
	identity->family = prefix->family;
	identity->prefix_len = prefix->prefix_len;
	identity->originator = daemon->node_id;
	memcpy(identity->prefix, prefix->address, sizeof(identity->prefix));
}

static int preserve_staged_sequence(struct midr_context *daemon,
				    const struct midr_owned *staged,
				    int operation_error)
{
	int ret = midr_owned_sequence_floor(
		daemon->owned, midr_owned_last_sequence(staged));

	return operation_error ? operation_error : ret;
}

static int commit_prefixes(struct midr_context *daemon,
			   const struct midr_prefix *prefixes, size_t count)
{
	struct midr_owned *staged = NULL, *old_owned = NULL;
	struct midr_core_identity *saved_group_prefixes = NULL;
	size_t saved_group_prefix_count;
	uint32_t saved_representative_group;
	uint32_t saved_representative_node;
	uint64_t saved_takeover_ready_at;
	bool saved_representative_committed;
	bool saved_group_reconcile_pending;
	bool owned_staged = false;
	uint64_t now = mono_ms();
	int ret;

	if (!daemon || (count && !prefixes) || count > MIDRD_MAX_SNAPSHOT)
		return -EINVAL;
	saved_group_prefix_count = daemon->group_prefix_count;
	saved_representative_group = daemon->representative_group;
	saved_representative_node = daemon->representative_node;
	saved_takeover_ready_at = daemon->takeover_ready_at;
	saved_representative_committed = daemon->representative_committed;
	saved_group_reconcile_pending = daemon->group_reconcile_pending;
	ret = midr_owned_clone(daemon->owned, publish_owned, daemon, &staged);
	if (ret)
		return ret;
	if (saved_group_prefix_count) {
		saved_group_prefixes = malloc(saved_group_prefix_count *
					     sizeof(*saved_group_prefixes));
		if (!saved_group_prefixes) {
			ret = -ENOMEM;
			goto failed;
		}
		memcpy(saved_group_prefixes, daemon->group_prefixes,
		       saved_group_prefix_count * sizeof(*saved_group_prefixes));
	}
	ret = midr_engine_begin_batch(daemon->engine);
	if (ret)
		goto failed;
	daemon->owned_commit_active = true;
	daemon->owned_commit_now_ms = now;
	for (size_t i = 0; i < daemon->ipc_prefix_count; i++) {
		struct midr_core_identity identity;

		if (prefix_find(prefixes, count, &daemon->ipc_prefixes[i]) >= 0)
			continue;
		prefix_identity(daemon, &daemon->ipc_prefixes[i], &identity);
		ret = midr_owned_withdraw(staged, &identity);
		if (ret == -ENOENT)
			ret = 0;
		if (ret)
			goto abort;
	}
	for (size_t i = 0; i < count; i++) {
		struct midr_core_object current;
		struct midr_core_object fact = {0};

		prefix_identity(daemon, &prefixes[i], &fact.identity);
		fact.state = MIDR_CORE_ACTIVE;
		fact.metric = prefixes[i].metric;
		if (!midr_owned_lookup(staged, &fact.identity, &current) &&
		    current.state == MIDR_CORE_ACTIVE &&
		    current.metric == fact.metric)
			continue;
		ret = midr_owned_upsert(staged, &fact);
		if (ret)
			goto abort;
	}
	old_owned = daemon->owned;
	daemon->owned = staged;
	owned_staged = true;
	ret = reconcile_group_prefixes(daemon, now);
	if (ret)
		goto abort;
	ret = midr_engine_end_batch(daemon->engine, now);
	if (ret)
		goto failed;
	note_engine_publication(daemon);
	daemon->owned_commit_active = false;
	owned_staged = false;
	staged = NULL;
	midr_owned_destroy(&old_owned);
	memcpy(daemon->ipc_prefixes, prefixes, count * sizeof(*prefixes));
	daemon->ipc_prefix_count = count;
	free(saved_group_prefixes);
	drain_events(daemon, NULL);
	(void)reconcile_group_prefixes(daemon, now);
	return 0;

abort:
	daemon->owned_commit_active = false;
	(void)midr_engine_abort_batch(daemon->engine);
failed:
	daemon->owned_commit_active = false;
	if (owned_staged) {
		staged = daemon->owned;
		daemon->owned = old_owned;
		if (saved_group_prefix_count)
			memcpy(daemon->group_prefixes, saved_group_prefixes,
			       saved_group_prefix_count *
				       sizeof(*saved_group_prefixes));
		daemon->group_prefix_count = saved_group_prefix_count;
		daemon->representative_group = saved_representative_group;
		daemon->representative_node = saved_representative_node;
		daemon->takeover_ready_at = saved_takeover_ready_at;
		daemon->representative_committed =
			saved_representative_committed;
		daemon->group_reconcile_pending =
			saved_group_reconcile_pending;
	}
	free(saved_group_prefixes);
	ret = preserve_staged_sequence(daemon, staged, ret);
	midr_owned_destroy(&staged);
	return ret;
}

static int prefix_stage_apply(struct midrd_prefix_stage *stage,
			      const struct midr_prefix_event *event)
{
	int index = prefix_find(stage->prefixes, stage->count, &event->prefix);

	if (event->kind == MIDR_PREFIX_UPSERT) {
		if (index >= 0) {
			stage->prefixes[index] = event->prefix;
			return 0;
		}
		if (stage->count == MIDRD_MAX_SNAPSHOT)
			return -ENOSPC;
		stage->prefixes[stage->count++] = event->prefix;
		return 0;
	}
	if (event->kind != MIDR_PREFIX_WITHDRAW)
		return -EINVAL;
	if (index >= 0)
		stage->prefixes[index] = stage->prefixes[--stage->count];
	return 0;
}

static void prefix_stage_reset(struct midrd_prefix_stage *stage)
{
	memset(stage, 0, sizeof(*stage));
}

static int prefix_event(void *arg, const struct midr_prefix_event *event)
{
	struct midr_context *daemon = arg;
	struct midrd_prefix_stage *stage;
	int ret;

	if (!daemon || midr_prefix_event_validate(event) ||
	    event->originator != daemon->node_id)
		return -EINVAL;
	stage = &daemon->prefix_stage;
	if (event->kind == MIDR_PREFIX_SNAPSHOT_BEGIN) {
		if (stage->active && event->generation < stage->generation)
			return 0;
		prefix_stage_reset(stage);
		stage->active = true;
		stage->generation = event->generation;
		stage->originator = event->originator;
		stage->discard = event->generation <= daemon->prefix_generation;
		return 0;
	}
	if (stage->active) {
		if (event->generation < stage->generation)
			return 0;
		if (event->generation != stage->generation ||
		    event->originator != stage->originator)
			return -EINVAL;
		if (event->kind == MIDR_PREFIX_SNAPSHOT_END) {
			stage->ended = true;
			return 0;
		}
		if (event->kind == MIDR_PREFIX_EOR) {
			if (!stage->ended)
				return -EINVAL;
			ret = stage->discard ? 0 :
				commit_prefixes(daemon, stage->prefixes, stage->count);
			if (!ret && !stage->discard)
				daemon->prefix_generation = stage->generation;
			prefix_stage_reset(stage);
			return ret;
		}
		if (stage->ended)
			return -EINVAL;
		return stage->discard ? 0 : prefix_stage_apply(stage, event);
	}
	if (event->kind == MIDR_PREFIX_SNAPSHOT_END ||
	    event->kind == MIDR_PREFIX_EOR)
		return event->generation <= daemon->prefix_generation ? 0 : -EINVAL;
	if (event->kind != MIDR_PREFIX_UPSERT &&
	    event->kind != MIDR_PREFIX_WITHDRAW)
		return -EINVAL;
	if (event->generation <= daemon->prefix_generation)
		return 0;
	stage->active = true;
	stage->ended = true;
	stage->generation = event->generation;
	stage->originator = event->originator;
	memcpy(stage->prefixes, daemon->ipc_prefixes,
	       daemon->ipc_prefix_count * sizeof(*stage->prefixes));
	stage->count = daemon->ipc_prefix_count;
	ret = prefix_stage_apply(stage, event);
	if (!ret)
		ret = commit_prefixes(daemon, stage->prefixes, stage->count);
	if (!ret)
		daemon->prefix_generation = event->generation;
	prefix_stage_reset(stage);
	return ret;
}

static int prefix_ipc_event(void *arg, const struct midr_prefix_event *event)
{
	return prefix_event(arg, event);
}

static void prefix_ipc_disconnect(void *arg, int reason)
{
	struct midr_context *daemon = arg;

	(void)reason;
	prefix_stage_reset(&daemon->prefix_stage);
	/* Provider generations are ordered within one IPC connection.  A
	 * restarted provider may begin again at generation 1; retain the last
	 * committed Prefix view, but let its replacement establish a new epoch. */
	daemon->prefix_generation = 0;
}

static bool local_link_key_equal(const struct midrd_link_config *left,
				 const struct midr_local_link *right)
{
	return left->remote == right->remote_node_id &&
	       left->link_id == right->link_id;
}

static int local_link_find(const struct midr_local_link *links, size_t count,
			   const struct midr_local_link *link)
{
	for (size_t i = 0; i < count; i++)
		if (links[i].remote_node_id == link->remote_node_id &&
		    links[i].link_id == link->link_id)
			return (int)i;
	return -1;
}

static const struct midrd_link_config *local_link_lookup(
	const struct midr_context *daemon, const struct midr_local_link *link)
{
	for (size_t i = 0; i < daemon->link_count; i++)
		if (local_link_key_equal(&daemon->links[i], link))
			return &daemon->links[i];
	return NULL;
}

static int local_link_version_find(
	const struct midrd_local_link_version *versions, size_t count,
	const struct midr_local_link *link)
{
	for (size_t i = 0; i < count; i++)
		if (versions[i].remote == link->remote_node_id &&
		    versions[i].link_id == link->link_id)
			return (int)i;
	return -1;
}

static int local_link_version_set(
	struct midrd_local_link_version *versions, size_t *count,
	uint32_t remote, uint64_t link_id, uint64_t version)
{
	if (!versions || !count || !remote || !version)
		return -EINVAL;
	for (size_t i = 0; i < *count; i++) {
		if (versions[i].remote != remote ||
		    versions[i].link_id != link_id)
			continue;
		if (version > versions[i].version)
			versions[i].version = version;
		return 0;
	}
	if (*count == MIDRD_MAX_SNAPSHOT)
		return -ENOSPC;
	versions[*count] = (struct midrd_local_link_version){
		.remote = remote,
		.link_id = link_id,
		.version = version,
	};
	(*count)++;
	return 0;
}

static bool local_link_input_equal(const struct midrd_link_config *current,
				   const struct midr_local_link *input)
{
	return current->input_version == input->version &&
	       current->local_ifindex == input->local_ifindex &&
	       current->address_family == input->family &&
	       current->latest_metrics.rtt_us == input->rtt_us &&
	       current->latest_metrics.loss_ppm == input->loss_ppm &&
	       current->latest_metrics.available_bandwidth_kbps ==
		       input->available_bandwidth_kbps &&
	       current->measurement_sequence == input->measurement_sequence &&
	       current->measurement_timestamp_ms ==
		       input->measurement_timestamp_ms &&
	       !memcmp(current->local_address, input->local_address,
		       sizeof(current->local_address)) &&
	       !memcmp(current->remote_address, input->remote_address,
		       sizeof(current->remote_address));
}

static bool cost_interval_elapsed(uint64_t last_ms, uint64_t now_ms)
{
	return now_ms >= last_ms &&
	       now_ms - last_ms >= MIDR_LINK_COST_MIN_ADVERTISEMENT_INTERVAL_MS;
}

static int local_link_config_at(const struct midr_local_link *input,
				const struct midrd_link_config *current,
				bool allow_version_reset, uint64_t now_ms,
				struct midrd_link_config *config)
{
	struct midr_cost_metrics metrics;
	uint32_t candidate;
	bool address_changed;
	int ret;

	if (!input || !config)
		return -EINVAL;
	metrics.rtt_us = input->rtt_us;
	metrics.loss_ppm = input->loss_ppm;
	metrics.available_bandwidth_kbps = input->available_bandwidth_kbps;
	ret = midr_cost_from_metrics(&metrics, &candidate);
	if (ret)
		return ret;
	if (!current) {
		memset(config, 0, sizeof(*config));
		config->remote = input->remote_node_id;
		config->link_id = input->link_id;
		config->metric = candidate;
		config->last_cost_advertised_ms = now_ms;
	} else {
		if (!allow_version_reset && input->version < current->input_version)
			return -ESTALE;
		if (!allow_version_reset &&
		    input->version == current->input_version) {
			if (!local_link_input_equal(current, input))
				return -EEXIST;
			*config = *current;
			return 0;
		}
		*config = *current;
	}
	address_changed = current &&
		(current->address_family != input->family ||
		 memcmp(current->local_address, input->local_address,
			sizeof(current->local_address)) ||
		 memcmp(current->remote_address, input->remote_address,
			sizeof(current->remote_address)));
	config->remote = input->remote_node_id;
	config->local_ifindex = input->local_ifindex;
	config->link_id = input->link_id;
	config->address_family = input->family;
	memcpy(config->local_address, input->local_address,
	       sizeof(config->local_address));
	memcpy(config->remote_address, input->remote_address,
	       sizeof(config->remote_address));
	config->latest_metrics = metrics;
	config->candidate_metric = candidate;
	config->input_version = input->version;
	config->measurement_sequence = input->measurement_sequence;
	config->measurement_timestamp_ms = input->measurement_timestamp_ms;
	if (!current || address_changed) {
		config->metric = candidate;
		config->last_cost_advertised_ms = now_ms;
		config->cost_pending = false;
	} else if (!midr_cost_change_significant(current->metric, candidate)) {
		config->metric = current->metric;
		config->cost_pending = false;
	} else if (cost_interval_elapsed(current->last_cost_advertised_ms,
					  now_ms)) {
		config->metric = candidate;
		config->last_cost_advertised_ms = now_ms;
		config->cost_pending = false;
	} else {
		config->metric = current->metric;
		config->cost_pending = true;
	}
	return 0;
}

static void link_identity(struct midr_context *daemon,
			  const struct midrd_link_config *link,
			  struct midr_core_identity *identity)
{
	memset(identity, 0, sizeof(*identity));
	identity->type = MIDR_CORE_LINK;
	identity->originator = daemon->node_id;
	identity->remote = link->remote;
	identity->link_id = link->link_id;
}

static void link_object(struct midr_context *daemon,
			const struct midrd_link_config *link,
			struct midr_core_object *object)
{
	memset(object, 0, sizeof(*object));
	link_identity(daemon, link, &object->identity);
	object->state = MIDR_CORE_ACTIVE;
	object->address_family = link->address_family;
	object->metric = link->metric;
	memcpy(object->local_address, link->local_address,
	       sizeof(object->local_address));
	memcpy(object->remote_address, link->remote_address,
	       sizeof(object->remote_address));
}

static uint32_t local_ifindex_lookup(void *arg, uint32_t local_node_id,
				     uint32_t remote_node_id,
				     uint64_t link_id)
{
	const struct midr_context *daemon = arg;

	if (!daemon || local_node_id != daemon->node_id)
		return 0;
	for (size_t i = 0; i < daemon->link_count; i++)
		if (daemon->links[i].remote == remote_node_id &&
		    daemon->links[i].link_id == link_id)
			return daemon->links[i].local_ifindex;
	return 0;
}

static bool local_ifindices_equal(const struct midrd_link_config *left,
				  size_t left_count,
				  const struct midrd_link_config *right,
				  size_t right_count)
{
	if (left_count != right_count)
		return false;
	for (size_t i = 0; i < left_count; i++) {
		bool found = false;

		for (size_t j = 0; j < right_count; j++) {
			if (left[i].remote != right[j].remote ||
			    left[i].link_id != right[j].link_id)
				continue;
			if (left[i].local_ifindex != right[j].local_ifindex)
				return false;
			found = true;
			break;
		}
		if (!found)
			return false;
	}
	return true;
}

static int commit_local_facts_at(
	struct midr_context *daemon, const struct midr_local_membership *membership,
	bool membership_present, const struct midr_local_link *links,
	size_t link_count, uint64_t membership_floor,
	const struct midrd_local_link_version *version_floors,
	size_t version_floor_count, uint64_t now_ms)
{
	struct midrd_link_config desired_links[MIDRD_MAX_LINKS];
	struct midrd_link_config saved_links[MIDRD_MAX_LINKS];
	struct midrd_local_link_version desired_versions[MIDRD_MAX_SNAPSHOT];
	struct midr_owned *staged = NULL, *old_owned = NULL;
	struct midr_core_identity *saved_group_prefixes = NULL;
	struct midr_core_identity membership_identity = {
		.type = MIDR_CORE_MEMBERSHIP,
	};
	size_t saved_group_prefix_count;
	size_t saved_link_count;
	uint32_t saved_representative_group;
	uint32_t saved_representative_node;
	uint64_t saved_takeover_ready_at;
	bool saved_representative_committed;
	bool saved_group_reconcile_pending;
	bool membership_changed = false;
	bool local_ifindex_changed;
	bool owned_staged = false;
	bool allow_version_reset;
	size_t desired_version_count;
	int ret;

	if (!daemon || (membership_present && !membership) ||
	    (link_count && !links) || link_count > MIDRD_MAX_LINKS ||
	    (version_floor_count && !version_floors) ||
	    version_floor_count > MIDRD_MAX_SNAPSHOT)
		return -EINVAL;
	allow_version_reset = daemon->local_generation == 0;
	saved_link_count = daemon->link_count;
	memcpy(saved_links, daemon->links,
	       saved_link_count * sizeof(*saved_links));
	desired_version_count = allow_version_reset
				? 0
				: daemon->local_link_version_count;
	if (desired_version_count)
		memcpy(desired_versions, daemon->local_link_versions,
		       desired_version_count * sizeof(*desired_versions));
	if (membership_present && !allow_version_reset &&
	    daemon->membership_version) {
		if (membership->version < daemon->membership_version)
			return -ESTALE;
		if (membership->version == daemon->membership_version) {
			if (!daemon->group_id)
				return -ESTALE;
			if (membership->group != daemon->group_id)
				return -EEXIST;
		}
	}
	for (size_t i = 0; i < link_count; i++) {
		const struct midrd_link_config *current =
			local_link_lookup(daemon, &links[i]);
		int version_index = local_link_version_find(
			desired_versions, desired_version_count, &links[i]);

		if (!allow_version_reset && version_index >= 0) {
			uint64_t floor = desired_versions[version_index].version;

			if (links[i].version < floor)
				return -ESTALE;
			if (links[i].version == floor && !current)
				return -ESTALE;
		}

		ret = local_link_config_at(&links[i], current,
					   allow_version_reset, now_ms,
					   &desired_links[i]);
		if (ret)
			return ret;
		if (version_index < 0) {
			if (desired_version_count == MIDRD_MAX_SNAPSHOT)
				return -ENOSPC;
			version_index = (int)desired_version_count++;
			desired_versions[version_index].remote =
				links[i].remote_node_id;
			desired_versions[version_index].link_id = links[i].link_id;
		}
		desired_versions[version_index].version = links[i].version;
	}
	for (size_t i = 0; i < version_floor_count; i++) {
		ret = local_link_version_set(
			desired_versions, &desired_version_count,
			version_floors[i].remote, version_floors[i].link_id,
			version_floors[i].version);
		if (ret)
			return ret;
	}
	local_ifindex_changed = !local_ifindices_equal(
		saved_links, saved_link_count, desired_links, link_count);
	saved_group_prefix_count = daemon->group_prefix_count;
	saved_representative_group = daemon->representative_group;
	saved_representative_node = daemon->representative_node;
	saved_takeover_ready_at = daemon->takeover_ready_at;
	saved_representative_committed = daemon->representative_committed;
	saved_group_reconcile_pending = daemon->group_reconcile_pending;
	ret = midr_owned_clone(daemon->owned, publish_owned, daemon, &staged);
	if (ret)
		return ret;
	if (saved_group_prefix_count) {
		saved_group_prefixes = malloc(saved_group_prefix_count *
					     sizeof(*saved_group_prefixes));
		if (!saved_group_prefixes) {
			ret = -ENOMEM;
			goto failed;
		}
		memcpy(saved_group_prefixes, daemon->group_prefixes,
		       saved_group_prefix_count * sizeof(*saved_group_prefixes));
	}
	ret = midr_engine_begin_batch(daemon->engine);
	if (ret)
		goto failed;
	daemon->owned_commit_active = true;
	daemon->owned_commit_now_ms = now_ms;
	membership_identity.originator = daemon->node_id;
	if (!membership_present) {
		ret = midr_owned_withdraw(staged, &membership_identity);
		if (ret == -ENOENT)
			ret = 0;
		else if (!ret)
			membership_changed = true;
		if (ret)
			goto abort;
	} else {
		struct midr_core_object current;
		struct midr_core_object object = {
			.identity = membership_identity,
			.state = MIDR_CORE_ACTIVE,
			.group = membership->group,
		};

		if (midr_owned_lookup(staged, &membership_identity, &current) ||
		    current.state != MIDR_CORE_ACTIVE ||
		    !midr_core_object_semantic_equal(&current, &object)) {
			ret = midr_owned_upsert(staged, &object);
			if (ret)
				goto abort;
			membership_changed = true;
		}
	}
	for (size_t i = 0; i < daemon->link_count; i++) {
		bool present = false;
		struct midr_core_identity identity;

		for (size_t j = 0; j < link_count; j++)
			if (local_link_key_equal(&daemon->links[i], &links[j])) {
				present = true;
				break;
			}
		if (present)
			continue;
		link_identity(daemon, &daemon->links[i], &identity);
		ret = midr_owned_withdraw(staged, &identity);
		if (ret == -ENOENT)
			ret = 0;
		if (ret)
			goto abort;
	}
	for (size_t i = 0; i < link_count; i++) {
		struct midr_core_object current;
		struct midr_core_object object;

		link_object(daemon, &desired_links[i], &object);
		{
			if (!midr_owned_lookup(staged, &object.identity, &current) &&
			    current.state == MIDR_CORE_ACTIVE &&
			    midr_core_object_semantic_equal(&current, &object))
				continue;
		}
		ret = midr_owned_upsert(staged, &object);
		if (ret)
			goto abort;
	}
	old_owned = daemon->owned;
	daemon->owned = staged;
	owned_staged = true;
	ret = reconcile_group_prefixes(daemon, now_ms);
	if (ret)
		goto abort;
	/* TED derivation runs synchronously inside engine_end_batch().  Expose the
	 * candidate local metadata for that derivation, and restore it if the
	 * canonical transaction itself fails. */
	memcpy(daemon->links, desired_links,
	       link_count * sizeof(*desired_links));
	daemon->link_count = link_count;
	{
		uint64_t generation = midr_engine_generation(daemon->engine);

		ret = midr_engine_end_batch(daemon->engine, now_ms);
		if (ret)
			goto failed;
		if (local_ifindex_changed &&
		    midr_engine_generation(daemon->engine) == generation)
			(void)midr_engine_republish(daemon->engine, now_ms);
	}
	note_engine_publication(daemon);
	daemon->owned_commit_active = false;
	owned_staged = false;
	staged = NULL;
	midr_owned_destroy(&old_owned);
	daemon->group_id = membership_present ? membership->group : 0;
	if (membership_present)
		daemon->membership_version = membership->version;
	else
		daemon->membership_version = membership_floor;
	memcpy(daemon->local_link_versions, desired_versions,
	       desired_version_count * sizeof(*desired_versions));
	daemon->local_link_version_count = desired_version_count;
	free(saved_group_prefixes);
	drain_events(daemon, NULL);
	if (membership_changed)
		reflood_scope(daemon, NULL);
	(void)reconcile_group_prefixes(daemon, now_ms);
	return 0;

abort:
	daemon->owned_commit_active = false;
	(void)midr_engine_abort_batch(daemon->engine);
failed:
	daemon->owned_commit_active = false;
	memcpy(daemon->links, saved_links,
	       saved_link_count * sizeof(*saved_links));
	daemon->link_count = saved_link_count;
	if (owned_staged) {
		staged = daemon->owned;
		daemon->owned = old_owned;
		if (saved_group_prefix_count)
			memcpy(daemon->group_prefixes, saved_group_prefixes,
			       saved_group_prefix_count *
				       sizeof(*saved_group_prefixes));
		daemon->group_prefix_count = saved_group_prefix_count;
		daemon->representative_group = saved_representative_group;
		daemon->representative_node = saved_representative_node;
		daemon->takeover_ready_at = saved_takeover_ready_at;
		daemon->representative_committed =
			saved_representative_committed;
		daemon->group_reconcile_pending =
			saved_group_reconcile_pending;
	}
	free(saved_group_prefixes);
	ret = preserve_staged_sequence(daemon, staged, ret);
	midr_owned_destroy(&staged);
	return ret;
}

static void local_stage_reset(struct midrd_local_stage *stage)
{
	memset(stage, 0, sizeof(*stage));
}

static int local_stage_apply(struct midrd_local_stage *stage,
			     const struct midr_local_event *event)
{
	if (event->kind == MIDR_LOCAL_MEMBERSHIP) {
		if (stage->membership_present)
			return -EEXIST;
		stage->membership = event->fact.membership;
		stage->membership_present = true;
		return 0;
	}
	if (event->kind != MIDR_LOCAL_LINK)
		return -EINVAL;
	if (local_link_find(stage->links, stage->link_count,
			    &event->fact.link) >= 0)
		return -EEXIST;
	if (stage->link_count == MIDRD_MAX_LINKS)
		return -ENOSPC;
	stage->links[stage->link_count++] = event->fact.link;
	return 0;
}

static int local_stage_delta_apply(struct midrd_local_stage *stage,
				   const struct midr_local_event *event)
{
	const struct midr_local_link *link = &event->fact.link;
	int index;
	int version_index;

	if (event->kind == MIDR_LOCAL_MEMBERSHIP) {
		if (stage->membership_floor > event->fact.membership.version)
			return -ESTALE;
		if (stage->membership_floor == event->fact.membership.version) {
			if (!stage->membership_present)
				return -ESTALE;
			if (stage->membership.group != event->fact.membership.group)
				return -EEXIST;
			return 0;
		}
		stage->membership = event->fact.membership;
		stage->membership_present = true;
		stage->membership_floor = event->fact.membership.version;
		return 0;
	}
	if (event->kind == MIDR_LOCAL_MEMBERSHIP_WITHDRAW) {
		if (event->fact.membership.version < stage->membership_floor)
			return -ESTALE;
		if (event->fact.membership.version == stage->membership_floor &&
		    !stage->membership_present)
			return 0;
		if (event->fact.membership.version == stage->membership_floor &&
		    stage->membership_present)
			return -EEXIST;
		stage->membership_present = false;
		stage->membership_floor = event->fact.membership.version;
		memset(&stage->membership, 0, sizeof(stage->membership));
		return 0;
	}
	if (event->kind != MIDR_LOCAL_LINK &&
	    event->kind != MIDR_LOCAL_LINK_WITHDRAW)
		return -EINVAL;
	version_index = local_link_version_find(stage->link_versions,
						stage->link_version_count, link);
	if (version_index >= 0 &&
	    link->version < stage->link_versions[version_index].version)
		return -ESTALE;
	index = local_link_find(stage->links, stage->link_count, link);
	if (event->kind == MIDR_LOCAL_LINK_WITHDRAW) {
		if (version_index >= 0 &&
		    link->version == stage->link_versions[version_index].version) {
			if (index < 0)
				return 0;
			return -EEXIST;
		}
		if (index >= 0)
			stage->links[index] = stage->links[--stage->link_count];
		return local_link_version_set(stage->link_versions,
					       &stage->link_version_count,
					       link->remote_node_id, link->link_id,
					       link->version);
	}
	if (version_index >= 0 &&
	    link->version == stage->link_versions[version_index].version) {
		if (index < 0)
			return -ESTALE;
		if (memcmp(&stage->links[index], link, sizeof(*link)))
			return -EEXIST;
		return 0;
	}
	if (index >= 0)
		stage->links[index] = *link;
	else {
		if (stage->link_count == MIDRD_MAX_LINKS)
			return -ENOSPC;
		stage->links[stage->link_count++] = *link;
	}
	return 0;
}

static int local_delta_at(struct midr_context *daemon,
			  const struct midr_local_event *event, uint64_t now_ms)
{
	struct midrd_local_stage stage = {0};
	int ret;

	if (!daemon || !event || !daemon->local_generation ||
	    event->generation != daemon->local_generation)
		return event && event->generation < daemon->local_generation ? 0 :
			-EAGAIN;
	stage.generation = daemon->local_generation;
	stage.originator = daemon->node_id;
	stage.membership_present = daemon->group_id != 0;
	stage.membership.group = daemon->group_id;
	stage.membership.version = daemon->membership_version;
	stage.membership_floor = daemon->membership_version;
	for (size_t i = 0; i < daemon->link_count; i++) {
		stage.links[i].remote_node_id = daemon->links[i].remote;
		stage.links[i].local_ifindex = daemon->links[i].local_ifindex;
		stage.links[i].family = daemon->links[i].address_family;
		stage.links[i].link_id = daemon->links[i].link_id;
		stage.links[i].version = daemon->links[i].input_version;
		stage.links[i].rtt_us = daemon->links[i].latest_metrics.rtt_us;
		stage.links[i].loss_ppm = daemon->links[i].latest_metrics.loss_ppm;
		stage.links[i].available_bandwidth_kbps =
			daemon->links[i].latest_metrics.available_bandwidth_kbps;
		stage.links[i].measurement_sequence =
			daemon->links[i].measurement_sequence;
		stage.links[i].measurement_timestamp_ms =
			daemon->links[i].measurement_timestamp_ms;
		memcpy(stage.links[i].local_address,
		       daemon->links[i].local_address,
		       sizeof(stage.links[i].local_address));
		memcpy(stage.links[i].remote_address,
		       daemon->links[i].remote_address,
		       sizeof(stage.links[i].remote_address));
	}
	stage.link_count = daemon->link_count;
	stage.link_version_count = daemon->local_link_version_count;
	memcpy(stage.link_versions, daemon->local_link_versions,
	       daemon->local_link_version_count * sizeof(*stage.link_versions));
	ret = local_stage_delta_apply(&stage, event);
	if (ret)
		return ret;
	ret = commit_local_facts_at(
		daemon,
		stage.membership_present ? &stage.membership : NULL,
		stage.membership_present, stage.links, stage.link_count,
		stage.membership_floor, stage.link_versions,
		stage.link_version_count, now_ms);
	return ret;
}

static int local_event_at(void *arg, const struct midr_local_event *event,
			  uint64_t now_ms)
{
	struct midr_context *daemon = arg;
	struct midrd_local_stage *stage;
	int ret;

	if (!daemon || midr_local_event_validate(event) ||
	    event->originator != daemon->node_id)
		return -EINVAL;
	stage = &daemon->local_stage;
	if (event->kind == MIDR_LOCAL_SNAPSHOT_BEGIN) {
		if (stage->active && event->generation < stage->generation)
			return 0;
		local_stage_reset(stage);
		stage->active = true;
		stage->generation = event->generation;
		stage->originator = event->originator;
		stage->discard = event->generation <= daemon->local_generation;
		if (!stage->discard && daemon->local_generation) {
			stage->membership_floor = daemon->membership_version;
			stage->link_version_count = daemon->local_link_version_count;
			memcpy(stage->link_versions, daemon->local_link_versions,
			       stage->link_version_count * sizeof(*stage->link_versions));
		}
		return 0;
	}
	if (!stage->active) {
		if ((event->kind == MIDR_LOCAL_MEMBERSHIP ||
		     event->kind == MIDR_LOCAL_MEMBERSHIP_WITHDRAW ||
		     event->kind == MIDR_LOCAL_LINK ||
		     event->kind == MIDR_LOCAL_LINK_WITHDRAW)) {
			if (event->generation < daemon->local_generation)
				return 0;
			if (event->generation > daemon->local_generation)
				return -EAGAIN;
			return local_delta_at(daemon, event, now_ms);
		}
		if (event->kind == MIDR_LOCAL_SNAPSHOT_END ||
		    event->kind == MIDR_LOCAL_EOR)
			return event->generation <= daemon->local_generation ? 0 :
				-EINVAL;
		return -EINVAL;
	}
	if (event->generation < stage->generation)
		return 0;
	if (event->generation != stage->generation ||
	    event->originator != stage->originator)
		return -EINVAL;
	if (event->kind == MIDR_LOCAL_SNAPSHOT_END) {
		stage->ended = true;
		return 0;
	}
	if (event->kind == MIDR_LOCAL_EOR) {
		if (!stage->ended)
			return -EINVAL;
		ret = stage->discard
			      ? 0
			      : commit_local_facts_at(
					daemon,
								stage->membership_present
									? &stage->membership
									: NULL,
								stage->membership_present,
								stage->links, stage->link_count,
								stage->membership_floor,
								stage->link_versions,
								stage->link_version_count, now_ms);
		if (!ret && !stage->discard)
			daemon->local_generation = stage->generation;
		local_stage_reset(stage);
		return ret;
	}
	if (stage->ended) {
		if (stage->discard)
			return 0;
		return local_stage_delta_apply(stage, event);
	}
	if (stage->discard)
		return 0;
	if (event->kind == MIDR_LOCAL_MEMBERSHIP_WITHDRAW ||
	    event->kind == MIDR_LOCAL_LINK_WITHDRAW)
		return local_stage_delta_apply(stage, event);
	return local_stage_apply(stage, event);
}

static int local_event(void *arg, const struct midr_local_event *event)
{
	return local_event_at(arg, event, mono_ms());
}

static void local_ipc_disconnect(void *arg, int reason)
{
	struct midr_context *daemon = arg;

	(void)reason;
	local_stage_reset(&daemon->local_stage);
	/* Generations and input versions are scoped to one provider connection.
	 * Keep the last committed facts while allowing a restarted provider to
	 * establish a complete replacement baseline beginning at generation 1. */
	daemon->local_generation = 0;
}

static int publish_pending_link_costs_at(struct midr_context *daemon,
					 uint64_t now_ms)
{
	struct midrd_link_config desired[MIDRD_MAX_LINKS];
	struct midr_owned *staged = NULL, *old_owned;
	bool changed = false;
	int ret;

	if (!daemon)
		return -EINVAL;
	memcpy(desired, daemon->links,
	       daemon->link_count * sizeof(*desired));
	for (size_t i = 0; i < daemon->link_count; i++) {
		if (!desired[i].cost_pending)
			continue;
		if (!midr_cost_change_significant(desired[i].metric,
						  desired[i].candidate_metric)) {
			desired[i].cost_pending = false;
			continue;
		}
		if (!cost_interval_elapsed(desired[i].last_cost_advertised_ms,
					   now_ms))
			continue;
		desired[i].metric = desired[i].candidate_metric;
		desired[i].last_cost_advertised_ms = now_ms;
		desired[i].cost_pending = false;
		changed = true;
	}
	if (!changed) {
		memcpy(daemon->links, desired,
		       daemon->link_count * sizeof(*desired));
		return 0;
	}
	ret = midr_owned_clone(daemon->owned, publish_owned, daemon, &staged);
	if (ret)
		return ret;
	ret = midr_engine_begin_batch(daemon->engine);
	if (ret)
		goto failed;
	daemon->owned_commit_active = true;
	daemon->owned_commit_now_ms = now_ms;
	for (size_t i = 0; i < daemon->link_count; i++) {
		struct midr_core_object object;

		if (desired[i].metric == daemon->links[i].metric)
			continue;
		link_object(daemon, &desired[i], &object);
		ret = midr_owned_upsert(staged, &object);
		if (ret)
			goto abort;
	}
	ret = midr_engine_end_batch(daemon->engine, now_ms);
	if (ret)
		goto failed;
	note_engine_publication(daemon);
	daemon->owned_commit_active = false;
	old_owned = daemon->owned;
	daemon->owned = staged;
	staged = NULL;
	midr_owned_destroy(&old_owned);
	memcpy(daemon->links, desired,
	       daemon->link_count * sizeof(*desired));
	drain_events(daemon, NULL);
	return 0;

abort:
	daemon->owned_commit_active = false;
	(void)midr_engine_abort_batch(daemon->engine);
failed:
	daemon->owned_commit_active = false;
	ret = preserve_staged_sequence(daemon, staged, ret);
	midr_owned_destroy(&staged);
	return ret;
}

static int install_local_prefix(struct midr_context *daemon, const char *text)
{
	char copy[128], *slash, *end;
	struct midr_prefix prefix = {.metric = 10};
	struct midr_prefix_provider_config config = {0};
	struct in_addr address4;
	struct in6_addr address6;
	unsigned long length;

	if (!text || strlen(text) >= sizeof(copy))
		return -EINVAL;
	strcpy(copy, text);
	slash = strchr(copy, '/');
	if (!slash)
		return -EINVAL;
	*slash++ = '\0';
	length = strtoul(slash, &end, 10);
	if (*end || length > 128)
		return -EINVAL;
	if (inet_pton(AF_INET, copy, &address4) == 1) {
		if (length > 32)
			return -EINVAL;
		prefix.family = MIDR_CORE_AF_IPV4;
		prefix.prefix_len = (uint8_t)length;
		memcpy(prefix.address, &address4, 4);
	} else if (inet_pton(AF_INET6, copy, &address6) == 1) {
		prefix.family = MIDR_CORE_AF_IPV6;
		prefix.prefix_len = (uint8_t)length;
		memcpy(prefix.address, &address6, 16);
	} else {
		return -EINVAL;
	}
	config.originator = daemon->node_id;
	config.on_event = local_provider_event;
	config.arg = daemon;
	if (midr_prefix_provider_create(&config, &daemon->prefix_provider))
		return -ENOMEM;
	return midr_prefix_provider_upsert(daemon->prefix_provider, &prefix);
}

static int install_local_membership(struct midr_context *daemon, uint32_t group_id)
{
	struct midr_core_object object = {0};
	int ret;

	if (!group_id)
		return -EINVAL;
	object.identity.type = MIDR_CORE_MEMBERSHIP;
	object.identity.originator = daemon->node_id;
	object.group = group_id;
	object.state = MIDR_CORE_ACTIVE;
	ret = midr_owned_upsert(daemon->owned, &object);
	if (ret)
		return ret;
	drain_events(daemon, NULL);
	reflood_scope(daemon, NULL);
	return 0;
}

static int install_local_link(struct midr_context *daemon,
			      const struct midrd_link_config *link)
{
	struct midr_core_object object;
	int ret;

	if (!daemon || !link || !link->remote || link->remote == daemon->node_id ||
	    !link->metric || link->metric == UINT32_MAX ||
	    (link->address_family != MIDR_CORE_AF_IPV4 &&
	     link->address_family != MIDR_CORE_AF_IPV6))
		return -EINVAL;
	link_object(daemon, link, &object);
	ret = midr_owned_upsert(daemon->owned, &object);
	if (ret)
		return ret;
	drain_events(daemon, NULL);
	return 0;
}

static bool identity_present(const struct midr_core_identity *identities,
			     size_t count,
			     const struct midr_core_identity *identity)
{
	for (size_t i = 0; i < count; i++)
		if (midr_core_identity_equal(&identities[i], identity))
			return true;
	return false;
}

static int withdraw_group_prefixes(struct midr_context *daemon)
{
	size_t index = 0;
	bool changed = false;
	int result = 0;

	while (index < daemon->group_prefix_count) {
		int ret = midr_owned_withdraw(daemon->owned,
					      &daemon->group_prefixes[index]);

		if (ret) {
			if (!result)
				result = ret;
			index++;
			continue;
		}
		daemon->group_prefixes[index] =
			daemon->group_prefixes[--daemon->group_prefix_count];
		changed = true;
	}
	if (changed && !daemon->owned_commit_active)
		drain_events(daemon, NULL);
	return result;
}

static int reconcile_group_prefixes(struct midr_context *daemon, uint64_t now_ms)
{
	struct midr_core_object *objects = NULL;
	struct midr_core_identity *desired = NULL;
	uint32_t group = 0, representative = 0;
	size_t object_count = 0, desired_count = 0, index = 0;
	bool changed = false;
	int ret = 0, result = 0;

	if (!daemon)
		return 0;
	if ((daemon->owned_commit_active
		     ? midr_engine_batch_membership(daemon->engine,
						 daemon->node_id, &group)
		     : midr_engine_membership(daemon->engine, daemon->node_id,
					      &group)) ||
	    (daemon->owned_commit_active
		     ? midr_engine_batch_representative(daemon->engine, group,
						     &representative)
		     : midr_engine_representative(daemon->engine, group,
						  &representative))) {
		group = 0;
		representative = 0;
	}
	if (group != daemon->representative_group ||
	    representative != daemon->representative_node) {
		result = withdraw_group_prefixes(daemon);
		daemon->representative_group = group;
		daemon->representative_node = representative;
		daemon->representative_committed = false;
		daemon->takeover_ready_at = representative == daemon->node_id
			? now_ms + daemon->takeover_delay_ms : 0;
	}
	if (!group || representative != daemon->node_id) {
		daemon->group_reconcile_pending = result != 0;
		return result;
	}
	if (!daemon->representative_committed) {
		if (now_ms < daemon->takeover_ready_at) {
			daemon->group_reconcile_pending = false;
			return result;
		}
		daemon->representative_committed = true;
	}
	objects = calloc(MIDRD_MAX_SNAPSHOT, sizeof(*objects));
	desired = calloc(MIDRD_MAX_SNAPSHOT, sizeof(*desired));
	if (!objects || !desired) {
		ret = -ENOMEM;
		goto done;
	}
	ret = daemon->owned_commit_active ?
		midr_engine_batch_snapshot(daemon->engine, now_ms, objects,
					   MIDRD_MAX_SNAPSHOT, &object_count) :
		midr_engine_snapshot(daemon->engine, now_ms, objects,
				     MIDRD_MAX_SNAPSHOT, &object_count);
	if (ret)
		goto done;
	for (size_t i = 0; i < object_count; i++) {
		struct midr_core_identity identity = {0};
		uint32_t owner_group;

		if (objects[i].state != MIDR_CORE_ACTIVE ||
		    objects[i].identity.type != MIDR_CORE_NODE_PREFIX ||
		    (daemon->owned_commit_active
			     ? midr_engine_batch_membership(
				       daemon->engine,
				       objects[i].identity.originator,
				       &owner_group)
			     : midr_engine_membership(
				       daemon->engine,
				       objects[i].identity.originator,
				       &owner_group)) ||
		    owner_group != group)
			continue;
		identity.type = MIDR_CORE_GROUP_PREFIX;
		identity.family = objects[i].identity.family;
		identity.prefix_len = objects[i].identity.prefix_len;
		identity.originator = daemon->node_id;
		identity.group = group;
		memcpy(identity.prefix, objects[i].identity.prefix,
		       sizeof(identity.prefix));
		if (!identity_present(desired, desired_count, &identity))
			desired[desired_count++] = identity;
	}
	while (index < daemon->group_prefix_count) {
		if (identity_present(desired, desired_count,
				     &daemon->group_prefixes[index])) {
			index++;
			continue;
		}
		ret = midr_owned_withdraw(daemon->owned,
					  &daemon->group_prefixes[index]);
		if (ret) {
			if (!result)
				result = ret;
			index++;
			continue;
		}
		daemon->group_prefixes[index] =
			daemon->group_prefixes[--daemon->group_prefix_count];
		changed = true;
	}
	for (size_t i = 0; i < desired_count; i++) {
		struct midr_core_object object = {0};

		if (identity_present(daemon->group_prefixes,
				     daemon->group_prefix_count, &desired[i]))
			continue;
		object.identity = desired[i];
		object.state = MIDR_CORE_ACTIVE;
		ret = midr_owned_upsert(daemon->owned, &object);
		if (ret) {
			if (!result)
				result = ret;
			continue;
		}
		daemon->group_prefixes[daemon->group_prefix_count++] = desired[i];
		changed = true;
	}
done:
	free(desired);
	free(objects);
	if (changed && !daemon->owned_commit_active)
		drain_events(daemon, NULL);
	daemon->group_reconcile_pending = (ret ? ret : result) != 0;
	return ret ? ret : result;
}

static void refresh_owned(struct midr_context *daemon)
{
	struct midr_core_identity identity = {0};

	if (daemon->have_local_identity)
		(void)midr_owned_refresh(daemon->owned, &daemon->local_identity);
	if (daemon->group_id) {
		identity.type = MIDR_CORE_MEMBERSHIP;
		identity.originator = daemon->node_id;
		(void)midr_owned_refresh(daemon->owned, &identity);
	}
	for (size_t i = 0; i < daemon->link_count; i++) {
		memset(&identity, 0, sizeof(identity));
		identity.type = MIDR_CORE_LINK;
		identity.originator = daemon->node_id;
		identity.remote = daemon->links[i].remote;
		identity.link_id = daemon->links[i].link_id;
		(void)midr_owned_refresh(daemon->owned, &identity);
	}
	for (size_t i = 0; i < daemon->ipc_prefix_count; i++) {
		prefix_identity(daemon, &daemon->ipc_prefixes[i], &identity);
		(void)midr_owned_refresh(daemon->owned, &identity);
	}
	for (size_t i = 0; i < daemon->group_prefix_count; i++)
		(void)midr_owned_refresh(daemon->owned,
					 &daemon->group_prefixes[i]);
	drain_events(daemon, NULL);
}

static void periodic(struct midr_context *daemon, uint64_t now)
{
	(void)publish_pending_link_costs_at(daemon, now);
	if (daemon->ted_rebuild_pending && now >= daemon->next_ted_retry) {
		(void)rebuild_ted(daemon);
		daemon->next_ted_retry = now + MIDRD_TED_RETRY_MS;
	}
	if (now >= daemon->next_refresh) {
		refresh_owned(daemon);
		daemon->next_refresh = now + daemon->lifetime_ms / 3U;
	}
	if (now >= daemon->next_expire) {
		size_t expired = 0;
		int ret;

		ret = midr_engine_expire(daemon->engine, now, &expired);
		if (!ret)
			note_engine_publication(daemon);
		if (!ret && expired) {
			drain_events(daemon, NULL);
			(void)reconcile_group_prefixes(daemon, now);
		}
		daemon->next_expire = now + 100U;
	}
	if (daemon->group_reconcile_pending ||
	    (!daemon->representative_committed && daemon->takeover_ready_at &&
	     now >= daemon->takeover_ready_at))
		(void)reconcile_group_prefixes(daemon, now);
}

static void midrd_hello_timer(struct event *event)
{
	struct midr_context *daemon = EVENT_ARG(event);

	daemon->hello_event = NULL;
	if (daemon->terminating)
		return;
	for (size_t i = 0; i < daemon->peer_count; i++)
		(void)send_hello(daemon, &daemon->peers[i].endpoint);
	event_add_timer_msec(daemon->master, midrd_hello_timer, daemon,
			     daemon->hello_ms, &daemon->hello_event);
}

static void midrd_keepalive_timer(struct event *event)
{
	struct midr_context *daemon = EVENT_ARG(event);

	daemon->keepalive_event = NULL;
	if (daemon->terminating)
		return;
	for (size_t i = 0; i < daemon->peer_count; i++)
		(void)send_frame(daemon, &daemon->peers[i].endpoint,
				 MIDR_WIRE_KEEPALIVE, NULL, 0);
	event_add_timer_msec(daemon->master, midrd_keepalive_timer, daemon,
			     daemon->hello_ms, &daemon->keepalive_event);
}

enum midrd_shutdown_result {
	MIDRD_SHUTDOWN_COMPLETE,
	MIDRD_SHUTDOWN_DEGRADED,
	MIDRD_SHUTDOWN_GENERATION_FAILED,
};

static enum midrd_shutdown_result shutdown_result(size_t remaining,
						 size_t pending,
						 uint64_t write_failures)
{
	if (remaining)
		return MIDRD_SHUTDOWN_GENERATION_FAILED;
	if (pending || write_failures)
		return MIDRD_SHUTDOWN_DEGRADED;
	return MIDRD_SHUTDOWN_COMPLETE;
}

static void shutdown_withdraw(struct midr_context *daemon)
{
	uint64_t deadline;
	bool generation_failed = false;
	bool attempted = false;

	if (!daemon || !daemon->owned)
		return;
	daemon->shutdown_active = true;
	daemon->shutdown_write_failures = 0;
	deadline = mono_ms() + MIDRD_SHUTDOWN_WAIT_MS;
	for (;;) {
		size_t withdrawn = 0;
		int ret = midr_owned_withdraw_all(daemon->owned, &withdrawn);

		attempted = true;
		if (withdrawn)
			drain_events(daemon, NULL);
		if (ret)
			generation_failed = true;
		if (!midr_owned_count(daemon->owned))
			break;
		if (mono_ms() >= deadline)
			break;
		if (daemon->transport)
			(void)midr_transport_poll(daemon->transport, 10);
	}
	/* Count one failure for this teardown generation.  Repeated polls below
	 * are retries, not additional teardown generations. */
	if (generation_failed)
		daemon->shutdown_generation_failures++;
	{
		size_t remaining = midr_owned_count(daemon->owned);
		size_t pending = daemon->transport
			? midr_transport_pending(daemon->transport) : 0;
		enum midrd_shutdown_result result =
			shutdown_result(remaining, pending, daemon->shutdown_write_failures);

		switch (result) {
		case MIDRD_SHUTDOWN_GENERATION_FAILED:
			printf("node=%" PRIu32
			       " shutdown=GENERATION_FAILED remaining=%zu failures=%" PRIu64
			       "\n", daemon->node_id, remaining,
			       daemon->shutdown_generation_failures);
			break;
		case MIDRD_SHUTDOWN_DEGRADED:
			printf("node=%" PRIu32
			       " shutdown=DEGRADED pending=%zu dropped=%" PRIu64
			       " generation-failures=%" PRIu64 "\n", daemon->node_id,
			       pending, daemon->shutdown_write_failures,
			       daemon->shutdown_generation_failures);
			break;
		case MIDRD_SHUTDOWN_COMPLETE:
			if (attempted)
				printf("node=%" PRIu32
				       " shutdown=COMPLETE generation-failures=%" PRIu64
				       "\n", daemon->node_id,
				       daemon->shutdown_generation_failures);
			break;
		}
	}
	daemon->shutdown_active = false;
}

static void midr_context_finish(struct midr_context *daemon)
{
	if (!daemon)
		return;
	for (size_t i = 0; i < MIDRD_MAX_PEERS; i++)
		stage_release(&daemon->stages[i]);
	midr_local_ipc_server_destroy(&daemon->local_ipc);
	midr_prefix_ipc_server_destroy(&daemon->prefix_ipc);
	midr_transport_destroy(&daemon->transport);
	midr_prefix_provider_destroy(&daemon->prefix_provider);
	midr_owned_destroy(&daemon->owned);
	midr_ted_destroy(&daemon->ted);
	midr_consumer_destroy(&daemon->consumer);
	midr_engine_destroy(&daemon->engine);
}

static int midr_context_initialize(
	struct midr_context *daemon,
	const struct midr_transport_config *transport_config)
{
	if (!daemon || !transport_config)
		return -EINVAL;

	struct midr_engine_config engine_config = {
		.node_id = daemon->node_id,
		.max_objects = MIDRD_MAX_SNAPSHOT,
		.lifetime_ms = daemon->lifetime_ms,
	};
	struct midr_owned_config owned_config = {
		.originator = daemon->node_id,
		.max_objects = MIDRD_MAX_SNAPSHOT,
		.lifetime_ms = daemon->lifetime_ms,
		.sequence_file = daemon->sequence_file,
	};
	struct midr_ted_config ted_config = {
		.max_events = MIDRD_MAX_SNAPSHOT,
		.local_ifindex_lookup = local_ifindex_lookup,
		.local_ifindex_arg = daemon,
	};
	struct midr_consumer_config consumer_config = {
		.on_event = on_consumer_event,
		.arg = daemon,
	};
	struct midr_transport_callbacks transport_callbacks = {
		.on_frame = on_frame,
		.on_established = on_established,
		.on_closed = on_closed,
		.on_frame_written = on_frame_written,
		.on_frame_dropped = on_frame_dropped,
		.arg = daemon,
	};

	if (midr_engine_create(&engine_config, &daemon->engine) ||
	    midr_ted_create(&ted_config, &daemon->ted) ||
	    midr_consumer_create(&consumer_config, &daemon->consumer) ||
	    midr_engine_attach_ted(daemon->engine, daemon->ted) ||
	    midr_engine_attach_consumer(daemon->engine, daemon->consumer) ||
	    midr_owned_create(&owned_config, publish_owned, daemon,
			      &daemon->owned) ||
	    midr_transport_create(transport_config, &transport_callbacks,
				  &daemon->transport) ||
	    midr_transport_start(daemon->transport)) {
		midr_context_finish(daemon);
		return -1;
	}
	return 0;
}

static FRR_NORETURN void midrd_terminate(int status)
{
	struct midr_context *daemon = midrd_runtime;

	if (daemon && !daemon->terminating) {
		daemon->terminating = true;
		event_cancel(&daemon->poll_event);
		event_cancel(&daemon->hello_event);
		event_cancel(&daemon->keepalive_event);
		shutdown_withdraw(daemon);
		printf("midrd node=%" PRIu32 " final-objects=%zu\n",
		       daemon->node_id, midr_engine_count(daemon->engine));
		midr_context_finish(daemon);
	}
	if (midrd_di.pid_file)
		(void)unlink(midrd_di.pid_file);
	midrd_runtime = NULL;
	frr_fini();
	exit(status);
}

static void midrd_poll(struct event *event)
{
	struct midr_context *daemon = EVENT_ARG(event);
	uint64_t now;

	daemon->poll_event = NULL;
	if (daemon->terminating)
		return;
	now = mono_ms();
	periodic(daemon, now);
	if (daemon->stop_at && now >= daemon->stop_at)
		midrd_terminate(0);
	event_add_timer_msec(daemon->master, midrd_poll, daemon,
			     MIDRD_POLL_INTERVAL_MS, &daemon->poll_event);
}

int main(int argc, char **argv, char **envp)
{
	struct midr_context daemon = {
		.lifetime_ms = MIDRD_DEFAULT_LIFETIME,
		.hello_ms = MIDRD_DEFAULT_HELLO,
		.takeover_delay_ms = MIDRD_DEFAULT_TAKEOVER_DELAY,
	};
	struct midr_transport_config transport_config = {0};
	const char *listen_text = NULL, *prefix_text = NULL, *prefix_socket = NULL;
	const char *local_socket = NULL;
	int runtime_sec = 0;
	int exit_status = 1;
	int opt;

	(void)envp;
	frr_preinit(&midrd_di, argc, argv);
	frr_opt_add("", midrd_longopts, midrd_help);
	while ((opt = frr_getopt(argc, argv, NULL)) != EOF) {
		switch (opt) {
		case MIDRD_OPT_NODE_ID:
			daemon.node_id = (uint32_t)strtoul(optarg, NULL, 10);
			break;
		case MIDRD_OPT_LISTEN:
			listen_text = optarg;
			break;
		case MIDRD_OPT_PEER:
			if (daemon.peer_count == MIDRD_MAX_PEERS ||
			    parse_endpoint(optarg,
					   &daemon.peers[daemon.peer_count].endpoint)) {
				frr_help_exit(2);
			}
			daemon.peer_count++;
			break;
		case MIDRD_OPT_PREFIX:
			prefix_text = optarg;
			break;
		case MIDRD_OPT_LINK:
			if (daemon.link_count == MIDRD_MAX_LINKS ||
			    parse_link(optarg, &daemon.links[daemon.link_count])) {
				frr_help_exit(2);
			}
			daemon.link_count++;
			break;
		case MIDRD_OPT_GROUP:
			daemon.group_id = (uint32_t)strtoul(optarg, NULL, 10);
			break;
		case MIDRD_OPT_LOCAL_FACT_SOCKET:
			local_socket = optarg;
			break;
		case MIDRD_OPT_PREFIX_SOCKET:
			prefix_socket = optarg;
			break;
		case MIDRD_OPT_SEQUENCE_FILE:
			daemon.sequence_file = optarg;
			break;
		case MIDRD_OPT_LIFETIME:
			daemon.lifetime_ms = (uint32_t)strtoul(optarg, NULL, 10);
			break;
		case MIDRD_OPT_HOLD_TIME:
			daemon.hold_time_ms = (uint32_t)strtoul(optarg, NULL, 10);
			break;
		case MIDRD_OPT_TAKEOVER_DELAY:
			daemon.takeover_delay_ms = (uint32_t)strtoul(optarg, NULL, 10);
			break;
		case MIDRD_OPT_RUNTIME:
			runtime_sec = atoi(optarg);
			break;
		case MIDRD_OPT_PIDFILE:
			midrd_di.pid_file = optarg;
			break;
		case 0:
			break;
		default:
			frr_help_exit(2);
		}
	}
	if (!daemon.hold_time_ms)
		daemon.hold_time_ms = daemon.lifetime_ms;
	if (!daemon.node_id || !listen_text ||
	    daemon.lifetime_ms <= MIDRD_FORWARD_BUDGET_MS) {
		fprintf(stderr, "lifetime must be greater than forwarding budget %u ms\n",
			MIDRD_FORWARD_BUDGET_MS);
		return 2;
	}
	if (daemon.hold_time_ms <= daemon.hello_ms) {
		fprintf(stderr, "hold time must be greater than hello interval %u ms\n",
			daemon.hello_ms);
		return 2;
	}
	if (prefix_text && prefix_socket) {
		fprintf(stderr, "--prefix and --prefix-socket are mutually exclusive\n");
		return 2;
	}
	if (local_socket && (daemon.group_id || daemon.link_count)) {
		fprintf(stderr,
			"--group/--link and --local-fact-socket are mutually exclusive\n");
		return 2;
	}
	if (parse_endpoint(listen_text, &transport_config.local)) {
		fprintf(stderr, "invalid listen endpoint\n");
		return 2;
	}
	for (size_t i = 0; i < daemon.link_count; i++) {
		daemon.links[i].address_family =
			transport_config.local.family == MIDR_TRANSPORT_AF_IPV4
				? MIDR_CORE_AF_IPV4
				: MIDR_CORE_AF_IPV6;
		daemon.links[i].last_cost_advertised_ms = mono_ms();
	}
	transport_config.hello_interval_ms = daemon.hello_ms;
	transport_config.hold_time_ms = daemon.hold_time_ms;
	transport_config.tx_budget_ms = MIDRD_FORWARD_BUDGET_MS;
	transport_config.max_frame_size = MIDRD_MAX_FRAME + MIDR_WIRE_HEADER_LEN;
	daemon.master = frr_init();
	transport_config.master = daemon.master;
	if (midr_context_initialize(&daemon, &transport_config)) {
		fprintf(stderr, "midrd initialization failed\n");
		frr_fini();
		return 1;
	}
	if (prefix_socket) {
		struct midr_prefix_ipc_config ipc_config = {
			.master = daemon.master,
			.path = prefix_socket,
			.on_event = prefix_ipc_event,
			.on_disconnect = prefix_ipc_disconnect,
			.arg = &daemon,
		};

		if (midr_prefix_ipc_server_create(&ipc_config, &daemon.prefix_ipc) ||
		    midr_prefix_ipc_server_start(daemon.prefix_ipc)) {
			fprintf(stderr, "prefix IPC initialization failed\n");
			goto fail;
		}
	}
	if (local_socket) {
		struct midr_local_ipc_config ipc_config = {
			.master = daemon.master,
			.path = local_socket,
			.on_event = local_event,
			.on_disconnect = local_ipc_disconnect,
			.arg = &daemon,
		};

		if (midr_local_ipc_server_create(&ipc_config, &daemon.local_ipc) ||
		    midr_local_ipc_server_start(daemon.local_ipc)) {
			fprintf(stderr, "local fact IPC initialization failed\n");
			goto fail;
		}
	}
	for (size_t i = 0; i < daemon.peer_count; i++)
		if (midr_transport_connect(daemon.transport,
					    &daemon.peers[i].endpoint)) {
			fprintf(stderr, "peer connection setup failed\n");
			goto fail;
		}
	if (prefix_text && install_local_prefix(&daemon, prefix_text)) {
		fprintf(stderr, "invalid prefix\n");
		exit_status = 2;
		goto fail;
	}
	if (daemon.group_id && install_local_membership(&daemon, daemon.group_id)) {
		fprintf(stderr, "invalid group\n");
		exit_status = 2;
		goto fail;
	}
	for (size_t i = 0; i < daemon.link_count; i++)
		if (install_local_link(&daemon, &daemon.links[i])) {
			fprintf(stderr, "invalid link\n");
			exit_status = 2;
			goto fail;
		}
	(void)reconcile_group_prefixes(&daemon, mono_ms());
	{
		uint64_t now = mono_ms();

		daemon.next_refresh = now + daemon.lifetime_ms / 3U;
		daemon.next_expire = now + 100U;
		daemon.next_ted_retry = now + MIDRD_TED_RETRY_MS;
		daemon.stop_at = runtime_sec > 0 ?
			now + (uint64_t)runtime_sec * 1000U : 0;
	}
	printf("midrd node=%" PRIu32 " family=%u listen-port=%u\n",
	       daemon.node_id, transport_config.local.family,
	       transport_config.local.port);
	drain_events(&daemon, NULL);
	midrd_runtime = &daemon;
	(void)fflush(NULL);
	frr_config_fork();
	event_add_timer_msec(daemon.master, midrd_hello_timer, &daemon,
			     daemon.hello_ms, &daemon.hello_event);
	event_add_timer_msec(daemon.master, midrd_keepalive_timer, &daemon,
			     daemon.hello_ms / 2U ? daemon.hello_ms / 2U : 1U,
			     &daemon.keepalive_event);
	event_add_timer_msec(daemon.master, midrd_poll, &daemon, 0,
			     &daemon.poll_event);
	frr_run(daemon.master);
	midrd_terminate(0);

fail:
	midr_context_finish(&daemon);
	frr_fini();
	return exit_status;
}
