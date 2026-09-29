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
 *   midrd-gre-tool vlink-setup [options]
 *
 * The plain setup/teardown pair only creates/removes the GRE device
 * (midr-gre.c).  vlink-setup drives the high-level virtual-link API
 * (midr-virtual-link.c): device + overlay address + admin-up + READY, i.e.
 * the same sequence the connectivity test used to script by hand with
 * `ip addr add` / `ip link set up`.  Teardown reuses the plain `teardown`
 * (the virtual-link registry is process-local, and deleting the GRE device
 * removes its overlay addresses with it).
 *
 * Options:
 *   --sock    PATH  zebra ZAPI socket      (default /tmp/zserv.api)
 *   --name    NAME  tunnel device name     (default midr0)
 *   --local   ADDR  tunnel source          (setup, required)
 *   --remote  ADDR  tunnel destination     (setup, required)
 *   --mtu     N     tunnel MTU             (default 1400)
 *   --wait-ms N     establishment budget   (default 3000)
 *   --overlay-local  ADDR   overlay local  (vlink-setup, required)
 *   --overlay-remote ADDR   overlay peer   (vlink-setup, required)
 *   --overlay-prefix N      overlay prefix (vlink-setup, required)
 *
 * Machine-readable status line on stdout:
 *   MIDR_GRE  name=<n> state=<up|down|pending|failed> ifindex=<n> err=<n>
 *   MIDR_VLINK name=<n> state=<down|creating|device_up|configuring|ready|
 *                              failed> ifindex=<n> iftype=<n> err=<n>
 *                              overlay_local=<addr|none> overlay_remote=<addr|none>
 *                              overlay_plen=<n> overlay_ready=<0|1>
 *                              event=<none|device_up|address_set|ready|failed|down>
 * The overlay_* and event tokens are appended so the existing name/state/ifindex/
 * iftype/err parsers keep working; event is the transition that fired the
 * notification (the query/status path reports event=none).
 *
 * Exit status: 0 when the requested action reached the expected state
 * (UP/READY for setup, DOWN for teardown), non-zero otherwise.
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
#include "midr-virtual-link.h"

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

/*
 * Render an overlay address for the machine-readable status lines.
 * ipaddr2str() is the tree-wide convention, but it yields an empty string
 * for IPADDR_NONE (AF_UNSPEC); print an explicit "none" instead so a
 * status/notify line that has no overlay yet stays deterministic and
 * parsable instead of ending in a bare "overlay_local=".
 */
static const char *gre_tool_ipaddr_str(const struct ipaddr *ia, char *buf,
				       size_t buflen)
{
	if (!ia || IS_IPADDR_NONE(ia)) {
		snprintf(buf, buflen, "none");
		return buf;
	}
	return ipaddr2str(ia, buf, (int)buflen);
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

static void gre_tool_vlink_notify_cb(
	const struct midr_virtual_link_status *st, void *arg)
{
	char obuf[IPADDR_STRING_SIZE], rbuf[IPADDR_STRING_SIZE];

	(void)arg;
	printf("    [notify] %s: state=%s ifindex=%u iftype=%u err=%d "
	       "overlay_local=%s overlay_remote=%s overlay_plen=%u "
	       "overlay_ready=%u event=%s\n",
	       st->ifname, midr_virtual_link_state_str(st->state), st->ifindex,
	       st->iftype, st->last_error,
	       gre_tool_ipaddr_str(&st->overlay_local, obuf, sizeof(obuf)),
	       gre_tool_ipaddr_str(&st->overlay_remote, rbuf, sizeof(rbuf)),
	       st->overlay_prefix_len, st->overlay_ready ? 1u : 0u,
	       midr_virtual_link_event_str(st->event));
	fflush(stdout);
}

static void gre_tool_vlink_status_line(
	const char *name, const struct midr_virtual_link_status *st)
{
	char obuf[IPADDR_STRING_SIZE], rbuf[IPADDR_STRING_SIZE];

	printf("MIDR_VLINK name=%s state=%s ifindex=%u iftype=%u err=%d "
	       "overlay_local=%s overlay_remote=%s overlay_plen=%u "
	       "overlay_ready=%u event=%s\n",
	       name, midr_virtual_link_state_str(st->state), st->ifindex,
	       st->iftype, st->last_error,
	       gre_tool_ipaddr_str(&st->overlay_local, obuf, sizeof(obuf)),
	       gre_tool_ipaddr_str(&st->overlay_remote, rbuf, sizeof(rbuf)),
	       st->overlay_prefix_len, st->overlay_ready ? 1u : 0u,
	       midr_virtual_link_event_str(st->event));
	fflush(stdout);
}

/* One-line failure report in whichever machine-readable format the caller
 * asked for, so the connectivity script always has a parsable status. */
static void gre_tool_fail(const char *name, bool vlink, int err)
{
	if (vlink)
		printf("MIDR_VLINK name=%s state=failed ifindex=0 iftype=0 "
		       "err=%d\n",
		       name, err);
	else
		printf("MIDR_GRE name=%s state=failed ifindex=0 err=%d\n", name,
		       err);
	fflush(stdout);
}


int main(int argc, char **argv)
{
	const char *sock = "/tmp/zserv.api";
	const char *name = "midr0";
	const char *local = NULL, *remote = NULL;
	const char *overlay_local = NULL, *overlay_remote = NULL;
	uint32_t mtu = 1400, wait_ms = 3000;
	uint32_t overlay_prefix = 0;
	bool has_overlay_prefix = false;
	struct midr_ted_config ted_config = {
		.max_events = 16,
	};
	struct midr_context ctx;
	struct midr_gre_status st;
	struct midr_virtual_link_status vst;
	enum {
		MODE_SETUP,
		MODE_TEARDOWN,
		MODE_VLINK_SETUP,
	} mode;
	bool setup;
	bool vlink;
	int rc = 1;

	if (argc < 2 || (!strcmp(argv[1], "setup") != 0 &&
			  !strcmp(argv[1], "teardown") != 0 &&
			  !strcmp(argv[1], "vlink-setup") != 0)) {
		printf("usage: %s setup|teardown|vlink-setup [--sock P] "
		       "[--name N] --local A --remote B [--mtu N] "
		       "[--wait-ms N] [--overlay-local A --overlay-remote B "
		       "--overlay-prefix N]\n",
		       argv[0]);
		return 2;
	}
	if (!strcmp(argv[1], "setup"))
		mode = MODE_SETUP;
	else if (!strcmp(argv[1], "teardown"))
		mode = MODE_TEARDOWN;
	else
		mode = MODE_VLINK_SETUP;

	setup = (mode == MODE_SETUP || mode == MODE_VLINK_SETUP);
	vlink = (mode == MODE_VLINK_SETUP);

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
		else if (!strcmp(argv[i], "--overlay-local") && i + 1 < argc)
			overlay_local = argv[++i];
		else if (!strcmp(argv[i], "--overlay-remote") && i + 1 < argc)
			overlay_remote = argv[++i];
		else if (!strcmp(argv[i], "--overlay-prefix") && i + 1 < argc) {
			overlay_prefix = strtoul(argv[++i], NULL, 10);
			has_overlay_prefix = true;
		} else {
			printf("unknown/incomplete option: %s\n", argv[i]);
			return 2;
		}
	}
	if (setup && (!local || !remote)) {
		printf("setup requires --local and --remote\n");
		return 2;
	}
	if (mode == MODE_VLINK_SETUP &&
	    (!overlay_local || !overlay_remote || !has_overlay_prefix)) {
		printf("vlink-setup requires --overlay-local, --overlay-remote "
		       "and --overlay-prefix\n");
		return 2;
	}

	/* Minimal MIDR host bootstrap, mirroring midrd's data-plane path. */
	memset(&ctx, 0, sizeof(ctx));
	snprintf(frr_zclientpath, sizeof(frr_zclientpath), "%s", sock);
	if (!frr_zclient_addr(&zclient_addr, &zclient_addr_len, sock)) {
		gre_tool_fail(name, vlink, EINVAL);
		return 1;
	}
	ctx.master = event_master_create("midr-gre-tool");
	if (!ctx.master) {
		gre_tool_fail(name, vlink, ENOMEM);
		return 1;
	}

	/* The libfrr interface-add handler needs the default VRF to exist. */
	vrf_get(VRF_DEFAULT, VRF_DEFAULT_NAME);

	if (midr_ted_create(&ted_config, &ctx.ted)) {
		gre_tool_fail(name, vlink, ENOMEM);
		goto out;
	}
	if (midr_dp_backend_start(&ctx, ctx.master, VRF_DEFAULT)) {
		gre_tool_fail(name, vlink, ENOTCONN);
		goto out;
	}
	midr_gre_init(ctx.master);
	midr_virtual_link_init(ctx.master);

	if (!gre_tool_wait_zebra(ctx.master, 5000)) {
		gre_tool_fail(name, vlink, ENOTCONN);
		goto out;
	}
	midr_gre_register_notify(gre_tool_notify_cb, NULL);

	/* vlink-setup: device + overlay address + admin-up, confirmed by READY. */
	if (vlink) {
		struct midr_virtual_link_desc d = {};

		memset(&vst, 0, sizeof(vst));
		strlcpy(d.ifname, name, sizeof(d.ifname));
		d.vrf_id = VRF_DEFAULT;
		gre_tool_fill_endpoint(&d.outer_local, local);
		gre_tool_fill_endpoint(&d.outer_remote, remote);
		d.mtu = mtu;
		gre_tool_fill_endpoint(&d.overlay_local, overlay_local);
		gre_tool_fill_endpoint(&d.overlay_remote, overlay_remote);
		d.overlay_prefix_len = (uint8_t)overlay_prefix;

		midr_virtual_link_register_notify(gre_tool_vlink_notify_cb,
						  NULL);
		if (midr_virtual_link_add(&ctx, &d, &vst) != 0) {
			gre_tool_vlink_status_line(name, &vst);
			goto out;
		}
		if (!midr_virtual_link_wait_ready(&ctx, name, wait_ms, &vst)) {
			gre_tool_vlink_status_line(name, &vst);
			goto out;
		}
		gre_tool_vlink_status_line(name, &vst);
		rc = vst.state == MIDR_VLINK_READY ? 0 : 1;
		goto out;
	}

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
	midr_virtual_link_unregister_notify(gre_tool_vlink_notify_cb);
	midr_gre_unregister_notify(gre_tool_notify_cb);
	midr_virtual_link_fini();
	midr_dp_backend_log_status("gre-tool");
	midr_dp_backend_stop();
	midr_gre_fini();
	if (ctx.ted)
		midr_ted_destroy(&ctx.ted);
	return rc;
}
