// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR high-level virtual-link device service (midr-virtual-link.h)
 *
 * =========================================================================
 *  Purpose
 * =========================================================================
 * midr-gre.{c,h} only creates/removes a GRE/ip6gre netdevice and reports
 * when zebra has recognised it.  A virtual link that the control plane can
 * actually use also needs an overlay address configured on that device and
 * the interface brought administratively UP.  This module is the *only*
 * CP-facing entry point for that full sequence:
 *
 *   outer endpoints validated
 *     -> GRE device created (via midr-gre)
 *     -> ifindex confirmed by zebra
 *     -> overlay local/prefix configured (via the P1 ZAPI primitive)
 *     -> interface admin UP   (via the P1 ZAPI primitive)
 *     -> overlay data plane confirmed reachable
 *     -> READY
 *
 * =========================================================================
 *  Design
 * =========================================================================
 *   - The CP describes a virtual link by its two GRE endpoints plus the
 *     overlay local/remote address and prefix.  It never touches netlink,
 *     the kernel or shell.
 *   - Only MIDR_VLINK_READY means "device present + overlay address
 *     configured + interface UP".  MIDR_VLINK_DEVICE_UP means zebra knows
 *     the device (ifindex valid) but the overlay is not configured yet and
 *     must NOT be treated as READY.
 *   - add() is asynchronous like midr_gre_interface_add(): a return of 0
 *     only means "accepted"; wait_ready() or the notify callback provide
 *     the final verdict.
 *   - The descriptor is retained across a device loss so the first group
 *     can decide whether to rebuild; a rebuild returns a fresh ifindex.
 *   - This module never calls midr_session_connect(),
 *     midr_topology_link_upsert()/withdraw(), never allocates a first-group
 *     link_id and never decides physical-vs-virtual.
 *
 * wire path:
 *   CP (first group)
 *      |  midr_virtual_link_add()/del()/get_state()/wait_ready()
 *      v
 *   midr-virtual-link.c
 *      |-- midr_gre_interface_add()/del()      (midrd/midr-gre.c)
 *      |-- zclient_send_interface_address_set()/_unset()  (P1, lib/zclient.c)
 *      |-- zclient_send_interface_admin_up()              (P1, lib/zclient.c)
 *      v
 *   zebra  ->  kernel netdevice
 */

#ifndef MIDRD_MIDR_VIRTUAL_LINK_H
#define MIDRD_MIDR_VIRTUAL_LINK_H

#include <zebra.h>

#include <stdbool.h>
#include <stdint.h>

#include "lib/if.h"
#include "lib/ipaddr.h"
#include "lib/vrf.h"

#ifdef __cplusplus
extern "C" {
#endif

struct midr_context;
struct event_loop;

/*
 * Externally observable state.  Only MIDR_VLINK_READY means the device,
 * overlay address and interface state are all done.
 */
enum midr_virtual_link_state {
	MIDR_VLINK_DOWN = 0, /* not requested, or removed */
	MIDR_VLINK_CREATING, /* GRE create sent, waiting for ifindex */
	MIDR_VLINK_DEVICE_UP, /* zebra knows the device (ifindex valid) */
	MIDR_VLINK_CONFIGURING, /* overlay address / link state being applied */
	MIDR_VLINK_READY, /* device + address + link UP all done */
	MIDR_VLINK_FAILED, /* some step failed or was rejected */
};

/* Failure stage, to locate which step did not pass. */
enum midr_virtual_link_stage {
	MIDR_VLINK_STAGE_NONE = 0,
	MIDR_VLINK_STAGE_VALIDATE,
	MIDR_VLINK_STAGE_GRE_CREATE,
	MIDR_VLINK_STAGE_DEVICE_CONFIRM,
	MIDR_VLINK_STAGE_ADDRESS_SET,
	MIDR_VLINK_STAGE_ADMIN_UP,
	MIDR_VLINK_STAGE_REACHABILITY,
};

/*
 * The transition that fired a notification.  MIDR_VLINK_EV_NONE is used only
 * by the query paths (get_state()/wait_ready()); they never emit a callback
 * and always report event == MIDR_VLINK_EV_NONE.  On the establishment path
 * DEVICE_UP -> ADDRESS_SET -> READY are three distinct notifications in that
 * order.  ADDRESS_SET means the overlay local/prefix is already visible on
 * the device in the daemon's interface view while IFF_UP is not required yet
 * (overlay_ready is still false at that point).
 */
enum midr_virtual_link_event {
	MIDR_VLINK_EV_NONE = 0,    /* query path only (get_state/wait_ready) */
	MIDR_VLINK_EV_DEVICE_UP,   /* zebra confirmed the device */
	MIDR_VLINK_EV_ADDRESS_SET, /* overlay local/prefix visible on device */
	MIDR_VLINK_EV_READY,       /* device + overlay address + IFF_UP */
	MIDR_VLINK_EV_FAILED,      /* a stage failed, incl. autonomous loss */
	MIDR_VLINK_EV_DOWN,        /* deleted */
};

/* desc flags. */
#define MIDR_VLINK_F_REBIND 0x1u /* allow same ifname -> new endpoint/overlay */

struct midr_virtual_link_desc {
	char ifname[IFNAMSIZ]; /* may be empty: auto "midr-gre-<n>" */
	vrf_id_t vrf_id;

	/* GRE outer / underlay endpoints. */
	struct ipaddr outer_local;
	struct ipaddr outer_remote;
	ifindex_t outer_link_ifindex; /* 0 => kernel resolves it */

	/* GRE inner / overlay endpoints. */
	struct ipaddr overlay_local;
	struct ipaddr overlay_remote;
	uint8_t overlay_prefix_len;

	uint32_t mtu;   /* 0 => kernel default */
	uint32_t flags; /* MIDR_VLINK_F_* */
};

/*
 * NOTE: overlay_local/overlay_remote/overlay_prefix_len/overlay_ready live
 * here and deliberately NOT in struct midr_gre_status.  midr-gre owns no
 * overlay concept - a GRE device can be UP while the overlay address is not
 * configured yet, so an overlay_ready flag there would be a false-ready trap;
 * midr-virtual-link-api.md §2 also freezes midr-gre semantics ("do not
 * change").  midr_virtual_link_status is the authoritative CP-facing surface,
 * and the overlay values travel with every notification through the unchanged
 * callback signature below.
 */
struct midr_virtual_link_status {
	enum midr_virtual_link_state state;
	enum midr_virtual_link_event event; /* transition that fired this notify */
	enum midr_virtual_link_stage stage; /* NONE on success */
	char ifname[IFNAMSIZ];
	vrf_id_t vrf_id;
	ifindex_t ifindex; /* valid when state >= DEVICE_UP (see §7.3/§5.3) */
	uint8_t iftype;    /* enum zebra_iftype; valid at READY */
	struct ipaddr overlay_local;
	struct ipaddr overlay_remote;
	uint8_t overlay_prefix_len; /* overlay local prefix length */
	bool overlay_ready;         /* true iff state == MIDR_VLINK_READY */
	int last_error;     /* errno style; valid when state == FAILED */
	uint32_t ready_ms;  /* ms spent READY; 0 if never READY */
};

typedef void (*midr_vlink_notify_cb)(const struct midr_virtual_link_status *st,
				     void *arg);

/*
 * =========================================================================
 *  Public API (contract doc/midr-doc/dp-doc/midr-virtual-link-api.md §4)
 * =========================================================================
 */

/*
 * Create or update a virtual link.  Returns 0 when the request was accepted
 * (CREATING/DEVICE_UP/CONFIGURING, or already READY); call wait_ready() or
 * the notify callback for the final verdict.  Returns -1 and fills @status
 * with state == FAILED otherwise.
 */
int midr_virtual_link_add(struct midr_context *ctx,
			  const struct midr_virtual_link_desc *desc,
			  struct midr_virtual_link_status *status);

/*
 * Delete a virtual link by ifname.  Returns 0 and status->state == DOWN on
 * success, -1 when the name is unknown or the request could not be sent.
 */
int midr_virtual_link_del(struct midr_context *ctx, const char *ifname,
			  struct midr_virtual_link_status *status);

/*
 * Non-blocking query.  Returns 0 and fills @status when the link is known,
 * -1 otherwise.  Pure query: it never emits a notification and always
 * reports status->event == MIDR_VLINK_EV_NONE.
 */
int midr_virtual_link_get_state(struct midr_context *ctx, const char *ifname,
				struct midr_virtual_link_status *status);

/*
 * Blocking wait for READY: pumps the event loop until READY / FAILED /
 * timeout.  Returns true iff the link reached READY.  Must be called from
 * the midrd main thread.
 */
bool midr_virtual_link_wait_ready(struct midr_context *ctx, const char *ifname,
				  uint32_t timeout_ms,
				  struct midr_virtual_link_status *status);

/*
 * State-change notification.  Fired on DEVICE_UP / ADDRESS_SET / READY /
 * FAILED / DOWN transitions in that order of progression; FAILED is also
 * emitted autonomously when zebra loses the device (see
 * midr_virtual_link_init()).  @st->event says which transition fired the
 * call; @st is only valid for the duration of the call.  The callback
 * signature is unchanged and the event travels inside @st.
 */
void midr_virtual_link_register_notify(midr_vlink_notify_cb cb, void *arg);
void midr_virtual_link_unregister_notify(midr_vlink_notify_cb cb);

/* Lifecycle and helpers. */
const char *midr_virtual_link_state_str(enum midr_virtual_link_state state);
const char *midr_virtual_link_event_str(enum midr_virtual_link_event event);
void midr_virtual_link_init(struct event_loop *master);
void midr_virtual_link_fini(void);

/*
 * =========================================================================
 *  Confirmation seams (external, non-static: wrap-able by tests)
 * =========================================================================
 */

/*
 * Report whether zebra has confirmed the device.  Fills *ifindex and
 * *iftype on success by reading the daemon's interface view
 * (if_lookup_by_name()) -- the same view zebra populates through the
 * interface notifications this daemon registers for.
 */
bool midr_virtual_link_probe_device(const char *ifname, vrf_id_t vrf_id,
				    ifindex_t *ifindex, uint8_t *iftype);

/*
 * Report whether the overlay data plane is ready: the overlay local/prefix
 * is visible on the device and the interface is IFF_UP.  v1 relies on the
 * connected route implied by overlay_remote being inside the prefix; no
 * active probe is performed.  Tests may wrap this symbol.
 */
bool midr_virtual_link_overlay_reachable(const char *ifname, vrf_id_t vrf_id,
					 const struct ipaddr *overlay_local,
					 uint8_t prefix_len);

#ifdef __cplusplus
}
#endif

#endif /* MIDRD_MIDR_VIRTUAL_LINK_H */
