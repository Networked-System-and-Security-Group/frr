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
#include "bgpd/bgpd.h"
#include "bgpd/bgp_fsm.h"
#include "bgpd/bgp_midr_ctrl.h"
#include "bgpd/bgp_midr_admission.h"
#include "bgpd/midr_ip2asn.h"
#include "bgpd/midr_tier1_list.h"
#include "bgpd/midr_trace_scheduler.h"

#define ADMISSION_LIMIT 128U
#define ADMISSION_MAX_AGE_MS 30000U
#define ADMISSION_DEADLINE_MS 105000U
#define ADMISSION_REMOTE_LIFETIME_MS 10000U
#define ADMISSION_RETRY_MIN_MS 5000U
#define ADMISSION_RETRY_MAX_MS 60000U
#define ADMISSION_REASON_COUNT (MIDR_SESSION_PEER_REQ_REPLY + 1)
#define ADMISSION_ANCHORS_PER_GROUP 2U

DEFINE_MTYPE_STATIC(BGPD, MIDR_ADMISSION, "MIDR admission manager");
DEFINE_MTYPE_STATIC(BGPD, MIDR_ADMISSION_ENTRY, "MIDR admission intent");
DEFINE_MTYPE_STATIC(BGPD, MIDR_ADMISSION_TOKEN, "MIDR admission callback token");

enum admission_state { WAIT_DATA, WAIT_TRACE, ALLOWED, BLOCKED, UNKNOWN };
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
	unsigned int backoff;
	int retx_budget;
	enum admission_state state;
	const char *detail;
	struct midr_tier1_result result;
	struct admission_token *token;
	struct event *resume;
};
struct midr_admission {
	struct bgp *bgp;
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

static struct admission_entry *find_entry(const struct bgp *bgp, struct ipaddr target)
{
	struct listnode *n;
	struct admission_entry *e;
	struct midr_admission *m = bgp && bgp->midr_nds_info
		? bgp->midr_nds_info->admission : NULL;
	if (m)
		for (ALL_LIST_ELEMENTS_RO(m->entries, n, e))
			if (midr_ipaddr_same(&e->target.transport_addr, &target))
				return e;
	return NULL;
}

static struct peer *entry_peer(struct admission_entry *e)
{
	union sockunion su;
	if (!midr_ipaddr_to_sockunion(&e->target.transport_addr, &su))
		return NULL;
	return peer_lookup(e->manager->bgp, &su);
}

bool midr_admission_has_intent(struct bgp *bgp, struct ipaddr target)
{
	return find_entry(bgp, target) != NULL;
}

bool midr_admission_is_manual(struct bgp *bgp, struct ipaddr target)
{
	struct admission_entry *e = find_entry(bgp, target);
	return e && (e->local_reasons & (1U << MIDR_SESSION_MANUAL));
}

/* A cooled-down candidate must not create an extra edge after a replacement
 * filled its slot. Count committed ledgers and other in-flight reservations. */
static bool room_for(struct admission_entry *e)
{
	struct midr_admission *m = e->manager;
	struct bgp_midr_nds *mi = m->bgp->midr_nds_info;
	struct listnode *n;
	struct midr_session_ledger_entry *ledger;
	struct admission_entry *other;
	enum midr_session_reason reason;
	unsigned int used = 0, limit;
	if (e->remote_reasons || entry_peer(e))
		return true;
	if (e->local_reasons == (1U << MIDR_SESSION_ATTACH)) {
		reason = MIDR_SESSION_ATTACH;
		limit = MIDR_ATTACH_K;
	} else if (e->local_reasons == (1U << MIDR_SESSION_CL_ANCHOR)) {
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
	struct bgp *bgp = e->manager->bgp;
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct midr_node_entry *known = mi->global_view ?
		midr_node_hash_find(&mi->global_view->nodes, &e->target) : NULL;
	struct listnode *n;
	struct midr_bootstrap_entry *candidate;
	bool has_candidate = false;
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
	if (midr_nds_is_session_excluded(bgp, e->target.node_id.u.prefix4)) {
		e->local_reasons &= 1U << MIDR_SESSION_MANUAL;
		e->remote_reasons = 0;
	}
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
	struct bgp *bgp = e->manager->bgp;
	struct ipaddr local;
	return !bgp->midr_nds_info->shutdown &&
	       !bgp->midr_nds_info->transport_reconfiguring &&
	       (!bgp->midr_nds_info->avoid_tier1 || bgp->vrf_id == VRF_DEFAULT) &&
	       midr_nds_local_transport_get(bgp, &local) &&
	       midr_ipaddr_same(&local, &e->source);
}

static bool fresh_permit(struct admission_entry *e)
{
	return e && valid_owners(e) && e->state == ALLOWED && e->permit && current_path(e) &&
	       current_data(e) && now_ms() - e->measured_at <= ADMISSION_MAX_AGE_MS;
}

static bool connection_ongoing(struct peer_connection *connection)
{
	return connection && connection->status != Idle &&
	       connection->status != Established &&
	       connection->midr_admission_permit &&
	       midr_admission_check(connection);
}

static bool ongoing_permit(struct admission_entry *e)
{
	struct peer *peer = entry_peer(e);
	return peer && (connection_ongoing(peer->connection) ||
		       (peer->doppelganger &&
			connection_ongoing(peer->doppelganger->connection)));
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
	if (m->bgp->midr_nds_info->shutdown || bgp_config_inprocess())
		return;
	midr_nds_attach_pick(m->bgp);
	midr_nds_notify_cl(m->bgp, MIDR_TRIGGER_ADMISSION_CHANGE);
}

static void changed(struct admission_entry *e)
{
	struct midr_admission *m = e->manager;
	if (!m->notify)
		event_add_event(bm->master, notify_cl, m, 0, &m->notify);
}

static void notify_node(struct event *event)
{
	struct midr_admission *m = EVENT_ARG(event);
	midr_nds_notify_cl(m->bgp, MIDR_TRIGGER_NODE_CHANGE);
}

void midr_admission_committed(struct bgp *bgp)
{
	struct midr_admission *m = bgp->midr_nds_info->admission;
	if (m && !m->node_notify)
		event_add_event(bm->master, notify_node, m, 0, &m->node_notify);
}

static void resume_connect(struct event *event)
{
	struct admission_entry *e = EVENT_ARG(event);
	(void)valid_owners(e);
	struct bgp *bgp = e->manager->bgp;
	struct midr_node_entry target = e->target;
	unsigned int local = e->local_reasons, remote = e->remote_reasons;
	unsigned int nudge = e->nudge_reasons;
	bool attach_request = e->remote_attach;
	struct peer *peer;
	int i;
	if (!local && !remote) {
		/* Remote request expired or its role disappeared. Do this outside
		 * the intent list walk, since peer deletion invokes NDS hooks. */
		peer = entry_peer(e);
		bool committed = midr_nds_ledger_lookup(bgp, target.transport_addr) != NULL;
		bool established = peer && peer->connection && peer->connection->status == Established;
		midr_admission_forget(bgp, target.transport_addr);
		/* Pending intent cancellation must not stop independent candidate PM.
		 * Established teardown remains owned by the existing NDS lifecycle. */
		if (established || (!committed && !peer))
			return;
		midr_ctrl_forget_target(bgp, target.transport_addr);
		midr_nds_ledger_drop(bgp, target.transport_addr);
		midr_nds_cleanup_by_transport(bgp, target.transport_addr,
					      MIDR_STOP_KEEPALIVE_TIMEOUT);
		return;
	}
	if (bgp_config_inprocess() || !current_path(e))
		return;
	if (!room_for(e))
		return;
	if (bgp->midr_nds_info->avoid_tier1 && !fresh_permit(e))
		return;
	/* connect may retire an intent. Work from values, never retain e across it. */
	for (i = 0; i < ADMISSION_REASON_COUNT; i++) {
		if (local & (1U << i))
			midr_ctrl_connect(bgp, &target, i, !!(nudge & (1U << i)));
		else if (remote & (1U << i))
			midr_ctrl_connect_received(bgp, &target, i, attach_request);
	}
	e = find_entry(bgp, target.transport_addr);
	if (!e)
		return;
	peer = entry_peer(e);
	if (peer && midr_nds_peer_is_overlay(peer) && peer->connection &&
	    peer->connection->status == Idle && !BGP_PEER_START_SUPPRESSED(peer)) {
		event_cancel(&peer->connection->t_start);
		BGP_EVENT_ADD(peer->connection, BGP_Start);
	}
}

static void queue_resume(struct admission_entry *e)
{
	if (!e->resume)
		event_add_event(bm->master, resume_connect, e, 0, &e->resume);
}

static void retry_later(struct admission_entry *e, const char *detail)
{
	e->permit = 0;
	e->state = UNKNOWN;
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
		e->state = WAIT_DATA;
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
		e->state = ALLOWED;
		e->detail = "no-tier1-observed";
		e->permit = next_serial();
		e->measured_at = now_ms() - (delivery->has_cache_metadata ?
			delivery->cache_age_msec : 0);
		e->backoff = 0;
		queue_resume(e);
	} else if (result == MIDR_ADMISSION_BLOCKED) {
		e->state = BLOCKED;
		e->detail = "tier1-observed";
		e->permit = 0;
		e->retry_at = now_ms() + ADMISSION_RETRY_MAX_MS;
		if (e->remote_reasons)
			midr_ctrl_reject_admission(e->manager->bgp, e->target.transport_addr);
		zlog_info("MIDR admission: refusing %pIA: Tier1 observed",
			  &e->target.transport_addr);
	} else {
		retry_later(e, "trace-execution-error");
		if (e->remote_reasons)
			midr_ctrl_reject_admission(e->manager->bgp, e->target.transport_addr);
	}
	changed(e);
}

static void start_trace(struct admission_entry *e)
{
	struct midr_trace_request_options options = {};
	struct midr_trace_query_view cached;
	struct prefix target;
	uint32_t age = 0;
	enum midr_trace_submit_rc rc;
	/* A new attempt may need fresher evidence, but must not revoke the
	 * permit of a handshake already in progress (including passive clones).
	 * Data/policy invalidation still revokes that permit immediately. */
	if (e->token || bgp_config_inprocess() || ongoing_permit(e))
		return;
	if (!room_for(e)) {
		e->state = WAIT_DATA;
		e->detail = "candidate-slots-full";
		return;
	}
	if (!current_path(e) || e->manager->bgp->vrf_id != VRF_DEFAULT) {
		e->state = WAIT_DATA;
		e->detail = "unsupported-or-inactive-context";
		return;
	}
	if (!midr_ip2asn_is_loaded() || !midr_tier1_list_active()) {
		e->state = WAIT_DATA;
		e->detail = "ip2asn-or-tier1-list-not-loaded";
		return;
	}
	midr_ipaddr_to_host_prefix(&e->target.transport_addr, &target);
	midr_ipaddr_to_host_prefix(&e->source, &options.context.source);
	options.context.instance_cookie = e->manager->cookie;
	options.context.vrf_id = e->manager->bgp->vrf_id;
	if (midr_trace_cache_lookup(&target, &options, &cached, &age) ==
	    MIDR_TRACE_LOOKUP_HIT && age > ADMISSION_MAX_AGE_MS)
		options.force_refresh = true;
	e->ip_generation = midr_ip2asn_generation();
	e->list_generation = midr_tier1_list_generation();
	e->permit = 0;
	e->deadline = now_ms() + ADMISSION_DEADLINE_MS;
	e->token = XCALLOC(MTYPE_MIDR_ADMISSION_TOKEN, sizeof(*e->token));
	e->token->owner = e;
	rc = midr_trace_request_async(&target, &options, trace_done, e->token,
				      &e->token->request_id);
	if (rc != MIDR_TRACE_SUBMIT_ACCEPTED) {
		XFREE(MTYPE_MIDR_ADMISSION_TOKEN, e->token);
		retry_later(e, "scheduler-not-ready-or-full");
		return;
	}
	e->state = WAIT_TRACE;
	e->detail = "queued-or-probing";
}

static void tick(struct event *event)
{
	struct midr_admission *m = EVENT_ARG(event);
	struct admission_entry *e;
	struct listnode *n, *next;
	uint64_t now = now_ms();
	for (ALL_LIST_ELEMENTS(m->entries, n, next, e)) {
		struct peer *peer = entry_peer(e);
		bool established = peer && peer->connection &&
			peer->connection->status == Established;
		(void)valid_owners(e);
		if (!established && now - e->remote_seen > ADMISSION_REMOTE_LIFETIME_MS)
			e->remote_reasons = 0;
		if (e->local_group != m->bgp->midr_nds_info->local_group_id)
			e->local_reasons &= ~(1U << MIDR_SESSION_SAME_GROUP);
		if (!(m->bgp->midr_nds_info->local_capabilities & MIDR_CAP_GROUP_REP))
			e->local_reasons &= ~(1U << MIDR_SESSION_ATTACH);
		if (!e->local_reasons && !e->remote_reasons) {
			cancel_trace(e);
			queue_resume(e);
			continue;
		}
		if (established || bgp_config_inprocess() || !current_path(e))
			continue;
		if (!m->bgp->midr_nds_info->avoid_tier1) {
			if (!peer || (peer->connection && peer->connection->status == Idle))
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
			if (!peer || (peer->connection && peer->connection->status == Idle))
				queue_resume(e);
		} else if (!e->token && now >= e->retry_at)
			start_trace(e);
	}
	/* A full manager must not strand a persisted manual configuration or an
	 * Idle peer which entered the FSM guard before an intent could be stored.
	 * Recover outside the entry walk; connect can add/remove list elements. */
	if (!bgp_config_inprocess() && !m->bgp->midr_nds_info->shutdown &&
	    !m->bgp->midr_nds_info->transport_reconfiguring && now >= m->recover_at) {
		struct peer *peer;
		m->recover_at = now + ADMISSION_RETRY_MIN_MS;
		if (m->bgp->midr_nds_info->manual_sessions)
			midr_nds_manual_sessions_restore(m->bgp);
		for (ALL_LIST_ELEMENTS_RO(m->bgp->peer, n, peer))
			if (midr_nds_peer_is_overlay(peer) && peer->connection &&
			    peer->connection->status == Idle && !BGP_PEER_START_SUPPRESSED(peer) &&
			    !peer->connection->t_start &&
			    midr_admission_peer_ready(peer)) {
				event_cancel(&peer->connection->t_start);
				BGP_EVENT_ADD(peer->connection, BGP_Start);
			}
	}
	event_add_timer_msec(bm->master, tick, m, 1000, &m->timer);
}

static void invalidate(struct midr_admission *m)
{
	struct listnode *n;
	struct admission_entry *e;
	struct peer *peer;
	for (ALL_LIST_ELEMENTS_RO(m->entries, n, e)) {
		cancel_trace(e);
		event_cancel(&e->resume);
		e->permit = 0;
		e->retry_at = 0;
		e->state = WAIT_DATA;
		e->detail = "policy-or-data-changed";
	}
	/* Queue stop, never delete peers while traversing the instance list. */
	for (ALL_LIST_ELEMENTS_RO(m->bgp->peer, n, peer))
		if (midr_nds_peer_is_overlay(peer) && peer->connection &&
		    peer->connection->status != Established &&
		    peer->connection->status != Idle)
			BGP_EVENT_ADD(peer->connection, BGP_Stop);
}

static int data_changed(void)
{
	struct midr_admission *m;
	struct listnode *n;
	if (managers)
		for (ALL_LIST_ELEMENTS_RO(managers, n, m))
			if (m->bgp->midr_nds_info->avoid_tier1)
				invalidate(m);
	return 0;
}

void midr_admission_init(struct bgp *bgp)
{
	struct midr_admission *m;
	if (bgp->midr_nds_info->admission)
		return;
	m = XCALLOC(MTYPE_MIDR_ADMISSION, sizeof(*m));
	m->bgp = bgp;
	m->entries = list_new();
	m->cookie = next_serial();
	bgp->midr_nds_info->admission = m;
	if (!managers) {
		managers = list_new();
		hook_register(midr_policy_changed, data_changed);
	}
	listnode_add(managers, m);
	event_add_timer_msec(bm->master, tick, m, 1000, &m->timer);
}

void midr_admission_reset(struct bgp *bgp)
{
	struct midr_admission *m = bgp->midr_nds_info->admission;
	struct admission_entry *e;
	struct listnode *n, *next;
	if (!m)
		return;
	event_cancel(&m->notify);
	event_cancel(&m->node_notify);
	for (ALL_LIST_ELEMENTS(m->entries, n, next, e))
		free_entry(e);
}

void midr_admission_finish(struct bgp *bgp)
{
	struct midr_admission *m = bgp->midr_nds_info->admission;
	if (!m)
		return;
	midr_admission_reset(bgp);
	event_cancel(&m->timer);
	list_delete(&m->entries);
	listnode_delete(managers, m);
	if (!listcount(managers)) {
		hook_unregister(midr_policy_changed, data_changed);
		list_delete(&managers);
	}
	bgp->midr_nds_info->admission = NULL;
	XFREE(MTYPE_MIDR_ADMISSION, m);
}

void midr_admission_set(struct bgp *bgp, bool enabled)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	if (mi->avoid_tier1 == enabled)
		return;
	mi->avoid_tier1 = enabled;
	/* Revoke immediately even during config reload. Resumption is deferred
	 * by tick/resume_connect until the entire configuration has been read. */
	if (mi->admission)
		invalidate(mi->admission);
}

void midr_admission_forget(struct bgp *bgp, struct ipaddr target)
{
	struct admission_entry *e = find_entry(bgp, target);
	if (e)
		free_entry(e);
}

void midr_admission_forget_reason(struct bgp *bgp, enum midr_session_reason reason)
{
	struct midr_admission *m = bgp->midr_nds_info->admission;
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

void midr_admission_remote_seen(struct bgp *bgp, struct ipaddr target)
{
	struct admission_entry *e = find_entry(bgp, target);
	if (e)
		e->remote_seen = now_ms();
}

enum midr_admission_result midr_admission_gate(struct bgp *bgp,
	const struct midr_node_entry *target, enum midr_session_reason reason,
	bool send_nudge, bool received, bool attach_request)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct midr_admission *m = mi->admission;
	struct admission_entry *e;
	struct ipaddr source;
	if (!mi->avoid_tier1)
		return MIDR_ADMISSION_READY;
	if (!m || !m->cookie || (unsigned int)reason >= ADMISSION_REASON_COUNT ||
	    !midr_nds_local_transport_get(bgp, &source))
		return MIDR_ADMISSION_INVALID;
	e = find_entry(bgp, target->transport_addr);
	if (e && e->target.node_id.u.prefix4.s_addr && target->node_id.u.prefix4.s_addr &&
	    !prefix_same(&e->target.node_id, &target->node_id))
		return MIDR_ADMISSION_INVALID;
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
				struct peer *peer = entry_peer(old);
				if (peer && peer->connection && peer->connection->status == Established) {
					free_entry(old);
					break;
				}
			}
		}
		if (listcount(m->entries) >= ADMISSION_LIMIT)
			return MIDR_ADMISSION_INVALID; /* No retained intent: never report pending. */
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
	if (e->state == BLOCKED && current_data(e) && now_ms() < e->retry_at)
		return MIDR_ADMISSION_BLOCKED;
	if (!e->token && now_ms() >= e->retry_at)
		start_trace(e);
	return MIDR_ADMISSION_PENDING;
}

static struct admission_entry *peer_entry(struct peer *peer)
{
	struct ipaddr target;
	if (!peer->connection ||
	    !midr_sockunion_to_ipaddr(&peer->connection->su, &target))
		return NULL;
	return find_entry(peer->bgp, target);
}

static bool peer_source_matches(struct peer *peer, struct admission_entry *e)
{
	struct ipaddr source;
	return e && !peer->update_if && !peer->conf_if && !peer->ifname &&
	       peer->update_source && midr_sockunion_to_ipaddr(peer->update_source, &source) &&
	       midr_ipaddr_same(&source, &e->source);
}

bool midr_admission_peer_ready(struct peer *peer)
{
	struct bgp_midr_nds *mi = peer->bgp->midr_nds_info;
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
	if (fresh_permit(e) && peer_source_matches(peer, e))
		return true;
	if (e) {
		if (!e->token && now_ms() >= e->retry_at)
			start_trace(e);
		return false;
	}
	if (!midr_sockunion_to_ipaddr(&peer->connection->su, &target.transport_addr))
		return false;
	ledger = midr_nds_ledger_lookup(peer->bgp, target.transport_addr);
	if (!ledger)
		return false;
	target.node_id.family = AF_INET;
	target.node_id.prefixlen = 32;
	target.node_id.u.prefix4 = ledger->remote_rid;
	target.has_transport_addr = true;
	target.asn = ledger->remote_asn;
	target.group_id = ledger->remote_group;
	return midr_admission_gate(peer->bgp, &target, ledger->reason, false, false, false)
		== MIDR_ADMISSION_READY;
}

bool midr_admission_begin(struct peer_connection *connection)
{
	struct admission_entry *e;
	if (!midr_admission_peer_ready(connection->peer))
		return false;
	e = peer_entry(connection->peer);
	connection->midr_admission_permit = e ? e->permit : 0;
	return true;
}

bool midr_admission_check(struct peer_connection *connection)
{
	struct peer *peer = connection->peer;
	struct admission_entry *e;
	struct ipaddr local;
	if (!midr_nds_peer_is_overlay(peer))
		return true;
	if (!peer->bgp->midr_nds_info)
		return false;
	if (!peer->bgp->midr_nds_info->avoid_tier1)
		return !peer->bgp->midr_nds_info->shutdown &&
		       !peer->bgp->midr_nds_info->transport_reconfiguring;
	e = peer_entry(peer);
	if (!peer_source_matches(peer, e))
		return false;
	/* An accepted socket can have arrived on another local address even
	 * though its remote address matched the configured MIDR peer. */
	if (connection->su_local &&
	    (!midr_sockunion_to_ipaddr(connection->su_local, &local) ||
	     !midr_ipaddr_same(&local, &e->source)))
		return false;
	return e && valid_owners(e) && e->permit && connection->midr_admission_permit == e->permit &&
	       current_path(e) && current_data(e);
}

bool midr_admission_unavailable(struct bgp *bgp, struct ipaddr target)
{
	struct admission_entry *e = find_entry(bgp, target);
	return bgp->midr_nds_info->avoid_tier1 && e &&
	       ((e->state == BLOCKED && current_data(e) && now_ms() < e->retry_at) ||
		(e->state == UNKNOWN && now_ms() < e->retry_at) || e->token);
}

bool midr_admission_candidate_blocked(const struct bgp *bgp, struct ipaddr target)
{
	struct admission_entry *e = find_entry(bgp, target);
	struct peer *peer = e ? entry_peer(e) : NULL;
	if (peer && peer->connection && peer->connection->status == Established)
		return false;
	return bgp->midr_nds_info->avoid_tier1 && e &&
	       (e->state == BLOCKED || e->state == UNKNOWN) &&
	       current_data(e) && now_ms() < e->retry_at;
}

size_t midr_admission_anchor_groups(struct bgp *bgp, uint32_t *groups, size_t size)
{
	struct midr_admission *m = bgp->midr_nds_info->admission;
	struct listnode *n;
	struct admission_entry *e;
	size_t count = 0, i;
	if (!m || !bgp->midr_nds_info->avoid_tier1)
		return 0;
	for (ALL_LIST_ELEMENTS_RO(m->entries, n, e)) {
		if (!(e->local_reasons & (1U << MIDR_SESSION_CL_ANCHOR)) ||
		    !e->target.group_id ||
		    !midr_admission_candidate_blocked(bgp, e->target.transport_addr))
			continue;
		for (i = 0; i < count; i++)
			if (groups[i] == e->target.group_id)
				break;
		if (i == count && count < size)
			groups[count++] = e->target.group_id;
	}
	return count;
}

unsigned int midr_admission_attach_pending(struct bgp *bgp)
{
	struct midr_admission *m = bgp->midr_nds_info->admission;
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

void midr_admission_retx_budget(struct bgp *bgp, struct ipaddr target, int count)
{
	struct admission_entry *e = find_entry(bgp, target);
	if (e)
		e->retx_budget = count;
}

int midr_admission_get_retx_budget(struct bgp *bgp, struct ipaddr target)
{
	struct admission_entry *e = find_entry(bgp, target);
	return e ? e->retx_budget : 0;
}

void midr_admission_show(struct bgp *bgp, struct vty *vty)
{
	static const char *const names[] = {
		"WAIT_DATA", "QUEUED/PROBING", "ALLOWED", "BLOCKED_TIER1", "INDETERMINATE"
	};
	struct midr_admission *m = bgp->midr_nds_info->admission;
	struct admission_entry *e;
	struct listnode *n;
	vty_out(vty, "MIDR avoid-tier1: %s (existing Established sessions retained)\n",
		bgp->midr_nds_info->avoid_tier1 ? "enabled" : "disabled");
	if (!m)
		return;
	vty_out(vty, "  Intent capacity: %u/%u; VRF: %u; instance context: %" PRIu64 "\n",
		(unsigned int)listcount(m->entries), ADMISSION_LIMIT, bgp->vrf_id, m->cookie);
	for (ALL_LIST_ELEMENTS_RO(m->entries, n, e)) {
		struct peer *peer = entry_peer(e);
		vty_out(vty, "  %pIA -> %pIA: %s (%s), IP2ASN %" PRIu64
			" Tier1 %" PRIu64 ", request %" PRIu64 ", owners local=0x%x remote=0x%x%s\n",
			&e->source, &e->target.transport_addr, names[e->state], e->detail,
			e->ip_generation, e->list_generation,
			e->token ? e->token->request_id : 0, e->local_reasons, e->remote_reasons,
			peer && peer->connection && peer->connection->status == Established ?
				" (Established retained)" : "");
	}
}
