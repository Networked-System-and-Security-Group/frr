// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR Tier-1 underlay path helpers.
 */

#include <zebra.h>

#include "bgpd/midr_tier1.h"

static const as_t midr_tier1_common_seed_202607_asns[] = {
	174, 1299, 2914, 3257, 3356, 6453, 6762, 6939,
};

const struct midr_tier1_list midr_tier1_common_seed_202607 = {
	.asns = midr_tier1_common_seed_202607_asns,
	.count = array_size(midr_tier1_common_seed_202607_asns),
	.version = "midr-common-tier1-seed-2026-07",
};

void midr_tier1_result_init(struct midr_tier1_result *result)
{
	if (!result)
		return;

	memset(result, 0, sizeof(*result));
}

bool midr_tier1_list_contains(const struct midr_tier1_list *list, as_t asn)
{
	size_t i;

	if (!list || !list->asns || !list->count)
		return false;

	for (i = 0; i < list->count; i++)
		if (list->asns[i] == asn)
			return true;

	return false;
}

static bool midr_tier1_hit_exists(const as_t *hits, unsigned int hit_count,
				  as_t asn)
{
	unsigned int i;

	for (i = 0; i < hit_count; i++)
		if (hits[i] == asn)
			return true;

	return false;
}

static void midr_tier1_add_hit(as_t *hits, unsigned int *hit_count,
			       struct midr_tier1_result *result, as_t asn)
{
	if (midr_tier1_hit_exists(hits, *hit_count, asn))
		return;

	if (*hit_count == MIDR_TIER1_MAX_HITS) {
		result->flags |= MIDR_TIER1_FLAG_HITS_TRUNCATED;
		return;
	}

	hits[*hit_count] = asn;
	(*hit_count)++;
}

static bool midr_tier1_should_ignore_asn(as_t asn,
					 struct midr_tier1_result *result,
					 bool zero_is_unmapped)
{
	if (asn == BGP_AS_ZERO) {
		if (zero_is_unmapped) {
			result->flags |= MIDR_TIER1_FLAG_UNMAPPED_ASNS_IGNORED;
			result->ignored_unmapped_asns++;
		} else {
			result->flags |= MIDR_TIER1_FLAG_AS_ZERO_IGNORED;
			result->ignored_as_zero_asns++;
		}
		return true;
	}

	if (BGP_AS_IS_PRIVATE(asn)) {
		result->flags |= MIDR_TIER1_FLAG_PRIVATE_ASNS_IGNORED;
		result->ignored_private_asns++;
		return true;
	}

	return false;
}

static void midr_tier1_process_ordered_asn(
	as_t asn, const struct midr_tier1_list *list,
	struct midr_tier1_result *result, bool *have_previous_asn,
	as_t *previous_asn, bool zero_is_unmapped)
{
	if (midr_tier1_should_ignore_asn(asn, result, zero_is_unmapped))
		return;

	if (*have_previous_asn && *previous_asn == asn)
		return;

	*have_previous_asn = true;
	*previous_asn = asn;
	result->normalized_hops++;

	if (midr_tier1_list_contains(list, asn))
		midr_tier1_add_hit(result->ordered_hits,
				   &result->ordered_hit_count, result, asn);
}

static void midr_tier1_process_unordered_asn(
	as_t asn, const struct midr_tier1_list *list,
	struct midr_tier1_result *result)
{
	if (midr_tier1_should_ignore_asn(asn, result, false))
		return;

	if (midr_tier1_list_contains(list, asn))
		midr_tier1_add_hit(result->unordered_hits,
				   &result->unordered_hit_count, result, asn);
}

int midr_tier1_target_check(const struct prefix *target,
			    const struct midr_tier1_observer *observer,
			    const struct midr_tier1_list *list,
			    struct midr_tier1_result *result,
			    struct midr_tier1_observation *observation)
{
	struct midr_tier1_observation local_observation;
	int ret;

	if (!target || !list || !list->asns || !list->count || !result)
		return -1;

	if (target->family != AF_INET && target->family != AF_INET6)
		return -1;

	if (!observer || !observer->observe)
		return -2;

	memset(&local_observation, 0, sizeof(local_observation));
	local_observation.target = *target;
	local_observation.source = observer->name;

	ret = observer->observe(target, &local_observation, observer->arg);
	if (ret)
		return ret;

	if (local_observation.observed_asn_count > MIDR_TIER1_MAX_OBSERVED_ASNS)
		return -1;

	ret = midr_tier1_observed_path_check(
		local_observation.observed_asns,
		local_observation.observed_asn_count, list, result);
	if (ret)
		return ret;

	if (observation)
		*observation = local_observation;

	return 0;
}

int midr_tier1_observed_path_check(const as_t *observed_asns,
				   size_t observed_asn_count,
				   const struct midr_tier1_list *list,
				   struct midr_tier1_result *result)
{
	bool have_previous_asn = false;
	as_t previous_asn = 0;
	size_t i;

	if (!list || !list->asns || !list->count || !result
	    || (observed_asn_count && !observed_asns))
		return -1;

	midr_tier1_result_init(result);
	result->tier1_list_version = list->version;

	if (!observed_asn_count) {
		result->flags |= MIDR_TIER1_FLAG_EMPTY_PATH;
		return 0;
	}

	for (i = 0; i < observed_asn_count; i++)
		midr_tier1_process_ordered_asn(observed_asns[i], list, result,
					       &have_previous_asn,
					       &previous_asn, true);

	result->tier1_ordered_observed = result->ordered_hit_count > 0;
	result->tier1_observed = result->tier1_ordered_observed;

	return 0;
}

int midr_tier1_aspath_check(const struct aspath *aspath,
			    const struct midr_tier1_list *list,
			    struct midr_tier1_result *result)
{
	const struct assegment *seg;
	bool have_previous_asn = false;
	as_t previous_asn = 0;

	if (!aspath || !list || !list->asns || !list->count || !result)
		return -1;

	midr_tier1_result_init(result);
	result->tier1_list_version = list->version;

	if (!aspath->segments) {
		result->flags |= MIDR_TIER1_FLAG_EMPTY_PATH;
		return 0;
	}

	for (seg = aspath->segments; seg; seg = seg->next) {
		unsigned int i;

		switch (seg->type) {
		case AS_SEQUENCE:
			for (i = 0; i < seg->length; i++)
				midr_tier1_process_ordered_asn(
					seg->as[i], list, result,
					&have_previous_asn, &previous_asn,
					false);
			break;
		case AS_SET:
			result->flags |= MIDR_TIER1_FLAG_AS_SET_PRESENT;
			for (i = 0; i < seg->length; i++)
				midr_tier1_process_unordered_asn(seg->as[i],
								 list, result);
			break;
		case AS_CONFED_SEQUENCE:
		case AS_CONFED_SET:
			result->flags |= MIDR_TIER1_FLAG_CONFED_SEGMENTS_IGNORED;
			result->ignored_confed_asns += seg->length;
			break;
		default:
			result->flags |= MIDR_TIER1_FLAG_UNKNOWN_SEGMENT_IGNORED;
			break;
		}
	}

	result->tier1_ordered_observed = result->ordered_hit_count > 0;
	result->tier1_unordered_observed = result->unordered_hit_count > 0;
	result->tier1_observed = result->tier1_ordered_observed
				 || result->tier1_unordered_observed;

	return 0;
}
