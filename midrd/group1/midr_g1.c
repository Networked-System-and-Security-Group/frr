// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR group 1 runtime inside midrd: instance, configuration lifecycle,
 * debug switches and the overlay session table.
 */
#include <zebra.h>

#include "command.h"
#include "log.h"
#include "monotime.h"
#include "vty.h"

#include "midrd/group1/midr_g1.h"
#include "midrd/group1/midr_nds.h"
#include "midrd/group1/midr_nds_vty.h"
#include "midrd/group1/midr_admission.h"
#include "midrd/group1/midr_tier1_list.h"
#include "midrd/group1/midr_tier1_vty.h"
#include "midrd/group1/midr_trace_scheduler.h"
#include "midrd/midr-spf.h"

DEFINE_MGROUP(MIDR_G1, "MIDR group 1");
DEFINE_MTYPE(MIDR_G1, MIDR_G1, "MIDR group 1 state");
DEFINE_MTYPE_STATIC(MIDR_G1, MIDR_G1_PEER, "MIDR group 1 session");

DEFINE_HOOK(midr_g1_config_end, (struct midr_g1 *g1), (g1));

/* Same windows as bgpd: wait at most this long for the end marker, then
 * run post-config work one second after the configuration ended. */
#define MIDR_G1_CONFIG_MAX_WAIT_SECONDS 600
#define MIDR_G1_POST_CONFIG_DELAY_SECONDS 1

unsigned long midr_g1_debug;

static struct event_loop *g1_master;
static struct midr_g1 *g1_instance;
static struct event *t_config;
static bool g1_terminating;

struct event_loop *midr_g1_master(void)
{
	return g1_master;
}

struct midr_g1 *midr_g1_get(void)
{
	return g1_instance;
}

bool midr_g1_config_inprocess(void)
{
	return event_is_scheduled(t_config);
}

static void midr_g1_config_finish(struct event *event)
{
	if (g1_instance)
		hook_call(midr_g1_config_end, g1_instance);
}

static void midr_g1_config_timeout(struct event *event)
{
	zlog_err("MIDR configuration end marker not seen after %d seconds",
		 MIDR_G1_CONFIG_MAX_WAIT_SECONDS);
	midr_g1_config_finish(event);
}

static void midr_g1_config_start(struct vty *vty)
{
	event_cancel(&t_config);
	event_add_timer(g1_master, midr_g1_config_timeout, NULL,
			MIDR_G1_CONFIG_MAX_WAIT_SECONDS, &t_config);
}

static void midr_g1_config_stop(struct vty *vty)
{
	if (!midr_g1_config_inprocess())
		return;
	event_cancel(&t_config);
	event_add_timer(g1_master, midr_g1_config_finish, NULL,
			MIDR_G1_POST_CONFIG_DELAY_SECONDS, &t_config);
}

/* ------------------------------------------------------------------------
 * Overlay sessions
 * ---------------------------------------------------------------------- */

struct midr_g1_peer *midr_g1_peer_lookup(struct midr_g1 *g1,
					 const struct ipaddr *transport)
{
	struct listnode *node;
	struct midr_g1_peer *peer;

	if (!g1 || !transport)
		return NULL;
	for (ALL_LIST_ELEMENTS_RO(g1->peer, node, peer))
		if (!ipaddr_cmp(&peer->transport, transport))
			return peer;
	return NULL;
}

struct midr_g1_peer *midr_g1_peer_lookup_rid(struct midr_g1 *g1,
					     struct in_addr remote_id)
{
	struct listnode *node;
	struct midr_g1_peer *peer;

	if (!g1 || remote_id.s_addr == INADDR_ANY)
		return NULL;
	for (ALL_LIST_ELEMENTS_RO(g1->peer, node, peer))
		if (peer->remote_id.s_addr == remote_id.s_addr)
			return peer;
	return NULL;
}

struct midr_g1_peer *midr_g1_peer_create(struct midr_g1 *g1,
					 const struct ipaddr *transport,
					 struct in_addr remote_id, as_t as)
{
	struct midr_g1_peer *peer;

	if (!g1 || !transport || ipaddr_is_zero(transport))
		return NULL;
	peer = midr_g1_peer_lookup(g1, transport);
	if (peer)
		return peer;
	peer = XCALLOC(MTYPE_MIDR_G1_PEER, sizeof(*peer));
	peer->g1 = g1;
	peer->transport = *transport;
	peer->remote_id = remote_id;
	peer->as = as;
	listnode_add(g1->peer, peer);
	midr_g1_peer_start(peer);
	return peer;
}

void midr_g1_peer_start(struct midr_g1_peer *peer)
{
	int ret;

	if (!peer || peer->requested || g1_terminating)
		return;
	if (!midr_admission_begin(peer))
		return;
	ret = midr_session_request(peer->g1->ctx, peer->remote_id.s_addr,
				   &peer->transport);
	if (ret) {
		zlog_warn("MIDR: session request to %pIA failed: %d",
			  &peer->transport, ret);
		return;
	}
	peer->requested = true;
}

void midr_g1_peer_stop(struct midr_g1_peer *peer)
{
	if (!peer || !peer->requested || peer->established || g1_terminating)
		return;
	MIDR_G1_LOG("MIDR: stop pending session %pIA", &peer->transport);
	(void)midr_session_release(peer->g1->ctx, &peer->transport);
	peer->requested = false;
}

/* Release a session midrd reports up but admission no longer allows; it is
 * requested again once a fresh permit exists. */
static void midr_g1_peer_stop_established(struct midr_g1_peer *peer)
{
	(void)midr_session_release(peer->g1->ctx, &peer->transport);
	peer->requested = false;
}

void midr_g1_peer_delete(struct midr_g1_peer *peer)
{
	struct midr_g1 *g1;

	if (!peer)
		return;
	g1 = peer->g1;
	/* On daemon exit midrd still needs the sessions to flood withdrawals. */
	if (peer->requested && !g1_terminating) {
		MIDR_G1_LOG("MIDR: release session %pIA (%s)", &peer->transport,
			    midr_g1_peer_state_str(peer));
		(void)midr_session_release(g1->ctx, &peer->transport);
	}
	listnode_delete(g1->peer, peer);
	XFREE(MTYPE_MIDR_G1_PEER, peer);
}

const char *midr_g1_peer_state_str(const struct midr_g1_peer *peer)
{
	if (peer->established)
		return "Established";
	return peer->requested ? "Connect" : "Idle";
}

/* Session changes arrive from inside midrd's receive path.  Handle them on
 * the event queue: group 1 may release sessions while reacting.  Events run
 * in the order they were queued, so an up/down pair keeps its order. */
struct midr_g1_session_event {
	struct ipaddr transport;
	uint32_t node_id;
	bool up;
};

static void midr_g1_session_up(struct midr_g1 *g1, struct midr_g1_peer *peer,
			       uint32_t node_id)
{
	if (peer->established)
		return;
	/* The permit that let the attempt start must still hold now. */
	if (!midr_admission_check(peer)) {
		zlog_info("MIDR: closing session %pIA, admission no longer holds",
			  &peer->transport);
		midr_g1_peer_stop_established(peer);
		return;
	}
	if (node_id) {
		struct in_addr reported = { .s_addr = node_id };

		if (peer->remote_id.s_addr != INADDR_ANY &&
		    peer->remote_id.s_addr != node_id)
			zlog_warn("MIDR: session %pIA reports node %pI4, expected %pI4",
				  &peer->transport, &reported, &peer->remote_id);
		peer->remote_id = reported;
	}
	peer->established = true;
	peer->uptime = monotime(NULL);
	peer->established_count++;
	midr_nds_session_status(peer, false);
	if (peer->established)
		midr_nodedir_session_up(g1, &peer->transport);
}

static void midr_g1_session_down(struct midr_g1_peer *peer)
{
	if (!peer->established)
		return;
	peer->established = false;
	peer->uptime = monotime(NULL);
	peer->dropped_count++;
	midr_nds_session_status(peer, true);
}

static void midr_g1_session_event_cb(struct event *event)
{
	struct midr_g1_session_event *ev = EVENT_ARG(event);
	struct midr_g1 *g1 = g1_instance;
	struct midr_g1_peer *peer = midr_g1_peer_lookup(g1, &ev->transport);

	if (peer && !g1_terminating) {
		/* A stale up for a session released meanwhile is ignored. */
		if (ev->up && peer->requested &&
		    midr_session_is_up(g1->ctx, &peer->transport))
			midr_g1_session_up(g1, peer, ev->node_id);
		else if (!ev->up)
			midr_g1_session_down(peer);
	}
	XFREE(MTYPE_MIDR_G1_PEER, ev);
}

static void midr_g1_session_queue(const struct ipaddr *remote,
				  uint32_t node_id, bool up)
{
	struct midr_g1_session_event *ev;

	ev = XCALLOC(MTYPE_MIDR_G1_PEER, sizeof(*ev));
	ev->transport = *remote;
	ev->node_id = node_id;
	ev->up = up;
	event_add_event(g1_master, midr_g1_session_event_cb, ev, 0, NULL);
}

static void midr_g1_session_up_cb(struct midr_context *ctx, uint32_t node_id,
				  const struct ipaddr *remote, void *arg)
{
	midr_g1_session_queue(remote, node_id, true);
}

static void midr_g1_session_down_cb(struct midr_context *ctx,
				    const struct ipaddr *remote, int reason,
				    void *arg)
{
	struct midr_g1_peer *peer = midr_g1_peer_lookup(arg, remote);

	if (peer)
		peer->last_reset = reason;
	midr_g1_session_queue(remote, 0, false);
}

static const struct midr_session_ops midr_g1_session_ops = {
	.session_up = midr_g1_session_up_cb,
	.session_down = midr_g1_session_down_cb,
};

/* ------------------------------------------------------------------------
 * Debug switches
 * ---------------------------------------------------------------------- */

DEFUN(debug_midr, debug_midr_cmd, "debug midr [discovery]",
      DEBUG_STR "MIDR group 1\n" "Discovery flow (list exchange, join)\n")
{
	if (argc > 2)
		midr_g1_debug |= MIDR_G1_DEBUG_DISCOVERY;
	else
		midr_g1_debug |= MIDR_G1_DEBUG_GENERAL;
	return CMD_SUCCESS;
}

DEFUN(no_debug_midr, no_debug_midr_cmd, "no debug midr [discovery]",
      NO_STR DEBUG_STR "MIDR group 1\n" "Discovery flow (list exchange, join)\n")
{
	if (argc > 3)
		midr_g1_debug &= ~MIDR_G1_DEBUG_DISCOVERY;
	else
		midr_g1_debug = 0;
	return CMD_SUCCESS;
}

DEFUN_NOSH(show_debugging_midr, show_debugging_midr_cmd,
	   "show debugging [midr]",
	   SHOW_STR DEBUG_STR "MIDR group 1\n")
{
	vty_out(vty, "MIDR debugging status:\n");
	if (MIDR_G1_DEBUG_ON(GENERAL))
		vty_out(vty, "  MIDR debugging is on\n");
	if (MIDR_G1_DEBUG_ON(DISCOVERY))
		vty_out(vty, "  MIDR discovery debugging is on\n");
	cmd_show_lib_debugs(vty);
	return CMD_SUCCESS;
}

/* ------------------------------------------------------------------------
 * Read-only view of midrd's SPF results (public midr-spf.h interface).
 * midrd's own modules print only to stdout; this is how an operator sees
 * that the Node/Link facts group 1 provides reach the computed topology.
 * ---------------------------------------------------------------------- */

static const char *midr_g1_spf_scope_str(enum midr_spf_route_scope scope)
{
	switch (scope) {
	case MIDR_SPF_ROUTE_LOCAL:
		return "local";
	case MIDR_SPF_ROUTE_INTRA_GROUP:
		return "intra";
	case MIDR_SPF_ROUTE_INTER_GROUP:
		return "inter";
	case MIDR_SPF_ROUTE_UNREACHABLE:
		break;
	}
	return "unreachable";
}

static void midr_g1_spf_addr(uint8_t family, const uint8_t *bytes,
			     struct ipaddr *addr)
{
	memset(addr, 0, sizeof(*addr));
	if (family == 4) {
		addr->ipa_type = IPADDR_V4;
		memcpy(&addr->ipaddr_v4, bytes, 4);
	} else if (family == 6) {
		addr->ipa_type = IPADDR_V6;
		memcpy(&addr->ipaddr_v6, bytes, 16);
	}
}

DEFUN(show_midr_spf, show_midr_spf_cmd, "show midr spf",
      SHOW_STR "MIDR information\n" "Show midrd SPF results\n")
{
	struct midr_g1 *g1 = midr_g1_get();
	const struct midr_spf_results *results = NULL;
	size_t count, reachable = 0, intra = 0, inter = 0, local = 0;
	int ret;

	if (!g1)
		return CMD_SUCCESS;
	ret = midr_spf_results_get(g1->ctx, &results);
	if (ret) {
		vty_out(vty, "%% SPF results unavailable: %d\n", ret);
		return CMD_WARNING;
	}
	count = midr_spf_results_count(results);
	vty_out(vty, "SPF generation %" PRIu64 ", %zu routes\n",
		midr_spf_results_generation(results), count);
	for (size_t i = 0; i < count; i++) {
		const struct midr_spf_route *route =
			midr_spf_results_at(results, i);
		struct ipaddr prefix;
		struct in_addr originator = { .s_addr = route->originator };

		if (route->reachable)
			reachable++;
		if (route->scope == MIDR_SPF_ROUTE_LOCAL)
			local++;
		else if (route->scope == MIDR_SPF_ROUTE_INTRA_GROUP)
			intra++;
		else if (route->scope == MIDR_SPF_ROUTE_INTER_GROUP)
			inter++;
		midr_g1_spf_addr(route->family, route->prefix, &prefix);
		vty_out(vty, "  %pIA/%u origin %pI4 %s metric %" PRIu64,
			&prefix, route->prefix_len, &originator,
			midr_g1_spf_scope_str(route->scope), route->metric);
		for (size_t j = 0; j < route->nexthop_count; j++) {
			const struct midr_spf_nexthop *nh = &route->nexthops[j];
			struct ipaddr via;
			struct in_addr remote = { .s_addr = nh->remote_node_id };

			midr_g1_spf_addr(nh->family, nh->address, &via);
			vty_out(vty, " via %pIA (node %pI4)", &via, &remote);
		}
		vty_out(vty, "\n");
	}
	vty_out(vty,
		"Summary: reachable %zu local %zu intra %zu inter %zu\n",
		reachable, local, intra, inter);
	midr_spf_results_release(&results);
	return CMD_SUCCESS;
}

static int midr_g1_debug_config_write(struct vty *vty);

static struct cmd_node midr_g1_debug_node = {
	.name = "debug",
	.node = DEBUG_NODE,
	.prompt = "",
	.config_write = midr_g1_debug_config_write,
};

static int midr_g1_debug_config_write(struct vty *vty)
{
	int written = 0;

	if (MIDR_G1_DEBUG_ON(GENERAL)) {
		vty_out(vty, "debug midr\n");
		written++;
	}
	if (MIDR_G1_DEBUG_ON(DISCOVERY)) {
		vty_out(vty, "debug midr discovery\n");
		written++;
	}
	return written;
}

static void midr_g1_debug_init(void)
{
	install_node(&midr_g1_debug_node);
	install_element(ENABLE_NODE, &debug_midr_cmd);
	install_element(CONFIG_NODE, &debug_midr_cmd);
	install_element(ENABLE_NODE, &no_debug_midr_cmd);
	install_element(CONFIG_NODE, &no_debug_midr_cmd);
	install_element(VIEW_NODE, &show_debugging_midr_cmd);
	install_element(VIEW_NODE, &show_midr_spf_cmd);
}

/* ------------------------------------------------------------------------
 * midrd entry points
 * ---------------------------------------------------------------------- */

void midr_group1_init(struct event_loop *master, struct midr_context *ctx)
{
	struct midr_g1 *g1;

	g1_master = master;
	g1 = XCALLOC(MTYPE_MIDR_G1, sizeof(*g1));
	g1->ctx = ctx;
	/* The BGP Identifier's raw s_addr is the MIDR node id (see
	 * midr_nds_facts.c); midrd is started with that value. */
	g1->router_id.s_addr = midr_context_node_id(ctx);
	g1->name_pretty = "midrd";
	g1->vrf_id = VRF_DEFAULT;
	g1->peer = list_new();
	g1_instance = g1;

	/* The traceroute backend opens sockets with vrf_socket(), which needs
	 * the default VRF/netns set up; bgpd did this for group 1. */
	vrf_init(NULL, NULL, NULL, NULL);
	cmd_init_config_callbacks(midr_g1_config_start, midr_g1_config_stop);
	midr_g1_debug_init();
	if (midr_trace_scheduler_init(master) != 0)
		zlog_warn("MIDR traceroute scheduler is unavailable; traceroute requests will be rejected");
	midr_tier1_vty_init();
	midr_nds_vty_init();

	(void)midr_session_owner_register(ctx, &midr_g1_session_ops, g1);
	midr_nodedir_init(g1);
	midr_nds_init(g1);
	zlog_notice("MIDR group 1 started, router-id %pI4", &g1->router_id);
}

void midr_group1_terminate(void)
{
	struct midr_g1 *g1 = g1_instance;
	struct midr_g1_peer *peer;

	if (!g1)
		return;
	g1_terminating = true;
	event_cancel(&t_config);
	(void)midr_session_owner_register(g1->ctx, NULL, NULL);
	midr_nds_finish(g1);
	midr_nodedir_finish(g1);
	while ((peer = listnode_head(g1->peer)))
		midr_g1_peer_delete(peer);
	list_delete(&g1->peer);
	midr_trace_scheduler_fini();
	midr_tier1_list_fini();
	vrf_terminate();
	g1_instance = NULL;
	XFREE(MTYPE_MIDR_G1, g1);
}
