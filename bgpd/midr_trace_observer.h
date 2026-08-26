// SPDX-License-Identifier: GPL-2.0-or-later
/* MIDR traceroute parser and IP2ASN mapping helpers. */

#ifndef _FRR_MIDR_TRACE_OBSERVER_H
#define _FRR_MIDR_TRACE_OBSERVER_H

#include <stddef.h>

#include "bgpd/midr_trace_scheduler.h"

#define MIDR_TRACE_LINE_MAX 1024U

enum midr_trace_parse_rc {
	MIDR_TRACE_PARSE_OK = 0,
	MIDR_TRACE_PARSE_OUTPUT_LIMIT,
	MIDR_TRACE_PARSE_INVALID,
};

struct midr_trace_parser {
	struct midr_trace_raw_path path;
	char line[MIDR_TRACE_LINE_MAX + 1];
	size_t line_len;
	enum midr_trace_parse_rc terminal_rc;
	bool finished;
};

extern void midr_trace_parser_init(struct midr_trace_parser *parser);
extern enum midr_trace_parse_rc
midr_trace_parser_feed(struct midr_trace_parser *parser, const void *data,
		       size_t data_len);
extern enum midr_trace_parse_rc
midr_trace_parser_finish(struct midr_trace_parser *parser,
			 struct midr_trace_raw_path *path);

/*
 * Construct a query-time view using the currently active IP2ASN generation.
 * This function is main-event-thread only and never yields.
 */
extern void midr_trace_map_ip2asn(const struct midr_trace_job_result *result,
				  struct midr_trace_query_view *view);

/*
 * Deprecated synchronous-observer compatibility.  It is now cache-only:
 * a miss schedules/joins an asynchronous job and returns -1 without waiting.
 *
 * Returns 0 on a mapped positive cache hit, -1 on a miss/measurement failure,
 * or -2 when no IP-to-ASN snapshot has been loaded.
 */
extern int midr_trace_observe_path(
	const struct prefix *target,
	struct midr_tier1_observation *observation);

extern const struct midr_tier1_observer *midr_trace_observer_get(void);

#endif /* _FRR_MIDR_TRACE_OBSERVER_H */
