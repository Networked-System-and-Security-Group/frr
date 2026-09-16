// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _BGP_MIDR_ADMISSION_H
#define _BGP_MIDR_ADMISSION_H

#include "bgpd/bgp_midr_nds.h"
#include "bgpd/midr_trace_types.h"

struct peer_connection;
struct midr_admission;
struct vty;

enum midr_admission_result {
	MIDR_ADMISSION_READY,
	MIDR_ADMISSION_PENDING,
	MIDR_ADMISSION_BLOCKED,
	MIDR_ADMISSION_INVALID,
};

void midr_admission_init(struct bgp *bgp);
void midr_admission_finish(struct bgp *bgp);
void midr_admission_set(struct bgp *bgp, bool enabled);
void midr_admission_reset(struct bgp *bgp);
void midr_admission_forget(struct bgp *bgp, struct ipaddr target);
void midr_admission_forget_reason(struct bgp *bgp,
				enum midr_session_reason reason);
void midr_admission_remote_seen(struct bgp *bgp, struct ipaddr target);
bool midr_admission_has_intent(struct bgp *bgp, struct ipaddr target);
bool midr_admission_is_manual(struct bgp *bgp, struct ipaddr target);
enum midr_admission_result midr_admission_gate(struct bgp *bgp,
	const struct midr_node_entry *entry, enum midr_session_reason reason,
	bool send_nudge, bool received, bool attach_request);
/* Trace a join/anchor candidate ahead of any session so that CL can skip
 * candidates whose path crosses a Tier1 AS. No-op unless avoid-tier1 is on. */
void midr_admission_screen(struct bgp *bgp, const struct midr_node_entry *target);
bool midr_admission_peer_ready(struct peer *peer);
bool midr_admission_begin(struct peer_connection *connection);
bool midr_admission_check(struct peer_connection *connection);
bool midr_admission_unavailable(struct bgp *bgp, struct ipaddr target);
unsigned int midr_admission_attach_pending(struct bgp *bgp);
void midr_admission_retx_budget(struct bgp *bgp, struct ipaddr target, int count);
int midr_admission_get_retx_budget(struct bgp *bgp, struct ipaddr target);
void midr_admission_show(struct bgp *bgp, struct vty *vty);
void midr_admission_committed(struct bgp *bgp);
size_t midr_admission_anchor_groups(struct bgp *bgp, uint32_t *groups, size_t size);
bool midr_admission_candidate_blocked(const struct bgp *bgp, struct ipaddr target);
enum midr_admission_result midr_admission_evaluate(
	const struct midr_trace_job_result *job, struct midr_tier1_result *result);

#endif
