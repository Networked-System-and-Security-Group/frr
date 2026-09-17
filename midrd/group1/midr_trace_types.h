// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _FRR_MIDR_TRACE_TYPES_H
#define _FRR_MIDR_TRACE_TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include "midrd/group1/midr_tier1.h"

#define MIDR_TRACE_PROFILE_VERSION 3U
#define MIDR_TRACE_CONCURRENCY_HARD_MAX 32U

enum midr_trace_status {
	MIDR_TRACE_OK = 0,
	MIDR_TRACE_ERR_INVALID,
	MIDR_TRACE_ERR_NO_SNAPSHOT,
	MIDR_TRACE_ERR_UNSUPPORTED,
	MIDR_TRACE_ERR_QUEUE_FULL,
	MIDR_TRACE_ERR_REQUEST_LIMIT,
	MIDR_TRACE_ERR_QUEUE_TIMEOUT,
	MIDR_TRACE_ERR_SPAWN, /* Reserved: removed external-process backend. */
	MIDR_TRACE_ERR_EXEC_TIMEOUT,
	MIDR_TRACE_ERR_OUTPUT_LIMIT, /* Reserved. */
	MIDR_TRACE_ERR_PARSE, /* Reserved. */
	MIDR_TRACE_ERR_EXIT_STATUS, /* Reserved. */
	MIDR_TRACE_ERR_CANCELED,
	MIDR_TRACE_ERR_SHUTDOWN,
	MIDR_TRACE_ERR_SOCKET,
	MIDR_TRACE_ERR_SEND,
	MIDR_TRACE_ERR_RECEIVE,
	MIDR_TRACE_ERR_RESOURCE,
};

enum midr_trace_cache_state {
	MIDR_TRACE_CACHE_MISS,
	MIDR_TRACE_CACHE_HIT,
	MIDR_TRACE_CACHE_NEGATIVE_HIT,
	MIDR_TRACE_CACHE_COALESCED,
	MIDR_TRACE_CACHE_REFRESH,
};

enum midr_trace_stop_reason {
	MIDR_TRACE_STOP_NONE,
	MIDR_TRACE_STOP_REACHED,
	MIDR_TRACE_STOP_MAX_HOPS,
	MIDR_TRACE_STOP_UNREACHABLE,
	MIDR_TRACE_STOP_ERROR,
};

/* Zero initialization selects the diagnostic CLI's default context.  MIDR
 * admission supplies an explicit source and an instance lifetime cookie.
 * Non-default VRFs are rejected until all MIDR channels support them. */
struct midr_trace_net_context {
	struct prefix source;
	uint64_t instance_cookie;
	uint32_t vrf_id;
};

struct midr_trace_raw_hop {
	bool visible;
	struct prefix address;
	uint8_t ttl;
	bool has_rtt;
	uint32_t rtt_usec;
	bool has_icmp;
	uint8_t icmp_type;
	uint8_t icmp_code;
};

struct midr_trace_raw_path {
	struct midr_trace_raw_hop hops[MIDR_TIER1_MAX_OBSERVED_ASNS];
	size_t hop_count;
	bool output_truncated;
};

struct midr_trace_job_result {
	uint64_t job_id;
	enum midr_trace_status status;
	struct prefix target;
	struct midr_trace_raw_path raw_path;
	uint32_t queue_msec;
	uint32_t execution_msec;
	bool target_reached;
	enum midr_trace_stop_reason stop_reason;
	int system_errno;
};

enum midr_trace_mapping_status {
	MIDR_TRACE_MAPPING_OK,
	MIDR_TRACE_MAPPING_NOT_APPLICABLE,
	MIDR_TRACE_MAPPING_NO_SNAPSHOT,
};

struct midr_trace_query_view {
	struct midr_trace_job_result job;
	enum midr_trace_mapping_status mapping_status;
	bool has_observation;
	bool has_ip2asn_generation;
	uint64_t ip2asn_generation;
	struct midr_tier1_observation observation;
};

#endif
