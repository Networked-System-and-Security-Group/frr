/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
/*
 * R7-MVP standalone MIDR daemon.
 *
 * This intentionally small runner uses native UDP (one address family per
 * process), the protocol-neutral core, and a local static Prefix provider.
 * It is a development executable for the IPv4/IPv6 containerlab smoke; BGP,
 * TCP/179 and FRR headers are not dependencies.
 */
#include "midr-core.h"
#include "midr-prefix-provider.h"
#include "midr-transport.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MIDRD_MAGIC 0x4d494452U
#define MIDRD_VERSION 1U
#define MIDRD_MAX_PEERS 32U
#define MIDRD_MAX_SNAPSHOT 4096U
#define MIDRD_MAX_FRAME 2048U
#define MIDRD_DEFAULT_LIFETIME 6000U
#define MIDRD_DEFAULT_HELLO 1000U

struct midrd_peer {
	struct midr_transport_endpoint endpoint;
	struct sockaddr_storage sockaddr;
	socklen_t sockaddr_len;
	bool active;
};

struct midrd {
	int fd;
	uint32_t node_id;
	uint32_t lifetime_ms;
	uint32_t hello_ms;
	uint64_t frame_sequence;
	struct midr_core *core;
	struct midr_core_identity local_identity;
	bool have_local_identity;
	struct midrd_peer peers[MIDRD_MAX_PEERS];
	size_t peer_count;
	uint64_t next_hello;
	uint64_t next_keepalive;
	uint64_t next_refresh;
	uint64_t next_expire;
	uint64_t stop_at;
	struct midr_prefix_provider *prefix_provider;
};

static uint64_t mono_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static uint64_t htonll_u64(uint64_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return ((uint64_t)htonl((uint32_t)value) << 32) |
	       htonl((uint32_t)(value >> 32));
#else
	return value;
#endif
}

static uint64_t ntohll_u64(uint64_t value)
{
	return htonll_u64(value);
}

static void put_u16(uint8_t *p, uint16_t value)
{
	uint16_t n = htons(value);
	memcpy(p, &n, sizeof(n));
}

static void put_u32(uint8_t *p, uint32_t value)
{
	uint32_t n = htonl(value);
	memcpy(p, &n, sizeof(n));
}

static void put_u64(uint8_t *p, uint64_t value)
{
	uint64_t n = htonll_u64(value);
	memcpy(p, &n, sizeof(n));
}

static uint32_t get_u32(const uint8_t *p)
{
	uint32_t n;
	memcpy(&n, p, sizeof(n));
	return ntohl(n);
}

static uint64_t get_u64(const uint8_t *p)
{
	uint64_t n;
	memcpy(&n, p, sizeof(n));
	return ntohll_u64(n);
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
		snprintf(service, service_len, "%s", end + 2);
		return service[0] ? 0 : -EINVAL;
	}
	colon = strrchr(text, ':');
	if (!colon || strchr(text, ':') != colon)
		return -EINVAL;
	len = (size_t)(colon - text);
	if (!len || len >= host_len)
		return -EINVAL;
	memcpy(host, text, len);
	host[len] = '\0';
	snprintf(service, service_len, "%s", colon + 1);
	return service[0] ? 0 : -EINVAL;
}

static int parse_endpoint(const char *text, struct midr_transport_endpoint *endpoint,
			  struct sockaddr_storage *sockaddr, socklen_t *sockaddr_len,
			  bool passive)
{
	char host[128];
	char service[32];
	struct addrinfo hints = {0}, *result = NULL;
	int ret;

	if (split_endpoint(text, host, sizeof(host), service, sizeof(service)))
		return -EINVAL;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = passive ? AI_PASSIVE : 0;
	ret = getaddrinfo(strcmp(host, "*") == 0 ? NULL : host, service,
			  &hints, &result);
	if (ret || !result)
		return -EINVAL;
	if (result->ai_family != AF_INET && result->ai_family != AF_INET6) {
		freeaddrinfo(result);
		return -EAFNOSUPPORT;
	}
	memset(endpoint, 0, sizeof(*endpoint));
	endpoint->family = result->ai_family == AF_INET ? MIDR_CORE_AF_IPV4
						       : MIDR_CORE_AF_IPV6;
	endpoint->port = ntohs(result->ai_family == AF_INET
				       ? ((struct sockaddr_in *)result->ai_addr)->sin_port
				       : ((struct sockaddr_in6 *)result->ai_addr)
						 ->sin6_port);
	if (result->ai_family == AF_INET)
		memcpy(endpoint->address,
		       &((struct sockaddr_in *)result->ai_addr)->sin_addr, 4);
	else {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)result->ai_addr;
		memcpy(endpoint->address, &sin6->sin6_addr, 16);
		endpoint->scope_id = sin6->sin6_scope_id;
	}
	memcpy(sockaddr, result->ai_addr, result->ai_addrlen);
	*sockaddr_len = (socklen_t)result->ai_addrlen;
	freeaddrinfo(result);
	return 0;
}

static bool endpoint_equal(const struct midr_transport_endpoint *a,
			   const struct midr_transport_endpoint *b)
{
	return a->family == b->family && a->port == b->port &&
	       a->scope_id == b->scope_id &&
	       !memcmp(a->address, b->address, sizeof(a->address));
}

static struct midrd_peer *find_peer(struct midrd *daemon,
				     const struct midr_transport_endpoint *endpoint)
{
	for (size_t i = 0; i < daemon->peer_count; i++)
		if (daemon->peers[i].active &&
		    endpoint_equal(&daemon->peers[i].endpoint, endpoint))
			return &daemon->peers[i];
	return NULL;
}

static struct midrd_peer *add_peer(struct midrd *daemon,
				   const struct midr_transport_endpoint *endpoint,
				   const struct sockaddr_storage *sockaddr,
				   socklen_t sockaddr_len)
{
	struct midrd_peer *peer = find_peer(daemon, endpoint);

	if (peer)
		return peer;
	if (daemon->peer_count == MIDRD_MAX_PEERS)
		return NULL;
	peer = &daemon->peers[daemon->peer_count++];
	memset(peer, 0, sizeof(*peer));
	peer->endpoint = *endpoint;
	peer->sockaddr = *sockaddr;
	peer->sockaddr_len = sockaddr_len;
	peer->active = true;
	return peer;
}

static int encode_object(const struct midr_core_object *object,
			 uint8_t *payload, size_t capacity, size_t *length)
{
	const struct midr_core_identity *id;

	if (!object || !payload || !length || capacity < 92U)
		return -EINVAL;
	id = &object->identity;
	payload[0] = id->type;
	payload[1] = id->family;
	payload[2] = id->prefix_len;
	payload[3] = 0;
	put_u32(payload + 4, id->originator);
	put_u32(payload + 8, id->remote);
	put_u32(payload + 12, id->group);
	put_u64(payload + 16, id->link_id);
	memcpy(payload + 24, id->prefix, 16);
	payload[40] = object->state;
	payload[41] = payload[42] = payload[43] = 0;
	put_u64(payload + 44, object->sequence);
	put_u32(payload + 52, object->lifetime_ms);
	put_u32(payload + 56, object->metric);
	memcpy(payload + 60, object->local_address, 16);
	memcpy(payload + 76, object->remote_address, 16);
	*length = 92U;
	return 0;
}

static int decode_object(const uint8_t *payload, size_t length,
			 struct midr_core_object *object)
{
	if (!payload || !object || length != 92U)
		return -EINVAL;
	memset(object, 0, sizeof(*object));
	object->identity.type = payload[0];
	object->identity.family = payload[1];
	object->identity.prefix_len = payload[2];
	object->identity.originator = get_u32(payload + 4);
	object->identity.remote = get_u32(payload + 8);
	object->identity.group = get_u32(payload + 12);
	object->identity.link_id = get_u64(payload + 16);
	memcpy(object->identity.prefix, payload + 24, 16);
	object->state = payload[40];
	object->sequence = get_u64(payload + 44);
	object->lifetime_ms = get_u32(payload + 52);
	object->metric = get_u32(payload + 56);
	memcpy(object->local_address, payload + 60, 16);
	memcpy(object->remote_address, payload + 76, 16);
	return 0;
}

static int send_frame(struct midrd *daemon, struct midrd_peer *peer,
		      uint8_t type, const uint8_t *payload, size_t payload_len)
{
	uint8_t frame[20U + MIDRD_MAX_FRAME];
	ssize_t sent;

	if (!daemon || !peer || payload_len > MIDRD_MAX_FRAME)
		return -EINVAL;
	put_u32(frame, MIDRD_MAGIC);
	frame[4] = MIDRD_VERSION;
	frame[5] = type;
	put_u16(frame + 6, 0);
	put_u64(frame + 8, ++daemon->frame_sequence);
	put_u32(frame + 16, (uint32_t)payload_len);
	if (payload_len)
		memcpy(frame + 20, payload, payload_len);
	sent = sendto(daemon->fd, frame, 20U + payload_len, 0,
		      (struct sockaddr *)&peer->sockaddr, peer->sockaddr_len);
	return sent == (ssize_t)(20U + payload_len) ? 0 : -errno;
}

static int send_hello(struct midrd *daemon, struct midrd_peer *peer)
{
	uint8_t payload[28] = {0};

	put_u32(payload, daemon->node_id);
	put_u32(payload + 4, daemon->lifetime_ms);
	payload[8] = peer->endpoint.family;
	put_u16(payload + 10, peer->endpoint.port);
	memcpy(payload + 12, peer->endpoint.address, 16);
	return send_frame(daemon, peer, MIDR_FRAME_HELLO, payload, sizeof(payload));
}

static int send_object(struct midrd *daemon, struct midrd_peer *peer,
		       uint8_t frame_type, const struct midr_core_object *object)
{
	uint8_t payload[92];
	size_t length;

	if (encode_object(object, payload, sizeof(payload), &length))
		return -EINVAL;
	return send_frame(daemon, peer, frame_type, payload, length);
}

static void flood_object(struct midrd *daemon, const struct midr_core_object *object,
			 const struct midr_transport_endpoint *except)
{
	uint8_t frame_type = object->state == MIDR_CORE_WITHDRAWN
				     ? MIDR_FRAME_WITHDRAW : MIDR_FRAME_UPDATE;

	for (size_t i = 0; i < daemon->peer_count; i++) {
		struct midrd_peer *peer = &daemon->peers[i];

		if (!peer->active ||
		    (except && endpoint_equal(&peer->endpoint, except)))
			continue;
		(void)send_object(daemon, peer, frame_type, object);
	}
}

static void drain_events(struct midrd *daemon,
			 const struct midr_transport_endpoint *except)
{
	struct midr_core_object object;

	while (midr_core_event_next(daemon->core, &object) == 0) {
		flood_object(daemon, &object, except);
		printf("node=%" PRIu32 " event state=%u seq=%" PRIu64 "\n",
		       daemon->node_id, object.state, object.sequence);
	}
}

static void send_snapshot(struct midrd *daemon, struct midrd_peer *peer)
{
	struct midr_core_object objects[MIDRD_MAX_SNAPSHOT];
	size_t count = 0;

	(void)send_frame(daemon, peer, MIDR_FRAME_SNAPSHOT_BEGIN, NULL, 0);
	if (midr_core_snapshot(daemon->core, mono_ms(), objects,
			       MIDRD_MAX_SNAPSHOT, &count) == 0)
		for (size_t i = 0; i < count; i++)
			(void)send_object(daemon, peer, MIDR_FRAME_SNAPSHOT_OBJECT,
					  &objects[i]);
	(void)send_frame(daemon, peer, MIDR_FRAME_SNAPSHOT_END, NULL, 0);
	(void)send_frame(daemon, peer, MIDR_FRAME_EOR, NULL, 0);
}

static void receive_packet(struct midrd *daemon)
{
	uint8_t frame[20U + MIDRD_MAX_FRAME];
	struct sockaddr_storage source = {0};
	socklen_t source_len = sizeof(source);
	struct midr_transport_endpoint endpoint = {0};
	ssize_t length;
	char host[128], service[32];
	struct midrd_peer *peer;
	uint32_t payload_len;
	uint8_t type;
	struct midr_core_object object;
	enum midr_core_result result;

	length = recvfrom(daemon->fd, frame, sizeof(frame), 0,
			  (struct sockaddr *)&source, &source_len);
	if (length < 20)
		return;
	if (get_u32(frame) != MIDRD_MAGIC || frame[4] != MIDRD_VERSION)
		return;
	type = frame[5];
	payload_len = get_u32(frame + 16);
	if (payload_len > MIDRD_MAX_FRAME ||
	    length != (ssize_t)(20U + payload_len))
		return;
	if (source.ss_family == AF_INET) {
		endpoint.family = MIDR_CORE_AF_IPV4;
		endpoint.port = ntohs(((struct sockaddr_in *)&source)->sin_port);
		memcpy(endpoint.address, &((struct sockaddr_in *)&source)->sin_addr, 4);
	} else if (source.ss_family == AF_INET6) {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&source;
		endpoint.family = MIDR_CORE_AF_IPV6;
		endpoint.port = ntohs(sin6->sin6_port);
		endpoint.scope_id = sin6->sin6_scope_id;
		memcpy(endpoint.address, &sin6->sin6_addr, 16);
	} else
		return;
	peer = add_peer(daemon, &endpoint, &source, source_len);
	if (!peer)
		return;
	if (getnameinfo((struct sockaddr *)&source, source_len, host, sizeof(host),
			service, sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV) == 0)
		printf("node=%" PRIu32 " rx=%s:%s type=%u\n",
		       daemon->node_id, host, service, type);

	switch (type) {
	case MIDR_FRAME_HELLO:
		if (payload_len != 28U)
			return;
		send_hello(daemon, peer);
		send_snapshot(daemon, peer);
		break;
	case MIDR_FRAME_KEEPALIVE:
		break;
	case MIDR_FRAME_SNAPSHOT_OBJECT:
	case MIDR_FRAME_UPDATE:
	case MIDR_FRAME_WITHDRAW:
		if (decode_object(frame + 20, payload_len, &object))
			return;
		if (type == MIDR_FRAME_WITHDRAW)
			object.state = MIDR_CORE_WITHDRAWN;
		if (midr_core_upsert(daemon->core, &object, mono_ms(), &result))
			return;
		if (result == MIDR_CORE_ACCEPTED)
			drain_events(daemon, &endpoint);
		break;
	case MIDR_FRAME_SNAPSHOT_BEGIN:
	case MIDR_FRAME_SNAPSHOT_END:
	case MIDR_FRAME_EOR:
		printf("node=%" PRIu32 " sync type=%u\n", daemon->node_id, type);
		break;
	default:
		break;
	}
}

static void periodic(struct midrd *daemon, uint64_t now)
{
	if (now >= daemon->next_hello) {
		for (size_t i = 0; i < daemon->peer_count; i++)
			if (daemon->peers[i].active)
				(void)send_hello(daemon, &daemon->peers[i]);
		daemon->next_hello = now + daemon->hello_ms;
	}
	if (now >= daemon->next_keepalive) {
		for (size_t i = 0; i < daemon->peer_count; i++)
			if (daemon->peers[i].active)
				(void)send_frame(daemon, &daemon->peers[i],
						 MIDR_FRAME_KEEPALIVE, NULL, 0);
		daemon->next_keepalive = now + daemon->hello_ms;
	}
	if (daemon->have_local_identity && now >= daemon->next_refresh) {
		if (midr_core_refresh(daemon->core, &daemon->local_identity, now) == 0)
			drain_events(daemon, NULL);
		daemon->next_refresh = now + daemon->lifetime_ms / 3U;
	}
	if (now >= daemon->next_expire) {
		(void)midr_core_expire(daemon->core, now, NULL);
		drain_events(daemon, NULL);
		daemon->next_expire = now + 100U;
	}
}

static int prefix_event(void *arg, const struct midr_prefix_event *event)
{
	struct midrd *daemon = arg;
	struct midr_core_object object = {0};
	enum midr_core_result result;
	int ret;

	if (!daemon || !event)
		return -EINVAL;
	if (event->kind == MIDR_PREFIX_SNAPSHOT_BEGIN ||
	    event->kind == MIDR_PREFIX_SNAPSHOT_END)
		return 0;
	object.identity.type = MIDR_CORE_NODE_PREFIX;
	object.identity.family = event->prefix.family;
	object.identity.prefix_len = event->prefix.prefix_len;
	object.identity.originator = event->originator;
	memcpy(object.identity.prefix, event->prefix.address,
	       sizeof(object.identity.prefix));
	object.state = event->kind == MIDR_PREFIX_WITHDRAW
			      ? MIDR_CORE_WITHDRAWN : MIDR_CORE_ACTIVE;
	object.sequence = event->generation;
	object.lifetime_ms = daemon->lifetime_ms;
	object.metric = event->prefix.metric;
	ret = midr_core_upsert(daemon->core, &object, mono_ms(), &result);
	if (ret)
		return ret;
	if (result == MIDR_CORE_ACCEPTED &&
	    event->kind == MIDR_PREFIX_UPSERT) {
		daemon->local_identity = object.identity;
		daemon->have_local_identity = true;
	} else if (event->kind == MIDR_PREFIX_WITHDRAW &&
		   midr_core_identity_equal(&daemon->local_identity,
					    &object.identity)) {
		daemon->have_local_identity = false;
	}
	return 0;
}

static int install_local_prefix(struct midrd *daemon, const char *prefix_text)
{
	char copy[128];
	char *slash;
	struct midr_prefix prefix = {0};
	struct midr_prefix_provider_config provider_config = {0};
	struct in_addr addr4;
	struct in6_addr addr6;
	unsigned long plen;
	char *end;

	if (!prefix_text || strlen(prefix_text) >= sizeof(copy))
		return -EINVAL;
	strcpy(copy, prefix_text);
	slash = strchr(copy, '/');
	if (!slash)
		return -EINVAL;
	*slash++ = '\0';
	plen = strtoul(slash, &end, 10);
	if (*end || plen > 128)
		return -EINVAL;
	prefix.metric = 10;
	if (inet_pton(AF_INET, copy, &addr4) == 1) {
		if (plen > 32)
			return -EINVAL;
		prefix.family = MIDR_CORE_AF_IPV4;
		prefix.prefix_len = (uint8_t)plen;
		memcpy(prefix.address, &addr4, 4);
	} else if (inet_pton(AF_INET6, copy, &addr6) == 1) {
		prefix.family = MIDR_CORE_AF_IPV6;
		prefix.prefix_len = (uint8_t)plen;
		memcpy(prefix.address, &addr6, 16);
	} else
		return -EINVAL;
	provider_config.originator = daemon->node_id;
	provider_config.on_event = prefix_event;
	provider_config.arg = daemon;
	if (midr_prefix_provider_create(&provider_config,
					&daemon->prefix_provider))
		return -ENOMEM;
	if (midr_prefix_provider_upsert(daemon->prefix_provider, &prefix)) {
		midr_prefix_provider_destroy(&daemon->prefix_provider);
		return -EINVAL;
	}
	return 0;
}

static void usage(const char *program)
{
	fprintf(stderr,
		"usage: %s --node-id N --listen HOST:PORT [--peer HOST:PORT]... "
		"[--prefix ADDRESS/LEN] [--lifetime MS] [--runtime SEC]\n",
		program);
}

int main(int argc, char **argv)
{
	struct midrd daemon = {
		.fd = -1,
		.lifetime_ms = MIDRD_DEFAULT_LIFETIME,
		.hello_ms = MIDRD_DEFAULT_HELLO,
	};
	struct midr_core_config core_config;
	struct midr_transport_endpoint listen_endpoint = {0};
	const char *listen_text = NULL;
	const char *prefix_text = NULL;
	int runtime_sec = 0;
	struct sockaddr_storage listen_sockaddr = {0};
	socklen_t listen_sockaddr_len = 0;
	int opt;

	for (opt = 1; opt < argc; opt++) {
		if (!strcmp(argv[opt], "--node-id") && opt + 1 < argc)
			daemon.node_id = (uint32_t)strtoul(argv[++opt], NULL, 10);
		else if (!strcmp(argv[opt], "--listen") && opt + 1 < argc)
			listen_text = argv[++opt];
		else if (!strcmp(argv[opt], "--peer") && opt + 1 < argc) {
			if (daemon.peer_count == MIDRD_MAX_PEERS ||
			    parse_endpoint(argv[++opt],
					   &daemon.peers[daemon.peer_count].endpoint,
					   &daemon.peers[daemon.peer_count].sockaddr,
					   &daemon.peers[daemon.peer_count].sockaddr_len,
					   false)) {
				usage(argv[0]);
				return 2;
			}
			daemon.peers[daemon.peer_count++].active = true;
		} else if (!strcmp(argv[opt], "--prefix") && opt + 1 < argc)
			prefix_text = argv[++opt];
		else if (!strcmp(argv[opt], "--lifetime") && opt + 1 < argc)
			daemon.lifetime_ms = (uint32_t)strtoul(argv[++opt], NULL, 10);
		else if (!strcmp(argv[opt], "--runtime") && opt + 1 < argc)
			runtime_sec = atoi(argv[++opt]);
		else {
			usage(argv[0]);
			return 2;
		}
	}
	if (!daemon.node_id || !listen_text) {
		usage(argv[0]);
		return 2;
	}
	if (parse_endpoint(listen_text, &listen_endpoint, &listen_sockaddr,
			   &listen_sockaddr_len, true)) {
		fprintf(stderr, "invalid listen endpoint\n");
		return 2;
	}
	daemon.fd = socket(listen_sockaddr.ss_family, SOCK_DGRAM, 0);
	if (daemon.fd < 0 || bind(daemon.fd, (struct sockaddr *)&listen_sockaddr,
				  listen_sockaddr_len) < 0) {
		perror("midrd socket/bind");
		return 1;
	}
	core_config.max_objects = 4096;
	core_config.lifetime_ms = daemon.lifetime_ms;
	if (midr_core_create(&core_config, &daemon.core)) {
		fprintf(stderr, "midrd core create failed\n");
		close(daemon.fd);
		return 1;
	}
	if (prefix_text && install_local_prefix(&daemon, prefix_text)) {
		fprintf(stderr, "invalid prefix\n");
		midr_core_destroy(&daemon.core);
		close(daemon.fd);
		return 2;
	}
	for (size_t i = 0; i < daemon.peer_count; i++)
		(void)send_hello(&daemon, &daemon.peers[i]);
	{
		uint64_t now = mono_ms();
		daemon.next_hello = now + daemon.hello_ms;
		daemon.next_keepalive = now + daemon.hello_ms / 2U;
		daemon.next_refresh = now + daemon.lifetime_ms / 3U;
		daemon.next_expire = now + 100U;
		daemon.stop_at = runtime_sec > 0 ? now + (uint64_t)runtime_sec * 1000U : 0;
	}
	printf("midrd node=%" PRIu32 " family=%u listen-port=%u\n",
	       daemon.node_id, listen_endpoint.family, listen_endpoint.port);
	drain_events(&daemon, NULL);

	for (;;) {
		fd_set readfds;
		struct timeval timeout = {.tv_sec = 0, .tv_usec = 100000};
		uint64_t now = mono_ms();

		if (daemon.stop_at && now >= daemon.stop_at)
			break;
		FD_ZERO(&readfds);
		FD_SET(daemon.fd, &readfds);
		if (select(daemon.fd + 1, &readfds, NULL, NULL, &timeout) > 0 &&
		    FD_ISSET(daemon.fd, &readfds))
			receive_packet(&daemon);
		periodic(&daemon, mono_ms());
	}
	printf("midrd node=%" PRIu32 " final-objects=%zu\n",
	       daemon.node_id, midr_core_count(daemon.core));
	midr_prefix_provider_destroy(&daemon.prefix_provider);
	midr_core_destroy(&daemon.core);
	close(daemon.fd);
	return 0;
}
