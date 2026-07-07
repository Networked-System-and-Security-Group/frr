// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR Performance Measurement (PM) module interface.
 *
 * Owns per-target probe contexts and drives UDP probing; feeds results
 * back to NDS via midr_nds_on_link_update() (I-5).
 *
 * Probe protocol (port MIDR_PM_PROBE_PORT):
 *   - prober sends midr_probe_pkt{REQ} to target's transport_addr
 *   - responder echoes back midr_probe_pkt{REP} with the same seqno/sent_us
 *   - prober computes RTT = now - sent_us, updates EWMA + loss window
 *
 * Security:
 *   - socket bound to local transport_addr only (not INADDR_ANY)
 *   - recv handler validates source is a known node in global_view
 *   - reply seqno must match outstanding probe seqno
 */

#ifndef _FRR_BGP_MIDR_PM_H
#define _FRR_BGP_MIDR_PM_H

#include <stdint.h>
#include <netinet/in.h>

#include "prefix.h"
#include "frrevent.h"

struct bgp;

/* UDP port for PM probing (distinct from ctrl channel port 5859) */
#define MIDR_PM_PROBE_PORT	    5860

/* Probe packet magic and type fields */
#define MIDR_PM_PROBE_MAGIC	    0x4D494452u /* 'MIDR' */
#define MIDR_PM_PROBE_REQ	    0
#define MIDR_PM_PROBE_REP	    1

/* Statistics parameters */
#define MIDR_PM_LOSS_WINDOW_SIZE    100  /* sliding window depth for loss rate */
#define MIDR_PM_CONSECUTIVE_FAIL_FAST 3 /* failures before switching to fast mode */
#define MIDR_PM_PROBE_INTERVAL_NORMAL 1 /* normal probe interval (seconds) */
#define MIDR_PM_PROBE_INTERVAL_FAST_MS 200 /* fast probe interval (milliseconds) */
#define MIDR_PM_PROBE_TIMEOUT_MS    500  /* per-probe reply deadline (ms) */
#define MIDR_PM_ALPHA_SHORT	    0.2  /* short-term EWMA decay */
#define MIDR_PM_ALPHA_LONG	    0.05 /* long-term EWMA decay */

/* Wire format: 20 bytes, network byte order */
struct midr_probe_pkt {
	uint32_t magic;	  /* MIDR_PM_PROBE_MAGIC */
	uint8_t type;	  /* MIDR_PM_PROBE_REQ or MIDR_PM_PROBE_REP */
	uint8_t pad[3];
	uint32_t seqno;
	uint64_t sent_us; /* sender monotonic clock (microseconds) */
} __attribute__((packed));

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
	MIDR_PROBE_NORMAL,
	MIDR_PROBE_FAST,
};

/* Per-target probe context (PM owns these in bgp_midr->probe_contexts) */
struct midr_probe_ctx {
	struct bgp *bgp;
	struct prefix target;	       /* node_id — hash key */
	struct in_addr target_addr;    /* transport_addr: where to send probes */
	enum midr_probe_state state;
	uint32_t capabilities;
	enum midr_node_source source;

	/* Wire state */
	uint32_t seqno;		  /* next seqno to send */
	uint32_t pending_seqno;	  /* seqno of the outstanding probe */
	bool probe_outstanding;
	uint64_t sent_us;	  /* µs timestamp of the last sent probe */
	uint32_t consecutive_failures;

	/* Loss sliding window (circular buffer: 1=received, 0=lost) */
	uint8_t loss_win[MIDR_PM_LOSS_WINDOW_SIZE];
	int loss_win_idx;     /* next write slot (wraps) */
	uint32_t loss_win_total; /* total probes counted, capped at SIZE */

	/* Short-term EWMA (fast-decaying, α = MIDR_PM_ALPHA_SHORT) */
	bool st_init;
	double st_rtt_us;
	uint32_t st_rtt_min_us;
	uint32_t st_rtt_max_us;

	/* Long-term EWMA (slow-decaying, α = MIDR_PM_ALPHA_LONG) */
	bool lt_init;
	double lt_rtt_us;
	double lt_loss_rate;

	struct event *t_probe;	  /* per-target periodic probe timer */
	struct event *t_timeout;  /* per-probe reply-timeout timer */
};

/* I-1: NDS -> PM, start probing a node */
extern int midr_pm_add_target(struct bgp *bgp, const struct prefix *node_id,
			      enum midr_node_source source,
			      uint32_t capabilities);

/* I-2: NDS -> PM, stop probing a node */
extern int midr_pm_remove_target(struct bgp *bgp, const struct prefix *node_id,
				 enum midr_stop_reason reason);

/* Called by the VTY `midr transport-address` command after the local
 * transport address is configured.  Opens the PM socket if not already open,
 * then starts probing any already-established adjacent neighbors whose I-1
 * call was silently dropped because the socket was not ready at the time. */
extern void midr_pm_on_transport_addr_set(struct bgp *bgp);

/* Module lifecycle: open/close PM socket and arm/cancel probe timers.
 * Called from bgp_midr_init() / bgp_midr_finish(). */
extern void midr_pm_init(struct bgp *bgp);
extern void midr_pm_finish(struct bgp *bgp);

#endif /* _FRR_BGP_MIDR_PM_H */
