// SPDX-License-Identifier: GPL-2.0-or-later
/* MIDR traceroute IP2ASN mapping helpers. */

#ifndef _FRR_MIDR_TRACE_OBSERVER_H
#define _FRR_MIDR_TRACE_OBSERVER_H

#include <stddef.h>

#include "midrd/group1/midr_trace_scheduler.h"

extern void midr_trace_map_ip2asn(const struct midr_trace_job_result *result,
				  struct midr_trace_query_view *view);

/*
 * Deprecated synchronous-observer compatibility.  It is now cache-only:
 * a miss schedules/joins an asynchronous job and returns -1 without waiting.
 *
 * Returns 0 on a mapped positive cache hit, -1 on a miss/measurement failure,
 * or -2 when no IP-to-ASN snapshot has been loaded.
 */
extern int midr_trace_observe_path(
	const struct prefix *target,
	struct midr_tier1_observation *observation);

extern const struct midr_tier1_observer *midr_trace_observer_get(void);

#endif /* _FRR_MIDR_TRACE_OBSERVER_H */
