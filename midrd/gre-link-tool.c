// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR GRE virtual-link provisioning tool (a miniature midrd host).
 *
 * The two-container GRE connectivity test needs a tiny program that can
 * create and remove a GRE virtual interface through the *midrd* GRE API
 * (midr-gre.c).  The GRE request travels on the shared data-plane zclient,
 * so this tool stands up the same minimal pieces a real midrd host does:
 *
 *   event loop -> TED -> data-plane backend (zclient) -> GRE module
 *
 * midrd.c is pulled in with the repository include trick so the private
 * struct midr_context layout is available without exporting it; the daemon
 * main() is renamed and never called.
 *
 * Usage:
 *   midrd-gre-tool setup|teardown [options]
 *
 * Options:
 *   --sock    PATH  zebra ZAPI socket      (default /tmp/zserv.api)
 *   --name    NAME  tunnel device name     (default midr0)
 *   --local   ADDR  tunnel source          (setup, required)
 *   --remote  ADDR  tunnel destination     (setup, required)
 *   --mtu     N     tunnel MTU             (default 1400)
 *   --wait-ms N     establishment budget   (default 3000)
 *
 * Machine-readable status line on stdout:
 *   MIDR_GRE name=<n> state=<up|down|pending|failed> ifindex=<n> err=<n>
 *
 * Exit status: 0 when the requested action reached the expected state
 * (UP for setup, DOWN for teardown), non-zero otherwise.
 */

#define main midrd_program_main
#include "midrd.c"
#undef main

#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frrevent.h"
#include "libfrr.h"
#include "zclient.h"
#include "midr-context.h"
#include "midr-dp-backend.h"
#include "midr-gre.h"

/* Wake-up-only callback: keeps the event loop honest while we block. */
static void gre_tool_tick_cb(struct event *t)
{
	(void)t;
}

/* Service the event loop for @ms milliseconds in 20 ms slices. */
static void gre_tool_pump(struct event_loop *master, uint32_t ms)
{
	uint32_t waited = 0;

	while (waited < ms) {
		struct event ev = {};
		struct event *wake = NULL;

		event_add_timer_msec(master, gre_tool_tick_cb, NULL, 20, &wake);
		event_fetch(master, &ev);
		event_call(&ev);
		event_cancel(&wake);
		waited += 20;
	}
}

/* Pump until the shared zclient is connected, or @budget_ms elapses. */
static bool gre_tool_wait_zebra(struct event_loop *master, uint32_t budget_ms)
{
	uint32_t waited = 0;

	while (waited < budget_ms && !midr_dp_backend_ready()) {
		gre_tool_pump(master, 20);
		waited += 20;
	}
	return midr_dp_backend_ready();
}

static void gre_tool_fill_endpoint(struct ipaddr *ia, const char *addr)
{
	if (strchr(addr, ':')) {
		ia->ipa_type = IPADDR_V6;
		inet_pton(AF_INET6, addr, &ia->ipaddr_v6);
	} else {
		ia->ipa_type = IPADDR_V4;
		inet_pton(AF_INET, addr, &ia->ipaddr_v4);
	}
}

static void gre_tool_notify_cb(const struct midr_gre_status *st, void *arg)
{
	(void)arg;
	printf("    [notify] %s: state=%s ifindex=%u err=%d\n", st->ifname,
	       midr_gre_state_str(st->state), st->ifindex, st->err);
	fflush(stdout);
}

static void gre_tool_status_line(const char *name,
				 const struct midr_gre_status *st)
{
	printf("MIDR_GRE name=%s state=%s ifindex=%u err=%d\n", name,
	       midr_gre_state_str(st->state), st->ifindex, st->err);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	const char *sock = "/tmp/zserv.api";
	const char *name = "midr0";
	const char *local = NULL, *remote = NULL;
	uint32_t mtu = 1400, wait_ms = 3000;
	struct midr_ted_config ted_config = {
		.max_events = 16,
	};
	struct midr_context ctx;
	struct midr_gre_status st;
	bool setup;
	int rc = 1;

	if (argc < 2 ||
	    (strcmp(argv[1], "setup") != 0 &&
	     strcmp(argv[1], "teardown") != 0)) {
		printf("usage: %s setup|teardown [--sock P] [--name N] "
		       "--local A --remote B [--mtu N] [--wait-ms N]\n",
		       argv[0]);
		return 2;
	}
	setup = strcmp(argv[1], "setup") == 0;

	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--sock") && i + 1 < argc)
			sock = argv[++i];
		else if (!strcmp(argv[i], "--name") && i + 1 < argc)
			name = argv[++i];
		else if (!strcmp(argv[i], "--local") && i + 1 < argc)
			local = argv[++i];
		else if (!strcmp(argv[i], "--remote") && i + 1 < argc)
			remote = argv[++i];
		else if (!strcmp(argv[i], "--mtu") && i + 1 < argc)
			mtu = strtoul(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--wait-ms") && i + 1 < argc)
			wait_ms = strtoul(argv[++i], NULL, 10);
		else {
			printf("unknown/incomplete option: %s\n", argv[i]);
			return 2;
		}
	}
	if (setup && (!local || !remote)) {
		printf("setup requires --local and --remote\n");
		return 2;
	}

	/* Minimal MIDR host bootstrap, mirroring midrd's data-plane path. */
	memset(&ctx, 0, sizeof(ctx));
	snprintf(frr_zclientpath, sizeof(frr_zclientpath), "%s", sock);
	if (!frr_zclient_addr(&zclient_addr, &zclient_addr_len, sock)) {
		printf("MIDR_GRE name=%s state=failed ifindex=0 err=%d\n", name,
		       EINVAL);
		return 1;
	}
	ctx.master = event_master_create("midr-gre-tool");
	if (!ctx.master) {
		printf("MIDR_GRE name=%s state=failed ifindex=0 err=%d\n", name,
		       ENOMEM);
		return 1;
	}

	/* The libfrr interface-add handler needs the default VRF to exist. */
	vrf_get(VRF_DEFAULT, VRF_DEFAULT_NAME);

	if (midr_ted_create(&ted_config, &ctx.ted)) {
		printf("MIDR_GRE name=%s state=failed ifindex=0 err=%d\n", name,
		       ENOMEM);
		goto out;
	}
	if (midr_dp_backend_start(&ctx, ctx.master, VRF_DEFAULT)) {
		printf("MIDR_GRE name=%s state=failed ifindex=0 err=%d\n", name,
		       ENOTCONN);
		goto out;
	}
	midr_gre_init(ctx.master);

	if (!gre_tool_wait_zebra(ctx.master, 5000)) {
		printf("MIDR_GRE name=%s state=failed ifindex=0 err=%d\n", name,
		       ENOTCONN);
		goto out;
	}
	midr_gre_register_notify(gre_tool_notify_cb, NULL);

	if (!setup) {
		if (midr_gre_interface_del(&ctx, name, &st) != 0) {
			gre_tool_status_line(name, &st);
			goto out;
		}
		gre_tool_pump(ctx.master, 1000);
		gre_tool_status_line(name, &st);
		rc = st.state == MIDR_GRE_STATE_DOWN ? 0 : 1;
		goto out;
	}

	{
		struct midr_gre_tunnel tun = {};

		strlcpy(tun.ifname, name, sizeof(tun.ifname));
		tun.vrf_id = VRF_DEFAULT;
		gre_tool_fill_endpoint(&tun.local, local);
		gre_tool_fill_endpoint(&tun.remote, remote);
		tun.mtu = mtu;

		if (midr_gre_interface_add(&ctx, &tun, &st) != 0) {
			gre_tool_status_line(name, &st);
			goto out;
		}
		if (!midr_gre_interface_wait_up(&ctx, name, wait_ms, &st)) {
			gre_tool_status_line(name, &st);
			goto out;
		}
		gre_tool_status_line(name, &st);
		rc = st.state == MIDR_GRE_STATE_UP ? 0 : 1;
	}

out:
	midr_gre_unregister_notify(gre_tool_notify_cb);
	midr_dp_backend_log_status("gre-tool");
	midr_dp_backend_stop();
	midr_gre_fini();
	if (ctx.ted)
		midr_ted_destroy(&ctx.ted);
	return rc;
}
