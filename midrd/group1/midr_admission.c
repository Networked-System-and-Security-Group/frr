// SPDX-License-Identifier: GPL-2.0-or-later
/* MIDR neighbor admission. All state and callbacks belong to the main loop. */
#include "zebra.h"
#include <inttypes.h>
#include "memory.h"
#include "frrevent.h"
#include "monotime.h"
#include "hook.h"
#include "log.h"
#include "vty.h"
#include "vrf.h"
#include "midrd/group1/midr_g1.h"
#include "midrd/group1/midr_ctrl.h"
#include "midrd/group1/midr_admission.h"
#include "midrd/group1/midr_ip2asn.h"
#include "midrd/group1/midr_tier1_list.h"
#include "midrd/group1/midr_trace_scheduler.h"

#define ADMISSION_LIMIT 128U
#define ADMISSION_MAX_AGE_MS 30000U
#define ADMISSION_DEADLINE_MS 105000U
#define ADMISSION_REMOTE_LIFETIME_MS 10000U
#define ADMISSION_RETRY_MIN_MS 5000U
#define ADMISSION_RETRY_MAX_MS 60000U
#define ADMISSION_REASON_COUNT (MIDR_SESSION_PEER_REQ_REPLY + 1)
#define ADMISSION_ANCHORS_PER_GROUP 2U
/* Candidate screening owns an intent without asking for a session, so CL can
 * rank join/anchor candidates on a path verdict before any connect. */
#define ADMISSION_SCREEN_BIT (1U << ADMISSION_REASON_COUNT)
#define ADMISSION_SESSION_MASK (ADMISSION_SCREEN_BIT - 1U)
#define ADMISSION_SCREEN_LIFETIME_MS 600000U

DEFINE_MTYPE_STATIC(MIDR_G1, MIDR_ADMISSION, "MIDR admission manager");
DEFINE_MTYPE_STATIC(MIDR_G1, MIDR_ADMISSION_ENTRY, "MIDR admission intent");
DEFINE_MTYPE_STATIC(MIDR_G1, MIDR_ADMISSION_TOKEN, "MIDR admission callback token");

enum admission_state {
	ADMISSION_WAIT_DATA,
	ADMISSION_WAIT_TRACE,
	ADMISSION_ALLOWED,
	ADMISSION_BLOCKED,
	ADMISSION_UNKNOWN,
};
struct admission_entry;
/* The scheduler owns this token until its one terminal callback, including
 * cancellation. Clearing owner before cancel makes per-instance deletion safe. */
struct admission_token {
	struct admission_entry *owner;
	uint64_t request_id;
};
struct admission_entry {
	struct midr_admission *manager;
	struct midr_node_entry target; /* only scalar identity/locator fields copied */
	struct ipaddr source;
	unsigned int local_reasons, remote_reasons, nudge_reasons;
	bool remote_attach;
	uint32_t local_group;
	uint64_t remote_seen, deadline, retry_at, measured_at;
	uint64_t ip_generation, list_generation, permit;
	uint64_t screen_until;
	unsigned int backoff;
	int retx_budget;
	enum admission_state state;
	/* Last completed trace hit Tier1; kept while a re-measurement runs. */
	bool last_blocked;
	const char *detail;
	struct midr_tier1_result result;
	struct admission_token *token;
	struct event *resume;
};
struct midr_admission {
	struct midr_g1 *g1;
	struct list *entries;
	struct event *timer;
	struct event *notify;
	struct event *node_notify;
	uint64_t cookie;
	uint64_t recover_at;
};
static struct list *managers;
static uint64_t serial;

static uint64_t now_ms(void)
{
	struct timeval tv;
	monotime(&tv);
	return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static uint64_t next_serial(void)
{
	return serial == UINT64_MAX ? 0 : ++serial;
}

static struct admission_entry *find_entry(const struct midr_g1 *g1, struct ipaddr target)
{
	struct listnode *n;
	struct admission_entry *e;
	struct midr_admission *m = g1 && g1->midr_nds_info
		? g1->midr_nds_info->admission : NULL;
	if (m)
		for (ALL_LIST_ELEMENTS_RO(m->entries, n, e))
			if (midr_ipaddr_same(&e->target.transport_addr, &target))
				return e;
	return NULL;
}

static struct midr_g1_peer *entry_peer(struct admission_entry *e)
{
	return midr_g1_peer_lookup(e->manager->g1, &e->target.transport_addr);
}

bool midr_admission_has_intent(struct midr_g1 *g1, struct ipaddr target)
{
	return find_entry(g1, target) != NULL;
}

bool midr_admission_is_manual(struct midr_g1 *g1, struct ipaddr target)
{
	struct admission_entry *e = find_entry(g1, target);
	return e && (e->local_reasons & (1U << MIDR_SESSION_MANUAL));
}

/* A cooled-down candidate must not create an extra edge after a replacement
 * filled its slot. Count committed ledgers and other in-flight reservations. */
static bool room_for(struct admission_entry *e)
{
	struct midr_admission *m = e->manager;
	struct midr_nds *mi = m->g1->midr_nds_info;
	struct listnode *n;
	struct midr_session_ledger_entry *ledger;
	struct admission_entry *other;
	enum midr_session_reason reason;
	unsigned int used = 0, limit;
	unsigned int local = e->local_reasons & ADMISSION_SESSION_MASK;
	if (e->remote_reasons || entry_peer(e))
		return true;
	if (local == (1U << MIDR_SESSION_ATTACH)) {
		reason = MIDR_SESSION_ATTACH;
		limit = MIDR_ATTACH_K;
	} else if (local == (1U << MIDR_SESSION_CL_ANCHOR)) {
		reason = MIDR_SESSION_CL_ANCHOR;
		limit = ADMISSION_ANCHORS_PER_GROUP;
	} else
		return true;
	if (mi->session_ledger)
		for (ALL_LIST_ELEMENTS_RO(mi->session_ledger, n, ledger))
			if (ledger->reason == reason &&
			    (reason == MIDR_SESSION_ATTACH ||
			     ledger->remote_group == e->target.group_id))
				used++;
	for (ALL_LIST_ELEMENTS_RO(m->entries, n, other))
		if (other != e && (other->local_reasons & (1U << reason)) &&
		    (other->token || other->resume) && !entry_peer(other) &&
		    (reason == MIDR_SESSION_ATTACH || other->target.group_id == e->target.group_id))
			used++;
	return used < limit;
}

/* Validate demand, independently of path evidence. This also covers a withdraw
 * before any peer/ledger exists, and role changes while a trace is queued. */
static bool valid_owners(struct admission_entry *e)
{
	struct midr_g1 *g1 = e->manager->g1;
	struct midr_nds *mi = g1->midr_nds_info;
	struct midr_g1_peer *peer = entry_peer(e);
	struct midr_node_entry *known = mi->global_view ?
		midr_node_hash_find(&mi->global_view->nodes, &e->target) : NULL;
	struct listnode *n;
	struct midr_bootstrap_entry *candidate;
	bool has_candidate = false;
	if ((e->local_reasons & ADMISSION_SCREEN_BIT) && now_ms() >= e->screen_until)
		e->local_reasons &= ~ADMISSION_SCREEN_BIT;
	if (!known || !known->has_transport_addr ||
	    !midr_ipaddr_same(&known->transport_addr, &e->target.transport_addr))
		e->local_reasons &= ~((1U << MIDR_SESSION_SAME_GROUP) |
				      (1U << MIDR_SESSION_CL_ANCHOR));
	if (e->local_group != mi->local_group_id ||
	    (known && known->group_id != mi->local_group_id))
		e->local_reasons &= ~(1U << MIDR_SESSION_SAME_GROUP);
	if (e->target.group_id != mi->local_group_id ||
	    (known && known->group_id != mi->local_group_id))
		e->remote_reasons &= ~(1U << MIDR_SESSION_SAME_GROUP);
	if (e->local_reasons & (1U << MIDR_SESSION_ATTACH)) {
		if (mi->bootstrap_list)
			for (ALL_LIST_ELEMENTS_RO(mi->bootstrap_list, n, candidate))
				if (!candidate->attach_failed &&
				    midr_ipaddr_same(&candidate->transport, &e->target.transport_addr)) {
					has_candidate = true;
					break;
				}
		if (!has_candidate || !(mi->local_capabilities & MIDR_CAP_GROUP_REP))
			e->local_reasons &= ~(1U << MIDR_SESSION_ATTACH);
	}
	if (midr_nds_is_session_excluded(g1, e->target.node_id.u.prefix4)) {
		e->local_reasons &= 1U << MIDR_SESSION_MANUAL;
		e->remote_reasons = 0;
	}
	/* A remote-only request is a lease, including between timer ticks.  An
	 * existing Established session is retained until its next reconnect. */
	if (!(peer && peer->established) &&
	    now_ms() - e->remote_seen > ADMISSION_REMOTE_LIFETIME_MS)
		e->remote_reasons = 0;
	return e->local_reasons || e->remote_reasons;
}

static void cancel_trace(struct admission_entry *e)
{
	struct admission_token *token = e->token;
	e->token = NULL;
	if (token) {
		token->owner = NULL;
		midr_trace_cancel(token->request_id);
	}
}

static void free_entry(struct admission_entry *e)
{
	cancel_trace(e);
	event_cancel(&e->resume);
	listnode_delete(e->manager->entries, e);
	XFREE(MTYPE_MIDR_ADMISSION_ENTRY, e);
}

static bool current_data(struct admission_entry *e)
{
	return midr_ip2asn_is_loaded() && midr_tier1_list_active() &&
	       e->ip_generation == midr_ip2asn_generation() &&
	       e->list_generation == midr_tier1_list_generation();
}

static bool current_path(struct admission_entry *e)
{
	struct midr_g1 *g1 = e->manager->g1;
	struct ipaddr local;
	return !g1->midr_nds_info->shutdown &&
	       !g1->midr_nds_info->transport_reconfiguring &&
	       (!g1->midr_nds_info->avoid_tier1 || g1->vrf_id == VRF_DEFAULT) &&
	       midr_nds_local_transport_get(g1, &local) &&
	       midr_ipaddr_same(&local, &e->source);
}

static bool fresh_permit(struct admission_entry *e)
{
	return e && valid_owners(e) && e->state == ADMISSION_ALLOWED && e->permit && current_path(e) &&
	       current_data(e) && now_ms() - e->measured_at <= ADMISSION_MAX_AGE_MS;
}

/* A requested, not yet established midrd session is the handshake that
 * holds a permit (BGP: Connect..OpenConfirm). */
static bool ongoing_permit(struct admission_entry *e)
{
	struct midr_g1_peer *peer = entry_peer(e);
	return peer && peer->requested && !peer->established &&
	       peer->admission_permit && midr_admission_check(peer);
}

/* Classify available evidence after bounded probing. Missing replies or ASN
 * mappings are not evidence of Tier1. Keep infrastructure failures separate. */
enum midr_admission_result midr_admission_evaluate(
	const struct midr_trace_job_result *job, struct midr_tier1_result *result)
{
	as_t asns[MIDR_TIER1_MAX_OBSERVED_ASNS] = {};
	size_t i;
	if (!job || !result || !midr_ip2asn_is_loaded() ||
	    !midr_tier1_list_active() || job->raw_path.hop_count > array_size(asns))
		return MIDR_ADMISSION_PENDING;
	for (i = 0; i < job->raw_path.hop_count; i++) {
		const struct midr_trace_raw_hop *h = &job->raw_path.hops[i];
		if (h->visible)
			midr_ip2asn_lookup(&h->address, &asns[i], NULL);
	}
	if (midr_tier1_active_path_check(asns, job->raw_path.hop_count, result))
		return MIDR_ADMISSION_PENDING;
	if (result->tier1_observed)
		return MIDR_ADMISSION_BLOCKED;
	/* Execution timeout retains observed hops, including exhausted '*' hops.
	 * Queue/socket/send failures and cancellation are not completed probing. */
	if (job->status == MIDR_TRACE_OK || job->status == MIDR_TRACE_ERR_EXEC_TIMEOUT)
		return MIDR_ADMISSION_READY;
	return MIDR_ADMISSION_PENDING;
}

static void notify_cl(struct event *event)
{
	struct midr_admission *m = EVENT_ARG(event);
	if (m->g1->midr_nds_info->shutdown || midr_g1_config_inprocess())
		return;
	midr_nds_attach_pick(m->g1);
	midr_nds_notify_cl(m->g1, MIDR_TRIGGER_ADMISSION_CHANGE);
}

static void changed(struct admission_entry *e)
{
	struct midr_admission *m = e->manager;
	if (!m->notify)
		event_add_event(midr_g1_master(), notify_cl, m, 0, &m->notify);
}

static void notify_node(struct event *event)
{
	struct midr_admission *m = EVENT_ARG(event);
	midr_nds_notify_cl(m->g1, MIDR_TRIGGER_NODE_CHANGE);
}

void midr_admission_committed(struct midr_g1 *g1)
{
	struct midr_admission *m = g1->midr_nds_info->admission;
	if (m && !m->node_notify)
		event_add_event(midr_g1_master(), notify_node, m, 0, &m->node_notify);
}

static void resume_connect(struct event *event)
{
	struct admission_entry *e = EVENT_ARG(event);
	(void)valid_owners(e);
	struct midr_g1 *g1 = e->manager->g1;
	struct midr_node_entry target = e->target;
	unsigned int local = e->local_reasons, remote = e->remote_reasons;
	unsigned int nudge = e->nudge_reasons;
	bool attach_request = e->remote_attach;
	struct midr_g1_peer *peer;
	int i;
	if (!local && !remote) {
		/* Remote request expired or its role disappeared. Do this outside
		 * the intent list walk, since peer deletion invokes NDS hooks. */
		peer = entry_peer(e);
		bool committed = midr_nds_ledger_lookup(g1, target.transport_addr) != NULL;
		bool established = peer && peer->established;
		midr_admission_forget(g1, target.transport_addr);
		/* Pending intent cancellation must not stop independent candidate PM.
		 * Established teardown remains owned by the existing NDS lifecycle. */
		if (established || (!committed && !peer))
			return;
		midr_ctrl_forget_target(g1, target.transport_addr);
		midr_nds_ledger_drop(g1, target.transport_addr);
		midr_nds_cleanup_by_transport(g1, target.transport_addr,
					      MIDR_STOP_KEEPALIVE_TIMEOUT);
		return;
	}
	/* A screening verdict alone never starts a session. */
	if (!(local & ADMISSION_SESSION_MASK) && !remote)
		return;
	if (midr_g1_config_inprocess() || !current_path(e))
		return;
	if (!room_for(e))
		return;
	if (g1->midr_nds_info->avoid_tier1 && !fresh_permit(e))
		return;
	/* connect may retire an intent. Work from values, never retain e across it. */
	for (i = 0; i < ADMISSION_REASON_COUNT; i++) {
		if (local & (1U << i))
			midr_ctrl_connect(g1, &target, i, !!(nudge & (1U << i)));
		else if (remote & (1U << i))
			midr_ctrl_connect_received(g1, &target, i, attach_request);
	}
	e = find_entry(g1, target.transport_addr);
	if (!e)
		return;
	peer = entry_peer(e);
	if (peer && !peer->requested)
		midr_g1_peer_start(peer);
}

static void queue_resume(struct admission_entry *e)
{
	if (!e->resume)
		event_add_event(midr_g1_master(), resume_connect, e, 0, &e->resume);
}

static void retry_later(struct admission_entry *e, const char *detail)
{
	e->permit = 0;
	e->state = ADMISSION_UNKNOWN;
	e->detail = detail;
	e->backoff = e->backoff ? MIN(e->backoff * 2, ADMISSION_RETRY_MAX_MS)
				: ADMISSION_RETRY_MIN_MS;
	/* Stable per-instance/target jitter, no shared RNG or blocking waits. */
	e->retry_at = now_ms() + e->backoff + (e->manager->cookie % 997);
	changed(e);
}

static void trace_done(const struct midr_trace_delivery *delivery, void *arg)
{
	struct admission_token *token = arg;
	struct admission_entry *e = token->owner;
	enum midr_admission_result result;
	if (!e) {
		XFREE(MTYPE_MIDR_ADMISSION_TOKEN, token);
		return;
	}
	e->token = NULL;
	XFREE(MTYPE_MIDR_ADMISSION_TOKEN, token);
	if (delivery->status == MIDR_TRACE_ERR_SHUTDOWN) {
		e->state = ADMISSION_WAIT_DATA;
		e->detail = "scheduler-shutdown";
		return;
	}
	if (!valid_owners(e) || !current_path(e) || !current_data(e) || now_ms() >= e->deadline ||
	    (!e->local_reasons && now_ms() - e->remote_seen > ADMISSION_REMOTE_LIFETIME_MS)) {
		retry_later(e, "obsolete-observation");
		return;
	}
	if (!delivery->has_view) {
		retry_later(e, "trace-failed");
		return;
	}
	result = midr_admission_evaluate(&delivery->view.job, &e->result);
	if (result == MIDR_ADMISSION_READY) {
		e->state = ADMISSION_ALLOWED;
		e->detail = "no-tier1-observed";
		e->last_blocked = false;
		e->permit = next_serial();
		e->measured_at = now_ms() - (delivery->has_cache_metadata ?
			delivery->cache_age_msec : 0);
		e->backoff = 0;
		queue_resume(e);
	} else if (result == MIDR_ADMISSION_BLOCKED) {
		e->state = ADMISSION_BLOCKED;
		e->detail = "tier1-observed";
		e->last_blocked = true;
		e->permit = 0;
		e->retry_at = now_ms() + ADMISSION_RETRY_MAX_MS;
		if (e->remote_reasons)
			midr_ctrl_reject_admission(e->manager->g1, e->target.transport_addr);
		zlog_info("MIDR admission: refusing %pIA: Tier1 observed",
			  &e->target.transport_addr);
	} else {
		retry_later(e, "trace-execution-error");
		if (e->remote_reasons)
			midr_ctrl_reject_admission(e->manager->g1, e->target.transport_addr);
	}
	changed(e);
}

static void start_trace(struct admission_entry *e)
{
	struct midr_trace_request_options options = {};
	struct midr_trace_scheduler_config trace_config;
	struct midr_trace_query_view cached;
	struct prefix target;
	uint32_t age = 0;
	uint64_t deadline_ms;
	enum midr_trace_submit_rc rc;
	/* A new attempt may need fresher evidence, but must not revoke the
	 * permit of a handshake already in progress (including passive clones).
	 * Data/policy invalidation still revokes that permit immediately. */
	if (e->token || midr_g1_config_inprocess() || ongoing_permit(e))
		return;
	if (!room_for(e)) {
		e->state = ADMISSION_WAIT_DATA;
		e->detail = "candidate-slots-full";
		return;
	}
	if (!current_path(e) || e->manager->g1->vrf_id != VRF_DEFAULT) {
		e->state = ADMISSION_WAIT_DATA;
		e->detail = "unsupported-or-inactive-context";
		return;
	}
	if (!midr_ip2asn_is_loaded() || !midr_tier1_list_active()) {
		e->state = ADMISSION_WAIT_DATA;
		e->detail = "ip2asn-or-tier1-list-not-loaded";
		return;
	}
	midr_ipaddr_to_host_prefix(&e->target.transport_addr, &target);
	midr_ipaddr_to_host_prefix(&e->source, &options.context.source);
	options.context.instance_cookie = e->manager->cookie;
	options.context.vrf_id = e->manager->g1->vrf_id;
	if (midr_trace_cache_lookup(&target, &options, &cached, &age) ==
	    MIDR_TRACE_LOOKUP_HIT && age > ADMISSION_MAX_AGE_MS)
		options.force_refresh = true;
	e->ip_generation = midr_ip2asn_generation();
	e->list_generation = midr_tier1_list_generation();
	e->permit = 0;
	/* Scheduler timeouts are configurable. Let its bounded queue and execution
	 * finish before the admission watchdog can cancel their partial result. */
	midr_trace_scheduler_config_get(&trace_config);
	deadline_ms = (uint64_t)trace_config.queue_timeout_msec +
		      trace_config.execution_timeout_msec + 5000U;
	e->deadline = now_ms() + MAX((uint64_t)ADMISSION_DEADLINE_MS,
				      deadline_ms);
	e->token = XCALLOC(MTYPE_MIDR_ADMISSION_TOKEN, sizeof(*e->token));
	e->token->owner = e;
	rc = midr_trace_request_async(&target, &options, trace_done, e->token,
				      &e->token->request_id);
	if (rc != MIDR_TRACE_SUBMIT_ACCEPTED) {
		XFREE(MTYPE_MIDR_ADMISSION_TOKEN, e->token);
		retry_later(e, "scheduler-not-ready-or-full");
		return;
	}
	e->state = ADMISSION_WAIT_TRACE;
	e->detail = "queued-or-probing";
}

static void tick(struct event *event)
{
	struct midr_admission *m = EVENT_ARG(event);
	struct admission_entry *e;
	struct listnode *n, *next;
	uint64_t now = now_ms();
	for (ALL_LIST_ELEMENTS(m->entries, n, next, e)) {
		struct midr_g1_peer *peer = entry_peer(e);
		bool established = peer && peer->established;
		(void)valid_owners(e);
		if (!established && now - e->remote_seen > ADMISSION_REMOTE_LIFETIME_MS)
			e->remote_reasons = 0;
		if (e->local_group != m->g1->midr_nds_info->local_group_id)
			e->local_reasons &= ~(1U << MIDR_SESSION_SAME_GROUP);
		if (!(m->g1->midr_nds_info->local_capabilities & MIDR_CAP_GROUP_REP))
			e->local_reasons &= ~(1U << MIDR_SESSION_ATTACH);
		if (!e->local_reasons && !e->remote_reasons) {
			cancel_trace(e);
			queue_resume(e);
			continue;
		}
		/* Screening re-measures only when NDS evaluates the candidate again. */
		if (!(e->local_reasons & ADMISSION_SESSION_MASK) && !e->remote_reasons) {
			if (e->token && now >= e->deadline) {
				cancel_trace(e);
				retry_later(e, "admission-timeout");
			}
			continue;
		}
		if (established || midr_g1_config_inprocess() || !current_path(e))
			continue;
		if (!m->g1->midr_nds_info->avoid_tier1) {
			if (!peer || !peer->requested)
				queue_resume(e);
			continue;
		}
		/* A permit's age gates NEW attempts, not a handshake already using
		 * it. Policy/data changes revoke it immediately through invalidate. */
		if (ongoing_permit(e))
			continue;
		if (e->token && now >= e->deadline) {
			cancel_trace(e);
			retry_later(e, "admission-timeout");
		}
		if (fresh_permit(e)) {
			if (!peer || !peer->requested)
				queue_resume(e);
		} else if (!e->token && now >= e->retry_at)
			start_trace(e);
	}
	/* A full manager must not strand a persisted manual configuration or an
	 * Idle peer which entered the FSM guard before an intent could be stored.
	 * Recover outside the entry walk; connect can add/remove list elements. */
	if (!midr_g1_config_inprocess() && !m->g1->midr_nds_info->shutdown &&
	    !m->g1->midr_nds_info->transport_reconfiguring && now >= m->recover_at) {
		struct midr_g1_peer *peer;
		m->recover_at = now + ADMISSION_RETRY_MIN_MS;
		if (m->g1->midr_nds_info->manual_sessions)
			midr_nds_manual_sessions_restore(m->g1);
		/* peer_start re-checks admission before requesting. */
		for (ALL_LIST_ELEMENTS_RO(m->g1->peer, n, peer))
			if (!peer->requested)
				midr_g1_peer_start(peer);
	}
	event_add_timer_msec(midr_g1_master(), tick, m, 1000, &m->timer);
}

static void invalidate(struct midr_admission *m)
{
	struct listnode *n;
	struct admission_entry *e;
	struct midr_g1_peer *peer;
	for (ALL_LIST_ELEMENTS_RO(m->entries, n, e)) {
		cancel_trace(e);
		event_cancel(&e->resume);
		e->permit = 0;
		e->retry_at = 0;
		e->state = ADMISSION_WAIT_DATA;
		e->last_blocked = false;
		e->detail = "policy-or-data-changed";
	}
	/* Stop pending attempts; stopping never deletes list elements. */
	for (ALL_LIST_ELEMENTS_RO(m->g1->peer, n, peer))
		if (peer->requested && !peer->established)
			midr_g1_peer_stop(peer);
}

static int data_changed(void)
{
	struct midr_admission *m;
	struct listnode *n;
	if (managers)
		for (ALL_LIST_ELEMENTS_RO(managers, n, m))
			if (m->g1->midr_nds_info->avoid_tier1)
				invalidate(m);
	return 0;
}

void midr_admission_init(struct midr_g1 *g1)
{
	struct midr_admission *m;
	if (g1->midr_nds_info->admission)
		return;
	m = XCALLOC(MTYPE_MIDR_ADMISSION, sizeof(*m));
	m->g1 = g1;
	m->entries = list_new();
	m->cookie = next_serial();
	g1->midr_nds_info->admission = m;
	if (!managers) {
		managers = list_new();
		hook_register(midr_policy_changed, data_changed);
	}
	listnode_add(managers, m);
	event_add_timer_msec(midr_g1_master(), tick, m, 1000, &m->timer);
}

void midr_admission_reset(struct midr_g1 *g1)
{
	struct midr_admission *m = g1->midr_nds_info->admission;
	struct admission_entry *e;
	struct listnode *n, *next;
	if (!m)
		return;
	event_cancel(&m->notify);
	event_cancel(&m->node_notify);
	for (ALL_LIST_ELEMENTS(m->entries, n, next, e))
		free_entry(e);
}

void midr_admission_finish(struct midr_g1 *g1)
{
	struct midr_admission *m = g1->midr_nds_info->admission;
	if (!m)
		return;
	midr_admission_reset(g1);
	event_cancel(&m->timer);
	list_delete(&m->entries);
	listnode_delete(managers, m);
	if (!listcount(managers)) {
		hook_unregister(midr_policy_changed, data_changed);
		list_delete(&managers);
	}
	g1->midr_nds_info->admission = NULL;
	XFREE(MTYPE_MIDR_ADMISSION, m);
}

void midr_admission_set(struct midr_g1 *g1, bool enabled)
{
	struct midr_nds *mi = g1->midr_nds_info;
	if (mi->avoid_tier1 == enabled)
		return;
	mi->avoid_tier1 = enabled;
	/* Revoke immediately even during config reload. Resumption is deferred
	 * by tick/resume_connect until the entire configuration has been read. */
	if (mi->admission)
		invalidate(mi->admission);
}

void midr_admission_forget(struct midr_g1 *g1, struct ipaddr target)
{
	struct admission_entry *e = find_entry(g1, target);
	if (e)
		free_entry(e);
}

void midr_admission_forget_reason(struct midr_g1 *g1, enum midr_session_reason reason)
{
	struct midr_admission *m = g1->midr_nds_info->admission;
	struct admission_entry *e;
	struct listnode *n, *next;
	if (!m)
		return;
	for (ALL_LIST_ELEMENTS(m->entries, n, next, e)) {
		e->local_reasons &= ~(1U << reason);
		e->remote_reasons &= ~(1U << reason);
		e->nudge_reasons &= ~(1U << reason);
		if (!e->local_reasons && !e->remote_reasons)
			free_entry(e);
	}
}

void midr_admission_remote_seen(struct midr_g1 *g1, struct ipaddr target)
{
	struct admission_entry *e = find_entry(g1, target);
	if (e)
		e->remote_seen = now_ms();
}

/* Find or create the intent for target from the current local transport.
 * Returns NULL on identity conflict or when capacity is exhausted. */
static struct admission_entry *intent_get(struct midr_g1 *g1,
					  const struct midr_node_entry *target,
					  struct ipaddr source)
{
	struct midr_admission *m = g1->midr_nds_info->admission;
	struct admission_entry *e;

	e = find_entry(g1, target->transport_addr);
	if (e && e->target.node_id.u.prefix4.s_addr && target->node_id.u.prefix4.s_addr &&
	    !prefix_same(&e->target.node_id, &target->node_id))
		return NULL;
	if (e && !midr_ipaddr_same(&e->source, &source)) {
		free_entry(e);
		e = NULL;
	}
	if (!e) {
		/* Established connections no longer need their admission observation.
		 * Reclaim one at capacity; a future reconnect recreates demand from
		 * the existing ledger. Never evict an in-flight or rejected intent. */
		if (listcount(m->entries) >= ADMISSION_LIMIT) {
			struct listnode *n;
			struct admission_entry *old;
			for (ALL_LIST_ELEMENTS_RO(m->entries, n, old)) {
				struct midr_g1_peer *peer = entry_peer(old);
				if (peer && peer->established) {
					free_entry(old);
					break;
				}
			}
		}
		if (listcount(m->entries) >= ADMISSION_LIMIT)
			return NULL; /* No retained intent: never report pending. */
		e = XCALLOC(MTYPE_MIDR_ADMISSION_ENTRY, sizeof(*e));
		e->manager = m;
		e->source = source;
		e->remote_seen = now_ms();
		e->detail = "waiting";
		listnode_add(m->entries, e);
	}
	/* Never copy node hash links, PM state, or borrowed pointers. */
	if (target->node_id.u.prefix4.s_addr || !e->target.node_id.family)
		e->target.node_id = target->node_id;
	e->target.asn = target->asn;
	e->target.group_id = target->group_id;
	e->target.capabilities = target->capabilities;
	e->target.has_transport_addr = true;
	e->target.transport_addr = target->transport_addr;
	return e;
}

enum midr_admission_result midr_admission_gate(struct midr_g1 *g1,
	const struct midr_node_entry *target, enum midr_session_reason reason,
	bool send_nudge, bool received, bool attach_request)
{
	struct midr_nds *mi = g1->midr_nds_info;
	struct midr_admission *m = mi->admission;
	struct admission_entry *e;
	struct ipaddr source;
	if (!mi->avoid_tier1)
		return MIDR_ADMISSION_READY;
	if (!m || !m->cookie || (unsigned int)reason >= ADMISSION_REASON_COUNT ||
	    !midr_nds_local_transport_get(g1, &source))
		return MIDR_ADMISSION_INVALID;
	e = intent_get(g1, target, source);
	if (!e)
		return MIDR_ADMISSION_INVALID;
	if (received) {
		e->remote_reasons |= 1U << reason;
		e->remote_attach = attach_request;
	}
	else {
		e->local_reasons |= 1U << reason;
		e->local_group = mi->local_group_id;
	}
	if (send_nudge)
		e->nudge_reasons |= 1U << reason;
	if (fresh_permit(e) || ongoing_permit(e))
		return MIDR_ADMISSION_READY;
	if (e->state == ADMISSION_BLOCKED && current_data(e) && now_ms() < e->retry_at)
		return MIDR_ADMISSION_BLOCKED;
	if (!e->token && now_ms() >= e->retry_at)
		start_trace(e);
	return MIDR_ADMISSION_PENDING;
}

void midr_admission_screen(struct midr_g1 *g1, const struct midr_node_entry *target)
{
	struct midr_nds *mi = g1 ? g1->midr_nds_info : NULL;
	struct admission_entry *e;
	struct ipaddr source;
	uint64_t now = now_ms();

	if (!mi || !mi->avoid_tier1 || !mi->admission || !target ||
	    !target->has_transport_addr ||
	    !midr_ipaddr_valid_locator(&target->transport_addr) ||
	    !midr_nds_local_transport_get(g1, &source))
		return;
	e = intent_get(g1, target, source);
	if (!e)
		return;
	e->local_reasons |= ADMISSION_SCREEN_BIT;
	e->screen_until = now + ADMISSION_SCREEN_LIFETIME_MS;
	if (e->token || now < e->retry_at)
		return;
	if (e->state == ADMISSION_ALLOWED && current_data(e) &&
	    now - e->measured_at <= ADMISSION_MAX_AGE_MS)
		return;
	start_trace(e);
}

static struct admission_entry *peer_entry(struct midr_g1_peer *peer)
{
	return find_entry(peer->g1, peer->transport);
}

/* midrd binds every outbound session to its listen address; the permit was
 * measured from that source. */
static bool peer_source_matches(struct midr_g1_peer *peer, struct admission_entry *e)
{
	struct ipaddr source;
	if (!e)
		return false;
	if (!midr_context_listen_address(peer->g1->ctx, &source))
		return true;
	return midr_ipaddr_same(&source, &e->source);
}

bool midr_admission_peer_ready(struct midr_g1_peer *peer)
{
	struct midr_nds *mi = peer->g1->midr_nds_info;
	struct admission_entry *e;
	struct midr_node_entry target = {};
	const struct midr_session_ledger_entry *ledger;
	if (!midr_nds_peer_is_overlay(peer))
		return true;
	if (!mi || mi->shutdown || mi->transport_reconfiguring)
		return false;
	if (!mi->avoid_tier1)
		return true;
	e = peer_entry(peer);
	if (e && !valid_owners(e)) {
		queue_resume(e);
		return false;
	}
	/* A screening verdict is not session demand; derive it from the ledger. */
	if (e && !(e->local_reasons & ADMISSION_SESSION_MASK) && !e->remote_reasons) {
		if (!midr_nds_ledger_lookup(peer->g1, e->target.transport_addr))
			return false;
		e = NULL;
	}
	if (fresh_permit(e) && peer_source_matches(peer, e))
		return true;
	if (e) {
		if (!e->token && now_ms() >= e->retry_at)
			start_trace(e);
		return false;
	}
	target.transport_addr = peer->transport;
	ledger = midr_nds_ledger_lookup(peer->g1, target.transport_addr);
	/* A received-only ledger is evidence of a past request, not a durable
	 * local demand. Wait for the requester to renew it. */
	if (!ledger || ledger->reason == MIDR_SESSION_PEER_REQ_REPLY)
		return false;
	target.node_id.family = AF_INET;
	target.node_id.prefixlen = 32;
	target.node_id.u.prefix4 = ledger->remote_rid;
	target.has_transport_addr = true;
	target.asn = ledger->remote_asn;
	target.group_id = ledger->remote_group;
	return midr_admission_gate(peer->g1, &target, ledger->reason, false, false, false)
		== MIDR_ADMISSION_READY;
}

bool midr_admission_begin(struct midr_g1_peer *peer)
{
	struct admission_entry *e;
	if (!midr_admission_peer_ready(peer))
		return false;
	e = peer_entry(peer);
	peer->admission_permit = e ? e->permit : 0;
	return true;
}

bool midr_admission_check(struct midr_g1_peer *peer)
{
	struct admission_entry *e;
	if (!midr_nds_peer_is_overlay(peer))
		return true;
	if (!peer->g1->midr_nds_info)
		return false;
	if (!peer->g1->midr_nds_info->avoid_tier1)
		return !peer->g1->midr_nds_info->shutdown &&
		       !peer->g1->midr_nds_info->transport_reconfiguring;
	e = peer_entry(peer);
	if (!peer_source_matches(peer, e))
		return false;
	return e && valid_owners(e) && e->permit && peer->admission_permit == e->permit &&
	       current_path(e) && current_data(e);
}

bool midr_admission_unavailable(struct midr_g1 *g1, struct ipaddr target)
{
	struct admission_entry *e = find_entry(g1, target);
	return g1->midr_nds_info->avoid_tier1 && e &&
	       ((e->state == ADMISSION_BLOCKED && current_data(e) && now_ms() < e->retry_at) ||
		(e->state == ADMISSION_UNKNOWN && now_ms() < e->retry_at) || e->token);
}

bool midr_admission_candidate_blocked(const struct midr_g1 *g1, struct ipaddr target)
{
	struct admission_entry *e = find_entry(g1, target);
	struct midr_g1_peer *peer = e ? entry_peer(e) : NULL;
	if (peer && peer->established)
		return false;
	if (!g1->midr_nds_info->avoid_tier1 || !e || !current_data(e))
		return false;
	if (e->state == ADMISSION_UNKNOWN && now_ms() < e->retry_at)
		return true;
	/* A Tier1 verdict stands until a completed re-measurement clears it. */
	return e->last_blocked &&
	       (e->state == ADMISSION_BLOCKED || e->state == ADMISSION_WAIT_TRACE);
}

size_t midr_admission_anchor_groups(struct midr_g1 *g1, uint32_t *groups, size_t size)
{
	struct midr_admission *m = g1->midr_nds_info->admission;
	struct listnode *n;
	struct admission_entry *e;
	size_t count = 0, i;
	if (!m || !g1->midr_nds_info->avoid_tier1)
		return 0;
	for (ALL_LIST_ELEMENTS_RO(m->entries, n, e)) {
		if (!(e->local_reasons & (1U << MIDR_SESSION_CL_ANCHOR)) ||
		    !e->target.group_id ||
		    !midr_admission_candidate_blocked(g1, e->target.transport_addr))
			continue;
		for (i = 0; i < count; i++)
			if (groups[i] == e->target.group_id)
				break;
		if (i == count && count < size)
			groups[count++] = e->target.group_id;
	}
	return count;
}

unsigned int midr_admission_attach_pending(struct midr_g1 *g1)
{
	struct midr_admission *m = g1->midr_nds_info->admission;
	struct admission_entry *e;
	struct listnode *n;
	unsigned int count = 0;
	if (m)
		for (ALL_LIST_ELEMENTS_RO(m->entries, n, e))
			if ((e->local_reasons & (1U << MIDR_SESSION_ATTACH)) &&
			    (e->token || e->resume) && !entry_peer(e))
				count++;
	return count;
}

void midr_admission_retx_budget(struct midr_g1 *g1, struct ipaddr target, int count)
{
	struct admission_entry *e = find_entry(g1, target);
	if (e)
		e->retx_budget = count;
}

int midr_admission_get_retx_budget(struct midr_g1 *g1, struct ipaddr target)
{
	struct admission_entry *e = find_entry(g1, target);
	return e ? e->retx_budget : 0;
}

void midr_admission_show(struct midr_g1 *g1, struct vty *vty)
{
	static const char *const names[] = {
		"WAIT_DATA", "QUEUED/PROBING", "ALLOWED", "BLOCKED_TIER1", "INDETERMINATE"
	};
	struct midr_admission *m = g1->midr_nds_info->admission;
	struct admission_entry *e;
	struct listnode *n;
	vty_out(vty, "MIDR avoid-tier1: %s (existing Established sessions retained)\n",
		g1->midr_nds_info->avoid_tier1 ? "enabled" : "disabled");
	if (!m)
		return;
	vty_out(vty, "  Intent capacity: %u/%u; VRF: %u; instance context: %" PRIu64 "\n",
		(unsigned int)listcount(m->entries), ADMISSION_LIMIT, g1->vrf_id, m->cookie);
	for (ALL_LIST_ELEMENTS_RO(m->entries, n, e)) {
		struct midr_g1_peer *peer = entry_peer(e);
		vty_out(vty, "  %pIA -> %pIA: %s (%s), IP2ASN %" PRIu64
			" Tier1 %" PRIu64 ", request %" PRIu64 ", owners local=0x%x remote=0x%x%s%s\n",
			&e->source, &e->target.transport_addr, names[e->state], e->detail,
			e->ip_generation, e->list_generation,
			e->token ? e->token->request_id : 0,
			e->local_reasons & ADMISSION_SESSION_MASK, e->remote_reasons,
			(e->local_reasons & ADMISSION_SCREEN_BIT) ? " (candidate screening)" : "",
			peer && peer->established ?
				" (Established retained)" : "");
	}
}
