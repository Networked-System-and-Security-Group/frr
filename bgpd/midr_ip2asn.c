// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR local IP-to-ASN snapshot lookup.
 *
 * Supported snapshot rows:
 *
 *   <prefix>/<prefixlen> <asn> [ignored fields...]
 *   <address> <prefixlen> <asn> [ignored fields...]
 *
 * Lines may be whitespace or comma separated.  Blank lines and lines whose
 * first non-space character is '#' are ignored.  When a CAIDA-style MOAS ASN
 * field is present, the first leading decimal ASN is used for the MVP.
 */

#include <zebra.h>

#include <ctype.h>
#include <errno.h>

#include "memory.h"
#include "prefix.h"
#include "table.h"
#include "vty.h"

#include "bgpd/midr_ip2asn.h"

struct midr_ip2asn_db {
	struct route_table *ipv4;
	struct route_table *ipv6;
	char *source_path;
	bool loaded;
	unsigned long entries;
};

static struct midr_ip2asn_db midr_ip2asn_db;

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

static void midr_ip2asn_tables_ensure(void)
{
	if (!midr_ip2asn_db.ipv4)
		midr_ip2asn_db.ipv4 = route_table_init();
	if (!midr_ip2asn_db.ipv6)
		midr_ip2asn_db.ipv6 = route_table_init();
}

static void midr_ip2asn_table_clear(struct route_table **tablep)
{
	struct route_table *table = *tablep;
	struct route_node *rn;

	if (!table)
		return;

	for (rn = route_top(table); rn; rn = route_next(rn)) {
		void *info;

		if (!rn->info)
			continue;

		info = rn->info;
		route_node_set_info(rn, NULL);
		XFREE(MTYPE_TMP, info);
		route_unlock_node(rn);
	}

	route_table_finish(table);
	*tablep = NULL;
}

void midr_ip2asn_clear(void)
{
	midr_ip2asn_table_clear(&midr_ip2asn_db.ipv4);
	midr_ip2asn_table_clear(&midr_ip2asn_db.ipv6);
	XFREE(MTYPE_TMP, midr_ip2asn_db.source_path);
	midr_ip2asn_db.loaded = false;
	midr_ip2asn_db.entries = 0;
}

bool midr_ip2asn_is_loaded(void)
{
	return midr_ip2asn_db.loaded;
}

const char *midr_ip2asn_source_path(void)
{
	return midr_ip2asn_db.source_path;
}

unsigned long midr_ip2asn_entry_count(void)
{
	return midr_ip2asn_db.entries;
}

static bool midr_ip2asn_asn_parse(const char *token, as_t *asn)
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

static bool midr_ip2asn_prefix_parse(char **tokens, int ntokens,
				     struct prefix *prefix, as_t *asn)
{
	char prefixbuf[PREFIX_STRLEN];

	if (ntokens < 2)
		return false;

	if (strchr(tokens[0], '/')) {
		if (!str2prefix(tokens[0], prefix))
			return false;
		if (!midr_ip2asn_asn_parse(tokens[1], asn))
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
		if (!midr_ip2asn_asn_parse(tokens[2], asn))
			return false;
	}

	if ((prefix->family == AF_INET && prefix->prefixlen > IPV4_MAX_BITLEN)
	    || (prefix->family == AF_INET6
		&& prefix->prefixlen > IPV6_MAX_BITLEN))
		return false;

	if (prefix->family != AF_INET && prefix->family != AF_INET6)
		return false;

	apply_mask(prefix);
	return true;
}

static int midr_ip2asn_line_tokens(char *line, char **tokens, int maxtokens)
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

static int midr_ip2asn_table_add(struct route_table *table,
				 const struct prefix *prefix, as_t asn)
{
	struct route_node *rn;
	as_t *stored_asn;

	rn = route_node_get(table, prefix);
	if (!rn)
		return -1;

	if (rn->info) {
		stored_asn = rn->info;
		*stored_asn = asn;
		route_unlock_node(rn);
		return 0;
	}

	stored_asn = XCALLOC(MTYPE_TMP, sizeof(*stored_asn));
	*stored_asn = asn;
	route_node_set_info(rn, stored_asn);
	route_lock_node(rn);
	route_unlock_node(rn);
	return 1;
}

static int midr_ip2asn_db_add(const struct prefix *prefix, as_t asn)
{
	struct route_table *table;

	midr_ip2asn_tables_ensure();
	table = prefix->family == AF_INET ? midr_ip2asn_db.ipv4
					  : midr_ip2asn_db.ipv6;

	return midr_ip2asn_table_add(table, prefix, asn);
}

int midr_ip2asn_load_file(const char *path, char *errmsg, size_t errmsg_len)
{
	FILE *fp;
	char line[1024];
	unsigned long entries = 0;
	unsigned long lineno = 0;
	struct midr_ip2asn_db old_db;
	struct midr_ip2asn_db new_db = {};

	if (!path || !path[0]) {
		midr_ip2asn_set_error(errmsg, errmsg_len,
				      "missing IP-to-ASN snapshot path");
		return -1;
	}

	fp = fopen(path, "r");
	if (!fp) {
		midr_ip2asn_set_error(errmsg, errmsg_len,
				      "cannot open %s: %s", path,
				      safe_strerror(errno));
		return -1;
	}

	old_db = midr_ip2asn_db;
	midr_ip2asn_db = new_db;
	midr_ip2asn_tables_ensure();

	while (fgets(line, sizeof(line), fp)) {
		char *tokens[8];
		struct prefix prefix;
		as_t asn;
		int ntokens;
		int ret;

		lineno++;
		ntokens = midr_ip2asn_line_tokens(line, tokens,
						  array_size(tokens));
		if (!ntokens)
			continue;

		if (!midr_ip2asn_prefix_parse(tokens, ntokens, &prefix, &asn)) {
			midr_ip2asn_set_error(
				errmsg, errmsg_len,
				"invalid IP-to-ASN row at %s:%lu", path,
				lineno);
			fclose(fp);
			midr_ip2asn_clear();
			midr_ip2asn_db = old_db;
			return -1;
		}

		ret = midr_ip2asn_db_add(&prefix, asn);
		if (ret < 0) {
			midr_ip2asn_set_error(
				errmsg, errmsg_len,
				"failed to add IP-to-ASN row at %s:%lu", path,
				lineno);
			fclose(fp);
			midr_ip2asn_clear();
			midr_ip2asn_db = old_db;
			return -1;
		}
		if (ret > 0)
			entries++;
	}

	if (ferror(fp)) {
		midr_ip2asn_set_error(errmsg, errmsg_len,
				      "failed to read %s: %s", path,
				      safe_strerror(errno));
		fclose(fp);
		midr_ip2asn_clear();
		midr_ip2asn_db = old_db;
		return -1;
	}

	fclose(fp);

	midr_ip2asn_db.source_path = XSTRDUP(MTYPE_TMP, path);
	midr_ip2asn_db.loaded = true;
	midr_ip2asn_db.entries = entries;

	midr_ip2asn_table_clear(&old_db.ipv4);
	midr_ip2asn_table_clear(&old_db.ipv6);
	XFREE(MTYPE_TMP, old_db.source_path);

	return 0;
}

bool midr_ip2asn_lookup(const struct prefix *addr, as_t *asn,
			struct prefix *matched_prefix)
{
	struct route_table *table;
	struct route_node *rn;

	if (!addr || !asn || !midr_ip2asn_db.loaded)
		return false;

	if (addr->family == AF_INET)
		table = midr_ip2asn_db.ipv4;
	else if (addr->family == AF_INET6)
		table = midr_ip2asn_db.ipv6;
	else
		return false;

	if (!table)
		return false;

	rn = route_node_match(table, addr);
	if (!rn || !rn->info)
		return false;

	*asn = *(as_t *)rn->info;
	if (matched_prefix)
		*matched_prefix = rn->p;
	route_unlock_node(rn);
	return true;
}

int midr_ip2asn_config_write(struct vty *vty)
{
	if (!midr_ip2asn_db.loaded || !midr_ip2asn_db.source_path)
		return 0;

	vty_out(vty, "midr ip2asn file %s\n", midr_ip2asn_db.source_path);
	return 1;
}
