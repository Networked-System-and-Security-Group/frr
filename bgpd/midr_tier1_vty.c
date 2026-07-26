// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR Tier-1, IP-to-ASN and asynchronous traceroute VTY commands.
 */

#include <zebra.h>

#include <errno.h>
#include <inttypes.h>

#include "asn.h"
#include "command.h"
#include "lib/json.h"
#include "memory.h"
#include "prefix.h"

#include "bgpd/midr_ip2asn.h"
#include "bgpd/midr_tier1.h"
#include "bgpd/midr_tier1_vty.h"
#include "bgpd/midr_trace_scheduler.h"

/*
 * json-c's signed integer constructor cannot represent the full uint64_t
 * domain used by generations and scheduler IDs.  Keep these fields stable and
 * lossless by publishing them as decimal strings.
 */
static void midr_json_u64_add(json_object *json, const char *key,
			      uint64_t value)
{
	char buf[32];

	snprintf(buf, sizeof(buf), "%" PRIu64, value);
	json_object_string_add(json, key, buf);
}

static bool midr_vty_target_parse(struct vty *vty, const char *target,
				  struct prefix *target_prefix)
{
	int ret;

	ret = str2prefix(target, target_prefix);
	if (!ret || (target_prefix->family != AF_INET
		     && target_prefix->family != AF_INET6)) {
		vty_out(vty, "%% Invalid target IP node: %s\n", target);
		return false;
	}

	return true;
}

static bool midr_vty_u64_parse(struct vty *vty, const char *value,
			       uint64_t *number)
{
	unsigned long long parsed;
	const unsigned char *p;
	char *endp = NULL;

	if (!value || !value[0])
		goto invalid;
	for (p = (const unsigned char *)value; *p; p++)
		if (*p < '0' || *p > '9')
			goto invalid;

	errno = 0;
	parsed = strtoull(value, &endp, 10);
	if (errno || !endp || *endp || parsed == 0)
		goto invalid;

	*number = (uint64_t)parsed;
	return true;

invalid:
	vty_out(vty, "%% Invalid job ID: %s\n", value ? value : "");
	return false;
}

static int midr_tier1_parse_observed_asns(struct vty *vty,
					  struct cmd_token **argv, int argc,
					  int start_idx, as_t **observed_asns,
					  size_t *observed_asn_count)
{
	size_t count = 0;
	size_t out_idx = 0;
	as_t *asns;
	int i;

	for (i = start_idx; i < argc; i++) {
		if (!strcmp(argv[i]->arg, "json"))
			continue;
		count++;
	}

	if (!count) {
		vty_out(vty, "%% At least one observed ASN is required\n");
		return CMD_WARNING;
	}

	if (count > MIDR_TIER1_MAX_OBSERVED_ASNS) {
		vty_out(vty, "%% Too many observed ASNs; maximum is %u\n",
			MIDR_TIER1_MAX_OBSERVED_ASNS);
		return CMD_WARNING;
	}

	asns = XCALLOC(MTYPE_TMP, sizeof(*asns) * count);
	for (i = start_idx; i < argc; i++) {
		unsigned long long asn;
		char *endp = NULL;

		if (!strcmp(argv[i]->arg, "json"))
			continue;

		errno = 0;
		asn = strtoull(argv[i]->arg, &endp, 10);
		if (errno || !endp || *endp || asn > UINT32_MAX) {
			vty_out(vty, "%% Invalid observed ASN value: %s\n",
				argv[i]->arg);
			XFREE(MTYPE_TMP, asns);
			return CMD_WARNING;
		}

		asns[out_idx++] = (as_t)asn;
	}

	*observed_asns = asns;
	*observed_asn_count = count;
	return CMD_SUCCESS;
}

struct midr_tier1_manual_observer_arg {
	const as_t *observed_asns;
	size_t observed_asn_count;
};

static int midr_tier1_manual_observe(
	const struct prefix *target, struct midr_tier1_observation *observation,
	void *arg)
{
	struct midr_tier1_manual_observer_arg *manual = arg;

	if (!manual || manual->observed_asn_count > MIDR_TIER1_MAX_OBSERVED_ASNS)
		return -1;

	observation->target = *target;
	observation->source = "manual-observed-as-path";
	observation->observed_asn_count = manual->observed_asn_count;
	memcpy(observation->observed_asns, manual->observed_asns,
	       sizeof(*manual->observed_asns) * manual->observed_asn_count);
	return 0;
}

static void midr_tier1_json_asn_array_add(json_object *array,
					  const as_t *asns, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++)
		asn_asn2json_array(array, asns[i], ASNOTATION_PLAIN);
}

static void midr_tier1_vty_print_asn_list(struct vty *vty,
					 const as_t *asns, size_t count)
{
	size_t i;

	if (!count) {
		vty_out(vty, "none");
		return;
	}

	for (i = 0; i < count; i++)
		vty_out(vty, "%s%u", i ? " " : "", asns[i]);
}

static void midr_tier1_vty_json(
	struct vty *vty, const char *target,
	const struct midr_tier1_result *result,
	const struct midr_tier1_observation *observation,
	const struct midr_trace_query_view *view)
{
	json_object *json = json_object_new_object();
	json_object *json_path = json_object_new_array();
	json_object *json_hits = json_object_new_array();

	json_object_string_add(json, "target", target);
	json_object_string_add(json, "status", "ok");
	json_object_string_add(json, "source", observation->source);
	json_object_string_add(json, "tier1ListVersion",
			       result->tier1_list_version);
	json_object_boolean_add(json, "tier1Observed",
				result->tier1_observed);
	json_object_int_add(json, "normalizedHops", result->normalized_hops);
	json_object_int_add(json, "flags", result->flags);
	json_object_int_add(json, "ignoredPrivateAsns",
			    result->ignored_private_asns);
	json_object_int_add(json, "ignoredUnmappedAsns",
			    result->ignored_unmapped_asns);

	midr_tier1_json_asn_array_add(json_path, observation->observed_asns,
				     observation->observed_asn_count);
	json_object_object_add(json, "observedAsPath", json_path);
	midr_tier1_json_asn_array_add(json_hits, result->ordered_hits,
				     result->ordered_hit_count);
	json_object_object_add(json, "tier1Hits", json_hits);

	if (view) {
		midr_json_u64_add(json, "jobId", view->job.job_id);
		midr_json_u64_add(json, "ip2asnGeneration",
				  view->ip2asn_generation);
		json_object_int_add(json, "queueMsec", view->job.queue_msec);
		json_object_int_add(json, "executionMsec",
				    view->job.execution_msec);
	}

	vty_json(vty, json);
}

static void midr_tier1_vty_text(
	struct vty *vty, const char *target,
	const struct midr_tier1_result *result,
	const struct midr_tier1_observation *observation,
	const struct midr_trace_query_view *view)
{
	vty_out(vty, "MIDR Tier-1 observation for %s\n", target);
	vty_out(vty, "  Source: %s\n", observation->source);
	if (view) {
		vty_out(vty, "  Job ID: %" PRIu64 "\n", view->job.job_id);
		vty_out(vty, "  IP-to-ASN generation: %" PRIu64 "\n",
			view->ip2asn_generation);
		vty_out(vty, "  Queue/execution: %u/%u ms\n",
			view->job.queue_msec, view->job.execution_msec);
	}
	vty_out(vty, "  Tier-1 list: %s\n", result->tier1_list_version);
	vty_out(vty, "  Observed AS path: ");
	midr_tier1_vty_print_asn_list(vty, observation->observed_asns,
				      observation->observed_asn_count);
	vty_out(vty, "\n");
	vty_out(vty, "  Normalized hops: %u\n", result->normalized_hops);
	vty_out(vty, "  Tier-1 observed: %s\n",
		result->tier1_observed ? "yes" : "no");
	vty_out(vty, "  Tier-1 hits: ");
	midr_tier1_vty_print_asn_list(vty, result->ordered_hits,
				      result->ordered_hit_count);
	vty_out(vty, "\n");
	vty_out(vty, "  Ignored private ASNs: %u\n",
		result->ignored_private_asns);
	vty_out(vty, "  Ignored unmapped ASNs: %u\n",
		result->ignored_unmapped_asns);
	vty_out(vty, "  Result flags: 0x%x\n", result->flags);
}

static const char *
midr_trace_mapping_status_name(enum midr_trace_mapping_status status)
{
	switch (status) {
	case MIDR_TRACE_MAPPING_OK:
		return "ok";
	case MIDR_TRACE_MAPPING_NOT_APPLICABLE:
		return "not-applicable";
	case MIDR_TRACE_MAPPING_NO_SNAPSHOT:
		return "snapshot-not-loaded";
	}

	return "unknown";
}

static void midr_trace_raw_hops_json_add(
	json_object *array, const struct midr_trace_raw_path *raw_path)
{
	char buf[PREFIX_STRLEN];
	size_t i;

	for (i = 0; i < raw_path->hop_count; i++) {
		if (!raw_path->hops[i].visible) {
			json_object_array_add(array, json_object_new_null());
			continue;
		}

		json_object_array_add(
			array,
			json_object_new_string(prefix2str(
				&raw_path->hops[i].address, buf, sizeof(buf))));
	}
}

static void midr_trace_vty_json(
	struct vty *vty, const char *target,
	const struct midr_trace_query_view *view, bool has_cache_metadata,
	enum midr_trace_cache_state cache_state, uint32_t cache_age_msec)
{
	json_object *json = json_object_new_object();
	json_object *json_raw = json_object_new_array();
	json_object *json_path = json_object_new_array();

	json_object_string_add(json, "target", target);
	json_object_string_add(json, "status",
			       midr_trace_status_name(view->job.status));
	midr_json_u64_add(json, "jobId", view->job.job_id);
	json_object_int_add(json, "queueMsec", view->job.queue_msec);
	json_object_int_add(json, "executionMsec", view->job.execution_msec);
	json_object_string_add(
		json, "mappingStatus",
		midr_trace_mapping_status_name(view->mapping_status));
	json_object_boolean_add(json, "outputTruncated",
				view->job.raw_path.output_truncated);

	midr_trace_raw_hops_json_add(json_raw, &view->job.raw_path);
	json_object_object_add(json, "rawHops", json_raw);

	if (view->has_ip2asn_generation)
		midr_json_u64_add(json, "ip2asnGeneration",
				  view->ip2asn_generation);
	if (view->has_observation) {
		json_object_string_add(json, "source", view->observation.source);
		midr_tier1_json_asn_array_add(
			json_path, view->observation.observed_asns,
			view->observation.observed_asn_count);
	}
	json_object_object_add(json, "observedAsPath", json_path);

	if (view->job.has_wait_status) {
		json_object_boolean_add(json, "exitedNormally",
					view->job.exited_normally);
		json_object_int_add(json, "childExitCode",
				    view->job.child_exit_code);
		json_object_int_add(json, "childSignal",
				    view->job.child_signal);
	}

	if (has_cache_metadata) {
		json_object_string_add(
			json, "cacheState",
			midr_trace_cache_state_name(cache_state));
		json_object_int_add(json, "cacheAgeMsec", cache_age_msec);
	}

	vty_json(vty, json);
}

static void midr_trace_vty_text(
	struct vty *vty, const char *target,
	const struct midr_trace_query_view *view, bool has_cache_metadata,
	enum midr_trace_cache_state cache_state, uint32_t cache_age_msec)
{
	char buf[PREFIX_STRLEN];
	size_t i;

	vty_out(vty, "MIDR traceroute result for %s\n", target);
	vty_out(vty, "  Status: %s\n",
		midr_trace_status_name(view->job.status));
	vty_out(vty, "  Job ID: %" PRIu64 "\n", view->job.job_id);
	vty_out(vty, "  Queue/execution: %u/%u ms\n",
		view->job.queue_msec, view->job.execution_msec);
	if (has_cache_metadata)
		vty_out(vty, "  Cache: %s, age %u ms\n",
			midr_trace_cache_state_name(cache_state),
			cache_age_msec);
	vty_out(vty, "  Raw hops: ");
	if (!view->job.raw_path.hop_count)
		vty_out(vty, "none");
	for (i = 0; i < view->job.raw_path.hop_count; i++) {
		if (!view->job.raw_path.hops[i].visible)
			vty_out(vty, "%s*", i ? " " : "");
		else
			vty_out(vty, "%s%s", i ? " " : "",
				prefix2str(&view->job.raw_path.hops[i].address,
					   buf, sizeof(buf)));
	}
	vty_out(vty, "\n");
	vty_out(vty, "  Mapping: %s\n",
		midr_trace_mapping_status_name(view->mapping_status));
	if (view->has_ip2asn_generation)
		vty_out(vty, "  IP-to-ASN generation: %" PRIu64 "\n",
			view->ip2asn_generation);
	if (view->has_observation) {
		vty_out(vty, "  ASN path: ");
		midr_tier1_vty_print_asn_list(
			vty, view->observation.observed_asns,
			view->observation.observed_asn_count);
		vty_out(vty, "\n");
	}
	if (view->job.has_wait_status)
		vty_out(vty, "  Child: %s, exit %d, signal %d\n",
			view->job.exited_normally ? "exited" : "signaled",
			view->job.child_exit_code, view->job.child_signal);
}

static void midr_trace_vty_error(struct vty *vty, bool uj,
				 const char *target, const char *status,
				 const char *message)
{
	if (uj) {
		json_object *json = json_object_new_object();

		json_object_string_add(json, "target", target);
		json_object_string_add(json, "status", status);
		json_object_string_add(json, "message", message);
		vty_json(vty, json);
		return;
	}

	vty_out(vty, "MIDR traceroute request for %s\n", target);
	vty_out(vty, "  Status: %s\n", status);
	vty_out(vty, "  Message: %s\n", message);
}

static const char *
midr_trace_submit_rc_name(enum midr_trace_submit_rc rc)
{
	switch (rc) {
	case MIDR_TRACE_SUBMIT_ACCEPTED:
		return "accepted";
	case MIDR_TRACE_SUBMIT_INVALID:
		return "invalid-request";
	case MIDR_TRACE_SUBMIT_NOT_READY:
		return "snapshot-not-loaded";
	case MIDR_TRACE_SUBMIT_UNSUPPORTED:
		return "unsupported";
	case MIDR_TRACE_SUBMIT_QUEUE_FULL:
		return "queue-full";
	case MIDR_TRACE_SUBMIT_REQUEST_LIMIT:
		return "request-limit";
	case MIDR_TRACE_SUBMIT_ID_EXHAUSTED:
		return "id-exhausted";
	case MIDR_TRACE_SUBMIT_SHUTDOWN:
		return "shutdown";
	}

	return "unknown";
}

static void midr_trace_vty_pending(struct vty *vty, bool uj,
				   const char *target, uint64_t job_id,
				   const char *state, bool tier1)
{
	if (uj) {
		json_object *json = json_object_new_object();

		json_object_string_add(json, "target", target);
		json_object_string_add(json, "status", state);
		midr_json_u64_add(json, "jobId", job_id);
		json_object_string_add(
			json, "pollCommand",
			tier1 ? "show midr tier1 job <job-id>"
			      : "show midr traceroute job <job-id>");
		vty_json(vty, json);
		return;
	}

	vty_out(vty, "MIDR traceroute request for %s\n", target);
	vty_out(vty, "  Status: %s\n", state);
	vty_out(vty, "  Job ID: %" PRIu64 "\n", job_id);
	vty_out(vty, "  Poll with: show midr %s job %" PRIu64 "\n",
		tier1 ? "tier1" : "traceroute", job_id);
}

static int midr_tier1_vty_from_view(
	struct vty *vty, const char *target,
	const struct midr_trace_query_view *view, bool uj)
{
	struct midr_tier1_result result;

	if (view->job.status != MIDR_TRACE_OK || !view->has_observation) {
		if (uj)
			midr_trace_vty_json(vty, target, view, false,
					    MIDR_TRACE_CACHE_MISS, 0);
		else
			midr_trace_vty_text(vty, target, view, false,
					    MIDR_TRACE_CACHE_MISS, 0);
		return CMD_WARNING;
	}

	if (midr_tier1_observed_path_check(
		    view->observation.observed_asns,
		    view->observation.observed_asn_count,
		    &midr_tier1_common_seed_202607, &result)) {
		midr_trace_vty_error(vty, uj, target, "tier1-failed",
				     "Tier-1 path evaluation failed");
		return CMD_WARNING;
	}

	if (uj)
		midr_tier1_vty_json(vty, target, &result, &view->observation,
				    view);
	else
		midr_tier1_vty_text(vty, target, &result, &view->observation,
				    view);
	return CMD_SUCCESS;
}

static int midr_trace_vty_target(struct vty *vty, const char *target,
				 const struct prefix *target_prefix,
				 bool refresh, bool uj, bool tier1)
{
	struct midr_trace_request_options options = {
		.force_refresh = refresh,
	};
	struct midr_trace_query_view view;
	enum midr_trace_cache_lookup_rc lookup_rc;
	enum midr_trace_ensure_state ensure_state;
	enum midr_trace_submit_rc submit_rc;
	uint32_t cache_age_msec = 0;
	uint64_t job_id = 0;

	lookup_rc = midr_trace_cache_lookup(target_prefix, &options, &view,
					    &cache_age_msec);
	if (lookup_rc == MIDR_TRACE_LOOKUP_NO_SNAPSHOT) {
		midr_trace_vty_error(
			vty, uj, target, "snapshot-not-loaded",
			"configure a MIDR IP-to-ASN snapshot before tracing");
		return CMD_WARNING;
	}
	if (lookup_rc == MIDR_TRACE_LOOKUP_HIT) {
		enum midr_trace_cache_state hit_state =
			view.job.status == MIDR_TRACE_OK
				? MIDR_TRACE_CACHE_HIT
				: MIDR_TRACE_CACHE_NEGATIVE_HIT;

		if (tier1)
			return midr_tier1_vty_from_view(vty, target, &view, uj);
		if (uj)
			midr_trace_vty_json(vty, target, &view, true,
					    hit_state,
					    cache_age_msec);
		else
			midr_trace_vty_text(vty, target, &view, true,
					    hit_state,
					    cache_age_msec);
		return view.job.status == MIDR_TRACE_OK ? CMD_SUCCESS
						       : CMD_WARNING;
	}

	submit_rc = midr_trace_ensure_job(target_prefix, &options, &job_id,
					  &ensure_state);
	if (submit_rc != MIDR_TRACE_SUBMIT_ACCEPTED) {
		midr_trace_vty_error(
			vty, uj, target, midr_trace_submit_rc_name(submit_rc),
			"traceroute request was not accepted by the scheduler");
		return CMD_WARNING;
	}

	if (ensure_state == MIDR_TRACE_ENSURE_CACHE_HIT) {
		struct midr_trace_request_options cached_options = {};

		if (midr_trace_cache_lookup(target_prefix, &cached_options, &view,
					    &cache_age_msec)
		    == MIDR_TRACE_LOOKUP_HIT) {
			enum midr_trace_cache_state hit_state =
				view.job.status == MIDR_TRACE_OK
					? MIDR_TRACE_CACHE_HIT
					: MIDR_TRACE_CACHE_NEGATIVE_HIT;

			if (tier1)
				return midr_tier1_vty_from_view(vty, target,
							       &view, uj);
			if (uj)
				midr_trace_vty_json(vty, target, &view, true,
						    hit_state,
						    cache_age_msec);
			else
				midr_trace_vty_text(vty, target, &view, true,
						    hit_state,
						    cache_age_msec);
			return view.job.status == MIDR_TRACE_OK ? CMD_SUCCESS
							       : CMD_WARNING;
		}
	}

	midr_trace_vty_pending(
		vty, uj, target, job_id,
		ensure_state == MIDR_TRACE_ENSURE_RUNNING ? "running"
							 : "queued",
		tier1);
	return CMD_SUCCESS;
}

static int midr_trace_vty_job(struct vty *vty, uint64_t job_id, bool uj,
			      bool tier1)
{
	struct midr_trace_job_snapshot snapshot;
	enum midr_trace_job_query_state state;
	char target[PREFIX_STRLEN] = {};

	state = midr_trace_job_lookup(job_id, &snapshot);
	if (state == MIDR_TRACE_QUERY_NOT_FOUND_OR_EXPIRED) {
		midr_trace_vty_error(vty, uj, "-", "not-found",
				     "job does not exist or has expired");
		return CMD_WARNING;
	}

	prefix2str(&snapshot.target, target, sizeof(target));
	if (state == MIDR_TRACE_QUERY_QUEUED
	    || state == MIDR_TRACE_QUERY_RUNNING) {
		midr_trace_vty_pending(
			vty, uj, target, job_id,
			state == MIDR_TRACE_QUERY_QUEUED ? "queued" : "running",
			tier1);
		return CMD_SUCCESS;
	}

	if (!snapshot.has_result) {
		midr_trace_vty_error(vty, uj, target, "internal-error",
				     "completed job has no result");
		return CMD_WARNING;
	}

	if (tier1)
		return midr_tier1_vty_from_view(vty, target, &snapshot.view,
					       uj);

	if (uj)
		midr_trace_vty_json(vty, target, &snapshot.view, false,
				    MIDR_TRACE_CACHE_MISS, 0);
	else
		midr_trace_vty_text(vty, target, &snapshot.view, false,
				    MIDR_TRACE_CACHE_MISS, 0);
	return snapshot.view.job.status == MIDR_TRACE_OK ? CMD_SUCCESS
							 : CMD_WARNING;
}

static void midr_ip2asn_vty_status_json(struct vty *vty)
{
	struct midr_ip2asn_status status;
	json_object *json = json_object_new_object();

	midr_ip2asn_get_status(&status);
	json_object_boolean_add(json, "loaded", status.loaded);
	json_object_int_add(json, "entries", (int64_t)status.entries);
	json_object_string_add(json, "sourcePath",
			       status.source_path ? status.source_path : "");
	json_object_boolean_add(json, "dirty", status.dirty);
	midr_json_u64_add(json, "generation", status.generation);
	midr_json_u64_add(json, "processUpdatesAppliedTotal",
			  status.process_updates_applied_total);
	json_object_string_add(
		json, "lastUpdateId",
		status.last_update_id ? status.last_update_id : "");
	json_object_string_add(
		json, "lastUpdatePath",
		status.last_update_path ? status.last_update_path : "");
	json_object_int_add(json, "lastUpdateTime",
			    (int64_t)status.last_update_time.tv_sec);
	json_object_int_add(json, "lastAdded", (int64_t)status.last_added);
	json_object_int_add(json, "lastReplaced",
			    (int64_t)status.last_replaced);
	json_object_int_add(json, "lastDeleted",
			    (int64_t)status.last_deleted);
	json_object_boolean_add(json, "lastResetValid",
				status.last_reset_valid);
	json_object_string_add(
		json, "lastResetKind",
		midr_ip2asn_reset_kind_name(status.last_reset_kind));
	midr_json_u64_add(json, "lastResetOldGeneration",
			  status.last_reset_old_generation);
	json_object_string_add(
		json, "lastResetDiscardedUpdateId",
		status.last_reset_discarded_update_id
			? status.last_reset_discarded_update_id
			: "");
	json_object_int_add(json, "lastResetTime",
			    (int64_t)status.last_reset_time.tv_sec);
	vty_json(vty, json);
}

static void midr_ip2asn_vty_status_text(struct vty *vty)
{
	struct midr_ip2asn_status status;

	midr_ip2asn_get_status(&status);
	vty_out(vty, "MIDR IP-to-ASN snapshot\n");
	vty_out(vty, "  Status: %s\n", status.loaded ? "loaded" : "not loaded");
	vty_out(vty, "  Source: %s\n",
		status.source_path ? status.source_path : "-");
	vty_out(vty, "  Entries: %lu\n", status.entries);
	vty_out(vty, "  Generation: %" PRIu64 "\n", status.generation);
	vty_out(vty, "  Runtime updates dirty: %s\n",
		status.dirty ? "yes" : "no");
	vty_out(vty, "  Process updates applied: %" PRIu64 "\n",
		status.process_updates_applied_total);
	if (status.last_update_id && status.last_update_id[0]) {
		vty_out(vty, "  Last update ID: %s\n", status.last_update_id);
		vty_out(vty, "  Last update path: %s\n",
			status.last_update_path ? status.last_update_path : "-");
		vty_out(vty, "  Last update time (epoch): %lld\n",
			(long long)status.last_update_time.tv_sec);
		vty_out(vty, "  Last update add/replace/delete: %lu/%lu/%lu\n",
			status.last_added, status.last_replaced,
			status.last_deleted);
	}
	if (status.last_reset_valid) {
		vty_out(vty, "  Last reset: %s\n",
			midr_ip2asn_reset_kind_name(status.last_reset_kind));
		vty_out(vty, "  Last reset old generation: %" PRIu64 "\n",
			status.last_reset_old_generation);
		vty_out(vty, "  Last reset discarded update ID: %s\n",
			status.last_reset_discarded_update_id
				? status.last_reset_discarded_update_id
				: "-");
		vty_out(vty, "  Last reset time (epoch): %lld\n",
			(long long)status.last_reset_time.tv_sec);
	}
	if (status.dirty)
		vty_out(vty,
			"  Warning: runtime deltas are not persisted by write memory\n");
}

static void midr_ip2asn_vty_lookup_json(struct vty *vty,
					const char *target, bool loaded,
					bool mapped, as_t asn,
					const struct prefix *matched_prefix)
{
	json_object *json = json_object_new_object();
	char prefix_buf[PREFIX_STRLEN];

	json_object_string_add(json, "target", target);
	json_object_boolean_add(json, "loaded", loaded);
	json_object_boolean_add(json, "mapped", mapped);
	midr_json_u64_add(json, "generation", midr_ip2asn_generation());
	if (!loaded)
		json_object_string_add(json, "status", "snapshot-not-loaded");
	else if (!mapped)
		json_object_string_add(json, "status", "not-found");
	else {
		json_object_string_add(json, "status", "mapped");
		asn_asn2json(json, "asn", asn, ASNOTATION_PLAIN);
		json_object_string_add(
			json, "matchedPrefix",
			prefix2str(matched_prefix, prefix_buf,
				   sizeof(prefix_buf)));
	}
	vty_json(vty, json);
}

static void midr_ip2asn_vty_lookup_text(struct vty *vty,
					const char *target, bool loaded,
					bool mapped, as_t asn,
					const struct prefix *matched_prefix)
{
	char prefix_buf[PREFIX_STRLEN];

	vty_out(vty, "MIDR IP-to-ASN lookup for %s\n", target);
	vty_out(vty, "  Generation: %" PRIu64 "\n",
		midr_ip2asn_generation());
	if (!loaded) {
		vty_out(vty, "  Status: snapshot not loaded\n");
		return;
	}
	if (!mapped) {
		vty_out(vty, "  Status: no matching prefix\n");
		return;
	}
	vty_out(vty, "  Status: mapped\n");
	vty_out(vty, "  ASN: %u\n", asn);
	vty_out(vty, "  Matched prefix: %s\n",
		prefix2str(matched_prefix, prefix_buf, sizeof(prefix_buf)));
}

DEFUN(midr_ip2asn_file,
      midr_ip2asn_file_cmd,
      "midr ip2asn file WORD [discard-runtime-updates]",
      "MIDR overlay routing\n"
      "Local IP-to-ASN mapper\n"
      "Load a Prefix2AS/CAIDA snapshot file\n"
      "Snapshot path\n"
      "Explicitly discard unapplied-to-disk runtime updates\n")
{
	const int idx_path = 3;
	struct midr_ip2asn_load_result result = {};
	unsigned int flags = MIDR_IP2ASN_REPLACE_NONE;
	char errmsg[256] = {};
	int idx;

	if (argv_find(argv, argc, "discard-runtime-updates", &idx))
		flags |= MIDR_IP2ASN_REPLACE_DISCARD_DIRTY;
	if (midr_ip2asn_load_file_ex(argv[idx_path]->arg, flags, &result,
				     errmsg, sizeof(errmsg))) {
		vty_out(vty, "%% %s\n", errmsg[0] ? errmsg
						  : "MIDR IP-to-ASN load failed");
		return CMD_WARNING_CONFIG_FAILED;
	}

	vty_out(vty,
		"MIDR IP-to-ASN snapshot loaded: %lu entries, generation %" PRIu64
		"\n",
		result.entries, result.new_generation);
	return CMD_SUCCESS;
}

DEFUN(no_midr_ip2asn_file,
      no_midr_ip2asn_file_cmd,
      "no midr ip2asn file [discard-runtime-updates]",
      NO_STR
      "MIDR overlay routing\n"
      "Local IP-to-ASN mapper\n"
      "Clear the loaded Prefix2AS/CAIDA snapshot\n"
      "Explicitly discard unapplied-to-disk runtime updates\n")
{
	unsigned int flags = MIDR_IP2ASN_REPLACE_NONE;
	char errmsg[256] = {};
	int idx;

	if (argv_find(argv, argc, "discard-runtime-updates", &idx))
		flags |= MIDR_IP2ASN_REPLACE_DISCARD_DIRTY;
	if (midr_ip2asn_clear_ex(flags, errmsg, sizeof(errmsg))) {
		vty_out(vty, "%% %s\n", errmsg[0] ? errmsg
						  : "MIDR IP-to-ASN clear failed");
		return CMD_WARNING_CONFIG_FAILED;
	}
	return CMD_SUCCESS;
}

DEFUN(midr_ip2asn_update_file_vty,
      midr_ip2asn_update_file_cmd,
      "midr ip2asn update file WORD [validate-only]",
      "MIDR overlay routing\n"
      "Local IP-to-ASN mapper\n"
      "Apply a transactional update file\n"
      "Update file\n"
      "Update path\n"
      "Validate the full transaction without changing the active snapshot\n")
{
	const int idx_path = 4;
	struct midr_ip2asn_update_result result = {};
	char errmsg[512] = {};
	bool validate_only;
	int idx;

	validate_only = argv_find(argv, argc, "validate-only", &idx);
	if (midr_ip2asn_update_file(argv[idx_path]->arg, validate_only, &result,
				    errmsg, sizeof(errmsg))) {
		vty_out(vty, "%% %s\n", errmsg[0] ? errmsg
						  : "MIDR IP-to-ASN update failed");
		return CMD_WARNING;
	}

	vty_out(vty, "MIDR IP-to-ASN update %s\n",
		validate_only ? "validation succeeded" : "applied");
	vty_out(vty, "  Update ID: %s\n", result.update_id);
	vty_out(vty, "  Base generation: %" PRIu64 "\n",
		result.base_generation);
	if (!validate_only)
		vty_out(vty, "  New generation: %" PRIu64 "\n",
			result.new_generation);
	vty_out(vty, "  %s add/replace/delete: %lu/%lu/%lu\n",
		validate_only ? "Would" : "Applied", result.added,
		result.replaced, result.deleted);
	vty_out(vty, "  Entries: %lu\n", result.entries);
	if (validate_only)
		vty_out(vty, "  No changes applied\n");
	return CMD_SUCCESS;
}

DEFUN(show_midr_ip2asn,
      show_midr_ip2asn_cmd,
      "show midr ip2asn [json]",
      SHOW_STR
      "MIDR overlay routing\n"
      "Local IP-to-ASN mapper\n"
      JSON_STR)
{
	if (use_json(argc, argv))
		midr_ip2asn_vty_status_json(vty);
	else
		midr_ip2asn_vty_status_text(vty);
	return CMD_SUCCESS;
}

DEFUN(show_midr_ip2asn_lookup,
      show_midr_ip2asn_lookup_cmd,
      "show midr ip2asn <A.B.C.D|X:X::X:X> [json]",
      SHOW_STR
      "MIDR overlay routing\n"
      "Local IP-to-ASN mapper\n"
      "IPv4 address to query\n"
      "IPv6 address to query\n"
      JSON_STR)
{
	const int idx_target = 3;
	const char *target = argv[idx_target]->arg;
	struct prefix target_prefix;
	struct prefix matched_prefix = {};
	bool loaded;
	bool mapped = false;
	bool uj = use_json(argc, argv);
	as_t asn = 0;

	if (!midr_vty_target_parse(vty, target, &target_prefix))
		return CMD_WARNING;
	loaded = midr_ip2asn_is_loaded();
	if (loaded)
		mapped = midr_ip2asn_lookup(&target_prefix, &asn,
					     &matched_prefix);
	if (uj)
		midr_ip2asn_vty_lookup_json(vty, target, loaded, mapped, asn,
					     &matched_prefix);
	else
		midr_ip2asn_vty_lookup_text(vty, target, loaded, mapped, asn,
					     &matched_prefix);
	return loaded ? CMD_SUCCESS : CMD_WARNING;
}

DEFUN(show_midr_traceroute,
      show_midr_traceroute_cmd,
      "show midr traceroute <A.B.C.D|X:X::X:X> [refresh] [json]",
      SHOW_STR
      "MIDR overlay routing\n"
      "Trace an underlay path and map its hops to ASNs\n"
      "Target IPv4 node\n"
      "Target IPv6 node\n"
      "Bypass a cached result\n"
      JSON_STR)
{
	const int idx_target = 3;
	const char *target = argv[idx_target]->arg;
	struct prefix target_prefix;
	int idx;

	if (!midr_vty_target_parse(vty, target, &target_prefix))
		return CMD_WARNING;
	return midr_trace_vty_target(
		vty, target, &target_prefix,
		argv_find(argv, argc, "refresh", &idx),
		use_json(argc, argv), false);
}

DEFUN(show_midr_traceroute_job,
      show_midr_traceroute_job_cmd,
      "show midr traceroute job WORD [json]",
      SHOW_STR
      "MIDR overlay routing\n"
      "Traceroute scheduler\n"
      "Query a queued, running, or recent job\n"
      "Job ID\n"
      JSON_STR)
{
	uint64_t job_id;

	if (!midr_vty_u64_parse(vty, argv[4]->arg, &job_id))
		return CMD_WARNING;
	return midr_trace_vty_job(vty, job_id, use_json(argc, argv), false);
}

DEFUN(show_midr_tier1_auto,
      show_midr_tier1_auto_cmd,
      "show midr tier1 <A.B.C.D|X:X::X:X> [refresh] [json]",
      SHOW_STR
      "MIDR overlay routing\n"
      "Check whether an observed underlay path crosses Tier-1 ASNs\n"
      "Target IPv4 node\n"
      "Target IPv6 node\n"
      "Bypass a cached result\n"
      JSON_STR)
{
	const int idx_target = 3;
	const char *target = argv[idx_target]->arg;
	struct prefix target_prefix;
	int idx;

	if (!midr_vty_target_parse(vty, target, &target_prefix))
		return CMD_WARNING;
	return midr_trace_vty_target(
		vty, target, &target_prefix,
		argv_find(argv, argc, "refresh", &idx),
		use_json(argc, argv), true);
}

DEFUN(show_midr_tier1_job,
      show_midr_tier1_job_cmd,
      "show midr tier1 job WORD [json]",
      SHOW_STR
      "MIDR overlay routing\n"
      "Tier-1 path observation\n"
      "Query a queued, running, or recent traceroute job\n"
      "Job ID\n"
      JSON_STR)
{
	uint64_t job_id;

	if (!midr_vty_u64_parse(vty, argv[4]->arg, &job_id))
		return CMD_WARNING;
	return midr_trace_vty_job(vty, job_id, use_json(argc, argv), true);
}

DEFUN(show_midr_tier1,
      show_midr_tier1_cmd,
      "show midr tier1 <A.B.C.D|X:X::X:X> observed-as-path (0-4294967295)... [json]",
      SHOW_STR
      "MIDR overlay routing\n"
      "Check whether an observed underlay path crosses Tier-1 ASNs\n"
      "Target IPv4 node\n"
      "Target IPv6 node\n"
      "Use a manually observed traceroute/IP-to-ASN sequence\n"
      "Observed ASN sequence; use 0 for unmapped traceroute hops\n"
      JSON_STR)
{
	const int idx_target = 3;
	const int idx_asn = 5;
	const char *target = argv[idx_target]->arg;
	struct prefix target_prefix;
	struct midr_tier1_result result;
	struct midr_tier1_observation observation;
	struct midr_tier1_manual_observer_arg manual;
	struct midr_tier1_observer observer = {
		.name = "manual-observed-as-path",
		.observe = midr_tier1_manual_observe,
		.arg = &manual,
	};
	as_t *observed_asns = NULL;
	size_t observed_asn_count = 0;
	bool uj = use_json(argc, argv);
	int ret;

	if (!midr_vty_target_parse(vty, target, &target_prefix))
		return CMD_WARNING;
	ret = midr_tier1_parse_observed_asns(vty, argv, argc, idx_asn,
					     &observed_asns,
					     &observed_asn_count);
	if (ret != CMD_SUCCESS)
		return ret;
	manual.observed_asns = observed_asns;
	manual.observed_asn_count = observed_asn_count;
	ret = midr_tier1_target_check(&target_prefix, &observer,
				      &midr_tier1_common_seed_202607,
				      &result, &observation);
	if (ret) {
		vty_out(vty, "%% MIDR Tier-1 check failed\n");
		XFREE(MTYPE_TMP, observed_asns);
		return CMD_WARNING;
	}
	if (uj)
		midr_tier1_vty_json(vty, target, &result, &observation, NULL);
	else
		midr_tier1_vty_text(vty, target, &result, &observation, NULL);
	XFREE(MTYPE_TMP, observed_asns);
	return CMD_SUCCESS;
}

static int midr_trace_config_apply(
	struct vty *vty, const struct midr_trace_scheduler_config *config)
{
	char errmsg[256] = {};

	if (midr_trace_scheduler_config_set(config, errmsg, sizeof(errmsg))) {
		vty_out(vty, "%% %s\n",
			errmsg[0] ? errmsg : "invalid traceroute configuration");
		return CMD_WARNING_CONFIG_FAILED;
	}
	return CMD_SUCCESS;
}

DEFUN(midr_traceroute_concurrency,
      midr_traceroute_concurrency_cmd,
      "midr traceroute concurrency (1-32)",
      "MIDR overlay routing\n"
      "Traceroute scheduler\n"
      "Maximum concurrently running traceroute children\n"
      "Concurrency\n")
{
	struct midr_trace_scheduler_config config;

	midr_trace_scheduler_config_get(&config);
	config.concurrency = strtoul(argv[3]->arg, NULL, 10);
	return midr_trace_config_apply(vty, &config);
}

DEFUN(midr_traceroute_queue_limit,
      midr_traceroute_queue_limit_cmd,
      "midr traceroute queue-limit (1-4096)",
      "MIDR overlay routing\n"
      "Traceroute scheduler\n"
      "Maximum number of queued jobs\n"
      "Queue limit\n")
{
	struct midr_trace_scheduler_config config;

	midr_trace_scheduler_config_get(&config);
	config.queue_limit = strtoul(argv[3]->arg, NULL, 10);
	return midr_trace_config_apply(vty, &config);
}

DEFUN(midr_traceroute_queue_timeout,
      midr_traceroute_queue_timeout_cmd,
      "midr traceroute queue-timeout (0-60)",
      "MIDR overlay routing\n"
      "Traceroute scheduler\n"
      "Maximum queue residence time; zero disables the timer\n"
      "Seconds\n")
{
	struct midr_trace_scheduler_config config;

	midr_trace_scheduler_config_get(&config);
	config.queue_timeout_msec =
		strtoul(argv[3]->arg, NULL, 10) * 1000U;
	return midr_trace_config_apply(vty, &config);
}

DEFUN(midr_traceroute_timeout,
      midr_traceroute_timeout_cmd,
      "midr traceroute timeout (1-120)",
      "MIDR overlay routing\n"
      "Traceroute scheduler\n"
      "Maximum child execution time\n"
      "Seconds\n")
{
	struct midr_trace_scheduler_config config;

	midr_trace_scheduler_config_get(&config);
	config.execution_timeout_msec =
		strtoul(argv[3]->arg, NULL, 10) * 1000U;
	return midr_trace_config_apply(vty, &config);
}

DEFUN(midr_traceroute_cache_ttl,
      midr_traceroute_cache_ttl_cmd,
      "midr traceroute cache ttl (0-86400)",
      "MIDR overlay routing\n"
      "Traceroute scheduler\n"
      "Raw measurement cache\n"
      "Successful result lifetime; zero disables successful caching\n"
      "Seconds\n")
{
	struct midr_trace_scheduler_config config;

	midr_trace_scheduler_config_get(&config);
	config.success_cache_ttl_msec =
		strtoul(argv[4]->arg, NULL, 10) * 1000U;
	return midr_trace_config_apply(vty, &config);
}

DEFUN(midr_traceroute_cache_negative_ttl,
      midr_traceroute_cache_negative_ttl_cmd,
      "midr traceroute cache negative-ttl (0-300)",
      "MIDR overlay routing\n"
      "Traceroute scheduler\n"
      "Raw measurement cache\n"
      "Failed result lifetime; zero disables negative caching\n"
      "Seconds\n")
{
	struct midr_trace_scheduler_config config;

	midr_trace_scheduler_config_get(&config);
	config.negative_cache_ttl_msec =
		strtoul(argv[4]->arg, NULL, 10) * 1000U;
	return midr_trace_config_apply(vty, &config);
}

DEFUN(midr_traceroute_cache_max_entries,
      midr_traceroute_cache_max_entries_cmd,
      "midr traceroute cache max-entries (0-65536)",
      "MIDR overlay routing\n"
      "Traceroute scheduler\n"
      "Raw measurement cache\n"
      "Maximum entries; zero disables the cache\n"
      "Entry limit\n")
{
	struct midr_trace_scheduler_config config;

	midr_trace_scheduler_config_get(&config);
	config.cache_capacity = strtoul(argv[4]->arg, NULL, 10);
	return midr_trace_config_apply(vty, &config);
}

DEFUN(show_midr_traceroute_scheduler,
      show_midr_traceroute_scheduler_cmd,
      "show midr traceroute scheduler [json]",
      SHOW_STR
      "MIDR overlay routing\n"
      "Traceroute scheduler\n"
      "Scheduler configuration and counters\n"
      JSON_STR)
{
	struct midr_trace_scheduler_config config;
	struct midr_trace_scheduler_stats stats;

	midr_trace_scheduler_config_get(&config);
	midr_trace_scheduler_stats_get(&stats);
	if (use_json(argc, argv)) {
		json_object *json = json_object_new_object();

		json_object_boolean_add(json, "ready",
					midr_trace_scheduler_is_ready());
		json_object_boolean_add(json, "accepting", stats.accepting);
		json_object_int_add(json, "concurrency", config.concurrency);
		json_object_int_add(json, "queueLimit", config.queue_limit);
		json_object_int_add(json, "queueTimeoutMsec",
				    config.queue_timeout_msec);
		json_object_int_add(json, "executionTimeoutMsec",
				    config.execution_timeout_msec);
		json_object_int_add(json, "successCacheTtlMsec",
				    config.success_cache_ttl_msec);
		json_object_int_add(json, "negativeCacheTtlMsec",
				    config.negative_cache_ttl_msec);
		json_object_int_add(json, "cacheCapacity",
				    config.cache_capacity);
		json_object_int_add(json, "activeSlots", stats.active_slots);
		json_object_int_add(json, "queuedJobs", stats.queued_jobs);
		json_object_int_add(json, "inflightJobs",
				    stats.inflight_jobs);
		json_object_int_add(json, "pendingRequests",
				    stats.pending_requests);
		json_object_int_add(json, "cacheEntries",
				    stats.cache_entries);
		json_object_int_add(json, "historyEntries",
				    stats.history_entries);
		midr_json_u64_add(json, "jobsSubmitted", stats.submitted);
		midr_json_u64_add(json, "jobsCompleted", stats.completed);
		midr_json_u64_add(json, "cacheHits", stats.cache_hits);
		midr_json_u64_add(json, "cacheMisses", stats.cache_misses);
		midr_json_u64_add(json, "coalesced", stats.coalesced);
		midr_json_u64_add(json, "evicted", stats.evicted);
		midr_json_u64_add(json, "queueFull", stats.queue_full);
		midr_json_u64_add(json, "queueTimeouts",
				  stats.queue_timeouts);
		midr_json_u64_add(json, "executionTimeouts",
				  stats.execution_timeouts);
		midr_json_u64_add(json, "spawnErrors", stats.spawn_errors);
		midr_json_u64_add(json, "parseErrors", stats.parse_errors);
		midr_json_u64_add(json, "exitErrors", stats.exit_errors);
		midr_json_u64_add(json, "outputLimitErrors",
				  stats.output_limit_errors);
		midr_json_u64_add(json, "canceled", stats.canceled);
		vty_json(vty, json);
		return CMD_SUCCESS;
	}

	vty_out(vty, "MIDR traceroute scheduler\n");
	vty_out(vty, "  Ready/accepting: %s/%s\n",
		midr_trace_scheduler_is_ready() ? "yes" : "no",
		stats.accepting ? "yes" : "no");
	vty_out(vty, "  Concurrency active/limit: %u/%u\n",
		stats.active_slots, config.concurrency);
	vty_out(vty, "  Queue current/limit/timeout: %u/%u/%u ms\n",
		stats.queued_jobs, config.queue_limit,
		config.queue_timeout_msec);
	vty_out(vty, "  Execution timeout: %u ms\n",
		config.execution_timeout_msec);
	vty_out(vty, "  Inflight jobs / pending requests: %u/%u\n",
		stats.inflight_jobs, stats.pending_requests);
	vty_out(vty, "  Cache entries/limit: %u/%u\n",
		stats.cache_entries, config.cache_capacity);
	vty_out(vty, "  Cache success/negative TTL: %u/%u ms\n",
		config.success_cache_ttl_msec,
		config.negative_cache_ttl_msec);
	vty_out(vty, "  History entries: %u\n", stats.history_entries);
	vty_out(vty, "  Jobs submitted/completed: %" PRIu64 "/%" PRIu64 "\n",
		stats.submitted, stats.completed);
	vty_out(vty, "  Cache hits/misses/coalesced/evicted: %" PRIu64
		     "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "\n",
		stats.cache_hits, stats.cache_misses, stats.coalesced,
		stats.evicted);
	vty_out(vty,
		"  Errors queue-full/queue-timeout/exec-timeout/spawn/parse/exit/output: "
		"%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64
		"/%" PRIu64 "/%" PRIu64 "\n",
		stats.queue_full, stats.queue_timeouts,
		stats.execution_timeouts, stats.spawn_errors,
		stats.parse_errors, stats.exit_errors,
		stats.output_limit_errors);
	return CMD_SUCCESS;
}

DEFUN(clear_midr_traceroute_cache,
      clear_midr_traceroute_cache_cmd,
      "clear midr traceroute cache",
      CLEAR_STR
      "MIDR overlay routing\n"
      "Traceroute scheduler\n"
      "Clear all cached raw measurements\n")
{
	vty_out(vty, "Cleared %u MIDR traceroute cache entries\n",
		midr_trace_cache_clear(NULL));
	return CMD_SUCCESS;
}

DEFUN(clear_midr_traceroute_cache_target,
      clear_midr_traceroute_cache_target_cmd,
      "clear midr traceroute cache <A.B.C.D|X:X::X:X>",
      CLEAR_STR
      "MIDR overlay routing\n"
      "Traceroute scheduler\n"
      "Clear a cached raw measurement\n"
      "Target IPv4 node\n"
      "Target IPv6 node\n")
{
	struct prefix target;

	if (!midr_vty_target_parse(vty, argv[4]->arg, &target))
		return CMD_WARNING;
	vty_out(vty, "Cleared %u MIDR traceroute cache entries\n",
		midr_trace_cache_clear(&target));
	return CMD_SUCCESS;
}

void midr_tier1_vty_init(void)
{
	install_element(CONFIG_NODE, &midr_ip2asn_file_cmd);
	install_element(CONFIG_NODE, &no_midr_ip2asn_file_cmd);
	install_element(ENABLE_NODE, &midr_ip2asn_update_file_cmd);

	install_element(CONFIG_NODE, &midr_traceroute_concurrency_cmd);
	install_element(CONFIG_NODE, &midr_traceroute_queue_limit_cmd);
	install_element(CONFIG_NODE, &midr_traceroute_queue_timeout_cmd);
	install_element(CONFIG_NODE, &midr_traceroute_timeout_cmd);
	install_element(CONFIG_NODE, &midr_traceroute_cache_ttl_cmd);
	install_element(CONFIG_NODE,
			&midr_traceroute_cache_negative_ttl_cmd);
	install_element(CONFIG_NODE,
			&midr_traceroute_cache_max_entries_cmd);

	install_element(VIEW_NODE, &show_midr_ip2asn_cmd);
	install_element(VIEW_NODE, &show_midr_ip2asn_lookup_cmd);
	install_element(VIEW_NODE, &show_midr_traceroute_cmd);
	install_element(VIEW_NODE, &show_midr_traceroute_job_cmd);
	install_element(VIEW_NODE, &show_midr_traceroute_scheduler_cmd);
	install_element(VIEW_NODE, &show_midr_tier1_auto_cmd);
	install_element(VIEW_NODE, &show_midr_tier1_job_cmd);
	install_element(VIEW_NODE, &show_midr_tier1_cmd);
	install_element(ENABLE_NODE, &clear_midr_traceroute_cache_cmd);
	install_element(ENABLE_NODE,
			&clear_midr_traceroute_cache_target_cmd);
}
