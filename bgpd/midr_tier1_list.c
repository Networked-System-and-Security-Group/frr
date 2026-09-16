// SPDX-License-Identifier: GPL-2.0-or-later
/* Atomic whole-file Tier-1 membership policy, independent of IP2ASN. */
#include <zebra.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "memory.h"
#include "vty.h"
#include "bgpd/bgp_memory.h"
#include "bgpd/midr_tier1_list.h"

DEFINE_HOOK(midr_policy_changed, (void), ());

DEFINE_MTYPE_STATIC(BGPD, MIDR_TIER1_LIST, "MIDR Tier-1 list");

struct midr_tier1_snapshot {
	struct midr_tier1_list view;
	as_t asns[MIDR_TIER1_LIST_MAX];
	char version[MIDR_TIER1_LIST_VERSION_MAX + 1];
	char *path;
	size_t duplicates;
};

static struct midr_tier1_snapshot *active;
static uint64_t generation;

static void midr_tier1_snapshot_free(struct midr_tier1_snapshot *snapshot)
{
	if (!snapshot)
		return;
	XFREE(MTYPE_MIDR_TIER1_LIST, snapshot->path);
	XFREE(MTYPE_MIDR_TIER1_LIST, snapshot);
}

static bool midr_tier1_space(unsigned char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n'
		|| c == '\v' || c == '\f';
}

static bool midr_tier1_version_valid(const char *s)
{
	size_t n = strlen(s);

	if (!n || n > MIDR_TIER1_LIST_VERSION_MAX)
		return false;
	for (; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z')
		      || (*s >= '0' && *s <= '9') || strchr("._:-", *s)))
			return false;
	return true;
}

static bool midr_tier1_parse_asn(const char *s, as_t *asn)
{
	uint32_t value = 0;

	if (!*s)
		return false;
	for (; *s; s++) {
		unsigned int digit;

		if (*s < '0' || *s > '9')
			return false;
		digit = *s - '0';
		if (value > (UINT32_MAX - digit) / 10U)
			return false;
		value = value * 10U + digit;
	}
	if (!value || BGP_AS_IS_PRIVATE(value))
		return false;
	*asn = value;
	return true;
}

static int midr_tier1_asn_compare(const void *a, const void *b)
{
	as_t x = *(const as_t *)a, y = *(const as_t *)b;

	return (x > y) - (x < y);
}

static const char *midr_tier1_parse_line(char *line, unsigned int *records,
				       struct midr_tier1_snapshot *candidate)
{
	char *start = line, *end;
	as_t asn;

	while (midr_tier1_space(*start))
		start++;
	end = start + strlen(start);
	while (end > start && midr_tier1_space(end[-1]))
		*--end = '\0';
	if (!*start || *start == '#')
		return NULL;
	if (*records < 2) {
		char *value = start;

		while (*value && !midr_tier1_space(*value))
			value++;
		if (!*value)
			return "header requires a keyword and value";
		*value++ = '\0';
		while (midr_tier1_space(*value))
			value++;
		if (*records == 0) {
			if (strcmp(start, "MIDR-TIER1-ASNS") || strcmp(value, "1"))
				return "expected MIDR-TIER1-ASNS 1 header";
		} else {
			if (strcmp(start, "LIST-ID") || !midr_tier1_version_valid(value))
				return "expected LIST-ID and a 1..64 character identifier";
			strlcpy(candidate->version, value, sizeof(candidate->version));
		}
	} else {
		if (candidate->view.count == MIDR_TIER1_LIST_MAX)
			return "too many ASN data records (maximum 4096)";
		if (!midr_tier1_parse_asn(start, &asn))
			return "expected one nonzero, nonprivate decimal ASN";
		candidate->asns[candidate->view.count++] = asn;
	}
	(*records)++;
	return NULL;
}

int midr_tier1_list_load_file(const char *path, bool validate_only,
			    char *errmsg, size_t errmsg_len)
{
	struct midr_tier1_snapshot *candidate = NULL, *old;
	struct stat st;
	FILE *file = NULL;
	char line[MIDR_TIER1_LIST_LINE_MAX + 1];
	size_t used = 0, bytes = 0, i, unique;
	unsigned int records = 0, lineno = 1;
	const char *error = NULL;
	int fd, c, saved_errno = 0;

	if (errmsg && errmsg_len)
		errmsg[0] = '\0';
	if (!path || !*path) {
		error = "missing path";
		goto fail;
	}
	/* NONBLOCK prevents a FIFO open from hanging before the fstat check. */
	fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		saved_errno = errno;
		error = "cannot open file";
		goto fail;
	}
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)
	    || st.st_size > MIDR_TIER1_LIST_FILE_MAX) {
		close(fd);
		error = "expected a regular file no larger than 128 KiB";
		goto fail;
	}
	file = fdopen(fd, "r");
	if (!file) {
		saved_errno = errno;
		close(fd);
		error = "cannot read file";
		goto fail;
	}
	candidate = XCALLOC(MTYPE_MIDR_TIER1_LIST, sizeof(*candidate));
	while ((c = fgetc(file)) != EOF) {
		if (++bytes > MIDR_TIER1_LIST_FILE_MAX || c == 0) {
			error = "file exceeds 128 KiB or contains NUL";
			goto fail;
		}
		if (c != '\n') {
			if (used == MIDR_TIER1_LIST_LINE_MAX) {
				error = "physical line exceeds 256 bytes";
				goto fail;
			}
			line[used++] = c;
			continue;
		}
		line[used] = '\0';
		error = midr_tier1_parse_line(line, &records, candidate);
		if (error)
			goto fail;
		used = 0;
		lineno++;
	}
	if (ferror(file)) {
		error = "file read failed";
		goto fail;
	}
	line[used] = '\0';
	error = midr_tier1_parse_line(line, &records, candidate);
	if (error)
		goto fail;
	if (records < 3 || !candidate->view.count) {
		error = "headers and at least one ASN are required";
		goto fail;
	}
	if (fclose(file) != 0) {
		file = NULL;
		error = "file close failed";
		goto fail;
	}
	file = NULL;
	qsort(candidate->asns, candidate->view.count, sizeof(as_t), midr_tier1_asn_compare);
	unique = 0;
	for (i = 0; i < candidate->view.count; i++) {
		if (unique && candidate->asns[i] == candidate->asns[unique - 1])
			continue;
		candidate->asns[unique++] = candidate->asns[i];
	}
	candidate->duplicates = candidate->view.count - unique;
	candidate->view.count = unique;
	if (validate_only) {
		midr_tier1_snapshot_free(candidate);
		return 0;
	}
	if (generation == UINT64_MAX) {
		error = "generation exhausted";
		goto fail;
	}
	candidate->path = XSTRDUP(MTYPE_MIDR_TIER1_LIST, path);
	candidate->view.asns = candidate->asns;
	candidate->view.version = candidate->version;
	old = active;
	active = candidate;
	generation++;
	midr_tier1_snapshot_free(old);
	hook_call(midr_policy_changed);
	return 0;
fail:
	if (file)
		fclose(file);
	midr_tier1_snapshot_free(candidate);
	if (errmsg && errmsg_len)
		snprintf(errmsg, errmsg_len, "line %u: %s%s%s", lineno, error,
			 saved_errno ? ": " : "", saved_errno ? safe_strerror(saved_errno) : "");
	return -1;
}

int midr_tier1_list_clear(char *errmsg, size_t errmsg_len)
{
	if (!active)
		return 0;
	if (generation == UINT64_MAX) {
		if (errmsg && errmsg_len)
			strlcpy(errmsg, "generation exhausted", errmsg_len);
		return -1;
	}
	midr_tier1_snapshot_free(active);
	active = NULL;
	generation++;
	hook_call(midr_policy_changed);
	return 0;
}

const struct midr_tier1_list *midr_tier1_list_active(void)
{
	return active ? &active->view : NULL;
}

uint64_t midr_tier1_list_generation(void)
{
	return generation;
}

void midr_tier1_list_get_status(struct midr_tier1_list_status *out)
{
	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	out->loaded = active != NULL;
	out->generation = generation;
	if (active) {
		out->count = active->view.count;
		out->duplicates = active->duplicates;
		out->source_path = active->path;
		out->version = active->version;
	}
}

int midr_tier1_list_config_write(struct vty *vty)
{
	if (!active)
		return 0;
	vty_out(vty, "midr tier1 file %s\n", active->path);
	return 1;
}

void midr_tier1_list_fini(void)
{
	midr_tier1_snapshot_free(active);
	active = NULL;
	generation = 0;
}

int midr_tier1_active_path_check(const as_t *asns, size_t count,
				struct midr_tier1_result *result)
{
	int rc;

	if (!result)
		return -1;
	midr_tier1_result_init(result);
	if (!active)
		return -2;
	rc = midr_tier1_observed_path_check(asns, count, &active->view, result);
	if (!rc) {
		result->has_tier1_list_generation = true;
		result->tier1_list_generation = generation;
	}
	return rc;
}
