/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * group 3 route-installation harness (dp-e2e-tool)
 *
 * Drives the public third-group facade (midr_zebra_route_add/del/flush) against
 * a REAL zebra over ZAPI, so the encoding, the installed-hash bookkeeping, the
 * dual-instance metric strategy and the Linux FIB can be verified without the
 * group-2 LS-flooding transport being up.
 *
 * Modes:
 *   add4 PREFIX NH [METRIC]        add6 PREFIX NH [METRIC]
 *   add4-ecmp PREFIX NH1 NH2       add4-ucmp PREFIX NH1:W1 NH2:W2
 *   add6-srv6 PREFIX NH SID1[,SID2...]
 *   add4-te PREFIX NH              del PREFIX [SPF|TE]
 *   status
 *   serve-ted SEC                  apply a synthetic committed TED, stay alive
 *
 * serve-ted exercises the full midrd chain (TED -> SPF -> adapter -> backend ->
 * ZAPI -> zebra RIB -> FIB) and is what the zebra-restart replay and the
 * shutdown-withdraw checks use.
 *
 * Usage: dp-e2e-tool -s ZAPI_SOCKET <command> [args]
 * Every action prints one machine-readable line:
 *   DP action=<name> prefix=<p> rc=<n> installed=<n> adds=<n> dels=<n> fails=<n>
 */

#define main midrd_program_main
#include "midrd.c"
#undef main

#include <arpa/inet.h>
#include <signal.h>

#include "frrdistance.h"
#include "ipaddr.h"
#include "nexthop.h"
#include "prefix.h"
#include "zclient.h"

#include "midr-consumer.h"
#include "midr-dp-backend.h"
#include "midr-gre.h"
#include "midr-spf-install.h"
#include "midr-ted.h"
#include "midr-zebra.h"

static struct midr_context ctx;
static volatile sig_atomic_t stop_requested;

static void stop_cb(int signo)
{
	(void)signo;
	stop_requested = 1;
}

static void noop_cb(struct event *t)
{
	(void)t;
}

/* Service the event loop for @ms; also used to wait for zebra messages. */
static void pump(struct event_loop *master, uint32_t ms)
{
	uint64_t deadline = mono_ms() + ms;

	while (mono_ms() < deadline && !stop_requested) {
		struct event *timer = NULL;
		struct event ev = {};

		event_add_timer_msec(master, noop_cb, NULL, 20, &timer);
		/* A signal (EINTR) can make event_fetch() return without
		 * filling @ev, so only dispatch a real event. */
		if (event_fetch(master, &ev))
			event_call(&ev);
		event_cancel(&timer);
	}
}

static void report(const char *action, const char *prefix, int rc)
{
	struct midr_dp_status st;

	midr_dp_backend_status_get(&st);
	printf("DP action=%s prefix=%s rc=%d installed=%zu adds=%" PRIu64
	       " dels=%" PRIu64 " fails=%" PRIu64 "\n",
	       action, prefix ? prefix : "-", rc, st.installed, st.route_adds,
	       st.route_deletes, st.send_failures);
	(void)fflush(stdout);
}

static int tool_addr(const char *text, struct ipaddr *addr)
{
	memset(addr, 0, sizeof(*addr));
	if (strchr(text, ':')) {
		SET_IPADDR_V6(addr);
		return inet_pton(AF_INET6, text, &addr->ipaddr_v6) == 1 ? 0 : -1;
	}
	SET_IPADDR_V4(addr);
	return inet_pton(AF_INET, text, &addr->ipaddr_v4) == 1 ? 0 : -1;
}

/* "ADDR" or, for UCMP, "ADDR:WEIGHT" (a single colon means IPv4 + weight). */
static int tool_nexthop(const char *text, struct midr_path *path)
{
	char buf[128];
	const char *cursor;
	int colons = 0;

	snprintf(buf, sizeof(buf), "%s", text);
	path->weight = 0;
	for (cursor = buf; *cursor; cursor++)
		if (*cursor == ':')
			colons++;
	if (colons == 1) {
		char *colon = strchr(buf, ':');

		*colon = '\0';
		path->weight = (uint8_t)atoi(colon + 1);
	}
	return tool_addr(buf, &path->nexthop);
}

static int run_facade_add(const char *label, const char *prefix_text, int count,
			  char **nexthops, uint8_t instance, const char *sid_text)
{
	struct midr_path paths[MIDR_SRV6_MAX_SEGS] = {};
	struct midr_path_result result = {
		.paths = paths,
		.instance = instance,
	};
	struct prefix p;
	int rc;

	if (str2prefix(prefix_text, &p) != 1) {
		fprintf(stderr, "invalid prefix %s\n", prefix_text);
		return 2;
	}
	if (count < 1 || count > 2) {
		fprintf(stderr, "expected 1 or 2 nexthops\n");
		return 2;
	}
	for (int i = 0; i < count; i++)
		if (tool_nexthop(nexthops[i], &paths[i])) {
			fprintf(stderr, "invalid nexthop %s\n", nexthops[i]);
			return 2;
		}
	result.path_count = (uint8_t)count;
	paths[0].metric = 17;

	if (sid_text) {
		char *copy = strdup(sid_text);
		char *save = NULL;
		char *token;

		for (token = strtok_r(copy, ",", &save); token;
		     token = strtok_r(NULL, ",", &save)) {
			if (result.explicit.sid_count >= MIDR_SRV6_MAX_SEGS) {
				fprintf(stderr, "too many SIDs\n");
				free(copy);
				return 2;
			}
			if (inet_pton(AF_INET6, token,
				      &result.explicit.sid_list
					       [result.explicit.sid_count]) != 1) {
				fprintf(stderr, "invalid SID %s\n", token);
				free(copy);
				return 2;
			}
			result.explicit.sid_count++;
		}
		free(copy);
	}

	rc = midr_zebra_route_add(&ctx, &p, &result);
	if (!rc)
		rc = midr_zebra_route_flush(&ctx);
	report(label, prefix_text, rc);
	return rc ? 1 : 0;
}

static int run_del(const char *prefix_text, const char *instance_text)
{
	struct prefix p;
	uint8_t instance = MIDR_INSTANCE_SPF;
	int rc;

	if (str2prefix(prefix_text, &p) != 1) {
		fprintf(stderr, "invalid prefix %s\n", prefix_text);
		return 2;
	}
	if (instance_text && strcmp(instance_text, "TE") == 0)
		instance = MIDR_INSTANCE_TE;

	rc = midr_zebra_route_del(&ctx, &p, instance);
	if (!rc)
		rc = midr_zebra_route_flush(&ctx);
	report("del", prefix_text, rc);
	return rc ? 1 : 0;
}

/*
 * Synthetic committed TED: node 2 advertises 198.51.100.0/24 through the link
 * 1->2 whose nexthop is 192.0.200.2.  The SPF adapter turns that into the same
 * facade add a real TED commit would produce, so the whole midrd chain
 * (TED -> SPF -> adapter -> backend -> ZAPI -> zebra RIB -> FIB) is exercised.
 */
static int apply_synthetic_ted(void)
{
	struct midr_consumer_event events[2] = {0};
	struct midr_consumer_snapshot snapshot = {
		.generation = 7,
		.count = 2,
		.events = events,
	};

	events[0].kind = MIDR_CONSUMER_LINK;
	events[0].generation = 7;
	events[0].originator = 1;
	events[0].remote = 2;
	events[0].link_id = 1;
	events[0].family = MIDR_CORE_AF_IPV4;
	events[0].metric = 5;
	events[0].remote_address[0] = 192;
	events[0].remote_address[1] = 0;
	events[0].remote_address[2] = 200;
	events[0].remote_address[3] = 2;

	events[1].kind = MIDR_CONSUMER_NODE_PREFIX;
	events[1].generation = 7;
	events[1].originator = 2;
	events[1].family = MIDR_CORE_AF_IPV4;
	events[1].prefix_len = 24;
	events[1].prefix[0] = 198;
	events[1].prefix[1] = 51;
	events[1].prefix[2] = 100;
	events[1].metric = 3;

	if (midr_ted_apply_snapshot(ctx.ted, &snapshot)) {
		fprintf(stderr, "TED snapshot rejected\n");
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *sock = NULL;
	const char *command;
	uint32_t hold_ms = 0;
	int opt;
	int rc = 0;

	while ((opt = getopt(argc, argv, "s:t:")) != -1) {
		switch (opt) {
		case 's':
			sock = optarg;
			break;
		case 't':
			hold_ms = (uint32_t)strtoul(optarg, NULL, 10) * 1000U;
			break;
		default:
			fprintf(stderr,
				"usage: %s -s ZAPI_SOCKET [-t HOLD_SEC] CMD [ARGS]\n",
				argv[0]);
			return 2;
		}
	}
	if (optind >= argc) {
		fprintf(stderr, "missing command\n");
		return 2;
	}
	command = argv[optind++];
	if (!sock) {
		fprintf(stderr, "missing -s ZAPI_SOCKET\n");
		return 2;
	}

	memset(&ctx, 0, sizeof(ctx));
	ctx.node_id = 1;
	{
		struct midr_ted_config ted_config = {
			.max_events = 16,
		};

		if (midr_ted_create(&ted_config, &ctx.ted)) {
			fprintf(stderr, "TED create failed\n");
			return 1;
		}
	}
	ctx.master = event_master_create("midr-dp-e2e-tool");
	if (!ctx.master) {
		fprintf(stderr, "event master create failed\n");
		return 1;
	}
	snprintf(frr_zclientpath, sizeof(frr_zclientpath), "%s", sock);
	if (!frr_zclient_addr(&zclient_addr, &zclient_addr_len, sock)) {
		fprintf(stderr, "invalid zserv path %s\n", sock);
		return 2;
	}
	if (midr_dp_backend_start(&ctx, ctx.master, VRF_DEFAULT)) {
		fprintf(stderr, "data-plane backend start failed\n");
		return 1;
	}
	midr_gre_init(ctx.master);

	/* Let zclient_init()'s scheduled connect run to completion. */
	pump(ctx.master, 500);
	if (!midr_dp_backend_ready()) {
		fprintf(stderr, "zclient did not connect to %s\n", sock);
		midr_dp_backend_stop();
		return 1;
	}
	report("connect", "-", 0);

	if (strcmp(command, "add4") == 0 || strcmp(command, "add6") == 0)
		rc = run_facade_add(command, argv[optind], argc - optind - 1,
				    &argv[optind + 1], MIDR_INSTANCE_SPF, NULL);
	else if (strcmp(command, "add4-ecmp") == 0 ||
		 strcmp(command, "add4-ucmp") == 0)
		rc = run_facade_add(command, argv[optind], argc - optind - 1,
				    &argv[optind + 1], MIDR_INSTANCE_SPF, NULL);
	else if (strcmp(command, "add4-te") == 0)
		rc = run_facade_add(command, argv[optind], 1, &argv[optind + 1],
				    MIDR_INSTANCE_TE, NULL);
	else if (strcmp(command, "add6-srv6") == 0)
		rc = run_facade_add(command, argv[optind], 1, &argv[optind + 1],
				    MIDR_INSTANCE_SPF, argv[optind + 2]);
	else if (strcmp(command, "del") == 0)
		rc = run_del(argv[optind],
			     optind + 1 < argc ? argv[optind + 1] : NULL);
	else if (strcmp(command, "dup4") == 0) {
		/* Two identical add+flush cycles in one process: the second one
		 * must not send anything (installed-hash diff). */
		struct midr_dp_status first, second;

		rc = run_facade_add("dup4-first", argv[optind], 1,
				    &argv[optind + 1], MIDR_INSTANCE_SPF,
				    NULL);
		midr_dp_backend_status_get(&first);
		rc |= run_facade_add("dup4-second", argv[optind], 1,
				     &argv[optind + 1], MIDR_INSTANCE_SPF,
				     NULL);
		midr_dp_backend_status_get(&second);
		printf("DP dup4 adds_first=%" PRIu64 " adds_second=%" PRIu64
		       " dels_first=%" PRIu64 " dels_second=%" PRIu64 "\n",
		       first.route_adds, second.route_adds,
		       first.route_deletes, second.route_deletes);
		(void)fflush(stdout);
	} else if (strcmp(command, "replace4") == 0) {
		/* Two different nexthops from one client: the second add must
		 * replace the first (delete + add inside the backend). */
		rc = run_facade_add("replace4-old", argv[optind], 1,
				    &argv[optind + 1], MIDR_INSTANCE_SPF,
				    NULL);
		rc |= run_facade_add("replace4-new", argv[optind], 1,
				     &argv[optind + 2], MIDR_INSTANCE_SPF,
				     NULL);
	} else if (strcmp(command, "cycle4") == 0) {
		/* add -> dwell -> del from ONE client, so the caller can watch
		 * the FIB appear and then disappear without cross-client
		 * nexthop merging. */
		uint32_t dwell_ms = (uint32_t)strtoul(
			optind + 2 < argc ? argv[optind + 2] : "5", NULL, 10) *
			1000U;

		rc = run_facade_add("cycle4-add", argv[optind], 1,
				    &argv[optind + 1], MIDR_INSTANCE_SPF,
				    NULL);
		pump(ctx.master, dwell_ms);
		rc |= run_del(argv[optind], NULL);
	} else if (strcmp(command, "status") == 0)
		report("status", "-", 0);
	else if (strcmp(command, "serve-ted") == 0) {
		uint32_t run_ms = 30000;
		uint64_t deadline;

		if (optind < argc)
			run_ms = (uint32_t)strtoul(argv[optind], NULL, 10) * 1000U;
		{
			struct sigaction sa = {
				.sa_handler = stop_cb,
			};
			sigset_t set;

			sigaction(SIGTERM, &sa, NULL);
			sigaction(SIGINT, &sa, NULL);
			/* libfrr may leave the process signals blocked; make sure
			 * SIGTERM can actually reach stop_cb() so the shutdown
			 * withdraw path runs instead of an abrupt exit. */
			sigemptyset(&set);
			sigaddset(&set, SIGTERM);
			sigaddset(&set, SIGINT);
			sigprocmask(SIG_UNBLOCK, &set, NULL);
		}
		if (apply_synthetic_ted()) {
			rc = 1;
			goto out;
		}
		report("serve-ted-add", "198.51.100.0/24", 0);
		deadline = mono_ms() + run_ms;
		while (!stop_requested && mono_ms() < deadline) {
			pump(ctx.master, 1000);
			report("serve-ted-tick", "198.51.100.0/24", 0);
		}
		report("serve-ted-exit", "198.51.100.0/24", 0);
	} else {
		fprintf(stderr, "unknown command %s\n", command);
		rc = 2;
	}

	/* Hold the connection open so a caller can observe zebra and the Linux
	 * FIB before this process withdraws its routes on exit. */
	if (hold_ms)
		pump(ctx.master, hold_ms);

out:
	midr_gre_fini();
	if (midr_dp_backend_get())
		midr_dp_backend_stop();
	report("shutdown", "-", rc);
	midr_ted_destroy(&ctx.ted);
	return rc;
}
