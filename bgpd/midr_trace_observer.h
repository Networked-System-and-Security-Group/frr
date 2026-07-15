// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR traceroute observer.
 */

#ifndef _FRR_MIDR_TRACE_OBSERVER_H
#define _FRR_MIDR_TRACE_OBSERVER_H

#include "bgpd/midr_tier1.h"

/*
 * Run traceroute for target and map each observed hop through the loaded
 * IP-to-ASN snapshot.  Unmapped or hidden hops are returned as ASN 0.
 *
 * Returns 0 on success, -1 on invalid input/measurement failure, or -2 when
 * no IP-to-ASN snapshot has been loaded.
 */
extern int midr_trace_observe_path(
	const struct prefix *target,
	struct midr_tier1_observation *observation);

extern const struct midr_tier1_observer *midr_trace_observer_get(void);

#endif /* _FRR_MIDR_TRACE_OBSERVER_H */
