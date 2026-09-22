// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR transactional IP-to-ASN update unit tests.
 */

#include <zebra.h>

#include "privs.h"

#include "bgpd/midr_ip2asn.h"

/* Required by libbgp test linkage. */
struct zebra_privs_t bgpd_privs = {};
struct event_loop *master = NULL;

static int failed;

static const char base_snapshot[] =
	"203.0.113.0/24 64500\n"
	"198.51.100.0/24 64496\n"
	"192.0.2.0/24 64498\n";

static void expect_true(bool condition, const char *what)
{
	if (condition)
		return;

	printf("failed: %s\n", what);
	failed++;
}

static bool write_temp_file(char *path, size_t path_len,
			    const char *contents)
{
	FILE *fp;
	size_t contents_len;
	bool write_ok;
	int fd;

	snprintf(path, path_len, "/tmp/midr-ip2asn-update-test-XXXXXX");
	fd = mkstemp(path);
	if (fd < 0)
		return false;

	fp = fdopen(fd, "w");
	if (!fp) {
		close(fd);
		unlink(path);
		return false;
	}

	contents_len = strlen(contents);
	write_ok = fwrite(contents, 1, contents_len, fp) == contents_len;
	if (fclose(fp) != 0)
		write_ok = false;
	if (!write_ok) {
		unlink(path);
		return false;
	}
	return true;
}

static int load_snapshot_text(const char *contents, unsigned int flags,
			      struct midr_ip2asn_load_result *result,
			      char *errmsg, size_t errmsg_len)
{
	char path[128];
	int ret;

	if (!write_temp_file(path, sizeof(path), contents)) {
		snprintf(errmsg, errmsg_len, "failed to create snapshot fixture");
		return -1;
	}
	ret = midr_ip2asn_load_file_ex(path, flags, result, errmsg,
				       errmsg_len);
	unlink(path);
	return ret;
}

static int run_update_text(const char *contents, bool validate_only,
			   struct midr_ip2asn_update_result *result,
			   char *errmsg, size_t errmsg_len)
{
	char path[128];
	int ret;

	if (!write_temp_file(path, sizeof(path), contents)) {
		snprintf(errmsg, errmsg_len, "failed to create update fixture");
		return -1;
	}
	ret = midr_ip2asn_update_file(path, validate_only, result, errmsg,
				      errmsg_len);
	unlink(path);
	return ret;
}

static void reset_database(void)
{
	char errmsg[256] = {};

	expect_true(
		midr_ip2asn_clear_ex(MIDR_IP2ASN_REPLACE_DISCARD_DIRTY,
				    errmsg, sizeof(errmsg))
			== 0,
		errmsg[0] ? errmsg : "database reset");
}

static bool lookup_exact_text(const char *text, as_t *asn)
{
	struct prefix prefix;

	if (!str2prefix(text, &prefix))
		return false;
	return midr_ip2asn_lookup_exact(&prefix, asn);
}

static void test_add_replace_delete_success(void)
{
	struct midr_ip2asn_update_result result = {};
	struct midr_ip2asn_status status;
	char update[2048];
	char errmsg[512] = {};
	uint64_t base_generation;
	as_t asn = 0;
	int ret;
	int failures_before = failed;

	printf("transactional ADD/REPLACE/DELETE\n");
	reset_database();
	ret = load_snapshot_text(base_snapshot, MIDR_IP2ASN_REPLACE_NONE,
				 NULL, errmsg, sizeof(errmsg));
	expect_true(ret == 0, errmsg[0] ? errmsg : "base snapshot loaded");
	if (ret != 0)
		return;

	base_generation = midr_ip2asn_generation();
	snprintf(update, sizeof(update),
		 "MIDR-IP2ASN-UPDATE 1\n"
		 "UPDATE-ID success-001\n"
		 "BASE-GENERATION %" PRIu64 "\n"
		 "ADD 203.0.114.0/24 64501\n"
		 "REPLACE 198.51.100.0/24 64496 64497\n"
		 "DELETE 192.0.2.0/24 64498\n",
		 base_generation);
	ret = run_update_text(update, false, &result, errmsg,
			      sizeof(errmsg));
	expect_true(ret == 0, errmsg[0] ? errmsg : "update applied");
	if (ret != 0)
		return;

	expect_true(result.base_generation == base_generation,
		    "result records base generation");
	expect_true(result.new_generation == base_generation + 1,
		    "successful transaction increments generation once");
	expect_true(result.added == 1 && result.replaced == 1
			    && result.deleted == 1,
		    "result records operation counts");
	expect_true(result.entries == 3, "entry count reflects whole batch");

	expect_true(lookup_exact_text("203.0.114.0/24", &asn)
			    && asn == 64501,
		    "ADD exact prefix is present");
	expect_true(lookup_exact_text("198.51.100.0/24", &asn)
			    && asn == 64497,
		    "REPLACE exact prefix has new ASN");
	expect_true(!lookup_exact_text("192.0.2.0/24", &asn),
		    "DELETE exact prefix is absent");

	midr_ip2asn_get_status(&status);
	expect_true(status.dirty, "successful update marks database dirty");
	expect_true(status.last_update_id
			    && !strcmp(status.last_update_id, "success-001"),
		    "status records update ID");
	expect_true(status.last_added == 1 && status.last_replaced == 1
			    && status.last_deleted == 1,
		    "status records operation counts");

	reset_database();
	if (failed == failures_before)
		printf("OK\n");
}

static void test_validate_only_and_rejections(void)
{
	struct midr_ip2asn_update_result result = {};
	char update[2048];
	char errmsg[512] = {};
	uint64_t generation;
	unsigned long entries;
	as_t asn = 0;
	int ret;
	int failures_before = failed;

	printf("validate-only and strict rejection paths\n");
	reset_database();
	ret = load_snapshot_text(base_snapshot, MIDR_IP2ASN_REPLACE_NONE,
				 NULL, errmsg, sizeof(errmsg));
	expect_true(ret == 0, errmsg[0] ? errmsg : "base snapshot loaded");
	if (ret != 0)
		return;

	generation = midr_ip2asn_generation();
	entries = midr_ip2asn_entry_count();
	snprintf(update, sizeof(update),
		 "MIDR-IP2ASN-UPDATE 1\n"
		 "UPDATE-ID validate-001\n"
		 "BASE-GENERATION %" PRIu64 "\n"
		 "ADD 203.0.115.0/24 64502\n",
		 generation);
	ret = run_update_text(update, true, &result, errmsg,
			      sizeof(errmsg));
	expect_true(ret == 0,
		    errmsg[0] ? errmsg : "validate-only succeeded");
	expect_true(result.validate_only,
		    "validate-only result is explicitly marked");
	expect_true(midr_ip2asn_generation() == generation,
		    "validate-only leaves generation unchanged");
	expect_true(midr_ip2asn_entry_count() == entries,
		    "validate-only leaves entries unchanged");
	expect_true(!midr_ip2asn_is_dirty(),
		    "validate-only does not mark database dirty");
	expect_true(!lookup_exact_text("203.0.115.0/24", &asn),
		    "validate-only does not attach ADD value");

	snprintf(update, sizeof(update),
		 "MIDR-IP2ASN-UPDATE 1\n"
		 "UPDATE-ID bad-base\n"
		 "BASE-GENERATION %" PRIu64 "\n"
		 "ADD 203.0.116.0/24 64503\n",
		 generation + 1);
	expect_true(run_update_text(update, false, NULL, errmsg,
				    sizeof(errmsg))
			    != 0,
		    "base generation mismatch is rejected");

	snprintf(update, sizeof(update),
		 "MIDR-IP2ASN-UPDATE 1\n"
		 "UPDATE-ID bad-old\n"
		 "BASE-GENERATION %" PRIu64 "\n"
		 "REPLACE 198.51.100.0/24 65000 64497\n",
		 generation);
	expect_true(run_update_text(update, false, NULL, errmsg,
				    sizeof(errmsg))
			    != 0,
		    "expected-old mismatch is rejected");

	snprintf(update, sizeof(update),
		 "MIDR-IP2ASN-UPDATE 1\n"
		 "UPDATE-ID duplicate\n"
		 "BASE-GENERATION %" PRIu64 "\n"
		 "ADD 203.0.117.0/24 64504\n"
		 "DELETE 203.0.117.0/24 64504\n",
		 generation);
	expect_true(run_update_text(update, false, NULL, errmsg,
				    sizeof(errmsg))
			    != 0,
		    "duplicate exact prefix is rejected");

	snprintf(update, sizeof(update),
		 "MIDR-IP2ASN-UPDATE 1\n"
		 "UPDATE-ID noncanonical\n"
		 "BASE-GENERATION %" PRIu64 "\n"
		 "ADD 203.0.118.1/24 64505\n",
		 generation);
	expect_true(run_update_text(update, false, NULL, errmsg,
				    sizeof(errmsg))
			    != 0,
		    "prefix with host bits is rejected");

	expect_true(midr_ip2asn_generation() == generation,
		    "all rejected files leave generation unchanged");
	expect_true(midr_ip2asn_entry_count() == entries,
		    "all rejected files leave entries unchanged");
	expect_true(lookup_exact_text("198.51.100.0/24", &asn)
			    && asn == 64496,
		    "rejected files leave existing values unchanged");
	expect_true(!midr_ip2asn_is_dirty(),
		    "rejected files leave dirty state unchanged");

	reset_database();
	if (failed == failures_before)
		printf("OK\n");
}

static void test_dirty_guards_and_explicit_discard(void)
{
	static const char replacement_snapshot[] =
		"10.0.0.0/8 64510\n";
	struct midr_ip2asn_status status;
	char update[1024];
	char replacement_path[128];
	char errmsg[512] = {};
	uint64_t generation;
	bool fixture_created;
	int ret;
	int failures_before = failed;

	printf("dirty load/clear guards and explicit discard\n");
	reset_database();
	ret = load_snapshot_text(base_snapshot, MIDR_IP2ASN_REPLACE_NONE,
				 NULL, errmsg, sizeof(errmsg));
	expect_true(ret == 0, errmsg[0] ? errmsg : "base snapshot loaded");
	if (ret != 0)
		return;

	generation = midr_ip2asn_generation();
	snprintf(update, sizeof(update),
		 "MIDR-IP2ASN-UPDATE 1\n"
		 "UPDATE-ID dirty-001\n"
		 "BASE-GENERATION %" PRIu64 "\n"
		 "ADD 203.0.119.0/24 64506\n",
		 generation);
	ret = run_update_text(update, false, NULL, errmsg, sizeof(errmsg));
	expect_true(ret == 0, errmsg[0] ? errmsg : "dirty update applied");
	if (ret != 0)
		return;

	generation = midr_ip2asn_generation();
	fixture_created = write_temp_file(replacement_path,
					 sizeof(replacement_path),
					 replacement_snapshot);
	expect_true(fixture_created,
		    "replacement snapshot fixture created");
	if (!fixture_created)
		return;

	ret = midr_ip2asn_load_file_ex(
		replacement_path, MIDR_IP2ASN_REPLACE_NONE, NULL, errmsg,
		sizeof(errmsg));
	expect_true(ret != 0, "dirty database blocks full load");
	expect_true(midr_ip2asn_generation() == generation,
		    "blocked full load preserves generation");
	expect_true(midr_ip2asn_clear_ex(MIDR_IP2ASN_REPLACE_NONE,
					 errmsg, sizeof(errmsg))
			    != 0,
		    "dirty database blocks clear");
	expect_true(midr_ip2asn_generation() == generation,
		    "blocked clear preserves generation");

	ret = midr_ip2asn_load_file_ex(
		replacement_path, MIDR_IP2ASN_REPLACE_DISCARD_DIRTY, NULL,
		errmsg, sizeof(errmsg));
	unlink(replacement_path);
	expect_true(ret == 0,
		    errmsg[0] ? errmsg : "explicit full-load discard");
	expect_true(midr_ip2asn_generation() == generation + 1,
		    "explicit full-load discard increments generation once");
	expect_true(!midr_ip2asn_is_dirty(),
		    "explicit full-load discard clears dirty state");
	midr_ip2asn_get_status(&status);
	expect_true(status.last_reset_valid
			    && status.last_reset_kind
				       == MIDR_IP2ASN_RESET_FULL_LOAD_DISCARD,
		    "full-load discard records reset audit");
	expect_true(status.last_reset_old_generation == generation,
		    "full-load audit records discarded generation");
	expect_true(status.last_reset_discarded_update_id
			    && !strcmp(
				    status.last_reset_discarded_update_id,
				    "dirty-001"),
		    "full-load audit records discarded update ID");

	generation = midr_ip2asn_generation();
	snprintf(update, sizeof(update),
		 "MIDR-IP2ASN-UPDATE 1\n"
		 "UPDATE-ID dirty-002\n"
		 "BASE-GENERATION %" PRIu64 "\n"
		 "ADD 10.1.0.0/16 64511\n",
		 generation);
	ret = run_update_text(update, false, NULL, errmsg, sizeof(errmsg));
	expect_true(ret == 0,
		    errmsg[0] ? errmsg : "second dirty update applied");
	generation = midr_ip2asn_generation();
	ret = midr_ip2asn_clear_ex(
		MIDR_IP2ASN_REPLACE_DISCARD_DIRTY, errmsg, sizeof(errmsg));
	expect_true(ret == 0,
		    errmsg[0] ? errmsg : "explicit clear discard");
	expect_true(!midr_ip2asn_is_loaded(),
		    "explicit clear discard unloads snapshot");
	expect_true(!midr_ip2asn_is_dirty(),
		    "explicit clear discard clears dirty state");
	expect_true(midr_ip2asn_generation() == generation + 1,
		    "explicit clear discard increments generation once");
	midr_ip2asn_get_status(&status);
	expect_true(status.last_reset_valid
			    && status.last_reset_kind
				       == MIDR_IP2ASN_RESET_CLEAR_DISCARD,
		    "clear discard records reset audit");
	expect_true(status.last_reset_discarded_update_id
			    && !strcmp(
				    status.last_reset_discarded_update_id,
				    "dirty-002"),
		    "clear audit records discarded update ID");

	if (failed == failures_before)
		printf("OK\n");
}

int main(void)
{
	test_add_replace_delete_success();
	test_validate_only_and_rejections();
	test_dirty_guards_and_explicit_discard();

	reset_database();
	printf("failures: %d\n", failed);
	return failed;
}
