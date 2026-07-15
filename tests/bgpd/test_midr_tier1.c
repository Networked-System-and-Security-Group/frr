// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR Tier-1 AS_PATH helper unit tests.
 */

#include <zebra.h>

#include "privs.h"

#include "bgpd/bgp_aspath.h"
#include "bgpd/midr_ip2asn.h"
#include "bgpd/midr_tier1.h"
#include "bgpd/midr_trace_observer.h"

/* need these to link in libbgp */
struct zebra_privs_t bgpd_privs = {};
struct event_loop *master = NULL;

static int failed;

static struct aspath *make_path(const char *str)
{
	struct aspath *path;

	path = aspath_str2aspath(str, ASNOTATION_PLAIN);
	assert(path);

	return path;
}

static void expect_true(bool condition, const char *what)
{
	if (condition)
		return;

	printf("failed: %s\n", what);
	failed++;
}

struct manual_observer_arg {
	const as_t *asns;
	size_t count;
};

static int manual_observer(const struct prefix *target,
			   struct midr_tier1_observation *observation,
			   void *arg)
{
	struct manual_observer_arg *manual = arg;

	observation->target = *target;
	observation->source = "test-manual-observer";
	observation->observed_asn_count = manual->count;
	memcpy(observation->observed_asns, manual->asns,
	       sizeof(*manual->asns) * manual->count);

	return 0;
}

static void test_ordered_tier1_hit(void)
{
	struct midr_tier1_result result;
	struct aspath *path;
	int ret;
	int failures_before = failed;

	printf("ordered tier1 hit\n");

	path = make_path("6447 34177 3356 3701 3701 3582 3582");
	ret = midr_tier1_aspath_check(path, &midr_tier1_common_seed_202607,
				      &result);

	expect_true(ret == 0, "ordered check returned success");
	expect_true(result.tier1_observed, "ordered path observed tier1");
	expect_true(result.tier1_ordered_observed,
		    "ordered path observed ordered tier1 evidence");
	expect_true(!result.tier1_unordered_observed,
		    "ordered path has no unordered tier1 evidence");
	expect_true(result.ordered_hit_count == 1,
		    "ordered path has one tier1 hit");
	expect_true(result.ordered_hits[0] == 3356,
		    "ordered path hit AS3356");
	expect_true(result.normalized_hops == 5,
		    "ordered path compresses adjacent prepends");
	expect_true(result.flags == 0, "ordered path has clean semantics");

	aspath_free(path);

	if (failed == failures_before)
		printf("OK\n");
}

static void test_private_asns_are_ignored(void)
{
	struct midr_tier1_result result;
	struct aspath *path;
	int ret;
	int failures_before = failed;

	printf("private asns ignored\n");

	path = make_path("65000 64512 174 174 3582");
	ret = midr_tier1_aspath_check(path, &midr_tier1_common_seed_202607,
				      &result);

	expect_true(ret == 0, "private check returned success");
	expect_true(result.tier1_ordered_observed,
		    "private-filtered path observed ordered tier1 evidence");
	expect_true(result.ordered_hit_count == 1,
		    "private-filtered path has one tier1 hit");
	expect_true(result.ordered_hits[0] == 174,
		    "private-filtered path hit AS174");
	expect_true(result.normalized_hops == 2,
		    "private ASNs do not count as normalized hops");
	expect_true(result.ignored_private_asns == 2,
		    "private ASNs are counted as ignored");
	expect_true(!!(result.flags & MIDR_TIER1_FLAG_PRIVATE_ASNS_IGNORED),
		    "private flag is set");
	expect_true(!!(result.flags & MIDR_TIER1_FLAG_SEMANTICS_DEGRADED),
		    "private-filtered evidence is degraded");

	aspath_free(path);

	if (failed == failures_before)
		printf("OK\n");
}

static void test_unordered_set_hit_is_degraded(void)
{
	struct midr_tier1_result result;
	struct aspath *path;
	int ret;
	int failures_before = failed;

	printf("unordered set hit is degraded\n");

	path = make_path("(65001 65002) {3356,64512} 6447 3582");
	ret = midr_tier1_aspath_check(path, &midr_tier1_common_seed_202607,
				      &result);

	expect_true(ret == 0, "set check returned success");
	expect_true(result.tier1_observed, "AS_SET path observed tier1");
	expect_true(!result.tier1_ordered_observed,
		    "AS_SET path has no ordered tier1 evidence");
	expect_true(result.tier1_unordered_observed,
		    "AS_SET path has unordered tier1 evidence");
	expect_true(result.unordered_hit_count == 1,
		    "AS_SET path has one unordered tier1 hit");
	expect_true(result.unordered_hits[0] == 3356,
		    "AS_SET path hit AS3356");
	expect_true(result.normalized_hops == 2,
		    "AS_SET does not add ordered normalized hops");
	expect_true(result.ignored_confed_asns == 2,
		    "confed sequence ASNs are ignored");
	expect_true(result.ignored_private_asns == 1,
		    "private ASN inside AS_SET is ignored");
	expect_true(!!(result.flags & MIDR_TIER1_FLAG_AS_SET_PRESENT),
		    "AS_SET flag is set");
	expect_true(!!(result.flags & MIDR_TIER1_FLAG_CONFED_SEGMENTS_IGNORED),
		    "confed flag is set");
	expect_true(!!(result.flags & MIDR_TIER1_FLAG_SEMANTICS_DEGRADED),
		    "semantics degraded flag set is detectable");

	aspath_free(path);

	if (failed == failures_before)
		printf("OK\n");
}

static void test_empty_aspath(void)
{
	struct midr_tier1_result result;
	struct aspath *path;
	int ret;
	int failures_before = failed;

	printf("empty aspath\n");

	path = make_path("");
	ret = midr_tier1_aspath_check(path, &midr_tier1_common_seed_202607,
				      &result);

	expect_true(ret == 0, "empty path check returned success");
	expect_true(!result.tier1_observed, "empty path has no tier1 hit");
	expect_true(result.normalized_hops == 0,
		    "empty path has zero normalized hops");
	expect_true(!!(result.flags & MIDR_TIER1_FLAG_EMPTY_PATH),
		    "empty path flag is set");

	aspath_free(path);

	if (failed == failures_before)
		printf("OK\n");
}

static void test_observed_path_from_traceroute(void)
{
	struct midr_tier1_result result;
	struct midr_tier1_observation observation;
	struct prefix target;
	const as_t observed_asns[] = {
		64512, 15169, 15169, BGP_AS_ZERO, 1299, 3582, 3582,
	};
	struct manual_observer_arg manual = {
		.asns = observed_asns,
		.count = array_size(observed_asns),
	};
	struct midr_tier1_observer observer = {
		.name = "test-manual-observer",
		.observe = manual_observer,
		.arg = &manual,
	};
	int ret;
	int failures_before = failed;

	printf("observed path from traceroute\n");

	ret = str2prefix("1.1.1.1", &target);
	expect_true(ret > 0, "target IP parsed");

	ret = midr_tier1_target_check(&target, &observer,
				      &midr_tier1_common_seed_202607,
				      &result, &observation);

	expect_true(ret == 0, "observed path check returned success");
	expect_true(observation.observed_asn_count == array_size(observed_asns),
		    "observation keeps observed path length");
	expect_true(result.tier1_observed, "observed path found tier1");
	expect_true(result.tier1_ordered_observed,
		    "observed path has ordered tier1 evidence");
	expect_true(!result.tier1_unordered_observed,
		    "observed path has no unordered evidence");
	expect_true(result.ordered_hit_count == 1,
		    "observed path has one tier1 hit");
	expect_true(result.ordered_hits[0] == 1299,
		    "observed path hit AS1299");
	expect_true(result.normalized_hops == 3,
		    "observed path compresses repeated ASNs");
	expect_true(result.ignored_private_asns == 1,
		    "observed private ASN is ignored");
	expect_true(result.ignored_unmapped_asns == 1,
		    "observed unmapped hop is ignored");
	expect_true(!!(result.flags & MIDR_TIER1_FLAG_UNMAPPED_ASNS_IGNORED),
		    "observed unmapped flag is set");
	expect_true(!(result.flags & MIDR_TIER1_FLAG_AS_ZERO_IGNORED),
		    "observed path treats zero as unmapped instead of BGP AS0");

	if (failed == failures_before)
		printf("OK\n");
}

static void test_ip2asn_snapshot_lookup(void)
{
	char path[] = "/tmp/midr-ip2asn-test-XXXXXX";
	char errmsg[256] = {};
	struct prefix addr;
	struct prefix matched;
	as_t asn = 0;
	FILE *fp;
	int fd;
	int ret;
	int failures_before = failed;

	printf("ip2asn snapshot lookup\n");

	fd = mkstemp(path);
	expect_true(fd >= 0, "temporary IP-to-ASN snapshot created");
	if (fd < 0)
		return;

	fp = fdopen(fd, "w");
	expect_true(fp != NULL, "temporary IP-to-ASN snapshot opened");
	if (!fp) {
		close(fd);
		unlink(path);
		return;
	}

	fprintf(fp, "# prefix/asn form\n");
	fprintf(fp, "1.1.1.0/24 13335\n");
	fprintf(fp, "1.1.1.0 25 6453\n");
	fprintf(fp, "2001:4860:: 32 15169\n");
	fclose(fp);

	ret = midr_ip2asn_load_file(path, errmsg, sizeof(errmsg));
	unlink(path);
	expect_true(ret == 0, errmsg[0] ? errmsg : "snapshot loaded");
	expect_true(midr_ip2asn_is_loaded(), "snapshot status is loaded");
	expect_true(midr_ip2asn_entry_count() == 3,
		    "snapshot entry count is tracked");

	ret = str2prefix("1.1.1.10", &addr);
	expect_true(ret > 0, "IPv4 lookup target parsed");
	expect_true(midr_ip2asn_lookup(&addr, &asn, &matched),
		    "IPv4 address is mapped");
	expect_true(asn == 6453, "IPv4 lookup prefers longest prefix");
	expect_true(matched.prefixlen == 25, "IPv4 matched prefix is /25");

	ret = str2prefix("1.1.1.200", &addr);
	expect_true(ret > 0, "IPv4 less-specific lookup target parsed");
	expect_true(midr_ip2asn_lookup(&addr, &asn, &matched),
		    "IPv4 less-specific address is mapped");
	expect_true(asn == 13335, "IPv4 less-specific prefix is used");
	expect_true(matched.prefixlen == 24, "IPv4 matched prefix is /24");

	ret = str2prefix("2001:4860:4860::8888", &addr);
	expect_true(ret > 0, "IPv6 lookup target parsed");
	expect_true(midr_ip2asn_lookup(&addr, &asn, &matched),
		    "IPv6 address is mapped");
	expect_true(asn == 15169, "IPv6 prefix maps to AS15169");
	expect_true(matched.prefixlen == 32, "IPv6 matched prefix is /32");

	midr_ip2asn_clear();

	if (failed == failures_before)
		printf("OK\n");
}

static void test_trace_observer_requires_ip2asn(void)
{
	struct midr_tier1_observation observation;
	struct prefix target;
	int ret;
	int failures_before = failed;

	printf("trace observer requires ip2asn\n");

	midr_ip2asn_clear();
	ret = str2prefix("1.1.1.1", &target);
	expect_true(ret > 0, "trace target IP parsed");
	ret = midr_trace_observe_path(&target, &observation);
	expect_true(ret == -2,
		    "direct trace observer reports missing IP-to-ASN data");

	if (failed == failures_before)
		printf("OK\n");
}

static void test_invalid_arguments(void)
{
	struct midr_tier1_result result;
	struct aspath *path;
	struct prefix target;
	int ret;
	int failures_before = failed;

	printf("invalid arguments\n");

	path = make_path("3356 3582");
	expect_true(midr_tier1_aspath_check(path, NULL, &result) == -1,
		    "NULL list is rejected");
	expect_true(midr_tier1_aspath_check(path, &midr_tier1_common_seed_202607,
					    NULL) == -1,
		    "NULL result is rejected");
	ret = midr_tier1_observed_path_check(NULL, 1,
					     &midr_tier1_common_seed_202607,
					     &result);
	expect_true(ret == -1,
		    "NULL observed path with non-zero length is rejected");
	ret = str2prefix("1.1.1.1", &target);
	expect_true(ret > 0, "invalid-args target IP parsed");
	expect_true(midr_tier1_target_check(&target, NULL,
					    &midr_tier1_common_seed_202607,
					    &result, NULL) == -2,
		    "NULL observer is reported unavailable");

	aspath_free(path);

	if (failed == failures_before)
		printf("OK\n");
}

int main(void)
{
	test_ordered_tier1_hit();
	test_private_asns_are_ignored();
	test_unordered_set_hit_is_degraded();
	test_empty_aspath();
	test_observed_path_from_traceroute();
	test_ip2asn_snapshot_lookup();
	test_trace_observer_requires_ip2asn();
	test_invalid_arguments();

	printf("failures: %d\n", failed);
	return failed;
}
