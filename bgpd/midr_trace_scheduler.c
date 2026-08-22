// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR asynchronous traceroute scheduler.
 */

#include <zebra.h>

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <sys/wait.h>

#include "frrevent.h"
#include "hash.h"
#include "jhash.h"
#include "linklist.h"
#include "log.h"
#include "memory.h"
#include "monotime.h"
#include "vty.h"

#include "bgpd/bgp_memory.h"
#include "bgpd/midr_ip2asn.h"
#include "bgpd/midr_trace_exec.h"
#include "bgpd/midr_trace_observer.h"
#include "bgpd/midr_trace_scheduler.h"

#define MIDR_TRACE_DEFAULT_CONCURRENCY 4U
#define MIDR_TRACE_DEFAULT_QUEUE_LIMIT 128U
#define MIDR_TRACE_DEFAULT_REQUESTS_PER_JOB 64U
#define MIDR_TRACE_DEFAULT_PENDING_REQUESTS 4096U
#define MIDR_TRACE_DEFAULT_QUEUE_TIMEOUT_MSEC 5000U
#define MIDR_TRACE_DEFAULT_EXEC_TIMEOUT_MSEC 35000U
#define MIDR_TRACE_DEFAULT_KILL_GRACE_MSEC 500U
#define MIDR_TRACE_DEFAULT_DRAIN_MSEC 100U
#define MIDR_TRACE_DEFAULT_OUTPUT_LIMIT (64U * 1024U)
#define MIDR_TRACE_DEFAULT_CACHE_TTL_MSEC (300U * 1000U)
#define MIDR_TRACE_DEFAULT_NEGATIVE_TTL_MSEC (15U * 1000U)
#define MIDR_TRACE_DEFAULT_CACHE_CAPACITY 1024U
#define MIDR_TRACE_DEFAULT_HISTORY_TTL_MSEC (300U * 1000U)
#define MIDR_TRACE_DEFAULT_HISTORY_CAPACITY 1024U
#define MIDR_TRACE_REAP_SAFETY_MSEC 1000U
#define MIDR_TRACE_CLEANUP_INTERVAL_MSEC 30000U
#define MIDR_TRACE_READ_BUFFER_SIZE 4096U
#define MIDR_TRACE_FINAL_REAP_TIMEOUT_MSEC 2000U
#define MIDR_TRACE_FINAL_REAP_POLL_NSEC (10U * 1000U * 1000U)

DEFINE_MTYPE_STATIC(BGPD, MIDR_TRACE_SCHEDULER, "MIDR traceroute scheduler");
DEFINE_MTYPE_STATIC(BGPD, MIDR_TRACE_JOB, "MIDR traceroute job");
DEFINE_MTYPE_STATIC(BGPD, MIDR_TRACE_REQUEST, "MIDR traceroute request");
DEFINE_MTYPE_STATIC(BGPD, MIDR_TRACE_CACHE, "MIDR traceroute cache entry");
DEFINE_MTYPE_STATIC(BGPD, MIDR_TRACE_HISTORY, "MIDR traceroute history entry");

enum midr_trace_termination_reason {
	MIDR_TRACE_TERM_NONE,
	MIDR_TRACE_TERM_TIMEOUT,
	MIDR_TRACE_TERM_OUTPUT_LIMIT,
	MIDR_TRACE_TERM_PARSE,
	MIDR_TRACE_TERM_SHUTDOWN,
};

enum midr_trace_delivery_kind {
	MIDR_TRACE_DELIVER_JOB_RESULT,
	MIDR_TRACE_DELIVER_CANCELED,
	MIDR_TRACE_DELIVER_SHUTDOWN,
};

enum midr_trace_request_state {
	MIDR_TRACE_REQUEST_PENDING,
	MIDR_TRACE_REQUEST_CALLBACK_QUEUED,
	MIDR_TRACE_REQUEST_CALLBACK_RUNNING,
	MIDR_TRACE_REQUEST_SETTLED,
};

struct midr_trace_key {
	struct prefix target;
	uint32_t profile_version;
};

struct midr_trace_delivery_seed {
	enum midr_trace_delivery_kind kind;
	enum midr_trace_cache_state cache_state;
	uint32_t cache_age_msec;
	bool has_job_result;
	struct midr_trace_job_result job_result;
};

struct midr_trace_scheduler;
struct midr_trace_job;

struct midr_trace_request {
	uint64_t request_id;
	enum midr_trace_request_state state;
	midr_trace_done_cb done;
	void *arg;
	struct event_loop *master;
	struct midr_trace_scheduler *scheduler;
	struct midr_trace_job *job;
	struct event *completion_event;
	enum midr_trace_cache_state source;
	struct midr_trace_delivery_seed seed;
};

struct midr_trace_job {
	struct midr_trace_key key;
	uint64_t job_id;
	struct midr_trace_scheduler *scheduler;
	enum midr_trace_job_state state;
	bool retain_for_poll;
	bool slot_owned;
	bool start_attempted;
	bool stdout_eof;
	bool stderr_eof;
	bool parser_finished;
	bool drain_complete;
	bool defer_finalize;
	bool finalize_started;
	bool result_published;
	bool suppress_publication;
	enum midr_trace_termination_reason termination_reason;
	enum midr_trace_status terminal_status;
	struct list *requests;
	struct timeval queued_at;
	struct timeval started_at;
	struct midr_trace_exec exec;
	struct midr_trace_parser parser;
	uint64_t output_bytes;
	struct event *start_event;
	struct event *read_stdout;
	struct event *read_stderr;
	struct event *queue_timer;
	struct event *execution_timer;
	struct event *kill_timer;
	struct event *final_drain_timer;
	struct event *reap_safety_timer;
};

struct midr_trace_cache_entry {
	struct midr_trace_key key;
	struct midr_trace_job_result result;
	struct timeval completed_at;
	uint32_t ttl_msec;
};

struct midr_trace_history_entry {
	uint64_t job_id;
	struct midr_trace_job_result result;
	struct timeval completed_at;
};

struct midr_trace_scheduler {
	struct event_loop *master;
	bool accepting;
	bool quiescing;
	bool finalizing;
	bool fini_deferred;
	uint64_t next_request_id;
	uint64_t next_job_id;
	uint32_t active_slot_count;
	uint32_t pending_request_count;
	struct midr_trace_scheduler_config config;
	struct midr_trace_scheduler_stats stats;
	struct event *pump_event;
	struct event *cleanup_event;
	struct list *queued_jobs;
	struct hash *inflight_by_key;
	struct hash *active_jobs_by_id;
	struct hash *request_index;
	struct hash *cache_by_key;
	struct list *cache_lru;
	struct hash *recent_jobs_by_id;
	struct list *history_lru;
};

static struct midr_trace_scheduler *midr_trace_scheduler;

static void midr_trace_schedule_pump(struct midr_trace_scheduler *scheduler);
static void midr_trace_job_finalize(struct midr_trace_job *job);
static void midr_trace_job_reaped(void *arg);
static void midr_trace_job_start_event(struct event *event);
static void midr_trace_job_queue_timeout(struct event *event);
static void midr_trace_job_read_stream(
	struct midr_trace_job *job, enum midr_trace_exec_stream stream);
static void midr_trace_job_begin_termination(
	struct midr_trace_job *job,
	enum midr_trace_termination_reason reason);

static struct midr_trace_scheduler_config midr_trace_default_config(void)
{
	return (struct midr_trace_scheduler_config){
		.concurrency = MIDR_TRACE_DEFAULT_CONCURRENCY,
		.queue_limit = MIDR_TRACE_DEFAULT_QUEUE_LIMIT,
		.max_requests_per_job =
			MIDR_TRACE_DEFAULT_REQUESTS_PER_JOB,
		.max_pending_requests =
			MIDR_TRACE_DEFAULT_PENDING_REQUESTS,
		.queue_timeout_msec =
			MIDR_TRACE_DEFAULT_QUEUE_TIMEOUT_MSEC,
		.execution_timeout_msec =
			MIDR_TRACE_DEFAULT_EXEC_TIMEOUT_MSEC,
		.kill_grace_msec = MIDR_TRACE_DEFAULT_KILL_GRACE_MSEC,
		.post_exit_drain_msec = MIDR_TRACE_DEFAULT_DRAIN_MSEC,
		.output_limit_bytes = MIDR_TRACE_DEFAULT_OUTPUT_LIMIT,
		.success_cache_ttl_msec =
			MIDR_TRACE_DEFAULT_CACHE_TTL_MSEC,
		.negative_cache_ttl_msec =
			MIDR_TRACE_DEFAULT_NEGATIVE_TTL_MSEC,
		.cache_capacity = MIDR_TRACE_DEFAULT_CACHE_CAPACITY,
		.history_ttl_msec =
			MIDR_TRACE_DEFAULT_HISTORY_TTL_MSEC,
		.history_capacity = MIDR_TRACE_DEFAULT_HISTORY_CAPACITY,
	};
}

static uint32_t midr_trace_elapsed_msec(const struct timeval *from,
					const struct timeval *to)
{
	struct timeval elapsed;
	uint64_t msec;

	if (!from || !to || timercmp(to, from, <))
		return 0;
	timersub(to, from, &elapsed);
	msec = (uint64_t)elapsed.tv_sec * 1000U
	       + (uint64_t)elapsed.tv_usec / 1000U;
	return msec > UINT32_MAX ? UINT32_MAX : (uint32_t)msec;
}

static uint32_t midr_trace_age_msec(const struct timeval *from)
{
	struct timeval now;

	monotime(&now);
	return midr_trace_elapsed_msec(from, &now);
}

static bool midr_trace_target_normalize(const struct prefix *target,
					struct prefix *normalized)
{
	if (!target || !normalized)
		return false;
	if (target->family != AF_INET && target->family != AF_INET6)
		return false;

	prefix_copy(normalized, target);
	normalized->prefixlen = target->family == AF_INET ? IPV4_MAX_BITLEN
							 : IPV6_MAX_BITLEN;
	apply_mask(normalized);
	return true;
}

static bool midr_trace_key_same(const struct midr_trace_key *a,
				const struct midr_trace_key *b)
{
	return a->profile_version == b->profile_version
	       && prefix_same(&a->target, &b->target);
}

static unsigned int midr_trace_key_hash(const struct midr_trace_key *key)
{
	return jhash_1word(key->profile_version,
			   prefix_hash_key(&key->target));
}

static unsigned int midr_trace_job_key_hash(const void *data)
{
	const struct midr_trace_job *job = data;

	return midr_trace_key_hash(&job->key);
}

static bool midr_trace_job_key_cmp(const void *a, const void *b)
{
	const struct midr_trace_job *job_a = a;
	const struct midr_trace_job *job_b = b;

	return midr_trace_key_same(&job_a->key, &job_b->key);
}

static unsigned int midr_trace_u64_hash(uint64_t value)
{
	return jhash_2words((uint32_t)value, (uint32_t)(value >> 32),
			    0x4d494452U);
}

static unsigned int midr_trace_job_id_hash(const void *data)
{
	const struct midr_trace_job *job = data;

	return midr_trace_u64_hash(job->job_id);
}

static bool midr_trace_job_id_cmp(const void *a, const void *b)
{
	const struct midr_trace_job *job_a = a;
	const struct midr_trace_job *job_b = b;

	return job_a->job_id == job_b->job_id;
}

static unsigned int midr_trace_request_hash(const void *data)
{
	const struct midr_trace_request *request = data;

	return midr_trace_u64_hash(request->request_id);
}

static bool midr_trace_request_cmp(const void *a, const void *b)
{
	const struct midr_trace_request *request_a = a;
	const struct midr_trace_request *request_b = b;

	return request_a->request_id == request_b->request_id;
}

static unsigned int midr_trace_cache_hash(const void *data)
{
	const struct midr_trace_cache_entry *entry = data;

	return midr_trace_key_hash(&entry->key);
}

static bool midr_trace_cache_cmp(const void *a, const void *b)
{
	const struct midr_trace_cache_entry *entry_a = a;
	const struct midr_trace_cache_entry *entry_b = b;

	return midr_trace_key_same(&entry_a->key, &entry_b->key);
}

static unsigned int midr_trace_history_hash(const void *data)
{
	const struct midr_trace_history_entry *entry = data;

	return midr_trace_u64_hash(entry->job_id);
}

static bool midr_trace_history_cmp(const void *a, const void *b)
{
	const struct midr_trace_history_entry *entry_a = a;
	const struct midr_trace_history_entry *entry_b = b;

	return entry_a->job_id == entry_b->job_id;
}

static uint64_t midr_trace_next_id(uint64_t *next)
{
	uint64_t id;

	if (!next || *next == 0)
		return 0;
	id = *next;
	*next = id == UINT64_MAX ? 0 : id + 1;
	return id;
}

static void midr_trace_cache_entry_free(void *data)
{
	XFREE(MTYPE_MIDR_TRACE_CACHE, data);
}

static void midr_trace_history_entry_free(void *data)
{
	XFREE(MTYPE_MIDR_TRACE_HISTORY, data);
}

static void midr_trace_cache_remove(struct midr_trace_scheduler *scheduler,
				    struct midr_trace_cache_entry *entry)
{
	if (!scheduler || !entry)
		return;
	hash_release(scheduler->cache_by_key, entry);
	listnode_delete(scheduler->cache_lru, entry);
	midr_trace_cache_entry_free(entry);
}

static void midr_trace_history_remove(
	struct midr_trace_scheduler *scheduler,
	struct midr_trace_history_entry *entry)
{
	if (!scheduler || !entry)
		return;
	hash_release(scheduler->recent_jobs_by_id, entry);
	listnode_delete(scheduler->history_lru, entry);
	midr_trace_history_entry_free(entry);
}

static void midr_trace_cache_prune(struct midr_trace_scheduler *scheduler)
{
	struct listnode *node;
	struct listnode *next;
	struct midr_trace_cache_entry *entry;

	for (ALL_LIST_ELEMENTS(scheduler->cache_lru, node, next, entry)) {
		if (midr_trace_age_msec(&entry->completed_at)
		    < entry->ttl_msec)
			continue;
		midr_trace_cache_remove(scheduler, entry);
	}
}

static void midr_trace_history_prune(struct midr_trace_scheduler *scheduler)
{
	struct listnode *node;
	struct listnode *next;
	struct midr_trace_history_entry *entry;

	for (ALL_LIST_ELEMENTS(scheduler->history_lru, node, next, entry)) {
		if (midr_trace_age_msec(&entry->completed_at)
		    < scheduler->config.history_ttl_msec)
			continue;
		midr_trace_history_remove(scheduler, entry);
	}
}

static struct midr_trace_cache_entry *
midr_trace_cache_find(struct midr_trace_scheduler *scheduler,
		      const struct midr_trace_key *key)
{
	struct midr_trace_cache_entry lookup = {
		.key = *key,
	};
	struct midr_trace_cache_entry *entry;

	entry = hash_lookup(scheduler->cache_by_key, &lookup);
	if (!entry)
		return NULL;
	if (midr_trace_age_msec(&entry->completed_at) >= entry->ttl_msec) {
		midr_trace_cache_remove(scheduler, entry);
		return NULL;
	}
	if (listnode_lookup(scheduler->cache_lru, entry))
		listnode_move_to_tail(
			scheduler->cache_lru,
			listnode_lookup(scheduler->cache_lru, entry));
	return entry;
}

static bool midr_trace_status_negative_cacheable(
	enum midr_trace_status status)
{
	return status == MIDR_TRACE_ERR_EXEC_TIMEOUT
	       || status == MIDR_TRACE_ERR_SPAWN
	       || status == MIDR_TRACE_ERR_PARSE
	       || status == MIDR_TRACE_ERR_EXIT_STATUS;
}

static void midr_trace_cache_insert(struct midr_trace_scheduler *scheduler,
				    const struct midr_trace_key *key,
				    const struct midr_trace_job_result *result)
{
	struct midr_trace_cache_entry *entry;
	uint32_t ttl_msec;

	if (!scheduler->config.cache_capacity)
		return;
	if (result->status == MIDR_TRACE_OK)
		ttl_msec = scheduler->config.success_cache_ttl_msec;
	else if (midr_trace_status_negative_cacheable(result->status))
		ttl_msec = scheduler->config.negative_cache_ttl_msec;
	else
		return;
	if (!ttl_msec)
		return;

	entry = midr_trace_cache_find(scheduler, key);
	if (!entry) {
		entry = XCALLOC(MTYPE_MIDR_TRACE_CACHE, sizeof(*entry));
		entry->key = *key;
		hash_get(scheduler->cache_by_key, entry, hash_alloc_intern);
		listnode_add(scheduler->cache_lru, entry);
	}
	entry->result = *result;
	entry->ttl_msec = ttl_msec;
	monotime(&entry->completed_at);

	while (listcount(scheduler->cache_lru)
	       > scheduler->config.cache_capacity) {
		entry = listnode_head(scheduler->cache_lru);
		midr_trace_cache_remove(scheduler, entry);
		scheduler->stats.evicted++;
	}
}

static void midr_trace_history_insert(
	struct midr_trace_scheduler *scheduler,
	const struct midr_trace_job_result *result)
{
	struct midr_trace_history_entry lookup = {
		.job_id = result->job_id,
	};
	struct midr_trace_history_entry *entry;

	if (!scheduler->config.history_capacity)
		return;
	entry = hash_lookup(scheduler->recent_jobs_by_id, &lookup);
	if (!entry) {
		entry = XCALLOC(MTYPE_MIDR_TRACE_HISTORY, sizeof(*entry));
		entry->job_id = result->job_id;
		hash_get(scheduler->recent_jobs_by_id, entry,
			 hash_alloc_intern);
		listnode_add(scheduler->history_lru, entry);
	}
	entry->result = *result;
	monotime(&entry->completed_at);

	while (listcount(scheduler->history_lru)
	       > scheduler->config.history_capacity) {
		entry = listnode_head(scheduler->history_lru);
		midr_trace_history_remove(scheduler, entry);
	}
}

static struct midr_trace_job *
midr_trace_inflight_find(struct midr_trace_scheduler *scheduler,
			 const struct midr_trace_key *key)
{
	struct midr_trace_job lookup = {
		.key = *key,
	};

	return hash_lookup(scheduler->inflight_by_key, &lookup);
}

static struct midr_trace_job *
midr_trace_active_job_find(struct midr_trace_scheduler *scheduler,
			   uint64_t job_id)
{
	struct midr_trace_job lookup = {
		.job_id = job_id,
	};

	return hash_lookup(scheduler->active_jobs_by_id, &lookup);
}

static struct midr_trace_request *
midr_trace_request_find(struct midr_trace_scheduler *scheduler,
			uint64_t request_id)
{
	struct midr_trace_request lookup = {
		.request_id = request_id,
	};

	return hash_lookup(scheduler->request_index, &lookup);
}

static void midr_trace_set_error(char *errmsg, size_t errmsg_len,
				 const char *message)
{
	if (errmsg && errmsg_len)
		strlcpy(errmsg, message, errmsg_len);
}

static int midr_trace_config_validate(
	const struct midr_trace_scheduler_config *config, char *errmsg,
	size_t errmsg_len)
{
	if (!config) {
		midr_trace_set_error(errmsg, errmsg_len, "missing configuration");
		return EINVAL;
	}
	if (config->concurrency < 1
	    || config->concurrency > MIDR_TRACE_CONCURRENCY_HARD_MAX) {
		midr_trace_set_error(errmsg, errmsg_len,
				     "concurrency must be in 1..32");
		return ERANGE;
	}
	if (config->queue_limit < 1 || config->queue_limit > 4096) {
		midr_trace_set_error(errmsg, errmsg_len,
				     "queue limit must be in 1..4096");
		return ERANGE;
	}
	if (config->max_requests_per_job < 1
	    || config->max_requests_per_job > 256) {
		midr_trace_set_error(errmsg, errmsg_len,
				     "per-job request limit must be in 1..256");
		return ERANGE;
	}
	if (config->max_pending_requests < 64
	    || config->max_pending_requests > 65536) {
		midr_trace_set_error(errmsg, errmsg_len,
				     "pending request limit must be in 64..65536");
		return ERANGE;
	}
	if (config->queue_timeout_msec > 60000) {
		midr_trace_set_error(errmsg, errmsg_len,
				     "queue timeout must be in 0..60000 ms");
		return ERANGE;
	}
	if (config->queue_timeout_msec % 1000U
	    || config->execution_timeout_msec % 1000U
	    || config->success_cache_ttl_msec % 1000U
	    || config->negative_cache_ttl_msec % 1000U) {
		midr_trace_set_error(errmsg, errmsg_len,
				     "CLI-persisted timeouts and TTLs require whole-second granularity");
		return EINVAL;
	}
	if (config->execution_timeout_msec < 1000
	    || config->execution_timeout_msec > 120000) {
		midr_trace_set_error(errmsg, errmsg_len,
				     "execution timeout must be in 1000..120000 ms");
		return ERANGE;
	}
	if (config->kill_grace_msec < 100
	    || config->kill_grace_msec > 5000) {
		midr_trace_set_error(errmsg, errmsg_len,
				     "kill grace must be in 100..5000 ms");
		return ERANGE;
	}
	if (config->post_exit_drain_msec < 10
	    || config->post_exit_drain_msec > 1000) {
		midr_trace_set_error(errmsg, errmsg_len,
				     "post-exit drain must be in 10..1000 ms");
		return ERANGE;
	}
	if (config->output_limit_bytes < 4096
	    || config->output_limit_bytes > 1024U * 1024U) {
		midr_trace_set_error(errmsg, errmsg_len,
				     "output limit must be in 4096..1048576 bytes");
		return ERANGE;
	}
	if (config->success_cache_ttl_msec > 86400U * 1000U
	    || config->negative_cache_ttl_msec > 300U * 1000U) {
		midr_trace_set_error(errmsg, errmsg_len,
				     "cache TTL is outside the supported range");
		return ERANGE;
	}
	if (config->cache_capacity > 65536
	    || config->history_capacity < 1
	    || config->history_capacity > 65536
	    || config->history_ttl_msec < 30000
	    || config->history_ttl_msec > 3600U * 1000U) {
		midr_trace_set_error(errmsg, errmsg_len,
				     "cache/history capacity or TTL is outside the supported range");
		return ERANGE;
	}
	return 0;
}

static void midr_trace_request_completion_event(struct event *event);

static struct midr_trace_request *
midr_trace_request_new(struct midr_trace_scheduler *scheduler,
		       uint64_t request_id, midr_trace_done_cb done, void *arg)
{
	struct midr_trace_request *request;

	request = XCALLOC(MTYPE_MIDR_TRACE_REQUEST, sizeof(*request));
	request->request_id = request_id;
	request->state = MIDR_TRACE_REQUEST_PENDING;
	request->done = done;
	request->arg = arg;
	request->master = scheduler->master;
	request->scheduler = scheduler;
	return request;
}

static void midr_trace_request_detach_job(
	struct midr_trace_request *request)
{
	if (!request || !request->job)
		return;
	listnode_delete(request->job->requests, request);
	request->job = NULL;
}

static bool midr_trace_request_claim_terminal(
	struct midr_trace_request *request,
	const struct midr_trace_delivery_seed *seed)
{
	if (!request || !seed
	    || request->state != MIDR_TRACE_REQUEST_PENDING)
		return false;

	midr_trace_request_detach_job(request);
	request->seed = *seed;
	request->state = MIDR_TRACE_REQUEST_CALLBACK_QUEUED;
	event_add_event(request->master,
			midr_trace_request_completion_event, request, 0,
			&request->completion_event);
	return true;
}

static void midr_trace_request_build_delivery(
	const struct midr_trace_request *request,
	struct midr_trace_delivery *delivery)
{
	memset(delivery, 0, sizeof(*delivery));
	delivery->request_id = request->request_id;

	switch (request->seed.kind) {
	case MIDR_TRACE_DELIVER_CANCELED:
		delivery->status = MIDR_TRACE_ERR_CANCELED;
		return;
	case MIDR_TRACE_DELIVER_SHUTDOWN:
		delivery->status = MIDR_TRACE_ERR_SHUTDOWN;
		return;
	case MIDR_TRACE_DELIVER_JOB_RESULT:
		break;
	}

	if (!request->seed.has_job_result) {
		delivery->status = MIDR_TRACE_ERR_PARSE;
		return;
	}
	delivery->has_cache_metadata = true;
	delivery->cache_state = request->seed.cache_state;
	delivery->cache_age_msec = request->seed.cache_age_msec;
	delivery->has_view = true;
	midr_trace_map_ip2asn(&request->seed.job_result, &delivery->view);
	delivery->status = delivery->view.job.status;
	if (delivery->status == MIDR_TRACE_OK
	    && delivery->view.mapping_status
		       == MIDR_TRACE_MAPPING_NO_SNAPSHOT)
		delivery->status = MIDR_TRACE_ERR_NO_SNAPSHOT;
}

static void midr_trace_request_release(
	struct midr_trace_request *request)
{
	if (!request)
		return;
	event_cancel_event(request->master, request);
	XFREE(MTYPE_MIDR_TRACE_REQUEST, request);
}

static void midr_trace_request_deliver(struct midr_trace_request *request,
				       bool remove_from_index)
{
	struct midr_trace_scheduler *scheduler = request->scheduler;
	struct midr_trace_delivery delivery;
	midr_trace_done_cb done = request->done;
	void *arg = request->arg;

	if (remove_from_index) {
		if (hash_release(scheduler->request_index, request)
		    && scheduler->pending_request_count)
			scheduler->pending_request_count--;
	}
	request->state = MIDR_TRACE_REQUEST_CALLBACK_RUNNING;
	midr_trace_request_build_delivery(request, &delivery);
	done(&delivery, arg);
	request->state = MIDR_TRACE_REQUEST_SETTLED;
	midr_trace_request_release(request);
}

static void midr_trace_request_completion_event(struct event *event)
{
	struct midr_trace_request *request = EVENT_ARG(event);

	if (!request
	    || request->state != MIDR_TRACE_REQUEST_CALLBACK_QUEUED)
		return;
	midr_trace_request_deliver(request, true);
}

static void midr_trace_job_schedule_read(struct midr_trace_job *job,
					 enum midr_trace_exec_stream stream);

static void midr_trace_job_parser_finish(struct midr_trace_job *job)
{
	enum midr_trace_parse_rc parse_rc;
	struct midr_trace_raw_path path;

	if (job->parser_finished)
		return;
	parse_rc = midr_trace_parser_finish(&job->parser, &path);
	job->parser_finished = true;
	if (parse_rc == MIDR_TRACE_PARSE_OK) {
		job->parser.path = path;
		return;
	}
	if (parse_rc == MIDR_TRACE_PARSE_OUTPUT_LIMIT) {
		job->parser.path = path;
		job->parser.path.output_truncated = true;
		midr_trace_job_begin_termination(
			job, MIDR_TRACE_TERM_OUTPUT_LIMIT);
		return;
	}
	midr_trace_job_begin_termination(job, MIDR_TRACE_TERM_PARSE);
}

static void midr_trace_job_close_stream(
	struct midr_trace_job *job, enum midr_trace_exec_stream stream)
{
	if (stream == MIDR_TRACE_EXEC_STDOUT) {
		event_cancel(&job->read_stdout);
		midr_trace_exec_close_stream(&job->exec, stream);
		job->stdout_eof = true;
		midr_trace_job_parser_finish(job);
	} else {
		event_cancel(&job->read_stderr);
		midr_trace_exec_close_stream(&job->exec, stream);
		job->stderr_eof = true;
	}
}

static struct midr_trace_job *
midr_trace_job_new(struct midr_trace_scheduler *scheduler,
		   const struct midr_trace_key *key, uint64_t job_id,
		   bool retain_for_poll)
{
	struct midr_trace_job *job;

	job = XCALLOC(MTYPE_MIDR_TRACE_JOB, sizeof(*job));
	job->key = *key;
	job->job_id = job_id;
	job->scheduler = scheduler;
	job->retain_for_poll = retain_for_poll;
	job->state = MIDR_TRACE_JOB_QUEUED;
	job->requests = list_new();
	job->terminal_status = MIDR_TRACE_ERR_PARSE;
	monotime(&job->queued_at);
	midr_trace_exec_init(&job->exec);
	midr_trace_parser_init(&job->parser);
	return job;
}

static void midr_trace_job_index(struct midr_trace_scheduler *scheduler,
				 struct midr_trace_job *job)
{
	hash_get(scheduler->inflight_by_key, job, hash_alloc_intern);
	hash_get(scheduler->active_jobs_by_id, job, hash_alloc_intern);
}

static void midr_trace_job_start_or_queue(struct midr_trace_job *job)
{
	struct midr_trace_scheduler *scheduler = job->scheduler;

	if (scheduler->active_slot_count < scheduler->config.concurrency) {
		job->slot_owned = true;
		scheduler->active_slot_count++;
		job->state = MIDR_TRACE_JOB_STARTING;
		event_add_event(scheduler->master,
				midr_trace_job_start_event, job, 0,
				&job->start_event);
		return;
	}

	job->state = MIDR_TRACE_JOB_QUEUED;
	listnode_add(scheduler->queued_jobs, job);
	if (scheduler->config.queue_timeout_msec)
		event_add_timer_msec(
			scheduler->master, midr_trace_job_queue_timeout, job,
			scheduler->config.queue_timeout_msec,
			&job->queue_timer);
}

static void midr_trace_scheduler_pump(struct event *event)
{
	struct midr_trace_scheduler *scheduler = EVENT_ARG(event);

	if (!scheduler || !scheduler->accepting)
		return;

	while (scheduler->active_slot_count < scheduler->config.concurrency
	       && listcount(scheduler->queued_jobs)) {
		struct midr_trace_job *job =
			listnode_head(scheduler->queued_jobs);

		listnode_delete(scheduler->queued_jobs, job);
		event_cancel(&job->queue_timer);
		job->state = MIDR_TRACE_JOB_STARTING;
		job->slot_owned = true;
		scheduler->active_slot_count++;
		event_add_event(scheduler->master,
				midr_trace_job_start_event, job, 0,
				&job->start_event);
	}
}

static void midr_trace_schedule_pump(struct midr_trace_scheduler *scheduler)
{
	if (!scheduler || !scheduler->accepting || scheduler->pump_event
	    || !listcount(scheduler->queued_jobs)
	    || scheduler->active_slot_count >= scheduler->config.concurrency)
		return;
	event_add_event(scheduler->master, midr_trace_scheduler_pump,
			scheduler, 0, &scheduler->pump_event);
}

static void midr_trace_scheduler_cleanup(struct event *event)
{
	struct midr_trace_scheduler *scheduler = EVENT_ARG(event);

	if (!scheduler || !scheduler->accepting)
		return;
	midr_trace_cache_prune(scheduler);
	midr_trace_history_prune(scheduler);
	event_add_timer_msec(
		scheduler->master, midr_trace_scheduler_cleanup, scheduler,
		MIDR_TRACE_CLEANUP_INTERVAL_MSEC, &scheduler->cleanup_event);
}

static enum midr_trace_submit_rc
midr_trace_submit_precheck(const struct prefix *target,
			   struct midr_trace_key *key)
{
	struct midr_trace_scheduler *scheduler = midr_trace_scheduler;

	if (!target || !key
	    || !midr_trace_target_normalize(target, &key->target))
		return MIDR_TRACE_SUBMIT_INVALID;
	key->profile_version = MIDR_TRACE_PROFILE_VERSION;
	if (!scheduler)
		return midr_trace_exec_supported()
			       ? MIDR_TRACE_SUBMIT_NOT_READY
			       : MIDR_TRACE_SUBMIT_UNSUPPORTED;
	if (!scheduler->accepting)
		return MIDR_TRACE_SUBMIT_SHUTDOWN;
	if (!midr_ip2asn_is_loaded())
		return MIDR_TRACE_SUBMIT_NOT_READY;
	return MIDR_TRACE_SUBMIT_ACCEPTED;
}

enum midr_trace_submit_rc midr_trace_request_async(
	const struct prefix *target,
	const struct midr_trace_request_options *options,
	midr_trace_done_cb done, void *arg, uint64_t *request_id)
{
	struct midr_trace_scheduler *scheduler = midr_trace_scheduler;
	struct midr_trace_cache_entry *cache = NULL;
	struct midr_trace_job *job = NULL;
	struct midr_trace_request *request;
	struct midr_trace_key key;
	struct midr_trace_delivery_seed seed;
	enum midr_trace_submit_rc rc;
	bool force_refresh = options && options->force_refresh;
	bool new_job;
	uint64_t new_request_id;
	uint64_t new_job_id = 0;

	if (request_id)
		*request_id = 0;
	if (!done || !request_id)
		return MIDR_TRACE_SUBMIT_INVALID;
	rc = midr_trace_submit_precheck(target, &key);
	if (rc != MIDR_TRACE_SUBMIT_ACCEPTED)
		return rc;

	if (!force_refresh)
		cache = midr_trace_cache_find(scheduler, &key);
	if (!cache)
		job = midr_trace_inflight_find(scheduler, &key);
	new_job = !cache && !job;

	if (scheduler->pending_request_count
	    >= scheduler->config.max_pending_requests)
		return MIDR_TRACE_SUBMIT_REQUEST_LIMIT;
	if (job
	    && listcount(job->requests)
		       >= scheduler->config.max_requests_per_job)
		return MIDR_TRACE_SUBMIT_REQUEST_LIMIT;
	if (new_job
	    && scheduler->active_slot_count >= scheduler->config.concurrency
	    && listcount(scheduler->queued_jobs)
		       >= scheduler->config.queue_limit) {
		scheduler->stats.queue_full++;
		return MIDR_TRACE_SUBMIT_QUEUE_FULL;
	}
	if (!scheduler->next_request_id
	    || (new_job && !scheduler->next_job_id))
		return MIDR_TRACE_SUBMIT_ID_EXHAUSTED;

	new_request_id = midr_trace_next_id(&scheduler->next_request_id);
	if (new_job)
		new_job_id = midr_trace_next_id(&scheduler->next_job_id);
	request = midr_trace_request_new(scheduler, new_request_id, done, arg);
	hash_get(scheduler->request_index, request, hash_alloc_intern);
	scheduler->pending_request_count++;
	*request_id = new_request_id;

	if (cache) {
		memset(&seed, 0, sizeof(seed));
		seed.kind = MIDR_TRACE_DELIVER_JOB_RESULT;
		seed.cache_state =
			cache->result.status == MIDR_TRACE_OK
				? MIDR_TRACE_CACHE_HIT
				: MIDR_TRACE_CACHE_NEGATIVE_HIT;
		seed.cache_age_msec =
			midr_trace_age_msec(&cache->completed_at);
		seed.has_job_result = true;
		seed.job_result = cache->result;
		request->source = seed.cache_state;
		scheduler->stats.cache_hits++;
		midr_trace_request_claim_terminal(request, &seed);
		return MIDR_TRACE_SUBMIT_ACCEPTED;
	}

	if (job) {
		request->source = MIDR_TRACE_CACHE_COALESCED;
		request->job = job;
		listnode_add(job->requests, request);
		scheduler->stats.coalesced++;
		scheduler->stats.cache_misses++;
		return MIDR_TRACE_SUBMIT_ACCEPTED;
	}

	job = midr_trace_job_new(scheduler, &key, new_job_id, false);
	midr_trace_job_index(scheduler, job);
	scheduler->stats.submitted++;
	request->source = force_refresh ? MIDR_TRACE_CACHE_REFRESH
					: MIDR_TRACE_CACHE_MISS;
	request->job = job;
	listnode_add(job->requests, request);
	scheduler->stats.cache_misses++;
	midr_trace_job_start_or_queue(job);
	midr_trace_schedule_pump(scheduler);
	return MIDR_TRACE_SUBMIT_ACCEPTED;
}

bool midr_trace_cancel(uint64_t request_id)
{
	struct midr_trace_scheduler *scheduler = midr_trace_scheduler;
	struct midr_trace_request *request;
	struct midr_trace_job *job;
	struct midr_trace_delivery_seed seed = {
		.kind = MIDR_TRACE_DELIVER_CANCELED,
	};

	if (!scheduler || !request_id)
		return false;
	request = midr_trace_request_find(scheduler, request_id);
	if (!request || request->state != MIDR_TRACE_REQUEST_PENDING)
		return false;

	job = request->job;
	if (!midr_trace_request_claim_terminal(request, &seed))
		return false;
	scheduler->stats.canceled++;

	if (job && job->state == MIDR_TRACE_JOB_QUEUED
	    && !listcount(job->requests) && !job->retain_for_poll) {
		job->suppress_publication = true;
		job->drain_complete = true;
		midr_trace_job_finalize(job);
	}
	return true;
}

enum midr_trace_cache_lookup_rc midr_trace_cache_lookup(
	const struct prefix *target,
	const struct midr_trace_request_options *options,
	struct midr_trace_query_view *view, uint32_t *cache_age_msec)
{
	struct midr_trace_scheduler *scheduler = midr_trace_scheduler;
	struct midr_trace_cache_entry *entry;
	struct midr_trace_key key;

	if (cache_age_msec)
		*cache_age_msec = 0;
	if (!midr_ip2asn_is_loaded())
		return MIDR_TRACE_LOOKUP_NO_SNAPSHOT;
	if (!scheduler || !target || !view
	    || !midr_trace_target_normalize(target, &key.target))
		return MIDR_TRACE_LOOKUP_MISS;
	key.profile_version = MIDR_TRACE_PROFILE_VERSION;
	if (options && options->force_refresh)
		return MIDR_TRACE_LOOKUP_MISS;

	entry = midr_trace_cache_find(scheduler, &key);
	if (!entry) {
		scheduler->stats.cache_misses++;
		return MIDR_TRACE_LOOKUP_MISS;
	}

	midr_trace_map_ip2asn(&entry->result, view);
	if (cache_age_msec)
		*cache_age_msec =
			midr_trace_age_msec(&entry->completed_at);
	scheduler->stats.cache_hits++;
	return MIDR_TRACE_LOOKUP_HIT;
}

enum midr_trace_submit_rc midr_trace_ensure_job(
	const struct prefix *target,
	const struct midr_trace_request_options *options, uint64_t *job_id,
	enum midr_trace_ensure_state *state)
{
	struct midr_trace_scheduler *scheduler = midr_trace_scheduler;
	struct midr_trace_cache_entry *cache = NULL;
	struct midr_trace_job *job;
	struct midr_trace_key key;
	enum midr_trace_submit_rc rc;
	bool force_refresh = options && options->force_refresh;
	uint64_t new_job_id;

	if (job_id)
		*job_id = 0;
	if (!job_id || !state)
		return MIDR_TRACE_SUBMIT_INVALID;
	rc = midr_trace_submit_precheck(target, &key);
	if (rc != MIDR_TRACE_SUBMIT_ACCEPTED)
		return rc;

	if (!force_refresh)
		cache = midr_trace_cache_find(scheduler, &key);
	if (cache) {
		*state = MIDR_TRACE_ENSURE_CACHE_HIT;
		scheduler->stats.cache_hits++;
		return MIDR_TRACE_SUBMIT_ACCEPTED;
	}

	job = midr_trace_inflight_find(scheduler, &key);
	if (job) {
		job->retain_for_poll = true;
		*job_id = job->job_id;
		*state = job->state == MIDR_TRACE_JOB_QUEUED
				 ? MIDR_TRACE_ENSURE_QUEUED
				 : MIDR_TRACE_ENSURE_RUNNING;
		scheduler->stats.coalesced++;
		return MIDR_TRACE_SUBMIT_ACCEPTED;
	}

	if (scheduler->active_slot_count >= scheduler->config.concurrency
	    && listcount(scheduler->queued_jobs)
		       >= scheduler->config.queue_limit) {
		scheduler->stats.queue_full++;
		return MIDR_TRACE_SUBMIT_QUEUE_FULL;
	}
	if (!scheduler->next_job_id)
		return MIDR_TRACE_SUBMIT_ID_EXHAUSTED;
	new_job_id = midr_trace_next_id(&scheduler->next_job_id);
	job = midr_trace_job_new(scheduler, &key, new_job_id, true);
	midr_trace_job_index(scheduler, job);
	scheduler->stats.submitted++;
	midr_trace_job_start_or_queue(job);
	*job_id = new_job_id;
	*state = job->state == MIDR_TRACE_JOB_QUEUED
			 ? MIDR_TRACE_ENSURE_QUEUED
			 : MIDR_TRACE_ENSURE_RUNNING;
	scheduler->stats.cache_misses++;
	midr_trace_schedule_pump(scheduler);
	return MIDR_TRACE_SUBMIT_ACCEPTED;
}

enum midr_trace_job_query_state midr_trace_job_lookup(
	uint64_t job_id, struct midr_trace_job_snapshot *snapshot)
{
	struct midr_trace_scheduler *scheduler = midr_trace_scheduler;
	struct midr_trace_job *job;
	struct midr_trace_history_entry lookup = {
		.job_id = job_id,
	};
	struct midr_trace_history_entry *history;
	struct listnode *node;

	if (!scheduler || !job_id || !snapshot)
		return MIDR_TRACE_QUERY_NOT_FOUND_OR_EXPIRED;
	memset(snapshot, 0, sizeof(*snapshot));
	midr_trace_history_prune(scheduler);

	job = midr_trace_active_job_find(scheduler, job_id);
	if (job) {
		snapshot->job_id = job_id;
		snapshot->state = job->state;
		snapshot->target = job->key.target;
		return job->state == MIDR_TRACE_JOB_QUEUED
			       ? MIDR_TRACE_QUERY_QUEUED
			       : MIDR_TRACE_QUERY_RUNNING;
	}

	history = hash_lookup(scheduler->recent_jobs_by_id, &lookup);
	if (!history)
		return MIDR_TRACE_QUERY_NOT_FOUND_OR_EXPIRED;
	node = listnode_lookup(scheduler->history_lru, history);
	if (node)
		listnode_move_to_tail(scheduler->history_lru, node);
	snapshot->job_id = job_id;
	snapshot->state = MIDR_TRACE_JOB_DONE;
	snapshot->target = history->result.target;
	snapshot->has_result = true;
	midr_trace_map_ip2asn(&history->result, &snapshot->view);
	return MIDR_TRACE_QUERY_COMPLETED;
}

int midr_trace_scheduler_init(struct event_loop *master)
{
	struct midr_trace_scheduler *scheduler;

	if (midr_trace_scheduler)
		return EALREADY;
	if (!master)
		return EINVAL;
	if (!midr_trace_exec_supported())
		return ENOTSUP;

	scheduler =
		XCALLOC(MTYPE_MIDR_TRACE_SCHEDULER, sizeof(*scheduler));
	scheduler->master = master;
	scheduler->config = midr_trace_default_config();
	scheduler->next_request_id = 1;
	scheduler->next_job_id = 1;
	scheduler->queued_jobs = list_new();
	scheduler->cache_lru = list_new();
	scheduler->history_lru = list_new();
	scheduler->inflight_by_key =
		hash_create(midr_trace_job_key_hash,
			    midr_trace_job_key_cmp,
			    "MIDR traceroute inflight");
	scheduler->active_jobs_by_id =
		hash_create(midr_trace_job_id_hash,
			    midr_trace_job_id_cmp,
			    "MIDR traceroute active jobs");
	scheduler->request_index =
		hash_create(midr_trace_request_hash,
			    midr_trace_request_cmp,
			    "MIDR traceroute requests");
	scheduler->cache_by_key =
		hash_create(midr_trace_cache_hash,
			    midr_trace_cache_cmp,
			    "MIDR traceroute cache");
	scheduler->recent_jobs_by_id =
		hash_create(midr_trace_history_hash,
			    midr_trace_history_cmp,
			    "MIDR traceroute history");
	scheduler->accepting = true;
	midr_trace_scheduler = scheduler;
	event_add_timer_msec(master, midr_trace_scheduler_cleanup, scheduler,
			     MIDR_TRACE_CLEANUP_INTERVAL_MSEC,
			     &scheduler->cleanup_event);
	return 0;
}

bool midr_trace_scheduler_is_ready(void)
{
	return midr_trace_scheduler != NULL;
}

void midr_trace_scheduler_sigchld(void)
{
	if (!midr_trace_scheduler)
		return;
	midr_trace_exec_sigchld();
}

void midr_trace_scheduler_config_get(
	struct midr_trace_scheduler_config *config)
{
	if (!config)
		return;
	*config = midr_trace_scheduler
			  ? midr_trace_scheduler->config
			  : midr_trace_default_config();
}

int midr_trace_scheduler_config_set(
	const struct midr_trace_scheduler_config *config, char *errmsg,
	size_t errmsg_len)
{
	struct midr_trace_scheduler *scheduler = midr_trace_scheduler;
	struct midr_trace_cache_entry *cache;
	struct midr_trace_history_entry *history;
	bool cache_ttl_changed;
	int rc;

	rc = midr_trace_config_validate(config, errmsg, errmsg_len);
	if (rc)
		return rc;
	if (!scheduler) {
		midr_trace_set_error(errmsg, errmsg_len,
				     "traceroute scheduler is not initialized");
		return ENODEV;
	}
	cache_ttl_changed =
		config->success_cache_ttl_msec
			!= scheduler->config.success_cache_ttl_msec
		|| config->negative_cache_ttl_msec
			   != scheduler->config.negative_cache_ttl_msec;
	scheduler->config = *config;

	/*
	 * Entries remember the TTL that applied when they were published.
	 * Invalidating on a TTL transition makes a configured zero take effect
	 * immediately and avoids retaining data beyond a newly shortened TTL.
	 */
	if (cache_ttl_changed)
		(void)midr_trace_cache_clear(NULL);
	while (listcount(scheduler->cache_lru)
	       > scheduler->config.cache_capacity) {
		cache = listnode_head(scheduler->cache_lru);
		midr_trace_cache_remove(scheduler, cache);
		scheduler->stats.evicted++;
	}
	while (listcount(scheduler->history_lru)
	       > scheduler->config.history_capacity) {
		history = listnode_head(scheduler->history_lru);
		midr_trace_history_remove(scheduler, history);
	}
	midr_trace_schedule_pump(scheduler);
	return 0;
}

int midr_trace_scheduler_config_write(struct vty *vty)
{
	struct midr_trace_scheduler_config config;
	struct midr_trace_scheduler_config defaults =
		midr_trace_default_config();
	int written = 0;

	if (!vty)
		return 0;
	midr_trace_scheduler_config_get(&config);
#define MIDR_TRACE_WRITE_CONFIG(_field, _format, ...)                         \
	do {                                                                  \
		if (config._field != defaults._field) {                        \
			vty_out(vty, _format, __VA_ARGS__);                    \
			written = 1;                                           \
		}                                                             \
	} while (0)
	MIDR_TRACE_WRITE_CONFIG(concurrency,
				"midr traceroute concurrency %u\n",
				config.concurrency);
	MIDR_TRACE_WRITE_CONFIG(queue_limit,
				"midr traceroute queue-limit %u\n",
				config.queue_limit);
	MIDR_TRACE_WRITE_CONFIG(queue_timeout_msec,
				"midr traceroute queue-timeout %u\n",
				config.queue_timeout_msec / 1000U);
	MIDR_TRACE_WRITE_CONFIG(execution_timeout_msec,
				"midr traceroute timeout %u\n",
				config.execution_timeout_msec / 1000U);
	MIDR_TRACE_WRITE_CONFIG(success_cache_ttl_msec,
				"midr traceroute cache ttl %u\n",
				config.success_cache_ttl_msec / 1000U);
	MIDR_TRACE_WRITE_CONFIG(negative_cache_ttl_msec,
				"midr traceroute cache negative-ttl %u\n",
				config.negative_cache_ttl_msec / 1000U);
	MIDR_TRACE_WRITE_CONFIG(cache_capacity,
				"midr traceroute cache max-entries %u\n",
				config.cache_capacity);
#undef MIDR_TRACE_WRITE_CONFIG
	return written;
}

void midr_trace_scheduler_stats_get(
	struct midr_trace_scheduler_stats *stats)
{
	struct midr_trace_scheduler *scheduler = midr_trace_scheduler;

	if (!stats)
		return;
	memset(stats, 0, sizeof(*stats));
	if (!scheduler)
		return;
	midr_trace_cache_prune(scheduler);
	midr_trace_history_prune(scheduler);
	*stats = scheduler->stats;
	stats->accepting = scheduler->accepting;
	stats->concurrency = scheduler->config.concurrency;
	stats->active_slots = scheduler->active_slot_count;
	stats->queued_jobs = listcount(scheduler->queued_jobs);
	stats->inflight_jobs = hashcount(scheduler->inflight_by_key);
	stats->pending_requests = scheduler->pending_request_count;
	stats->cache_entries = hashcount(scheduler->cache_by_key);
	stats->history_entries = hashcount(scheduler->recent_jobs_by_id);
}

unsigned int midr_trace_cache_clear(const struct prefix *target)
{
	struct midr_trace_scheduler *scheduler = midr_trace_scheduler;
	struct listnode *node;
	struct listnode *next;
	struct midr_trace_cache_entry *entry;
	struct midr_trace_key key;
	unsigned int removed = 0;

	if (!scheduler)
		return 0;
	if (target) {
		if (!midr_trace_target_normalize(target, &key.target))
			return 0;
		key.profile_version = MIDR_TRACE_PROFILE_VERSION;
		entry = midr_trace_cache_find(scheduler, &key);
		if (!entry)
			return 0;
		midr_trace_cache_remove(scheduler, entry);
		return 1;
	}

	for (ALL_LIST_ELEMENTS(scheduler->cache_lru, node, next, entry)) {
		midr_trace_cache_remove(scheduler, entry);
		removed++;
	}
	return removed;
}

void midr_trace_scheduler_quiesce(void)
{
	struct midr_trace_scheduler *scheduler = midr_trace_scheduler;
	struct list *request_snapshot;
	struct list *delivery_list;
	struct list *job_snapshot;
	struct listnode *node;
	struct listnode *next;
	struct midr_trace_request *request;
	struct midr_trace_job *job;
	bool run_deferred_fini;

	if (!scheduler)
		return;
	if (scheduler->quiescing)
		return;
	scheduler->quiescing = true;
	scheduler->accepting = false;
	event_cancel_event(scheduler->master, scheduler);

	request_snapshot = hash_to_list(scheduler->request_index);
	delivery_list = list_new();
	for (ALL_LIST_ELEMENTS_RO(request_snapshot, node, request)) {
		if (request->state == MIDR_TRACE_REQUEST_PENDING) {
			struct midr_trace_delivery_seed seed = {
				.kind = MIDR_TRACE_DELIVER_SHUTDOWN,
			};

			midr_trace_request_claim_terminal(request, &seed);
		}
		midr_trace_request_detach_job(request);
		event_cancel_event(scheduler->master, request);
		if (hash_release(scheduler->request_index, request)
		    && scheduler->pending_request_count)
			scheduler->pending_request_count--;
		listnode_add(delivery_list, request);
	}
	list_delete(&request_snapshot);

	for (ALL_LIST_ELEMENTS(delivery_list, node, next, request)) {
		list_delete_node(delivery_list, node);
		if (request->state
		    == MIDR_TRACE_REQUEST_CALLBACK_QUEUED)
			midr_trace_request_deliver(request, false);
	}
	list_delete(&delivery_list);

	job_snapshot = hash_to_list(scheduler->active_jobs_by_id);
	for (ALL_LIST_ELEMENTS_RO(job_snapshot, node, job)) {
		job->suppress_publication = true;
		if (!job->exec.spawned) {
			job->termination_reason =
				MIDR_TRACE_TERM_SHUTDOWN;
			job->terminal_status =
				MIDR_TRACE_ERR_SHUTDOWN;
			job->drain_complete = true;
			midr_trace_job_finalize(job);
			continue;
		}
		midr_trace_job_begin_termination(
			job, MIDR_TRACE_TERM_SHUTDOWN);
	}
	list_delete(&job_snapshot);

	scheduler->quiescing = false;
	run_deferred_fini =
		scheduler->fini_deferred && !scheduler->finalizing;
	scheduler->fini_deferred = false;
	if (run_deferred_fini)
		midr_trace_scheduler_fini();
}

static bool
midr_trace_job_reap_for_fini(struct midr_trace_job *job,
			     const struct timeval *teardown_started)
{
	enum midr_trace_exec_reap_rc reap_rc;

	for (;;) {
		struct timespec pause = {
			.tv_sec = 0,
			.tv_nsec = MIDR_TRACE_FINAL_REAP_POLL_NSEC,
		};

		reap_rc = midr_trace_exec_reap_one(&job->exec);
		if (reap_rc == MIDR_TRACE_EXEC_REAP_DONE)
			return true;
		if (reap_rc == MIDR_TRACE_EXEC_REAP_ERROR) {
			zlog_err(
				"MIDR traceroute job %ju: final waitpid failed: %s",
				(uintmax_t)job->job_id,
				safe_strerror(errno));
			return false;
		}

		(void)midr_trace_exec_signal(&job->exec, SIGKILL);

		if (midr_trace_age_msec(teardown_started)
		    >= MIDR_TRACE_FINAL_REAP_TIMEOUT_MSEC)
			break;
		while (nanosleep(&pause, &pause) != 0 && errno == EINTR)
			;
	}

	reap_rc = midr_trace_exec_reap_one(&job->exec);
	if (reap_rc == MIDR_TRACE_EXEC_REAP_DONE)
		return true;
	zlog_err(
		"MIDR traceroute job %ju: child did not exit within %u ms after SIGKILL",
		(uintmax_t)job->job_id, MIDR_TRACE_FINAL_REAP_TIMEOUT_MSEC);
	return false;
}

void midr_trace_scheduler_fini(void)
{
	struct midr_trace_scheduler *scheduler = midr_trace_scheduler;
	struct list *jobs;
	struct listnode *node;
	struct midr_trace_job *job;
	struct timeval teardown_started;
	sigset_t block_mask;
	sigset_t old_mask;
	bool mask_blocked = false;
	bool reap_failed = false;
	int rc;

	if (!scheduler)
		return;
	if (scheduler->quiescing) {
		scheduler->fini_deferred = true;
		return;
	}
	if (scheduler->finalizing)
		return;
	scheduler->finalizing = true;
	midr_trace_scheduler_quiesce();

	sigemptyset(&block_mask);
	sigaddset(&block_mask, SIGCHLD);
	rc = pthread_sigmask(SIG_BLOCK, &block_mask, &old_mask);
	if (!rc)
		mask_blocked = true;
	else
		zlog_err("MIDR traceroute: cannot block SIGCHLD for teardown: %s",
			 safe_strerror(rc));

	jobs = hash_to_list(scheduler->active_jobs_by_id);
	monotime(&teardown_started);
	for (ALL_LIST_ELEMENTS_RO(jobs, node, job)) {
		int signal_rc;

		event_cancel_event(scheduler->master, job);
		job->suppress_publication = true;
		if (!job->exec.spawned || job->exec.child_reaped)
			continue;
		signal_rc = midr_trace_exec_signal(&job->exec, SIGKILL);
		if (signal_rc && signal_rc != ESRCH)
			zlog_err(
				"MIDR traceroute job %ju: teardown SIGKILL failed: %s",
				(uintmax_t)job->job_id,
				safe_strerror(signal_rc));
		midr_trace_exec_close_streams(&job->exec);
	}
	for (ALL_LIST_ELEMENTS_RO(jobs, node, job)) {
		if (job->exec.spawned && !job->exec.child_reaped
		    && !midr_trace_job_reap_for_fini(
			    job, &teardown_started)) {
			reap_failed = true;
			continue;
		}
		job->termination_reason = MIDR_TRACE_TERM_SHUTDOWN;
		job->terminal_status = MIDR_TRACE_ERR_SHUTDOWN;
		midr_trace_exec_close_streams(&job->exec);
		job->stdout_eof = true;
		job->stderr_eof = true;
		job->drain_complete = true;
		midr_trace_job_finalize(job);
	}
	list_delete(&jobs);

	if (reap_failed) {
		/*
		 * Never free a job still referenced by the PID registry.  The
		 * daemon's caller is in its final process-exit path, so make late
		 * SIGCHLD dispatch a no-op and intentionally retain this bounded
		 * teardown state until process exit instead of hanging forever.
		 */
		midr_trace_scheduler = NULL;
		scheduler->finalizing = false;
		if (mask_blocked)
			pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
		return;
	}

	event_cancel_event(scheduler->master, scheduler);
	list_delete(&scheduler->queued_jobs);
	list_delete(&scheduler->cache_lru);
	list_delete(&scheduler->history_lru);
	hash_clean_and_free(&scheduler->cache_by_key,
			    midr_trace_cache_entry_free);
	hash_clean_and_free(&scheduler->recent_jobs_by_id,
			    midr_trace_history_entry_free);
	hash_clean_and_free(&scheduler->request_index, NULL);
	hash_clean_and_free(&scheduler->inflight_by_key, NULL);
	hash_clean_and_free(&scheduler->active_jobs_by_id, NULL);
	midr_trace_scheduler = NULL;
	XFREE(MTYPE_MIDR_TRACE_SCHEDULER, scheduler);
	if (mask_blocked)
		pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
}

const char *midr_trace_status_name(enum midr_trace_status status)
{
	switch (status) {
	case MIDR_TRACE_OK:
		return "ok";
	case MIDR_TRACE_ERR_INVALID:
		return "invalid";
	case MIDR_TRACE_ERR_NO_SNAPSHOT:
		return "no-snapshot";
	case MIDR_TRACE_ERR_UNSUPPORTED:
		return "unsupported";
	case MIDR_TRACE_ERR_QUEUE_FULL:
		return "queue-full";
	case MIDR_TRACE_ERR_REQUEST_LIMIT:
		return "request-limit";
	case MIDR_TRACE_ERR_QUEUE_TIMEOUT:
		return "queue-timeout";
	case MIDR_TRACE_ERR_SPAWN:
		return "spawn-error";
	case MIDR_TRACE_ERR_EXEC_TIMEOUT:
		return "timeout";
	case MIDR_TRACE_ERR_OUTPUT_LIMIT:
		return "output-limit";
	case MIDR_TRACE_ERR_PARSE:
		return "parse-error";
	case MIDR_TRACE_ERR_EXIT_STATUS:
		return "exit-status";
	case MIDR_TRACE_ERR_CANCELED:
		return "canceled";
	case MIDR_TRACE_ERR_SHUTDOWN:
		return "shutdown";
	}
	return "unknown";
}

const char *midr_trace_cache_state_name(enum midr_trace_cache_state state)
{
	switch (state) {
	case MIDR_TRACE_CACHE_MISS:
		return "miss";
	case MIDR_TRACE_CACHE_HIT:
		return "hit";
	case MIDR_TRACE_CACHE_NEGATIVE_HIT:
		return "negative-hit";
	case MIDR_TRACE_CACHE_COALESCED:
		return "coalesced";
	case MIDR_TRACE_CACHE_REFRESH:
		return "refresh";
	}
	return "unknown";
}

static enum midr_trace_status midr_trace_job_status(
	const struct midr_trace_job *job)
{
	switch (job->termination_reason) {
	case MIDR_TRACE_TERM_TIMEOUT:
		return MIDR_TRACE_ERR_EXEC_TIMEOUT;
	case MIDR_TRACE_TERM_OUTPUT_LIMIT:
		return MIDR_TRACE_ERR_OUTPUT_LIMIT;
	case MIDR_TRACE_TERM_PARSE:
		return MIDR_TRACE_ERR_PARSE;
	case MIDR_TRACE_TERM_SHUTDOWN:
		return MIDR_TRACE_ERR_SHUTDOWN;
	case MIDR_TRACE_TERM_NONE:
		break;
	}

	if (!job->exec.wait_status_valid
	    || job->exec.reap_consistency_error)
		return MIDR_TRACE_ERR_EXIT_STATUS;
	if (!WIFEXITED(job->exec.wait_status)
	    || WEXITSTATUS(job->exec.wait_status) != 0)
		return MIDR_TRACE_ERR_EXIT_STATUS;
	if (!job->parser_finished || job->parser.path.hop_count == 0)
		return MIDR_TRACE_ERR_PARSE;
	return MIDR_TRACE_OK;
}

static void midr_trace_job_record_result_stats(
	struct midr_trace_scheduler *scheduler,
	enum midr_trace_status status)
{
	switch (status) {
	case MIDR_TRACE_ERR_QUEUE_TIMEOUT:
		scheduler->stats.queue_timeouts++;
		break;
	case MIDR_TRACE_ERR_EXEC_TIMEOUT:
		scheduler->stats.execution_timeouts++;
		break;
	case MIDR_TRACE_ERR_SPAWN:
		scheduler->stats.spawn_errors++;
		break;
	case MIDR_TRACE_ERR_PARSE:
		scheduler->stats.parse_errors++;
		break;
	case MIDR_TRACE_ERR_EXIT_STATUS:
		scheduler->stats.exit_errors++;
		break;
	case MIDR_TRACE_ERR_OUTPUT_LIMIT:
		scheduler->stats.output_limit_errors++;
		break;
	default:
		break;
	}
}

static void midr_trace_job_build_result(
	struct midr_trace_job *job, struct midr_trace_job_result *result)
{
	struct timeval now;

	memset(result, 0, sizeof(*result));
	monotime(&now);
	result->job_id = job->job_id;
	result->status = job->terminal_status;
	result->target = job->key.target;
	result->raw_path = job->parser.path;
	if (job->start_attempted) {
		result->queue_msec = midr_trace_elapsed_msec(
			&job->queued_at, &job->started_at);
		if (job->exec.spawned)
			result->execution_msec = midr_trace_elapsed_msec(
				&job->started_at, &now);
	} else {
		result->queue_msec =
			midr_trace_elapsed_msec(&job->queued_at, &now);
	}

	result->has_wait_status = job->exec.wait_status_valid;
	if (!result->has_wait_status)
		return;
	result->exited_normally = WIFEXITED(job->exec.wait_status);
	if (result->exited_normally)
		result->child_exit_code = WEXITSTATUS(job->exec.wait_status);
	else if (WIFSIGNALED(job->exec.wait_status))
		result->child_signal = WTERMSIG(job->exec.wait_status);
}

static void midr_trace_job_finalize(struct midr_trace_job *job)
{
	struct midr_trace_scheduler *scheduler;
	struct midr_trace_job_result result;
	struct listnode *node;
	struct listnode *next;
	struct midr_trace_request *request;
	bool publish;

	if (!job || job->finalize_started)
		return;
	if (job->exec.spawned
	    && (!job->exec.child_reaped || !job->drain_complete))
		return;

	job->finalize_started = true;
	job->state = MIDR_TRACE_JOB_DONE;
	scheduler = job->scheduler;
	event_cancel_event(scheduler->master, job);
	if (listnode_lookup(scheduler->queued_jobs, job))
		listnode_delete(scheduler->queued_jobs, job);

	hash_release(scheduler->inflight_by_key, job);
	hash_release(scheduler->active_jobs_by_id, job);
	if (job->slot_owned) {
		job->slot_owned = false;
		if (scheduler->active_slot_count)
			scheduler->active_slot_count--;
	}

	publish = !job->suppress_publication;
	if (publish) {
		if (job->exec.spawned)
			job->terminal_status = midr_trace_job_status(job);
		midr_trace_job_build_result(job, &result);
		job->result_published = true;
		midr_trace_cache_insert(scheduler, &job->key, &result);
		midr_trace_history_insert(scheduler, &result);
		midr_trace_job_record_result_stats(scheduler, result.status);
		scheduler->stats.completed++;
	}

	for (ALL_LIST_ELEMENTS(job->requests, node, next, request)) {
		struct midr_trace_delivery_seed seed = {
			.kind = MIDR_TRACE_DELIVER_JOB_RESULT,
			.cache_state = request->source,
			.cache_age_msec = 0,
			.has_job_result = publish,
		};

		if (publish)
			seed.job_result = result;
		midr_trace_request_claim_terminal(request, &seed);
	}
	list_delete(&job->requests);
	midr_trace_exec_reset(&job->exec);
	XFREE(MTYPE_MIDR_TRACE_JOB, job);
	midr_trace_schedule_pump(scheduler);
}

static void midr_trace_job_kill_timeout(struct event *event)
{
	struct midr_trace_job *job = EVENT_ARG(event);
	int signal_rc;

	if (!job || job->finalize_started || job->exec.child_reaped)
		return;
	if (job->state != MIDR_TRACE_JOB_TERM_SENT)
		return;
	signal_rc = midr_trace_exec_signal(&job->exec, SIGKILL);
	if (signal_rc && signal_rc != ESRCH)
		zlog_err("MIDR traceroute job %ju: SIGKILL failed: %s",
			 (uintmax_t)job->job_id,
			 safe_strerror(signal_rc));
	job->state = MIDR_TRACE_JOB_KILL_SENT;
}

static void midr_trace_job_begin_termination(
	struct midr_trace_job *job,
	enum midr_trace_termination_reason reason)
{
	if (!job || job->finalize_started)
		return;
	if (reason == MIDR_TRACE_TERM_SHUTDOWN)
		job->suppress_publication = true;
	if (job->termination_reason == MIDR_TRACE_TERM_NONE)
		job->termination_reason = reason;

	event_cancel(&job->execution_timer);
	if (!job->exec.spawned) {
		job->terminal_status = reason == MIDR_TRACE_TERM_SHUTDOWN
					       ? MIDR_TRACE_ERR_SHUTDOWN
					       : MIDR_TRACE_ERR_PARSE;
		job->drain_complete = true;
		midr_trace_job_finalize(job);
		return;
	}
	if (job->exec.child_reaped)
		return;
	if (job->state != MIDR_TRACE_JOB_TERM_SENT
	    && job->state != MIDR_TRACE_JOB_KILL_SENT) {
		(void)midr_trace_exec_signal(&job->exec, SIGTERM);
		job->state = MIDR_TRACE_JOB_TERM_SENT;
	}
	if (!job->kill_timer)
		event_add_timer_msec(
			job->scheduler->master,
			midr_trace_job_kill_timeout, job,
			job->scheduler->config.kill_grace_msec,
			&job->kill_timer);
}

static void midr_trace_job_execution_timeout(struct event *event)
{
	struct midr_trace_job *job = EVENT_ARG(event);

	if (!job || job->finalize_started
	    || job->state != MIDR_TRACE_JOB_RUNNING
	    || job->exec.child_reaped
	    || job->termination_reason != MIDR_TRACE_TERM_NONE)
		return;
	midr_trace_job_begin_termination(job, MIDR_TRACE_TERM_TIMEOUT);
}

static void midr_trace_job_final_drain_timeout(struct event *event)
{
	struct midr_trace_job *job = EVENT_ARG(event);

	if (!job || job->finalize_started
	    || job->state != MIDR_TRACE_JOB_FINALIZING
	    || !job->exec.child_reaped)
		return;
	midr_trace_job_close_stream(job, MIDR_TRACE_EXEC_STDOUT);
	midr_trace_job_close_stream(job, MIDR_TRACE_EXEC_STDERR);
	job->drain_complete = true;
	midr_trace_job_finalize(job);
}

static void midr_trace_job_reap_safety(struct event *event)
{
	struct midr_trace_job *job = EVENT_ARG(event);
	enum midr_trace_exec_reap_rc reap_rc;
	int signal_rc;

	if (!job || job->finalize_started || job->exec.child_reaped)
		return;
	reap_rc = midr_trace_exec_reap_one(&job->exec);
	if (reap_rc == MIDR_TRACE_EXEC_REAP_DONE) {
		midr_trace_job_reaped(job);
		return;
	}
	if (reap_rc == MIDR_TRACE_EXEC_REAP_ERROR)
		zlog_warn("MIDR traceroute job %ju: safety waitpid failed: %s",
			  (uintmax_t)job->job_id, safe_strerror(errno));
	if (job->state == MIDR_TRACE_JOB_KILL_SENT) {
		signal_rc = midr_trace_exec_signal(&job->exec, SIGKILL);
		if (signal_rc && signal_rc != ESRCH)
			zlog_err(
				"MIDR traceroute job %ju: repeated SIGKILL failed: %s",
				(uintmax_t)job->job_id,
				safe_strerror(signal_rc));
	}
	event_add_timer_msec(job->scheduler->master,
			     midr_trace_job_reap_safety, job,
			     MIDR_TRACE_REAP_SAFETY_MSEC,
			     &job->reap_safety_timer);
}

static void midr_trace_job_reaped(void *arg)
{
	struct midr_trace_job *job = arg;

	if (!job || job->finalize_started || !job->exec.child_reaped)
		return;

	event_cancel(&job->execution_timer);
	event_cancel(&job->kill_timer);
	event_cancel(&job->reap_safety_timer);
	job->state = MIDR_TRACE_JOB_FINALIZING;
	job->defer_finalize = true;
	midr_trace_job_read_stream(job, MIDR_TRACE_EXEC_STDOUT);
	midr_trace_job_read_stream(job, MIDR_TRACE_EXEC_STDERR);
	job->defer_finalize = false;

	if (job->stdout_eof && job->stderr_eof) {
		job->drain_complete = true;
		midr_trace_job_finalize(job);
		return;
	}
	if (!job->final_drain_timer)
		event_add_timer_msec(
			job->scheduler->master,
			midr_trace_job_final_drain_timeout, job,
			job->scheduler->config.post_exit_drain_msec,
			&job->final_drain_timer);
}

static void midr_trace_job_maybe_finish_drain(struct midr_trace_job *job)
{
	if (!job->exec.child_reaped || job->defer_finalize)
		return;
	if (!job->stdout_eof || !job->stderr_eof)
		return;
	job->drain_complete = true;
	midr_trace_job_finalize(job);
}

static void midr_trace_job_start_event(struct event *event)
{
	struct midr_trace_job *job = EVENT_ARG(event);
	struct midr_trace_scheduler *scheduler;
	int rc;

	if (!job || job->finalize_started
	    || job->state != MIDR_TRACE_JOB_STARTING)
		return;
	scheduler = job->scheduler;
	if (!scheduler->accepting) {
		job->suppress_publication = true;
		job->terminal_status = MIDR_TRACE_ERR_SHUTDOWN;
		job->drain_complete = true;
		midr_trace_job_finalize(job);
		return;
	}

	job->start_attempted = true;
	monotime(&job->started_at);
	rc = midr_trace_exec_start(&job->exec, &job->key.target, job,
				   midr_trace_job_reaped);
	if (rc) {
		job->terminal_status = MIDR_TRACE_ERR_SPAWN;
		job->drain_complete = true;
		midr_trace_job_finalize(job);
		return;
	}

	job->state = MIDR_TRACE_JOB_RUNNING;
	midr_trace_job_schedule_read(job, MIDR_TRACE_EXEC_STDOUT);
	midr_trace_job_schedule_read(job, MIDR_TRACE_EXEC_STDERR);
	event_add_timer_msec(scheduler->master,
			     midr_trace_job_execution_timeout, job,
			     scheduler->config.execution_timeout_msec,
			     &job->execution_timer);
	event_add_timer_msec(scheduler->master,
			     midr_trace_job_reap_safety, job,
			     MIDR_TRACE_REAP_SAFETY_MSEC,
			     &job->reap_safety_timer);
}

static void midr_trace_job_queue_timeout(struct event *event)
{
	struct midr_trace_job *job = EVENT_ARG(event);

	if (!job || job->finalize_started
	    || job->state != MIDR_TRACE_JOB_QUEUED)
		return;
	listnode_delete(job->scheduler->queued_jobs, job);
	job->terminal_status = MIDR_TRACE_ERR_QUEUE_TIMEOUT;
	job->drain_complete = true;
	midr_trace_job_finalize(job);
}

static void midr_trace_job_read_stream(
	struct midr_trace_job *job, enum midr_trace_exec_stream stream)
{
	unsigned char buf[MIDR_TRACE_READ_BUFFER_SIZE];

	while (midr_trace_exec_stream_fd(&job->exec, stream) >= 0) {
		ssize_t nread =
			midr_trace_exec_read(&job->exec, stream, buf,
					     sizeof(buf));

		if (nread > 0) {
			enum midr_trace_parse_rc parse_rc;
			uint64_t limit =
				job->scheduler->config.output_limit_bytes;

			if ((uint64_t)nread > limit - MIN(limit,
							 job->output_bytes)) {
				job->parser.path.output_truncated = true;
				midr_trace_job_begin_termination(
					job,
					MIDR_TRACE_TERM_OUTPUT_LIMIT);
				midr_trace_job_close_stream(
					job, MIDR_TRACE_EXEC_STDOUT);
				midr_trace_job_close_stream(
					job, MIDR_TRACE_EXEC_STDERR);
				break;
			}
			job->output_bytes += (uint64_t)nread;
			if (stream == MIDR_TRACE_EXEC_STDERR
			    || job->termination_reason
				       != MIDR_TRACE_TERM_NONE)
				continue;

			parse_rc = midr_trace_parser_feed(&job->parser, buf,
							  (size_t)nread);
			if (parse_rc == MIDR_TRACE_PARSE_OK)
				continue;
			if (parse_rc == MIDR_TRACE_PARSE_OUTPUT_LIMIT) {
				job->parser.path.output_truncated = true;
				midr_trace_job_begin_termination(
					job,
					MIDR_TRACE_TERM_OUTPUT_LIMIT);
				midr_trace_job_close_stream(
					job, MIDR_TRACE_EXEC_STDOUT);
				midr_trace_job_close_stream(
					job, MIDR_TRACE_EXEC_STDERR);
			} else {
				midr_trace_job_begin_termination(
					job, MIDR_TRACE_TERM_PARSE);
			}
			break;
		}
		if (nread == 0) {
			midr_trace_job_close_stream(job, stream);
			break;
		}
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			midr_trace_job_schedule_read(job, stream);
			break;
		}

		midr_trace_job_close_stream(job, stream);
		midr_trace_job_begin_termination(job,
						 MIDR_TRACE_TERM_PARSE);
		break;
	}

	midr_trace_job_maybe_finish_drain(job);
}

static void midr_trace_job_read_stdout(struct event *event)
{
	struct midr_trace_job *job = EVENT_ARG(event);

	if (!job || job->finalize_started)
		return;
	midr_trace_job_read_stream(job, MIDR_TRACE_EXEC_STDOUT);
}

static void midr_trace_job_read_stderr(struct event *event)
{
	struct midr_trace_job *job = EVENT_ARG(event);

	if (!job || job->finalize_started)
		return;
	midr_trace_job_read_stream(job, MIDR_TRACE_EXEC_STDERR);
}

static void midr_trace_job_schedule_read(struct midr_trace_job *job,
					 enum midr_trace_exec_stream stream)
{
	int fd = midr_trace_exec_stream_fd(&job->exec, stream);

	if (fd < 0 || job->finalize_started)
		return;
	if (stream == MIDR_TRACE_EXEC_STDOUT) {
		if (!job->read_stdout)
			event_add_read(job->scheduler->master,
				       midr_trace_job_read_stdout, job, fd,
				       &job->read_stdout);
	} else if (!job->read_stderr) {
		event_add_read(job->scheduler->master,
			       midr_trace_job_read_stderr, job, fd,
			       &job->read_stderr);
	}
}
