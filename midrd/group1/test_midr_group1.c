// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Group 1 inside midrd: error paths across the public Session and Topology
 * services.  The real group-1 modules run against stubs of those services,
 * so registration and upsert failures can be injected.
 */
#include <zebra.h>

#include "command.h"
#include "frrevent.h"
#include "memory.h"

#include "midrd/midr-context.h"
#include "midrd/midr-session.h"
#include "midrd/midr-spf.h"
#include "midrd/midr-topology.h"
#include "midrd/group1/midr_g1.h"
#include "midrd/group1/midr_nds.h"
#include "midrd/group1/midr_nds_facts.h"

#define LOCAL_NODE_ID 0x0100000aU /* 10.0.0.1 */
#define REMOTE_NODE_ID 0x0200000aU /* 10.0.0.2 */
#define OTHER_NODE_ID 0x0300000aU /* 10.0.0.3 */
#define MIDR_PORT 5858

static struct event_loop *master;
static char ctx_storage;
static struct midr_context *const ctx = (struct midr_context *)&ctx_storage;

/* ------------------------------------------------------------------------
 * Stubs of midrd's public services
 * ---------------------------------------------------------------------- */

static struct {
	int observer_rc;
	int provider_rc;
	int node_upsert_rc;
	int link_upsert_rc;

	const struct midr_session_observer_ops *observer;
	void *observer_arg;
	midr_topology_snapshot_get_cb snapshot_get;
	midr_topology_snapshot_release_cb snapshot_release;

	/* One remote session, as the Session service would report it. */
	struct ipaddr session_addr;
	enum midr_session_state session_state;
	uint32_t session_node_id;

	unsigned int connects;
	unsigned int disconnects;
	unsigned int node_upserts;
	unsigned int node_withdraws;
	unsigned int link_upserts;
	unsigned int link_withdraws;
} stub;

uint32_t midr_context_node_id(const struct midr_context *c)
{
	assert(c == ctx);
	return LOCAL_NODE_ID;
}

bool midr_context_listen_endpoint(const struct midr_context *c,
				  struct ipaddr *address, uint16_t *port)
{
	assert(c == ctx);
	if (address) {
		memset(address, 0, sizeof(*address));
		address->ipa_type = IPADDR_V4;
		address->ipaddr_v4.s_addr = htonl(0xc0000201); /* 192.0.2.1 */
	}
	if (port)
		*port = MIDR_PORT;
	return true;
}

int midr_session_connect(struct midr_context *c,
			 const struct midr_session_endpoint *remote)
{
	assert(c == ctx && remote->port == MIDR_PORT);
	stub.connects++;
	return 0;
}

int midr_session_disconnect(struct midr_context *c,
			    const struct midr_session_endpoint *remote,
			    enum midr_session_close_reason reason)
{
	(void)reason;
	assert(c == ctx && remote->port == MIDR_PORT);
	stub.disconnects++;
	return 0;
}

int midr_session_status_get(struct midr_context *c,
			    const struct midr_session_endpoint *remote,
			    struct midr_session_status *status)
{
	assert(c == ctx);
	if (ipaddr_cmp(&remote->address, &stub.session_addr))
		return -ENOENT;
	memset(status, 0, sizeof(*status));
	status->remote = *remote;
	status->state = stub.session_state;
	status->remote_node_id = stub.session_node_id;
	return 0;
}

int midr_session_observer_register(struct midr_context *c,
				   const struct midr_session_observer_ops *ops,
				   void *arg)
{
	assert(c == ctx && ops);
	if (stub.observer_rc)
		return stub.observer_rc;
	assert(!stub.observer);
	stub.observer = ops;
	stub.observer_arg = arg;
	return 0;
}

void midr_session_observer_unregister(struct midr_context *c)
{
	assert(c == ctx);
	stub.observer = NULL;
	stub.observer_arg = NULL;
}

int midr_topology_provider_register(
	struct midr_context *c, midr_topology_snapshot_get_cb snapshot_get,
	midr_topology_snapshot_release_cb snapshot_release)
{
	assert(c == ctx && snapshot_get && snapshot_release);
	if (stub.provider_rc)
		return stub.provider_rc;
	assert(!stub.snapshot_get);
	stub.snapshot_get = snapshot_get;
	stub.snapshot_release = snapshot_release;
	return 0;
}

void midr_topology_provider_unregister(struct midr_context *c)
{
	assert(c == ctx);
	stub.snapshot_get = NULL;
	stub.snapshot_release = NULL;
}

int midr_topology_node_upsert(struct midr_context *c,
			      const struct midr_node_update *node)
{
	assert(c == ctx && node->node_id == LOCAL_NODE_ID);
	stub.node_upserts++;
	return stub.node_upsert_rc;
}

int midr_topology_node_withdraw(struct midr_context *c, uint32_t node_id,
				uint64_t version)
{
	(void)node_id;
	(void)version;
	assert(c == ctx);
	stub.node_withdraws++;
	return 0;
}

int midr_topology_link_upsert(struct midr_context *c,
			      const struct midr_link_update *link)
{
	(void)link;
	assert(c == ctx);
	stub.link_upserts++;
	return stub.link_upsert_rc;
}

int midr_topology_link_withdraw(struct midr_context *c,
				const struct midr_link_key *key,
				uint64_t version)
{
	(void)key;
	(void)version;
	assert(c == ctx);
	stub.link_withdraws++;
	return 0;
}

int midr_topology_resync_begin(struct midr_context *c,
			       enum midr_topology_resync_reason reason)
{
	(void)reason;
	assert(c == ctx);
	return 0;
}

int midr_spf_results_get(struct midr_context *c,
			 const struct midr_spf_results **out)
{
	(void)c;
	*out = NULL;
	return -ENOENT;
}

void midr_spf_results_release(const struct midr_spf_results **results)
{
	*results = NULL;
}

uint64_t midr_spf_results_generation(const struct midr_spf_results *results)
{
	(void)results;
	return 0;
}

size_t midr_spf_results_count(const struct midr_spf_results *results)
{
	(void)results;
	return 0;
}

const struct midr_spf_route *
midr_spf_results_at(const struct midr_spf_results *results, size_t index)
{
	(void)results;
	(void)index;
	return NULL;
}

/* ------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static void fence_cb(struct event *event)
{
	*(bool *)EVENT_ARG(event) = true;
}

/* Run everything already on the event queue.  Events run in FIFO order, so
 * the queue is drained once a fence event added last has run. */
static void run_queued(void)
{
	struct event ev;
	bool done = false;

	event_add_event(master, fence_cb, &done, 0, NULL);
	while (!done && event_fetch(master, &ev))
		event_call(&ev);
	assert(done);
}

static struct ipaddr v4(uint32_t host_order)
{
	struct ipaddr a = { .ipa_type = IPADDR_V4 };

	a.ipaddr_v4.s_addr = htonl(host_order);
	return a;
}

/* Deliver a Session service state change the way midrd does: synchronously
 * from its receive path. */
static void notify(const struct ipaddr *addr, enum midr_session_state state,
		   uint32_t node_id, int last_error)
{
	struct midr_session_status status = {};

	assert(stub.observer && stub.observer->state_changed);
	status.remote.address = *addr;
	status.remote.port = MIDR_PORT;
	status.state = state;
	status.remote_node_id = node_id;
	status.last_error = last_error;
	stub.observer->state_changed(ctx, &status, stub.observer_arg);
}

static void session_set(const struct ipaddr *addr,
			enum midr_session_state state, uint32_t node_id)
{
	stub.session_addr = *addr;
	stub.session_state = state;
	stub.session_node_id = node_id;
}

/* ------------------------------------------------------------------------
 * Cases
 * ---------------------------------------------------------------------- */

static void test_observer_registration_failure(void)
{
	stub.observer_rc = -EALREADY;
	assert(midr_group1_init(master, ctx) == -EALREADY);
	assert(!midr_g1_get());
	assert(!stub.observer && !stub.snapshot_get);
	stub.observer_rc = 0;
	printf("observer registration failure rolls back: PASS\n");
}

static void test_provider_registration_failure(void)
{
	stub.provider_rc = -EALREADY;
	assert(midr_group1_init(master, ctx) == -EALREADY);
	assert(!midr_g1_get());
	/* The observer registered first is released again. */
	assert(!stub.observer && !stub.snapshot_get);
	stub.provider_rc = 0;
	printf("provider registration failure rolls back: PASS\n");
}

static void test_session_events(struct midr_g1 *g1)
{
	struct ipaddr remote = v4(0xc0000202); /* 192.0.2.2 */
	struct ipaddr unknown = v4(0xc0000209);
	struct in_addr rid = { .s_addr = REMOTE_NODE_ID };
	struct midr_g1_peer *peer;
	unsigned int connects = stub.connects;

	peer = midr_g1_peer_create(g1, &remote, rid, 0);
	assert(peer && peer->requested && !peer->established);
	assert(stub.connects == connects + 1);

	/* Up: handled on the event queue, not inside the callback. */
	session_set(&remote, MIDR_SESSION_ESTABLISHED, REMOTE_NODE_ID);
	notify(&remote, MIDR_SESSION_ESTABLISHED, REMOTE_NODE_ID, 0);
	assert(!peer->established);
	run_queued();
	assert(peer->established && peer->established_count == 1);
	assert(peer->remote_id.s_addr == REMOTE_NODE_ID);

	/* Down. */
	session_set(&remote, MIDR_SESSION_DOWN, 0);
	notify(&remote, MIDR_SESSION_DOWN, 0, -ETIMEDOUT);
	run_queued();
	assert(!peer->established && peer->dropped_count == 1);
	assert(peer->last_reset == -ETIMEDOUT);

	/* A late Up: the session dropped again before the event ran. */
	notify(&remote, MIDR_SESSION_ESTABLISHED, REMOTE_NODE_ID, 0);
	run_queued();
	assert(!peer->established && peer->established_count == 1);

	/* Up again, then the remote turns out to be a different node. */
	session_set(&remote, MIDR_SESSION_ESTABLISHED, REMOTE_NODE_ID);
	notify(&remote, MIDR_SESSION_ESTABLISHED, REMOTE_NODE_ID, 0);
	run_queued();
	assert(peer->established && peer->established_count == 2);
	session_set(&remote, MIDR_SESSION_IDENTITY_MISMATCH, OTHER_NODE_ID);
	notify(&remote, MIDR_SESSION_IDENTITY_MISMATCH, OTHER_NODE_ID, 0);
	run_queued();
	assert(!peer->established && peer->dropped_count == 2);

	/* Events for a transport group 1 never asked for are ignored. */
	notify(&unknown, MIDR_SESSION_ESTABLISHED, OTHER_NODE_ID, 0);
	notify(&unknown, MIDR_SESSION_DOWN, 0, 0);
	run_queued();
	assert(!midr_g1_peer_lookup(g1, &unknown));

	/* An Up still queued when the peer is deleted must not touch it. */
	session_set(&remote, MIDR_SESSION_ESTABLISHED, REMOTE_NODE_ID);
	notify(&remote, MIDR_SESSION_ESTABLISHED, REMOTE_NODE_ID, 0);
	midr_g1_peer_delete(peer);
	run_queued();
	assert(!midr_g1_peer_lookup(g1, &remote));
	printf("session up/down/identity mismatch/late events: PASS\n");
}

static void test_node_upsert_failure(struct midr_g1 *g1)
{
	struct midr_nds *mi = g1->midr_nds_info;
	struct midr_nds_facts *f = mi->facts;
	struct midr_topology_snapshot snap;
	unsigned int upserts = stub.node_upserts;

	assert(f && stub.snapshot_get);
	/* Not in a group yet: nothing to publish, an empty snapshot is right. */
	assert(stub.snapshot_get(ctx, &snap) == 0);
	assert(snap.node_count == 0 && !snap.nodes);
	stub.snapshot_release(ctx, &snap);

	mi->local_group_id = 7;
	mi->transport_active = true;
	mi->active_transport_addr = v4(0xc0000201);

	stub.node_upsert_rc = -EAGAIN;
	midr_nds_report_node(g1, MIDR_ORIGIN_GROUP_UPDATE);
	assert(stub.node_upserts == upserts + 1);
	assert(f->node_pending && !f->node_reported);

	/* The fact survives the failure and a resync snapshot carries it. */
	assert(stub.snapshot_get(ctx, &snap) == 0);
	assert(snap.node_count == 1 && snap.nodes[0].node_id == LOCAL_NODE_ID);
	assert(snap.nodes[0].group_id == 7);
	stub.snapshot_release(ctx, &snap);

	/* The next report of the unchanged fact retries it. */
	stub.node_upsert_rc = 0;
	midr_nds_report_node(g1, MIDR_ORIGIN_GROUP_UPDATE);
	assert(stub.node_upserts == upserts + 2);
	assert(f->node_reported && !f->node_pending);
	printf("node upsert failure keeps the fact for retry and resync: PASS\n");
}

static void test_terminate_with_queued_events(struct midr_g1 *g1)
{
	struct ipaddr remote = v4(0xc0000203); /* 192.0.2.3 */
	struct in_addr rid = { .s_addr = OTHER_NODE_ID };
	struct midr_g1_peer *peer;
	unsigned int disconnects, node_upserts, link_upserts;

	peer = midr_g1_peer_create(g1, &remote, rid, 0);
	assert(peer);
	session_set(&remote, MIDR_SESSION_ESTABLISHED, OTHER_NODE_ID);
	notify(&remote, MIDR_SESSION_ESTABLISHED, OTHER_NODE_ID, 0);
	run_queued();
	assert(peer->established);

	/* Events queued by the Session service just before shutdown. */
	notify(&remote, MIDR_SESSION_DOWN, 0, 0);
	notify(&remote, MIDR_SESSION_ESTABLISHED, OTHER_NODE_ID, 0);

	disconnects = stub.disconnects;
	node_upserts = stub.node_upserts;
	link_upserts = stub.link_upserts;
	midr_group1_terminate();
	assert(!midr_g1_get());
	assert(!stub.observer && !stub.snapshot_get);
	/* Sessions stay open: group 2 still floods its withdrawals over them. */
	assert(stub.disconnects == disconnects);

	/* The queued events find group 1 gone and only free themselves. */
	run_queued();
	assert(stub.node_upserts == node_upserts);
	assert(stub.link_upserts == link_upserts);
	printf("terminate with queued session events: PASS\n");
}

int main(void)
{
	struct midr_g1 *g1;

	setvbuf(stdout, NULL, _IONBF, 0);
	master = event_master_create(NULL);
	cmd_init(1);

	test_observer_registration_failure();
	test_provider_registration_failure();

	assert(midr_group1_init(master, ctx) == 0);
	g1 = midr_g1_get();
	assert(g1 && stub.observer && stub.snapshot_get);

	test_session_events(g1);
	test_node_upsert_failure(g1);
	test_terminate_with_queued_events(g1);

	cmd_terminate();
	event_master_free(master);
	printf("midr group1 test: PASS\n");
	return 0;
}
