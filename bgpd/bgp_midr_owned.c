// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Locally originated MIDR Membership and Link objects.
 */

#include <zebra.h>

#include <errno.h>

#include "command.h"
#include "hash.h"
#include "jhash.h"
#include "memory.h"
#include "monotime.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_midr_cost.h"
#include "bgpd/bgp_midr_lsdb.h"
#include "bgpd/bgp_midr_owned.h"
#include "bgpd/bgp_midr_prefix.h"
#include "bgpd/bgp_midr_private.h"
#include "bgpd/bgp_midr_rib.h"
#include "bgpd/bgp_midr_sync.h"

#define MIDR_LINK_COST_MIN_ADVERTISEMENT_INTERVAL_MSEC 300000U
#define MIDR_SEQUENCE_RETRY_MSEC 1000U

DEFINE_MTYPE_STATIC(BGPD, MIDR_OWNED_STORE, "MIDR owned object store");
DEFINE_MTYPE_STATIC(BGPD, MIDR_OWNED_ENTRY, "MIDR owned object entry");

enum midr_owned_domain {
	MIDR_OWNED_DOMAIN_TOPOLOGY = (1U << 0),
	MIDR_OWNED_DOMAIN_NODE_PREFIX = (1U << 1),
	MIDR_OWNED_DOMAIN_GROUP_PREFIX = (1U << 2),
	MIDR_OWNED_DOMAIN_ALL = MIDR_OWNED_DOMAIN_TOPOLOGY | MIDR_OWNED_DOMAIN_NODE_PREFIX |
				MIDR_OWNED_DOMAIN_GROUP_PREFIX,
};

struct midr_owned_entry {
	struct midr_owned_store *store;
	struct midr_ls_object_key key;
	struct midr_ls_object advertised;
	struct midr_link_update latest_link;
	ifindex_t local_ifindex;
	struct timeval last_advertised;
	struct event *timer;
	uint32_t domain;
	bool advertised_present;
	bool seen;
	bool suppressed;
};

static uint32_t midr_owned_key_domain(const struct midr_ls_object_key *key)
{
	switch (key->type) {
	case MIDR_NLRI_TYPE_MEMBERSHIP:
	case MIDR_NLRI_TYPE_LINK:
		return MIDR_OWNED_DOMAIN_TOPOLOGY;
	case MIDR_NLRI_TYPE_NODE_PREFIX:
		return MIDR_OWNED_DOMAIN_NODE_PREFIX;
	case MIDR_NLRI_TYPE_GROUP_PREFIX:
		return MIDR_OWNED_DOMAIN_GROUP_PREFIX;
	case MIDR_NLRI_TYPE_RESERVED:
		break;
	}
	return 0;
}

struct midr_owned_store {
	struct midr_context *ctx;
	struct hash *entries;
	struct midr_sequence_allocator allocator;
	const struct midr_sequence_store_ops *sequence_ops;
	void *sequence_arg;
	struct event *sequence_retry;
	struct event *takeover_timer;
	uint32_t owner_node_id;
	uint32_t representative_group_id;
	uint32_t representative_candidate;
	uint32_t takeover_delay_msec;
	bool ready;
	bool reconciling;
	bool representative_committed;
	bool takeover_delay_elapsed;
	bool local_member_active;
	uint32_t pending_domains;
	uint64_t sequence_failures;
	uint64_t fightbacks;
};

static bool midr_owned_local_member_active(struct midr_owned_store *store)
{
	struct midr_node_update node;
	bool active;

	if (!store || !store->ctx || !store->owner_node_id)
		return false;
	if (midr_local_fact_node_get(store->ctx, store->owner_node_id, &node,
				     &active) != 0)
		return false;
	return active && node.group_id &&
	       node.policy_state == MIDR_POLICY_ALLOWED;
}

static unsigned int midr_owned_hash_key(const void *arg)
{
	const struct midr_owned_entry *entry = arg;

	return midr_ls_object_key_hash(&entry->key);
}

static bool midr_owned_hash_cmp(const void *a, const void *b)
{
	const struct midr_owned_entry *left = a;
	const struct midr_owned_entry *right = b;

	return midr_ls_object_key_same(&left->key, &right->key);
}

static void *midr_owned_hash_alloc(void *arg)
{
	const struct midr_owned_entry *source = arg;
	struct midr_owned_entry *entry;

	entry = XCALLOC(MTYPE_MIDR_OWNED_ENTRY, sizeof(*entry));
	entry->key = source->key;
	return entry;
}

static struct midr_owned_entry *
midr_owned_entry_get(struct midr_owned_store *store,
		     const struct midr_ls_object_key *key)
{
	struct midr_owned_entry lookup = {
		.key = *key,
	};

	struct midr_owned_entry *entry;

	entry = hash_get(store->entries, &lookup, midr_owned_hash_alloc);
	entry->store = store;
	entry->domain = midr_owned_key_domain(key);
	return entry;
}

static struct midr_owned_entry *
midr_owned_entry_lookup(struct midr_owned_store *store,
			const struct midr_ls_object_key *key)
{
	struct midr_owned_entry lookup = {
		.key = *key,
	};

	return hash_lookup(store->entries, &lookup);
}

static void midr_owned_path_withdraw(struct midr_owned_store *store,
				     struct midr_owned_entry *entry)
{
	if (!entry->advertised_present)
		return;

	(void)midr_rib_path_withdraw(store->ctx, store->ctx->bgp->peer_self,
				     &entry->key);
	entry->advertised_present = false;
	memset(&entry->advertised, 0, sizeof(entry->advertised));
}

static void midr_owned_entry_free(void *arg)
{
	struct midr_owned_entry *entry = arg;

	event_cancel(&entry->timer);
	XFREE(MTYPE_MIDR_OWNED_ENTRY, entry);
}

static void midr_owned_withdraw_iter(struct hash_bucket *bucket, void *arg)
{
	struct midr_owned_store *store = arg;
	struct midr_owned_entry *entry = bucket->data;

	event_cancel(&entry->timer);
	midr_owned_path_withdraw(store, entry);
}

static void midr_owned_withdraw_all(struct midr_owned_store *store)
{
	if (!store)
		return;
	hash_iterate(store->entries, midr_owned_withdraw_iter, store);
}

static int midr_owned_allocator_start(struct midr_owned_store *store,
				      uint32_t node_id)
{
	int ret;

	memset(&store->allocator, 0, sizeof(store->allocator));
	store->owner_node_id = node_id;
	if (!node_id) {
		store->ready = false;
		return -ENOENT;
	}

	ret = midr_sequence_allocator_init(&store->allocator, node_id,
					   store->sequence_ops,
					   store->sequence_arg);
	store->ready = ret == 0;
	if (ret)
		store->sequence_failures++;
	return ret;
}

static void midr_owned_reconcile_event(struct event *event);

static void midr_owned_schedule_sequence_retry(struct midr_owned_store *store)
{
	if (!store || store->sequence_retry || !bm || !bm->master)
		return;
	event_add_timer_msec(bm->master, midr_owned_reconcile_event, store,
			     MIDR_SEQUENCE_RETRY_MSEC, &store->sequence_retry);
}

static int midr_owned_next_sequence(struct midr_owned_store *store,
				    uint64_t *sequence)
{
	int ret;

	if (!store->ready) {
		ret = midr_owned_allocator_start(store,
						store->ctx->bgp->router_id.s_addr);
		if (ret) {
			midr_owned_withdraw_all(store);
			midr_owned_schedule_sequence_retry(store);
			return ret;
		}
	}

	ret = midr_sequence_allocator_next(&store->allocator, sequence);
	if (!ret)
		return 0;

	store->ready = false;
	store->sequence_failures++;
	midr_owned_withdraw_all(store);
	midr_owned_schedule_sequence_retry(store);
	return ret;
}

static bool midr_owned_object_payload_same(const struct midr_ls_object *a,
					   const struct midr_ls_object *b)
{
	struct midr_ls_object left = *a;
	struct midr_ls_object right = *b;

	left.ls_sequence = 0;
	right.ls_sequence = 0;
	return midr_ls_object_same(&left, &right);
}

static int midr_owned_publish(struct midr_owned_store *store,
			      struct midr_owned_entry *entry,
			      struct midr_ls_object *object)
{
	struct midr_propagation_path path = {};
	uint64_t sequence;
	int ret;

	if (entry->advertised_present &&
	    midr_owned_object_payload_same(&entry->advertised, object))
		return 0;

	ret = midr_owned_next_sequence(store, &sequence);
	if (ret)
		return ret;
	object->ls_sequence = sequence;

	ret = midr_propagation_path_init(&path, store->owner_node_id);
	if (ret)
		return ret;
	ret = midr_rib_path_upsert(store->ctx, store->ctx->bgp->peer_self,
				   object, &path);
	midr_propagation_path_fini(&path);
	if (ret)
		return ret;

	entry->advertised = *object;
	entry->advertised_present = true;
	monotime(&entry->last_advertised);
	entry->suppressed = false;
	return 0;
}

static struct midr_ls_object
midr_owned_membership_object(const struct midr_node_update *node)
{
	return (struct midr_ls_object){
		.key =
			{
				.type = MIDR_NLRI_TYPE_MEMBERSHIP,
				.originator_node_id = node->node_id,
			},
		.policy_tags = node->policy_tags,
		.payload.membership =
			{
				.group_id = node->group_id,
				.has_transport_address =
					node->has_transport_address,
				.transport_address = node->transport_address,
				.cap_flags = node->cap_flags,
			},
	};
}

static struct midr_ls_object
midr_owned_link_object(const struct midr_link_update *link,
		       uint32_t canonical_cost)
{
	return (struct midr_ls_object){
		.key =
			{
				.type = MIDR_NLRI_TYPE_LINK,
				.originator_node_id =
					link->key.local_node_id,
				.u.link =
					{
						.remote_node_id =
							link->key.remote_node_id,
						.link_id = link->key.link_id,
					},
			},
		.policy_tags = link->policy_tags,
		.payload.link =
			{
				.link_local_address =
					link->link_local_address,
				.link_remote_address =
					link->link_remote_address,
				.canonical_cost = canonical_cost,
			},
	};
}

static int midr_owned_node_fact(const struct midr_node_update *node,
				void *arg)
{
	struct midr_owned_store *store = arg;
	struct midr_ls_object object;
	struct midr_owned_entry *entry;

	object = midr_owned_membership_object(node);
	entry = midr_owned_entry_get(store, &object.key);
	entry->seen = true;
	if (!node->group_id || node->policy_state == MIDR_POLICY_BLOCKED) {
		midr_owned_path_withdraw(store, entry);
		return 0;
	}

	return midr_owned_publish(store, entry, &object);
}

static bool midr_owned_link_non_measurement_changed(
	const struct midr_owned_entry *entry, const struct midr_link_update *link)
{
	const struct midr_ls_object *old = &entry->advertised;

	if (!entry->advertised_present)
		return true;
	return old->policy_tags != link->policy_tags ||
	       ipaddr_cmp(&old->payload.link.link_local_address,
			  &link->link_local_address) != 0 ||
	       ipaddr_cmp(&old->payload.link.link_remote_address,
			  &link->link_remote_address) != 0;
}

static int midr_owned_publish_link(struct midr_owned_store *store,
				   struct midr_owned_entry *entry)
{
	struct midr_ls_object object;
	uint32_t cost;
	int ret;

	if (!midr_owned_local_member_active(store)) {
		midr_owned_path_withdraw(store, entry);
		entry->suppressed = false;
		return 0;
	}

	ret = midr_cost_from_metrics(&entry->latest_link.metrics, &cost);
	if (ret)
		return ret;
	object = midr_owned_link_object(&entry->latest_link, cost);
	if (entry->advertised_present &&
	    !midr_cost_change_significant(
		    entry->advertised.payload.link.canonical_cost, cost)) {
		entry->suppressed =
			!midr_owned_object_payload_same(&entry->advertised,
						       &object);
		return 0;
	}
	ret = midr_owned_publish(store, entry, &object);
	if (!ret)
		entry->suppressed = false;
	return ret;
}

static void midr_owned_link_timer_cb(struct event *event)
{
	struct midr_owned_entry *entry = EVENT_ARG(event);

	if (!entry)
		return;
	entry->timer = NULL;
	if (entry->store)
		(void)midr_owned_publish_link(entry->store, entry);
}

static int midr_owned_link_fact(const struct midr_link_update *link, void *arg)
{
	struct midr_owned_store *store = arg;
	struct midr_ls_object object;
	struct midr_owned_entry *entry;
	uint32_t cost;
	int64_t elapsed_usec;
	uint32_t remaining_msec;
	int ret;

	ret = midr_cost_from_metrics(&link->metrics, &cost);
	if (ret)
		return ret;
	object = midr_owned_link_object(link, cost);
	entry = midr_owned_entry_get(store, &object.key);
	entry->seen = true;
	entry->local_ifindex = link->local_ifindex;
	entry->latest_link = *link;
	if (link->policy_state == MIDR_POLICY_BLOCKED ||
	    !midr_owned_local_member_active(store)) {
		event_cancel(&entry->timer);
		midr_owned_path_withdraw(store, entry);
		entry->suppressed = false;
		return 0;
	}

	if (!entry->advertised_present ||
	    midr_owned_link_non_measurement_changed(entry, link)) {
		event_cancel(&entry->timer);
		return midr_owned_publish(store, entry, &object);
	}
	if (midr_owned_object_payload_same(&entry->advertised, &object)) {
		event_cancel(&entry->timer);
		entry->suppressed = false;
		return 0;
	}
	if (!midr_cost_change_significant(
		    entry->advertised.payload.link.canonical_cost, cost)) {
		event_cancel(&entry->timer);
		entry->suppressed = true;
		return 0;
	}

	elapsed_usec = monotime_since(&entry->last_advertised, NULL);
	if (elapsed_usec < 0)
		elapsed_usec = 0;
	if (elapsed_usec >=
	    (int64_t)MIDR_LINK_COST_MIN_ADVERTISEMENT_INTERVAL_MSEC *
		    1000) {
		event_cancel(&entry->timer);
		return midr_owned_publish(store, entry, &object);
	}
	entry->suppressed = true;
	remaining_msec =
		MIDR_LINK_COST_MIN_ADVERTISEMENT_INTERVAL_MSEC -
		(uint32_t)(elapsed_usec / 1000);
	if (!entry->timer && bm && bm->master)
		event_add_timer_msec(bm->master, midr_owned_link_timer_cb, entry,
				     remaining_msec, &entry->timer);
	return 0;
}

struct midr_owned_sweep {
	struct midr_owned_store *store;
	uint32_t domains;
};

static void midr_owned_mark_unseen(struct hash_bucket *bucket, void *arg)
{
	const struct midr_owned_sweep *sweep = arg;
	struct midr_owned_entry *entry = bucket->data;

	if (CHECK_FLAG(sweep->domains, entry->domain))
		entry->seen = false;
}

static void midr_owned_sweep_unseen(struct hash_bucket *bucket, void *arg)
{
	struct midr_owned_sweep *sweep = arg;
	struct midr_owned_entry *entry = bucket->data;

	if (!CHECK_FLAG(sweep->domains, entry->domain) || entry->seen)
		return;
	event_cancel(&entry->timer);
	midr_owned_path_withdraw(sweep->store, entry);
}

static int midr_owned_node_prefix(const struct prefix *prefix, void *arg)
{
	struct midr_owned_store *store = arg;
	struct midr_ls_object object = {
		.key =
			{
				.type = MIDR_NLRI_TYPE_NODE_PREFIX,
				.originator_node_id = store->owner_node_id,
				.u.node_prefix =
					{
						.afi =
							family2afi(
								prefix
									->family),
						.safi = SAFI_UNICAST,
					},
			},
	};
	struct midr_owned_entry *entry;

	prefix_copy(&object.key.u.node_prefix.prefix, prefix);
	apply_mask(&object.key.u.node_prefix.prefix);
	entry = midr_owned_entry_get(store, &object.key);
	entry->seen = true;
	return midr_owned_publish(store, entry, &object);
}

static int midr_owned_group_prefix(const struct midr_lsdb_group_prefix_candidate *candidate,
				   void *arg)
{
	struct midr_owned_store *store = arg;
	struct midr_ls_object object = {
		.key =
			{
				.type = MIDR_NLRI_TYPE_GROUP_PREFIX,
				.originator_node_id = store->owner_node_id,
				.u.group_prefix =
					{
						.group_id =
							candidate->group_id,
						.prefix =
							candidate->prefix,
					},
			},
	};
	struct midr_owned_entry *entry;

	if (!candidate->contributor_count ||
	    candidate->representative_node_id != store->owner_node_id)
		return 0;
	entry = midr_owned_entry_get(store, &object.key);
	entry->seen = true;
	return midr_owned_publish(store, entry, &object);
}

static void midr_owned_reconcile_domains(struct midr_context *ctx, uint32_t domains)
{
	struct midr_owned_store *store;
	struct midr_owned_sweep sweep;

	if (!ctx || !ctx->owned_store)
		return;
	store = ctx->owned_store;
	if (store->reconciling) {
		SET_FLAG(store->pending_domains, domains);
		return;
	}

	store->reconciling = true;
	SET_FLAG(store->pending_domains, domains);
	do {
		domains = store->pending_domains;
		store->pending_domains = 0;
		sweep.store = store;
		sweep.domains = domains;
		hash_iterate(store->entries, midr_owned_mark_unseen, &sweep);
		if (CHECK_FLAG(domains, MIDR_OWNED_DOMAIN_TOPOLOGY) &&
		    store->owner_node_id != ctx->bgp->router_id.s_addr)
			midr_owned_identity_start(ctx,
						  ctx->bgp->router_id.s_addr);
		if (store->ready &&
		    CHECK_FLAG(domains, MIDR_OWNED_DOMAIN_TOPOLOGY)) {
			(void)midr_local_fact_foreach(ctx, midr_owned_node_fact,
						     midr_owned_link_fact,
						     store);
		}
		if (store->ready && midr_owned_local_member_active(store) &&
		    CHECK_FLAG(domains, MIDR_OWNED_DOMAIN_NODE_PREFIX))
			(void)midr_prefix_contributor_foreach(ctx, midr_owned_node_prefix, store);
		if (store->ready && midr_owned_local_member_active(store) &&
		    store->representative_committed &&
		    CHECK_FLAG(domains, MIDR_OWNED_DOMAIN_GROUP_PREFIX))
			(void)midr_lsdb_local_group_prefix_foreach(ctx, midr_owned_group_prefix,
								   store);
		hash_iterate(store->entries, midr_owned_sweep_unseen, &sweep);
	} while (store->pending_domains);
	store->reconciling = false;
	midr_lsdb_local_metadata_changed(ctx);
}

void midr_owned_reconcile(struct midr_context *ctx)
{
	struct midr_owned_store *store;
	uint32_t domains = MIDR_OWNED_DOMAIN_TOPOLOGY;
	bool active;

	if (!ctx || !ctx->owned_store)
		return;
	store = ctx->owned_store;
	active = midr_owned_local_member_active(store);
	if (active != store->local_member_active) {
		store->local_member_active = active;
		if (!active) {
			event_cancel(&store->takeover_timer);
			store->representative_group_id = 0;
			store->representative_candidate = 0;
			store->representative_committed = false;
			store->takeover_delay_elapsed = false;
		}
		domains |= MIDR_OWNED_DOMAIN_NODE_PREFIX |
			   MIDR_OWNED_DOMAIN_GROUP_PREFIX;
	}
	midr_owned_reconcile_domains(ctx, domains);
}

void midr_owned_prefix_reconcile(struct midr_context *ctx)
{
	midr_owned_reconcile_domains(ctx, MIDR_OWNED_DOMAIN_NODE_PREFIX);
}

static bool midr_owned_group_prefix_gate_ready(struct midr_owned_store *store)
{
	struct midr_sync_status sync;

	if (!store->ready || !midr_prefix_is_ready(store->ctx))
		return false;
	if (midr_sync_status_get(store->ctx, &sync) != 0)
		return false;
	return sync.state == MIDR_SYNC_READY;
}

static void midr_owned_takeover_timer_cb(struct event *event)
{
	struct midr_owned_store *store = EVENT_ARG(event);

	if (!store)
		return;
	store->takeover_timer = NULL;
	store->takeover_delay_elapsed = true;
	midr_owned_group_reconcile(store->ctx);
}

static void midr_owned_takeover_schedule(struct midr_owned_store *store)
{
	if (store->takeover_timer || store->takeover_delay_elapsed)
		return;
	if (!store->takeover_delay_msec) {
		store->takeover_delay_elapsed = true;
		return;
	}
	if (bm && bm->master)
		event_add_timer_msec(bm->master, midr_owned_takeover_timer_cb, store,
				     store->takeover_delay_msec, &store->takeover_timer);
}

void midr_owned_group_reconcile(struct midr_context *ctx)
{
	struct midr_owned_store *store;
	uint32_t representative = 0;
	uint32_t group_id = 0;
	bool candidate_changed;

	if (!ctx || !ctx->owned_store)
		return;
	store = ctx->owned_store;
	(void)midr_lsdb_local_group_get(ctx, &group_id, &representative);
	candidate_changed = group_id != store->representative_group_id ||
			    representative != store->representative_candidate;
	if (candidate_changed) {
		event_cancel(&store->takeover_timer);
		store->representative_group_id = group_id;
		store->representative_candidate = representative;
		store->representative_committed = false;
		store->takeover_delay_elapsed = false;
		midr_owned_reconcile_domains(ctx, MIDR_OWNED_DOMAIN_GROUP_PREFIX);
	}

	if (!group_id || representative != store->owner_node_id) {
		event_cancel(&store->takeover_timer);
		store->representative_committed = false;
		store->takeover_delay_elapsed = false;
		midr_owned_reconcile_domains(ctx, MIDR_OWNED_DOMAIN_GROUP_PREFIX);
		return;
	}

	if (!midr_owned_group_prefix_gate_ready(store)) {
		if (store->representative_committed) {
			store->representative_committed = false;
			store->takeover_delay_elapsed = false;
		}
		midr_owned_reconcile_domains(ctx, MIDR_OWNED_DOMAIN_GROUP_PREFIX);
		return;
	}

	if (!store->representative_committed) {
		midr_owned_takeover_schedule(store);
		if (!store->takeover_delay_elapsed) {
			midr_owned_reconcile_domains(ctx, MIDR_OWNED_DOMAIN_GROUP_PREFIX);
			return;
		}
		store->representative_committed = true;
	}
	midr_owned_reconcile_domains(ctx, MIDR_OWNED_DOMAIN_GROUP_PREFIX);
}

static void midr_owned_reconcile_event(struct event *event)
{
	struct midr_owned_store *store = EVENT_ARG(event);

	if (!store)
		return;
	store->sequence_retry = NULL;
	if (!store->ready)
		(void)midr_owned_allocator_start(
			store, store->ctx->bgp->router_id.s_addr);
	midr_owned_reconcile_domains(store->ctx, MIDR_OWNED_DOMAIN_ALL);
}

void midr_owned_input_state_changed(struct midr_context *ctx)
{
	midr_lsdb_input_state_changed(ctx);
}

void midr_owned_identity_withdraw(struct midr_context *ctx)
{
	struct midr_owned_store *store;

	if (!ctx || !ctx->owned_store)
		return;
	store = ctx->owned_store;
	event_cancel(&store->sequence_retry);
	event_cancel(&store->takeover_timer);
	midr_owned_withdraw_all(store);
	store->ready = false;
	store->owner_node_id = 0;
	store->representative_group_id = 0;
	store->representative_candidate = 0;
	store->representative_committed = false;
	store->takeover_delay_elapsed = false;
	store->local_member_active = false;
	memset(&store->allocator, 0, sizeof(store->allocator));
	midr_owned_input_state_changed(ctx);
}

void midr_owned_identity_start(struct midr_context *ctx, uint32_t node_id)
{
	struct midr_owned_store *store;

	if (!ctx || !ctx->owned_store)
		return;
	store = ctx->owned_store;
	event_cancel(&store->sequence_retry);
	event_cancel(&store->takeover_timer);
	midr_owned_withdraw_all(store);
	store->representative_group_id = 0;
	store->representative_candidate = 0;
	store->representative_committed = false;
	store->takeover_delay_elapsed = false;
	store->local_member_active = false;
	(void)midr_owned_allocator_start(store, node_id);
	if (node_id && !store->ready)
		midr_owned_schedule_sequence_retry(store);
	midr_owned_input_state_changed(ctx);
}

int midr_owned_init(struct midr_context *ctx)
{
	struct midr_owned_store *store;

	if (!ctx || !ctx->bgp)
		return -EINVAL;
	if (ctx->owned_store)
		return -EALREADY;

	store = XCALLOC(MTYPE_MIDR_OWNED_STORE, sizeof(*store));
	store->ctx = ctx;
	store->entries = hash_create(midr_owned_hash_key,
				     midr_owned_hash_cmp,
				     "MIDR owned objects");
	store->sequence_ops = &midr_sequence_frr_store_ops;
	store->takeover_delay_msec = MIDR_GROUP_PREFIX_TAKEOVER_DELAY_DEFAULT_MSEC;
	ctx->owned_store = store;
	if (ctx->bgp->router_id.s_addr)
		(void)midr_owned_allocator_start(store,
						ctx->bgp->router_id.s_addr);
	return 0;
}

void midr_owned_finish(struct midr_context *ctx)
{
	struct midr_owned_store *store;

	if (!ctx || !ctx->owned_store)
		return;
	store = ctx->owned_store;
	event_cancel(&store->sequence_retry);
	event_cancel(&store->takeover_timer);
	midr_owned_withdraw_all(store);
	hash_clean_and_free(&store->entries, midr_owned_entry_free);
	ctx->owned_store = NULL;
	XFREE(MTYPE_MIDR_OWNED_STORE, store);
}

int midr_owned_observe_self_sequence(struct midr_context *ctx,
				     const struct midr_ls_object *object)
{
	struct midr_owned_store *store;
	int ret;

	if (!ctx || !ctx->owned_store)
		return -ENOENT;
	if (!object || object->key.originator_node_id !=
			       ctx->bgp->router_id.s_addr ||
	    midr_ls_object_validate(object) != 0)
		return -EINVAL;
	store = ctx->owned_store;
	if (!store->ready)
		return -EAGAIN;

	ret = midr_sequence_allocator_advance_past(&store->allocator,
						  object->ls_sequence);
	if (ret) {
		store->ready = false;
		store->sequence_failures++;
		midr_owned_withdraw_all(store);
		midr_owned_schedule_sequence_retry(store);
		return ret;
	}
	store->fightbacks++;
	midr_owned_withdraw_all(store);
	midr_owned_reconcile(ctx);
	return 0;
}

int midr_owned_link_metadata_get(struct midr_context *ctx,
				 const struct midr_ls_object_key *key,
				 ifindex_t *local_ifindex)
{
	struct midr_owned_entry *entry;

	if (!ctx || !ctx->owned_store)
		return -ENOENT;
	if (!key || !local_ifindex || key->type != MIDR_NLRI_TYPE_LINK)
		return -EINVAL;
	entry = midr_owned_entry_lookup(ctx->owned_store, key);
	if (!entry)
		return -ENOENT;
	*local_ifindex = entry->local_ifindex;
	return 0;
}

struct midr_owned_count_state {
	struct midr_owned_summary *summary;
};

static void midr_owned_count_iter(struct hash_bucket *bucket, void *arg)
{
	struct midr_owned_count_state *state = arg;
	struct midr_owned_entry *entry = bucket->data;

	if (entry->advertised_present) {
		if (entry->key.type == MIDR_NLRI_TYPE_MEMBERSHIP)
			state->summary->membership_count++;
		else if (entry->key.type == MIDR_NLRI_TYPE_LINK)
			state->summary->link_count++;
		else if (entry->key.type == MIDR_NLRI_TYPE_NODE_PREFIX)
			state->summary->node_prefix_count++;
		else if (entry->key.type == MIDR_NLRI_TYPE_GROUP_PREFIX)
			state->summary->group_prefix_count++;
	}
	if (entry->suppressed)
		state->summary->suppressed_link_count++;
	if (entry->timer)
		state->summary->pending_timer_count++;
}

int midr_owned_summary_get(struct midr_context *ctx,
			   struct midr_owned_summary *summary)
{
	struct midr_owned_store *store;
	struct midr_owned_count_state state = {
		.summary = summary,
	};

	if (!summary)
		return -EINVAL;
	memset(summary, 0, sizeof(*summary));
	if (!ctx || !ctx->owned_store)
		return -ENOENT;
	store = ctx->owned_store;
	summary->ready = store->ready;
	summary->owner_node_id = store->owner_node_id;
	summary->sequence_failures = store->sequence_failures;
	summary->fightbacks = store->fightbacks;
	summary->representative_group_id = store->representative_group_id;
	summary->representative_candidate = store->representative_candidate;
	summary->representative_committed = store->representative_committed;
	summary->takeover_delay_msec = store->takeover_delay_msec;
	summary->takeover_timer_pending = store->takeover_timer != NULL;
	hash_iterate(store->entries, midr_owned_count_iter, &state);
	return 0;
}

void midr_show_owned(struct vty *vty, struct midr_context *ctx)
{
	struct midr_owned_summary summary;
	struct in_addr owner;

	if (midr_owned_summary_get(ctx, &summary) != 0) {
		vty_out(vty, "MIDR owned object state is unavailable\n");
		return;
	}
	owner.s_addr = summary.owner_node_id;
	vty_out(vty, "MIDR owned objects: %s\n",
		summary.ready ? "READY" : "NOT_READY");
	vty_out(vty, "  owner:              %pI4\n", &owner);
	vty_out(vty, "  memberships:        %zu\n",
		summary.membership_count);
	vty_out(vty, "  links:              %zu\n", summary.link_count);
	vty_out(vty, "  node prefixes:      %zu\n", summary.node_prefix_count);
	vty_out(vty, "  group prefixes:     %zu\n", summary.group_prefix_count);
	vty_out(vty, "  suppressed links:   %zu\n", summary.suppressed_link_count);
	vty_out(vty, "  pending timers:     %zu\n", summary.pending_timer_count);
	vty_out(vty, "  representative group: %u\n", summary.representative_group_id);
	if (summary.representative_candidate) {
		struct in_addr representative = {
			.s_addr = summary.representative_candidate,
		};

		vty_out(vty, "  representative:     %pI4 (%s)\n", &representative,
			summary.representative_committed ? "COMMITTED" : "PENDING");
	}
	vty_out(vty, "  takeover delay:     %u ms%s\n", summary.takeover_delay_msec,
		summary.takeover_timer_pending ? " (timer pending)" : "");
	vty_out(vty, "  sequence failures:  %" PRIu64 "\n", summary.sequence_failures);
	vty_out(vty, "  fightbacks:         %" PRIu64 "\n", summary.fightbacks);
}

int midr_owned_takeover_delay_set(struct midr_context *ctx, uint32_t delay_msec)
{
	struct midr_owned_store *store;

	if (!ctx || !ctx->owned_store)
		return -ENOENT;
	if (delay_msec > MIDR_GROUP_PREFIX_TAKEOVER_DELAY_MAX_MSEC)
		return -EINVAL;
	store = ctx->owned_store;
	if (store->takeover_delay_msec == delay_msec)
		return 0;
	store->takeover_delay_msec = delay_msec;
	if (!store->representative_committed &&
	    store->representative_candidate == store->owner_node_id) {
		event_cancel(&store->takeover_timer);
		store->takeover_delay_elapsed = delay_msec == 0;
		midr_owned_group_reconcile(ctx);
	}
	return 0;
}

int midr_owned_takeover_delay_get(struct midr_context *ctx, uint32_t *delay_msec)
{
	if (!delay_msec)
		return -EINVAL;
	if (!ctx || !ctx->owned_store)
		return -ENOENT;
	*delay_msec = ctx->owned_store->takeover_delay_msec;
	return 0;
}

int midr_owned_test_set_sequence_store(struct midr_context *ctx,
				       const struct midr_sequence_store_ops *ops, void *arg)
{
	struct midr_owned_store *store;

	if (!ctx || !ctx->owned_store)
		return -ENOENT;
	if (!ops || !ops->load_epoch || !ops->save_epoch)
		return -EINVAL;
	store = ctx->owned_store;
	if (store->ready || store->owner_node_id)
		return -EBUSY;
	store->sequence_ops = ops;
	store->sequence_arg = arg;
	return 0;
}

static void midr_owned_fire_timer_iter(struct hash_bucket *bucket, void *arg)
{
	struct midr_owned_entry *entry = bucket->data;
	struct midr_owned_store *store = arg;

	if (!entry->timer)
		return;
	event_cancel(&entry->timer);
	(void)midr_owned_publish_link(store, entry);
}

void midr_owned_test_fire_timers(struct midr_context *ctx)
{
	if (!ctx || !ctx->owned_store)
		return;
	hash_iterate(ctx->owned_store->entries, midr_owned_fire_timer_iter,
		     ctx->owned_store);
}

void midr_owned_test_fire_takeover(struct midr_context *ctx)
{
	struct midr_owned_store *store;

	if (!ctx || !ctx->owned_store)
		return;
	store = ctx->owned_store;
	if (!store->takeover_timer)
		return;
	event_cancel(&store->takeover_timer);
	store->takeover_delay_elapsed = true;
	midr_owned_group_reconcile(ctx);
}
