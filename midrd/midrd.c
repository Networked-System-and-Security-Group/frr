/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L

#include "midr-engine.h"
#include "midr-prefix-provider.h"
#include "midr-prefix-ipc.h"
#include "midr-spf.h"
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

#define MIDRD_MAX_PEERS 32U
#define MIDRD_MAX_LINKS 64U
#define MIDRD_MAX_SNAPSHOT 4096U
#define MIDRD_MAX_FRAME 4096U
#define MIDRD_DEFAULT_LIFETIME 6000U
#define MIDRD_DEFAULT_HELLO 1000U
#define MIDRD_DEFAULT_TAKEOVER_DELAY 3000U

static volatile sig_atomic_t stop_requested;

static void on_signal(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

struct midrd_peer_config {
	struct midr_transport_endpoint endpoint;
	uint32_t node_id;
};

/* Development-time static Link input.  The daemon is the owner of the
 * resulting object; the remote node and metric are the only wire-facing
 * parameters needed for a single link between two nodes. */
struct midrd_link_config {
	uint32_t remote;
	uint32_t metric;
	uint64_t link_id;
};

struct midrd_snapshot_stage {
	struct midr_transport_endpoint peer;
	struct midr_core_object *objects;
	size_t count;
	size_t capacity;
	bool active;
};

struct midrd {
	uint32_t node_id;
	uint32_t group_id;
	uint32_t lifetime_ms;
	uint32_t hello_ms;
	uint32_t takeover_delay_ms;
	uint64_t frame_sequence;
	struct midr_engine *engine;
	struct midr_owned *owned;
	struct midr_consumer *consumer;
	struct midr_prefix_provider *prefix_provider;
	struct midr_prefix_ipc *prefix_ipc;
	struct midr_transport *transport;
	struct midrd_peer_config peers[MIDRD_MAX_PEERS];
	struct midrd_link_config links[MIDRD_MAX_LINKS];
	struct midr_core_identity group_prefixes[MIDRD_MAX_SNAPSHOT];
	struct midrd_snapshot_stage stages[MIDRD_MAX_PEERS];
	size_t peer_count;
	size_t link_count;
	size_t group_prefix_count;
	uint32_t representative_group;
	uint32_t representative_node;
	uint64_t takeover_ready_at;
	bool representative_committed;
	bool group_reconcile_pending;
	struct midr_core_identity local_identity;
	bool have_local_identity;
	bool prefix_batch;
	const char *sequence_file;
	uint64_t next_hello;
	uint64_t next_keepalive;
	uint64_t next_refresh;
	uint64_t next_expire;
	uint64_t stop_at;
};

static int reconcile_group_prefixes(struct midrd *daemon, uint64_t now_ms);

static uint32_t peer_node_id(const struct midrd *daemon,
			     const struct midr_transport_endpoint *peer)
{
	if (!daemon || !peer)
		return 0;
	for (size_t i = 0; i < daemon->peer_count; i++)
		if (midr_transport_endpoint_equal(&daemon->peers[i].endpoint, peer))
			return daemon->peers[i].node_id;
	return 0;
}

static struct midrd_snapshot_stage *stage_for(struct midrd *daemon,
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
	if (!free_stage->objects)
		return NULL;
	free_stage->count = 0;
	free_stage->active = true;
	return free_stage;
}

static void stage_release(struct midrd_snapshot_stage *stage)
{
	if (!stage)
		return;
	free(stage->objects);
	memset(stage, 0, sizeof(*stage));
}

static uint64_t mono_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000U +
	       (uint64_t)ts.tv_nsec / 1000000U;
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
	/* A single static link per remote is sufficient for development input.
	 * The identity remains stable across refreshes and restarts. */
	link->link_id = link->remote;
	return 0;
}

static int send_frame(struct midrd *daemon,
		      const struct midr_transport_endpoint *peer,
		      uint8_t type, const uint8_t *payload, size_t payload_len)
{
	struct midr_transport_frame frame = {
		.version = MIDR_WIRE_VERSION,
		.type = type,
		.sequence = ++daemon->frame_sequence,
		.payload = payload,
		.payload_len = payload_len,
	};

	return midr_transport_send(daemon->transport, peer, &frame);
}

static int send_hello(struct midrd *daemon,
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

static int send_object(struct midrd *daemon,
			       const struct midr_transport_endpoint *peer,
			       const struct midr_core_object *object)
{
	uint8_t payload[MIDR_WIRE_OBJECT_LEN];
	size_t length;
	uint8_t type = object->state == MIDR_CORE_WITHDRAWN ?
		MIDR_WIRE_WITHDRAW : MIDR_WIRE_UPDATE;

	if (midr_wire_encode_object(object, payload, sizeof(payload), &length))
		return -EINVAL;
	return send_frame(daemon, peer, type, payload, length);
}

static int send_snapshot_object(struct midrd *daemon,
				const struct midr_transport_endpoint *peer,
				const struct midr_core_object *object)
{
	uint8_t payload[MIDR_WIRE_OBJECT_LEN];
	size_t length;

	if (midr_wire_encode_object(object, payload, sizeof(payload), &length))
		return -EINVAL;
	return send_frame(daemon, peer, MIDR_WIRE_SNAPSHOT_OBJECT, payload, length);
}

static void flood_object(struct midrd *daemon,
			 const struct midr_core_object *object,
			 const struct midr_transport_endpoint *except)
{
	for (size_t i = 0; i < daemon->peer_count; i++) {
		if (except && midr_transport_endpoint_equal(
				&daemon->peers[i].endpoint, except))
			continue;
		if (!midr_engine_export(daemon->engine, object,
					daemon->peers[i].node_id))
			continue;
		(void)send_object(daemon, &daemon->peers[i].endpoint, object);
	}
}

static void reflood_scope(struct midrd *daemon,
			  const struct midr_transport_endpoint *except)
{
	struct midr_core_object *objects;
	size_t count = 0;

	objects = calloc(MIDRD_MAX_SNAPSHOT, sizeof(*objects));
	if (!objects)
		return;
	if (!midr_engine_snapshot(daemon->engine, mono_ms(), objects,
				  MIDRD_MAX_SNAPSHOT, &count)) {
		for (size_t i = 0; i < count; i++)
			if (objects[i].state == MIDR_CORE_ACTIVE)
				flood_object(daemon, &objects[i], except);
	}
	free(objects);
}

static int apply_update(struct midrd *daemon,
			const struct midr_core_object *object,
			uint64_t now_ms, enum midr_core_result *result,
			bool *scope_changed)
{
	int ret;

	if (!daemon || !object || !result || !scope_changed)
		return -EINVAL;
	*scope_changed = false;
	ret = midr_engine_apply(daemon->engine, object, now_ms, result);
	if (!ret && *result == MIDR_CORE_ACCEPTED &&
	    object->identity.type == MIDR_CORE_MEMBERSHIP)
		*scope_changed = true;
	return ret;
}

static void drain_consumer(struct midrd *daemon)
{
	struct midr_consumer_event event;
	struct midr_consumer_snapshot snapshot = {0};
	struct midr_spf_route routes[MIDRD_MAX_SNAPSHOT];
	size_t route_count = 0;

	while (midr_consumer_event_next(daemon->consumer, &event) == 0)
		printf("node=%" PRIu32 " ted-event kind=%u generation=%" PRIu64 "\n",
		       daemon->node_id, event.kind, event.generation);
	if (midr_consumer_snapshot_acquire(daemon->consumer, &snapshot) == 0) {
		if (midr_spf_compute(&snapshot, daemon->node_id, routes,
				     MIDRD_MAX_SNAPSHOT,
				     &route_count) == 0) {
			printf("node=%" PRIu32 " spf generation=%" PRIu64
			       " routes=%zu\n", daemon->node_id, snapshot.generation,
			       route_count);
			for (size_t i = 0; i < route_count; i++)
				printf("node=%" PRIu32 " route originator=%" PRIu32
				       " metric=%" PRIu32 " reachable=%u\n",
				       daemon->node_id, routes[i].originator,
				       routes[i].metric, routes[i].reachable ? 1U : 0U);
		}
		midr_consumer_snapshot_release(&snapshot);
	}
}

static void drain_events(struct midrd *daemon,
			 const struct midr_transport_endpoint *except)
{
	struct midr_core_object object;

	while (midr_engine_event_next(daemon->engine, &object) == 0) {
		flood_object(daemon, &object, except);
		printf("node=%" PRIu32 " event state=%u type=%u originator=%" PRIu32
		       " group=%" PRIu32 " seq=%" PRIu64 "\n",
		       daemon->node_id, object.state, object.identity.type,
		       object.identity.originator, object.identity.group,
		       object.sequence);
	}
	drain_consumer(daemon);
}

static void send_snapshot(struct midrd *daemon,
			  const struct midr_transport_endpoint *peer)
{
	struct midr_core_object *objects;
	size_t count = 0;

	(void)send_frame(daemon, peer, MIDR_WIRE_SNAPSHOT_BEGIN, NULL, 0);
	objects = calloc(MIDRD_MAX_SNAPSHOT, sizeof(*objects));
	if (objects && midr_engine_snapshot(daemon->engine, mono_ms(), objects,
					    MIDRD_MAX_SNAPSHOT, &count) == 0)
		for (size_t i = 0; i < count; i++)
			if (objects[i].state == MIDR_CORE_ACTIVE &&
			    midr_engine_export(daemon->engine, &objects[i],
					       peer_node_id(daemon, peer)))
				(void)send_snapshot_object(daemon, peer, &objects[i]);
	free(objects);
	(void)send_frame(daemon, peer, MIDR_WIRE_SNAPSHOT_END, NULL, 0);
	(void)send_frame(daemon, peer, MIDR_WIRE_EOR, NULL, 0);
}

static int on_consumer_event(void *arg, const struct midr_consumer_event *event)
{
	struct midrd *daemon = arg;

	(void)daemon;
	return event ? 0 : -EINVAL;
}

static void on_established(void *arg,
			   const struct midr_transport_endpoint *peer)
{
	struct midrd *daemon = arg;

	printf("node=%" PRIu32 " established family=%u port=%u\n",
	       daemon->node_id, peer->family, peer->port);
	(void)send_hello(daemon, peer);
	send_snapshot(daemon, peer);
}

static void on_closed(void *arg, const struct midr_transport_endpoint *peer,
		      int reason)
{
	struct midrd *daemon = arg;
	struct midrd_snapshot_stage *stage = stage_for(daemon, peer, false);

	stage_release(stage);
	printf("node=%" PRIu32 " closed family=%u port=%u reason=%d\n",
	       daemon->node_id, peer->family, peer->port, reason);
}

static int on_frame(void *arg, const struct midr_transport_endpoint *peer,
			const struct midr_transport_frame *frame)
{
	struct midrd *daemon = arg;
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
		return 0;
	case MIDR_WIRE_SNAPSHOT_OBJECT:
		{
			struct midrd_snapshot_stage *stage = stage_for(daemon, peer,
								false);

			if (!stage || !stage->active || stage->count == stage->capacity)
				return -ENOSPC;
			if (midr_wire_decode_object(frame->payload, frame->payload_len,
						    &stage->objects[stage->count]))
				return -EINVAL;
			stage->count++;
			return 0;
		}
	case MIDR_WIRE_UPDATE:
	case MIDR_WIRE_WITHDRAW:
		{
		bool scope_changed;

		if (midr_wire_decode_object(frame->payload, frame->payload_len,
					    &object))
			return -EINVAL;
		if (frame->type == MIDR_WIRE_WITHDRAW)
			object.state = MIDR_CORE_WITHDRAWN;
		ret = apply_update(daemon, &object, mono_ms(), &result,
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

			if (!stage)
				return -ENOSPC;
			/* A repeated begin starts a fresh snapshot generation. */
			stage->count = 0;
		}
		printf("node=%" PRIu32 " sync type=%u\n", daemon->node_id,
		       frame->type);
		return 0;
	case MIDR_WIRE_SNAPSHOT_END:
		printf("node=%" PRIu32 " sync type=%u\n", daemon->node_id,
		       frame->type);
		return 0;
	case MIDR_WIRE_EOR:
		{
			struct midrd_snapshot_stage *stage = stage_for(daemon, peer,
								false);
			bool scope_changed = false;
			int ret;

			printf("node=%" PRIu32 " sync type=%u\n", daemon->node_id,
			       frame->type);
			if (!stage)
				return 0;
			ret = midr_engine_begin_batch(daemon->engine);
			if (!ret)
				for (size_t i = 0; i < stage->count; i++) {
					ret = midr_engine_apply(daemon->engine,
								&stage->objects[i], mono_ms(),
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
				ret = midr_engine_end_batch(daemon->engine, mono_ms());
			if (ret)
				(void)midr_engine_abort_batch(daemon->engine);
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
	struct midrd *daemon = arg;
	enum midr_core_result result;
	int ret;

	ret = midr_engine_apply(daemon->engine, object, mono_ms(), &result);
	if (ret)
		return ret;
	return result == MIDR_CORE_ACCEPTED ? 0 : -EAGAIN;
}

static void set_local_identity(struct midrd *daemon,
			       const struct midr_core_identity *identity)
{
	daemon->local_identity = *identity;
	daemon->have_local_identity = true;
}

static int local_provider_event(void *arg,
				const struct midr_prefix_event *event)
{
	struct midrd *daemon = arg;
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

static int prefix_event(void *arg, const struct midr_prefix_event *event)
{
	struct midrd *daemon = arg;
	struct midr_core_object object = {0};
	int ret;

	if (event->kind == MIDR_PREFIX_SNAPSHOT_BEGIN) {
		if (daemon->prefix_batch)
			return -EINVAL;
		ret = midr_engine_begin_batch(daemon->engine);
		if (!ret)
			daemon->prefix_batch = true;
		return ret;
	}
	if (event->kind == MIDR_PREFIX_EOR) {
		if (!daemon->prefix_batch)
			return 0;
		daemon->prefix_batch = false;
		ret = midr_engine_end_batch(daemon->engine, mono_ms());
		if (!ret) {
			drain_events(daemon, NULL);
			(void)reconcile_group_prefixes(daemon, mono_ms());
		}
		return ret;
	}
	if (event->kind == MIDR_PREFIX_SNAPSHOT_END)
		return 0;
	ret = midr_engine_apply_prefix_event(daemon->engine, event, mono_ms());
	if (ret)
		return ret;
	if (event->originator == daemon->node_id &&
	    event->kind == MIDR_PREFIX_UPSERT) {
		object.identity.type = MIDR_CORE_NODE_PREFIX;
		object.identity.family = event->prefix.family;
		object.identity.prefix_len = event->prefix.prefix_len;
		object.identity.originator = event->originator;
		memcpy(object.identity.prefix, event->prefix.address,
		       sizeof(object.identity.prefix));
		daemon->local_identity = object.identity;
		daemon->have_local_identity = true;
	} else if (event->originator == daemon->node_id &&
		   event->kind == MIDR_PREFIX_WITHDRAW) {
		daemon->have_local_identity = false;
	}
	if (!daemon->prefix_batch) {
		drain_events(daemon, NULL);
		(void)reconcile_group_prefixes(daemon, mono_ms());
	}
	return 0;
}

static int prefix_ipc_event(void *arg, const struct midr_prefix_event *event)
{
	return prefix_event(arg, event);
}

static void prefix_ipc_disconnect(void *arg, int reason)
{
	struct midrd *daemon = arg;

	(void)reason;
	if (daemon->prefix_batch) {
		(void)midr_engine_abort_batch(daemon->engine);
		daemon->prefix_batch = false;
	}
}

static int install_local_prefix(struct midrd *daemon, const char *text)
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

static int install_local_membership(struct midrd *daemon, uint32_t group_id)
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

static int install_local_link(struct midrd *daemon,
			      const struct midrd_link_config *link)
{
	struct midr_core_object object = {0};
	int ret;

	if (!daemon || !link || !link->remote || link->remote == daemon->node_id ||
	    !link->metric || link->metric == UINT32_MAX)
		return -EINVAL;
	object.identity.type = MIDR_CORE_LINK;
	object.identity.originator = daemon->node_id;
	object.identity.remote = link->remote;
	object.identity.link_id = link->link_id;
	object.state = MIDR_CORE_ACTIVE;
	object.metric = link->metric;
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

static int withdraw_group_prefixes(struct midrd *daemon)
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
	if (changed)
		drain_events(daemon, NULL);
	return result;
}

static int reconcile_group_prefixes(struct midrd *daemon, uint64_t now_ms)
{
	struct midr_core_object *objects = NULL;
	struct midr_core_identity *desired = NULL;
	uint32_t group = 0, representative = 0;
	size_t object_count = 0, desired_count = 0, index = 0;
	bool changed = false;
	int ret = 0, result = 0;

	if (!daemon || daemon->prefix_batch)
		return 0;
	if (midr_engine_membership(daemon->engine, daemon->node_id, &group) ||
	    midr_engine_representative(daemon->engine, group, &representative)) {
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
	ret = midr_engine_snapshot(daemon->engine, now_ms, objects,
				   MIDRD_MAX_SNAPSHOT, &object_count);
	if (ret)
		goto done;
	for (size_t i = 0; i < object_count; i++) {
		struct midr_core_identity identity = {0};
		uint32_t owner_group;

		if (objects[i].state != MIDR_CORE_ACTIVE ||
		    objects[i].identity.type != MIDR_CORE_NODE_PREFIX ||
		    midr_engine_membership(daemon->engine,
					   objects[i].identity.originator,
					   &owner_group) || owner_group != group)
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
	if (changed)
		drain_events(daemon, NULL);
	daemon->group_reconcile_pending = (ret ? ret : result) != 0;
	return ret ? ret : result;
}

static void refresh_owned(struct midrd *daemon)
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
	for (size_t i = 0; i < daemon->group_prefix_count; i++)
		(void)midr_owned_refresh(daemon->owned,
					 &daemon->group_prefixes[i]);
	drain_events(daemon, NULL);
}

static void periodic(struct midrd *daemon, uint64_t now)
{
	if (now >= daemon->next_hello) {
		for (size_t i = 0; i < daemon->peer_count; i++)
			(void)send_hello(daemon, &daemon->peers[i].endpoint);
		daemon->next_hello = now + daemon->hello_ms;
	}
	if (now >= daemon->next_keepalive) {
		for (size_t i = 0; i < daemon->peer_count; i++)
			(void)send_frame(daemon, &daemon->peers[i].endpoint,
					 MIDR_WIRE_KEEPALIVE, NULL, 0);
		daemon->next_keepalive = now + daemon->hello_ms;
	}
	if (now >= daemon->next_refresh) {
		refresh_owned(daemon);
		daemon->next_refresh = now + daemon->lifetime_ms / 3U;
	}
	if (now >= daemon->next_expire) {
		size_t expired = 0;

		if (midr_engine_expire(daemon->engine, now, &expired) == 0 && expired) {
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

static void usage(const char *program)
{
	fprintf(stderr,
		"usage: %s --node-id N --listen HOST:PORT [--peer HOST:PORT]... "
		"[--group ID] [--prefix ADDRESS/LEN] [--link NODE:COST]... "
		"[--prefix-socket PATH] "
		"[--sequence-file PATH] [--lifetime MS] [--runtime SEC] "
		"[--takeover-delay MS] [--pidfile PATH]\n",
		program);
}

int main(int argc, char **argv)
{
	struct midrd daemon = {
		.lifetime_ms = MIDRD_DEFAULT_LIFETIME,
		.hello_ms = MIDRD_DEFAULT_HELLO,
		.takeover_delay_ms = MIDRD_DEFAULT_TAKEOVER_DELAY,
	};
	struct midr_engine_config engine_config;
	struct midr_owned_config owned_config;
	struct midr_consumer_config consumer_config = {
		.on_event = on_consumer_event,
	};
	struct midr_transport_config transport_config = {0};
	struct midr_transport_callbacks transport_callbacks = {
		.on_frame = on_frame,
		.on_established = on_established,
		.on_closed = on_closed,
	};
	const char *listen_text = NULL, *prefix_text = NULL, *prefix_socket = NULL;
	const char *pidfile = NULL;
	FILE *pid_stream = NULL;
	int runtime_sec = 0;
	int opt;

	for (opt = 1; opt < argc; opt++) {
		if (!strcmp(argv[opt], "--node-id") && opt + 1 < argc)
			daemon.node_id = (uint32_t)strtoul(argv[++opt], NULL, 10);
		else if (!strcmp(argv[opt], "--listen") && opt + 1 < argc)
			listen_text = argv[++opt];
		else if (!strcmp(argv[opt], "--peer") && opt + 1 < argc) {
			if (daemon.peer_count == MIDRD_MAX_PEERS ||
			    parse_endpoint(argv[++opt],
					   &daemon.peers[daemon.peer_count].endpoint)) {
				usage(argv[0]);
				return 2;
			}
			daemon.peer_count++;
		} else if (!strcmp(argv[opt], "--prefix") && opt + 1 < argc)
			prefix_text = argv[++opt];
		else if (!strcmp(argv[opt], "--link") && opt + 1 < argc) {
			if (daemon.link_count == MIDRD_MAX_LINKS ||
			    parse_link(argv[++opt], &daemon.links[daemon.link_count])) {
				usage(argv[0]);
				return 2;
			}
			daemon.link_count++;
		}
		else if (!strcmp(argv[opt], "--group") && opt + 1 < argc)
			daemon.group_id = (uint32_t)strtoul(argv[++opt], NULL, 10);
		else if (!strcmp(argv[opt], "--prefix-socket") && opt + 1 < argc)
			prefix_socket = argv[++opt];
		else if (!strcmp(argv[opt], "--sequence-file") && opt + 1 < argc)
			daemon.sequence_file = argv[++opt];
		else if (!strcmp(argv[opt], "--lifetime") && opt + 1 < argc)
			daemon.lifetime_ms = (uint32_t)strtoul(argv[++opt], NULL, 10);
		else if (!strcmp(argv[opt], "--takeover-delay") && opt + 1 < argc)
			daemon.takeover_delay_ms =
				(uint32_t)strtoul(argv[++opt], NULL, 10);
		else if (!strcmp(argv[opt], "--runtime") && opt + 1 < argc)
			runtime_sec = atoi(argv[++opt]);
		else if (!strcmp(argv[opt], "--pidfile") && opt + 1 < argc)
			pidfile = argv[++opt];
		else {
			usage(argv[0]);
			return 2;
		}
	}
	if (!daemon.node_id || !listen_text || !daemon.lifetime_ms)
		return 2;
	if (parse_endpoint(listen_text, &transport_config.local)) {
		fprintf(stderr, "invalid listen endpoint\n");
		return 2;
	}
	transport_config.hello_interval_ms = daemon.hello_ms;
	transport_config.hold_time_ms = daemon.lifetime_ms;
	transport_config.max_frame_size = MIDRD_MAX_FRAME + MIDR_WIRE_HEADER_LEN;
	transport_callbacks.arg = &daemon;
	consumer_config.arg = &daemon;
	engine_config.node_id = daemon.node_id;
	engine_config.max_objects = MIDRD_MAX_SNAPSHOT;
	engine_config.lifetime_ms = daemon.lifetime_ms;
	owned_config.originator = daemon.node_id;
	owned_config.max_objects = MIDRD_MAX_SNAPSHOT;
	owned_config.lifetime_ms = daemon.lifetime_ms;
	owned_config.sequence_file = daemon.sequence_file;
	if (midr_engine_create(&engine_config, &daemon.engine) ||
	    midr_consumer_create(&consumer_config, &daemon.consumer) ||
	    midr_engine_attach_consumer(daemon.engine, daemon.consumer) ||
	    midr_owned_create(&owned_config, publish_owned, &daemon,
			       &daemon.owned) ||
	    midr_transport_create(&transport_config, &transport_callbacks,
				   &daemon.transport) ||
	    midr_transport_start(daemon.transport)) {
		fprintf(stderr, "midrd initialization failed\n");
		return 1;
	}
	if (prefix_socket) {
		struct midr_prefix_ipc_config ipc_config = {
			.path = prefix_socket,
			.on_event = prefix_ipc_event,
			.on_disconnect = prefix_ipc_disconnect,
			.arg = &daemon,
		};

		if (midr_prefix_ipc_server_create(&ipc_config, &daemon.prefix_ipc) ||
		    midr_prefix_ipc_server_start(daemon.prefix_ipc)) {
			fprintf(stderr, "prefix IPC initialization failed\n");
			return 1;
		}
	}
	for (size_t i = 0; i < daemon.peer_count; i++)
		if (midr_transport_connect(daemon.transport,
					    &daemon.peers[i].endpoint)) {
			fprintf(stderr, "peer connection setup failed\n");
			return 1;
		}
	if (prefix_text && install_local_prefix(&daemon, prefix_text)) {
		fprintf(stderr, "invalid prefix\n");
		return 2;
	}
	if (daemon.group_id && install_local_membership(&daemon, daemon.group_id)) {
		fprintf(stderr, "invalid group\n");
		return 2;
	}
	for (size_t i = 0; i < daemon.link_count; i++)
		if (install_local_link(&daemon, &daemon.links[i])) {
			fprintf(stderr, "invalid link\n");
			return 2;
		}
	(void)reconcile_group_prefixes(&daemon, mono_ms());
	if (signal(SIGINT, on_signal) == SIG_ERR || signal(SIGTERM, on_signal) == SIG_ERR)
		return 1;
	if (pidfile) {
		pid_stream = fopen(pidfile, "w");
		if (!pid_stream || fprintf(pid_stream, "%ld\n", (long)getpid()) < 0) {
			if (pid_stream)
				(void)fclose(pid_stream);
			(void)unlink(pidfile);
			return 1;
		}
		if (fclose(pid_stream) != 0) {
			(void)unlink(pidfile);
			return 1;
		}
		pid_stream = NULL;
	}
	{
		uint64_t now = mono_ms();

		daemon.next_hello = now + daemon.hello_ms;
		daemon.next_keepalive = now + daemon.hello_ms / 2U;
		daemon.next_refresh = now + daemon.lifetime_ms / 3U;
		daemon.next_expire = now + 100U;
		daemon.stop_at = runtime_sec > 0 ?
			now + (uint64_t)runtime_sec * 1000U : 0;
	}
	printf("midrd node=%" PRIu32 " family=%u listen-port=%u\n",
	       daemon.node_id, transport_config.local.family,
	       transport_config.local.port);
	drain_events(&daemon, NULL);
	for (;;) {
		uint64_t now = mono_ms();

		if (stop_requested || (daemon.stop_at && now >= daemon.stop_at))
			break;
		(void)midr_prefix_ipc_server_poll(daemon.prefix_ipc, 0);
		(void)midr_transport_poll(daemon.transport, 100);
		periodic(&daemon, mono_ms());
	}
	if (daemon.owned) {
		size_t withdrawn = 0;

		if (!midr_owned_withdraw_all(daemon.owned, &withdrawn) && withdrawn)
			drain_events(&daemon, NULL);
	}
	printf("midrd node=%" PRIu32 " final-objects=%zu\n", daemon.node_id,
	       midr_engine_count(daemon.engine));
	for (size_t i = 0; i < MIDRD_MAX_PEERS; i++)
		stage_release(&daemon.stages[i]);
	midr_prefix_ipc_server_destroy(&daemon.prefix_ipc);
	midr_transport_destroy(&daemon.transport);
	midr_prefix_provider_destroy(&daemon.prefix_provider);
	midr_owned_destroy(&daemon.owned);
	midr_consumer_destroy(&daemon.consumer);
	midr_engine_destroy(&daemon.engine);
	if (pidfile)
		(void)unlink(pidfile);
	return 0;
}
