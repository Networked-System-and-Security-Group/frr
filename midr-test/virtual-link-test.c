// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR high-level virtual-link component tests.
 *
 * Covers the twelve cases frozen in doc/midr-doc/dp-doc/midr-virtual-link-api.md
 * §9.2 plus the registry allocation-failure path of §4.1 and the third-group
 * D1-D4 regression cases 13..18:
 *
 *   - case13/18: autonomous device-loss failure (module-wide reconcile timer,
 *     MIDR_VLINK_RECONCILE_INTERVAL_MS = 1000) with no get_state() call, and
 *     the D4 create/delete accounting of the retry that follows it;
 *   - case14: the DEVICE_UP -> ADDRESS_SET -> READY notification order and the
 *     overlay payload each of those three carries;
 *   - case15: D2/§7.3 FAILED ifindex retention (non-zero for ADDRESS_SET /
 *     ADMIN_UP failures, zero for DEVICE_CONFIRM);
 *   - case16: D3 teardown-before-rebuild for the empty-ifname / same-endpoint
 *     case, with no stale overlay left behind;
 *   - case17: get_state()/wait_ready() are pure queries (no notification,
 *     event == MIDR_VLINK_EV_NONE);
 *   - case19: the GRE-layer notify subscription (D1a) fails the entry without
 *     any polling.
 *
 * The program is a miniature midrd host (same #include "midrd.c" trick as
 * gre-registry-test.c and gre-link-tool.c) so no real zebra is needed:
 *
 *   - device create/delete and the three overlay ZAPI primitives are injected
 *     with -Wl,--wrap= (see midrd/Makefile);
 *   - "zebra confirmed the device / overlay" is simulated by driving the
 *     client-side libfrr interface view itself, exactly as the contract §9.1
 *     prescribes: if_get_by_name() + if_set_index() + connected_add_by_prefix()
 *     + the IFF_UP bit.  No production test hook is used for that path, so
 *     midr_virtual_link_probe_device() / _overlay_reachable() run their real
 *     implementations (if_lookup_by_name + connected_lookup_prefix_exact +
 *     if_is_up).
 *   - the autonomous paths are exercised by pumping the very same event loop
 *     midrd uses (event_add_timer_msec + event_fetch/event_call), so the
 *     module's own establishment poll and reconcile timers run for real with
 *     no test hook.
 *
 * Every assertion checks the observable status (state / stage / last_error /
 * ifindex / iftype) or the injected call counters, never just a return value.
 */

#define main midrd_program_main
#include "midrd.c"
#undef main

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "zclient.h"
#include "qobj.h"

#include "midr-dp-backend.h"
#include "midr-gre.h"
#include "midr-virtual-link.h"

/* ------------------------------------------------------------------ *
 *  Wrapped ZAPI traffic and allocation fault injection
 * ------------------------------------------------------------------ */
static size_t gre_add_calls;
static size_t gre_del_calls;
static size_t addr_set_calls;
static size_t addr_unset_calls;
static size_t admin_up_calls;

static bool fail_gre_add;
static bool fail_addr_set;
static bool fail_admin_up;
static bool fail_next_calloc;

/*
 * Total ordering of the injected ZAPI calls.  Case 16 has to prove that a
 * rebuild tears the old link down (address unset, GRE delete) *before* the new
 * device is created; a plain per-operation counter cannot show that.
 */
static unsigned long op_seq;
static unsigned long seq_gre_add;
static unsigned long seq_gre_del;
static unsigned long seq_addr_set;
static unsigned long seq_addr_unset;
static unsigned long seq_admin_up;

enum zclient_send_status __wrap_zclient_send_gre_add(
	struct zclient *client, vrf_id_t vrf_id,
	const struct zclient_gre_if *gre)
{
	(void)client;
	(void)vrf_id;
	(void)gre;
	gre_add_calls++;
	seq_gre_add = ++op_seq;
	return fail_gre_add ? ZCLIENT_SEND_FAILURE : ZCLIENT_SEND_SUCCESS;
}

enum zclient_send_status __wrap_zclient_send_gre_delete(
	struct zclient *client, vrf_id_t vrf_id, const char *ifname)
{
	(void)client;
	(void)vrf_id;
	(void)ifname;
	gre_del_calls++;
	seq_gre_del = ++op_seq;
	return ZCLIENT_SEND_SUCCESS;
}

enum zclient_send_status __wrap_zclient_send_interface_address_set(
	struct zclient *client, vrf_id_t vrf_id,
	const struct zclient_interface_address *addr)
{
	(void)client;
	(void)vrf_id;
	(void)addr;
	addr_set_calls++;
	seq_addr_set = ++op_seq;
	return fail_addr_set ? ZCLIENT_SEND_FAILURE : ZCLIENT_SEND_SUCCESS;
}

enum zclient_send_status __wrap_zclient_send_interface_address_unset(
	struct zclient *client, vrf_id_t vrf_id,
	const struct zclient_interface_address *addr)
{
	(void)client;
	(void)vrf_id;
	(void)addr;
	addr_unset_calls++;
	seq_addr_unset = ++op_seq;
	return ZCLIENT_SEND_SUCCESS;
}

enum zclient_send_status __wrap_zclient_send_interface_admin_up(
	struct zclient *client, vrf_id_t vrf_id, const char *ifname, bool up)
{
	(void)client;
	(void)vrf_id;
	(void)ifname;
	(void)up;
	admin_up_calls++;
	seq_admin_up = ++op_seq;
	return fail_admin_up ? ZCLIENT_SEND_FAILURE : ZCLIENT_SEND_SUCCESS;
}

void *__real_calloc(size_t nmemb, size_t size);

/* Fail exactly the next calloc() issued from the linked test objects, so we
 * can drive the registry's allocation-failure path deterministically. */
void *__wrap_calloc(size_t nmemb, size_t size)
{
	if (fail_next_calloc) {
		fail_next_calloc = false;
		return NULL;
	}
	return __real_calloc(nmemb, size);
}

/* ------------------------------------------------------------------ *
 *  Small helpers
 * ------------------------------------------------------------------ */
static void inject_reset(void)
{
	fail_gre_add = false;
	fail_addr_set = false;
	fail_admin_up = false;
	fail_next_calloc = false;
}

static struct ipaddr ipv4(const char *text)
{
	struct ipaddr ia = {};

	SET_IPADDR_V4(&ia);
	assert(inet_pton(AF_INET, text, &ia.ipaddr_v4) == 1);
	return ia;
}

static struct ipaddr ipv6(const char *text)
{
	struct ipaddr ia = {};

	SET_IPADDR_V6(&ia);
	assert(inet_pton(AF_INET6, text, &ia.ipaddr_v6) == 1);
	return ia;
}

static struct midr_virtual_link_desc desc_v4(const char *ifname)
{
	struct midr_virtual_link_desc d = {};

	if (ifname)
		strlcpy(d.ifname, ifname, sizeof(d.ifname));
	d.vrf_id = VRF_DEFAULT;
	d.outer_local = ipv4("10.1.1.11");
	d.outer_remote = ipv4("10.1.1.12");
	d.overlay_local = ipv4("192.168.100.1");
	d.overlay_remote = ipv4("192.168.100.2");
	d.overlay_prefix_len = 30;
	d.mtu = 1400;
	return d;
}

/* The event loop created by host_bringup(); the autonomous-path helpers below
 * pump it exactly like midrd's own event loop. */
static struct event_loop *test_master;

/* Minimal midrd host: event loop -> TED -> zclient backend -> GRE + vlink. */
static void host_bringup(struct midr_context *ctx, const char *master_name)
{
	struct midr_ted_config config = {
		.max_events = 16,
	};

	memset(ctx, 0, sizeof(*ctx));
	ctx->node_id = 1;
	assert(midr_ted_create(&config, &ctx->ted) == 0);

	ctx->master = event_master_create(master_name);
	assert(ctx->master);
	test_master = ctx->master;

	midr_gre_init(ctx->master);
	midr_virtual_link_init(ctx->master);

	assert(midr_dp_backend_start(ctx, ctx->master, VRF_DEFAULT) == 0);
	assert(midr_dp_backend_zclient() != NULL);
	/* A non-negative socket is all the wrapped senders need. */
	midr_dp_backend_zclient()->sock = 0;
}

static void host_teardown(struct midr_context *ctx)
{
	midr_virtual_link_fini();
	midr_dp_backend_stop();
	midr_gre_fini();
	midr_ted_destroy(&ctx->ted);
}

/* ------------------------------------------------------------------ *
 *  "zebra has confirmed" simulation on the client interface view (§9.1)
 * ------------------------------------------------------------------ */
static struct interface *sim_ifp(const char *ifname)
{
	struct interface *ifp = if_get_by_name(ifname, VRF_DEFAULT, NULL);

	assert(ifp);
	return ifp;
}

/* Device present with a valid ifindex and the expected GRE iftype. */
static void sim_device_up(const char *ifname, ifindex_t idx,
			  enum zebra_iftype type)
{
	struct interface *ifp = sim_ifp(ifname);

	assert(if_set_index(ifp, idx) == 0);
	ifp->zif_type = type;
}

/* Overlay local/prefix visible on the device, IFF_UP deliberately untouched.
 * This is the state the ADDRESS_SET decision is made from: the internal helper
 * only checks that the prefix is present in the client view and ignores
 * IFF_UP. */
static void sim_overlay_address(const char *ifname, const struct ipaddr *local,
				uint8_t prefix_len)
{
	struct interface *ifp = sim_ifp(ifname);
	struct prefix p = {};

	p.family = ipaddr_family(local);
	p.prefixlen = prefix_len;
	if (IS_IPADDR_V4(local))
		p.u.prefix4 = local->ipaddr_v4;
	else
		p.u.prefix6 = local->ipaddr_v6;

	assert(connected_add_by_prefix(ifp, &p, NULL) != NULL);
}

/* Bring the link up.  if_set_flags() lives in zebra/ioctl.h (daemon side), so
 * the component test sets the public struct interface flag bit directly. */
static void sim_set_if_up(const char *ifname)
{
	struct interface *ifp = sim_ifp(ifname);

	ifp->flags |= IFF_UP;
}

/* Overlay local/prefix visible on the device AND the link IFF_UP. */
static void sim_overlay_ready(const char *ifname, const struct ipaddr *local,
			      uint8_t prefix_len)
{
	sim_overlay_address(ifname, local, prefix_len);
	sim_set_if_up(ifname);
}

/* Kernel device gone: invalid ifindex, not IFF_UP. */
static void sim_device_gone(const char *ifname)
{
	struct interface *ifp = if_lookup_by_name(ifname, VRF_DEFAULT);

	if (!ifp)
		return;
	assert(if_set_index(ifp, IFINDEX_INTERNAL) == 0);
	ifp->flags &= ~(uint64_t)IFF_UP;
	ifp->zif_type = ZEBRA_IF_OTHER;
}

static void assert_failed(const struct midr_virtual_link_status *st,
			  enum midr_virtual_link_stage stage, int err)
{
	assert(st->state == MIDR_VLINK_FAILED);
	assert(st->stage == stage);
	assert(st->last_error == err);
}

static void assert_ready(const struct midr_virtual_link_status *st,
			 ifindex_t ifindex)
{
	assert(st->state == MIDR_VLINK_READY);
	assert(st->stage == MIDR_VLINK_STAGE_NONE);
	assert(st->ifindex == ifindex);
	assert(st->iftype == ZEBRA_IF_GRE || st->iftype == ZEBRA_IF_IP6GRE);
}


/* ------------------------------------------------------------------ *
 *  Notification recorder (§9.2 case 12, D1, W0 event ordering)
 * ------------------------------------------------------------------ */
#define NOTIFY_SEQ_MAX 32

static size_t notify_total;
static size_t notify_device_up;
static size_t notify_addr_set;
static size_t notify_ready;
static size_t notify_failed;
static size_t notify_down;
static enum midr_virtual_link_state notify_last_state;
static enum midr_virtual_link_event notify_last_event;

/* Exact notification order, plus a copy of the status each one carried (the
 * pointer handed to the callback is only valid for the duration of the call). */
static enum midr_virtual_link_event notify_seq[NOTIFY_SEQ_MAX];
static struct midr_virtual_link_status notify_snap[NOTIFY_SEQ_MAX];
static size_t notify_seq_n;
/* Whether the device was IFF_UP when ADDRESS_SET was emitted. */
static bool notify_addr_set_if_up;

static void notify_reset(void)
{
	notify_total = 0;
	notify_device_up = 0;
	notify_addr_set = 0;
	notify_ready = 0;
	notify_failed = 0;
	notify_down = 0;
	notify_last_state = MIDR_VLINK_DOWN;
	notify_last_event = MIDR_VLINK_EV_NONE;
	notify_seq_n = 0;
	notify_addr_set_if_up = true;
}

static void notify_cb(const struct midr_virtual_link_status *st, void *arg)
{
	(void)arg;
	notify_total++;
	notify_last_state = st->state;
	notify_last_event = st->event;

	if (notify_seq_n < NOTIFY_SEQ_MAX) {
		notify_seq[notify_seq_n] = st->event;
		notify_snap[notify_seq_n] = *st;
		notify_seq_n++;
	}

	if (st->event == MIDR_VLINK_EV_ADDRESS_SET) {
		struct interface *ifp = if_lookup_by_name(st->ifname,
							  st->vrf_id);

		notify_addr_set_if_up = ifp ? if_is_up(ifp) : false;
	}

	switch (st->event) {
	case MIDR_VLINK_EV_DEVICE_UP:
		notify_device_up++;
		break;
	case MIDR_VLINK_EV_ADDRESS_SET:
		notify_addr_set++;
		break;
	case MIDR_VLINK_EV_READY:
		notify_ready++;
		break;
	case MIDR_VLINK_EV_FAILED:
		notify_failed++;
		break;
	case MIDR_VLINK_EV_DOWN:
		notify_down++;
		break;
	case MIDR_VLINK_EV_NONE:
	default:
		break;
	}
}

/* ------------------------------------------------------------------ *
 *  Event-loop pumping for the autonomous (timer-driven) paths
 * ------------------------------------------------------------------ */
static uint64_t sim_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sim_tick_cb(struct event *t)
{
	(void)t;
}

/*
 * Pump the midrd event loop for about @ms milliseconds without touching any
 * link state, so the module's own timers (the per-entry establishment poll and
 * the module-wide reconcile timer, MIDR_VLINK_RECONCILE_INTERVAL_MS = 1000) run
 * exactly as they would inside midrd.  event_fetch() blocks until the next
 * timer is due, so a 20 ms wake tick per iteration keeps the loop honest while
 * still letting the 1000 ms reconcile timer fire.
 */
static void pump_events(uint32_t ms)
{
	uint64_t deadline = sim_now_ms() + ms;

	assert(test_master);
	while (sim_now_ms() < deadline) {
		struct event *wake = NULL;
		struct event ev = {};

		event_add_timer_msec(test_master, sim_tick_cb, NULL, 20, &wake);
		event_fetch(test_master, &ev);
		event_call(&ev);
		event_cancel(&wake);
	}
}

/* Pump until the next FAILED notification, or @max_ms elapsed.  This is the
 * only thing driving the state machine: no get_state() call is made. */
static bool pump_until_notify_failed(uint32_t max_ms)
{
	uint64_t deadline = sim_now_ms() + max_ms;

	while (sim_now_ms() < deadline) {
		if (notify_failed > 0)
			return true;
		pump_events(20);
	}

	return notify_failed > 0;
}

/* ================================================================== *
 *  §9.2 case 1 — missing outer endpoint
 * ================================================================== */
static void test_case01_missing_outer_endpoint(void)
{
	struct midr_context ctx;
	struct midr_virtual_link_status st;
	struct midr_virtual_link_desc d;

	inject_reset();
	host_bringup(&ctx, "vlink-test-01");

	/* outer_local missing */
	d = desc_v4("vl01a");
	SET_IPADDR_NONE(&d.outer_local);
	assert(midr_virtual_link_add(&ctx, &d, &st) == -1);
	assert_failed(&st, MIDR_VLINK_STAGE_VALIDATE, EINVAL);

	/* outer_remote missing */
	d = desc_v4("vl01b");
	SET_IPADDR_NONE(&d.outer_remote);
	assert(midr_virtual_link_add(&ctx, &d, &st) == -1);
	assert_failed(&st, MIDR_VLINK_STAGE_VALIDATE, EINVAL);

	/* nothing must have reached zebra */
	assert(gre_add_calls == 0);
	host_teardown(&ctx);
}

/* ================================================================== *
 *  §9.2 case 2 — outer endpoint family mismatch
 * ================================================================== */
static void test_case02_outer_family_mismatch(void)
{
	struct midr_context ctx;
	struct midr_virtual_link_status st;
	struct midr_virtual_link_desc d = desc_v4("vl02");

	inject_reset();
	host_bringup(&ctx, "vlink-test-02");

	d.outer_remote = ipv6("fd00:1::12");
	assert(midr_virtual_link_add(&ctx, &d, &st) == -1);
	assert_failed(&st, MIDR_VLINK_STAGE_VALIDATE, EAFNOSUPPORT);
	assert(gre_add_calls == 0);

	host_teardown(&ctx);
}

/* ================================================================== *
 *  §9.2 case 3 — illegal overlay address / prefix
 * ================================================================== */
static void test_case03_overlay_invalid(void)
{
	struct midr_context ctx;
	struct midr_virtual_link_status st;
	struct midr_virtual_link_desc d;

	inject_reset();
	host_bringup(&ctx, "vlink-test-03");

	/* prefix length out of range for the family */
	d = desc_v4("vl03a");
	d.overlay_prefix_len = 33;
	assert(midr_virtual_link_add(&ctx, &d, &st) == -1);
	assert_failed(&st, MIDR_VLINK_STAGE_VALIDATE, EINVAL);

	d.overlay_prefix_len = 0;
	assert(midr_virtual_link_add(&ctx, &d, &st) == -1);
	assert_failed(&st, MIDR_VLINK_STAGE_VALIDATE, EINVAL);

	/* overlay_remote outside overlay_local/prefix */
	d = desc_v4("vl03b");
	d.overlay_remote = ipv4("192.168.100.200");
	assert(midr_virtual_link_add(&ctx, &d, &st) == -1);
	assert_failed(&st, MIDR_VLINK_STAGE_VALIDATE, EINVAL);

	/* overlay family mismatch */
	d = desc_v4("vl03c");
	d.overlay_remote = ipv6("fd00:100::2");
	assert(midr_virtual_link_add(&ctx, &d, &st) == -1);
	assert_failed(&st, MIDR_VLINK_STAGE_VALIDATE, EAFNOSUPPORT);

	assert(gre_add_calls == 0);
	host_teardown(&ctx);
}


/* ================================================================== *
 *  §9.2 case 4 — zclient_send_gre_add fails
 * ================================================================== */
static void test_case04_gre_create_failure(void)
{
	struct midr_context ctx;
	struct midr_virtual_link_status st;
	struct midr_virtual_link_desc d = desc_v4("vl04");

	inject_reset();
	host_bringup(&ctx, "vlink-test-04");

	fail_gre_add = true;
	assert(midr_virtual_link_add(&ctx, &d, &st) == -1);
	assert_failed(&st, MIDR_VLINK_STAGE_GRE_CREATE, EIO);
	assert(gre_add_calls == 1);
	assert(gre_del_calls == 0); /* nothing to roll back */

	/* The same descriptor succeeds once zebra accepts it (idempotent retry). */
	fail_gre_add = false;
	assert(midr_virtual_link_add(&ctx, &d, &st) == 0);
	assert(midr_virtual_link_get_state(&ctx, "vl04", &st) == 0);
	assert(st.state == MIDR_VLINK_CREATING);
	assert(gre_add_calls == 2);

	host_teardown(&ctx);
}

/* ================================================================== *
 *  §9.2 case 5 — overlay address-set fails → rollback device
 * ================================================================== */
static void test_case05_address_set_failure(void)
{
	struct midr_context ctx;
	struct midr_virtual_link_status st;
	struct midr_virtual_link_desc d = desc_v4("vl05");

	inject_reset();
	host_bringup(&ctx, "vlink-test-05");
	sim_device_up("vl05", 51, ZEBRA_IF_GRE);
	/* overlay not yet present: the SET is what fails */

	fail_addr_set = true;
	assert(midr_virtual_link_add(&ctx, &d, &st) == -1);
	assert_failed(&st, MIDR_VLINK_STAGE_ADDRESS_SET, EIO);
	assert(addr_set_calls == 1);
	/* device rolled back, address never staged → no unset */
	assert(gre_del_calls == 1);
	assert(addr_unset_calls == 0);
	/* D2/§7.3: a configuration failure keeps the last confirmed ifindex;
	 * only DEVICE_CONFIRM ("the device was never there") zeroes it. */
	assert(st.ifindex == 51);

	host_teardown(&ctx);
}

/* ================================================================== *
 *  §9.2 case 6 — admin-up fails → rollback address AND device
 * ================================================================== */
static void test_case06_admin_up_failure(void)
{
	struct midr_context ctx;
	struct midr_virtual_link_status st;
	struct midr_virtual_link_desc d = desc_v4("vl06");
	size_t s0, n0, u0, d0;

	inject_reset();
	host_bringup(&ctx, "vlink-test-06");
	sim_device_up("vl06", 52, ZEBRA_IF_GRE);

	s0 = addr_set_calls;
	n0 = admin_up_calls;
	u0 = addr_unset_calls;
	d0 = gre_del_calls;

	fail_admin_up = true;
	assert(midr_virtual_link_add(&ctx, &d, &st) == -1);
	assert_failed(&st, MIDR_VLINK_STAGE_ADMIN_UP, EIO);
	assert(addr_set_calls == s0 + 1);
	assert(admin_up_calls == n0 + 1);
	/* the address was staged, so rollback must withdraw it, then del device */
	assert(addr_unset_calls == u0 + 1);
	assert(gre_del_calls == d0 + 1);
	/* D2/§7.3: ADMIN_UP is not a device-loss stage, so the confirmed
	 * ifindex is retained. */
	assert(st.ifindex == 52);

	host_teardown(&ctx);
}

/* ================================================================== *
 *  §9.2 case 7 — ifindex never confirmed → DEVICE_CONFIRM / ETIMEDOUT
 * ================================================================== */
static void test_case07_device_confirm_timeout(void)
{
	struct midr_context ctx;
	struct midr_virtual_link_status st;
	struct midr_virtual_link_desc d = desc_v4("vl07");
	size_t d0;

	inject_reset();
	host_bringup(&ctx, "vlink-test-07");
	/* deliberately create no client interface view */

	d0 = gre_del_calls;
	assert(midr_virtual_link_add(&ctx, &d, &st) == 0);
	assert(st.state == MIDR_VLINK_CREATING);

	/* wait_ready pumps the event loop past the 3000 ms confirm timeout */
	assert(midr_virtual_link_wait_ready(&ctx, "vl07", 3500, &st) == false);
	assert_failed(&st, MIDR_VLINK_STAGE_DEVICE_CONFIRM, ETIMEDOUT);
	/* D2/§7.3: DEVICE_CONFIRM means the device was never confirmed, so
	 * there is no ifindex to retain. */
	assert(st.ifindex == 0);
	/* the unconfirmed device is recycled */
	assert(gre_del_calls == d0 + 1);

	host_teardown(&ctx);
}


/* ================================================================== *
 *  §9.2 case 8 — duplicate add of the same descriptor is idempotent
 * ================================================================== */
static void test_case08_idempotent_add(void)
{
	struct midr_context ctx;
	struct midr_virtual_link_status st;
	struct midr_virtual_link_desc d = desc_v4("vl08");
	ifindex_t saved_idx;
	size_t saved_add;
	size_t saved_set;
	size_t a0, s0;

	inject_reset();
	host_bringup(&ctx, "vlink-test-08");
	sim_device_up("vl08", 53, ZEBRA_IF_GRE);
	sim_overlay_ready("vl08", &d.overlay_local, d.overlay_prefix_len);

	a0 = gre_add_calls;
	s0 = addr_set_calls;

	assert(midr_virtual_link_add(&ctx, &d, &st) == 0);
	assert(midr_virtual_link_wait_ready(&ctx, "vl08", 500, &st) == true);
	assert_ready(&st, 53);
	assert(gre_add_calls == a0 + 1);
	assert(addr_set_calls == s0 + 1);

	saved_idx = st.ifindex;
	saved_add = gre_add_calls;
	saved_set = addr_set_calls;

	/* identical descriptor again: accepted, no second ZAPI */
	assert(midr_virtual_link_add(&ctx, &d, &st) == 0);
	assert(st.state == MIDR_VLINK_READY);
	assert(st.ifindex == saved_idx);
	assert(gre_add_calls == saved_add);
	assert(addr_set_calls == saved_set);

	host_teardown(&ctx);
}

/* ================================================================== *
 *  §9.2 case 9 — same ifname, different endpoint, no REBIND → EEXIST
 * ================================================================== */
static void test_case09_same_name_rejected(void)
{
	struct midr_context ctx;
	struct midr_virtual_link_status st;
	struct midr_virtual_link_desc d = desc_v4("vl09");
	struct midr_virtual_link_desc d2;

	inject_reset();
	host_bringup(&ctx, "vlink-test-09");
	sim_device_up("vl09", 54, ZEBRA_IF_GRE);
	sim_overlay_ready("vl09", &d.overlay_local, d.overlay_prefix_len);

	assert(midr_virtual_link_add(&ctx, &d, &st) == 0);
	assert(midr_virtual_link_wait_ready(&ctx, "vl09", 500, &st) == true);
	assert_ready(&st, 54);

	/* same name, different outer endpoint, default flags */
	d2 = desc_v4("vl09");
	d2.outer_remote = ipv4("10.1.1.99");
	assert(midr_virtual_link_add(&ctx, &d2, &st) == -1);
	assert_failed(&st, MIDR_VLINK_STAGE_VALIDATE, EEXIST);
	/* the established link is untouched */
	assert(midr_virtual_link_get_state(&ctx, "vl09", &st) == 0);
	assert(st.state == MIDR_VLINK_READY);
	assert(st.ifindex == 54);

	host_teardown(&ctx);
}

/* ================================================================== *
 *  §9.2 case 10 — same ifname, different endpoint, REBIND → delete+create
 * ================================================================== */
static void test_case10_rebind(void)
{
	struct midr_context ctx;
	struct midr_virtual_link_status st;
	struct midr_virtual_link_desc d = desc_v4("vl10");
	struct midr_virtual_link_desc d2;
	size_t del_before;
	size_t a0;

	inject_reset();
	host_bringup(&ctx, "vlink-test-10");
	sim_device_up("vl10", 55, ZEBRA_IF_GRE);
	sim_overlay_ready("vl10", &d.overlay_local, d.overlay_prefix_len);

	a0 = gre_add_calls;
	assert(midr_virtual_link_add(&ctx, &d, &st) == 0);
	assert(midr_virtual_link_wait_ready(&ctx, "vl10", 500, &st) == true);
	assert_ready(&st, 55);

	del_before = gre_del_calls;

	/* same name, new outer remote, explicit rebind */
	d2 = desc_v4("vl10");
	d2.outer_remote = ipv4("10.1.1.88");
	d2.flags = MIDR_VLINK_F_REBIND;
	assert(midr_virtual_link_add(&ctx, &d2, &st) == 0);
	assert(midr_virtual_link_wait_ready(&ctx, "vl10", 500, &st) == true);
	assert_ready(&st, 55);
	/* the old binding was explicitly removed first */
	assert(gre_del_calls == del_before + 1);
	assert(gre_add_calls == a0 + 2);

	host_teardown(&ctx);
}


/* ================================================================== *
 *  §9.2 case 11 — delete then rebuild yields a fresh ifindex
 * ================================================================== */
static void test_case11_delete_rebuild(void)
{
	struct midr_context ctx;
	struct midr_virtual_link_status st;
	struct midr_virtual_link_desc d = desc_v4("vl11");
	size_t u0, d0;

	inject_reset();
	host_bringup(&ctx, "vlink-test-11");
	sim_device_up("vl11", 61, ZEBRA_IF_GRE);
	sim_overlay_ready("vl11", &d.overlay_local, d.overlay_prefix_len);

	assert(midr_virtual_link_add(&ctx, &d, &st) == 0);
	assert(midr_virtual_link_wait_ready(&ctx, "vl11", 500, &st) == true);
	assert_ready(&st, 61);

	u0 = addr_unset_calls;
	d0 = gre_del_calls;

	/* delete: overlay address withdrawn, device removed, entry gone */
	assert(midr_virtual_link_del(&ctx, "vl11", &st) == 0);
	assert(st.state == MIDR_VLINK_DOWN);
	assert(addr_unset_calls == u0 + 1);
	assert(gre_del_calls == d0 + 1);
	assert(midr_virtual_link_get_state(&ctx, "vl11", &st) == -1);

	/* rebuild with a new ifindex */
	sim_device_up("vl11", 71, ZEBRA_IF_GRE);
	assert(midr_virtual_link_add(&ctx, &d, &st) == 0);
	assert(midr_virtual_link_wait_ready(&ctx, "vl11", 500, &st) == true);
	assert_ready(&st, 71);

	host_teardown(&ctx);
}

/* ================================================================== *
 *  §9.2 case 12 — device disappears, ifindex changes, desc retained
 * ================================================================== */
static void test_case12_device_loss_and_ifindex_change(void)
{
	struct midr_context ctx;
	struct midr_virtual_link_status st;
	struct midr_virtual_link_desc d = desc_v4("vl12");

	inject_reset();
	host_bringup(&ctx, "vlink-test-12");
	sim_device_up("vl12", 81, ZEBRA_IF_GRE);
	sim_overlay_ready("vl12", &d.overlay_local, d.overlay_prefix_len);

	assert(midr_virtual_link_add(&ctx, &d, &st) == 0);
	assert(midr_virtual_link_wait_ready(&ctx, "vl12", 500, &st) == true);
	assert_ready(&st, 81);

	notify_reset();
	midr_virtual_link_register_notify(notify_cb, NULL);

	/* zebra loses the device.  D1: the module now reports this on its own
	 * (the module-wide reconcile timer), so the test must NOT call
	 * get_state() to learn about it -- it only pumps the event loop. */
	sim_device_gone("vl12");
	assert(pump_until_notify_failed(2000) == true);
	assert(notify_failed == 1);
	assert(notify_last_event == MIDR_VLINK_EV_FAILED);

	assert(midr_virtual_link_get_state(&ctx, "vl12", &st) == 0);
	assert_failed(&st, MIDR_VLINK_STAGE_DEVICE_CONFIRM, ENODEV);
	assert(st.ifindex == 0);
	assert(st.event == MIDR_VLINK_EV_NONE);

	/* the descriptor is retained: a rebuild returns a fresh ifindex and READY */
	sim_device_up("vl12", 91, ZEBRA_IF_GRE);
	sim_overlay_ready("vl12", &d.overlay_local, d.overlay_prefix_len);
	assert(midr_virtual_link_add(&ctx, &d, &st) == 0);
	assert(midr_virtual_link_wait_ready(&ctx, "vl12", 500, &st) == true);
	assert_ready(&st, 91);
	assert(notify_ready >= 1);

	midr_virtual_link_unregister_notify(notify_cb);
	host_teardown(&ctx);
}

/* ================================================================== *
 *  D1b (case 13) — autonomous device loss via the module-wide
 *  reconcile timer, without any get_state() call
 * ================================================================== */
static void test_case13_autonomous_device_loss(void)
{
	struct midr_context ctx;
	struct midr_virtual_link_status st;
	struct midr_virtual_link_desc d = desc_v4("vl13");
	uint64_t t0;
	size_t a0;

	inject_reset();
	host_bringup(&ctx, "vlink-test-13");
	sim_device_up("vl13", 131, ZEBRA_IF_GRE);
	sim_overlay_ready("vl13", &d.overlay_local, d.overlay_prefix_len);

	notify_reset();
	midr_virtual_link_register_notify(notify_cb, NULL);

	a0 = gre_add_calls;
	t0 = sim_now_ms();
	assert(midr_virtual_link_add(&ctx, &d, &st) == 0);
	assert(midr_virtual_link_wait_ready(&ctx, "vl13", 500, &st) == true);
	assert_ready(&st, 131);
	assert(notify_failed == 0);
	assert(gre_add_calls == a0 + 1);

	/* zebra loses the device; nothing here calls get_state(). */
	sim_device_gone("vl13");

	/* Only the module's own 1000 ms reconcile timer can have noticed: the
	 * GRE layer was never told, so this is D1b and not D1a. */
	assert(pump_until_notify_failed(2000) == true);
	assert(sim_now_ms() - t0 >= 1000); /* MIDR_VLINK_RECONCILE_INTERVAL_MS */
	assert(notify_failed == 1);
	assert(notify_last_event == MIDR_VLINK_EV_FAILED);
	assert(notify_seq_n >= 1);
	assert(notify_seq[notify_seq_n - 1] == MIDR_VLINK_EV_FAILED);

	/* FAILED(DEVICE_CONFIRM) invalidates the ifindex but keeps the entry */
	assert(midr_virtual_link_get_state(&ctx, "vl13", &st) == 0);
	assert_failed(&st, MIDR_VLINK_STAGE_DEVICE_CONFIRM, ENODEV);
	assert(st.ifindex == 0);
	assert(st.event == MIDR_VLINK_EV_NONE);

	/* descriptor retained: the same add() rebuilds with a fresh ifindex */
	sim_device_up("vl13", 132, ZEBRA_IF_GRE);
	sim_overlay_ready("vl13", &d.overlay_local, d.overlay_prefix_len);
	assert(midr_virtual_link_add(&ctx, &d, &st) == 0);
	assert(midr_virtual_link_wait_ready(&ctx, "vl13", 500, &st) == true);
	assert_ready(&st, 132);
	assert(gre_add_calls == a0 + 2);
	assert(st.overlay_prefix_len == d.overlay_prefix_len);
	assert(ipaddr_cmp(&st.overlay_local, &d.overlay_local) == 0);
	assert(ipaddr_cmp(&st.overlay_remote, &d.overlay_remote) == 0);

	midr_virtual_link_unregister_notify(notify_cb);
	host_teardown(&ctx);
}

/* ================================================================== *
 *  W0/§5.1 (case 14) — DEVICE_UP -> ADDRESS_SET -> READY order and
 *  the status payload each notification carries
 * ================================================================== */
static void test_case14_event_ordering_and_overlay_payload(void)
{
	struct midr_context ctx;
	struct midr_virtual_link_status st;
	struct midr_virtual_link_desc d = desc_v4("vl14");

	inject_reset();
	host_bringup(&ctx, "vlink-test-14");

	/* Device + connected overlay prefix, but deliberately NOT IFF_UP yet:
	 * ADDRESS_SET must fire here (the module ignores IFF_UP for it) and
	 * READY must not. */
	sim_device_up("vl14", 141, ZEBRA_IF_GRE);
	sim_overlay_address("vl14", &d.overlay_local, d.overlay_prefix_len);

	notify_reset();
	midr_virtual_link_register_notify(notify_cb, NULL);

	assert(midr_virtual_link_add(&ctx, &d, &st) == 0);

	/* add() drives the synchronous part of the state machine: two separate
	 * notifications, in this exact order. */
	assert(notify_seq_n == 2);
	assert(notify_seq[0] == MIDR_VLINK_EV_DEVICE_UP);
	assert(notify_seq[1] == MIDR_VLINK_EV_ADDRESS_SET);
	assert(notify_ready == 0);
	assert(notify_addr_set == 1);

	/* DEVICE_UP carries the confirmed ifindex; overlay is not ready. */
	assert(notify_snap[0].state == MIDR_VLINK_DEVICE_UP);
	assert(notify_snap[0].ifindex == 141);
	assert(notify_snap[0].overlay_ready == false);

	/* ADDRESS_SET: overlay visible, IFF_UP deliberately still unset. */
	assert(notify_snap[1].state == MIDR_VLINK_CONFIGURING);
	assert(notify_snap[1].overlay_ready == false);
	assert(notify_snap[1].ifindex == 141);
	assert(notify_snap[1].overlay_prefix_len == d.overlay_prefix_len);
	assert(ipaddr_cmp(&notify_snap[1].overlay_local, &d.overlay_local) == 0);
	assert(ipaddr_cmp(&notify_snap[1].overlay_remote,
			  &d.overlay_remote) == 0);
	assert(notify_addr_set_if_up == false);

	/* still CONFIGURING, and a query reports no transition of its own */
	assert(midr_virtual_link_get_state(&ctx, "vl14", &st) == 0);
	assert(st.state == MIDR_VLINK_CONFIGURING);
	assert(st.overlay_ready == false);
	assert(st.event == MIDR_VLINK_EV_NONE);

	/* now the link comes up: READY is the third and last transition */
	sim_set_if_up("vl14");
	assert(midr_virtual_link_wait_ready(&ctx, "vl14", 500, &st) == true);
	assert_ready(&st, 141);
	assert(notify_seq_n == 3);
	assert(notify_seq[2] == MIDR_VLINK_EV_READY);
	assert(notify_ready == 1);
	assert(notify_addr_set == 1);
	assert(notify_snap[2].state == MIDR_VLINK_READY);
	assert(notify_snap[2].overlay_ready == true);
	assert(notify_snap[2].ifindex == 141);
	assert(notify_snap[2].overlay_prefix_len == d.overlay_prefix_len);
	assert(ipaddr_cmp(&notify_snap[2].overlay_local, &d.overlay_local) == 0);
	assert(ipaddr_cmp(&notify_snap[2].overlay_remote,
			  &d.overlay_remote) == 0);

	midr_virtual_link_unregister_notify(notify_cb);
	host_teardown(&ctx);
}

/* ================================================================== *
 *  D2/§7.3 (case 15) — FAILED carries the last confirmed ifindex for
 *  every stage except DEVICE_CONFIRM
 * ================================================================== */
static void test_case15_failed_ifindex_retention(void)
{
	struct midr_context ctx;
	struct midr_virtual_link_status st;
	struct midr_virtual_link_desc d = desc_v4("vl15a");
	struct midr_virtual_link_desc d2 = desc_v4("vl15b");
	size_t u0, d0, s0;

	inject_reset();
	host_bringup(&ctx, "vlink-test-15");

	/* ADDRESS_SET failure: the device WAS confirmed (ifindex 151). */
	sim_device_up("vl15a", 151, ZEBRA_IF_GRE);
	u0 = addr_unset_calls;
	d0 = gre_del_calls;
	s0 = addr_set_calls;
	fail_addr_set = true;
	assert(midr_virtual_link_add(&ctx, &d, &st) == -1);
	assert_failed(&st, MIDR_VLINK_STAGE_ADDRESS_SET, EIO);
	assert(st.ifindex == 151);
	assert(addr_set_calls == s0 + 1);
	assert(addr_unset_calls == u0); /* the SET never staged an address */
	assert(gre_del_calls == d0 + 1); /* the device is still rolled back */
	fail_addr_set = false;

	/* ADMIN_UP failure: address staged then withdrawn, ifindex retained.
	 * A different outer pair keeps the two sub-cases independent. */
	d2.outer_remote = ipv4("10.1.1.13");
	sim_device_up("vl15b", 152, ZEBRA_IF_GRE);
	u0 = addr_unset_calls;
	d0 = gre_del_calls;
	s0 = addr_set_calls;
	fail_admin_up = true;
	assert(midr_virtual_link_add(&ctx, &d2, &st) == -1);
	assert_failed(&st, MIDR_VLINK_STAGE_ADMIN_UP, EIO);
	assert(st.ifindex == 152);
	assert(addr_set_calls == s0 + 1);
	assert(addr_unset_calls == u0 + 1);
	assert(gre_del_calls == d0 + 1);
	fail_admin_up = false;

	host_teardown(&ctx);
}

/* ================================================================== *
 *  D3 (case 16a) — empty-ifname rebuild with the same outer endpoint
 *  pair but a different overlay: full teardown first, no stale overlay
 * ================================================================== */
static void test_case16a_empty_name_rebuild_no_stale_overlay(void)
{
	struct midr_context ctx;
	struct midr_virtual_link_status st;
	struct midr_virtual_link_desc d1 = desc_v4(NULL);
	struct midr_virtual_link_desc d2 = desc_v4(NULL);
	char auto1[IFNAMSIZ];
	char auto2[IFNAMSIZ];
	const char *name;
	size_t a0, d0, u0, s0;
	unsigned long seq0;

	inject_reset();
	host_bringup(&ctx, "vlink-test-16a");

	/* (a) first link: empty ifname -> auto-generated device name */
	assert(midr_virtual_link_add(&ctx, &d1, &st) == 0);
	assert(st.state == MIDR_VLINK_CREATING);
	name = midr_gre_interface_name(VRF_DEFAULT, &d1.outer_local,
				       &d1.outer_remote);
	assert(name != NULL);
	strlcpy(auto1, name, sizeof(auto1)); /* the GRE entry dies below */
	sim_device_up(auto1, 161, ZEBRA_IF_GRE);
	sim_overlay_ready(auto1, &d1.overlay_local, d1.overlay_prefix_len);
	assert(midr_virtual_link_wait_ready(&ctx, auto1, 500, &st) == true);
	assert_ready(&st, 161);
	assert(st.overlay_prefix_len == d1.overlay_prefix_len);

	/* same outer endpoint pair + DIFFERENT overlay, still empty ifname:
	 * the old link must be torn down (address unset, device deleted)
	 * BEFORE the new device is created, and nothing of the old overlay
	 * may survive into the new link's status. */
	d2.overlay_local = ipv4("192.168.200.1");
	d2.overlay_remote = ipv4("192.168.200.2");
	u0 = addr_unset_calls;
	d0 = gre_del_calls;
	a0 = gre_add_calls;
	s0 = addr_set_calls;
	seq0 = op_seq;

	assert(midr_virtual_link_add(&ctx, &d2, &st) == 0);
	assert(addr_unset_calls == u0 + 1);
	assert(gre_del_calls == d0 + 1);
	assert(gre_add_calls == a0 + 1);
	/* the wrap sequence proves the order: unset -> delete -> create */
	assert(seq_addr_unset > seq0 && seq_gre_del > seq_addr_unset);
	assert(seq_gre_add > seq_gre_del);

	/* the returned status already describes only the NEW overlay */
	assert(st.state == MIDR_VLINK_CREATING);
	assert(st.overlay_ready == false);
	assert(st.overlay_prefix_len == d2.overlay_prefix_len);
	assert(ipaddr_cmp(&st.overlay_local, &d2.overlay_local) == 0);
	assert(ipaddr_cmp(&st.overlay_remote, &d2.overlay_remote) == 0);

	name = midr_gre_interface_name(VRF_DEFAULT, &d2.outer_local,
				       &d2.outer_remote);
	assert(name != NULL);
	strlcpy(auto2, name, sizeof(auto2));
	assert(strcmp(auto1, auto2) != 0); /* the old device name is gone */
	sim_device_up(auto2, 162, ZEBRA_IF_GRE);
	sim_overlay_ready(auto2, &d2.overlay_local, d2.overlay_prefix_len);
	assert(midr_virtual_link_wait_ready(&ctx, auto2, 500, &st) == true);
	assert_ready(&st, 162);
	assert(ipaddr_cmp(&st.overlay_local, &d2.overlay_local) == 0);
	assert(ipaddr_cmp(&st.overlay_remote, &d2.overlay_remote) == 0);
	/* the new link's own sequence is create -> set -> admin-up */
	assert(seq_addr_set > seq_gre_add);
	assert(seq_admin_up > seq_addr_set);

	/* (b) the identical empty-ifname descriptor stays idempotent */
	assert(midr_virtual_link_add(&ctx, &d2, &st) == 0);
	assert(st.state == MIDR_VLINK_READY);
	assert(st.ifindex == 162);
	assert(ipaddr_cmp(&st.overlay_local, &d2.overlay_local) == 0);
	assert(addr_unset_calls == u0 + 1);
	assert(gre_add_calls == a0 + 1);
	assert(addr_set_calls == s0 + 1);

	host_teardown(&ctx);
}

/* ================================================================== *
 *  D3 (case 16b) — a live foreign GRE entry for the pair under another
 *  name is refused with VALIDATE/EEXIST before any create
 * ================================================================== */
static void test_case16b_empty_name_live_foreign_pair_refused(void)
{
	struct midr_context ctx;
	struct midr_virtual_link_status st;
	struct midr_virtual_link_desc d = desc_v4(NULL);
	struct midr_gre_tunnel ft = {};
	struct midr_gre_status fst;
	size_t a0, d0, u0;

	inject_reset();
	host_bringup(&ctx, "vlink-test-16b");

	/* A tunnel owned by somebody else: same endpoint pair, live state. */
	d.outer_local = ipv4("10.1.1.21");
	d.outer_remote = ipv4("10.1.1.22");
	ft.vrf_id = VRF_DEFAULT;
	ft.local = d.outer_local;
	ft.remote = d.outer_remote;
	strlcpy(ft.ifname, "foreign-gre", sizeof(ft.ifname));
	assert(midr_gre_interface_add(&ctx, &ft, &fst) == 0);
	assert(fst.state == MIDR_GRE_STATE_PENDING); /* live, not DOWN/FAILED */

	a0 = gre_add_calls;
	d0 = gre_del_calls;
	u0 = addr_unset_calls;

	/* The auto-name add must refuse instead of silently taking over the
	 * device (the name it would generate is not the live one) and report
	 * the foreign name so the caller knows what to tear down. */
	assert(midr_virtual_link_add(&ctx, &d, &st) == -1);
	assert_failed(&st, MIDR_VLINK_STAGE_VALIDATE, EEXIST);
	assert(strcmp(st.ifname, "foreign-gre") == 0);
	assert(gre_add_calls == a0); /* refused before any create */
	assert(gre_del_calls == d0); /* and without touching the device */
	assert(addr_unset_calls == u0);
	assert(midr_virtual_link_get_state(&ctx, "foreign-gre", &st) == -1);

	host_teardown(&ctx);
}

/* ================================================================== *
 *  §5.4/§7.4 (case 17) — get_state() and wait_ready() are pure
 *  queries: no notification, event == MIDR_VLINK_EV_NONE
 * ================================================================== */
static void test_case17_query_paths_do_not_notify(void)
{
	struct midr_context ctx;
	struct midr_virtual_link_status st;
	struct midr_virtual_link_desc d = desc_v4("vl17");
	struct midr_virtual_link_desc d2 = desc_v4("vl17b");
	size_t n0;

	inject_reset();
	host_bringup(&ctx, "vlink-test-17");
	sim_device_up("vl17", 171, ZEBRA_IF_GRE);
	sim_overlay_ready("vl17", &d.overlay_local, d.overlay_prefix_len);

	assert(midr_virtual_link_add(&ctx, &d, &st) == 0);
	assert(midr_virtual_link_wait_ready(&ctx, "vl17", 500, &st) == true);
	assert_ready(&st, 171);
	assert(st.event == MIDR_VLINK_EV_NONE);

	/* A second link whose device never appears, still under construction. */
	d2.outer_remote = ipv4("10.1.1.14");
	assert(midr_virtual_link_add(&ctx, &d2, &st) == 0);
	assert(st.state == MIDR_VLINK_CREATING);

	notify_reset();
	midr_virtual_link_register_notify(notify_cb, NULL);
	n0 = notify_total;

	/* get_state() on a READY link: a pure query */
	assert(midr_virtual_link_get_state(&ctx, "vl17", &st) == 0);
	assert(st.state == MIDR_VLINK_READY);
	assert(st.overlay_ready == true);
	assert(st.event == MIDR_VLINK_EV_NONE);
	assert(notify_total == n0);

	/* get_state() on a CREATING link: still pure, and it must not drive
	 * the state machine towards DEVICE_UP either */
	assert(midr_virtual_link_get_state(&ctx, "vl17b", &st) == 0);
	assert(st.state == MIDR_VLINK_CREATING);
	assert(st.ifindex == 0);
	assert(st.event == MIDR_VLINK_EV_NONE);
	assert(notify_total == n0);

	/* wait_ready() on a terminal (READY) link: a pure query as well */
	assert(midr_virtual_link_wait_ready(&ctx, "vl17", 500, &st) == true);
	assert(st.state == MIDR_VLINK_READY);
	assert(st.event == MIDR_VLINK_EV_NONE);
	assert(notify_total == n0);

	midr_virtual_link_unregister_notify(notify_cb);
	host_teardown(&ctx);
}

/* ================================================================== *
 *  D4 (case 18) — after an autonomous failure, a retry + delete does
 *  exactly one create and one delete (no stale gre_created/addr_set)
 * ================================================================== */
static void test_case18_no_stale_bookkeeping_after_loss(void)
{
	struct midr_context ctx;
	struct midr_virtual_link_status st;
	struct midr_virtual_link_desc d = desc_v4("vl18");
	size_t a0, d0, u0;

	inject_reset();
	host_bringup(&ctx, "vlink-test-18");
	sim_device_up("vl18", 181, ZEBRA_IF_GRE);
	sim_overlay_ready("vl18", &d.overlay_local, d.overlay_prefix_len);

	notify_reset();
	midr_virtual_link_register_notify(notify_cb, NULL);

	assert(midr_virtual_link_add(&ctx, &d, &st) == 0);
	assert(midr_virtual_link_wait_ready(&ctx, "vl18", 500, &st) == true);
	assert_ready(&st, 181);

	/* autonomous failure, again with no get_state() */
	sim_device_gone("vl18");
	assert(pump_until_notify_failed(2000) == true);
	assert(notify_failed == 1);

	/* Retry the retained FAILED descriptor, then delete it. */
	sim_device_up("vl18", 182, ZEBRA_IF_GRE);
	sim_overlay_ready("vl18", &d.overlay_local, d.overlay_prefix_len);
	a0 = gre_add_calls;
	d0 = gre_del_calls;
	u0 = addr_unset_calls;

	assert(midr_virtual_link_add(&ctx, &d, &st) == 0);
	/* D4: nothing of the old link is live any more, so the retry must not
	 * delete (or unset) anything on the way in; a stale gre_created or
	 * addr_set would show up right here. */
	assert(gre_del_calls == d0);
	assert(addr_unset_calls == u0);
	assert(gre_add_calls == a0 + 1);
	assert(midr_virtual_link_wait_ready(&ctx, "vl18", 500, &st) == true);
	assert_ready(&st, 182);

	/* ... and the caller-visible delete stays a single create + delete */
	assert(midr_virtual_link_del(&ctx, "vl18", &st) == 0);
	assert(st.state == MIDR_VLINK_DOWN);
	assert(addr_unset_calls == u0 + 1); /* exactly one unset ... */
	assert(gre_del_calls == d0 + 1);    /* ... and exactly one delete */
	assert(gre_add_calls == a0 + 1);    /* and no extra create */
	assert(midr_virtual_link_get_state(&ctx, "vl18", &st) == -1);

	midr_virtual_link_unregister_notify(notify_cb);
	host_teardown(&ctx);
}

/* ================================================================== *
 *  D1a (case 19) — a GRE-layer DOWN/FAILED for our own device fails
 *  the entry synchronously: no polling, no get_state()
 * ================================================================== */
static void test_case19_autonomous_gre_notify_failure(void)
{
	struct midr_context ctx;
	struct midr_virtual_link_status st;
	struct midr_virtual_link_desc d = desc_v4("vl19");

	inject_reset();
	host_bringup(&ctx, "vlink-test-19");
	sim_device_up("vl19", 191, ZEBRA_IF_GRE);
	sim_overlay_ready("vl19", &d.overlay_local, d.overlay_prefix_len);

	notify_reset();
	midr_virtual_link_register_notify(notify_cb, NULL);

	assert(midr_virtual_link_add(&ctx, &d, &st) == 0);
	assert(midr_virtual_link_wait_ready(&ctx, "vl19", 500, &st) == true);
	assert_ready(&st, 191);
	assert(notify_failed == 0);

	/* The GRE layer reports the device gone (zebra reconnect / kernel
	 * loss / its own reconcile all end here).  No event-loop pumping and
	 * no get_state() call: the subscription alone must fail the entry. */
	assert(midr_gre_interface_del(&ctx, "vl19", NULL) == 0);
	assert(notify_failed == 1);
	assert(notify_last_event == MIDR_VLINK_EV_FAILED);

	assert(midr_virtual_link_get_state(&ctx, "vl19", &st) == 0);
	assert_failed(&st, MIDR_VLINK_STAGE_DEVICE_CONFIRM, ENODEV);
	assert(st.ifindex == 0);
	assert(st.event == MIDR_VLINK_EV_NONE);

	midr_virtual_link_unregister_notify(notify_cb);
	host_teardown(&ctx);
}

/* ================================================================== *
 *  §4.1 — registry allocation failure is reported as GRE_CREATE/ENOMEM
 * ================================================================== */
static void test_alloc_failure(void)
{
	struct midr_context ctx;
	struct midr_virtual_link_status st;
	struct midr_virtual_link_desc d = desc_v4("vl13");

	inject_reset();
	host_bringup(&ctx, "vlink-test-13");

	fail_next_calloc = true;
	assert(midr_virtual_link_add(&ctx, &d, &st) == -1);
	assert(st.state == MIDR_VLINK_FAILED);
	assert(st.stage == MIDR_VLINK_STAGE_GRE_CREATE);
	assert(st.last_error == ENOMEM);

	fail_next_calloc = false;
	host_teardown(&ctx);
}

int main(void)
{
	/* frr_init() normally does this; the test bootstraps libfrr by hand. */
	qobj_init();
	/* The libfrr interface view needs the default VRF to exist. */
	midrd_vrf_init();

	test_case01_missing_outer_endpoint();
	test_case02_outer_family_mismatch();
	test_case03_overlay_invalid();
	test_case04_gre_create_failure();
	test_case05_address_set_failure();
	test_case06_admin_up_failure();
	test_case07_device_confirm_timeout();
	test_case08_idempotent_add();
	test_case09_same_name_rejected();
	test_case10_rebind();
	test_case11_delete_rebuild();
	test_case12_device_loss_and_ifindex_change();
	test_case13_autonomous_device_loss();
	test_case14_event_ordering_and_overlay_payload();
	test_case15_failed_ifindex_retention();
	test_case16a_empty_name_rebuild_no_stale_overlay();
	test_case16b_empty_name_live_foreign_pair_refused();
	test_case17_query_paths_do_not_notify();
	test_case18_no_stale_bookkeeping_after_loss();
	test_case19_autonomous_gre_notify_failure();
	test_alloc_failure();

	/* Everything above really went through the wrapped ZAPI path. */
	assert(gre_add_calls > 0);
	assert(gre_del_calls > 0);
	assert(addr_set_calls > 0);
	assert(addr_unset_calls > 0);
	assert(admin_up_calls > 0);

	midrd_vrf_terminate();
	puts("midrd-virtual-link-test: PASS");
	return 0;
}

