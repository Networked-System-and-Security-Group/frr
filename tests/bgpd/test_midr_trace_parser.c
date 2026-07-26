// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR incremental traceroute-output parser unit tests.
 */

#include <zebra.h>

#include "privs.h"

#include "bgpd/midr_trace_observer.h"

/* Required by libbgp test linkage. */
struct zebra_privs_t bgpd_privs = {};
struct event_loop *master = NULL;

static int failed;

static void expect_true(bool condition, const char *what)
{
	if (condition)
		return;

	printf("failed: %s\n", what);
	failed++;
}

static void test_chunked_ipv4_ipv6_and_hidden_hops(void)
{
	static const char first_chunk[] =
		"traceroute to 198.51.100.7, 30 hops max\n"
		" 1  192.0.";
	static const char second_chunk[] =
		"2.1  0.123 ms\n"
		" 2  * * *\n"
		" 3  router.example (2001:db8::1)  1.2 ms\n";
	struct midr_trace_parser parser;
	struct midr_trace_raw_path path;
	struct prefix expected;
	enum midr_trace_parse_rc rc;
	int failures_before = failed;

	printf("chunked IPv4/IPv6 and hidden hops\n");
	midr_trace_parser_init(&parser);
	rc = midr_trace_parser_feed(&parser, first_chunk,
				    strlen(first_chunk));
	expect_true(rc == MIDR_TRACE_PARSE_OK, "first chunk accepted");
	rc = midr_trace_parser_feed(&parser, second_chunk,
				    strlen(second_chunk));
	expect_true(rc == MIDR_TRACE_PARSE_OK, "second chunk accepted");
	rc = midr_trace_parser_finish(&parser, &path);
	expect_true(rc == MIDR_TRACE_PARSE_OK, "chunked path completed");
	expect_true(path.hop_count == 3, "three hop rows parsed");

	expect_true(str2prefix("192.0.2.1", &expected) > 0,
		    "expected IPv4 address parsed");
	expect_true(path.hops[0].visible
			    && prefix_same(&path.hops[0].address, &expected),
		    "first visible IPv4 hop parsed");
	expect_true(!path.hops[1].visible, "asterisk hop remains hidden");
	expect_true(str2prefix("2001:db8::1", &expected) > 0,
		    "expected IPv6 address parsed");
	expect_true(path.hops[2].visible
			    && prefix_same(&path.hops[2].address, &expected),
		    "parenthesized IPv6 hop parsed");

	if (failed == failures_before)
		printf("OK\n");
}

static void test_final_line_without_newline(void)
{
	static const char output[] = " 1  203.0.113.9  0.5 ms";
	struct midr_trace_parser parser;
	struct midr_trace_raw_path path;
	enum midr_trace_parse_rc rc;
	int failures_before = failed;

	printf("final line without newline\n");
	midr_trace_parser_init(&parser);
	rc = midr_trace_parser_feed(&parser, output, strlen(output));
	expect_true(rc == MIDR_TRACE_PARSE_OK,
		    "unterminated final line buffered");
	rc = midr_trace_parser_finish(&parser, &path);
	expect_true(rc == MIDR_TRACE_PARSE_OK,
		    "unterminated final line completed");
	expect_true(path.hop_count == 1 && path.hops[0].visible,
		    "unterminated hop row parsed");

	if (failed == failures_before)
		printf("OK\n");
}

static void test_empty_and_overlong_output(void)
{
	struct midr_trace_parser parser;
	struct midr_trace_raw_path path;
	char overlong[MIDR_TRACE_LINE_MAX + 1];
	enum midr_trace_parse_rc rc;
	int failures_before = failed;

	printf("empty and overlong output rejection\n");
	midr_trace_parser_init(&parser);
	rc = midr_trace_parser_finish(&parser, &path);
	expect_true(rc == MIDR_TRACE_PARSE_INVALID,
		    "empty output is rejected");

	memset(overlong, 'x', sizeof(overlong));
	midr_trace_parser_init(&parser);
	rc = midr_trace_parser_feed(&parser, overlong, sizeof(overlong));
	expect_true(rc == MIDR_TRACE_PARSE_OUTPUT_LIMIT,
		    "overlong line reaches output limit");
	expect_true(parser.path.output_truncated,
		    "overlong line marks output truncated");

	if (failed == failures_before)
		printf("OK\n");
}

static void test_hop_capacity_limit(void)
{
	static const char hidden_hop[] = " 1  * * *\n";
	struct midr_trace_parser parser;
	enum midr_trace_parse_rc rc = MIDR_TRACE_PARSE_OK;
	size_t i;
	int failures_before = failed;

	printf("hop capacity limit\n");
	midr_trace_parser_init(&parser);
	for (i = 0; i < MIDR_TIER1_MAX_OBSERVED_ASNS; i++) {
		rc = midr_trace_parser_feed(&parser, hidden_hop,
					    strlen(hidden_hop));
		if (rc != MIDR_TRACE_PARSE_OK)
			break;
	}
	expect_true(rc == MIDR_TRACE_PARSE_OK,
		    "maximum supported hop count accepted");
	rc = midr_trace_parser_feed(&parser, hidden_hop,
				    strlen(hidden_hop));
	expect_true(rc == MIDR_TRACE_PARSE_OUTPUT_LIMIT,
		    "additional hop is rejected");
	expect_true(parser.path.output_truncated,
		    "hop overflow marks output truncated");

	if (failed == failures_before)
		printf("OK\n");
}

int main(void)
{
	test_chunked_ipv4_ipv6_and_hidden_hops();
	test_final_line_without_newline();
	test_empty_and_overlong_output();
	test_hop_capacity_limit();

	printf("failures: %d\n", failed);
	return failed;
}
