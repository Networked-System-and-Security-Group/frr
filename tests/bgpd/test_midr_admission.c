// SPDX-License-Identifier: GPL-2.0-or-later
/* Real admission policy/ownership, fake trace transport and connection sink. */
#include "zebra.h"
#include "privs.h"
#include "vrf.h"
#include "bgpd/bgpd.h"
#include "bgpd/bgp_midr_admission.h"
#include "bgpd/bgp_midr_ctrl.h"
#include "bgpd/midr_trace_scheduler.h"
#include "bgpd/midr_ip2asn.h"
#include "bgpd/midr_tier1_list.h"

struct zebra_privs_t bgpd_privs = {};
struct event_loop *master;
static struct {
	midr_trace_done_cb done;
	void *arg;
	bool canceled;
} requests[16];
static unsigned int submitted, connected;
static struct peer *lookup_peer;
static struct midr_trace_net_context captured_context;
static time_t clock_offset;

int __real_clock_gettime(clockid_t clock_id, struct timespec *ts);
int __wrap_clock_gettime(clockid_t clock_id, struct timespec *ts);

int __wrap_clock_gettime(clockid_t clock_id, struct timespec *ts)
{
	int rc = __real_clock_gettime(clock_id, ts);
	if (!rc && clock_id == CLOCK_MONOTONIC)
		ts->tv_sec += clock_offset;
	return rc;
}

enum midr_trace_submit_rc __wrap_midr_trace_request_async(
	const struct prefix *target, const struct midr_trace_request_options *options,
	midr_trace_done_cb done, void *arg, uint64_t *id);
bool __wrap_midr_trace_cancel(uint64_t id);
enum midr_trace_cache_lookup_rc __wrap_midr_trace_cache_lookup(
	const struct prefix *target, const struct midr_trace_request_options *options,
	struct midr_trace_query_view *view, uint32_t *age);
struct peer *__wrap_peer_lookup(struct bgp *bgp, union sockunion *su);
bool __wrap_midr_nds_local_transport_get(const struct bgp *bgp, struct ipaddr *out);
enum midr_admission_result __wrap_midr_ctrl_connect(struct bgp *bgp,
	const struct midr_node_entry *target, enum midr_session_reason reason, bool nudge);

enum midr_trace_submit_rc __wrap_midr_trace_request_async(
	const struct prefix *target, const struct midr_trace_request_options *options,
	midr_trace_done_cb done, void *arg, uint64_t *id)
{
	(void)target;
	assert(submitted < array_size(requests));
	requests[submitted].done = done;
	requests[submitted].arg = arg;
	captured_context = options->context;
	*id = ++submitted;
	return MIDR_TRACE_SUBMIT_ACCEPTED;
}

bool __wrap_midr_trace_cancel(uint64_t id)
{
	assert(id && id <= submitted);
	requests[id - 1].canceled = true;
	/* Cancellation still has a callback. The test delivers it after teardown. */
	return true;
}

enum midr_trace_cache_lookup_rc __wrap_midr_trace_cache_lookup(
	const struct prefix *target, const struct midr_trace_request_options *options,
	struct midr_trace_query_view *view, uint32_t *age)
{
	(void)target;
	(void)options;
	(void)view;
	(void)age;
	return MIDR_TRACE_LOOKUP_MISS;
}

struct peer *__wrap_peer_lookup(struct bgp *bgp, union sockunion *su)
{
	(void)bgp;
	(void)su;
	return lookup_peer;
}

bool __wrap_midr_nds_local_transport_get(const struct bgp *bgp, struct ipaddr *out)
{
	*out = bgp->midr_nds_info->active_transport_addr;
	return true;
}

enum midr_admission_result __wrap_midr_ctrl_connect(struct bgp *bgp,
	const struct midr_node_entry *target, enum midr_session_reason reason, bool nudge)
{
	enum midr_admission_result result = midr_admission_gate(
		bgp, target, reason, nudge, false, false);
	assert(result == MIDR_ADMISSION_READY);
	connected++;
	return result;
}

static void load_file(const char *contents, bool tier1)
{
	char path[] = "/tmp/midr-admission-XXXXXX", error[256];
	int fd = mkstemp(path);
	assert(fd >= 0);
	assert(write(fd, contents, strlen(contents)) == (ssize_t)strlen(contents));
	close(fd);
	assert((tier1 ? midr_tier1_list_load_file(path, false, error, sizeof(error)) :
		midr_ip2asn_load_file(path, error, sizeof(error))) == 0);
	unlink(path);
}

static struct midr_trace_job_result job(const char *address)
{
	struct midr_trace_job_result out = {};
	out.status = MIDR_TRACE_OK;
	out.target_reached = true;
	out.stop_reason = MIDR_TRACE_STOP_REACHED;
	assert(str2prefix(address, &out.target));
	out.raw_path.hop_count = 1;
	out.raw_path.hops[0].ttl = 1;
	out.raw_path.hops[0].visible = true;
	out.raw_path.hops[0].address = out.target;
	return out;
}

static void deliver(unsigned int index, const struct midr_trace_job_result *result)
{
	struct midr_trace_delivery d = {};
	midr_trace_done_cb done = requests[index].done;
	void *arg = requests[index].arg;
	assert(done);
	requests[index].done = NULL;
	d.request_id = index + 1;
	d.status = requests[index].canceled ? MIDR_TRACE_ERR_CANCELED : MIDR_TRACE_OK;
	d.has_view = true;
	d.view.job = *result;
	done(&d, arg);
}

static void timeout(struct event *event)
{
	(void)event;
	assert(!"admission test timed out");
}

int main(void)
{
	struct bgp_master state = {};
	struct bgp bgp = {};
	struct bgp_midr_nds mi = {};
	struct midr_node_entry target = {};
	struct midr_tier1_result verdict;
	struct midr_trace_job_result clean, hit, partial;
	struct event *watchdog = NULL;
	struct event event;
	struct prefix source;
	struct peer peer = {};
	struct peer_connection connection = {};
	struct midr_manual_session manual = {};
	union sockunion local_su;
	struct midr_global_view view = {};
	struct midr_node_entry known = {};

	load_file("192.0.2.0/24 64512\n198.51.100.0/24 174\n2001:db8::/32 64512\n", false);
	load_file("MIDR-TIER1-ASNS 1\nLIST-ID admission-test\n174\n", true);
	clean = job("192.0.2.2");
	hit = job("198.51.100.2");
	assert(midr_admission_evaluate(&clean, &verdict) == MIDR_ADMISSION_READY);
	assert(midr_admission_evaluate(&hit, &verdict) == MIDR_ADMISSION_BLOCKED);
	partial = clean;
	partial.raw_path.hops[0].visible = false;
	assert(midr_admission_evaluate(&partial, &verdict) == MIDR_ADMISSION_READY);
	partial = job("203.0.113.2");
	assert(midr_admission_evaluate(&partial, &verdict) == MIDR_ADMISSION_READY);
	partial = hit;
	partial.target_reached = false;
	partial.stop_reason = MIDR_TRACE_STOP_MAX_HOPS;
	assert(midr_admission_evaluate(&partial, &verdict) == MIDR_ADMISSION_BLOCKED);
	partial = clean;
	partial.raw_path.output_truncated = true;
	assert(midr_admission_evaluate(&partial, &verdict) == MIDR_ADMISSION_READY);
	partial = clean;
	partial.raw_path.hop_count = 0;
	assert(midr_admission_evaluate(&partial, &verdict) == MIDR_ADMISSION_READY);
	partial = hit;
	partial.raw_path.hops[0].ttl = 2;
	assert(midr_admission_evaluate(&partial, &verdict) == MIDR_ADMISSION_BLOCKED);
	partial = clean;
	partial.status = MIDR_TRACE_ERR_EXEC_TIMEOUT;
	partial.target_reached = false;
	partial.raw_path.hops[0].visible = false;
	assert(midr_admission_evaluate(&partial, &verdict) == MIDR_ADMISSION_READY);
	partial = hit;
	partial.status = MIDR_TRACE_ERR_EXEC_TIMEOUT;
	assert(midr_admission_evaluate(&partial, &verdict) == MIDR_ADMISSION_BLOCKED);
	partial = clean;
	partial.status = MIDR_TRACE_ERR_SOCKET;
	assert(midr_admission_evaluate(&partial, &verdict) == MIDR_ADMISSION_PENDING);
	partial = clean;
	partial.raw_path.hop_count = 3;
	partial.target_reached = false;
	partial.stop_reason = MIDR_TRACE_STOP_MAX_HOPS;
	for (size_t i = 0; i < partial.raw_path.hop_count; i++) {
		partial.raw_path.hops[i].ttl = i + 1;
		partial.raw_path.hops[i].visible = false;
	}
	assert(midr_admission_evaluate(&partial, &verdict) == MIDR_ADMISSION_READY);
	partial = job("2001:db8::2");
	assert(midr_admission_evaluate(&partial, &verdict) == MIDR_ADMISSION_READY);

	master = event_master_create("MIDR admission test");
	bm = &state;
	state.master = master;
	bgp.midr_nds_info = &mi;
	bgp.peer = list_new();
	mi.bgp = &bgp;
	assert(str2prefix("192.0.2.1", &source));
	assert(midr_ipaddr_from_prefix(&source, &mi.active_transport_addr));
	target.node_id.family = AF_INET;
	target.node_id.prefixlen = 32; /* MANUAL legitimately has unknown RID. */
	target.has_transport_addr = true;
	assert(midr_ipaddr_from_prefix(&clean.target, &target.transport_addr));
	midr_admission_init(&bgp);
	assert(midr_admission_gate(&bgp, &target, MIDR_SESSION_MANUAL, false, false, false)
	       == MIDR_ADMISSION_READY && submitted == 0);
	midr_admission_set(&bgp, true);
	assert(midr_admission_gate(&bgp, &target, MIDR_SESSION_MANUAL, false, false, false)
	       == MIDR_ADMISSION_PENDING);
	assert(submitted == 1 && !connected);
	assert(prefix_same(&captured_context.source, &source));
	assert(captured_context.instance_cookie);
	assert(midr_admission_gate(&bgp, &target, MIDR_SESSION_MANUAL, false, false, false)
	       == MIDR_ADMISSION_PENDING && submitted == 1);
	deliver(0, &clean);
	event_add_timer(master, timeout, NULL, 5, &watchdog);
	while (!connected) {
		assert(event_fetch(master, &event));
		event_call(&event);
	}
	assert(connected == 1);
	midr_admission_reset(&bgp);
	assert(midr_ipaddr_from_prefix(&hit.target, &target.transport_addr));
	assert(midr_admission_gate(&bgp, &target, MIDR_SESSION_MANUAL, false, false, false)
	       == MIDR_ADMISSION_PENDING);
	deliver(1, &hit);
	assert(midr_admission_gate(&bgp, &target, MIDR_SESSION_MANUAL, false, false, false)
	       == MIDR_ADMISSION_BLOCKED && connected == 1);
	midr_admission_reset(&bgp);
	assert(midr_admission_gate(&bgp, &target, MIDR_SESSION_MANUAL, false, false, false)
	       == MIDR_ADMISSION_PENDING);
	/* Dataset publication invalidates outstanding callback ownership. */
	load_file("MIDR-TIER1-ASNS 1\nLIST-ID new-policy\n1299\n", true);
	assert(requests[2].canceled);
	deliver(2, &clean);
	assert(connected == 1);
	assert(midr_admission_gate(&bgp, &target, MIDR_SESSION_MANUAL, false, false, false)
	       == MIDR_ADMISSION_PENDING);
	midr_admission_finish(&bgp);
	assert(requests[3].canceled);
	deliver(3, &clean); /* no use-after-free, even after manager destruction */
	assert(connected == 1);

	/* Real connection guards: one permitted attempt, VRF mismatch, policy
	 * toggles and a partial replacement observation. No sockets. */
	midr_admission_init(&bgp);
	assert(midr_ipaddr_from_prefix(&clean.target, &target.transport_addr));
	peer.bgp = &bgp;
	peer.connection = &connection;
	connection.peer = &peer;
	connection.status = Idle;
	assert(midr_ipaddr_to_sockunion(&mi.active_transport_addr, &local_su));
	peer.update_source = &local_su;
	assert(midr_ipaddr_to_sockunion(&target.transport_addr, &connection.su));
	assert(midr_admission_check(&connection)); /* Native BGP is unaffected. */
	SET_FLAG(peer.flags, PEER_FLAG_MIDR_OVERLAY);
	lookup_peer = &peer;
	assert(midr_admission_gate(&bgp, &target, MIDR_SESSION_MANUAL, false, false, false)
	       == MIDR_ADMISSION_PENDING);
	assert(submitted == 5 && !midr_admission_check(&connection));
	deliver(4, &clean);
	assert(midr_admission_begin(&connection) && connection.midr_admission_permit);
	assert(midr_admission_check(&connection));
	peer.update_if = "unexpected-interface";
	assert(!midr_admission_check(&connection));
	peer.update_if = NULL;
	connection.su_local = &connection.su;
	assert(!midr_admission_check(&connection));
	connection.su_local = &local_su;
	midr_admission_forget_reason(&bgp, MIDR_SESSION_ATTACH);
	assert(midr_admission_is_manual(&bgp, target.transport_addr));
	assert(midr_admission_check(&connection));
	bgp.vrf_id = 1;
	assert(!midr_admission_check(&connection));
	assert(!midr_admission_peer_ready(&peer) && submitted == 5);
	bgp.vrf_id = VRF_DEFAULT;
	midr_admission_set(&bgp, false);
	assert(midr_admission_check(&connection));
	midr_admission_set(&bgp, true);
	assert(!midr_admission_check(&connection));
	assert(!midr_admission_peer_ready(&peer) && submitted == 6);
	partial = clean;
	partial.target_reached = false;
	partial.raw_path.hops[0].visible = false;
	deliver(5, &partial);
	assert(midr_admission_begin(&connection));
	assert(midr_admission_check(&connection));
	assert(submitted == 6); /* No re-probe merely because a hop is unknown. */
	/* Aging gates new attempts without revoking an active handshake. */
	connection.status = OpenConfirm;
	clock_offset = 31;
	assert(!midr_admission_peer_ready(&peer));
	assert(submitted == 6 && midr_admission_check(&connection));
	/* A configured Idle peer can have a passive clone still handshaking. */
	{
		struct peer incoming_peer = peer;
		struct peer_connection incoming = connection;
		incoming_peer.connection = &incoming;
		incoming.peer = &incoming_peer;
		incoming_peer.doppelganger = &peer;
		peer.doppelganger = &incoming_peer;
		connection.status = Idle;
		connection.midr_admission_permit = 0;
		assert(!midr_admission_peer_ready(&peer));
		assert(submitted == 6 && midr_admission_check(&incoming));
		peer.doppelganger = NULL;
	}
	/* Once the handshake is gone, a new attempt must refresh the path. */
	assert(!midr_admission_peer_ready(&peer) && submitted == 7);
	clock_offset = 0;
	midr_admission_reset(&bgp);
	assert(requests[6].canceled);
	deliver(6, &clean);

	/* The ANCHOR handler has already cleared its probe slots. Aborting the
	 * round must still cancel admission, while preserving another owner. */
	known.node_id = target.node_id;
	known.has_transport_addr = true;
	known.transport_addr = target.transport_addr;
	midr_node_hash_init(&view.nodes);
	midr_node_hash_add(&view.nodes, &known);
	mi.global_view = &view;
	assert(midr_admission_gate(&bgp, &target, MIDR_SESSION_CL_ANCHOR,
				   true, false, false) == MIDR_ADMISSION_PENDING);
	assert(submitted == 8);
	midr_nds_anchor_ctx_clear(&bgp);
	assert(requests[7].canceled);
	assert(!midr_admission_has_intent(&bgp, target.transport_addr));
	deliver(7, &clean);
	assert(midr_admission_gate(&bgp, &target, MIDR_SESSION_MANUAL,
				   false, false, false) == MIDR_ADMISSION_PENDING);
	assert(midr_admission_gate(&bgp, &target, MIDR_SESSION_CL_ANCHOR,
				   true, false, false) == MIDR_ADMISSION_PENDING);
	midr_nds_anchor_ctx_clear(&bgp);
	assert(submitted == 9 && !requests[8].canceled);
	assert(midr_admission_is_manual(&bgp, target.transport_addr));
	deliver(8, &clean);
	assert(midr_admission_begin(&connection));
	midr_admission_finish(&bgp);
	/* A reply-only ledger must not become a permanent local owner after an
	 * Established entry is reclaimed. Its active lease also expires at the
	 * connection guard, without waiting for the one-second manager tick. */
	mi.session_ledger = list_new();
	midr_nds_ledger_note(&bgp, target.transport_addr,
			     MIDR_SESSION_PEER_REQ_REPLY,
			     target.node_id.u.prefix4, target.asn, target.group_id);
	midr_admission_init(&bgp);
	assert(!midr_admission_peer_ready(&peer) && submitted == 9);
	midr_nds_ledger_drop(&bgp, target.transport_addr);
	assert(midr_admission_gate(&bgp, &target,
				   MIDR_SESSION_PEER_REQ_REPLY, false, true, false)
	       == MIDR_ADMISSION_PENDING && submitted == 10);
	deliver(9, &clean);
	connection.status = OpenConfirm;
	assert(midr_admission_begin(&connection));
	clock_offset = 11;
	assert(!midr_admission_check(&connection));
	assert(!midr_admission_peer_ready(&peer) && submitted == 10);
	clock_offset = 0;
	midr_admission_finish(&bgp);
	/* A manual command replayed during configuration must join an existing
	 * automatic observation even when its peer is already Established. */
	mi.manual_sessions = list_new();
	manual.transport = target.transport_addr;
	manual.remote_asn = 65002;
	listnode_add(mi.manual_sessions, &manual);
	midr_nds_ledger_note(&bgp, target.transport_addr,
			     MIDR_SESSION_PEER_REQ_REPLY,
			     target.node_id.u.prefix4, target.asn, target.group_id);
	midr_admission_init(&bgp);
	connection.status = Idle;
	assert(midr_admission_gate(&bgp, &target,
				   MIDR_SESSION_PEER_REQ_REPLY, false, true, false)
	       == MIDR_ADMISSION_PENDING && submitted == 11);
	deliver(10, &clean);
	connection.status = Established;
	midr_nds_manual_sessions_restore(&bgp);
	assert(midr_admission_is_manual(&bgp, target.transport_addr));
	assert(connected == 2);
	midr_admission_finish(&bgp);
	midr_nds_ledger_drop(&bgp, target.transport_addr);
	list_delete(&mi.manual_sessions);
	list_delete(&mi.session_ledger);
	midr_node_hash_del(&view.nodes, &known);
	midr_node_hash_fini(&view.nodes);
	mi.global_view = NULL;
	lookup_peer = NULL;
	assert(connected == 2);
	event_cancel(&watchdog);
	list_delete(&bgp.peer);
	midr_tier1_list_fini();
	midr_ip2asn_clear();
	event_master_free(master);
	bm = NULL;
	puts("MIDR admission tests passed");
	return 0;
}
