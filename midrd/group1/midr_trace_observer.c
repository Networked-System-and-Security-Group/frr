// SPDX-License-Identifier: GPL-2.0-or-later
#include <zebra.h>
#include "midrd/group1/midr_ip2asn.h"
#include "midrd/group1/midr_trace_observer.h"

void midr_trace_map_ip2asn(const struct midr_trace_job_result *result,
			   struct midr_trace_query_view *view)
{
	size_t i;

	if (!view)
		return;
	memset(view, 0, sizeof(*view));
	if (!result) {
		view->mapping_status = MIDR_TRACE_MAPPING_NOT_APPLICABLE;
		return;
	}

	view->job = *result;
	if (result->status != MIDR_TRACE_OK
	    || result->raw_path.hop_count > MIDR_TIER1_MAX_OBSERVED_ASNS) {
		view->mapping_status = MIDR_TRACE_MAPPING_NOT_APPLICABLE;
		return;
	}
	if (!midr_ip2asn_is_loaded()) {
		view->mapping_status = MIDR_TRACE_MAPPING_NO_SNAPSHOT;
		return;
	}

	view->ip2asn_generation = midr_ip2asn_generation();
	view->has_ip2asn_generation = true;
	view->observation.target = result->target;
	view->observation.source = "traceroute-ip2asn";
	for (i = 0; i < result->raw_path.hop_count; i++) {
		const struct midr_trace_raw_hop *hop =
			&result->raw_path.hops[i];
		as_t asn = 0;

		if (hop->visible
		    && !midr_ip2asn_lookup(&hop->address, &asn, NULL))
			asn = 0;
		view->observation
			.observed_asns[view->observation.observed_asn_count++] =
			asn;
	}
	view->mapping_status = MIDR_TRACE_MAPPING_OK;
	view->has_observation = true;
}

int midr_trace_observe_path(const struct prefix *target,
			    struct midr_tier1_observation *observation)
{
	struct midr_trace_request_options options = {};
	struct midr_trace_query_view view;
	enum midr_trace_cache_lookup_rc lookup_rc;
	uint64_t job_id;
	enum midr_trace_ensure_state ensure_state;

	if (!target || !observation)
		return -1;

	lookup_rc = midr_trace_cache_lookup(target, &options, &view, NULL);
	if (lookup_rc == MIDR_TRACE_LOOKUP_NO_SNAPSHOT)
		return -2;
	if (lookup_rc == MIDR_TRACE_LOOKUP_HIT) {
		if (view.job.status != MIDR_TRACE_OK || !view.has_observation)
			return -1;
		*observation = view.observation;
		return 0;
	}

	(void)midr_trace_ensure_job(target, &options, &job_id,
				    &ensure_state);
	return -1;
}

static int midr_trace_observe_cb(
	const struct prefix *target,
	struct midr_tier1_observation *observation, void *arg)
{
	(void)arg;
	return midr_trace_observe_path(target, observation);
}

static const struct midr_tier1_observer midr_trace_observer = {
	.name = "traceroute-ip2asn-cache",
	.observe = midr_trace_observe_cb,
	.arg = NULL,
};

const struct midr_tier1_observer *midr_trace_observer_get(void)
{
	return &midr_trace_observer;
}
