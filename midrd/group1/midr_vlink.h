// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR group 1: virtual links.
 *
 * Every MIDR adjacency group 1 reports to group 2 crosses the BGP underlay,
 * so its Link has no directly connected next hop.  Group 1 therefore asks
 * group 3's virtual-link service (midrd/midr-virtual-link.h) for one GRE
 * tunnel per adjacency and reports the Link with the tunnel's ifindex and
 * inner (overlay) addresses once the tunnel is READY.
 *
 *   outer endpoints   = the two nodes' transport locators
 *   overlay addresses = fe80::<local node id><remote node id>/64 locally,
 *                       mirrored on the peer, so both ends agree without
 *                       exchanging anything
 *   device name       = "mgre-<remote node id in hex>"
 *
 * The MIDR session itself stays on the underlay locators.  When midrd runs
 * without zebra the service does not exist and Links keep the locator
 * addresses and ifindex 0.
 */
#ifndef _MIDR_VLINK_H
#define _MIDR_VLINK_H

#include <zebra.h>

#include "if.h"
#include "ipaddr.h"

struct midr_g1;

/* A tunnel that is READY, as the Link report needs it. */
struct midr_g1_vlink_info {
	ifindex_t ifindex;
	struct ipaddr overlay_local;
	struct ipaddr overlay_remote;
	struct ipaddr outer_local;
	struct ipaddr outer_remote;
};

/* Retry interval after the service reports a tunnel FAILED. */
#define MIDR_G1_VLINK_RETRY_MS 5000

/*
 * The service's READY only covers the local end.  Before a Link is reported
 * group 1 pings the peer's overlay address out of the tunnel (ICMPv6 echo),
 * which also proves the peer built its end with the mirrored addresses.
 * Until the first reply it probes every PROBE_MS, then every KEEPALIVE_MS;
 * PROBE_MISSES unanswered probes in a row withdraw the Link (the tunnel is
 * kept and probed on).
 */
#define MIDR_G1_VLINK_PROBE_MS 1000
#define MIDR_G1_VLINK_KEEPALIVE_MS 5000
#define MIDR_G1_VLINK_PROBE_MISSES 3

/* True when midrd runs the data plane, i.e. virtual links are in use. */
bool midr_g1_vlink_enabled(void);

void midr_g1_vlink_init(struct midr_g1 *g1);
/* Delete every tunnel group 1 created. */
void midr_g1_vlink_finish(struct midr_g1 *g1);

/*
 * Make sure a tunnel to @remote_rid exists with these outer endpoints and
 * return true, filling @out, when it is READY.  A missing tunnel is
 * requested, a tunnel whose endpoints changed is rebuilt.  When a tunnel
 * becomes READY or fails later, the Link to @remote_rid is reported or
 * withdrawn again from the event loop.
 */
bool midr_g1_vlink_ready(struct midr_g1 *g1, uint32_t remote_rid,
			 const struct ipaddr *local_locator,
			 const struct ipaddr *remote_locator,
			 struct midr_g1_vlink_info *out);

/* Side-effect-free: true, filling @out, when the tunnel to @remote_rid is
 * READY and its peer answers on the overlay. */
bool midr_g1_vlink_get(uint32_t remote_rid, struct midr_g1_vlink_info *out);

/* The adjacency is gone: delete its tunnel.  The caller withdraws the Link
 * first. */
void midr_g1_vlink_release(struct midr_g1 *g1, uint32_t remote_rid);

/* Outcome of one overlay probe: a reply, or a probe that went unanswered.
 * Called by the ICMPv6 prober; tests call it directly. */
void midr_g1_vlink_probe_event(uint32_t remote_rid, bool replied);

/* fe80::<a><b>, both node ids as raw s_addr values in dotted byte order. */
void midr_g1_vlink_overlay_addr(uint32_t a_rid, uint32_t b_rid,
				struct ipaddr *out);

#endif /* _MIDR_VLINK_H */
