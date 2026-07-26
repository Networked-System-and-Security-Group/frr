// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR local IP-to-ASN snapshot lookup and transactional incremental update.
 *
 * Supported snapshot rows:
 *
 *   <prefix>/<prefixlen> <asn> [ignored fields...]
 *   <address> <prefixlen> <asn> [ignored fields...]
 *
 * Snapshot rows may be whitespace or comma separated.  Blank lines and lines
 * whose first non-space character is '#' are ignored.  A CAIDA-style MOAS ASN
 * field is accepted by using its first leading decimal ASN.
 *
 * Incremental update files intentionally use a separate, strict format.  See
 * midr_ip2asn_update_parse() below.
 */

#include <zebra.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include "memory.h"
#include "prefix.h"
#include "table.h"
#include "vty.h"

#include "bgpd/bgp_memory.h"
#include "bgpd/midr_ip2asn.h"

#define MIDR_IP2ASN_UPDATE_MAX_OPERATIONS 10000U
#define MIDR_IP2ASN_UPDATE_MAX_FILE_SIZE (8U * 1024U * 1024U)
#define MIDR_IP2ASN_UPDATE_MAX_LINE_SIZE 1024U

DEFINE_MTYPE_STATIC(BGPD, MIDR_IP2ASN_VALUE, "MIDR IP-to-ASN value");
DEFINE_MTYPE_STATIC(BGPD, MIDR_IP2ASN_PATH, "MIDR IP-to-ASN path");
DEFINE_MTYPE_STATIC(BGPD, MIDR_IP2ASN_UPDATE,
		    "MIDR IP-to-ASN update staging");

struct midr_ip2asn_snapshot {
	struct route_table *ipv4;
	struct route_table *ipv6;
	char *source_path;
	bool loaded;
	unsigned long entries;
};

struct midr_ip2asn_reset_audit {
	bool valid;
	enum midr_ip2asn_reset_kind kind;
	uint64_t old_generation;
	char discarded_update_id[MIDR_IP2ASN_UPDATE_ID_MAX + 1];
	struct timeval time;
};

struct midr_ip2asn_manager {
	struct midr_ip2asn_snapshot active;
	bool dirty;
	uint64_t generation;
	char *last_update_path;
	char last_update_id[MIDR_IP2ASN_UPDATE_ID_MAX + 1];
	struct timeval last_update_time;
	uint64_t process_updates_applied_total;
	unsigned long last_added;
	unsigned long last_replaced;
	unsigned long last_deleted;
	struct midr_ip2asn_reset_audit last_reset;
	bool update_in_progress;
};

static struct midr_ip2asn_manager midr_ip2asn_mgr;

enum midr_ip2asn_update_op_type {
	MIDR_IP2ASN_UPDATE_ADD,
	MIDR_IP2ASN_UPDATE_REPLACE,
	MIDR_IP2ASN_UPDATE_DELETE,
};

struct midr_ip2asn_update_op {
	enum midr_ip2asn_update_op_type type;
	struct prefix prefix;
	as_t expected_old_asn;
	as_t new_asn;
	unsigned long line;

	/*
	 * new_value is staging-owned until a successful ADD consumes it.
	 * applied_value is a non-owning identity pointer while the DB owns it.
	 * detached_value is undo-owned after a successful DELETE.
	 */
	as_t *new_value;
	as_t *applied_value;
	as_t *detached_value;
	bool applied;
};

struct midr_ip2asn_update_plan {
	struct midr_ip2asn_update_op *ops;
	size_t count;
	size_t capacity;
	char update_id[MIDR_IP2ASN_UPDATE_ID_MAX + 1];
	uint64_t base_generation;
	unsigned long base_generation_line;
	unsigned long added;
	unsigned long replaced;
	unsigned long deleted;
	unsigned long final_entries;
	char *prepared_update_path;
	struct timeval prepared_update_time;
};

static void midr_ip2asn_set_error(char *errmsg, size_t errmsg_len,
				  const char *fmt, ...)
{
	va_list ap;

	if (!errmsg || !errmsg_len)
		return;

	va_start(ap, fmt);
	vsnprintf(errmsg, errmsg_len, fmt, ap);
	va_end(ap);
}

static void midr_ip2asn_set_update_error(char *errmsg, size_t errmsg_len,
					 const char *path,
					 unsigned long line,
					 const char *fmt, ...)
{
	char reason[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(reason, sizeof(reason), fmt, ap);
	va_end(ap);

	midr_ip2asn_set_error(errmsg, errmsg_len, "%s:%lu: %s; "
			     "no changes applied",
			     path && path[0] ? path : "<missing>", line, reason);
}

static struct route_table *
midr_ip2asn_snapshot_table(struct midr_ip2asn_snapshot *snapshot, int family)
{
	if (family == AF_INET)
		return snapshot->ipv4;
	if (family == AF_INET6)
		return snapshot->ipv6;
	return NULL;
}

static void
midr_ip2asn_snapshot_ensure_tables(struct midr_ip2asn_snapshot *snapshot)
{
	if (!snapshot->ipv4)
		snapshot->ipv4 = route_table_init();
	if (!snapshot->ipv6)
		snapshot->ipv6 = route_table_init();
}

static void midr_ip2asn_table_destroy(struct route_table **tablep)
{
	struct route_table *table = *tablep;
	struct route_node *rn;

	if (!table)
		return;

	for (rn = route_top(table); rn; rn = route_next(rn)) {
		as_t *value;

		if (!rn->info)
			continue;

		value = rn->info;
		route_node_set_info(rn, NULL);
		XFREE(MTYPE_MIDR_IP2ASN_VALUE, value);

		/*
		 * route_top()/route_next() own the traversal lock.  This
		 * explicit unlock releases the one persistent info lock.
		 */
		route_unlock_node(rn);
	}

	route_table_finish(table);
	*tablep = NULL;
}

static void
midr_ip2asn_snapshot_destroy(struct midr_ip2asn_snapshot *snapshot)
{
	if (!snapshot)
		return;

	midr_ip2asn_table_destroy(&snapshot->ipv4);
	midr_ip2asn_table_destroy(&snapshot->ipv6);
	XFREE(MTYPE_MIDR_IP2ASN_PATH, snapshot->source_path);
	memset(snapshot, 0, sizeof(*snapshot));
}

static void
midr_ip2asn_snapshot_swap(struct midr_ip2asn_snapshot *left,
			  struct midr_ip2asn_snapshot *right)
{
	struct midr_ip2asn_snapshot tmp = *left;

	*left = *right;
	*right = tmp;
}

static unsigned long
midr_ip2asn_snapshot_info_count(const struct midr_ip2asn_snapshot *snapshot)
{
	unsigned long count = 0;

	if (snapshot->ipv4)
		count += route_table_info_count(snapshot->ipv4);
	if (snapshot->ipv6)
		count += route_table_info_count(snapshot->ipv6);
	return count;
}

static void midr_ip2asn_assert_invariants(void)
{
	assert(midr_ip2asn_mgr.active.entries
	       == midr_ip2asn_snapshot_info_count(&midr_ip2asn_mgr.active));
	assert(!midr_ip2asn_mgr.dirty
	       || (midr_ip2asn_mgr.active.loaded
		   && midr_ip2asn_mgr.active.source_path));
	assert(!midr_ip2asn_mgr.last_update_path || midr_ip2asn_mgr.dirty);
	assert(!midr_ip2asn_mgr.last_update_id[0] || midr_ip2asn_mgr.dirty);
}

static void midr_ip2asn_reset_update_metadata(void)
{
	XFREE(MTYPE_MIDR_IP2ASN_PATH,
	      midr_ip2asn_mgr.last_update_path);
	midr_ip2asn_mgr.last_update_id[0] = '\0';
	memset(&midr_ip2asn_mgr.last_update_time, 0,
	       sizeof(midr_ip2asn_mgr.last_update_time));
	midr_ip2asn_mgr.last_added = 0;
	midr_ip2asn_mgr.last_replaced = 0;
	midr_ip2asn_mgr.last_deleted = 0;
	midr_ip2asn_mgr.dirty = false;
}

static void
midr_ip2asn_prepare_reset_audit(struct midr_ip2asn_reset_audit *audit,
				enum midr_ip2asn_reset_kind kind)
{
	memset(audit, 0, sizeof(*audit));
	audit->valid = true;
	audit->kind = kind;
	audit->old_generation = midr_ip2asn_mgr.generation;
	snprintf(audit->discarded_update_id,
		 sizeof(audit->discarded_update_id), "%s",
		 midr_ip2asn_mgr.last_update_id);
	gettimeofday(&audit->time, NULL);
}

bool midr_ip2asn_is_loaded(void)
{
	return midr_ip2asn_mgr.active.loaded;
}

const char *midr_ip2asn_source_path(void)
{
	return midr_ip2asn_mgr.active.source_path;
}

unsigned long midr_ip2asn_entry_count(void)
{
	return midr_ip2asn_mgr.active.entries;
}

uint64_t midr_ip2asn_generation(void)
{
	return midr_ip2asn_mgr.generation;
}

bool midr_ip2asn_is_dirty(void)
{
	return midr_ip2asn_mgr.dirty;
}

void midr_ip2asn_get_status(struct midr_ip2asn_status *status)
{
	if (!status)
		return;

	memset(status, 0, sizeof(*status));
	status->loaded = midr_ip2asn_mgr.active.loaded;
	status->entries = midr_ip2asn_mgr.active.entries;
	status->source_path = midr_ip2asn_mgr.active.source_path;
	status->dirty = midr_ip2asn_mgr.dirty;
	status->generation = midr_ip2asn_mgr.generation;
	status->last_update_path = midr_ip2asn_mgr.last_update_path;
	status->last_update_id = midr_ip2asn_mgr.last_update_id;
	status->last_update_time = midr_ip2asn_mgr.last_update_time;
	status->process_updates_applied_total =
		midr_ip2asn_mgr.process_updates_applied_total;
	status->last_added = midr_ip2asn_mgr.last_added;
	status->last_replaced = midr_ip2asn_mgr.last_replaced;
	status->last_deleted = midr_ip2asn_mgr.last_deleted;
	status->last_reset_valid = midr_ip2asn_mgr.last_reset.valid;
	status->last_reset_kind = midr_ip2asn_mgr.last_reset.kind;
	status->last_reset_old_generation =
		midr_ip2asn_mgr.last_reset.old_generation;
	status->last_reset_discarded_update_id =
		midr_ip2asn_mgr.last_reset.discarded_update_id;
	status->last_reset_time = midr_ip2asn_mgr.last_reset.time;
}

const char *
midr_ip2asn_reset_kind_name(enum midr_ip2asn_reset_kind kind)
{
	switch (kind) {
	case MIDR_IP2ASN_RESET_NONE:
		return "none";
	case MIDR_IP2ASN_RESET_FULL_LOAD_DISCARD:
		return "full-load-discard";
	case MIDR_IP2ASN_RESET_CLEAR_DISCARD:
		return "clear-discard";
	}

	return "unknown";
}

static bool midr_ip2asn_snapshot_asn_parse(const char *token, as_t *asn)
{
	unsigned long long value;
	char *endp = NULL;

	if (!token || !isdigit((unsigned char)token[0]))
		return false;

	errno = 0;
	value = strtoull(token, &endp, 10);
	if (errno || endp == token || value > UINT32_MAX)
		return false;

	*asn = (as_t)value;
	return true;
}

static bool midr_ip2asn_snapshot_prefix_parse(char **tokens, int ntokens,
					      struct prefix *prefix,
					      as_t *asn)
{
	char prefixbuf[PREFIX_STRLEN];

	if (ntokens < 2)
		return false;

	if (strchr(tokens[0], '/')) {
		if (!str2prefix(tokens[0], prefix))
			return false;
		if (!midr_ip2asn_snapshot_asn_parse(tokens[1], asn))
			return false;
	} else {
		unsigned long prefixlen;
		char *endp = NULL;

		if (ntokens < 3)
			return false;

		errno = 0;
		prefixlen = strtoul(tokens[1], &endp, 10);
		if (errno || !endp || *endp || prefixlen > 128)
			return false;

		snprintf(prefixbuf, sizeof(prefixbuf), "%s/%lu", tokens[0],
			 prefixlen);
		if (!str2prefix(prefixbuf, prefix))
			return false;
		if (!midr_ip2asn_snapshot_asn_parse(tokens[2], asn))
			return false;
	}

	if ((prefix->family == AF_INET
	     && prefix->prefixlen > IPV4_MAX_BITLEN)
	    || (prefix->family == AF_INET6
		&& prefix->prefixlen > IPV6_MAX_BITLEN))
		return false;

	if (prefix->family != AF_INET && prefix->family != AF_INET6)
		return false;

	apply_mask(prefix);
	return true;
}

static int midr_ip2asn_snapshot_line_tokens(char *line, char **tokens,
					    int maxtokens)
{
	int ntokens = 0;
	char *p = line;

	while (*p) {
		while (*p && (isspace((unsigned char)*p) || *p == ','))
			p++;
		if (!*p || *p == '#')
			break;

		if (ntokens >= maxtokens)
			break;

		tokens[ntokens++] = p;
		while (*p && !isspace((unsigned char)*p) && *p != ','
		       && *p != '#')
			p++;
		if (*p == '#') {
			*p = '\0';
			break;
		}
		if (*p)
			*p++ = '\0';
	}

	return ntokens;
}

static int
midr_ip2asn_snapshot_add(struct midr_ip2asn_snapshot *snapshot,
			 const struct prefix *prefix, as_t asn)
{
	struct route_table *table;
	struct route_node *rn;
	as_t *stored_asn;

	table = midr_ip2asn_snapshot_table(snapshot, prefix->family);
	if (!table)
		return -1;

	rn = route_node_get(table, prefix);
	if (!rn)
		return -1;

	if (rn->info) {
		stored_asn = rn->info;
		*stored_asn = asn;
		route_unlock_node(rn);
		return 0;
	}

	stored_asn = XCALLOC(MTYPE_MIDR_IP2ASN_VALUE,
			     sizeof(*stored_asn));
	*stored_asn = asn;
	route_node_set_info(rn, stored_asn);
	route_lock_node(rn);
	route_unlock_node(rn);
	snapshot->entries++;
	return 1;
}

static int
midr_ip2asn_snapshot_build_file(const char *path,
				struct midr_ip2asn_snapshot *candidate,
				char *errmsg, size_t errmsg_len)
{
	FILE *fp;
	char line[1024];
	unsigned long lineno = 0;
	int ret = -1;

	fp = fopen(path, "r");
	if (!fp) {
		midr_ip2asn_set_error(errmsg, errmsg_len,
				      "cannot open %s: %s", path,
				      safe_strerror(errno));
		return -1;
	}

	midr_ip2asn_snapshot_ensure_tables(candidate);

	while (fgets(line, sizeof(line), fp)) {
		char *tokens[8];
		struct prefix prefix;
		as_t asn;
		int ntokens;

		lineno++;
		ntokens = midr_ip2asn_snapshot_line_tokens(
			line, tokens, array_size(tokens));
		if (!ntokens)
			continue;

		if (!midr_ip2asn_snapshot_prefix_parse(tokens, ntokens,
						       &prefix, &asn)) {
			midr_ip2asn_set_error(
				errmsg, errmsg_len,
				"invalid IP-to-ASN row at %s:%lu", path,
				lineno);
			goto done;
		}

		if (midr_ip2asn_snapshot_add(candidate, &prefix, asn) < 0) {
			midr_ip2asn_set_error(
				errmsg, errmsg_len,
				"failed to add IP-to-ASN row at %s:%lu", path,
				lineno);
			goto done;
		}
	}

	if (ferror(fp)) {
		midr_ip2asn_set_error(errmsg, errmsg_len,
				      "failed to read %s: %s", path,
				      safe_strerror(errno));
		goto done;
	}

	if (fclose(fp) != 0) {
		fp = NULL;
		midr_ip2asn_set_error(errmsg, errmsg_len,
				      "failed to close %s: %s", path,
				      safe_strerror(errno));
		goto done;
	}
	fp = NULL;

	candidate->source_path = XSTRDUP(MTYPE_MIDR_IP2ASN_PATH, path);
	candidate->loaded = true;
	assert(candidate->entries
	       == midr_ip2asn_snapshot_info_count(candidate));
	ret = 0;

done:
	if (fp)
		fclose(fp);
	if (ret != 0)
		midr_ip2asn_snapshot_destroy(candidate);
	return ret;
}

int midr_ip2asn_load_file_ex(const char *path, unsigned int flags,
			     struct midr_ip2asn_load_result *result,
			     char *errmsg, size_t errmsg_len)
{
	struct midr_ip2asn_snapshot candidate = {};
	struct midr_ip2asn_reset_audit reset_audit = {};
	uint64_t old_generation;
	bool discard;

	if (errmsg && errmsg_len)
		errmsg[0] = '\0';
	if (result)
		memset(result, 0, sizeof(*result));

	if (flags & ~((unsigned int)MIDR_IP2ASN_REPLACE_DISCARD_DIRTY)) {
		midr_ip2asn_set_error(errmsg, errmsg_len,
				      "unsupported IP-to-ASN load flags");
		return -1;
	}
	if (!path || !path[0]) {
		midr_ip2asn_set_error(errmsg, errmsg_len,
				      "missing IP-to-ASN snapshot path");
		return -1;
	}
	if (midr_ip2asn_mgr.update_in_progress) {
		midr_ip2asn_set_error(errmsg, errmsg_len,
				      "IP-to-ASN update is in progress");
		return -1;
	}
	if (midr_ip2asn_mgr.dirty
	    && !(flags & MIDR_IP2ASN_REPLACE_DISCARD_DIRTY)) {
		midr_ip2asn_set_error(
			errmsg, errmsg_len,
			"runtime IP-to-ASN updates are not materialized; "
			"explicit discard authorization is required");
		return -1;
	}
	if (midr_ip2asn_mgr.generation == UINT64_MAX) {
		midr_ip2asn_set_error(errmsg, errmsg_len,
				      "IP-to-ASN generation is exhausted");
		return -1;
	}

	if (midr_ip2asn_snapshot_build_file(path, &candidate, errmsg,
					    errmsg_len)
	    != 0)
		return -1;

	discard = midr_ip2asn_mgr.dirty;
	if (discard)
		midr_ip2asn_prepare_reset_audit(
			&reset_audit, MIDR_IP2ASN_RESET_FULL_LOAD_DISCARD);

	old_generation = midr_ip2asn_mgr.generation;
	midr_ip2asn_snapshot_swap(&midr_ip2asn_mgr.active, &candidate);
	midr_ip2asn_mgr.generation++;
	midr_ip2asn_reset_update_metadata();
	if (discard)
		midr_ip2asn_mgr.last_reset = reset_audit;

	if (result) {
		result->old_generation = old_generation;
		result->new_generation = midr_ip2asn_mgr.generation;
		result->entries = midr_ip2asn_mgr.active.entries;
		result->discarded_runtime_updates = discard;
	}

	midr_ip2asn_snapshot_destroy(&candidate);
	midr_ip2asn_assert_invariants();
	return 0;
}

int midr_ip2asn_load_file(const char *path, char *errmsg, size_t errmsg_len)
{
	return midr_ip2asn_load_file_ex(path, MIDR_IP2ASN_REPLACE_NONE, NULL,
					errmsg, errmsg_len);
}

int midr_ip2asn_clear_ex(unsigned int flags, char *errmsg,
			 size_t errmsg_len)
{
	struct midr_ip2asn_snapshot old_snapshot = {};
	struct midr_ip2asn_reset_audit reset_audit = {};
	bool discard;

	if (errmsg && errmsg_len)
		errmsg[0] = '\0';

	if (flags & ~((unsigned int)MIDR_IP2ASN_REPLACE_DISCARD_DIRTY)) {
		midr_ip2asn_set_error(errmsg, errmsg_len,
				      "unsupported IP-to-ASN clear flags");
		return -1;
	}
	if (midr_ip2asn_mgr.update_in_progress) {
		midr_ip2asn_set_error(errmsg, errmsg_len,
				      "IP-to-ASN update is in progress");
		return -1;
	}
	if (!midr_ip2asn_mgr.active.loaded)
		return 0;
	if (midr_ip2asn_mgr.dirty
	    && !(flags & MIDR_IP2ASN_REPLACE_DISCARD_DIRTY)) {
		midr_ip2asn_set_error(
			errmsg, errmsg_len,
			"runtime IP-to-ASN updates are not materialized; "
			"explicit discard authorization is required");
		return -1;
	}
	if (midr_ip2asn_mgr.generation == UINT64_MAX) {
		midr_ip2asn_set_error(errmsg, errmsg_len,
				      "IP-to-ASN generation is exhausted");
		return -1;
	}

	discard = midr_ip2asn_mgr.dirty;
	if (discard)
		midr_ip2asn_prepare_reset_audit(
			&reset_audit, MIDR_IP2ASN_RESET_CLEAR_DISCARD);

	midr_ip2asn_snapshot_swap(&midr_ip2asn_mgr.active, &old_snapshot);
	midr_ip2asn_mgr.generation++;
	midr_ip2asn_reset_update_metadata();
	if (discard)
		midr_ip2asn_mgr.last_reset = reset_audit;

	midr_ip2asn_snapshot_destroy(&old_snapshot);
	midr_ip2asn_assert_invariants();
	return 0;
}

void midr_ip2asn_clear(void)
{
	/*
	 * Compatibility wrapper: preserve the old signature without granting an
	 * implicit destructive exception when runtime updates are dirty.
	 */
	(void)midr_ip2asn_clear_ex(MIDR_IP2ASN_REPLACE_NONE, NULL, 0);
}

bool midr_ip2asn_lookup(const struct prefix *addr, as_t *asn,
			struct prefix *matched_prefix)
{
	struct route_table *table;
	struct route_node *rn;

	if (!addr || !asn || !midr_ip2asn_mgr.active.loaded)
		return false;

	table = midr_ip2asn_snapshot_table(&midr_ip2asn_mgr.active,
					   addr->family);
	if (!table)
		return false;

	rn = route_node_match(table, addr);
	if (!rn)
		return false;
	if (!rn->info) {
		route_unlock_node(rn);
		return false;
	}

	*asn = *(as_t *)rn->info;
	if (matched_prefix)
		*matched_prefix = rn->p;
	route_unlock_node(rn);
	return true;
}

bool midr_ip2asn_lookup_exact(const struct prefix *prefix, as_t *asn)
{
	struct prefix masked;
	struct route_table *table;
	struct route_node *rn;

	if (!prefix || !asn || !midr_ip2asn_mgr.active.loaded)
		return false;
	if ((prefix->family == AF_INET
	     && prefix->prefixlen > IPV4_MAX_BITLEN)
	    || (prefix->family == AF_INET6
		&& prefix->prefixlen > IPV6_MAX_BITLEN)
	    || (prefix->family != AF_INET
		&& prefix->family != AF_INET6))
		return false;
	masked = *prefix;
	apply_mask(&masked);
	if (!prefix_same(prefix, &masked))
		return false;

	table = midr_ip2asn_snapshot_table(&midr_ip2asn_mgr.active,
					   prefix->family);
	if (!table)
		return false;

	rn = route_node_lookup(table, prefix);
	if (!rn)
		return false;
	if (!rn->info) {
		route_unlock_node(rn);
		return false;
	}

	*asn = *(as_t *)rn->info;
	route_unlock_node(rn);
	return true;
}

/*
 * Raw mutation helpers modify only route nodes and route_table::info_count.
 * On error they provide the strong guarantee: no table or ownership change.
 */
static int
midr_ip2asn_insert_exact_raw(const struct prefix *prefix,
			     as_t **owned_value_io)
{
	struct route_table *table;
	struct route_node *rn;

	if (!prefix || !owned_value_io || !*owned_value_io
	    || !midr_ip2asn_mgr.active.loaded)
		return -1;

	table = midr_ip2asn_snapshot_table(&midr_ip2asn_mgr.active,
					   prefix->family);
	if (!table)
		return -1;

	rn = route_node_get(table, prefix);
	if (!rn)
		return -1;
	if (rn->info) {
		route_unlock_node(rn);
		return -1;
	}

	route_node_set_info(rn, *owned_value_io);
	route_lock_node(rn);
	route_unlock_node(rn);
	*owned_value_io = NULL;
	return 0;
}

static int midr_ip2asn_replace_exact_raw(const struct prefix *prefix,
					 as_t expected_old_asn,
					 as_t new_asn)
{
	struct route_table *table;
	struct route_node *rn;
	as_t *value;

	if (!prefix || !midr_ip2asn_mgr.active.loaded)
		return -1;

	table = midr_ip2asn_snapshot_table(&midr_ip2asn_mgr.active,
					   prefix->family);
	if (!table)
		return -1;

	rn = route_node_lookup(table, prefix);
	if (!rn)
		return -1;
	value = rn->info;
	if (!value || *value != expected_old_asn) {
		route_unlock_node(rn);
		return -1;
	}

	*value = new_asn;
	route_unlock_node(rn);
	return 0;
}

static int
midr_ip2asn_delete_exact_raw(const struct prefix *prefix,
			     as_t expected_old_asn,
			     as_t **detached_value_out)
{
	struct route_table *table;
	struct route_node *rn;
	as_t *value;

	if (!prefix || !detached_value_out || *detached_value_out
	    || !midr_ip2asn_mgr.active.loaded)
		return -1;

	table = midr_ip2asn_snapshot_table(&midr_ip2asn_mgr.active,
					   prefix->family);
	if (!table)
		return -1;

	rn = route_node_lookup(table, prefix);
	if (!rn)
		return -1;
	value = rn->info;
	if (!value || *value != expected_old_asn) {
		route_unlock_node(rn);
		return -1;
	}

	route_node_set_info(rn, NULL);

	/* Release the lookup lock and the one persistent info lock. */
	route_unlock_node(rn);
	route_unlock_node(rn);
	rn = NULL;

	*detached_value_out = value;
	return 0;
}

static bool midr_ip2asn_ascii_space(unsigned char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n'
	       || c == '\v' || c == '\f';
}

static int midr_ip2asn_update_tokens(char *line, char **tokens,
				     size_t max_tokens)
{
	size_t count = 0;
	char *p = line;

	while (*p && midr_ip2asn_ascii_space((unsigned char)*p))
		p++;
	if (!*p || *p == '#')
		return 0;

	while (*p) {
		if (count == max_tokens)
			return -1;
		tokens[count++] = p;

		while (*p && !midr_ip2asn_ascii_space((unsigned char)*p))
			p++;
		if (!*p)
			break;
		*p++ = '\0';
		while (*p && midr_ip2asn_ascii_space((unsigned char)*p))
			p++;
	}

	return (int)count;
}

static bool midr_ip2asn_parse_u64_strict(const char *token, uint64_t *value)
{
	unsigned long long parsed;
	const unsigned char *p;
	char *endp = NULL;

	if (!token || !token[0])
		return false;
	for (p = (const unsigned char *)token; *p; p++)
		if (*p < '0' || *p > '9')
			return false;

	errno = 0;
	parsed = strtoull(token, &endp, 10);
	if (errno || !endp || *endp)
		return false;
	*value = (uint64_t)parsed;
	return true;
}

static bool midr_ip2asn_parse_asn_strict(const char *token, bool allow_zero,
					 as_t *asn)
{
	uint64_t value;

	if (!midr_ip2asn_parse_u64_strict(token, &value)
	    || value > UINT32_MAX || (!allow_zero && value == 0))
		return false;
	*asn = (as_t)value;
	return true;
}

static bool midr_ip2asn_update_id_valid(const char *id)
{
	const unsigned char *p;
	size_t len;

	if (!id)
		return false;
	len = strlen(id);
	if (len == 0 || len > MIDR_IP2ASN_UPDATE_ID_MAX)
		return false;

	for (p = (const unsigned char *)id; *p; p++)
		if (!((*p >= 'a' && *p <= 'z')
		      || (*p >= 'A' && *p <= 'Z')
		      || (*p >= '0' && *p <= '9') || *p == '.'
		      || *p == '_' || *p == ':' || *p == '-'))
			return false;
	return true;
}

static bool midr_ip2asn_update_prefix_parse(const char *token,
					    struct prefix *prefix)
{
	const char *slash;
	char address[INET6_ADDRSTRLEN];
	struct prefix masked;
	uint64_t prefixlen;
	size_t address_len;

	if (!token)
		return false;
	slash = strchr(token, '/');
	if (!slash || slash == token || !slash[1] || strchr(slash + 1, '/'))
		return false;

	address_len = (size_t)(slash - token);
	if (address_len >= sizeof(address))
		return false;
	memcpy(address, token, address_len);
	address[address_len] = '\0';

	if (!midr_ip2asn_parse_u64_strict(slash + 1, &prefixlen))
		return false;

	memset(prefix, 0, sizeof(*prefix));
	if (inet_pton(AF_INET, address, &prefix->u.prefix4) == 1) {
		if (prefixlen > IPV4_MAX_BITLEN)
			return false;
		prefix->family = AF_INET;
		prefix->prefixlen = (uint8_t)prefixlen;
	} else if (inet_pton(AF_INET6, address, &prefix->u.prefix6) == 1) {
		if (prefixlen > IPV6_MAX_BITLEN)
			return false;
		prefix->family = AF_INET6;
		prefix->prefixlen = (uint8_t)prefixlen;
	} else {
		return false;
	}

	masked = *prefix;
	apply_mask(&masked);
	if (!prefix_same(prefix, &masked))
		return false;
	*prefix = masked;
	return true;
}

static struct midr_ip2asn_update_op *
midr_ip2asn_update_plan_add(struct midr_ip2asn_update_plan *plan)
{
	struct midr_ip2asn_update_op *op;
	size_t old_capacity;
	size_t new_capacity;

	if (plan->count == plan->capacity) {
		old_capacity = plan->capacity;
		new_capacity = old_capacity ? old_capacity * 2 : 64;
		if (new_capacity > MIDR_IP2ASN_UPDATE_MAX_OPERATIONS)
			new_capacity = MIDR_IP2ASN_UPDATE_MAX_OPERATIONS;
		plan->ops = XREALLOC(MTYPE_MIDR_IP2ASN_UPDATE, plan->ops,
				    new_capacity * sizeof(*plan->ops));
		memset(plan->ops + old_capacity, 0,
		       (new_capacity - old_capacity) * sizeof(*plan->ops));
		plan->capacity = new_capacity;
	}

	op = &plan->ops[plan->count++];
	memset(op, 0, sizeof(*op));
	return op;
}

static void
midr_ip2asn_update_plan_cleanup(struct midr_ip2asn_update_plan *plan)
{
	size_t i;

	for (i = 0; i < plan->count; i++) {
		XFREE(MTYPE_MIDR_IP2ASN_VALUE,
		      plan->ops[i].new_value);
		XFREE(MTYPE_MIDR_IP2ASN_VALUE,
		      plan->ops[i].detached_value);
		plan->ops[i].applied_value = NULL;
	}
	XFREE(MTYPE_MIDR_IP2ASN_UPDATE, plan->ops);
	XFREE(MTYPE_MIDR_IP2ASN_PATH,
	      plan->prepared_update_path);
	memset(plan, 0, sizeof(*plan));
}

static int midr_ip2asn_update_op_ptr_cmp(const void *left,
					 const void *right)
{
	const struct midr_ip2asn_update_op *const *op_left = left;
	const struct midr_ip2asn_update_op *const *op_right = right;
	int ret;

	ret = prefix_cmp(&(*op_left)->prefix, &(*op_right)->prefix);
	if (ret)
		return ret;
	if ((*op_left)->line < (*op_right)->line)
		return -1;
	if ((*op_left)->line > (*op_right)->line)
		return 1;
	return 0;
}

static int
midr_ip2asn_update_check_duplicates(const char *path,
				    struct midr_ip2asn_update_plan *plan,
				    char *errmsg, size_t errmsg_len)
{
	struct midr_ip2asn_update_op **sorted;
	size_t i;

	sorted = XCALLOC(MTYPE_MIDR_IP2ASN_UPDATE,
			 plan->count * sizeof(*sorted));
	for (i = 0; i < plan->count; i++)
		sorted[i] = &plan->ops[i];
	qsort(sorted, plan->count, sizeof(*sorted),
	      midr_ip2asn_update_op_ptr_cmp);

	for (i = 1; i < plan->count; i++) {
		if (prefix_cmp(&sorted[i - 1]->prefix,
			       &sorted[i]->prefix)
		    != 0)
			continue;

		midr_ip2asn_set_update_error(
			errmsg, errmsg_len, path, sorted[i]->line,
			"duplicate exact prefix (first declared at line %lu)",
			sorted[i - 1]->line);
		XFREE(MTYPE_MIDR_IP2ASN_UPDATE, sorted);
		return -1;
	}

	XFREE(MTYPE_MIDR_IP2ASN_UPDATE, sorted);
	return 0;
}

enum midr_ip2asn_update_parse_stage {
	MIDR_IP2ASN_PARSE_MAGIC,
	MIDR_IP2ASN_PARSE_UPDATE_ID,
	MIDR_IP2ASN_PARSE_BASE_GENERATION,
	MIDR_IP2ASN_PARSE_OPERATIONS,
};

static int
midr_ip2asn_update_parse_operation(const char *path, unsigned long line,
				   char **tokens, int ntokens,
				   struct midr_ip2asn_update_plan *plan,
				   char *errmsg, size_t errmsg_len)
{
	struct midr_ip2asn_update_op *op;
	struct prefix prefix;
	as_t expected_old_asn = 0;
	as_t new_asn = 0;
	enum midr_ip2asn_update_op_type type;

	if (!strcmp(tokens[0], "ADD")) {
		if (ntokens != 3
		    || !midr_ip2asn_update_prefix_parse(tokens[1], &prefix)
		    || !midr_ip2asn_parse_asn_strict(tokens[2], false,
						      &new_asn)) {
			midr_ip2asn_set_update_error(
				errmsg, errmsg_len, path, line,
				"invalid ADD; expected ADD <canonical-prefix> "
				"<new-asn>");
			return -1;
		}
		type = MIDR_IP2ASN_UPDATE_ADD;
	} else if (!strcmp(tokens[0], "REPLACE")) {
		if (ntokens != 4
		    || !midr_ip2asn_update_prefix_parse(tokens[1], &prefix)
		    || !midr_ip2asn_parse_asn_strict(tokens[2], true,
						      &expected_old_asn)
		    || !midr_ip2asn_parse_asn_strict(tokens[3], false,
						      &new_asn)
		    || expected_old_asn == new_asn) {
			midr_ip2asn_set_update_error(
				errmsg, errmsg_len, path, line,
				"invalid REPLACE; expected REPLACE "
				"<canonical-prefix> <expected-old-asn> "
				"<different-new-asn>");
			return -1;
		}
		type = MIDR_IP2ASN_UPDATE_REPLACE;
	} else if (!strcmp(tokens[0], "DELETE")) {
		if (ntokens != 3
		    || !midr_ip2asn_update_prefix_parse(tokens[1], &prefix)
		    || !midr_ip2asn_parse_asn_strict(tokens[2], true,
						      &expected_old_asn)) {
			midr_ip2asn_set_update_error(
				errmsg, errmsg_len, path, line,
				"invalid DELETE; expected DELETE "
				"<canonical-prefix> <expected-old-asn>");
			return -1;
		}
		type = MIDR_IP2ASN_UPDATE_DELETE;
	} else {
		midr_ip2asn_set_update_error(errmsg, errmsg_len, path, line,
					     "unknown update keyword '%s'",
					     tokens[0]);
		return -1;
	}

	if (plan->count == MIDR_IP2ASN_UPDATE_MAX_OPERATIONS) {
		midr_ip2asn_set_update_error(
			errmsg, errmsg_len, path, line,
			"operation limit of %u exceeded",
			MIDR_IP2ASN_UPDATE_MAX_OPERATIONS);
		return -1;
	}

	op = midr_ip2asn_update_plan_add(plan);
	op->type = type;
	op->prefix = prefix;
	op->expected_old_asn = expected_old_asn;
	op->new_asn = new_asn;
	op->line = line;

	switch (type) {
	case MIDR_IP2ASN_UPDATE_ADD:
		plan->added++;
		break;
	case MIDR_IP2ASN_UPDATE_REPLACE:
		plan->replaced++;
		break;
	case MIDR_IP2ASN_UPDATE_DELETE:
		plan->deleted++;
		break;
	}

	return 0;
}

static int
midr_ip2asn_update_parse_line(
	const char *path, unsigned long lineno, char *linebuf,
	size_t line_length, enum midr_ip2asn_update_parse_stage *stage,
	struct midr_ip2asn_update_plan *plan, char *errmsg,
	size_t errmsg_len)
{
	char *tokens[5];
	int ntokens;

	while (line_length > 0
	       && (linebuf[line_length - 1] == '\n'
		   || linebuf[line_length - 1] == '\r'))
		linebuf[--line_length] = '\0';
	linebuf[line_length] = '\0';

	ntokens = midr_ip2asn_update_tokens(
		linebuf, tokens, array_size(tokens));
	if (ntokens == 0)
		return 0;
	if (ntokens < 0) {
		midr_ip2asn_set_update_error(errmsg, errmsg_len, path,
					     lineno, "too many tokens");
		return -1;
	}

	switch (*stage) {
	case MIDR_IP2ASN_PARSE_MAGIC:
		if (ntokens != 2
		    || strcmp(tokens[0], "MIDR-IP2ASN-UPDATE")
		    || strcmp(tokens[1], "1")) {
			midr_ip2asn_set_update_error(
				errmsg, errmsg_len, path, lineno,
				"expected 'MIDR-IP2ASN-UPDATE 1'");
			return -1;
		}
		*stage = MIDR_IP2ASN_PARSE_UPDATE_ID;
		break;
	case MIDR_IP2ASN_PARSE_UPDATE_ID:
		if (ntokens != 2 || strcmp(tokens[0], "UPDATE-ID")
		    || !midr_ip2asn_update_id_valid(tokens[1])) {
			midr_ip2asn_set_update_error(
				errmsg, errmsg_len, path, lineno,
				"expected a valid UPDATE-ID");
			return -1;
		}
		snprintf(plan->update_id, sizeof(plan->update_id), "%s",
			 tokens[1]);
		*stage = MIDR_IP2ASN_PARSE_BASE_GENERATION;
		break;
	case MIDR_IP2ASN_PARSE_BASE_GENERATION:
		if (ntokens != 2
		    || strcmp(tokens[0], "BASE-GENERATION")
		    || !midr_ip2asn_parse_u64_strict(
			    tokens[1], &plan->base_generation)) {
			midr_ip2asn_set_update_error(
				errmsg, errmsg_len, path, lineno,
				"expected a valid BASE-GENERATION");
			return -1;
		}
		plan->base_generation_line = lineno;
		*stage = MIDR_IP2ASN_PARSE_OPERATIONS;
		break;
	case MIDR_IP2ASN_PARSE_OPERATIONS:
		return midr_ip2asn_update_parse_operation(
			path, lineno, tokens, ntokens, plan, errmsg,
			errmsg_len);
	}

	return 0;
}

static int
midr_ip2asn_update_parse(const char *path,
			 struct midr_ip2asn_update_plan *plan,
			 char *errmsg, size_t errmsg_len)
{
	enum midr_ip2asn_update_parse_stage stage = MIDR_IP2ASN_PARSE_MAGIC;
	struct stat st;
	FILE *fp = NULL;
	char linebuf[MIDR_IP2ASN_UPDATE_MAX_LINE_SIZE + 1];
	unsigned char readbuf[16 * 1024];
	size_t line_length = 0;
	size_t total_bytes = 0;
	unsigned long lineno = 0;
	int fd = -1;
	int open_flags = O_RDONLY;
	int ret = -1;

#ifdef O_CLOEXEC
	open_flags |= O_CLOEXEC;
#endif
	fd = open(path, open_flags);
	if (fd < 0) {
		midr_ip2asn_set_update_error(errmsg, errmsg_len, path, 0,
					     "cannot open file: %s",
					     safe_strerror(errno));
		goto done;
	}
#ifndef O_CLOEXEC
	if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
		midr_ip2asn_set_update_error(
			errmsg, errmsg_len, path, 0,
			"cannot set close-on-exec: %s", safe_strerror(errno));
		goto done;
	}
#endif

	if (fstat(fd, &st) != 0) {
		midr_ip2asn_set_update_error(errmsg, errmsg_len, path, 0,
					     "cannot stat file: %s",
					     safe_strerror(errno));
		goto done;
	}
	if (!S_ISREG(st.st_mode)) {
		midr_ip2asn_set_update_error(
			errmsg, errmsg_len, path, 0,
			"update source is not a regular file");
		goto done;
	}
	if (st.st_size < 0
	    || (uintmax_t)st.st_size > MIDR_IP2ASN_UPDATE_MAX_FILE_SIZE) {
		midr_ip2asn_set_update_error(
			errmsg, errmsg_len, path, 0,
			"file exceeds the %u-byte limit",
			MIDR_IP2ASN_UPDATE_MAX_FILE_SIZE);
		goto done;
	}

	fp = fdopen(fd, "r");
	if (!fp) {
		midr_ip2asn_set_update_error(errmsg, errmsg_len, path, 0,
					     "cannot open file stream: %s",
					     safe_strerror(errno));
		goto done;
	}
	fd = -1;

	for (;;) {
		size_t chunk_length;
		size_t offset;

		chunk_length = fread(readbuf, 1, sizeof(readbuf), fp);
		if (chunk_length == 0)
			break;
		if (total_bytes
		    > MIDR_IP2ASN_UPDATE_MAX_FILE_SIZE - chunk_length) {
			midr_ip2asn_set_update_error(
				errmsg, errmsg_len, path, lineno + 1,
				"file grew beyond the %u-byte limit",
				MIDR_IP2ASN_UPDATE_MAX_FILE_SIZE);
			goto done;
		}
		total_bytes += chunk_length;

		for (offset = 0; offset < chunk_length; offset++) {
			unsigned char ch = readbuf[offset];

			if (line_length
			    == MIDR_IP2ASN_UPDATE_MAX_LINE_SIZE) {
				midr_ip2asn_set_update_error(
					errmsg, errmsg_len, path, lineno + 1,
					"line exceeds the %u-byte limit",
					MIDR_IP2ASN_UPDATE_MAX_LINE_SIZE);
				goto done;
			}
			if (ch == '\0') {
				midr_ip2asn_set_update_error(
					errmsg, errmsg_len, path, lineno + 1,
					"embedded NUL byte is not allowed");
				goto done;
			}

			linebuf[line_length++] = (char)ch;
			if (ch != '\n')
				continue;

			lineno++;
			if (midr_ip2asn_update_parse_line(
				    path, lineno, linebuf, line_length,
				    &stage, plan, errmsg, errmsg_len)
			    != 0)
				goto done;
			line_length = 0;
		}
	}

	if (ferror(fp)) {
		midr_ip2asn_set_update_error(errmsg, errmsg_len, path,
					     lineno,
					     "failed to read file: %s",
					     safe_strerror(errno));
		goto done;
	}
	if (line_length > 0) {
		lineno++;
		if (midr_ip2asn_update_parse_line(
			    path, lineno, linebuf, line_length, &stage,
			    plan, errmsg, errmsg_len)
		    != 0)
			goto done;
	}
	if (stage != MIDR_IP2ASN_PARSE_OPERATIONS) {
		midr_ip2asn_set_update_error(errmsg, errmsg_len, path,
					     lineno + 1,
					     "incomplete update header");
		goto done;
	}
	if (plan->count == 0) {
		midr_ip2asn_set_update_error(errmsg, errmsg_len, path,
					     lineno + 1,
					     "at least one operation is required");
		goto done;
	}
	if (midr_ip2asn_update_check_duplicates(path, plan, errmsg,
						 errmsg_len)
	    != 0)
		goto done;

	ret = 0;

done:
	if (fp) {
		if (fclose(fp) != 0 && ret == 0) {
			midr_ip2asn_set_update_error(
				errmsg, errmsg_len, path, lineno,
				"failed to close file: %s",
				safe_strerror(errno));
			ret = -1;
		}
	} else if (fd >= 0) {
		close(fd);
	}
	return ret;
}

static int
midr_ip2asn_update_preflight(const char *path,
			     struct midr_ip2asn_update_plan *plan,
			     char *errmsg, size_t errmsg_len)
{
	unsigned long final_entries = midr_ip2asn_mgr.active.entries;
	size_t i;

	if (plan->base_generation != midr_ip2asn_mgr.generation) {
		midr_ip2asn_set_update_error(
			errmsg, errmsg_len, path, plan->base_generation_line,
			"BASE-GENERATION %" PRIu64
			" does not match current generation %" PRIu64,
			plan->base_generation, midr_ip2asn_mgr.generation);
		return -1;
	}

	for (i = 0; i < plan->count; i++) {
		struct midr_ip2asn_update_op *op = &plan->ops[i];
		as_t current_asn = 0;
		bool exists;

		exists = midr_ip2asn_lookup_exact(&op->prefix, &current_asn);
		switch (op->type) {
		case MIDR_IP2ASN_UPDATE_ADD:
			if (exists) {
				midr_ip2asn_set_update_error(
					errmsg, errmsg_len, path, op->line,
					"ADD exact prefix already exists");
				return -1;
			}
			if (final_entries == ULONG_MAX) {
				midr_ip2asn_set_update_error(
					errmsg, errmsg_len, path, op->line,
					"entry count overflow");
				return -1;
			}
			final_entries++;
			break;
		case MIDR_IP2ASN_UPDATE_REPLACE:
		case MIDR_IP2ASN_UPDATE_DELETE:
			if (!exists) {
				midr_ip2asn_set_update_error(
					errmsg, errmsg_len, path, op->line,
					"exact prefix does not exist");
				return -1;
			}
			if (current_asn != op->expected_old_asn) {
				midr_ip2asn_set_update_error(
					errmsg, errmsg_len, path, op->line,
					"expected old ASN %u, found %u",
					(unsigned int)op->expected_old_asn,
					(unsigned int)current_asn);
				return -1;
			}
			if (op->type == MIDR_IP2ASN_UPDATE_DELETE) {
				if (final_entries == 0) {
					midr_ip2asn_set_update_error(
						errmsg, errmsg_len, path,
						op->line,
						"entry count underflow");
					return -1;
				}
				final_entries--;
			}
			break;
		}
	}

	plan->final_entries = final_entries;
	return 0;
}

static void
midr_ip2asn_update_preallocate(const char *path,
			       struct midr_ip2asn_update_plan *plan)
{
	size_t i;

	for (i = 0; i < plan->count; i++) {
		struct midr_ip2asn_update_op *op = &plan->ops[i];

		if (op->type != MIDR_IP2ASN_UPDATE_ADD)
			continue;
		op->new_value = XCALLOC(MTYPE_MIDR_IP2ASN_VALUE,
					sizeof(*op->new_value));
		*op->new_value = op->new_asn;
	}

	plan->prepared_update_path =
		XSTRDUP(MTYPE_MIDR_IP2ASN_PATH, path);
	gettimeofday(&plan->prepared_update_time, NULL);
}

static void
midr_ip2asn_update_rollback(struct midr_ip2asn_update_plan *plan,
			    size_t applied_count)
{
	while (applied_count > 0) {
		struct midr_ip2asn_update_op *op;
		as_t *detached = NULL;
		int ret;

		op = &plan->ops[--applied_count];
		if (!op->applied)
			continue;

		switch (op->type) {
		case MIDR_IP2ASN_UPDATE_ADD:
			ret = midr_ip2asn_delete_exact_raw(
				&op->prefix, op->new_asn, &detached);
			if (ret != 0 || detached != op->applied_value)
				abort();
			op->new_value = detached;
			op->applied_value = NULL;
			break;
		case MIDR_IP2ASN_UPDATE_REPLACE:
			ret = midr_ip2asn_replace_exact_raw(
				&op->prefix, op->new_asn,
				op->expected_old_asn);
			if (ret != 0)
				abort();
			break;
		case MIDR_IP2ASN_UPDATE_DELETE:
			ret = midr_ip2asn_insert_exact_raw(
				&op->prefix, &op->detached_value);
			if (ret != 0 || op->detached_value)
				abort();
			break;
		}
		op->applied = false;
	}

	midr_ip2asn_assert_invariants();
}

static int
midr_ip2asn_update_commit(const char *path,
			  struct midr_ip2asn_update_plan *plan,
			  struct midr_ip2asn_update_result *result,
			  char *errmsg, size_t errmsg_len)
{
	char *old_update_path;
	size_t i;

	for (i = 0; i < plan->count; i++) {
		struct midr_ip2asn_update_op *op = &plan->ops[i];
		int ret;

		switch (op->type) {
		case MIDR_IP2ASN_UPDATE_ADD:
			op->applied_value = op->new_value;
			ret = midr_ip2asn_insert_exact_raw(
				&op->prefix, &op->new_value);
			break;
		case MIDR_IP2ASN_UPDATE_REPLACE:
			ret = midr_ip2asn_replace_exact_raw(
				&op->prefix, op->expected_old_asn,
				op->new_asn);
			break;
		case MIDR_IP2ASN_UPDATE_DELETE:
			ret = midr_ip2asn_delete_exact_raw(
				&op->prefix, op->expected_old_asn,
				&op->detached_value);
			break;
		default:
			abort();
		}

		if (ret != 0) {
			midr_ip2asn_update_rollback(plan, i);
			midr_ip2asn_set_update_error(
				errmsg, errmsg_len, path, op->line,
				"transaction commit precondition changed");
			return -1;
		}
		op->applied = true;
	}

	/*
	 * Everything below is allocation-free and cannot report a recoverable
	 * error.  Publish manager metadata only after every table mutation.
	 */
	old_update_path = midr_ip2asn_mgr.last_update_path;
	midr_ip2asn_mgr.active.entries = plan->final_entries;
	midr_ip2asn_mgr.generation++;
	midr_ip2asn_mgr.last_update_path = plan->prepared_update_path;
	plan->prepared_update_path = NULL;
	snprintf(midr_ip2asn_mgr.last_update_id,
		 sizeof(midr_ip2asn_mgr.last_update_id), "%s",
		 plan->update_id);
	midr_ip2asn_mgr.last_update_time = plan->prepared_update_time;
	midr_ip2asn_mgr.last_added = plan->added;
	midr_ip2asn_mgr.last_replaced = plan->replaced;
	midr_ip2asn_mgr.last_deleted = plan->deleted;
	midr_ip2asn_mgr.dirty = true;
	midr_ip2asn_mgr.process_updates_applied_total++;
	XFREE(MTYPE_MIDR_IP2ASN_PATH, old_update_path);

	if (result) {
		snprintf(result->update_id, sizeof(result->update_id), "%s",
			 plan->update_id);
		result->base_generation = plan->base_generation;
		result->new_generation = midr_ip2asn_mgr.generation;
		result->added = plan->added;
		result->replaced = plan->replaced;
		result->deleted = plan->deleted;
		result->entries = plan->final_entries;
		result->validate_only = false;
	}

	midr_ip2asn_assert_invariants();
	return 0;
}

int midr_ip2asn_update_file(const char *path, bool validate_only,
			    struct midr_ip2asn_update_result *result,
			    char *errmsg, size_t errmsg_len)
{
	struct midr_ip2asn_update_plan plan = {};
	int ret = -1;

	if (errmsg && errmsg_len)
		errmsg[0] = '\0';
	if (result)
		memset(result, 0, sizeof(*result));

	if (!path || !path[0]) {
		midr_ip2asn_set_update_error(errmsg, errmsg_len, path, 0,
					     "missing update file path");
		return -1;
	}
	if (!midr_ip2asn_mgr.active.loaded) {
		midr_ip2asn_set_update_error(
			errmsg, errmsg_len, path, 0,
			"no IP-to-ASN snapshot is loaded");
		return -1;
	}
	if (midr_ip2asn_mgr.update_in_progress) {
		midr_ip2asn_set_update_error(errmsg, errmsg_len, path, 0,
					     "another update is in progress");
		return -1;
	}
	if (midr_ip2asn_mgr.generation == UINT64_MAX) {
		midr_ip2asn_set_update_error(errmsg, errmsg_len, path, 0,
					     "IP-to-ASN generation is exhausted");
		return -1;
	}
	if (midr_ip2asn_mgr.process_updates_applied_total == UINT64_MAX) {
		midr_ip2asn_set_update_error(
			errmsg, errmsg_len, path, 0,
			"IP-to-ASN process update counter is exhausted");
		return -1;
	}

	midr_ip2asn_mgr.update_in_progress = true;

	if (midr_ip2asn_update_parse(path, &plan, errmsg, errmsg_len) != 0)
		goto done;
	if (midr_ip2asn_update_preflight(path, &plan, errmsg, errmsg_len)
	    != 0)
		goto done;

	midr_ip2asn_update_preallocate(path, &plan);

	if (validate_only) {
		if (result) {
			snprintf(result->update_id, sizeof(result->update_id),
				 "%s", plan.update_id);
			result->base_generation = plan.base_generation;
			result->new_generation = midr_ip2asn_mgr.generation;
			result->added = plan.added;
			result->replaced = plan.replaced;
			result->deleted = plan.deleted;
			result->entries = plan.final_entries;
			result->validate_only = true;
		}
		ret = 0;
		goto done;
	}

	ret = midr_ip2asn_update_commit(path, &plan, result, errmsg,
					errmsg_len);

done:
	midr_ip2asn_update_plan_cleanup(&plan);
	midr_ip2asn_mgr.update_in_progress = false;
	midr_ip2asn_assert_invariants();
	return ret;
}

int midr_ip2asn_config_write(struct vty *vty)
{
	if (!midr_ip2asn_mgr.active.loaded
	    || !midr_ip2asn_mgr.active.source_path)
		return 0;

	vty_out(vty, "midr ip2asn file %s\n",
		midr_ip2asn_mgr.active.source_path);
	return 1;
}
