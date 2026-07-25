// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR topology provider input, Local Fact, and resynchronization state.
 */

#include <zebra.h>

#include <errno.h>

#include "command.h"
#include "hash.h"
#include "jhash.h"
#include "linklist.h"
#include "log.h"
#include "memory.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_midr.h"
#include "bgpd/bgp_midr_private.h"

#define MIDR_EVENT_QUEUE_LIMIT 4096U
#define MIDR_RESYNC_QUEUE_LIMIT 4096U
#define MIDR_RESYNC_RETRY_MSEC 1000U

DEFINE_MTYPE_STATIC(BGPD, MIDR_INPUT, "MIDR topology input");
DEFINE_MTYPE_STATIC(BGPD, MIDR_EVENT, "MIDR topology event");
DEFINE_MTYPE_STATIC(BGPD, MIDR_FACT_TABLE, "MIDR Local Fact table");
DEFINE_MTYPE_STATIC(BGPD, MIDR_NODE_ENTRY, "MIDR node entry");
DEFINE_MTYPE_STATIC(BGPD, MIDR_LINK_ENTRY, "MIDR link entry");

enum midr_event_type {
	MIDR_EVENT_NODE_UPSERT,
	MIDR_EVENT_NODE_WITHDRAW,
	MIDR_EVENT_LINK_UPSERT,
	MIDR_EVENT_LINK_WITHDRAW,
};

enum midr_provider_state {
	MIDR_PROVIDER_UNKNOWN,
	MIDR_PROVIDER_AVAILABLE,
	MIDR_PROVIDER_UNAVAILABLE,
};

struct midr_event {
	enum midr_event_type type;
	union {
		struct midr_node_update node;
		struct {
			uint32_t node_id;
			uint64_t version;
		} node_withdraw;
		struct midr_link_update link;
		struct {
			struct midr_link_key key;
			uint64_t version;
		} link_withdraw;
	} u;
};

struct midr_node_entry {
	struct midr_node_update data;
	bool active;
};

struct midr_link_entry {
	struct midr_link_update data;
	bool active;
};

struct midr_fact_table {
	struct hash *nodes;
	struct hash *links;
};

struct midr_input_store {
	struct midr_context *ctx;
	struct midr_fact_table *active;
	struct list *normal_queue;
	struct list *resync_queue;
	struct event *t_process;
	struct event *t_resync;
	enum midr_input_state state;
	enum midr_topology_resync_reason reason;
	enum midr_provider_state provider_state;
	uint32_t owner_node_id;
	uint64_t snapshot_version;
	size_t normal_queue_limit;
	size_t resync_queue_limit;
	bool startup_probe;
	bool identity_restart_pending;
	uint64_t event_enqueued;
	uint64_t event_processed;
	uint64_t event_ignored_old;
	uint64_t event_rejected_full;
	uint64_t event_rejected_sync;
	uint64_t event_dropped_resync;
	uint64_t resync_attempts;
	uint64_t resync_commits;
	uint64_t resync_failures;
	uint64_t queue_overflows;
	uint64_t identity_restarts;
};

/*
 * The topology provider lives in the first-group implementation.  These weak
 * definitions keep standalone bgpd and second-group tests linkable until that
 * provider is present; a strong provider definition overrides them.
 */
__attribute__((weak)) int midr_topology_snapshot_get(struct midr_context *ctx,
						     struct midr_topology_snapshot *snapshot)
{
	(void)ctx;

	if (!snapshot)
		return -EINVAL;
	memset(snapshot, 0, sizeof(*snapshot));
	return -ENOSYS;
}

__attribute__((weak)) void midr_topology_snapshot_release(struct midr_context *ctx,
							  struct midr_topology_snapshot *snapshot)
{
	(void)ctx;

	if (snapshot)
		memset(snapshot, 0, sizeof(*snapshot));
}

static unsigned int midr_node_hash_key(const void *data)
{
	const struct midr_node_entry *entry = data;

	return jhash_1word(entry->data.node_id, 0);
}

static bool midr_node_hash_cmp(const void *data1, const void *data2)
{
	const struct midr_node_entry *a = data1;
	const struct midr_node_entry *b = data2;

	return a->data.node_id == b->data.node_id;
}

static unsigned int midr_link_hash_key(const void *data)
{
	const struct midr_link_entry *entry = data;
	const struct midr_link_key *key = &entry->data.key;
	uint32_t words[4];

	words[0] = key->local_node_id;
	words[1] = key->remote_node_id;
	words[2] = (uint32_t)(key->link_id >> 32);
	words[3] = (uint32_t)key->link_id;

	return jhash2(words, array_size(words), 0);
}

static bool midr_link_key_same(const struct midr_link_key *a, const struct midr_link_key *b)
{
	return a->local_node_id == b->local_node_id && a->remote_node_id == b->remote_node_id &&
	       a->link_id == b->link_id;
}

static bool midr_link_hash_cmp(const void *data1, const void *data2)
{
	const struct midr_link_entry *a = data1;
	const struct midr_link_entry *b = data2;

	return midr_link_key_same(&a->data.key, &b->data.key);
}

static void midr_event_free(void *data)
{
	XFREE(MTYPE_MIDR_EVENT, data);
}

static void midr_node_entry_free(void *data)
{
	XFREE(MTYPE_MIDR_NODE_ENTRY, data);
}

static void midr_link_entry_free(void *data)
{
	XFREE(MTYPE_MIDR_LINK_ENTRY, data);
}

static struct midr_fact_table *midr_fact_table_new(void)
{
	struct midr_fact_table *table;

	table = XCALLOC(MTYPE_MIDR_FACT_TABLE, sizeof(*table));
	table->nodes = hash_create(midr_node_hash_key, midr_node_hash_cmp, "MIDR node table");
	table->links = hash_create(midr_link_hash_key, midr_link_hash_cmp, "MIDR link table");
	return table;
}

static void midr_fact_table_free(struct midr_fact_table **tablep)
{
	struct midr_fact_table *table;

	if (!tablep || !*tablep)
		return;

	table = *tablep;
	hash_clean_and_free(&table->nodes, midr_node_entry_free);
	hash_clean_and_free(&table->links, midr_link_entry_free);
	XFREE(MTYPE_MIDR_FACT_TABLE, table);
	*tablep = NULL;
}

static bool midr_policy_valid(enum midr_policy_state state)
{
	return state == MIDR_POLICY_ALLOWED || state == MIDR_POLICY_BLOCKED;
}

static bool midr_ipaddr_present(const struct ipaddr *address)
{
	return address->ipa_type == IPADDR_V4 || address->ipa_type == IPADDR_V6;
}

int midr_validate_node_update(uint32_t local_node_id, const struct midr_node_update *node)
{
	if (!local_node_id)
		return -ENOENT;
	if (!node || !node->node_id || node->node_id != local_node_id)
		return -EINVAL;
	if (!midr_policy_valid(node->policy_state))
		return -EINVAL;
	if (node->has_transport_address) {
		if (!midr_ipaddr_present(&node->transport_address))
			return -EINVAL;
	} else if (node->transport_address.ipa_type != IPADDR_NONE) {
		return -EINVAL;
	}

	return 0;
}

int midr_validate_node_withdraw(uint32_t local_node_id, uint32_t node_id)
{
	if (!local_node_id)
		return -ENOENT;
	if (!node_id || node_id != local_node_id)
		return -EINVAL;

	return 0;
}

int midr_validate_link_update(uint32_t local_node_id, const struct midr_link_update *link)
{
	if (!local_node_id)
		return -ENOENT;
	if (!link || !link->key.local_node_id || link->key.local_node_id != local_node_id ||
	    !link->key.remote_node_id || link->key.remote_node_id == link->key.local_node_id)
		return -EINVAL;
	if (link->local_ifindex < 0 || !midr_ipaddr_present(&link->link_local_address) ||
	    !midr_ipaddr_present(&link->link_remote_address) ||
	    link->link_local_address.ipa_type != link->link_remote_address.ipa_type)
		return -EINVAL;
	if (!link->metrics.has_rtt_us || !link->metrics.has_loss_ppm ||
	    !link->metrics.has_available_bandwidth_kbps)
		return -EINVAL;
	if (!link->metrics.rtt_us || !link->metrics.available_bandwidth_kbps ||
	    link->metrics.loss_ppm >= 1000000)
		return -EINVAL;
	if (!midr_policy_valid(link->policy_state))
		return -EINVAL;

	return 0;
}

int midr_validate_link_withdraw(uint32_t local_node_id, const struct midr_link_key *key)
{
	if (!local_node_id)
		return -ENOENT;
	if (!key || !key->local_node_id || key->local_node_id != local_node_id ||
	    !key->remote_node_id || key->remote_node_id == key->local_node_id)
		return -EINVAL;

	return 0;
}

static void midr_apply_node_upsert(struct midr_fact_table *table,
				   const struct midr_node_update *node, uint64_t *ignored_old)
{
	struct midr_node_entry lookup = { .data.node_id = node->node_id };
	struct midr_node_entry *entry;

	entry = hash_lookup(table->nodes, &lookup);
	if (entry && node->version <= entry->data.version) {
		(*ignored_old)++;
		return;
	}

	if (!entry) {
		entry = XCALLOC(MTYPE_MIDR_NODE_ENTRY, sizeof(*entry));
		entry->data.node_id = node->node_id;
		(void)hash_get(table->nodes, entry, hash_alloc_intern);
	}

	entry->data = *node;
	entry->active = true;
}

static void midr_apply_node_withdraw(struct midr_fact_table *table, uint32_t node_id,
				     uint64_t version, uint64_t *ignored_old)
{
	struct midr_node_entry lookup = { .data.node_id = node_id };
	struct midr_node_entry *entry;

	entry = hash_lookup(table->nodes, &lookup);
	if (entry && version <= entry->data.version) {
		(*ignored_old)++;
		return;
	}

	if (!entry) {
		entry = XCALLOC(MTYPE_MIDR_NODE_ENTRY, sizeof(*entry));
		entry->data.node_id = node_id;
		(void)hash_get(table->nodes, entry, hash_alloc_intern);
	}

	memset(&entry->data, 0, sizeof(entry->data));
	entry->data.node_id = node_id;
	entry->data.version = version;
	entry->active = false;
}

static void midr_apply_link_upsert(struct midr_fact_table *table,
				   const struct midr_link_update *link, uint64_t *ignored_old)
{
	struct midr_link_entry lookup = { .data.key = link->key };
	struct midr_link_entry *entry;

	entry = hash_lookup(table->links, &lookup);
	if (entry && link->version <= entry->data.version) {
		(*ignored_old)++;
		return;
	}

	if (!entry) {
		entry = XCALLOC(MTYPE_MIDR_LINK_ENTRY, sizeof(*entry));
		entry->data.key = link->key;
		(void)hash_get(table->links, entry, hash_alloc_intern);
	}

	entry->data = *link;
	entry->active = true;
}

static void midr_apply_link_withdraw(struct midr_fact_table *table, const struct midr_link_key *key,
				     uint64_t version, uint64_t *ignored_old)
{
	struct midr_link_entry lookup = { .data.key = *key };
	struct midr_link_entry *entry;

	entry = hash_lookup(table->links, &lookup);
	if (entry && version <= entry->data.version) {
		(*ignored_old)++;
		return;
	}

	if (!entry) {
		entry = XCALLOC(MTYPE_MIDR_LINK_ENTRY, sizeof(*entry));
		entry->data.key = *key;
		(void)hash_get(table->links, entry, hash_alloc_intern);
	}

	memset(&entry->data, 0, sizeof(entry->data));
	entry->data.key = *key;
	entry->data.version = version;
	entry->active = false;
}

static void midr_apply_event(struct midr_fact_table *table, const struct midr_event *event,
			     uint64_t *ignored_old)
{
	switch (event->type) {
	case MIDR_EVENT_NODE_UPSERT:
		midr_apply_node_upsert(table, &event->u.node, ignored_old);
		break;
	case MIDR_EVENT_NODE_WITHDRAW:
		midr_apply_node_withdraw(table, event->u.node_withdraw.node_id,
					 event->u.node_withdraw.version, ignored_old);
		break;
	case MIDR_EVENT_LINK_UPSERT:
		midr_apply_link_upsert(table, &event->u.link, ignored_old);
		break;
	case MIDR_EVENT_LINK_WITHDRAW:
		midr_apply_link_withdraw(table, &event->u.link_withdraw.key,
					 event->u.link_withdraw.version, ignored_old);
		break;
	}
}

static size_t midr_queue_discard(struct list *queue)
{
	size_t count;

	if (!queue)
		return 0;

	count = listcount(queue);
	list_delete_all_node(queue);
	return count;
}

static void midr_process_queue(struct midr_input_store *store, struct list *queue,
			       struct midr_fact_table *table)
{
	struct midr_event *event;

	while ((event = listnode_head(queue)) != NULL) {
		listnode_delete(queue, event);
		midr_apply_event(table, event, &store->event_ignored_old);
		store->event_processed++;
		midr_event_free(event);
	}
}

static void midr_input_schedule_resync(struct midr_input_store *store, unsigned int delay_msec);

static void midr_input_enter_out_of_sync(struct midr_input_store *store)
{
	event_cancel(&store->t_process);
	event_cancel(&store->t_resync);
	store->event_dropped_resync += midr_queue_discard(store->normal_queue);
	store->event_dropped_resync += midr_queue_discard(store->resync_queue);
	store->state = MIDR_INPUT_OUT_OF_SYNC;
	store->startup_probe = false;
}

static int midr_snapshot_to_fact_table(struct midr_input_store *store,
				       const struct midr_topology_snapshot *snapshot,
				       struct midr_fact_table **out)
{
	struct midr_fact_table *table;
	size_t i;

	if (!snapshot || !out || *out)
		return -EINVAL;
	if (snapshot->node_count > 1 || (!!snapshot->nodes != (snapshot->node_count != 0)) ||
	    (!!snapshot->links != (snapshot->link_count != 0)))
		return -EINVAL;

	table = midr_fact_table_new();
	for (i = 0; i < snapshot->node_count; i++) {
		struct midr_node_entry lookup = { .data.node_id = snapshot->nodes[i].node_id };

		if (midr_validate_node_update(store->owner_node_id, &snapshot->nodes[i]) != 0 ||
		    hash_lookup(table->nodes, &lookup)) {
			midr_fact_table_free(&table);
			return -EINVAL;
		}
		midr_apply_node_upsert(table, &snapshot->nodes[i], &store->event_ignored_old);
	}

	for (i = 0; i < snapshot->link_count; i++) {
		struct midr_link_entry lookup = { .data.key = snapshot->links[i].key };

		if (midr_validate_link_update(store->owner_node_id, &snapshot->links[i]) != 0 ||
		    hash_lookup(table->links, &lookup)) {
			midr_fact_table_free(&table);
			return -EINVAL;
		}
		midr_apply_link_upsert(table, &snapshot->links[i], &store->event_ignored_old);
	}

	*out = table;
	return 0;
}

static void midr_input_resync_step(struct midr_input_store *store)
{
	struct midr_topology_snapshot snapshot = {};
	struct midr_fact_table *staging = NULL;
	struct midr_fact_table *old;
	uint64_t snapshot_version;
	int ret;

	if (!store || store->state != MIDR_INPUT_RESYNCING)
		return;

	store->resync_attempts++;
	midr_process_queue(store, store->normal_queue, store->active);

	ret = midr_topology_snapshot_get(store->ctx, &snapshot);
	if (store->state != MIDR_INPUT_RESYNCING) {
		if (!ret)
			midr_topology_snapshot_release(store->ctx, &snapshot);
		return;
	}
	if (ret == -ENOSYS) {
		store->provider_state = MIDR_PROVIDER_UNAVAILABLE;
		if (store->startup_probe && !store->identity_restart_pending) {
			midr_process_queue(store, store->resync_queue, store->active);
			store->state = MIDR_INPUT_NORMAL;
			store->startup_probe = false;
			return;
		}
		store->resync_failures++;
		midr_input_enter_out_of_sync(store);
		if (store->identity_restart_pending)
			store->state = MIDR_INPUT_IDENTITY_RESTART;
		return;
	}

	store->provider_state = MIDR_PROVIDER_AVAILABLE;
	if (ret) {
		store->resync_failures++;
		midr_input_schedule_resync(store, MIDR_RESYNC_RETRY_MSEC);
		return;
	}

	snapshot_version = snapshot.snapshot_version;
	ret = midr_snapshot_to_fact_table(store, &snapshot, &staging);
	if (!ret)
		midr_process_queue(store, store->resync_queue, staging);
	midr_topology_snapshot_release(store->ctx, &snapshot);
	if (ret) {
		store->resync_failures++;
		midr_fact_table_free(&staging);
		midr_input_schedule_resync(store, MIDR_RESYNC_RETRY_MSEC);
		return;
	}

	old = store->active;
	store->active = staging;
	store->snapshot_version = snapshot_version;
	store->state = MIDR_INPUT_NORMAL;
	store->startup_probe = false;
	store->identity_restart_pending = false;
	store->resync_commits++;
	midr_fact_table_free(&old);
}

static void midr_resync_event_cb(struct event *event)
{
	struct midr_input_store *store = EVENT_ARG(event);

	if (!store)
		return;

	store->t_resync = NULL;
	midr_input_resync_step(store);
}

static void midr_input_schedule_resync(struct midr_input_store *store, unsigned int delay_msec)
{
	if (!store || store->t_resync || !bm || !bm->master)
		return;

	if (delay_msec)
		event_add_timer_msec(bm->master, midr_resync_event_cb, store, delay_msec,
				     &store->t_resync);
	else
		event_add_event(bm->master, midr_resync_event_cb, store, 0, &store->t_resync);
}

static int midr_input_begin_resync(struct midr_input_store *store,
				   enum midr_topology_resync_reason reason, bool startup)
{
	if (!store)
		return -ENOENT;
	if (store->state == MIDR_INPUT_RESYNCING || store->state == MIDR_INPUT_IDENTITY_RESTART)
		return -EBUSY;
	if (!startup && store->provider_state != MIDR_PROVIDER_AVAILABLE)
		return -EAGAIN;

	event_cancel(&store->t_process);
	event_cancel(&store->t_resync);
	store->state = MIDR_INPUT_RESYNCING;
	store->reason = reason;
	store->startup_probe = startup;
	midr_input_schedule_resync(store, 0);
	return 0;
}

static void midr_process_event_cb(struct event *event)
{
	struct midr_input_store *store = EVENT_ARG(event);

	if (!store)
		return;

	store->t_process = NULL;
	if (store->state == MIDR_INPUT_NORMAL)
		midr_process_queue(store, store->normal_queue, store->active);
}

static void midr_schedule_process(struct midr_input_store *store)
{
	if (!store || store->t_process || !bm || !bm->master)
		return;

	event_add_event(bm->master, midr_process_event_cb, store, 0, &store->t_process);
}

static int midr_enqueue_event(struct midr_context *ctx, struct midr_event *event)
{
	struct midr_input_store *store;
	struct list *queue;
	size_t limit;

	if (!ctx || !ctx->input_store || !event)
		return -EINVAL;

	store = ctx->input_store;
	if (store->state == MIDR_INPUT_OUT_OF_SYNC || store->state == MIDR_INPUT_IDENTITY_RESTART) {
		store->event_rejected_sync++;
		return -EAGAIN;
	}

	if (store->state == MIDR_INPUT_RESYNCING) {
		queue = store->resync_queue;
		limit = store->resync_queue_limit;
	} else {
		queue = store->normal_queue;
		limit = store->normal_queue_limit;
	}

	if (listcount(queue) >= limit) {
		store->event_rejected_full++;
		store->queue_overflows++;
		store->reason = MIDR_TOPOLOGY_RESYNC_STATE_INCONSISTENT;
		midr_input_enter_out_of_sync(store);
		return -ENOSPC;
	}

	listnode_add(queue, event);
	store->event_enqueued++;
	if (store->state == MIDR_INPUT_NORMAL)
		midr_schedule_process(store);
	return 0;
}

int midr_input_init(struct midr_context *ctx)
{
	struct midr_input_store *store;

	if (!ctx || !ctx->bgp)
		return -EINVAL;
	if (ctx->input_store)
		return -EALREADY;

	store = XCALLOC(MTYPE_MIDR_INPUT, sizeof(*store));
	store->ctx = ctx;
	store->active = midr_fact_table_new();
	store->normal_queue = list_new();
	store->normal_queue->del = midr_event_free;
	store->resync_queue = list_new();
	store->resync_queue->del = midr_event_free;
	store->normal_queue_limit = MIDR_EVENT_QUEUE_LIMIT;
	store->resync_queue_limit = MIDR_RESYNC_QUEUE_LIMIT;
	store->owner_node_id = ctx->bgp->router_id.s_addr;
	store->provider_state = MIDR_PROVIDER_UNKNOWN;
	store->state = MIDR_INPUT_NORMAL;
	ctx->input_store = store;

	if (store->owner_node_id)
		return midr_input_begin_resync(store, MIDR_TOPOLOGY_RESYNC_STATE_INCONSISTENT,
					       true);

	store->state = MIDR_INPUT_IDENTITY_RESTART;
	return 0;
}

void midr_input_finish(struct midr_context *ctx)
{
	struct midr_input_store *store;

	if (!ctx || !ctx->input_store)
		return;

	store = ctx->input_store;
	event_cancel(&store->t_process);
	event_cancel(&store->t_resync);
	list_delete(&store->normal_queue);
	list_delete(&store->resync_queue);
	midr_fact_table_free(&store->active);
	ctx->input_store = NULL;
	XFREE(MTYPE_MIDR_INPUT, store);
}

void midr_topology_process_pending(struct midr_context *ctx)
{
	struct midr_input_store *store;

	if (!ctx || !ctx->input_store)
		return;

	store = ctx->input_store;
	if (store->state == MIDR_INPUT_NORMAL)
		midr_process_queue(store, store->normal_queue, store->active);
	else if (store->state == MIDR_INPUT_RESYNCING)
		midr_process_queue(store, store->normal_queue, store->active);
}

int midr_topology_node_upsert(struct midr_context *ctx, const struct midr_node_update *node)
{
	struct midr_event *event;
	int ret;

	if (!ctx || !ctx->bgp || !ctx->input_store)
		return -ENOENT;
	ret = midr_validate_node_update(ctx->bgp->router_id.s_addr, node);
	if (ret)
		return ret;

	event = XCALLOC(MTYPE_MIDR_EVENT, sizeof(*event));
	event->type = MIDR_EVENT_NODE_UPSERT;
	event->u.node = *node;
	ret = midr_enqueue_event(ctx, event);
	if (ret)
		midr_event_free(event);
	return ret;
}

int midr_topology_node_withdraw(struct midr_context *ctx, uint32_t node_id, uint64_t version)
{
	struct midr_event *event;
	int ret;

	if (!ctx || !ctx->bgp || !ctx->input_store)
		return -ENOENT;
	ret = midr_validate_node_withdraw(ctx->bgp->router_id.s_addr, node_id);
	if (ret)
		return ret;

	event = XCALLOC(MTYPE_MIDR_EVENT, sizeof(*event));
	event->type = MIDR_EVENT_NODE_WITHDRAW;
	event->u.node_withdraw.node_id = node_id;
	event->u.node_withdraw.version = version;
	ret = midr_enqueue_event(ctx, event);
	if (ret)
		midr_event_free(event);
	return ret;
}

int midr_topology_link_upsert(struct midr_context *ctx, const struct midr_link_update *link)
{
	struct midr_event *event;
	int ret;

	if (!ctx || !ctx->bgp || !ctx->input_store)
		return -ENOENT;
	ret = midr_validate_link_update(ctx->bgp->router_id.s_addr, link);
	if (ret)
		return ret;

	event = XCALLOC(MTYPE_MIDR_EVENT, sizeof(*event));
	event->type = MIDR_EVENT_LINK_UPSERT;
	event->u.link = *link;
	ret = midr_enqueue_event(ctx, event);
	if (ret)
		midr_event_free(event);
	return ret;
}

int midr_topology_link_withdraw(struct midr_context *ctx, const struct midr_link_key *key,
				uint64_t version)
{
	struct midr_event *event;
	int ret;

	if (!ctx || !ctx->bgp || !ctx->input_store)
		return -ENOENT;
	ret = midr_validate_link_withdraw(ctx->bgp->router_id.s_addr, key);
	if (ret)
		return ret;

	event = XCALLOC(MTYPE_MIDR_EVENT, sizeof(*event));
	event->type = MIDR_EVENT_LINK_WITHDRAW;
	event->u.link_withdraw.key = *key;
	event->u.link_withdraw.version = version;
	ret = midr_enqueue_event(ctx, event);
	if (ret)
		midr_event_free(event);
	return ret;
}

int midr_topology_resync_begin(struct midr_context *ctx, enum midr_topology_resync_reason reason)
{
	if (!ctx || !ctx->bgp || !ctx->input_store)
		return -ENOENT;
	if (reason != MIDR_TOPOLOGY_RESYNC_VERSION_LOST &&
	    reason != MIDR_TOPOLOGY_RESYNC_PROVIDER_RESTART &&
	    reason != MIDR_TOPOLOGY_RESYNC_STATE_INCONSISTENT)
		return -EINVAL;

	return midr_input_begin_resync(ctx->input_store, reason, false);
}

int midr_input_router_id_update(struct bgp *bgp, bool withdraw)
{
	struct midr_context *ctx;
	struct midr_input_store *store;
	struct midr_fact_table *empty;
	bool had_identity;

	if (!bgp || !bgp->midr_info)
		return 0;

	ctx = &bgp->midr_info->ctx;
	store = ctx->input_store;
	if (!store)
		return 0;

	if (withdraw) {
		had_identity = store->owner_node_id != 0;
		event_cancel(&store->t_process);
		event_cancel(&store->t_resync);
		store->event_dropped_resync += midr_queue_discard(store->normal_queue);
		store->event_dropped_resync += midr_queue_discard(store->resync_queue);
		empty = midr_fact_table_new();
		midr_fact_table_free(&store->active);
		store->active = empty;
		store->snapshot_version = 0;
		store->owner_node_id = 0;
		store->state = MIDR_INPUT_IDENTITY_RESTART;
		store->startup_probe = false;
		store->identity_restart_pending = had_identity;
		if (had_identity)
			store->identity_restarts++;
		return 0;
	}

	store->owner_node_id = bgp->router_id.s_addr;
	if (!store->owner_node_id) {
		store->state = MIDR_INPUT_IDENTITY_RESTART;
		return 0;
	}
	store->reason = MIDR_TOPOLOGY_RESYNC_PROVIDER_RESTART;
	if (store->provider_state == MIDR_PROVIDER_UNAVAILABLE) {
		store->state = store->identity_restart_pending ? MIDR_INPUT_IDENTITY_RESTART
							       : MIDR_INPUT_NORMAL;
		return 0;
	}

	store->state = MIDR_INPUT_OUT_OF_SYNC;
	if (midr_input_begin_resync(store, MIDR_TOPOLOGY_RESYNC_PROVIDER_RESTART,
				    store->provider_state == MIDR_PROVIDER_UNKNOWN) != 0)
		store->state = MIDR_INPUT_IDENTITY_RESTART;
	return 0;
}

int midr_local_fact_node_get(struct midr_context *ctx, uint32_t node_id,
			     struct midr_node_update *node, bool *active)
{
	struct midr_node_entry lookup = { .data.node_id = node_id };
	struct midr_node_entry *entry;

	if (!ctx || !ctx->input_store)
		return -ENOENT;
	if (!node || !active)
		return -EINVAL;

	entry = hash_lookup(ctx->input_store->active->nodes, &lookup);
	if (!entry)
		return -ENOENT;
	*node = entry->data;
	*active = entry->active;
	return 0;
}

int midr_local_fact_link_get(struct midr_context *ctx, const struct midr_link_key *key,
			     struct midr_link_update *link, bool *active)
{
	struct midr_link_entry lookup;
	struct midr_link_entry *entry;

	if (!ctx || !ctx->input_store)
		return -ENOENT;
	if (!key || !link || !active)
		return -EINVAL;

	memset(&lookup, 0, sizeof(lookup));
	lookup.data.key = *key;
	entry = hash_lookup(ctx->input_store->active->links, &lookup);
	if (!entry)
		return -ENOENT;
	*link = entry->data;
	*active = entry->active;
	return 0;
}

struct midr_fact_count {
	size_t active;
	size_t tombstone;
};

static void midr_count_node_iter(struct hash_bucket *bucket, void *arg)
{
	const struct midr_node_entry *entry = bucket->data;
	struct midr_fact_count *count = arg;

	if (entry->active)
		count->active++;
	else
		count->tombstone++;
}

static void midr_count_link_iter(struct hash_bucket *bucket, void *arg)
{
	const struct midr_link_entry *entry = bucket->data;
	struct midr_fact_count *count = arg;

	if (entry->active)
		count->active++;
	else
		count->tombstone++;
}

int midr_input_status_get(struct midr_context *ctx, struct midr_input_status *status)
{
	struct midr_input_store *store;
	struct midr_fact_count nodes = {};
	struct midr_fact_count links = {};

	if (!ctx || !ctx->input_store)
		return -ENOENT;
	if (!status)
		return -EINVAL;

	store = ctx->input_store;
	hash_iterate(store->active->nodes, midr_count_node_iter, &nodes);
	hash_iterate(store->active->links, midr_count_link_iter, &links);
	memset(status, 0, sizeof(*status));
	status->state = store->state;
	status->reason = store->reason;
	status->owner_node_id = store->owner_node_id;
	status->provider_available = store->provider_state == MIDR_PROVIDER_AVAILABLE;
	status->snapshot_version = store->snapshot_version;
	status->normal_queue_count = listcount(store->normal_queue);
	status->resync_queue_count = listcount(store->resync_queue);
	status->normal_queue_limit = store->normal_queue_limit;
	status->resync_queue_limit = store->resync_queue_limit;
	status->active_node_count = nodes.active;
	status->active_link_count = links.active;
	status->node_tombstone_count = nodes.tombstone;
	status->link_tombstone_count = links.tombstone;
	status->event_enqueued = store->event_enqueued;
	status->event_processed = store->event_processed;
	status->event_ignored_old = store->event_ignored_old;
	status->event_rejected_full = store->event_rejected_full;
	status->event_rejected_sync = store->event_rejected_sync;
	status->event_dropped_resync = store->event_dropped_resync;
	status->resync_attempts = store->resync_attempts;
	status->resync_commits = store->resync_commits;
	status->resync_failures = store->resync_failures;
	status->queue_overflows = store->queue_overflows;
	status->identity_restarts = store->identity_restarts;
	return 0;
}

int midr_input_test_set_queue_limits(struct midr_context *ctx, size_t normal_limit,
				     size_t resync_limit)
{
	if (!ctx || !ctx->input_store)
		return -ENOENT;
	if (!normal_limit || !resync_limit)
		return -EINVAL;
	if (listcount(ctx->input_store->normal_queue) > normal_limit ||
	    listcount(ctx->input_store->resync_queue) > resync_limit)
		return -EBUSY;

	ctx->input_store->normal_queue_limit = normal_limit;
	ctx->input_store->resync_queue_limit = resync_limit;
	return 0;
}

void midr_input_test_resync_now(struct midr_context *ctx)
{
	if (!ctx || !ctx->input_store)
		return;

	event_cancel(&ctx->input_store->t_resync);
	midr_input_resync_step(ctx->input_store);
}

static void midr_show_node_iter(struct hash_bucket *bucket, void *arg)
{
	struct vty *vty = arg;
	struct midr_node_entry *entry = bucket->data;
	struct in_addr addr = { .s_addr = entry->data.node_id };
	char transport[IPADDR_STRING_SIZE];

	if (!entry->active)
		return;

	vty_out(vty, "%pI4 group %u", &addr, entry->data.group_id);
	if (entry->data.has_transport_address)
		vty_out(vty, " transport %s %s",
			entry->data.transport_address.ipa_type == IPADDR_V4 ? "ipv4" : "ipv6",
			ipaddr2str(&entry->data.transport_address, transport, sizeof(transport)));
	vty_out(vty, " policy %s version %" PRIu64 " active\n",
		entry->data.policy_state == MIDR_POLICY_ALLOWED ? "allowed" : "blocked",
		entry->data.version);
}

void midr_show_topology_nodes(struct vty *vty, struct midr_context *ctx)
{
	if (!ctx || !ctx->input_store) {
		vty_out(vty, "MIDR is not initialized\n");
		return;
	}

	midr_topology_process_pending(ctx);
	vty_out(vty, "MIDR topology nodes:\n");
	hash_iterate(ctx->input_store->active->nodes, midr_show_node_iter, vty);
}

static void midr_show_link_iter(struct hash_bucket *bucket, void *arg)
{
	struct vty *vty = arg;
	struct midr_link_entry *entry = bucket->data;
	struct in_addr local = { .s_addr = entry->data.key.local_node_id };
	struct in_addr remote = { .s_addr = entry->data.key.remote_node_id };
	char local_address[IPADDR_STRING_SIZE];
	char remote_address[IPADDR_STRING_SIZE];
	const char *address_type;

	if (!entry->active)
		return;

	address_type = entry->data.link_local_address.ipa_type == IPADDR_V4 ? "ipv4" : "ipv6";
	vty_out(vty,
		"%pI4 -> %pI4 id %" PRIu64 " local-address %s %s remote-address %s %s ifindex %d"
		" rtt-us %u loss-ppm %u available-bandwidth-kbps %u"
		" policy %s version %" PRIu64,
		&local, &remote, entry->data.key.link_id, address_type,
		ipaddr2str(&entry->data.link_local_address, local_address, sizeof(local_address)),
		address_type,
		ipaddr2str(&entry->data.link_remote_address, remote_address,
			   sizeof(remote_address)),
		entry->data.local_ifindex, entry->data.metrics.rtt_us,
		entry->data.metrics.loss_ppm, entry->data.metrics.available_bandwidth_kbps,
		entry->data.policy_state == MIDR_POLICY_ALLOWED ? "allowed" : "blocked",
		entry->data.version);
	if (entry->data.metrics.measurement_seqno)
		vty_out(vty, " seqno %" PRIu64, entry->data.metrics.measurement_seqno);
	if (entry->data.metrics.measurement_timestamp_ms)
		vty_out(vty, " timestamp-ms %" PRIu64,
			entry->data.metrics.measurement_timestamp_ms);
	vty_out(vty, " active\n");
}

void midr_show_topology_links(struct vty *vty, struct midr_context *ctx)
{
	if (!ctx || !ctx->input_store) {
		vty_out(vty, "MIDR is not initialized\n");
		return;
	}

	midr_topology_process_pending(ctx);
	vty_out(vty, "MIDR topology links:\n");
	hash_iterate(ctx->input_store->active->links, midr_show_link_iter, vty);
}

static void midr_show_node_tombstone_iter(struct hash_bucket *bucket, void *arg)
{
	struct vty *vty = arg;
	struct midr_node_entry *entry = bucket->data;
	struct in_addr addr = { .s_addr = entry->data.node_id };

	if (entry->active)
		return;

	vty_out(vty, "    %pI4 version %" PRIu64 "\n", &addr, entry->data.version);
}

static void midr_show_link_tombstone_iter(struct hash_bucket *bucket, void *arg)
{
	struct vty *vty = arg;
	struct midr_link_entry *entry = bucket->data;
	struct in_addr local = { .s_addr = entry->data.key.local_node_id };
	struct in_addr remote = { .s_addr = entry->data.key.remote_node_id };

	if (entry->active)
		return;

	vty_out(vty, "    %pI4 -> %pI4 id %" PRIu64 " version %" PRIu64 "\n", &local, &remote,
		entry->data.key.link_id, entry->data.version);
}

void midr_show_topology_tombstones(struct vty *vty, struct midr_context *ctx)
{
	if (!ctx || !ctx->input_store) {
		vty_out(vty, "MIDR is not initialized\n");
		return;
	}

	midr_topology_process_pending(ctx);
	vty_out(vty, "MIDR topology tombstones:\n");
	vty_out(vty, "  nodes:\n");
	hash_iterate(ctx->input_store->active->nodes, midr_show_node_tombstone_iter, vty);
	vty_out(vty, "  links:\n");
	hash_iterate(ctx->input_store->active->links, midr_show_link_tombstone_iter, vty);
}

static const char *midr_input_state_name(enum midr_input_state state)
{
	switch (state) {
	case MIDR_INPUT_NORMAL:
		return "NORMAL";
	case MIDR_INPUT_RESYNCING:
		return "RESYNCING";
	case MIDR_INPUT_OUT_OF_SYNC:
		return "OUT_OF_SYNC";
	case MIDR_INPUT_IDENTITY_RESTART:
		return "IDENTITY_RESTART";
	}

	return "UNKNOWN";
}

static const char *midr_resync_reason_name(enum midr_topology_resync_reason reason)
{
	switch (reason) {
	case MIDR_TOPOLOGY_RESYNC_VERSION_LOST:
		return "VERSION_LOST";
	case MIDR_TOPOLOGY_RESYNC_PROVIDER_RESTART:
		return "PROVIDER_RESTART";
	case MIDR_TOPOLOGY_RESYNC_STATE_INCONSISTENT:
		return "STATE_INCONSISTENT";
	}

	return "UNKNOWN";
}

void midr_show_topology_sync(struct vty *vty, struct midr_context *ctx)
{
	struct midr_input_status status;
	struct in_addr owner;

	if (midr_input_status_get(ctx, &status) != 0) {
		vty_out(vty, "MIDR is not initialized\n");
		return;
	}

	owner.s_addr = status.owner_node_id;
	vty_out(vty, "MIDR topology synchronization:\n");
	vty_out(vty, "  state:             %s\n", midr_input_state_name(status.state));
	vty_out(vty, "  reason:            %s\n",
		status.state == MIDR_INPUT_RESYNCING || status.state == MIDR_INPUT_OUT_OF_SYNC
			? midr_resync_reason_name(status.reason)
			: "none");
	if (status.owner_node_id)
		vty_out(vty, "  owner:             %pI4\n", &owner);
	else
		vty_out(vty, "  owner:             unset\n");
	vty_out(vty, "  provider:          %s\n",
		status.provider_available ? "available" : "unavailable");
	vty_out(vty, "  snapshot version:  %" PRIu64 "\n", status.snapshot_version);
	vty_out(vty, "  normal queue:      %zu/%zu\n", status.normal_queue_count,
		status.normal_queue_limit);
	vty_out(vty, "  resync queue:      %zu/%zu\n", status.resync_queue_count,
		status.resync_queue_limit);
	vty_out(vty, "  active facts:      %zu nodes, %zu links\n", status.active_node_count,
		status.active_link_count);
	vty_out(vty, "  tombstones:        %zu nodes, %zu links\n", status.node_tombstone_count,
		status.link_tombstone_count);
	vty_out(vty, "  resync attempts:   %" PRIu64 "\n", status.resync_attempts);
	vty_out(vty, "  resync commits:    %" PRIu64 "\n", status.resync_commits);
	vty_out(vty, "  resync failures:   %" PRIu64 "\n", status.resync_failures);
	vty_out(vty, "  queue overflows:   %" PRIu64 "\n", status.queue_overflows);
	vty_out(vty, "  identity restarts: %" PRIu64 "\n", status.identity_restarts);
}

void midr_show_events(struct vty *vty, struct midr_context *ctx)
{
	struct midr_input_status status;

	if (midr_input_status_get(ctx, &status) != 0) {
		vty_out(vty, "MIDR is not initialized\n");
		return;
	}

	midr_topology_process_pending(ctx);
	(void)midr_input_status_get(ctx, &status);
	vty_out(vty, "MIDR events:\n");
	vty_out(vty, "  enqueued:       %" PRIu64 "\n", status.event_enqueued);
	vty_out(vty, "  processed:      %" PRIu64 "\n", status.event_processed);
	vty_out(vty, "  ignored-old:    %" PRIu64 "\n", status.event_ignored_old);
	vty_out(vty, "  rejected-full:  %" PRIu64 "\n", status.event_rejected_full);
	vty_out(vty, "  rejected-sync:  %" PRIu64 "\n", status.event_rejected_sync);
	vty_out(vty, "  dropped-resync: %" PRIu64 "\n", status.event_dropped_resync);
	vty_out(vty, "  peer hooks:     %" PRIu64 "\n", ctx->midr->peer_hook_events);
	vty_out(vty, "  route hooks:    %" PRIu64 "\n", ctx->midr->route_hook_events);
	vty_out(vty, "  pending queue:  %zu\n",
		status.normal_queue_count + status.resync_queue_count);
}
