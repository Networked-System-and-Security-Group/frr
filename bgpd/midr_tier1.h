// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR Tier-1 underlay path helpers.
 *
 * MIDR is an application-layer overlay.  In a normal deployment it may only
 * have active measurements such as ping/traceroute plus an IP-to-ASN mapper,
 * not local bgpd RIB state.  The target-check API below is therefore the
 * primary runtime entry point: callers provide a target IP and an observer
 * that supplies the measured ASN sequence.  The aspath API is a reuse hook for
 * tests, collectors, or deployments that do have a BGP AS_PATH available.
 */

#ifndef _FRR_MIDR_TIER1_H
#define _FRR_MIDR_TIER1_H

#include <stdbool.h>
#include <stddef.h>

#include "bgpd/bgp_aspath.h"
#include "prefix.h"

#define MIDR_TIER1_MAX_HITS 32
#define MIDR_TIER1_MAX_OBSERVED_ASNS 128

enum midr_tier1_result_flags {
	MIDR_TIER1_FLAG_AS_SET_PRESENT = (1U << 0),
	MIDR_TIER1_FLAG_CONFED_SEGMENTS_IGNORED = (1U << 1),
	MIDR_TIER1_FLAG_PRIVATE_ASNS_IGNORED = (1U << 2),
	MIDR_TIER1_FLAG_AS_ZERO_IGNORED = (1U << 3),
	MIDR_TIER1_FLAG_UNKNOWN_SEGMENT_IGNORED = (1U << 4),
	MIDR_TIER1_FLAG_HITS_TRUNCATED = (1U << 5),
	MIDR_TIER1_FLAG_EMPTY_PATH = (1U << 6),
	MIDR_TIER1_FLAG_UNMAPPED_ASNS_IGNORED = (1U << 7),
};

#define MIDR_TIER1_FLAG_SEMANTICS_DEGRADED                                  \
	(MIDR_TIER1_FLAG_AS_SET_PRESENT                                      \
	 | MIDR_TIER1_FLAG_CONFED_SEGMENTS_IGNORED                           \
	 | MIDR_TIER1_FLAG_PRIVATE_ASNS_IGNORED                              \
	 | MIDR_TIER1_FLAG_AS_ZERO_IGNORED                                   \
	 | MIDR_TIER1_FLAG_UNKNOWN_SEGMENT_IGNORED                           \
	 | MIDR_TIER1_FLAG_UNMAPPED_ASNS_IGNORED)

#define MIDR_TIER1_FLAG_EMPTY_ASPATH MIDR_TIER1_FLAG_EMPTY_PATH

struct midr_tier1_list {
	const as_t *asns;
	size_t count;
	const char *version;
};

struct midr_tier1_result {
	bool tier1_observed;
	bool tier1_ordered_observed;
	bool tier1_unordered_observed;
	const char *tier1_list_version;
	unsigned int flags;
	unsigned int normalized_hops;
	unsigned int ignored_private_asns;
	unsigned int ignored_confed_asns;
	unsigned int ignored_as_zero_asns;
	unsigned int ignored_unmapped_asns;
	unsigned int ordered_hit_count;
	unsigned int unordered_hit_count;
	as_t ordered_hits[MIDR_TIER1_MAX_HITS];
	as_t unordered_hits[MIDR_TIER1_MAX_HITS];
};

struct midr_tier1_observation {
	struct prefix target;
	const char *source;
	as_t observed_asns[MIDR_TIER1_MAX_OBSERVED_ASNS];
	size_t observed_asn_count;
};

typedef int (*midr_tier1_observe_cb)(
	const struct prefix *target, struct midr_tier1_observation *observation,
	void *arg);

struct midr_tier1_observer {
	const char *name;
	midr_tier1_observe_cb observe;
	void *arg;
};

/*
 * Versioned starter list for early development.  Production deployments should
 * replace it with a CAIDA clique/as-rank snapshot or another explicitly
 * versioned operator policy list.
 */
extern const struct midr_tier1_list midr_tier1_common_seed_202607;

extern void midr_tier1_result_init(struct midr_tier1_result *result);
extern bool midr_tier1_list_contains(const struct midr_tier1_list *list,
				     as_t asn);
extern int midr_tier1_target_check(const struct prefix *target,
				   const struct midr_tier1_observer *observer,
				   const struct midr_tier1_list *list,
				   struct midr_tier1_result *result,
				   struct midr_tier1_observation *observation);
extern int midr_tier1_observed_path_check(const as_t *observed_asns,
					  size_t observed_asn_count,
					  const struct midr_tier1_list *list,
					  struct midr_tier1_result *result);
extern int midr_tier1_aspath_check(const struct aspath *aspath,
				   const struct midr_tier1_list *list,
				   struct midr_tier1_result *result);

#endif /* _FRR_MIDR_TIER1_H */
