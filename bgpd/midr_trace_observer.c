// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR traceroute parser and query-time IP2ASN mapper.
 */

#include <zebra.h>

#include <ctype.h>
#include <errno.h>

#include "prefix.h"

#include "bgpd/midr_ip2asn.h"
#include "bgpd/midr_trace_observer.h"

#define MIDR_TRACE_TOKEN_MAX 128U

static void midr_trace_token_clean(char *token)
{
	size_t len;

	while (*token && (unsigned char)*token <= ' ')
		memmove(token, token + 1, strlen(token));
	while (*token == '(' || *token == '[')
		memmove(token, token + 1, strlen(token));

	len = strlen(token);
	while (len && (token[len - 1] == ')' || token[len - 1] == ']'
		       || token[len - 1] == ',' || token[len - 1] == ';')) {
		token[--len] = '\0';
	}
}

static bool midr_trace_token_to_prefix(const char *token,
				       struct prefix *prefix)
{
	struct in_addr addr4;
	struct in6_addr addr6;

	if (inet_pton(AF_INET, token, &addr4) == 1) {
		memset(prefix, 0, sizeof(*prefix));
		prefix->family = AF_INET;
		prefix->prefixlen = IPV4_MAX_BITLEN;
		prefix->u.prefix4 = addr4;
		return true;
	}
	if (inet_pton(AF_INET6, token, &addr6) == 1) {
		memset(prefix, 0, sizeof(*prefix));
		prefix->family = AF_INET6;
		prefix->prefixlen = IPV6_MAX_BITLEN;
		prefix->u.prefix6 = addr6;
		return true;
	}
	return false;
}

static bool midr_trace_line_is_hop(const char *line)
{
	char *endp;

	while (*line && isspace((unsigned char)*line))
		line++;
	if (!isdigit((unsigned char)*line))
		return false;

	errno = 0;
	(void)strtoul(line, &endp, 10);
	return errno == 0 && endp > line
	       && (!*endp || isspace((unsigned char)*endp));
}

static bool midr_trace_line_first_hop(const char *line, struct prefix *hop)
{
	char copy[MIDR_TRACE_LINE_MAX + 1];
	char *saveptr = NULL;
	char *token;

	strlcpy(copy, line, sizeof(copy));
	for (token = strtok_r(copy, " \t\r\n,", &saveptr); token;
	     token = strtok_r(NULL, " \t\r\n,", &saveptr)) {
		char clean[MIDR_TRACE_TOKEN_MAX];

		strlcpy(clean, token, sizeof(clean));
		midr_trace_token_clean(clean);
		if (!clean[0] || !strcmp(clean, "*"))
			continue;
		if (midr_trace_token_to_prefix(clean, hop))
			return true;
	}
	return false;
}

static enum midr_trace_parse_rc
midr_trace_parser_line(struct midr_trace_parser *parser)
{
	struct midr_trace_raw_hop *raw_hop;
	size_t len = parser->line_len;

	while (len && parser->line[len - 1] == '\r')
		len--;
	parser->line[len] = '\0';

	if (!midr_trace_line_is_hop(parser->line))
		return MIDR_TRACE_PARSE_OK;
	if (parser->path.hop_count >= array_size(parser->path.hops)) {
		parser->path.output_truncated = true;
		return MIDR_TRACE_PARSE_OUTPUT_LIMIT;
	}

	raw_hop = &parser->path.hops[parser->path.hop_count++];
	memset(raw_hop, 0, sizeof(*raw_hop));
	raw_hop->visible =
		midr_trace_line_first_hop(parser->line, &raw_hop->address);
	return MIDR_TRACE_PARSE_OK;
}

void midr_trace_parser_init(struct midr_trace_parser *parser)
{
	if (parser)
		memset(parser, 0, sizeof(*parser));
}

enum midr_trace_parse_rc
midr_trace_parser_feed(struct midr_trace_parser *parser, const void *data,
		       size_t data_len)
{
	const unsigned char *bytes = data;
	size_t i;

	if (!parser || (!data && data_len) || parser->finished)
		return MIDR_TRACE_PARSE_INVALID;
	if (parser->terminal_rc != MIDR_TRACE_PARSE_OK)
		return parser->terminal_rc;

	for (i = 0; i < data_len; i++) {
		enum midr_trace_parse_rc rc;

		if (bytes[i] == '\n') {
			rc = midr_trace_parser_line(parser);
			parser->line_len = 0;
			parser->line[0] = '\0';
			if (rc != MIDR_TRACE_PARSE_OK) {
				parser->terminal_rc = rc;
				return rc;
			}
			continue;
		}
		if (parser->line_len >= MIDR_TRACE_LINE_MAX) {
			parser->path.output_truncated = true;
			parser->terminal_rc =
				MIDR_TRACE_PARSE_OUTPUT_LIMIT;
			return MIDR_TRACE_PARSE_OUTPUT_LIMIT;
		}
		parser->line[parser->line_len++] = (char)bytes[i];
	}
	return MIDR_TRACE_PARSE_OK;
}

enum midr_trace_parse_rc
midr_trace_parser_finish(struct midr_trace_parser *parser,
			 struct midr_trace_raw_path *path)
{
	enum midr_trace_parse_rc rc = MIDR_TRACE_PARSE_OK;

	if (!parser || !path)
		return MIDR_TRACE_PARSE_INVALID;
	rc = parser->terminal_rc;
	if (!parser->finished) {
		if (rc == MIDR_TRACE_PARSE_OK && parser->line_len)
			rc = midr_trace_parser_line(parser);
	}
	parser->line_len = 0;
	parser->finished = true;

	if (rc == MIDR_TRACE_PARSE_OK && parser->path.hop_count == 0)
		rc = MIDR_TRACE_PARSE_INVALID;
	parser->terminal_rc = rc;
	*path = parser->path;
	return rc;
}

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
	if (result->status != MIDR_TRACE_OK) {
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
