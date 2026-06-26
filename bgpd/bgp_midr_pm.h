// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR Performance Measurement (PM) module interface.
 *
 * Owns per-target probe contexts and drives UDP probing; feeds results
 * back to NDS via midr_nds_on_link_update() (I-5).  Skeleton stage:
 * add/remove targets are stubs.
 */

#ifndef _FRR_BGP_MIDR_PM_H
#define _FRR_BGP_MIDR_PM_H

#include <stdint.h>

#include "prefix.h"
#include "frrevent.h"

struct bgp;

/* I-1 discovery source */
enum midr_node_source {
	MIDR_SRC_BOOTSTRAP = 0,
	MIDR_SRC_GOSSIP,
	MIDR_SRC_MANUAL,
};

/* I-2 stop reason */
enum midr_stop_reason {
	MIDR_STOP_KEEPALIVE_TIMEOUT = 0,
	MIDR_STOP_GRACEFUL_SHUTDOWN,
	MIDR_STOP_ADMIN_DOWN,
	MIDR_STOP_CLUSTER_CHANGE,
};

enum midr_probe_state {
	MIDR_PROBE_INIT = 0,
	MIDR_PROBE_FAST,
	MIDR_PROBE_NORMAL,
};

/* Per-target probe context (PM owns these in bgp_midr->probe_contexts) */
struct midr_probe_ctx {
	struct bgp *bgp;
	struct prefix target;
	enum midr_probe_state state;
	uint32_t capabilities;
	enum midr_node_source source;
	struct event *t_probe;
	struct event *t_timeout;
};

/* I-1: NDS -> PM, start probing a node */
extern int midr_pm_add_target(struct bgp *bgp, const struct prefix *node_id,
			      enum midr_node_source source,
			      uint32_t capabilities);

/* I-2: NDS -> PM, stop probing a node */
extern int midr_pm_remove_target(struct bgp *bgp, const struct prefix *node_id,
				 enum midr_stop_reason reason);

/* Module lifecycle: arm/cancel the periodic probe-of-connected-nodes timer
 * (the time-sequence diagram's `loop [持续探测]`).  Called from
 * bgp_midr_init() / bgp_midr_finish(). */
extern void midr_pm_init(struct bgp *bgp);
extern void midr_pm_finish(struct bgp *bgp);

#endif /* _FRR_BGP_MIDR_PM_H */
