// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR high-level virtual-link device service (midrd/midr-virtual-link.c)
 *
 * See midr-virtual-link.h for the design overview.  This module reuses
 * midr-gre.{c,h} for the netdevice lifecycle and the P1 ZAPI primitives for
 * the overlay address and administrative state, then confirms the result by
 * observing zebra's interface view (if_lookup_by_name() +
 * connected_lookup_prefix_exact() + if_is_up()): no new confirmation ZAPI and
 * no active probe are needed.
 *
 * It owns a small local registry mapping an ifname to the establishment
 * state so add()/del()/get_state()/wait_ready() are idempotent and a device
 * loss can be reported (and rebuilt) without losing the descriptor.
 */

#include <zebra.h>

#include "lib/frrevent.h"
#include "lib/if.h"
#include "lib/ipaddr.h"
#include "lib/linklist.h"
#include "lib/prefix.h"
#include "lib/vrf.h"
#include "lib/zclient.h"

#include "midr-context.h"
#include "midr-dp-backend.h"
#include "midr-gre.h"
#include "midr-virtual-link.h"

/* How often the establishment state machine is re-driven. */
#define MIDR_VLINK_POLL_INTERVAL_MS 50
/* How long the whole establishment may take before it is declared failed. */
#define MIDR_VLINK_CONFIRM_TIMEOUT_MS 3000
/*
 * How often every non-DOWN entry is re-checked against zebra's interface view
 * (autonomous device-loss detection, D1b).  One timer serves the whole module.
 */
#define MIDR_VLINK_RECONCILE_INTERVAL_MS 1000

/* One virtual link under construction / established. */
struct midr_vlink_entry {
	struct midr_virtual_link_desc desc;
	char ifname[IFNAMSIZ]; /* resolved name (never empty) */
	vrf_id_t vrf_id;

	enum midr_virtual_link_state state;
	enum midr_virtual_link_stage stage;
	uint8_t iftype; /* enum zebra_iftype; valid at READY */
	ifindex_t ifindex; /* valid when state >= DEVICE_UP */
	ifindex_t last_ifindex; /* last confirmed ifindex, kept for FAILED (§7.3) */
	int last_error;

	bool gre_created; /* device handed to / known by the GRE layer */
	bool addr_set; /* overlay address-set request was sent */
	bool addr_seen; /* ADDRESS_SET emitted (address visible on device) */
	bool admin_up; /* admin-up request was sent */
	bool deleting; /* teardown: stop the poll timer, ignore GRE loss */

	uint64_t started_ms;
	uint64_t ready_since_ms;
	struct event *t_poll;
};

struct midr_vlink_notifier {
	midr_vlink_notify_cb cb;
	void *arg;
};

static struct list *midr_vlink_registry;
static struct list *midr_vlink_notifiers;
static struct event_loop *midr_vlink_master;
static struct event *midr_vlink_t_reconcile; /* one per module, not per entry */


static uint64_t midr_vlink_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static struct zclient *midr_vlink_zclient(void)
{
	return midr_dp_backend_zclient();
}

static vrf_id_t midr_vlink_resolve_vrf(vrf_id_t vrf_id)
{
	return vrf_id == VRF_UNKNOWN ? midr_dp_backend_vrf_id() : vrf_id;
}

/* Defined below; forward-declared for the state machine. */
static void midr_vlink_poll_cancel(struct midr_vlink_entry *e);
static void midr_vlink_advance(struct midr_vlink_entry *e);
static void midr_vlink_notify(const struct midr_vlink_entry *e,
			      enum midr_virtual_link_event ev);
static void midr_vlink_reconcile_sync(void);

/* ------------------------------------------------------------------ *
 *  Registry
 * ------------------------------------------------------------------ */
static struct list *midr_vlink_registry_get(void)
{
	if (!midr_vlink_registry)
		midr_vlink_registry = list_new();

	return midr_vlink_registry;
}

static struct midr_vlink_entry *
midr_vlink_registry_lookup_by_name(const char *ifname)
{
	struct listnode *node;
	struct midr_vlink_entry *e;

	if (!midr_vlink_registry || !ifname)
		return NULL;

	for (ALL_LIST_ELEMENTS_RO(midr_vlink_registry, node, e)) {
		if (strcmp(e->ifname, ifname) == 0)
			return e;
	}

	return NULL;
}

static struct midr_vlink_entry *
midr_vlink_registry_lookup_by_outer(vrf_id_t vrf_id,
				    const struct ipaddr *local,
				    const struct ipaddr *remote)
{
	struct listnode *node;
	struct midr_vlink_entry *e;

	if (!midr_vlink_registry || !local || !remote)
		return NULL;

	for (ALL_LIST_ELEMENTS_RO(midr_vlink_registry, node, e)) {
		if (e->vrf_id == vrf_id &&
		    ipaddr_cmp(&e->desc.outer_local, local) == 0 &&
		    ipaddr_cmp(&e->desc.outer_remote, remote) == 0)
			return e;
	}

	return NULL;
}

/*
 * Are two descriptors the same virtual link?  The resolved VRF is compared
 * (desc.vrf_id may be VRF_UNKNOWN) and every endpoint/overlay field must
 * match; mtu and the underlay link index are part of the device identity.
 */
static bool midr_vlink_desc_same(const struct midr_vlink_entry *e,
				 const struct midr_virtual_link_desc *d,
				 vrf_id_t vrf_id)
{
	return e->vrf_id == vrf_id &&
	       ipaddr_cmp(&e->desc.outer_local, &d->outer_local) == 0 &&
	       ipaddr_cmp(&e->desc.outer_remote, &d->outer_remote) == 0 &&
	       ipaddr_cmp(&e->desc.overlay_local, &d->overlay_local) == 0 &&
	       ipaddr_cmp(&e->desc.overlay_remote, &d->overlay_remote) == 0 &&
	       e->desc.overlay_prefix_len == d->overlay_prefix_len &&
	       e->desc.outer_link_ifindex == d->outer_link_ifindex &&
	       e->desc.mtu == d->mtu;
}

/* ------------------------------------------------------------------ *
 *  Status reporting
 * ------------------------------------------------------------------ */
static void midr_vlink_fill_status(const struct midr_vlink_entry *e,
				   struct midr_virtual_link_status *st)
{
	if (!st)
		return;

	/* memset() clears event to MIDR_VLINK_EV_NONE, so the query paths
	 * (get_state()/wait_ready()) always report "no transition".  Only
	 * midr_vlink_notify() stamps the real event. */
	memset(st, 0, sizeof(*st));
	st->state = e->state;
	st->stage = e->stage;
	strlcpy(st->ifname, e->ifname, sizeof(st->ifname));
	st->vrf_id = e->vrf_id;
	st->overlay_local = e->desc.overlay_local;
	st->overlay_remote = e->desc.overlay_remote;
	st->overlay_prefix_len = e->desc.overlay_prefix_len;
	st->overlay_ready = (e->state == MIDR_VLINK_READY);

	if (e->state == MIDR_VLINK_READY) {
		st->ifindex = e->ifindex;
		st->iftype = e->iftype;
	} else if (e->state == MIDR_VLINK_FAILED) {
		/*
		 * §7.3: keep the last known ifindex on a configuration
		 * failure, so the first group can locate the device.  §5.3:
		 * a device that disappeared invalidates it (stage
		 * DEVICE_CONFIRM / last_error ENODEV).  NOTE: FAILED(5) >
		 * DEVICE_UP(2), so the numeric test below cannot be used
		 * alone.
		 */
		st->ifindex = (e->stage == MIDR_VLINK_STAGE_DEVICE_CONFIRM)
				      ? 0
				      : e->last_ifindex;
		st->last_error = e->last_error ? e->last_error : EIO;
	} else if (e->state >= MIDR_VLINK_DEVICE_UP) {
		st->ifindex = e->ifindex;
	}

	if (e->ready_since_ms)
		st->ready_ms =
			(uint32_t)(midr_vlink_now_ms() - e->ready_since_ms);
}

/* Report a failure that happened before an entry existed (validation etc.). */
static int midr_vlink_report_fail(struct midr_virtual_link_status *st,
				  const char *ifname, vrf_id_t vrf_id,
				  enum midr_virtual_link_stage stage, int err)
{
	if (st) {
		memset(st, 0, sizeof(*st));
		st->state = MIDR_VLINK_FAILED;
		st->stage = stage;
		st->last_error = err ? err : EIO;
		st->vrf_id = vrf_id;
		if (ifname)
			strlcpy(st->ifname, ifname, sizeof(st->ifname));
	}
	return -1;
}

static void midr_vlink_notify(const struct midr_vlink_entry *e,
			      enum midr_virtual_link_event ev)
{
	struct listnode *node;
	struct midr_vlink_notifier *n;
	struct midr_virtual_link_status st;

	if (!midr_vlink_notifiers)
		return;

	midr_vlink_fill_status(e, &st);
	st.event = ev;

	for (ALL_LIST_ELEMENTS_RO(midr_vlink_notifiers, node, n)) {
		if (n->cb)
			n->cb(&st, n->arg);
	}
}

/* ------------------------------------------------------------------ *
 *  Validation (§4.2) and zebra-view confirmation (§5.4)
 * ------------------------------------------------------------------ */
static void midr_vlink_prefix_from_ipaddr(const struct ipaddr *addr,
					  uint8_t prefixlen,
					  struct prefix *p)
{
	memset(p, 0, sizeof(*p));
	p->family = ipaddr_family(addr);
	p->prefixlen = prefixlen;
	if (IS_IPADDR_V4(addr))
		p->u.prefix4 = addr->ipaddr_v4;
	else
		p->u.prefix6 = addr->ipaddr_v6;
}

/* overlay_remote must fall inside overlay_local/overlay_prefix_len (v1). */
static bool
midr_vlink_remote_in_prefix(const struct ipaddr *local, uint8_t prefixlen,
			    const struct ipaddr *remote)
{
	struct prefix net, host;

	midr_vlink_prefix_from_ipaddr(local, prefixlen, &net);
	midr_vlink_prefix_from_ipaddr(
		remote, IS_IPADDR_V4(remote) ? IPV4_MAX_BITLEN : IPV6_MAX_BITLEN,
		&host);

	return prefix_match(&net, &host);
}

static int midr_vlink_validate(const struct midr_virtual_link_desc *d)
{
	uint8_t max_len;

	if (!d)
		return EINVAL;

	if (IS_IPADDR_NONE(&d->outer_local) ||
	    IS_IPADDR_NONE(&d->outer_remote))
		return EINVAL;
	if (ipaddr_family(&d->outer_local) != ipaddr_family(&d->outer_remote))
		return EAFNOSUPPORT;

	if (IS_IPADDR_NONE(&d->overlay_local) ||
	    IS_IPADDR_NONE(&d->overlay_remote))
		return EINVAL;
	if (ipaddr_family(&d->overlay_local) !=
	    ipaddr_family(&d->overlay_remote))
		return EAFNOSUPPORT;

	max_len = IS_IPADDR_V4(&d->overlay_local) ? IPV4_MAX_BITLEN
						  : IPV6_MAX_BITLEN;
	if (d->overlay_prefix_len == 0 || d->overlay_prefix_len > max_len)
		return EINVAL;

	if (!midr_vlink_remote_in_prefix(&d->overlay_local,
					 d->overlay_prefix_len,
					 &d->overlay_remote))
		return EINVAL;

	return 0;
}

bool midr_virtual_link_probe_device(const char *ifname, vrf_id_t vrf_id,
				    ifindex_t *ifindex, uint8_t *iftype)
{
	struct interface *ifp;

	if (!ifname || !ifindex)
		return false;

	ifp = if_lookup_by_name(ifname, vrf_id);
	if (!ifp || ifp->ifindex == IFINDEX_INTERNAL)
		return false;

	*ifindex = ifp->ifindex;
	if (iftype)
		*iftype = ifp->zif_type;
	return true;
}

bool midr_virtual_link_overlay_reachable(const char *ifname, vrf_id_t vrf_id,
					 const struct ipaddr *overlay_local,
					 uint8_t prefix_len)
{
	struct interface *ifp;
	struct prefix p;

	ifp = if_lookup_by_name(ifname, vrf_id);
	if (!ifp)
		return false;

	midr_vlink_prefix_from_ipaddr(overlay_local, prefix_len, &p);
	if (!connected_lookup_prefix_exact(ifp, &p))
		return false;

	return if_is_up(ifp);
}

/*
 * ADDRESS_SET milestone: is the overlay local/prefix already visible on the
 * device in the daemon's interface view?  Unlike the public
 * midr_virtual_link_overlay_reachable() this deliberately ignores IFF_UP, so
 * the module can emit ADDRESS_SET ("address present, link not up yet") as a
 * transition distinct from READY.  Static on purpose: it is production logic,
 * not a test seam, and adds no external symbol.
 */
static bool midr_vlink_overlay_addr_present(const char *ifname,
					    vrf_id_t vrf_id,
					    const struct ipaddr *overlay_local,
					    uint8_t prefix_len)
{
	struct interface *ifp;
	struct prefix p;

	ifp = if_lookup_by_name(ifname, vrf_id);
	if (!ifp)
		return false;

	midr_vlink_prefix_from_ipaddr(overlay_local, prefix_len, &p);
	return connected_lookup_prefix_exact(ifp, &p) != NULL;
}

/* ------------------------------------------------------------------ *
 *  Establishment state machine (§5.1/§5.2) and rollback (§7.3)
 * ------------------------------------------------------------------ */
static void midr_vlink_poll_cb(struct event *t);

static void midr_vlink_poll_cancel(struct midr_vlink_entry *e)
{
	if (e->t_poll)
		event_cancel(&e->t_poll);
}

static void midr_vlink_poll_start(struct midr_vlink_entry *e)
{
	if (e->t_poll || !midr_vlink_master)
		return;

	event_add_timer_msec(midr_vlink_master, midr_vlink_poll_cb, e,
			     MIDR_VLINK_POLL_INTERVAL_MS, &e->t_poll);
}

static void midr_vlink_poll_cb(struct event *t)
{
	struct midr_vlink_entry *e = EVENT_ARG(t);

	e->t_poll = NULL;
	if (e->deleting)
		return;

	midr_vlink_advance(e);

	/* Keep polling while the establishment is still in progress. */
	if (e->state == MIDR_VLINK_CREATING ||
	    e->state == MIDR_VLINK_DEVICE_UP ||
	    e->state == MIDR_VLINK_CONFIGURING)
		midr_vlink_poll_start(e);
}

/*
 * Undo whatever the establishment already did: withdraw the overlay address
 * (if it was sent) and delete the GRE device.  Best-effort: a failed rollback
 * leaves the entry in FAILED so the first group can retry or rebuild.
 */
static void midr_vlink_rollback(struct midr_vlink_entry *e)
{
	struct zclient *zc = midr_vlink_zclient();

	/*
	 * Suppress the GRE layer's own DOWN notification for this teardown:
	 * midr_gre_interface_del() below calls midr_gre_notify() synchronously
	 * and we are already handling (or about to report) this failure.  The
	 * flag also keeps the per-entry poll timer from re-arming while the
	 * entry is being unwound.
	 */
	e->deleting = true;

	if (e->addr_set && zc && zc->sock >= 0) {
		struct zclient_interface_address za = {};

		strlcpy(za.ifname, e->ifname, sizeof(za.ifname));
		za.ifindex = e->ifindex;
		za.addr = e->desc.overlay_local;
		za.prefixlen = e->desc.overlay_prefix_len;
		SET_IPADDR_NONE(&za.peer);
		zclient_send_interface_address_unset(zc, e->vrf_id, &za);
	}
	e->addr_set = false;
	e->admin_up = false;

	if (e->gre_created) {
		midr_gre_interface_del(NULL, e->ifname, NULL);
		e->gre_created = false;
	}

	/* §5.3/§7.3: ifindex is invalidated here, but last_ifindex is kept so
	 * a configuration failure can still report it. */
	e->ifindex = 0;
	e->iftype = 0;
}

static void midr_vlink_fail(struct midr_vlink_entry *e,
			    enum midr_virtual_link_stage stage, int err)
{
	midr_vlink_poll_cancel(e);
	e->state = MIDR_VLINK_FAILED;
	e->stage = stage;
	e->last_error = err ? err : EIO;
	e->ready_since_ms = 0;
	midr_vlink_notify(e, MIDR_VLINK_EV_FAILED);
}

/*
 * Autonomous device-loss handling (D1).  Move an entry to FAILED because
 * zebra's view says its device is gone (or, for a READY link, the overlay
 * address / IFF_UP was lost).  Contract §5.3: invalidate the old ifindex and
 * retain the descriptor so the caller can rebuild.  D4: re-sync
 * gre_created/addr_set against the GRE layer so a later add()/del() does not
 * act on a stale view.  Emits the FAILED notification without any polling by
 * the caller.
 */
static void midr_vlink_lost(struct midr_vlink_entry *e, int err)
{
	struct midr_gre_status gst = {};

	if (e->state == MIDR_VLINK_FAILED || e->deleting)
		return;

	midr_vlink_poll_cancel(e);
	e->state = MIDR_VLINK_FAILED;
	e->stage = MIDR_VLINK_STAGE_DEVICE_CONFIRM;
	e->last_error = err ? err : ENODEV;
	e->ifindex = 0;
	e->iftype = 0;
	e->ready_since_ms = 0;
	e->admin_up = false;

	/* D4: reconcile the bookkeeping against the GRE layer's view. */
	if (midr_gre_interface_get_state(e->vrf_id, e->ifname, &gst) == 0)
		e->gre_created = (gst.state != MIDR_GRE_STATE_DOWN);
	else
		e->gre_created = false;
	e->addr_set = false;

	midr_vlink_notify(e, MIDR_VLINK_EV_FAILED);
}

/*
 * Reconcile one entry against zebra's interface view.  CREATING is left to
 * the per-entry poll timer (which owns the confirmation timeout); DOWN and
 * FAILED have nothing to re-check.  For entries that already reached READY
 * the overlay address and IFF_UP are re-checked as well (contract §8.2).
 */
static void midr_vlink_reconcile_entry(struct midr_vlink_entry *e)
{
	ifindex_t idx = 0;
	uint8_t type = 0;

	if (e->deleting)
		return;
	if (e->state != MIDR_VLINK_DEVICE_UP &&
	    e->state != MIDR_VLINK_CONFIGURING &&
	    e->state != MIDR_VLINK_READY)
		return;

	if (!midr_virtual_link_probe_device(e->ifname, e->vrf_id, &idx, &type)) {
		/* §5.3: the device disappeared -> FAILED, ifindex invalidated. */
		midr_vlink_lost(e, ENODEV);
		return;
	}

	if (idx && idx != e->ifindex) {
		e->ifindex = idx;
		e->last_ifindex = idx;
		if (type)
			e->iftype = type;
	}

	if (e->state == MIDR_VLINK_READY &&
	    !midr_virtual_link_overlay_reachable(
		    e->ifname, e->vrf_id, &e->desc.overlay_local,
		    e->desc.overlay_prefix_len))
		midr_vlink_lost(e, ENODEV);
}

/*
 * D1a: subscribe to the GRE layer.  When it reports DOWN/FAILED for a device
 * this module owns while the overlay was established, fail the entry and
 * notify - no get_state() polling needed.  Frames from our own rollback or
 * delete are suppressed through e->deleting.
 */
static void midr_vlink_gre_notify(const struct midr_gre_status *gst, void *arg)
{
	struct midr_vlink_entry *e;

	(void)arg;

	if (!gst || gst->ifname[0] == '\0')
		return;
	if (gst->state != MIDR_GRE_STATE_DOWN &&
	    gst->state != MIDR_GRE_STATE_FAILED)
		return;

	e = midr_vlink_registry_lookup_by_name(gst->ifname);
	if (!e || e->deleting)
		return;
	if (e->state != MIDR_VLINK_DEVICE_UP &&
	    e->state != MIDR_VLINK_CONFIGURING &&
	    e->state != MIDR_VLINK_READY)
		return;

	midr_vlink_lost(e, ENODEV);
}

/* D1b: one module-wide timer re-drives every non-DOWN entry. */
static void midr_vlink_reconcile_cb(struct event *t)
{
	struct listnode *node, *nnode;
	struct midr_vlink_entry *e;

	(void)t;
	midr_vlink_t_reconcile = NULL;

	if (midr_vlink_registry) {
		for (ALL_LIST_ELEMENTS(midr_vlink_registry, node, nnode, e))
			midr_vlink_reconcile_entry(e);
	}

	midr_vlink_reconcile_sync();
}

/*
 * Arm exactly one reconcile timer while the registry holds any entry that is
 * not DOWN (READY included), and disarm it when the registry is idle.
 */
static void midr_vlink_reconcile_sync(void)
{
	struct listnode *node;
	struct midr_vlink_entry *e;
	bool wanted = false;

	if (midr_vlink_registry) {
		for (ALL_LIST_ELEMENTS_RO(midr_vlink_registry, node, e)) {
			if (e->state != MIDR_VLINK_DOWN) {
				wanted = true;
				break;
			}
		}
	}

	if (!wanted || !midr_vlink_master) {
		if (midr_vlink_t_reconcile)
			event_cancel(&midr_vlink_t_reconcile);
		return;
	}

	if (midr_vlink_t_reconcile)
		return;

	event_add_timer_msec(midr_vlink_master, midr_vlink_reconcile_cb, NULL,
			     MIDR_VLINK_RECONCILE_INTERVAL_MS,
			     &midr_vlink_t_reconcile);
}

/*
 * Drive the establishment forward one (or more) steps.  Each step either
 * makes progress, waits for zebra, or fails with rollback.  Called both from
 * the per-entry poll timer and synchronously from add()/wait_ready().
 */
static void midr_vlink_advance(struct midr_vlink_entry *e)
{
	struct zclient *zc;
	uint64_t now = midr_vlink_now_ms();

	for (;;) {
		switch (e->state) {
		case MIDR_VLINK_CREATING: {
			ifindex_t idx = 0;
			uint8_t type = 0;

			if (midr_virtual_link_probe_device(
				    e->ifname, e->vrf_id, &idx, &type)) {
				e->ifindex = idx;
				e->last_ifindex = idx;
				if (type)
					e->iftype = type;
				e->state = MIDR_VLINK_DEVICE_UP;
				midr_vlink_notify(e, MIDR_VLINK_EV_DEVICE_UP);
				continue;
			}
			if (now - e->started_ms >= MIDR_VLINK_CONFIRM_TIMEOUT_MS) {
				midr_vlink_rollback(e);
				midr_vlink_fail(e,
						MIDR_VLINK_STAGE_DEVICE_CONFIRM,
						ETIMEDOUT);
			}
			return;
		}
		case MIDR_VLINK_DEVICE_UP: {
			struct zclient_interface_address za = {};

			zc = midr_vlink_zclient();
			if (!zc || zc->sock < 0) {
				midr_vlink_rollback(e);
				midr_vlink_fail(e, MIDR_VLINK_STAGE_GRE_CREATE,
						ENOTCONN);
				return;
			}

			strlcpy(za.ifname, e->ifname, sizeof(za.ifname));
			za.ifindex = e->ifindex;
			za.addr = e->desc.overlay_local;
			za.prefixlen = e->desc.overlay_prefix_len;
			SET_IPADDR_NONE(&za.peer);

			if (zclient_send_interface_address_set(
				    zc, e->vrf_id, &za) ==
			    ZCLIENT_SEND_FAILURE) {
				midr_vlink_rollback(e);
				midr_vlink_fail(e,
						MIDR_VLINK_STAGE_ADDRESS_SET,
						EIO);
				return;
			}
			e->addr_set = true;

			if (zclient_send_interface_admin_up(
				    zc, e->vrf_id, e->ifname, true) ==
			    ZCLIENT_SEND_FAILURE) {
				midr_vlink_rollback(e);
				midr_vlink_fail(e, MIDR_VLINK_STAGE_ADMIN_UP,
						EIO);
				return;
			}
			e->admin_up = true;
			e->state = MIDR_VLINK_CONFIGURING;
			continue;
		}
		case MIDR_VLINK_CONFIGURING: {
			/*
			 * ADDRESS_SET milestone: the overlay local/prefix is
			 * now visible on the device.  It is emitted as its own
			 * transition before READY; overlay_ready is still false
			 * here.  The check deliberately ignores IFF_UP --
			 * midr_virtual_link_overlay_reachable() would fold
			 * ADDRESS_SET and READY together.  Wait a tick before
			 * testing READY so DEVICE_UP -> ADDRESS_SET -> READY
			 * stay three distinct, ordered notifications.
			 */
			if (!e->addr_seen) {
				if (midr_vlink_overlay_addr_present(
					    e->ifname, e->vrf_id,
					    &e->desc.overlay_local,
					    e->desc.overlay_prefix_len)) {
					e->addr_seen = true;
					midr_vlink_notify(
						e, MIDR_VLINK_EV_ADDRESS_SET);
				} else if (now - e->started_ms >=
					   MIDR_VLINK_CONFIRM_TIMEOUT_MS) {
					midr_vlink_rollback(e);
					midr_vlink_fail(
						e,
						MIDR_VLINK_STAGE_ADDRESS_SET,
						ETIMEDOUT);
				}
				return;
			}

			if (midr_virtual_link_overlay_reachable(
				    e->ifname, e->vrf_id,
				    &e->desc.overlay_local,
				    e->desc.overlay_prefix_len)) {
				e->state = MIDR_VLINK_READY;
				e->stage = MIDR_VLINK_STAGE_NONE;
				e->last_error = 0;
				e->ready_since_ms = now;
				midr_vlink_poll_cancel(e);
				midr_vlink_notify(e, MIDR_VLINK_EV_READY);
				midr_vlink_reconcile_sync();
				return;
			}

			if (now - e->started_ms >= MIDR_VLINK_CONFIRM_TIMEOUT_MS) {
				midr_vlink_rollback(e);
				midr_vlink_fail(
					e, MIDR_VLINK_STAGE_REACHABILITY,
					ETIMEDOUT);
			}
			return;
		}
		case MIDR_VLINK_DOWN:
		case MIDR_VLINK_READY:
		case MIDR_VLINK_FAILED:
			/* DOWN is idle (no request outstanding) and READY/FAILED
			 * are terminal until the caller acts on them, so there is
			 * nothing to advance here. */
			return;
		}
	}
}
/* ------------------------------------------------------------------ *
 *  Public API
 * ------------------------------------------------------------------ */
static void midr_vlink_gre_from_desc(const struct midr_virtual_link_desc *d,
				     vrf_id_t vrf_id,
				     struct midr_gre_tunnel *tun)
{
	memset(tun, 0, sizeof(*tun));
	if (d->ifname[0])
		strlcpy(tun->ifname, d->ifname, sizeof(tun->ifname));
	tun->vrf_id = vrf_id;
	tun->local = d->outer_local;
	tun->remote = d->outer_remote;
	tun->link_ifindex = d->outer_link_ifindex;
	tun->mtu = d->mtu;
}

int midr_virtual_link_add(struct midr_context *ctx,
			  const struct midr_virtual_link_desc *desc,
			  struct midr_virtual_link_status *status)
{
	struct midr_vlink_entry *e;
	struct midr_gre_tunnel tun;
	struct midr_gre_status gst = {};
	const char *name;
	vrf_id_t vrf_id;
	int err;

	(void)ctx;
	if (status)
		memset(status, 0, sizeof(*status));

	err = midr_vlink_validate(desc);
	if (err)
		return midr_vlink_report_fail(
			status, desc ? desc->ifname : NULL,
			desc ? midr_vlink_resolve_vrf(desc->vrf_id)
			     : VRF_UNKNOWN,
			MIDR_VLINK_STAGE_VALIDATE, err);

	vrf_id = midr_vlink_resolve_vrf(desc->vrf_id);

	/* Idempotency / rebind against an already-known link (§7.1/§7.2). */
	e = NULL;
	if (desc->ifname[0]) {
		e = midr_vlink_registry_lookup_by_name(desc->ifname);
		if (e && midr_vlink_desc_same(e, desc, vrf_id)) {
			if (e->state != MIDR_VLINK_FAILED) {
				midr_vlink_fill_status(e, status);
				return 0;
			}
			/* FAILED: tear down and retry the same desc below. */
		} else if (e) {
			if (!(desc->flags & MIDR_VLINK_F_REBIND))
				return midr_vlink_report_fail(
					status, desc->ifname, vrf_id,
					MIDR_VLINK_STAGE_VALIDATE,
					EEXIST);
			/* Explicit rebind: tear the old link down below. */
		}
	} else {
		e = midr_vlink_registry_lookup_by_outer(
			vrf_id, &desc->outer_local, &desc->outer_remote);
		if (e && midr_vlink_desc_same(e, desc, vrf_id) &&
		    e->state != MIDR_VLINK_FAILED) {
			midr_vlink_fill_status(e, status);
			return 0;
		}
	}

	/*
	 * D3: an entry already exists for this name (explicit rebind or a
	 * FAILED retry) or for this endpoint pair (empty ifname).  A bare
	 * listnode_delete()+free() used to leak the old overlay address and
	 * leave the GRE-layer device in place; the following
	 * midr_gre_interface_add() then reuses that same device (the GRE
	 * layer keeps one entry per endpoint pair), stacking the new overlay
	 * address on the old one -- exactly the silent overwrite §7.2
	 * forbids.  Tear the old link down completely (address unset, GRE
	 * delete, timers) so the rebuild starts from a clean slate.
	 */
	if (e) {
		midr_virtual_link_del(NULL, e->ifname, NULL);
		e = NULL;
	}

	if (!midr_vlink_zclient() || midr_vlink_zclient()->sock < 0)
		return midr_vlink_report_fail(status, desc->ifname, vrf_id,
					      MIDR_VLINK_STAGE_GRE_CREATE,
					      ENOTCONN);

	/*
	 * The GRE layer keys its registry by (vrf, outer local, outer remote)
	 * as well as by device name, and re-adding an already-known endpoint
	 * pair under a *different* name makes that name win without
	 * recreating the kernel device.  The device would then still be
	 * called by the old name, this link's probe for the new name would
	 * never be confirmed and the request would end in a confusing
	 * timeout.  The same trap exists for an auto-generated name: the
	 * caller asked for "midr-gre-<n>" but the GRE layer would hand back
	 * the existing device's old name.
	 *
	 * Task spec §6 forbids silently overwriting a binding, so refuse it
	 * explicitly (EEXIST): the caller must tear the old link down first
	 * (or reuse its name).  A DOWN/FAILED entry carries no live device
	 * (the common post-device-loss case), so re-adding simply refreshes
	 * it and must NOT be refused.  Any vlink entry for this endpoint
	 * pair was already torn down above, so a *live* name still known
	 * here belongs to a foreign GRE tunnel and must not be rebound
	 * either.
	 */
	name = midr_gre_interface_name(vrf_id, &desc->outer_local,
				       &desc->outer_remote);
	if (name && (!desc->ifname[0] || strcmp(name, desc->ifname) != 0)) {
		struct midr_gre_status cur = {};

		if (midr_gre_interface_get_state(vrf_id, name, &cur) == 0 &&
		    cur.state != MIDR_GRE_STATE_DOWN &&
		    cur.state != MIDR_GRE_STATE_FAILED)
			return midr_vlink_report_fail(
				status, desc->ifname[0] ? desc->ifname : name,
				vrf_id, MIDR_VLINK_STAGE_VALIDATE, EEXIST);
	}

	midr_vlink_gre_from_desc(desc, vrf_id, &tun);
	if (midr_gre_interface_add(ctx, &tun, &gst) != 0)
		return midr_vlink_report_fail(
			status, desc->ifname, vrf_id,
			MIDR_VLINK_STAGE_GRE_CREATE,
			gst.err ? gst.err : EIO);

	name = desc->ifname[0] ? desc->ifname : gst.ifname;
	if (!name || name[0] == '\0') {
		midr_gre_interface_del(NULL, tun.ifname, NULL);
		return midr_vlink_report_fail(status, desc->ifname, vrf_id,
					      MIDR_VLINK_STAGE_GRE_CREATE,
					      EIO);
	}

	e = calloc(1, sizeof(*e));
	if (!e) {
		midr_gre_interface_del(NULL, name, NULL);
		return midr_vlink_report_fail(status, desc->ifname, vrf_id,
					      MIDR_VLINK_STAGE_GRE_CREATE,
					      ENOMEM);
	}

	e->desc = *desc;
	e->desc.vrf_id = vrf_id;
	strlcpy(e->ifname, name, sizeof(e->ifname));
	e->vrf_id = vrf_id;
	e->state = MIDR_VLINK_CREATING;
	e->stage = MIDR_VLINK_STAGE_NONE;
	e->gre_created = true;
	e->started_ms = midr_vlink_now_ms();

	listnode_add(midr_vlink_registry_get(), e);
	midr_vlink_poll_start(e);
	midr_vlink_reconcile_sync();
	midr_vlink_advance(e);

	midr_vlink_fill_status(e, status);
	return e->state == MIDR_VLINK_FAILED ? -1 : 0;
}


int midr_virtual_link_del(struct midr_context *ctx, const char *ifname,
			  struct midr_virtual_link_status *status)
{
	struct midr_vlink_entry *e;
	struct zclient *zc;

	(void)ctx;
	if (status)
		memset(status, 0, sizeof(*status));

	if (!ifname || ifname[0] == '\0')
		return midr_vlink_report_fail(status, ifname, VRF_UNKNOWN,
					      MIDR_VLINK_STAGE_VALIDATE,
					      EINVAL);

	e = midr_vlink_registry_lookup_by_name(ifname);
	if (!e)
		return midr_vlink_report_fail(status, ifname, VRF_UNKNOWN,
					      MIDR_VLINK_STAGE_VALIDATE,
					      ENOENT);

	/* 1. stop new configuration and state notifications for this link. */
	e->deleting = true;
	midr_vlink_poll_cancel(e);

	/* 2. withdraw the overlay address. */
	zc = midr_vlink_zclient();
	if (e->addr_set && zc && zc->sock >= 0) {
		struct zclient_interface_address za = {};

		strlcpy(za.ifname, e->ifname, sizeof(za.ifname));
		za.ifindex = e->ifindex;
		za.addr = e->desc.overlay_local;
		za.prefixlen = e->desc.overlay_prefix_len;
		SET_IPADDR_NONE(&za.peer);
		zclient_send_interface_address_unset(zc, e->vrf_id, &za);
	}
	e->addr_set = false;

	/* 3. delete the GRE device. */
	if (e->gre_created) {
		midr_gre_interface_del(NULL, e->ifname, NULL);
		e->gre_created = false;
	}

	/* 4/5. clear the registry entry and report DOWN. */
	e->state = MIDR_VLINK_DOWN;
	e->stage = MIDR_VLINK_STAGE_NONE;
	e->last_error = 0;
	e->ifindex = 0;
	e->iftype = 0;
	e->ready_since_ms = 0;
	midr_vlink_notify(e, MIDR_VLINK_EV_DOWN);
	midr_vlink_fill_status(e, status);
	listnode_delete(midr_vlink_registry, e);
	free(e);
	midr_vlink_reconcile_sync();

	zlog_info("MIDR virtual link %s removed", ifname);
	return 0;
}

int midr_virtual_link_get_state(struct midr_context *ctx, const char *ifname,
				struct midr_virtual_link_status *status)
{
	struct midr_vlink_entry *e;

	(void)ctx;
	if (status)
		memset(status, 0, sizeof(*status));

	if (!ifname || ifname[0] == '\0')
		return -1;

	e = midr_vlink_registry_lookup_by_name(ifname);
	if (!e)
		return -1;

	/*
	 * Pure query (see the event enum): all transitions and notifications
	 * are owned by the establishment poll timer, the GRE notify
	 * subscription and the reconcile timer, so get_state() must not emit
	 * anything itself.  The returned status always carries
	 * event == MIDR_VLINK_EV_NONE (memset in midr_vlink_fill_status()).
	 */
	midr_vlink_fill_status(e, status);
	return 0;
}

static void midr_vlink_wait_tick_cb(struct event *t)
{
	(void)t;
}

bool midr_virtual_link_wait_ready(struct midr_context *ctx, const char *ifname,
				  uint32_t timeout_ms,
				  struct midr_virtual_link_status *status)
{
	struct event_loop *master = midr_vlink_master;
	struct midr_vlink_entry *e;
	uint64_t deadline;
	bool ready = false;

	(void)ctx;
	if (status)
		memset(status, 0, sizeof(*status));

	if (!ifname || ifname[0] == '\0')
		return false;

	e = midr_vlink_registry_lookup_by_name(ifname);
	if (!e)
		return false;

	if (e->state == MIDR_VLINK_READY) {
		midr_vlink_fill_status(e, status);
		return true;
	}
	if (e->state == MIDR_VLINK_FAILED) {
		midr_vlink_fill_status(e, status);
		return false;
	}

	if (!master || timeout_ms == 0) {
		midr_vlink_advance(e);
		midr_vlink_fill_status(e, status);
		return e->state == MIDR_VLINK_READY;
	}

	deadline = midr_vlink_now_ms() + timeout_ms;
	while (midr_vlink_now_ms() < deadline) {
		struct event *wake = NULL;
		struct event ev = {};

		midr_vlink_advance(e);
		if (e->state == MIDR_VLINK_READY) {
			ready = true;
			break;
		}
		if (e->state == MIDR_VLINK_FAILED)
			break;

		/* Sleep for at most 20 ms while still servicing events. */
		event_add_timer_msec(master, midr_vlink_wait_tick_cb, NULL, 20,
				     &wake);
		event_fetch(master, &ev);
		event_call(&ev);
		event_cancel(&wake);
	}

	midr_vlink_advance(e);
	if (e->state == MIDR_VLINK_READY)
		ready = true;

	midr_vlink_fill_status(e, status);
	return ready;
}


void midr_virtual_link_register_notify(midr_vlink_notify_cb cb, void *arg)
{
	struct midr_vlink_notifier *n;

	if (!cb)
		return;

	if (!midr_vlink_notifiers)
		midr_vlink_notifiers = list_new();

	n = calloc(1, sizeof(*n));
	n->cb = cb;
	n->arg = arg;

	listnode_add(midr_vlink_notifiers, n);
}

void midr_virtual_link_unregister_notify(midr_vlink_notify_cb cb)
{
	struct listnode *node, *nnode;
	struct midr_vlink_notifier *n;

	if (!midr_vlink_notifiers)
		return;

	for (ALL_LIST_ELEMENTS(midr_vlink_notifiers, node, nnode, n)) {
		if (n->cb != cb)
			continue;
		list_delete_node(midr_vlink_notifiers, node);
		free(n);
	}
}

const char *midr_virtual_link_state_str(enum midr_virtual_link_state state)
{
	switch (state) {
	case MIDR_VLINK_DOWN:
		return "down";
	case MIDR_VLINK_CREATING:
		return "creating";
	case MIDR_VLINK_DEVICE_UP:
		return "device_up";
	case MIDR_VLINK_CONFIGURING:
		return "configuring";
	case MIDR_VLINK_READY:
		return "ready";
	case MIDR_VLINK_FAILED:
		return "failed";
	}

	return "unknown";
}

const char *midr_virtual_link_event_str(enum midr_virtual_link_event event)
{
	switch (event) {
	case MIDR_VLINK_EV_NONE:
		return "none";
	case MIDR_VLINK_EV_DEVICE_UP:
		return "device_up";
	case MIDR_VLINK_EV_ADDRESS_SET:
		return "address_set";
	case MIDR_VLINK_EV_READY:
		return "ready";
	case MIDR_VLINK_EV_FAILED:
		return "failed";
	case MIDR_VLINK_EV_DOWN:
		return "down";
	}

	return "unknown";
}

void midr_virtual_link_init(struct event_loop *master)
{
	midr_vlink_master = master;

	/* D1a: react autonomously to a GRE-layer DOWN/FAILED for our devices. */
	midr_gre_register_notify(midr_vlink_gre_notify, NULL);
}

void midr_virtual_link_fini(void)
{
	struct listnode *node, *nnode;
	struct midr_vlink_entry *e;
	struct midr_vlink_notifier *n;

	midr_gre_unregister_notify(midr_vlink_gre_notify);

	if (midr_vlink_t_reconcile)
		event_cancel(&midr_vlink_t_reconcile);

	if (midr_vlink_registry) {
		for (ALL_LIST_ELEMENTS(midr_vlink_registry, node, nnode, e)) {
			midr_vlink_poll_cancel(e);
			free(e);
		}
		list_delete(&midr_vlink_registry);
	}

	if (midr_vlink_notifiers) {
		for (ALL_LIST_ELEMENTS(midr_vlink_notifiers, node, nnode, n))
			free(n);
		list_delete(&midr_vlink_notifiers);
	}

	midr_vlink_master = NULL;
}
