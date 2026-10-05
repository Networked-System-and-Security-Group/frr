/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Test-only CL dependencies. This is a behavioral harness, not an FRR ABI
 * test: NDS/PM/admission, logging, allocation and list storage are replaced.
 * The complete production CL .c is included without editing its contents.
 */
#ifndef CL_TEST_SUPPORT_H
#define CL_TEST_SUPPORT_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#ifdef CL_TEST_MIDRD
#define test_owner midr_g1
#define test_nds midr_nds
#else
#define test_owner bgp
#define test_nds bgp_midr_nds
#endif

#define IPV4_MAX_BITLEN 32
#define MIDR_CAP_GROUP_REP (1U << 3)
#define MIDR_CAP_BOOTSTRAP (1U << 2)
#define IPV4_ADDR_SAME(a, b) ((a)->s_addr == (b)->s_addr)
#define MIDR_JOIN_PROBE_WAIT_SECS 60
#define MIDR_JOIN_BATCH_MAX 3
#define array_size(a) (sizeof(a) / sizeof((a)[0]))
#define DEFINE_MTYPE_STATIC(...)
#define XCALLOC(type, size) test_calloc(1, size)
#define XFREE(type, ptr) do { free(ptr); (ptr) = NULL; } while (0)
#define MIDR_LOG(...) test_log(__VA_ARGS__)
#define MIDR_FLOW_LOG(...) test_log(__VA_ARGS__)
#define zlog_warn(...) test_log(__VA_ARGS__)
#define zlog_info(...) test_log(__VA_ARGS__)

static void *test_calloc(size_t count, size_t size)
{
	void *p = calloc(count, size);
	assert(p);
	return p;
}

static void test_log(const char *format, ...)
{
	(void)format;
}

struct listnode {
	struct listnode *next;
	void *data;
};
struct list {
	struct listnode *head, *tail;
	size_t count;
};

#define listcount(l) ((l)->count)
#define ALL_LIST_ELEMENTS_RO(l, n, d) \
	(n) = (l)->head, (d) = NULL; \
	(n) != NULL && (((d) = (n)->data), 1); \
	(n) = (n)->next, (d) = NULL
#define ALL_LIST_ELEMENTS(l, n, nextn, d) \
	(n) = (l)->head, (d) = NULL; \
	(n) != NULL && (((d) = (n)->data), ((nextn) = (n)->next), 1); \
	(n) = (nextn), (d) = NULL

static struct list *list_new(void)
{
	return test_calloc(1, sizeof(struct list));
}

static void listnode_add(struct list *list, void *data)
{
	struct listnode *node = test_calloc(1, sizeof(*node));
	node->data = data;
	if (list->tail)
		list->tail->next = node;
	else
		list->head = node;
	list->tail = node;
	list->count++;
}

/* As in midr_nds_cl_nodes_getter(), returned lists borrow node entries. */
static void list_delete(struct list **list)
{
	struct listnode *node = (*list)->head;
	while (node) {
		struct listnode *next = node->next;
		free(node);
		node = next;
	}
	free(*list);
	*list = NULL;
}

struct prefix {
	uint8_t family, prefixlen;
	union { struct in_addr prefix4; } u;
};
struct ipaddr {
	uint32_t v4;
};

static bool prefix_same(const struct prefix *a, const struct prefix *b)
{
	return a->family == b->family && a->prefixlen == b->prefixlen &&
	       a->u.prefix4.s_addr == b->u.prefix4.s_addr;
}

static int prefix_cmp(const struct prefix *a, const struct prefix *b)
{
	uint32_t av = ntohl(a->u.prefix4.s_addr), bv = ntohl(b->u.prefix4.s_addr);
	return av < bv ? -1 : av != bv;
}

struct midr_nds_link_metrics {
	uint32_t rtt_us, rtt_min_us, rtt_max_us;
	double loss_rate;
	uint32_t bw_score;
};
enum midr_link_status { MIDR_LINK_UP, MIDR_LINK_DEGRADED, MIDR_LINK_DOWN };
struct midr_link_entry {
	struct prefix remote_node_id;
	enum midr_link_status status;
	struct midr_nds_link_metrics long_term;
};
struct midr_node_entry {
	struct prefix node_id;
	uint32_t group_id;
	struct ipaddr transport_addr;
	bool has_transport_addr, is_self, is_adjacent;
};
struct midr_global_view {
	struct list *nodes, *links;
};
struct midr_rep_entry {
	uint32_t group_id;
	struct ipaddr rep_transport;
	struct in_addr rep_rid;
	bool members_listed;
};
enum midr_trigger_type {
	MIDR_TRIGGER_REP_PROBE_DONE = 1, MIDR_TRIGGER_MEMBER_PROBE_DONE,
	MIDR_TRIGGER_CAPABILITY_UPDATE, MIDR_TRIGGER_PERIODIC_SYNC,
	MIDR_TRIGGER_NODE_CHANGE, MIDR_TRIGGER_ANCHOR_PROBE_DONE,
	MIDR_TRIGGER_ISOLATED, MIDR_TRIGGER_ADMISSION_CHANGE
};
enum midr_decision_type {
	MIDR_DECISION_RECOMMEND = 1, MIDR_DECISION_JOIN, MIDR_DECISION_LEAVE,
	MIDR_DECISION_SPLIT, MIDR_DECISION_CREATE, MIDR_DECISION_REP_ELECT,
	MIDR_DECISION_REP_RESIGN, MIDR_DECISION_ANCHOR, MIDR_DECISION_RECONNECT
};
enum midr_join_phase {
	MIDR_JOIN_IDLE, MIDR_JOIN_PROBING_REPS, MIDR_JOIN_PROBING_MEMBERS
};
struct midr_node_evidence {
	struct prefix node_id;
	struct midr_nds_link_metrics metrics;
};
struct midr_rep_identity {
	uint32_t group_id;
	struct prefix node_id;
};
struct midr_cluster_decision {
	enum midr_decision_type decision_type;
	uint32_t new_group_id, old_group_id;
	struct prefix recommended_rep_id;
	struct list *evidence, *candidate_reps, *anchor_reps;
};
struct test_owner;
typedef void (*midr_global_view_cb)(struct test_owner *, enum midr_trigger_type,
				   const struct midr_global_view *);
struct test_nds {
	struct list *rep_dir;
	uint32_t local_group_id, join_group_id, local_capabilities;
	uint32_t config_group_id;
	uint32_t anchor_group_id[2];
	time_t group_settled_at;
	enum midr_join_phase join_phase;
	struct event *t_rep_probe_done, *t_member_probe_done, *t_rep_refresh;
	struct event *t_anchor_probe_done;
	uint32_t join_batch_gid[MIDR_JOIN_BATCH_MAX];
	bool join_in_progress, join_intent;
	struct list *bootstrap_list;
	struct listnode *bootstrap_cur;
	midr_global_view_cb cl_callback;
};
struct test_owner {
	struct test_nds *midr_nds_info;
	struct in_addr router_id;
};

/* Small event scheduler: the slot is cleared before callback delivery, as in
 * FRR. Only production request/callback code decides when to arm timers. */
struct event {
	void (*callback)(struct event *);
	void *arg;
	unsigned int seconds;
};
#define EVENT_ARG(e) ((e)->arg)
static void event_cancel(struct event **slot)
{
	free(*slot);
	*slot = NULL;
}
static void event_add_timer(void *master, void (*callback)(struct event *),
			    void *arg, unsigned int seconds, struct event **slot)
{
	(void)master;
	assert(!*slot);
	*slot = test_calloc(1, sizeof(**slot));
	(*slot)->callback = callback;
	(*slot)->arg = arg;
	(*slot)->seconds = seconds;
}
static __attribute__((unused)) void *midr_g1_master(void) { return NULL; }
static struct { void *master; } test_bgp_master __attribute__((unused));
#define bm (&test_bgp_master)

static void midr_nds_notify_cl(struct test_owner *owner, enum midr_trigger_type trigger)
{
	extern struct midr_global_view view;
	owner->midr_nds_info->cl_callback(owner, trigger, &view);
}

enum { MIDR_CTRL_MEMBER_LIST_REQ = 1 };
static const unsigned int midr_ctrl_retx_default = 1;
static uint32_t requested_groups[128];
static size_t request_count;
static void midr_ctrl_enqueue_request(struct test_owner *owner, struct ipaddr dst,
				     unsigned int type, uint32_t gid, const void *params)
{
	(void)owner;
	(void)dst;
	(void)type;
	(void)params;
	assert(request_count < array_size(requested_groups));
	requested_groups[request_count++] = gid;
}
static void midr_nds_anchor_ctx_clear(struct test_owner *owner)
{
	memset(owner->midr_nds_info->anchor_group_id, 0,
	       sizeof(owner->midr_nds_info->anchor_group_id));
	event_cancel(&owner->midr_nds_info->t_anchor_probe_done);
}
static bool midr_ipaddr_valid_locator(const struct ipaddr *addr) { return addr->v4 != 0; }
static bool midr_ipaddr_same(const struct ipaddr *a, const struct ipaddr *b)
{
	return a->v4 == b->v4;
}
static int ipaddr_family(const struct ipaddr *addr) { (void)addr; return AF_INET; }
static bool midr_nds_local_transport_get(const struct test_owner *owner, struct ipaddr *out)
{
	(void)owner;
	out->v4 = 1;
	return true;
}
static void midr_join_settle_group(struct test_owner *owner, uint32_t gid)
{
	owner->midr_nds_info->local_group_id = gid;
	owner->midr_nds_info->join_phase = MIDR_JOIN_IDLE;
}
struct midr_bootstrap_entry { bool failed; };
#define listhead(l) ((l)->head)
static void midr_bootstrap_start_attempt(struct test_owner *owner) { (void)owner; }
static bool execute_nds;
static void test_apply_recommend(struct test_owner *, const struct midr_cluster_decision *);

static struct list *midr_nds_cl_nodes_getter(const struct midr_global_view *gv)
{
	struct list *out = list_new();
	struct listnode *node;
	struct midr_node_entry *entry;
	for (ALL_LIST_ELEMENTS_RO(gv->nodes, node, entry))
		listnode_add(out, entry);
	return out;
}

static struct ipaddr blocked[16];
static size_t blocked_count;
static bool midr_admission_candidate_blocked(const struct test_owner *owner,
					     struct ipaddr address)
{
	(void)owner;
	for (size_t i = 0; i < blocked_count; i++)
		if (blocked[i].v4 == address.v4)
			return true;
	return false;
}

static size_t midr_admission_anchor_groups(struct test_owner *owner,
					 uint32_t *groups, size_t capacity)
{
	(void)owner;
	(void)groups;
	(void)capacity;
	return 0;
}

static time_t monotime(struct timespec *unused)
{
	(void)unused;
	return 1000;
}

/* Copy borrowed identities synchronously: CL frees the lists on return. */
static struct {
	enum midr_decision_type type;
	uint32_t gid, old_gid;
	struct prefix representative;
	struct midr_rep_identity anchors[2];
	size_t anchor_count;
	uint32_t candidates[MIDR_JOIN_BATCH_MAX];
	size_t candidate_count;
} decisions[16];
static size_t decision_count;

static void midr_nds_on_cluster_decision(struct test_owner *owner,
					const struct midr_cluster_decision *d)
{
	struct listnode *node;
	struct midr_rep_identity *identity;
	size_t index = decision_count++;
	(void)owner;
	assert(index < array_size(decisions));
	decisions[index].type = d->decision_type;
	decisions[index].gid = d->new_group_id;
	decisions[index].old_gid = d->old_group_id;
	decisions[index].representative = d->recommended_rep_id;
	if (d->anchor_reps)
		for (ALL_LIST_ELEMENTS_RO(d->anchor_reps, node, identity)) {
			size_t slot = decisions[index].anchor_count++;
			assert(slot < array_size(decisions[index].anchors));
			decisions[index].anchors[slot] = *identity;
		}
	if (d->candidate_reps)
		for (ALL_LIST_ELEMENTS_RO(d->candidate_reps, node, identity)) {
			size_t slot = decisions[index].candidate_count++;
			assert(slot < array_size(decisions[index].candidates));
			decisions[index].candidates[slot] = identity->group_id;
		}
	if (execute_nds && d->decision_type == MIDR_DECISION_RECOMMEND)
		test_apply_recommend(owner, d);
}

#endif
