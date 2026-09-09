// SPDX-License-Identifier: GPL-2.0-or-later

#include <zebra.h>

#include <inttypes.h>
#include <sys/time.h>

#include "bgpd/bgp_midr_pm.h"
#include "bgpd/bgp_midr_pm_net.h"

_Static_assert(sizeof(struct midr_probe_pkt) == 20,
	       "MIDR PM wire format must remain 20 bytes");

static uint64_t now_us(void)
{
	struct timespec timestamp;

	clock_gettime(CLOCK_MONOTONIC, &timestamp);
	return (uint64_t)timestamp.tv_sec * 1000000ULL
	       + (uint64_t)timestamp.tv_nsec / 1000ULL;
}

static bool parse_locator(const char *text, struct ipaddr *locator)
{
	return str2ipaddr(text, locator) == 0;
}

static int open_endpoint(const struct ipaddr *local)
{
	union sockunion address = {};
	struct timeval timeout = {.tv_sec = 5};
	int family;
	int reuse = 1;
	int sock;

	if (!midr_pm_net_transport_to_sockunion(local, MIDR_PM_PROBE_PORT,
						&address))
		return -1;
	family = ipaddr_family(local);
	sock = socket(family, SOCK_DGRAM, 0);
	if (sock < 0)
		return -1;
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse))
	    < 0)
		goto fail;
	if (midr_pm_net_enable_v6only(family, sock) < 0)
		goto fail;
	if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout,
		       sizeof(timeout)) < 0)
		goto fail;
	if (bind(sock, &address.sa, midr_pm_net_sockaddr_size(&address)) < 0)
		goto fail;
	if (!midr_pm_net_socket_matches(sock, local, MIDR_PM_PROBE_PORT))
		goto fail;

	if (family == AF_INET6) {
		socklen_t option_length;
		int enabled = 0;

		option_length = sizeof(enabled);
		if (getsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &enabled,
			       &option_length) < 0
		    || enabled != 1)
			goto fail;
	}

	printf("PASS exact-bind\n");
	printf("PASS %s\n",
	       family == AF_INET6 ? "ipv6-v6only" : "ipv4-native");
	return sock;

fail:
	close(sock);
	return -1;
}

static void build_request(struct midr_probe_pkt *packet, uint32_t seqno)
{
	memset(packet, 0, sizeof(*packet));
	packet->magic = htonl(MIDR_PM_PROBE_MAGIC);
	packet->type = MIDR_PM_PROBE_REQ;
	packet->seqno = htonl(seqno);
	packet->sent_us = htobe64(now_us());
}

static int run_server(const char *local_text, const char *known_text)
{
	struct ipaddr local;
	struct ipaddr known;
	bool accepted = false;
	bool rejected = false;
	int sock;
	int attempt;

	if (!parse_locator(local_text, &local)
	    || !parse_locator(known_text, &known))
		return EXIT_FAILURE;
	sock = open_endpoint(&local);
	if (sock < 0)
		return EXIT_FAILURE;

	for (attempt = 0; attempt < 4 && (!accepted || !rejected); attempt++) {
		union sockunion source = {};
		struct midr_probe_pkt packet;
		struct ipaddr source_addr;
		socklen_t source_length = sizeof(source);
		ssize_t received;

		received = recvfrom(sock, &packet, sizeof(packet), MSG_TRUNC,
				    &source.sa, &source_length);
		if (received < 0)
			break;
		if (!midr_sockunion_to_ipaddr(&source, &source_addr)
		    || !midr_pm_net_source_valid(
			       &local, &source_addr,
			       midr_pm_net_get_port(&source),
			       MIDR_PM_PROBE_PORT)
		    || !midr_ipaddr_same(&source_addr, &known)) {
			printf("PASS unknown-source-rejected\n");
			rejected = true;
			continue;
		}
		if ((size_t)received != sizeof(packet)
		    || ntohl(packet.magic) != MIDR_PM_PROBE_MAGIC
		    || packet.type != MIDR_PM_PROBE_REQ)
			continue;

		packet.type = MIDR_PM_PROBE_REP;
		if (sendto(sock, &packet, sizeof(packet), 0, &source.sa,
			   source_length)
		    != sizeof(packet))
			break;
		printf("PASS request-echoed\n");
		accepted = true;
	}

	close(sock);
	return accepted && rejected ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int send_request(const char *local_text, const char *remote_text,
			bool wait_for_reply)
{
	struct midr_probe_pkt request;
	struct ipaddr local;
	struct ipaddr remote;
	union sockunion destination = {};
	int sock;

	if (!parse_locator(local_text, &local)
	    || !parse_locator(remote_text, &remote)
	    || !midr_pm_net_transport_to_sockunion(
		    &remote, MIDR_PM_PROBE_PORT, &destination))
		return EXIT_FAILURE;
	sock = open_endpoint(&local);
	if (sock < 0)
		return EXIT_FAILURE;

	build_request(&request, 42);
	if (sendto(sock, &request, sizeof(request), 0, &destination.sa,
		   midr_pm_net_sockaddr_size(&destination))
	    != sizeof(request)) {
		close(sock);
		return EXIT_FAILURE;
	}

	if (wait_for_reply) {
		union sockunion source = {};
		struct midr_probe_pkt reply;
		struct ipaddr source_addr;
		socklen_t source_length = sizeof(source);
		ssize_t received;
		uint64_t rtt;

		received = recvfrom(sock, &reply, sizeof(reply), MSG_TRUNC,
				    &source.sa, &source_length);
		if ((size_t)received != sizeof(reply)
		    || !midr_sockunion_to_ipaddr(&source, &source_addr)
		    || !midr_pm_net_source_valid(
			       &local, &source_addr,
			       midr_pm_net_get_port(&source),
			       MIDR_PM_PROBE_PORT)
		    || !midr_ipaddr_same(&source_addr, &remote)
		    || ntohl(reply.magic) != MIDR_PM_PROBE_MAGIC
		    || reply.type != MIDR_PM_PROBE_REP
		    || reply.seqno != request.seqno
		    || reply.sent_us != request.sent_us) {
			close(sock);
			return EXIT_FAILURE;
		}
		rtt = now_us() - be64toh(reply.sent_us);
		printf("PASS request-reply RTT_US=%" PRIu64 "\n", rtt);
	} else {
		printf("PASS unknown-source-sent\n");
	}

	close(sock);
	return EXIT_SUCCESS;
}

static bool classification_case(const struct ipaddr *local,
				const char *source_text, uint16_t port,
				bool expected)
{
	struct ipaddr source;

	return parse_locator(source_text, &source)
	       && midr_pm_net_source_valid(local, &source, port,
					   MIDR_PM_PROBE_PORT)
			  == expected;
}

static int run_classification(const char *family)
{
	struct ipaddr local;
	const char *valid_source;
	const char *multicast_source;
	const char *unspecified_source;
	const char *cross_family_source;

	if (strcmp(family, "ipv4") == 0) {
		if (!parse_locator("10.99.0.1", &local))
			return EXIT_FAILURE;
		valid_source = "10.99.0.2";
		multicast_source = "239.1.1.1";
		unspecified_source = "0.0.0.0";
		cross_family_source = "fd00:99::2";
	} else if (strcmp(family, "ipv6") == 0) {
		if (!parse_locator("fd00:99::a", &local))
			return EXIT_FAILURE;
		valid_source = "fd00:99::b";
		multicast_source = "ff02::1";
		unspecified_source = "::";
		cross_family_source = "192.0.2.1";
	} else {
		return EXIT_FAILURE;
	}

	if (!classification_case(&local, valid_source, MIDR_PM_PROBE_PORT, true)
	    || !classification_case(&local, "::ffff:192.0.2.1",
				    MIDR_PM_PROBE_PORT, false)
	    || !classification_case(&local, "fe80::1", MIDR_PM_PROBE_PORT,
				    false)
	    || !classification_case(&local, multicast_source,
				    MIDR_PM_PROBE_PORT, false)
	    || !classification_case(&local, cross_family_source,
				    MIDR_PM_PROBE_PORT, false)
	    || !classification_case(&local, unspecified_source,
				    MIDR_PM_PROBE_PORT, false)
	    || !classification_case(&local, valid_source,
				    MIDR_PM_PROBE_PORT + 1, false))
		return EXIT_FAILURE;

	printf("PASS valid-source\n");
	printf("PASS mapped-source-rejected\n");
	printf("PASS link-local-source-rejected\n");
	printf("PASS multicast-source-rejected\n");
	printf("PASS cross-family-source-rejected\n");
	printf("PASS unspecified-source-rejected\n");
	printf("PASS wrong-port-rejected\n");
	return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "classify") == 0)
		return run_classification(argv[2]);
	if (argc == 4 && strcmp(argv[1], "server") == 0)
		return run_server(argv[2], argv[3]);
	if (argc == 4 && strcmp(argv[1], "client") == 0)
		return send_request(argv[2], argv[3], true);
	if (argc == 4 && strcmp(argv[1], "unknown") == 0)
		return send_request(argv[2], argv[3], false);

	fprintf(stderr,
		"Usage: %s classify ipv4|ipv6 | server LOCAL KNOWN | client LOCAL REMOTE | unknown LOCAL REMOTE\n",
		argv[0]);
	return EXIT_FAILURE;
}
