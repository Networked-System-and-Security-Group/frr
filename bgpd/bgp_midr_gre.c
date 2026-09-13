// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR GRE virtual interface provisioning (bgp_midr_gre.c)
 *
 * See bgp_midr_gre.h for the design overview.
 *
 * This file owns the small local registry that maps a set of tunnel
 * endpoints to the netdevice name that was handed to zebra.  That lets the
 * CP either name tunnels explicitly or let this module generate a stable
 * name, and it makes create requests idempotent.
 */

#include <zebra.h>

#include "lib/frrevent.h"
#include "lib/if.h"
#include "lib/ipaddr.h"
#include "lib/linklist.h"
#include "lib/memory.h"
#include "lib/vrf.h"
#include "lib/zclient.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_midr_gre.h"

/*
 * Only need the zclient pointer from bgp_zebra; avoid pulling in
 * bgp_zebra.h which drags in bgp_path_info / BGP_ROUTE_*.
 */
extern struct zclient *bgp_zclient;

DEFINE_MTYPE_STATIC(BGPD, MIDR_GRE, "MIDR GRE tunnel");

/* How often the pending-request confirmer polls zebra's interface list. */
#define MIDR_GRE_CONFIRM_INTERVAL_MS 100
/* How long a create request may stay PENDING before it is declared failed. */
#define MIDR_GRE_CONFIRM_TIMEOUT_MS 3000

/* One MIDR GRE tunnel, plus its establishment state. */
struct midr_gre_entry {
	char ifname[IFNAMSIZ];
	vrf_id_t vrf_id;
	struct ipaddr local;
	struct ipaddr remote;

	/* ---- establishment state ---- */
	enum midr_gre_state state;
	struct event *t_confirm; /* pending-confirmation poll timer */
	uint32_t waited_ms;	 /* time spent in PENDING */
	ifindex_t ifindex;	 /* valid when UP */
	uint8_t iftype;		 /* enum zebra_iftype */
	int err;		 /* reason when FAILED */
	uint64_t up_since_ms;	 /* monotonic ms when it became UP */
};

/* Registered state-change notification. */
struct midr_gre_notifier {
	midr_gre_notify_cb cb;
	void *arg;
};

static struct list *midr_gre_registry;
static struct list *midr_gre_notifiers;
static uint32_t midr_gre_name_seq;

/* Monotonic milliseconds, used to report how long an interface is up. */
static uint64_t midr_gre_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The bgpd event loop, or NULL when running outside of bgpd (e.g. tests). */
static struct event_loop *midr_gre_master(void)
{
	return bm ? bm->master : NULL;
}

static struct list *midr_gre_registry_get(void)
{
	if (!midr_gre_registry)
		midr_gre_registry = list_new();

	return midr_gre_registry;
}

static struct midr_gre_entry *
midr_gre_registry_lookup_by_name(const char *ifname)
{
	struct listnode *node;
	struct midr_gre_entry *e;

	if (!midr_gre_registry || !ifname)
		return NULL;

	for (ALL_LIST_ELEMENTS_RO(midr_gre_registry, node, e)) {
		if (strcmp(e->ifname, ifname) == 0)
			return e;
	}

	return NULL;
}

static struct midr_gre_entry *
midr_gre_registry_lookup_by_endpoints(vrf_id_t vrf_id,
				      const struct ipaddr *local,
				      const struct ipaddr *remote)
{
	struct listnode *node;
	struct midr_gre_entry *e;

	if (!midr_gre_registry || !local || !remote)
		return NULL;

	for (ALL_LIST_ELEMENTS_RO(midr_gre_registry, node, e)) {
		if (e->vrf_id == vrf_id && ipaddr_cmp(&e->local, local) == 0 &&
		    ipaddr_cmp(&e->remote, remote) == 0)
			return e;
	}

	return NULL;
}

static struct midr_gre_entry *
midr_gre_registry_add(const char *ifname, vrf_id_t vrf_id,
		      const struct ipaddr *local, const struct ipaddr *remote)
{
	struct midr_gre_entry *e;

	e = XCALLOC(MTYPE_MIDR_GRE, sizeof(*e));
	strlcpy(e->ifname, ifname, sizeof(e->ifname));
	e->vrf_id = vrf_id;
	e->local = *local;
	e->remote = *remote;
	e->state = MIDR_GRE_STATE_DOWN;

	listnode_add(midr_gre_registry_get(), e);

	return e;
}

static void midr_gre_registry_del(struct midr_gre_entry *e)
{
	if (!e)
		return;

	listnode_delete(midr_gre_registry, e);
	XFREE(MTYPE_MIDR_GRE, e);
}

/*
 * Derive (or reuse) the netdevice name for a tunnel.
 *
 * A caller-supplied name wins.  Otherwise the endpoints are looked up in the
 * registry and, on first use, a fresh "midr-gre-<n>" name is generated.
 */
static void midr_gre_resolve_name(vrf_id_t vrf_id,
				  const struct midr_gre_tunnel *tun, char *buf,
				  size_t buflen)
{
	struct midr_gre_entry *e;

	if (tun->ifname[0]) {
		strlcpy(buf, tun->ifname, buflen);
		return;
	}

	e = midr_gre_registry_lookup_by_endpoints(vrf_id, &tun->local,
						  &tun->remote);
	if (e) {
		strlcpy(buf, e->ifname, buflen);
		return;
	}

	snprintf(buf, buflen, "midr-gre-%u", ++midr_gre_name_seq);
}

/*
 * =========================================================================
 *  Establishment tracking
 * =========================================================================
 */

const char *midr_gre_state_str(enum midr_gre_state state)
{
	switch (state) {
	case MIDR_GRE_STATE_DOWN:
		return "down";
	case MIDR_GRE_STATE_PENDING:
		return "pending";
	case MIDR_GRE_STATE_UP:
		return "up";
	case MIDR_GRE_STATE_FAILED:
		return "failed";
	case MIDR_GRE_STATE_MAX:
		break;
	}

	return "unknown";
}

/* Snapshot an entry into the caller-facing status structure. */
static void midr_gre_fill_status(const struct midr_gre_entry *e,
				 struct midr_gre_status *st)
{
	if (!st)
		return;

	memset(st, 0, sizeof(*st));
	st->state = e->state;
	strlcpy(st->ifname, e->ifname, sizeof(st->ifname));
	st->vrf_id = e->vrf_id;

	if (e->state == MIDR_GRE_STATE_UP) {
		st->ifindex = e->ifindex;
		st->iftype = e->iftype;
		if (e->up_since_ms)
			st->up_ms = (uint32_t)(midr_gre_now_ms() -
					       e->up_since_ms);
	} else if (e->state == MIDR_GRE_STATE_FAILED) {
		st->err = e->err;
	}
}

/* Fan out a state change to all registered listeners. */
static void midr_gre_notify(const struct midr_gre_entry *e)
{
	struct listnode *node;
	struct midr_gre_notifier *n;
	struct midr_gre_status st;

	if (!midr_gre_notifiers)
		return;

	midr_gre_fill_status(e, &st);

	for (ALL_LIST_ELEMENTS_RO(midr_gre_notifiers, node, n)) {
		if (n->cb)
			n->cb(&st, n->arg);
	}
}

/*
 * Promote an entry to UP when zebra already knows the device.
 *
 * bgpd learns about new netdevices through zebra's ZEBRA_INTERFACE_ADD
 * broadcast.  That message is consumed by lib/zclient.c's default handler
 * (lib_handlers[] -> zclient_interface_add()), which creates the interface
 * in the daemon's interface list, so confirming is a name lookup.
 */
static bool midr_gre_entry_check_present(struct midr_gre_entry *e)
{
	struct interface *ifp = if_lookup_by_name(e->ifname, e->vrf_id);

	if (!ifp || ifp->ifindex == IFINDEX_INTERNAL)
		return false;

	e->state = MIDR_GRE_STATE_UP;
	e->ifindex = ifp->ifindex;
	e->iftype = ifp->zif_type;
	e->err = 0;
	if (!e->up_since_ms)
		e->up_since_ms = midr_gre_now_ms();

	return true;
}

/* Poll zebra's view of the interface and resolve PENDING -> UP/FAILED. */
static void midr_gre_confirm_cb(struct event *t)
{
	struct midr_gre_entry *e = EVENT_ARG(t);

	e->t_confirm = NULL;
	if (e->state != MIDR_GRE_STATE_PENDING)
		return;

	if (midr_gre_entry_check_present(e)) {
		zlog_info("MIDR GRE interface %s is UP (ifindex %u, zif_type %u)",
			  e->ifname, e->ifindex, e->iftype);
		midr_gre_notify(e);
		return;
	}

	e->waited_ms += MIDR_GRE_CONFIRM_INTERVAL_MS;
	if (e->waited_ms >= MIDR_GRE_CONFIRM_TIMEOUT_MS) {
		e->state = MIDR_GRE_STATE_FAILED;
		e->err = ETIMEDOUT;
		zlog_warn("MIDR GRE interface %s: creation not confirmed after %u ms",
			  e->ifname, e->waited_ms);
		midr_gre_notify(e);
		return;
	}

	if (midr_gre_master())
		event_add_timer_msec(midr_gre_master(), midr_gre_confirm_cb, e,
				     MIDR_GRE_CONFIRM_INTERVAL_MS,
				     &e->t_confirm);
}

/* Start (or restart) confirmation polling for an entry. */
static void midr_gre_confirm_start(struct midr_gre_entry *e)
{
	if (e->t_confirm)
		event_cancel(&e->t_confirm);

	e->state = MIDR_GRE_STATE_PENDING;
	e->err = 0;
	e->waited_ms = 0;
	e->ifindex = 0;
	e->iftype = 0;
	e->up_since_ms = 0;

	if (midr_gre_master())
		event_add_timer_msec(midr_gre_master(), midr_gre_confirm_cb, e,
				     MIDR_GRE_CONFIRM_INTERVAL_MS,
				     &e->t_confirm);
}

static void midr_gre_confirm_cancel(struct midr_gre_entry *e)
{
	if (e->t_confirm)
		event_cancel(&e->t_confirm);
}

/* Wake-up only callback used by midr_gre_interface_wait_up(). */
static void midr_gre_wait_tick_cb(struct event *t)
{
	(void)t;
}

int midr_gre_interface_add(struct bgp *bgp, const struct midr_gre_tunnel *tun,
			   struct midr_gre_status *status)
{
	struct zclient_gre_if gre = {};
	struct midr_gre_entry *e;
	char name[IFNAMSIZ] = {};
	vrf_id_t vrf_id;
	int ret;
	int err = 0;

	if (status)
		memset(status, 0, sizeof(*status));

	if (!tun) {
		zlog_warn("%s: NULL tunnel descriptor", __func__);
		err = EINVAL;
		goto out_fail;
	}

	if (IS_IPADDR_NONE(&tun->local) || IS_IPADDR_NONE(&tun->remote)) {
		zlog_warn("%s: GRE tunnel endpoints must both be set",
			  __func__);
		err = EINVAL;
		goto out_fail;
	}

	if (tun->local.ipa_type != tun->remote.ipa_type) {
		zlog_warn("%s: GRE tunnel endpoint families differ", __func__);
		err = EAFNOSUPPORT;
		goto out_fail;
	}

	if (!bgp_zclient || bgp_zclient->sock < 0) {
		zlog_warn("%s: zclient not ready", __func__);
		err = ENOTCONN;
		goto out_fail;
	}

	vrf_id = (tun->vrf_id == VRF_UNKNOWN && bgp) ? bgp->vrf_id
						     : tun->vrf_id;

	midr_gre_resolve_name(vrf_id, tun, name, sizeof(name));

	strlcpy(gre.ifname, name, sizeof(gre.ifname));
	gre.local = tun->local;
	gre.remote = tun->remote;
	gre.link_ifindex = tun->link_ifindex;
	gre.ikey = tun->ikey;
	gre.okey = tun->okey;
	gre.encap_flags = tun->encap_flags;
	gre.mtu = tun->mtu;

	ret = zclient_send_gre_add(bgp_zclient, vrf_id, &gre);
	if (ret == ZCLIENT_SEND_FAILURE) {
		zlog_warn("%s: failed to send GRE add for %s", __func__,
			  gre.ifname);
		err = EIO;
		goto out_fail;
	}

	/* Remember the mapping so the same name is reused / can be removed. */
	e = midr_gre_registry_lookup_by_name(name);
	if (!e)
		e = midr_gre_registry_add(name, vrf_id, &tun->local,
					  &tun->remote);

	/*
	 * If zebra already knows the device report UP straight away,
	 * otherwise arm the confirmation poller.
	 */
	if (midr_gre_entry_check_present(e)) {
		midr_gre_confirm_cancel(e);
		zlog_info("MIDR GRE interface %s already present (ifindex %u)",
			  e->ifname, e->ifindex);
		midr_gre_fill_status(e, status);
		return 0;
	}

	midr_gre_confirm_start(e);
	zlog_info("MIDR GRE interface %s requested (local %pIA remote %pIA), awaiting confirmation",
		  e->ifname, &e->local, &e->remote);

	midr_gre_fill_status(e, status);
	return 0;

out_fail:
	if (status) {
		status->state = MIDR_GRE_STATE_FAILED;
		status->err = err ? err : EIO;
		if (name[0])
			strlcpy(status->ifname, name, sizeof(status->ifname));
		else if (tun && tun->ifname[0])
			strlcpy(status->ifname, tun->ifname,
				sizeof(status->ifname));
		if (bgp)
			status->vrf_id = bgp->vrf_id;
	}
	return -1;
}

int midr_gre_interface_del(struct bgp *bgp, const char *ifname,
			   struct midr_gre_status *status)
{
	struct midr_gre_entry *e;
	vrf_id_t vrf_id;
	int ret;

	if (status)
		memset(status, 0, sizeof(*status));

	if (!ifname || ifname[0] == '\0') {
		zlog_warn("%s: NULL or empty interface name", __func__);
		if (status) {
			status->state = MIDR_GRE_STATE_FAILED;
			status->err = EINVAL;
		}
		return -1;
	}

	if (!bgp_zclient || bgp_zclient->sock < 0) {
		zlog_warn("%s: zclient not ready", __func__);
		if (status) {
			status->state = MIDR_GRE_STATE_FAILED;
			status->err = ENOTCONN;
			strlcpy(status->ifname, ifname, sizeof(status->ifname));
		}
		return -1;
	}

	e = midr_gre_registry_lookup_by_name(ifname);
	if (e)
		vrf_id = e->vrf_id;
	else if (bgp)
		vrf_id = bgp->vrf_id;
	else
		vrf_id = VRF_DEFAULT;

	ret = zclient_send_gre_delete(bgp_zclient, vrf_id, ifname);
	if (ret == ZCLIENT_SEND_FAILURE) {
		zlog_warn("%s: failed to send GRE delete for %s", __func__,
			  ifname);
		if (status) {
			status->state = MIDR_GRE_STATE_FAILED;
			status->err = EIO;
			status->vrf_id = vrf_id;
			strlcpy(status->ifname, ifname, sizeof(status->ifname));
		}
		return -1;
	}

	if (!e) {
		/* Never tracked locally: nothing to clean up. */
		if (status) {
			status->state = MIDR_GRE_STATE_DOWN;
			status->vrf_id = vrf_id;
			strlcpy(status->ifname, ifname, sizeof(status->ifname));
		}
		return 0;
	}

	midr_gre_confirm_cancel(e);
	e->state = MIDR_GRE_STATE_DOWN;
	e->ifindex = 0;
	e->iftype = 0;
	e->err = 0;
	e->up_since_ms = 0;

	midr_gre_notify(e);
	midr_gre_fill_status(e, status);
	midr_gre_registry_del(e);

	zlog_info("MIDR GRE interface %s removal requested", ifname);

	return 0;
}

int midr_gre_interface_del_by_endpoints(struct bgp *bgp, vrf_id_t vrf_id,
					const struct ipaddr *local,
					const struct ipaddr *remote,
					struct midr_gre_status *status)
{
	struct midr_gre_entry *e;

	if (status)
		memset(status, 0, sizeof(*status));

	if (vrf_id == VRF_UNKNOWN && bgp)
		vrf_id = bgp->vrf_id;

	e = midr_gre_registry_lookup_by_endpoints(vrf_id, local, remote);
	if (!e) {
		zlog_warn("%s: no MIDR GRE tunnel for the given endpoints",
			  __func__);
		if (status) {
			status->state = MIDR_GRE_STATE_FAILED;
			status->err = ENOENT;
			status->vrf_id = vrf_id;
		}
		return -1;
	}

	return midr_gre_interface_del(bgp, e->ifname, status);
}

const char *midr_gre_interface_name(vrf_id_t vrf_id,
				    const struct ipaddr *local,
				    const struct ipaddr *remote)
{
	struct midr_gre_entry *e;

	e = midr_gre_registry_lookup_by_endpoints(vrf_id, local, remote);

	return e ? e->ifname : NULL;
}

int midr_gre_interface_get_state(vrf_id_t vrf_id, const char *ifname,
				 struct midr_gre_status *status)
{
	struct midr_gre_entry *e;
	struct interface *ifp;

	if (!ifname || ifname[0] == '\0')
		return -1;

	e = midr_gre_registry_lookup_by_name(ifname);
	if (!e)
		return -1;

	if (vrf_id != VRF_UNKNOWN && e->vrf_id != vrf_id)
		return -1;

	/* Reconcile an interface that has since disappeared. */
	if (e->state == MIDR_GRE_STATE_UP) {
		ifp = if_lookup_by_name(e->ifname, e->vrf_id);
		if (!ifp || ifp->ifindex == IFINDEX_INTERNAL) {
			e->state = MIDR_GRE_STATE_DOWN;
			e->ifindex = 0;
			e->iftype = 0;
			e->err = ENODEV;
			e->up_since_ms = 0;
			midr_gre_notify(e);
		}
	}

	midr_gre_fill_status(e, status);
	return 0;
}

bool midr_gre_interface_wait_up(struct bgp *bgp, const char *ifname,
				uint32_t timeout_ms, struct midr_gre_status *status)
{
	struct event_loop *master = midr_gre_master();
	struct midr_gre_entry *e;
	uint64_t deadline;
	bool up = false;

	(void)bgp;

	if (status)
		memset(status, 0, sizeof(*status));

	if (!ifname || ifname[0] == '\0')
		return false;

	e = midr_gre_registry_lookup_by_name(ifname);
	if (!e)
		return false;

	/* Fast path: already UP, or zebra already knows the device. */
	if (e->state == MIDR_GRE_STATE_UP || midr_gre_entry_check_present(e)) {
		midr_gre_fill_status(e, status);
		return true;
	}

	if (!master || timeout_ms == 0) {
		midr_gre_fill_status(e, status);
		return e->state == MIDR_GRE_STATE_UP;
	}

	deadline = midr_gre_now_ms() + timeout_ms;
	while (midr_gre_now_ms() < deadline) {
		struct event *wake = NULL;
		struct event ev = {};

		if (e->state == MIDR_GRE_STATE_UP) {
			up = true;
			break;
		}
		if (e->state == MIDR_GRE_STATE_FAILED)
			break;

		/* Sleep for at most 20 ms while still servicing events. */
		event_add_timer_msec(master, midr_gre_wait_tick_cb, NULL, 20,
				     &wake);
		event_fetch(master, &ev);
		event_call(&ev);
		event_cancel(&wake);
	}

	if (!up)
		up = midr_gre_entry_check_present(e);

	midr_gre_fill_status(e, status);
	return up;
}

void midr_gre_register_notify(midr_gre_notify_cb cb, void *arg)
{
	struct midr_gre_notifier *n;

	if (!cb)
		return;

	if (!midr_gre_notifiers)
		midr_gre_notifiers = list_new();

	n = XCALLOC(MTYPE_MIDR_GRE, sizeof(*n));
	n->cb = cb;
	n->arg = arg;

	listnode_add(midr_gre_notifiers, n);
}

void midr_gre_unregister_notify(midr_gre_notify_cb cb)
{
	struct listnode *node, *nnode;
	struct midr_gre_notifier *n;

	if (!midr_gre_notifiers)
		return;

	for (ALL_LIST_ELEMENTS(midr_gre_notifiers, node, nnode, n)) {
		if (n->cb != cb)
			continue;
		list_delete_node(midr_gre_notifiers, node);
		XFREE(MTYPE_MIDR_GRE, n);
	}
}

void midr_gre_fini(void)
{
	struct listnode *node, *nnode;
	struct midr_gre_entry *e;
	struct midr_gre_notifier *n;

	if (midr_gre_registry) {
		for (ALL_LIST_ELEMENTS(midr_gre_registry, node, nnode, e)) {
			midr_gre_confirm_cancel(e);
			XFREE(MTYPE_MIDR_GRE, e);
		}
		list_delete(&midr_gre_registry);
	}

	if (midr_gre_notifiers) {
		for (ALL_LIST_ELEMENTS(midr_gre_notifiers, node, nnode, n))
			XFREE(MTYPE_MIDR_GRE, n);
		list_delete(&midr_gre_notifiers);
	}

	midr_gre_name_seq = 0;
}
