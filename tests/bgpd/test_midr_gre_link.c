// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR GRE virtual link setup / teardown helper (used by the two-container
 * connectivity test).
 *
 * Creates (or removes) a GRE virtual interface through the bgpd MIDR
 * control-plane API and, in "setup" mode, optionally configures an overlay
 * address and brings the link up so connectivity can be verified.
 *
 * Usage:
 *   test_midr_gre_link setup    [options]
 *   test_midr_gre_link teardown [options]
 *
 * Options:
 *   --sock   PATH     zebra ZAPI socket     (default /tmp/zserv.api)
 *   --name   NAME     tunnel device name    (default midr0)
 *   --local  ADDR     tunnel source         (required)
 *   --remote ADDR     tunnel destination    (required)
 *   --ip     A/len    overlay IPv4 to assign (optional)
 *   --ip6    X::Y/len  overlay IPv6 to assign (optional)
 *   --mtu    N        tunnel MTU            (default 1400)
 *   --wait-ms N       establishment budget  (default 3000)
 *
 * Machine readable status line printed on stdout:
 *   MIDR_GRE name=<n> state=<up|down|pending|failed> ifindex=<n> err=<n>
 */
#include <zebra.h>

#include <arpa/inet.h>

#include "lib/frrevent.h"
#include "lib/if.h"
#include "lib/ipaddr.h"
#include "lib/libfrr.h"
#include "lib/vrf.h"
#include "lib/zclient.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_midr_gre.h"

struct zebra_privs_t bgpd_privs = {};
struct event_loop *master;
struct zclient *bgp_zclient;
struct bgp_master *bm;
static struct bgp_master dummy_bm;

static void gre_notify_cb(const struct midr_gre_status *st, void *arg)
{
	(void)arg;
	printf("    [notify] %s: state=%s ifindex=%u err=%d\n", st->ifname,
	       midr_gre_state_str(st->state), st->ifindex, st->err);
	fflush(stdout);
}

static int run(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int run(const char *fmt, ...)
{
	char cmd[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);

	return system(cmd);
}

/* zclient_init() schedules a connect event - run it to completion. */
static int zebra_connect(const char *sock)
{
	master = event_master_create("midr_gre_link");
	dummy_bm.master = master;
	bm = &dummy_bm;

	/* The lib interface-add handler needs the VRF to exist. */
	vrf_get(VRF_DEFAULT, VRF_DEFAULT_NAME);

	bgp_zclient = zclient_new(master, &zclient_options_default, NULL, 0);
	if (!bgp_zclient)
		return -1;

	zclient_init(bgp_zclient, ZEBRA_ROUTE_BGP_MIDR, 0, &bgpd_privs);
	if (!frr_zclient_addr(&zclient_addr, &zclient_addr_len, sock))
		return -1;

	for (int i = 0; i < 8 && bgp_zclient->sock < 0; i++) {
		struct event ev = {};

		event_fetch(master, &ev);
		event_call(&ev);
	}

	return bgp_zclient->sock < 0 ? -1 : 0;
}

static void fill_endpoint(struct ipaddr *ia, const char *addr)
{
	if (strchr(addr, ':')) {
		ia->ipa_type = IPADDR_V6;
		inet_pton(AF_INET6, addr, &ia->ipaddr_v6);
	} else {
		ia->ipa_type = IPADDR_V4;
		inet_pton(AF_INET, addr, &ia->ipaddr_v4);
	}
}

static void wait_noop_cb(struct event *t)
{
	(void)t;
}

/*
 * Keep the zclient (and its socket) alive for @ms milliseconds while
 * servicing incoming zebra messages.  Exiting immediately after a send
 * races with zebra's client read: if the socket is closed in the same
 * read the pending request can be dropped.
 */
static void pump_events(uint32_t ms)
{
	struct event *wake = NULL;
	struct event ev = {};
	uint32_t waited = 0;

	while (waited < ms) {
		event_add_timer_msec(master, wait_noop_cb, NULL, 50, &wake);
		event_fetch(master, &ev);
		event_call(&ev);
		event_cancel(&wake);
		waited += 50;
	}
}

int main(int argc, char **argv)
{
	const char *sock = "/tmp/zserv.api";
	const char *name = "midr0";
	const char *local = NULL, *remote = NULL;
	const char *ip = NULL, *ip6 = NULL;
	uint32_t mtu = 1400, wait_ms = 3000;
	struct midr_gre_status st;
	bool setup;
	int rc = 1;

	if (argc < 2 ||
	    (strcmp(argv[1], "setup") != 0 && strcmp(argv[1], "teardown") != 0)) {
		printf("usage: %s setup|teardown [--sock P] [--name N] "
		       "--local A --remote B [--ip A/len] [--ip6 X/len] "
		       "[--mtu N] [--wait-ms N]\n",
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
		else if (!strcmp(argv[i], "--ip") && i + 1 < argc)
			ip = argv[++i];
		else if (!strcmp(argv[i], "--ip6") && i + 1 < argc)
			ip6 = argv[++i];
		else if (!strcmp(argv[i], "--mtu") && i + 1 < argc)
			mtu = strtoul(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--wait-ms") && i + 1 < argc)
			wait_ms = strtoul(argv[++i], NULL, 10);
		else {
			printf("unknown/incomplete option: %s\n", argv[i]);
			return 2;
		}
	}

	if (zebra_connect(sock) < 0) {
		printf("MIDR_GRE name=%s state=failed ifindex=0 err=%d\n", name,
		       ENOTCONN);
		return 1;
	}
	midr_gre_register_notify(gre_notify_cb, NULL);

	if (!setup) {
		if (midr_gre_interface_del(NULL, name, &st) != 0) {
			printf("MIDR_GRE name=%s state=%s ifindex=0 err=%d\n",
			       name, midr_gre_state_str(st.state), st.err);
			goto out;
		}
		pump_events(1000);
		printf("MIDR_GRE name=%s state=%s ifindex=0 err=0\n", name,
		       midr_gre_state_str(st.state));
		rc = 0;
		goto out;
	}

	if (!local || !remote) {
		printf("setup requires --local and --remote\n");
		rc = 2;
		goto out;
	}

	{
		struct midr_gre_tunnel tun = {};

		strlcpy(tun.ifname, name, sizeof(tun.ifname));
		tun.vrf_id = VRF_DEFAULT;
		fill_endpoint(&tun.local, local);
		fill_endpoint(&tun.remote, remote);
		tun.mtu = mtu;

		if (midr_gre_interface_add(NULL, &tun, &st) != 0) {
			printf("MIDR_GRE name=%s state=failed ifindex=0 err=%d\n",
			       name, st.err);
			goto out;
		}

		if (!midr_gre_interface_wait_up(NULL, name, wait_ms, &st)) {
			printf("MIDR_GRE name=%s state=%s ifindex=0 err=%d\n",
			       name, midr_gre_state_str(st.state), st.err);
			goto out;
		}

		/* Configure overlay address(es) and bring the link up. */
		if (ip)
			run("ip addr add %s dev %s 2>/dev/null || true", ip,
			    name);
		if (ip6)
			run("ip addr add %s dev %s 2>/dev/null || true", ip6,
			    name);
		run("ip link set %s up 2>/dev/null || true", name);

		printf("MIDR_GRE name=%s state=%s ifindex=%u err=0\n", name,
		       midr_gre_state_str(st.state), st.ifindex);
		rc = 0;
	}

out:
	midr_gre_unregister_notify(gre_notify_cb);
	midr_gre_fini();
	return rc;
}
