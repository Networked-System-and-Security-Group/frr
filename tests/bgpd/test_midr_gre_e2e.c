// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Standalone E2E test for the MIDR GRE interface create/delete path and the
 * establishment-status API.
 *
 * Connects to a running zebra (built with ZEBRA_GRE_ADD / ZEBRA_GRE_DELETE)
 * and drives the bgpd/bgp_midr_gre.c control-plane API:
 *   midr_gre_interface_add()       -> immediate status (PENDING / UP / FAILED)
 *   midr_gre_interface_wait_up()   -> confirmed establishment result
 *   midr_gre_interface_get_state() -> non-blocking query
 *   midr_gre_interface_del()       -> teardown
 *
 * Usage: ./test_midr_gre_e2e [zserv_socket]
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

static const char *IFNAME = "midr0";
static const char *IFNAME6 = "midr6";
static const char *LOCAL = "192.0.2.1";
static const char *REMOTE = "192.0.2.2";

static int if_exists(const char *name)
{
	char cmd[128];

	snprintf(cmd, sizeof(cmd), "ip link show %s >/dev/null 2>&1", name);
	return system(cmd) == 0;
}

static void gre_notify_cb(const struct midr_gre_status *st, void *arg)
{
	(void)arg;
	printf("    [notify] %s: state=%s ifindex=%u err=%d\n", st->ifname,
	       midr_gre_state_str(st->state), st->ifindex, st->err);
}

static void fill_v4(struct midr_gre_tunnel *t, const char *local,
		    const char *remote)
{
	t->vrf_id = VRF_DEFAULT;
	t->local.ipa_type = IPADDR_V4;
	inet_pton(AF_INET, local, &t->local.ipaddr_v4);
	t->remote.ipa_type = IPADDR_V4;
	inet_pton(AF_INET, remote, &t->remote.ipaddr_v4);
}

static void fill_v6(struct midr_gre_tunnel *t, const char *local,
		    const char *remote)
{
	t->vrf_id = VRF_DEFAULT;
	t->local.ipa_type = IPADDR_V6;
	inet_pton(AF_INET6, local, &t->local.ipaddr_v6);
	t->remote.ipa_type = IPADDR_V6;
	inet_pton(AF_INET6, remote, &t->remote.ipaddr_v6);
}

int main(int argc, char **argv)
{
	const char *sock = argc > 1 ? argv[1] : "/tmp/zserv_gre.api";
	struct bgp bgp = {};
	struct midr_gre_status st;
	struct midr_gre_tunnel tun = {};
	static struct bgp_master dummy_bm = {};
	int rc = 0;

	printf("=== MIDR GRE interface E2E test (status API) ===\n");

	system("ip addr add 192.0.2.1/32 dev lo 2>/dev/null || true");
	system("ip addr add 2001:db8::1/128 dev lo 2>/dev/null || true");

	printf("[1] connect zebra %s\n", sock);
	master = event_master_create("gre_e2e");
	dummy_bm.master = master;
	bm = &dummy_bm;

	/*
	 * zclient_interface_add() (the lib default handler for
	 * ZEBRA_INTERFACE_ADD) needs the VRF to exist, otherwise zebra's
	 * interface-add broadcast for our new device is dropped.
	 */
	vrf_get(VRF_DEFAULT, VRF_DEFAULT_NAME);

	bgp_zclient = zclient_new(master, &zclient_options_default, NULL, 0);
	if (!bgp_zclient) {
		printf("FAIL: zclient_new\n");
		return 1;
	}
	zclient_init(bgp_zclient, ZEBRA_ROUTE_BGP_MIDR, 0, &bgpd_privs);
	if (!frr_zclient_addr(&zclient_addr, &zclient_addr_len, sock)) {
		printf("FAIL: bad socket path\n");
		return 1;
	}
	/*
	 * zclient_init() scheduled a connect event; running it performs
	 * connect + HELLO + "subscribe for interface notifications"
	 * (ZEBRA_INTERFACE_ADD) and arms the read handler.  All of that is
	 * required for the establishment confirmation to work.
	 */
	for (int i = 0; i < 8 && bgp_zclient->sock < 0; i++) {
		struct event ev = {};

		event_fetch(master, &ev);
		event_call(&ev);
	}
	if (bgp_zclient->sock < 0) {
		printf("FAIL: cannot connect to zebra\n");
		return 1;
	}
	midr_gre_register_notify(gre_notify_cb, NULL);
	printf("    connected (sock %d)\n", bgp_zclient->sock);

	/* ---- IPv4 ---- */
	printf("[2] IPv4: add %s (local %s remote %s)\n", IFNAME, LOCAL,
	       REMOTE);
	strlcpy(tun.ifname, IFNAME, sizeof(tun.ifname));
	fill_v4(&tun, LOCAL, REMOTE);
	tun.mtu = 1400;

	if (midr_gre_interface_add(&bgp, &tun, &st) != 0) {
		printf("FAIL: add request (state=%s err=%d)\n",
		       midr_gre_state_str(st.state), st.err);
		rc = 1;
	} else {
		printf("    add accepted, immediate state=%s\n",
		       midr_gre_state_str(st.state));
	}

	if (midr_gre_interface_wait_up(&bgp, IFNAME, 3000, &st)) {
		printf("PASS: %s established (state=%s ifindex=%u iftype=%u up_ms=%u)\n",
		       IFNAME, midr_gre_state_str(st.state), st.ifindex,
		       st.iftype, st.up_ms);
	} else {
		printf("FAIL: %s not established (state=%s err=%d)\n", IFNAME,
		       midr_gre_state_str(st.state), st.err);
		rc = 1;
	}
	if (if_exists(IFNAME))
		system("ip -d link show midr0 | head -3");
	else {
		printf("FAIL: %s missing in kernel\n", IFNAME);
		rc = 1;
	}

	if (midr_gre_interface_get_state(VRF_DEFAULT, IFNAME, &st) == 0)
		printf("    get_state(%s) = %s ifindex=%u\n", IFNAME,
		       midr_gre_state_str(st.state), st.ifindex);
	else {
		printf("FAIL: get_state(%s) unknown\n", IFNAME);
		rc = 1;
	}

	/* ---- IPv6 ---- */
	printf("[3] IPv6: add %s (ip6gre)\n", IFNAME6);
	memset(&tun, 0, sizeof(tun));
	strlcpy(tun.ifname, IFNAME6, sizeof(tun.ifname));
	fill_v6(&tun, "2001:db8::1", "2001:db8::2");
	tun.mtu = 1440;

	if (midr_gre_interface_add(&bgp, &tun, &st) != 0) {
		printf("FAIL: ip6gre add request\n");
		rc = 1;
	}
	if (midr_gre_interface_wait_up(&bgp, IFNAME6, 3000, &st)) {
		printf("PASS: %s established (ifindex=%u)\n", IFNAME6,
		       st.ifindex);
		if (system("ip -d link show midr6 | grep -q ip6gre") == 0)
			printf("PASS: %s uses the ip6gre kind\n", IFNAME6);
		else {
			printf("FAIL: %s is not ip6gre\n", IFNAME6);
			rc = 1;
		}
	} else {
		printf("FAIL: %s not established (state=%s err=%d)\n",
		       IFNAME6, midr_gre_state_str(st.state), st.err);
		rc = 1;
	}
	if (midr_gre_interface_del(&bgp, IFNAME6, &st) != 0) {
		printf("FAIL: ip6gre delete\n");
		rc = 1;
	} else
		printf("PASS: %s removed (state=%s)\n", IFNAME6,
		       midr_gre_state_str(st.state));

	/* ---- auto naming ---- */
	printf("[4] auto-named tunnel (ifname empty)\n");
	{
		struct midr_gre_tunnel auto_tun = {};
		const char *nm;

		fill_v4(&auto_tun, LOCAL, "192.0.2.3");
		if (midr_gre_interface_add(&bgp, &auto_tun, &st) != 0) {
			printf("FAIL: auto add request\n");
			rc = 1;
		}
		nm = midr_gre_interface_name(VRF_DEFAULT, &auto_tun.local,
					     &auto_tun.remote);
		if (nm) {
			printf("PASS: auto name = %s\n", nm);
			if (midr_gre_interface_wait_up(&bgp, nm, 3000, &st))
				printf("PASS: %s established (ifindex=%u)\n",
				       nm, st.ifindex);
			else {
				printf("FAIL: %s not established\n", nm);
				rc = 1;
			}
			if (midr_gre_interface_del_by_endpoints(
				    &bgp, VRF_DEFAULT, &auto_tun.local,
				    &auto_tun.remote, &st) != 0) {
				printf("FAIL: auto delete\n");
				rc = 1;
			}
		} else {
			printf("FAIL: no auto name registered\n");
			rc = 1;
		}
	}

	/* ---- explicit delete ---- */
	printf("[5] delete %s\n", IFNAME);
	if (midr_gre_interface_del(&bgp, IFNAME, &st) != 0) {
		printf("FAIL: delete request\n");
		rc = 1;
	}
	sleep(1);
	if (!if_exists(IFNAME))
		printf("PASS: %s removed\n", IFNAME);
	else {
		printf("FAIL: %s still present\n", IFNAME);
		rc = 1;
	}

	midr_gre_unregister_notify(gre_notify_cb);
	midr_gre_fini();

	printf("=== result: %s ===\n", rc == 0 ? "ALL PASS" : "FAILURES");
	return rc;
}
