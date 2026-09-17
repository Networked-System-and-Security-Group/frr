// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _BGP_MIDR_ADMISSION_H
#define _BGP_MIDR_ADMISSION_H

#include "midrd/group1/midr_nds.h"
#include "midrd/group1/midr_trace_types.h"

struct midr_admission;
struct vty;

enum midr_admission_result {
	MIDR_ADMISSION_READY,
	MIDR_ADMISSION_PENDING,
	MIDR_ADMISSION_BLOCKED,
	MIDR_ADMISSION_INVALID,
};

void midr_admission_init(struct midr_g1 *g1);
void midr_admission_finish(struct midr_g1 *g1);
void midr_admission_set(struct midr_g1 *g1, bool enabled);
void midr_admission_reset(struct midr_g1 *g1);
void midr_admission_forget(struct midr_g1 *g1, struct ipaddr target);
void midr_admission_forget_reason(struct midr_g1 *g1,
				enum midr_session_reason reason);
void midr_admission_remote_seen(struct midr_g1 *g1, struct ipaddr target);
bool midr_admission_has_intent(struct midr_g1 *g1, struct ipaddr target);
bool midr_admission_is_manual(struct midr_g1 *g1, struct ipaddr target);
enum midr_admission_result midr_admission_gate(struct midr_g1 *g1,
	const struct midr_node_entry *entry, enum midr_session_reason reason,
	bool send_nudge, bool received, bool attach_request);
/* Trace a join/anchor candidate ahead of any session so that CL can skip
 * candidates whose path crosses a Tier1 AS. No-op unless avoid-tier1 is on. */
void midr_admission_screen(struct midr_g1 *g1, const struct midr_node_entry *target);
bool midr_admission_peer_ready(struct midr_g1_peer *peer);
/* Called when a session is about to be requested; records the permit. */
bool midr_admission_begin(struct midr_g1_peer *peer);
/* The permit recorded at begin still holds. */
bool midr_admission_check(struct midr_g1_peer *peer);
bool midr_admission_unavailable(struct midr_g1 *g1, struct ipaddr target);
unsigned int midr_admission_attach_pending(struct midr_g1 *g1);
void midr_admission_retx_budget(struct midr_g1 *g1, struct ipaddr target, int count);
int midr_admission_get_retx_budget(struct midr_g1 *g1, struct ipaddr target);
void midr_admission_show(struct midr_g1 *g1, struct vty *vty);
void midr_admission_committed(struct midr_g1 *g1);
size_t midr_admission_anchor_groups(struct midr_g1 *g1, uint32_t *groups, size_t size);
bool midr_admission_candidate_blocked(const struct midr_g1 *g1, struct ipaddr target);
enum midr_admission_result midr_admission_evaluate(
	const struct midr_trace_job_result *job, struct midr_tier1_result *result);

#endif
