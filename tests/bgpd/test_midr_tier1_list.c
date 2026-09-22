// SPDX-License-Identifier: GPL-2.0-or-later
#include <zebra.h>
#include <sys/stat.h>
#include "privs.h"
#include "bgpd/midr_tier1_list.h"

struct zebra_privs_t bgpd_privs = {};
struct event_loop *master;

static int load_bytes(const void *data, size_t len, bool validate)
{
	char path[] = "/tmp/midr-list-XXXXXX", error[256];
	int fd = mkstemp(path), rc;

	assert(fd >= 0);
	assert(write(fd, data, len) == (ssize_t)len);
	assert(close(fd) == 0);
	rc = midr_tier1_list_load_file(path, validate, error, sizeof(error));
	unlink(path);
	return rc;
}

static int load_text(const char *s, bool validate)
{
	return load_bytes(s, strlen(s), validate);
}

int main(void)
{
	struct midr_tier1_result result, saved;
	struct midr_tier1_list_status status;
	as_t path[] = {64512, 174, 0};
	char error[256], long_line[300];
	uint64_t generation;
	const char *invalid[] = {
		"", "174\n", "MIDR-TIER1-ASNS 2\nLIST-ID v\n174\n",
		"MIDR-TIER1-ASNS 1\nLIST-ID v\n",
		"MIDR-TIER1-ASNS 1\nLIST-ID v\n0\n",
		"MIDR-TIER1-ASNS 1\nLIST-ID v\n64512\n",
		"MIDR-TIER1-ASNS 1\nLIST-ID v\n4294967296\n",
		"MIDR-TIER1-ASNS 1\nLIST-ID v\n+174\n",
		"MIDR-TIER1-ASNS 1\nLIST-ID v\n174 1299\n",
		"MIDR-TIER1-ASNS 1\nLIST-ID bad/id\n174\n",
	};
	size_t i;

	assert(midr_tier1_active_path_check(path, 3, &result) == -2);
	assert(load_text("# policy\nMIDR-TIER1-ASNS\t1\nLIST-ID  old\n1299\n174\n174", false) == 0);
	midr_tier1_list_get_status(&status);
	assert(status.loaded && status.count == 2 && status.duplicates == 1);
	assert(midr_tier1_active_path_check(path, 3, &result) == 0);
	assert(result.tier1_observed && result.has_tier1_list_generation);
	saved = result;
	generation = status.generation;
	assert(load_text("MIDR-TIER1-ASNS 1\nLIST-ID new\n1299\n", true) == 0);
	assert(midr_tier1_list_generation() == generation);
	for (i = 0; i < array_size(invalid); i++) {
		assert(load_text(invalid[i], false) != 0);
		assert(midr_tier1_list_generation() == generation);
		assert(midr_tier1_list_contains(midr_tier1_list_active(), 174));
	}
	memset(long_line, 'x', sizeof(long_line));
	assert(load_bytes(long_line, sizeof(long_line), false) != 0);
	assert(load_bytes("MIDR-TIER1-ASNS 1\n\0", 19, false) != 0);
	assert(midr_tier1_list_load_file("/tmp", false, error, sizeof(error)) != 0);
	assert(load_text("MIDR-TIER1-ASNS 1\nLIST-ID new\n1299\n", false) == 0);
	assert(midr_tier1_active_path_check(path, 3, &result) == 0);
	assert(!result.tier1_observed && result.tier1_list_generation == generation + 1);
	assert(!strcmp(saved.tier1_list_version, "old"));
	assert(saved.tier1_observed); /* A copied result survives replacement. */
	assert(midr_tier1_list_clear(error, sizeof(error)) == 0);
	generation = midr_tier1_list_generation();
	assert(midr_tier1_list_clear(error, sizeof(error)) == 0);
	assert(midr_tier1_list_generation() == generation);
	assert(midr_tier1_active_path_check(path, 3, &result) == -2);
	assert(!strcmp(saved.tier1_list_version, "old"));
	midr_tier1_list_fini();
	puts("MIDR Tier-1 list tests passed");
	return 0;
}
