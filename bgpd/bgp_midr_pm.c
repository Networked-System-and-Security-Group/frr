// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR Performance Measurement (PM) module.
 *
 * Skeleton stage: I-1 / I-2 entry points are stubs.  The real UDP probing,
 * EWMA RTT / sliding-window loss / bandwidth-score statistics (ported from
 * link-perf) and the I-5 feedback to NDS land in a later phase.
 */

#include <zebra.h>

#include "log.h"
#include "prefix.h"
#include "network.h" /* frr_weak_random */

#include "bgpd/bgpd.h"
#include "bgpd/bgp_midr.h"
#include "bgpd/bgp_midr_liveness.h"
#include "bgpd/bgp_midr_pm.h"

/*
 * Skeleton metric synthesis.  Real UDP probing (EWMA RTT / sliding-window
 * loss / bandwidth score, ported from link-perf) lands in a later phase; for
 * now produce a plausible measurement — random RTT, no loss, constant
 * bandwidth score — so the join + I-5 pipeline can run end to end.
 */
static void midr_pm_fake_metrics(struct midr_link_metrics *m)
{
	memset(m, 0, sizeof(*m));
	m->rtt_us = 1000 + (uint32_t)(frr_weak_random() % 9000); /* 1-10 ms */
	m->rtt_min_us = m->rtt_us > 500 ? m->rtt_us - 500 : m->rtt_us;
	m->rtt_max_us = m->rtt_us + 500;
	m->loss_rate = 0.0;
	m->bw_score = 1000;
}

/* I-1: start probing a (newly discovered or group-member) node. */
int midr_pm_add_target(struct bgp *bgp, const struct prefix *node_id,
		       enum midr_node_source source, uint32_t capabilities)
{
	struct midr_link_metrics m;

	if (!bgp || !bgp->midr_info || !node_id)
		return -1;

	/*
	 * I-1 callers pass the probe endpoint (normally TLV 1188, falling back
	 * to Router-ID).  Indirect discovery must not restart probing for an
	 * endpoint owned by a SUSPECT/REMOVING node or retained in the recent
	 * removal quarantine.
	 */
	if (!midr_liveness_endpoint_usable(bgp, node_id)) {
		MIDR_LOG("MIDR PM I-1: ignore quarantined probe target %pFX",
			 node_id);
		return -1;
	}

	midr_pm_fake_metrics(&m);

	MIDR_LOG("MIDR PM I-1: probe %pFX src=%d caps=0x%x -> rtt=%uus (stub metrics)",
		   node_id, source, capabilities, m.rtt_us);

	/* I-5: push the (synthetic) short-term + long-term link state to NDS. */
	midr_nds_on_link_update(bgp, node_id, MIDR_LINK_UP, 0, &m, &m);
	return 0;
}

/* I-2: stop probing a node and reclaim its resources. */
int midr_pm_remove_target(struct bgp *bgp, const struct prefix *node_id,
			  enum midr_stop_reason reason)
{
	if (!bgp || !bgp->midr_info || !node_id)
		return -1;

	MIDR_LOG("MIDR PM I-2: remove probe target %pFX reason=%d (stub)",
		   node_id, reason);
	return 0;
}

/*
 * Periodic PM probe timer (the diagram's `loop [持续探测]`).
 *
 * Skeleton: walk the node table and, for every node with an Established
 * BGP-LS session, synthesize a measurement and push it to NDS via I-5.  When
 * the real prober lands, this body is replaced by an iteration over registered
 * probe targets driving actual UDP probes; NDS keeps starting us via
 * midr_pm_init().
 */
static void midr_pm_probe_timer(struct event *t)
{
	struct bgp *bgp = EVENT_ARG(t);
	struct bgp_midr *mi = bgp->midr_info;
	struct midr_node_entry *entry;

	frr_each (midr_node_hash, &mi->global_view->nodes, entry) {
		struct midr_link_metrics m;

		if (entry->is_self)
			continue;
		if (!midr_liveness_node_usable(entry))
			continue;
		/*
		 * 只探邻居：is_adjacent 是语义判据（是不是邻居），
		 * established_peer 是可达性判据（会话已建、探得通）。
		 * established ⊆ is_adjacent，两者都要。
		 */
		if (!entry->is_adjacent)
			continue;
		if (!midr_node_established_peer(bgp, &entry->node_id))
			continue;

		midr_pm_fake_metrics(&m);

		MIDR_LOG("MIDR PM loop: probe %pFX -> rtt=%uus (stub metrics)",
			   &entry->node_id, m.rtt_us);

		/* I-5: report short-term + long-term link state to NDS. */
		midr_nds_on_link_update(bgp, &entry->node_id, MIDR_LINK_UP, 0,
					&m, &m);
	}

	event_add_timer(bm->master, midr_pm_probe_timer, bgp,
			MIDR_PM_PROBE_INTERVAL, &mi->t_pm_probe);
}

void midr_pm_init(struct bgp *bgp)
{
	if (!bgp || !bgp->midr_info)
		return;

	event_add_timer(bm->master, midr_pm_probe_timer, bgp,
			MIDR_PM_PROBE_INTERVAL, &bgp->midr_info->t_pm_probe);
}

void midr_pm_finish(struct bgp *bgp)
{
	if (!bgp || !bgp->midr_info)
		return;

	event_cancel(&bgp->midr_info->t_pm_probe);
}
