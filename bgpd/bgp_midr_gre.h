// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR GRE virtual interface provisioning (bgp_midr_gre.h)
 *
 * =========================================================================
 *  Purpose
 * =========================================================================
 * The MIDR control plane (standard SPF and TE CSPF) computes overlay paths
 * that, for the SRv6 / TE case, need to be carried over GRE tunnels between
 * domain border routers.  This module is the *only* CP-facing entry point
 * used to create and remove those GRE virtual interfaces: it translates a
 * technology-neutral tunnel description into a ZAPI request and hands it to
 * zebra, which owns the kernel netdevice.
 *
 * =========================================================================
 *  Design
 * =========================================================================
 *   - The CP describes a tunnel by its two endpoints (and optional keys,
 *     underlay link, MTU).  It never touches netlink or zebra internals.
 *   - If the CP supplies an interface name it is used verbatim.
 *     If the name is empty one is generated automatically in the form
 *     "midr-gre-<n>" and remembered, so that repeated create calls for the
 *     same endpoints are idempotent and the tunnel can later be deleted by
 *     endpoints as well as by name.
 *   - Deletion is best-effort: the registry entry is dropped and the request
 *     is forwarded to zebra (which emits RTM_DELLINK).
 *
 *   wire path:
 *     CP (SPF/CSPF)
 *        │  midr_gre_interface_add() / _del()
 *        ▼
 *     bgp_midr_gre.c  ──►  zclient_send_gre_add()/delete()  (lib/zclient.c)
 *        │  ZEBRA_GRE_ADD / ZEBRA_GRE_DELETE
 *        ▼
 *     zebra (zapi_msg.c → zebra_dplane.c → if_netlink.c)  ──►  kernel
 */

#ifndef _FRR_BGP_MIDR_GRE_H
#define _FRR_BGP_MIDR_GRE_H

#include <zebra.h>

#include "lib/if.h"
#include "lib/ipaddr.h"
#include "lib/vrf.h"

#ifdef __cplusplus
extern "C" {
#endif

struct bgp;

/*
 * Description of a MIDR GRE tunnel.
 *
 * ifname may be left empty; a stable, unique name ("midr-gre-<n>") is then
 * generated automatically and can be retrieved with
 * midr_gre_interface_name().
 *
 * local/remote must be set and must share the same address family.
 */
struct midr_gre_tunnel {
	/* Optional device name.  Empty => auto-generate. */
	char ifname[IFNAMSIZ];

	/* VRF of the tunnel; VRF_UNKNOWN means "use the BGP instance VRF". */
	vrf_id_t vrf_id;

	/* Tunnel endpoints. */
	struct ipaddr local;  /* tunnel source  (IFLA_GRE_LOCAL) */
	struct ipaddr remote; /* tunnel destination (IFLA_GRE_REMOTE) */

	/* Underlay egress link ifindex; 0 lets the kernel resolve it. */
	ifindex_t link_ifindex;

	/* Optional GRE keys. */
	uint32_t ikey;
	uint32_t okey;

	/* Optional GRE encap flags (ZEBRA_GRE_ENCAP_FLAGS_CSUM/CSUM6). */
	uint16_t encap_flags;

	/* Tunnel MTU; 0 means "use the kernel default". */
	uint32_t mtu;
};

/*
 * =========================================================================
 *  Establishment state
 * =========================================================================
 * The kernel device is created asynchronously (bgpd -> zebra -> netlink).
 * A create request therefore has an immediate result (accepted / rejected)
 * and a later, confirmed result (the device actually exists).
 *
 *   MIDR_GRE_STATE_DOWN     no request outstanding, or device removed
 *   MIDR_GRE_STATE_PENDING  request sent, kernel confirmation pending
 *   MIDR_GRE_STATE_UP       zebra reported the device; ifindex is valid
 *   MIDR_GRE_STATE_FAILED   request rejected, or confirmation timed out
 */
enum midr_gre_state {
	MIDR_GRE_STATE_DOWN = 0,
	MIDR_GRE_STATE_PENDING,
	MIDR_GRE_STATE_UP,
	MIDR_GRE_STATE_FAILED,
	MIDR_GRE_STATE_MAX,
};

/* Human readable state name (logs / CLI). */
extern const char *midr_gre_state_str(enum midr_gre_state state);

/*
 * Detailed status of a MIDR GRE tunnel interface.
 *
 * ifindex / iftype are meaningful only when state == MIDR_GRE_STATE_UP.
 * err carries an errno-style reason when state == MIDR_GRE_STATE_FAILED.
 */
struct midr_gre_status {
	enum midr_gre_state state;
	char ifname[IFNAMSIZ];
	vrf_id_t vrf_id;
	ifindex_t ifindex;
	uint8_t iftype; /* enum zebra_iftype */
	int err;
	uint32_t up_ms; /* ms since the interface became UP; 0 if not UP */
};

/*
 * Asynchronous state-change notification.
 *
 * Fired on every transition: PENDING -> UP, PENDING -> FAILED, UP -> DOWN.
 * The @status pointer is only valid for the duration of the call.
 */
typedef void (*midr_gre_notify_cb)(const struct midr_gre_status *status,
				   void *arg);
extern void midr_gre_register_notify(midr_gre_notify_cb cb, void *arg);
extern void midr_gre_unregister_notify(midr_gre_notify_cb cb);

/*
 * Create (or update) a GRE virtual interface.
 *
 * @status (optional, may be NULL) receives the immediate result:
 *   UP      - the device already existed (the request is also re-sent)
 *   PENDING - request accepted by zebra; confirmation still outstanding
 *   FAILED  - rejected (see status->err)
 *
 * Returns 0 when the request was accepted (UP or PENDING), -1 otherwise.
 * For a PENDING request use midr_gre_interface_wait_up() or the notify
 * callback to obtain the confirmed result.
 */
extern int midr_gre_interface_add(struct bgp *bgp,
				  const struct midr_gre_tunnel *tun,
				  struct midr_gre_status *status);

/*
 * Remove a GRE virtual interface by name.
 * @status (optional) receives state == DOWN with err == 0 on success.
 * Returns 0 on success, -1 on error.
 */
extern int midr_gre_interface_del(struct bgp *bgp, const char *ifname,
				  struct midr_gre_status *status);

/*
 * Remove the GRE virtual interface created for the given endpoints.
 * @status (optional) receives state == DOWN with err == 0 on success.
 * Returns 0 on success, -1 if no such tunnel is known.
 */
extern int midr_gre_interface_del_by_endpoints(struct bgp *bgp,
					       vrf_id_t vrf_id,
					       const struct ipaddr *local,
					       const struct ipaddr *remote,
					       struct midr_gre_status *status);

/*
 * Non-blocking query of the current (reconciled) state.
 * Returns 0 and fills @status when the tunnel is known, -1 otherwise.
 */
extern int midr_gre_interface_get_state(vrf_id_t vrf_id, const char *ifname,
					struct midr_gre_status *status);

/*
 * Wait until the interface is UP, or has FAILED, or @timeout_ms elapses.
 * Pumps the bgpd event loop while waiting, so it must be called from the
 * bgpd main thread.
 *
 * Returns true if the interface is UP; fills @status when non-NULL.
 */
extern bool midr_gre_interface_wait_up(struct bgp *bgp, const char *ifname,
				       uint32_t timeout_ms,
				       struct midr_gre_status *status);

/*
 * Look up the (possibly auto-generated) interface name for a set of
 * endpoints.  Returns NULL if no tunnel is known for them.
 */
extern const char *midr_gre_interface_name(vrf_id_t vrf_id,
					   const struct ipaddr *local,
					   const struct ipaddr *remote);

/*
 * Release the local registry.  Called at bgpd shutdown.
 */
extern void midr_gre_fini(void);

#ifdef __cplusplus
}
#endif

#endif /* _FRR_BGP_MIDR_GRE_H */
