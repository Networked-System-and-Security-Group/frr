// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR asynchronous traceroute scheduler.
 */

#ifndef _FRR_MIDR_TRACE_SCHEDULER_H
#define _FRR_MIDR_TRACE_SCHEDULER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "frrevent.h"
#include "prefix.h"

#include "bgpd/midr_tier1.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MIDR_TRACE_PROFILE_VERSION 1U
#define MIDR_TRACE_CONCURRENCY_HARD_MAX 32U

enum midr_trace_status {
	MIDR_TRACE_OK = 0,
	MIDR_TRACE_ERR_INVALID,
	MIDR_TRACE_ERR_NO_SNAPSHOT,
	MIDR_TRACE_ERR_UNSUPPORTED,
	MIDR_TRACE_ERR_QUEUE_FULL,
	MIDR_TRACE_ERR_REQUEST_LIMIT,
	MIDR_TRACE_ERR_QUEUE_TIMEOUT,
	MIDR_TRACE_ERR_SPAWN,
	MIDR_TRACE_ERR_EXEC_TIMEOUT,
	MIDR_TRACE_ERR_OUTPUT_LIMIT,
	MIDR_TRACE_ERR_PARSE,
	MIDR_TRACE_ERR_EXIT_STATUS,
	MIDR_TRACE_ERR_CANCELED,
	MIDR_TRACE_ERR_SHUTDOWN,
};

enum midr_trace_cache_state {
	MIDR_TRACE_CACHE_MISS,
	MIDR_TRACE_CACHE_HIT,
	MIDR_TRACE_CACHE_NEGATIVE_HIT,
	MIDR_TRACE_CACHE_COALESCED,
	MIDR_TRACE_CACHE_REFRESH,
};

struct midr_trace_raw_hop {
	bool visible;
	struct prefix address;
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
	bool has_wait_status;
	bool exited_normally;
	int child_exit_code;
	int child_signal;
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

struct midr_trace_delivery {
	uint64_t request_id;
	enum midr_trace_status status;
	bool has_cache_metadata;
	enum midr_trace_cache_state cache_state;
	uint32_t cache_age_msec;
	bool has_view;
	struct midr_trace_query_view view;
};

struct midr_trace_request_options {
	bool force_refresh;
};

enum midr_trace_submit_rc {
	MIDR_TRACE_SUBMIT_ACCEPTED = 0,
	MIDR_TRACE_SUBMIT_INVALID,
	MIDR_TRACE_SUBMIT_NOT_READY,
	MIDR_TRACE_SUBMIT_UNSUPPORTED,
	MIDR_TRACE_SUBMIT_QUEUE_FULL,
	MIDR_TRACE_SUBMIT_REQUEST_LIMIT,
	MIDR_TRACE_SUBMIT_ID_EXHAUSTED,
	MIDR_TRACE_SUBMIT_SHUTDOWN,
};

typedef void (*midr_trace_done_cb)(
	const struct midr_trace_delivery *delivery, void *arg);

enum midr_trace_cache_lookup_rc {
	MIDR_TRACE_LOOKUP_NO_SNAPSHOT,
	MIDR_TRACE_LOOKUP_MISS,
	MIDR_TRACE_LOOKUP_HIT,
};

enum midr_trace_ensure_state {
	MIDR_TRACE_ENSURE_CACHE_HIT,
	MIDR_TRACE_ENSURE_QUEUED,
	MIDR_TRACE_ENSURE_RUNNING,
};

enum midr_trace_job_state {
	MIDR_TRACE_JOB_QUEUED,
	MIDR_TRACE_JOB_STARTING,
	MIDR_TRACE_JOB_RUNNING,
	MIDR_TRACE_JOB_TERM_SENT,
	MIDR_TRACE_JOB_KILL_SENT,
	MIDR_TRACE_JOB_FINALIZING,
	MIDR_TRACE_JOB_DONE,
};

enum midr_trace_job_query_state {
	MIDR_TRACE_QUERY_QUEUED,
	MIDR_TRACE_QUERY_RUNNING,
	MIDR_TRACE_QUERY_COMPLETED,
	MIDR_TRACE_QUERY_NOT_FOUND_OR_EXPIRED,
};

struct midr_trace_job_snapshot {
	uint64_t job_id;
	enum midr_trace_job_state state;
	struct prefix target;
	bool has_result;
	struct midr_trace_query_view view;
};

struct midr_trace_scheduler_config {
	uint32_t concurrency;
	uint32_t queue_limit;
	uint32_t max_requests_per_job;
	uint32_t max_pending_requests;
	uint32_t queue_timeout_msec;
	uint32_t execution_timeout_msec;
	uint32_t kill_grace_msec;
	uint32_t post_exit_drain_msec;
	uint32_t output_limit_bytes;
	uint32_t success_cache_ttl_msec;
	uint32_t negative_cache_ttl_msec;
	uint32_t cache_capacity;
	uint32_t history_ttl_msec;
	uint32_t history_capacity;
};

struct midr_trace_scheduler_stats {
	bool accepting;
	uint32_t concurrency;
	uint32_t active_slots;
	uint32_t queued_jobs;
	uint32_t inflight_jobs;
	uint32_t pending_requests;
	uint32_t cache_entries;
	uint32_t history_entries;
	uint64_t submitted;
	uint64_t completed;
	uint64_t cache_hits;
	uint64_t cache_misses;
	uint64_t coalesced;
	uint64_t evicted;
	uint64_t queue_full;
	uint64_t queue_timeouts;
	uint64_t execution_timeouts;
	uint64_t spawn_errors;
	uint64_t parse_errors;
	uint64_t exit_errors;
	uint64_t output_limit_errors;
	uint64_t canceled;
};

struct vty;

int midr_trace_scheduler_init(struct event_loop *master);
void midr_trace_scheduler_quiesce(void);
void midr_trace_scheduler_fini(void);
bool midr_trace_scheduler_is_ready(void);

/*
 * Register this wrapper as bgpd's SIGCHLD dispatch entry.  It only reaps
 * children registered by the MIDR executor.
 */
void midr_trace_scheduler_sigchld(void);

enum midr_trace_submit_rc midr_trace_request_async(
	const struct prefix *target,
	const struct midr_trace_request_options *options,
	midr_trace_done_cb done, void *arg, uint64_t *request_id);
bool midr_trace_cancel(uint64_t request_id);

enum midr_trace_cache_lookup_rc midr_trace_cache_lookup(
	const struct prefix *target,
	const struct midr_trace_request_options *options,
	struct midr_trace_query_view *view, uint32_t *cache_age_msec);

enum midr_trace_submit_rc midr_trace_ensure_job(
	const struct prefix *target,
	const struct midr_trace_request_options *options, uint64_t *job_id,
	enum midr_trace_ensure_state *state);

enum midr_trace_job_query_state midr_trace_job_lookup(
	uint64_t job_id, struct midr_trace_job_snapshot *snapshot);

void midr_trace_scheduler_config_get(
	struct midr_trace_scheduler_config *config);
int midr_trace_scheduler_config_set(
	const struct midr_trace_scheduler_config *config, char *errmsg,
	size_t errmsg_len);
int midr_trace_scheduler_config_write(struct vty *vty);
void midr_trace_scheduler_stats_get(
	struct midr_trace_scheduler_stats *stats);

/* A NULL target clears the entire raw cache. */
unsigned int midr_trace_cache_clear(const struct prefix *target);

const char *midr_trace_status_name(enum midr_trace_status status);
const char *midr_trace_cache_state_name(enum midr_trace_cache_state state);

#ifdef __cplusplus
}
#endif

#endif /* _FRR_MIDR_TRACE_SCHEDULER_H */
