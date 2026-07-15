// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR Tier-1 VTY commands.
 */

#include <zebra.h>

#include <errno.h>

#include "asn.h"
#include "command.h"
#include "lib/json.h"
#include "memory.h"
#include "prefix.h"

#include "bgpd/midr_ip2asn.h"
#include "bgpd/midr_tier1.h"
#include "bgpd/midr_tier1_vty.h"
#include "bgpd/midr_trace_observer.h"

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

static void midr_tier1_json_asn_array_add(json_object *array, const as_t *asns,
					  unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		asn_asn2json_array(array, asns[i], ASNOTATION_PLAIN);
}

static void midr_tier1_json_observed_array_add(json_object *array,
					       const as_t *asns, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++)
		asn_asn2json_array(array, asns[i], ASNOTATION_PLAIN);
}

static void midr_tier1_vty_print_asn_list(struct vty *vty, const as_t *asns,
					  unsigned int count)
{
	unsigned int i;

	if (!count) {
		vty_out(vty, "none");
		return;
	}

	for (i = 0; i < count; i++)
		vty_out(vty, "%s%u", i ? " " : "", asns[i]);
}

static void midr_tier1_vty_print_observed_list(struct vty *vty,
					       const as_t *asns, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++)
		vty_out(vty, "%s%u", i ? " " : "", asns[i]);
}

static void midr_tier1_vty_json(
	struct vty *vty, const char *target,
	const struct midr_tier1_result *result,
	const struct midr_tier1_observation *observation)
{
	json_object *json = json_object_new_object();
	json_object *json_path = json_object_new_array();
	json_object *json_hits = json_object_new_array();

	json_object_string_add(json, "target", target);
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

	midr_tier1_json_observed_array_add(json_path, observation->observed_asns,
					   observation->observed_asn_count);
	json_object_object_add(json, "observedAsPath", json_path);

	midr_tier1_json_asn_array_add(json_hits, result->ordered_hits,
				      result->ordered_hit_count);
	json_object_object_add(json, "tier1Hits", json_hits);

	vty_json(vty, json);
}

static void midr_tier1_vty_text(
	struct vty *vty, const char *target,
	const struct midr_tier1_result *result,
	const struct midr_tier1_observation *observation)
{
	vty_out(vty, "MIDR Tier-1 observation for %s\n", target);
	vty_out(vty, "  Source: %s\n", observation->source);
	vty_out(vty, "  Tier-1 list: %s\n", result->tier1_list_version);
	vty_out(vty, "  Observed AS path: ");
	midr_tier1_vty_print_observed_list(vty, observation->observed_asns,
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
	vty_out(vty, "  Ignored unmapped hops: %u\n",
		result->ignored_unmapped_asns);
	vty_out(vty, "  Result flags: 0x%x\n", result->flags);
}

static void midr_tier1_vty_unavailable_json(struct vty *vty,
					    const char *target)
{
	json_object *json = json_object_new_object();

	json_object_string_add(json, "target", target);
	json_object_string_add(json, "status", "observer-unavailable");
	json_object_string_add(
		json, "message",
		"automatic traceroute/IP-to-ASN observer requires an IP-to-ASN snapshot");

	vty_json(vty, json);
}

static void midr_tier1_vty_unavailable_text(struct vty *vty,
					    const char *target)
{
	vty_out(vty, "MIDR Tier-1 observation for %s\n", target);
	vty_out(vty, "  Status: automatic observer unavailable\n");
	vty_out(vty, "  Configure a MIDR IP-to-ASN snapshot,\n");
	vty_out(vty, "  or use observed-as-path for manual verification.\n");
}

static void midr_ip2asn_vty_status_json(struct vty *vty)
{
	json_object *json = json_object_new_object();

	json_object_boolean_add(json, "loaded", midr_ip2asn_is_loaded());
	json_object_int_add(json, "entries", midr_ip2asn_entry_count());
	if (midr_ip2asn_source_path())
		json_object_string_add(json, "sourcePath",
				       midr_ip2asn_source_path());
	else
		json_object_string_add(json, "sourcePath", "");

	vty_json(vty, json);
}

static void midr_ip2asn_vty_status_text(struct vty *vty)
{
	vty_out(vty, "MIDR IP-to-ASN snapshot\n");
	vty_out(vty, "  Status: %s\n",
		midr_ip2asn_is_loaded() ? "loaded" : "not loaded");
	vty_out(vty, "  Source: %s\n",
		midr_ip2asn_source_path() ? midr_ip2asn_source_path() : "-");
	vty_out(vty, "  Entries: %lu\n", midr_ip2asn_entry_count());
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

static void midr_trace_vty_json(
	struct vty *vty, const char *target,
	const struct midr_tier1_observation *observation)
{
	json_object *json = json_object_new_object();
	json_object *json_path = json_object_new_array();

	json_object_string_add(json, "target", target);
	json_object_string_add(json, "status", "ok");
	json_object_string_add(json, "source", observation->source);
	json_object_int_add(json, "hopCount", observation->observed_asn_count);
	midr_tier1_json_observed_array_add(json_path, observation->observed_asns,
					   observation->observed_asn_count);
	json_object_object_add(json, "observedAsPath", json_path);

	vty_json(vty, json);
}

static void midr_trace_vty_text(
	struct vty *vty, const char *target,
	const struct midr_tier1_observation *observation)
{
	vty_out(vty, "MIDR traceroute ASN path for %s\n", target);
	vty_out(vty, "  Source: %s\n", observation->source);
	vty_out(vty, "  Hop count: %zu\n", observation->observed_asn_count);
	vty_out(vty, "  ASN path: ");
	midr_tier1_vty_print_observed_list(vty, observation->observed_asns,
					   observation->observed_asn_count);
	vty_out(vty, "\n");
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

	vty_out(vty, "MIDR traceroute ASN path for %s\n", target);
	vty_out(vty, "  Status: %s\n", status);
	vty_out(vty, "  Message: %s\n", message);
}

DEFUN(midr_ip2asn_file,
      midr_ip2asn_file_cmd,
      "midr ip2asn file WORD",
      "MIDR overlay routing\n"
      "Local IP-to-ASN mapper\n"
      "Load a Prefix2AS/CAIDA snapshot file\n"
      "Snapshot path\n")
{
	const int idx_path = 3;
	char errmsg[256] = {};

	if (midr_ip2asn_load_file(argv[idx_path]->arg, errmsg, sizeof(errmsg))) {
		vty_out(vty, "%% %s\n", errmsg[0] ? errmsg
						  : "MIDR IP-to-ASN load failed");
		return CMD_WARNING_CONFIG_FAILED;
	}

	vty_out(vty, "MIDR IP-to-ASN snapshot loaded: %lu entries\n",
		midr_ip2asn_entry_count());
	return CMD_SUCCESS;
}

DEFUN(no_midr_ip2asn_file,
      no_midr_ip2asn_file_cmd,
      "no midr ip2asn file",
      NO_STR
      "MIDR overlay routing\n"
      "Local IP-to-ASN mapper\n"
      "Clear the loaded Prefix2AS/CAIDA snapshot\n")
{
	midr_ip2asn_clear();
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
      "show midr traceroute <A.B.C.D|X:X::X:X> [json]",
      SHOW_STR
      "MIDR overlay routing\n"
      "Trace an underlay path and map its hops to ASNs\n"
      "Target IPv4 node\n"
      "Target IPv6 node\n"
      JSON_STR)
{
	const int idx_target = 3;
	const char *target = argv[idx_target]->arg;
	struct midr_tier1_observation observation;
	struct prefix target_prefix;
	bool uj = use_json(argc, argv);
	int ret;

	if (!midr_vty_target_parse(vty, target, &target_prefix))
		return CMD_WARNING;

	ret = midr_trace_observe_path(&target_prefix, &observation);
	if (ret == -2) {
		midr_trace_vty_error(
			vty, uj, target, "snapshot-not-loaded",
			"configure a MIDR IP-to-ASN snapshot before tracing");
		return CMD_WARNING;
	}

	if (ret) {
		midr_trace_vty_error(
			vty, uj, target, "trace-failed",
			"traceroute execution or output parsing failed");
		return CMD_WARNING;
	}

	if (uj)
		midr_trace_vty_json(vty, target, &observation);
	else
		midr_trace_vty_text(vty, target, &observation);

	return CMD_SUCCESS;
}

DEFUN(show_midr_tier1_auto,
      show_midr_tier1_auto_cmd,
      "show midr tier1 <A.B.C.D|X:X::X:X> [json]",
      SHOW_STR
      "MIDR overlay routing\n"
      "Check whether an observed underlay path crosses Tier-1 ASNs\n"
      "Target IPv4 node\n"
      "Target IPv6 node\n"
      JSON_STR)
{
	const int idx_target = 3;
	const char *target = argv[idx_target]->arg;
	struct prefix target_prefix;
	struct midr_tier1_result result;
	struct midr_tier1_observation observation;
	bool uj = use_json(argc, argv);
	int ret;

	if (!midr_vty_target_parse(vty, target, &target_prefix))
		return CMD_WARNING;

	ret = midr_tier1_target_check(&target_prefix, midr_trace_observer_get(),
				      &midr_tier1_common_seed_202607,
				      &result, &observation);
	if (ret == -2) {
		if (uj)
			midr_tier1_vty_unavailable_json(vty, target);
		else
			midr_tier1_vty_unavailable_text(vty, target);
		return CMD_SUCCESS;
	}

	if (!ret) {
		if (uj)
			midr_tier1_vty_json(vty, target, &result,
					    &observation);
		else
			midr_tier1_vty_text(vty, target, &result,
					    &observation);
		return CMD_SUCCESS;
	}

	vty_out(vty, "%% MIDR Tier-1 check failed\n");
	return CMD_WARNING;
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
		midr_tier1_vty_json(vty, target, &result, &observation);
	else
		midr_tier1_vty_text(vty, target, &result, &observation);

	XFREE(MTYPE_TMP, observed_asns);
	return CMD_SUCCESS;
}

void midr_tier1_vty_init(void)
{
	install_element(CONFIG_NODE, &midr_ip2asn_file_cmd);
	install_element(CONFIG_NODE, &no_midr_ip2asn_file_cmd);
	install_element(VIEW_NODE, &show_midr_ip2asn_cmd);
	install_element(VIEW_NODE, &show_midr_ip2asn_lookup_cmd);
	install_element(VIEW_NODE, &show_midr_traceroute_cmd);
	install_element(VIEW_NODE, &show_midr_tier1_auto_cmd);
	install_element(VIEW_NODE, &show_midr_tier1_cmd);
}
