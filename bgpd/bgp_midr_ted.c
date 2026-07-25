// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Immutable MIDR TED snapshots and the normalized in-memory builder.
 */

#include <zebra.h>

#include <errno.h>
#include <limits.h>
#include <stdlib.h>

#include "linklist.h"
#include "memory.h"

#include "bgpd/bgp_memory.h"
#include "bgpd/bgp_midr_ted_private.h"

DEFINE_MTYPE_STATIC(BGPD, MIDR_TED_STORE, "MIDR TED store");
DEFINE_MTYPE_STATIC(BGPD, MIDR_TED_SNAPSHOT, "MIDR TED snapshot");
DEFINE_MTYPE_STATIC(BGPD, MIDR_TED_ARRAY, "MIDR TED array");
DEFINE_MTYPE_STATIC(BGPD, MIDR_TED_BUILDER, "MIDR TED builder");
DEFINE_MTYPE_STATIC(BGPD, MIDR_TED_CONSUMER, "MIDR TED consumer");

struct midr_ted_snapshot_internal {
	struct midr_ted_snapshot public;
	uint32_t refcount;
	struct midr_ted_node *nodes;
	struct midr_ted_link *intra_links;
	struct midr_ted_link *egress_links;
	struct midr_ted_node_prefix *node_prefixes;
	struct midr_ted_group_edge *group_edges;
	struct midr_ted_prefix_group *prefix_groups;
	size_t pending_link_count;
	size_t pending_node_prefix_count;
	size_t pending_prefix_group_count;
};

struct midr_ted_consumer {
	struct midr_ted_store *store;
	struct midr_ted_consumer_ops ops;
	void *arg;
	uint64_t serial;
	bool removed;
};

struct midr_ted_store {
	struct midr_context *ctx;
	struct midr_ted_snapshot_internal *current;
	struct list *consumers;
	uint64_t consumer_serial;
	size_t pending_link_count;
	size_t pending_node_prefix_count;
	size_t pending_prefix_group_count;
	bool notifying;
};

struct midr_ted_builder {
	uint32_t local_node_id;
	uint32_t local_group_id;

	struct midr_ted_node *nodes;
	size_t node_count;
	size_t node_capacity;
	struct midr_ted_link_input *links;
	size_t link_count;
	size_t link_capacity;
	struct midr_ted_node_prefix *node_prefixes;
	size_t node_prefix_count;
	size_t node_prefix_capacity;
	struct midr_ted_prefix_group *prefix_groups;
	size_t prefix_group_count;
	size_t prefix_group_capacity;
};

static int midr_ted_cmp_u32(uint32_t a, uint32_t b)
{
	return (a > b) - (a < b);
}

static int midr_ted_cmp_u64(uint64_t a, uint64_t b)
{
	return (a > b) - (a < b);
}

static int midr_ted_cmp_node_id(uint32_t a, uint32_t b)
{
	return midr_ted_cmp_u32(ntohl(a), ntohl(b));
}

static bool midr_ted_prefix_key_same(const struct midr_ted_prefix_key *a,
				     const struct midr_ted_prefix_key *b)
{
	return a->afi == b->afi && a->safi == b->safi && prefix_same(&a->prefix, &b->prefix);
}

static int midr_ted_prefix_key_cmp(const struct midr_ted_prefix_key *a,
				   const struct midr_ted_prefix_key *b)
{
	int ret;

	ret = midr_ted_cmp_u32(a->afi, b->afi);
	if (ret)
		return ret;
	ret = midr_ted_cmp_u32(a->safi, b->safi);
	if (ret)
		return ret;
	return prefix_cmp(&a->prefix, &b->prefix);
}

static int midr_ted_prefix_key_normalize(const struct midr_ted_prefix_key *input,
					 struct midr_ted_prefix_key *output)
{
	if (!input || !output || input->safi != SAFI_UNICAST)
		return -EINVAL;
	if ((input->afi == AFI_IP &&
	     (input->prefix.family != AF_INET || input->prefix.prefixlen > IPV4_MAX_BITLEN)) ||
	    (input->afi == AFI_IP6 &&
	     (input->prefix.family != AF_INET6 || input->prefix.prefixlen > IPV6_MAX_BITLEN)) ||
	    (input->afi != AFI_IP && input->afi != AFI_IP6))
		return -EINVAL;

	*output = *input;
	apply_mask(&output->prefix);
	return 0;
}

static int midr_ted_node_cmp(const void *data1, const void *data2)
{
	const struct midr_ted_node *a = data1;
	const struct midr_ted_node *b = data2;

	return midr_ted_cmp_node_id(a->node_id, b->node_id);
}

static int midr_ted_link_cmp(const void *data1, const void *data2)
{
	const struct midr_ted_link *a = data1;
	const struct midr_ted_link *b = data2;
	int ret;

	ret = midr_ted_cmp_node_id(a->local_node_id, b->local_node_id);
	if (ret)
		return ret;
	ret = midr_ted_cmp_node_id(a->remote_node_id, b->remote_node_id);
	if (ret)
		return ret;
	return midr_ted_cmp_u64(a->link_id, b->link_id);
}

static int midr_ted_group_edge_cmp(const void *data1, const void *data2)
{
	const struct midr_ted_group_edge *a = data1;
	const struct midr_ted_group_edge *b = data2;
	int ret;

	ret = midr_ted_cmp_u32(a->source_group_id, b->source_group_id);
	if (ret)
		return ret;
	ret = midr_ted_cmp_u32(a->target_group_id, b->target_group_id);
	if (ret)
		return ret;
	return midr_ted_cmp_u64(a->aggregate_cost, b->aggregate_cost);
}

static int midr_ted_node_prefix_cmp(const void *data1, const void *data2)
{
	const struct midr_ted_node_prefix *a = data1;
	const struct midr_ted_node_prefix *b = data2;
	int ret;

	ret = midr_ted_prefix_key_cmp(&a->key, &b->key);
	if (ret)
		return ret;
	return midr_ted_cmp_node_id(a->node_id, b->node_id);
}

static int midr_ted_prefix_group_cmp(const void *data1, const void *data2)
{
	const struct midr_ted_prefix_group *a = data1;
	const struct midr_ted_prefix_group *b = data2;
	int ret;

	ret = midr_ted_prefix_key_cmp(&a->key, &b->key);
	if (ret)
		return ret;
	return midr_ted_cmp_u32(a->group_id, b->group_id);
}

static bool midr_ted_node_same(const struct midr_ted_node *a, const struct midr_ted_node *b)
{
	return a->node_id == b->node_id && a->group_id == b->group_id &&
	       a->cap_flags == b->cap_flags && a->policy_tags == b->policy_tags;
}

static bool midr_ted_link_same(const struct midr_ted_link *a, const struct midr_ted_link *b)
{
	return a->local_node_id == b->local_node_id && a->remote_node_id == b->remote_node_id &&
	       a->local_group_id == b->local_group_id &&
	       a->remote_group_id == b->remote_group_id && a->link_id == b->link_id &&
	       a->canonical_cost == b->canonical_cost &&
	       a->available_bandwidth_kbps == b->available_bandwidth_kbps &&
	       a->policy_tags == b->policy_tags &&
	       ipaddr_cmp(&a->link_local_address, &b->link_local_address) == 0 &&
	       ipaddr_cmp(&a->link_remote_address, &b->link_remote_address) == 0 &&
	       a->local_ifindex == b->local_ifindex;
}

static bool midr_ted_node_prefix_same(const struct midr_ted_node_prefix *a,
				      const struct midr_ted_node_prefix *b)
{
	return midr_ted_prefix_key_same(&a->key, &b->key) && a->node_id == b->node_id;
}

static bool midr_ted_group_edge_same(const struct midr_ted_group_edge *a,
				     const struct midr_ted_group_edge *b)
{
	return a->source_group_id == b->source_group_id &&
	       a->target_group_id == b->target_group_id && a->aggregate_cost == b->aggregate_cost;
}

static bool midr_ted_prefix_group_same(const struct midr_ted_prefix_group *a,
				       const struct midr_ted_prefix_group *b)
{
	return midr_ted_prefix_key_same(&a->key, &b->key) && a->group_id == b->group_id;
}

static int midr_ted_array_reserve(void **array, size_t *capacity, size_t needed,
				  size_t element_size)
{
	size_t next;

	if (needed <= *capacity)
		return 0;

	next = *capacity ? *capacity : 8;
	while (next < needed) {
		if (next > SIZE_MAX / 2)
			return -EOVERFLOW;
		next *= 2;
	}
	if (next > SIZE_MAX / element_size)
		return -EOVERFLOW;

	*array = XREALLOC(MTYPE_MIDR_TED_ARRAY, *array, next * element_size);
	*capacity = next;
	return 0;
}

static int midr_ted_snapshot_array_alloc(void **array, size_t count, size_t element_size)
{
	if (!count) {
		*array = NULL;
		return 0;
	}
	if (count > SIZE_MAX / element_size)
		return -EOVERFLOW;

	*array = XCALLOC(MTYPE_MIDR_TED_ARRAY, count * element_size);
	return 0;
}

static void midr_ted_snapshot_free(struct midr_ted_snapshot_internal *snapshot)
{
	if (!snapshot)
		return;

	XFREE(MTYPE_MIDR_TED_ARRAY, snapshot->nodes);
	XFREE(MTYPE_MIDR_TED_ARRAY, snapshot->intra_links);
	XFREE(MTYPE_MIDR_TED_ARRAY, snapshot->egress_links);
	XFREE(MTYPE_MIDR_TED_ARRAY, snapshot->node_prefixes);
	XFREE(MTYPE_MIDR_TED_ARRAY, snapshot->group_edges);
	XFREE(MTYPE_MIDR_TED_ARRAY, snapshot->prefix_groups);
	XFREE(MTYPE_MIDR_TED_SNAPSHOT, snapshot);
}

static void midr_ted_snapshot_put(struct midr_ted_snapshot_internal *snapshot)
{
	if (!snapshot)
		return;

	assert(snapshot->refcount > 0);
	snapshot->refcount--;
	if (!snapshot->refcount)
		midr_ted_snapshot_free(snapshot);
}

static const struct midr_ted_node *
midr_ted_builder_find_node(const struct midr_ted_builder *builder, uint32_t node_id)
{
	size_t i;

	for (i = 0; i < builder->node_count; i++)
		if (builder->nodes[i].node_id == node_id)
			return &builder->nodes[i];
	return NULL;
}

static bool midr_ted_builder_group_exists(const struct midr_ted_builder *builder, uint32_t group_id)
{
	size_t i;

	for (i = 0; i < builder->node_count; i++)
		if (builder->nodes[i].group_id == group_id)
			return true;
	return false;
}

static void midr_ted_link_from_input(struct midr_ted_link *output,
				     const struct midr_ted_link_input *input,
				     uint32_t local_group_id, uint32_t remote_group_id)
{
	memset(output, 0, sizeof(*output));
	output->local_node_id = input->local_node_id;
	output->remote_node_id = input->remote_node_id;
	output->local_group_id = local_group_id;
	output->remote_group_id = remote_group_id;
	output->link_id = input->link_id;
	output->canonical_cost = input->canonical_cost;
	output->available_bandwidth_kbps = input->available_bandwidth_kbps;
	output->policy_tags = input->policy_tags;
	output->link_local_address = input->link_local_address;
	output->link_remote_address = input->link_remote_address;
	output->local_ifindex = input->local_ifindex;
}

static int midr_ted_snapshot_build(const struct midr_ted_builder *builder,
				   uint64_t sync_reason_flags,
				   struct midr_ted_snapshot_internal **out)
{
	const uint64_t valid_reasons = MIDR_TED_SYNC_REASON_EOR_TIMEOUT |
				       MIDR_TED_SYNC_REASON_RESYNC_FAILED;
	struct midr_ted_snapshot_internal *snapshot = NULL;
	const struct midr_ted_node *local;
	size_t group_edge_candidates = 0;
	size_t group_edge_count = 0;
	size_t i;
	int ret;

	if (!builder || !out || *out)
		return -EINVAL;
	if (sync_reason_flags & ~valid_reasons)
		return -EINVAL;

	local = midr_ted_builder_find_node(builder, builder->local_node_id);
	if (!local || !local->group_id || local->group_id != builder->local_group_id)
		return -EINVAL;

	snapshot = XCALLOC(MTYPE_MIDR_TED_SNAPSHOT, sizeof(*snapshot));
	snapshot->refcount = 1;
	snapshot->public.local_node_id = builder->local_node_id;
	snapshot->public.local_group_id = builder->local_group_id;
	snapshot->public.ready = true;
	snapshot->public.sync_reason_flags = sync_reason_flags;

	ret = midr_ted_snapshot_array_alloc((void **)&snapshot->nodes, builder->node_count,
					    sizeof(*snapshot->nodes));
	if (ret)
		goto fail;
	ret = midr_ted_snapshot_array_alloc((void **)&snapshot->intra_links, builder->link_count,
					    sizeof(*snapshot->intra_links));
	if (ret)
		goto fail;
	ret = midr_ted_snapshot_array_alloc((void **)&snapshot->egress_links, builder->link_count,
					    sizeof(*snapshot->egress_links));
	if (ret)
		goto fail;
	ret = midr_ted_snapshot_array_alloc((void **)&snapshot->group_edges, builder->link_count,
					    sizeof(*snapshot->group_edges));
	if (ret)
		goto fail;
	ret = midr_ted_snapshot_array_alloc((void **)&snapshot->node_prefixes,
					    builder->node_prefix_count,
					    sizeof(*snapshot->node_prefixes));
	if (ret)
		goto fail;
	ret = midr_ted_snapshot_array_alloc((void **)&snapshot->prefix_groups,
					    builder->prefix_group_count,
					    sizeof(*snapshot->prefix_groups));
	if (ret)
		goto fail;

	for (i = 0; i < builder->node_count; i++) {
		if (builder->nodes[i].group_id != builder->local_group_id)
			continue;
		snapshot->nodes[snapshot->public.node_count++] = builder->nodes[i];
	}

	for (i = 0; i < builder->link_count; i++) {
		const struct midr_ted_link_input *input = &builder->links[i];
		const struct midr_ted_node *local_node;
		const struct midr_ted_node *remote_node;
		struct midr_ted_link link;

		local_node = midr_ted_builder_find_node(builder, input->local_node_id);
		remote_node = midr_ted_builder_find_node(builder, input->remote_node_id);
		if (!local_node || !remote_node || !local_node->group_id ||
		    !remote_node->group_id) {
			snapshot->pending_link_count++;
			continue;
		}

		midr_ted_link_from_input(&link, input, local_node->group_id, remote_node->group_id);
		if (local_node->group_id == remote_node->group_id) {
			if (local_node->group_id == builder->local_group_id)
				snapshot->intra_links[snapshot->public.intra_link_count++] = link;
			continue;
		}

		snapshot->group_edges[group_edge_candidates++] = (struct midr_ted_group_edge){
			.source_group_id = local_node->group_id,
			.target_group_id = remote_node->group_id,
			.aggregate_cost = input->canonical_cost,
		};
		if (local_node->group_id == builder->local_group_id)
			snapshot->egress_links[snapshot->public.egress_link_count++] = link;
	}

	if (group_edge_candidates)
		qsort(snapshot->group_edges, group_edge_candidates, sizeof(*snapshot->group_edges),
		      midr_ted_group_edge_cmp);
	for (i = 0; i < group_edge_candidates; i++) {
		if (group_edge_count &&
		    snapshot->group_edges[group_edge_count - 1].source_group_id ==
			    snapshot->group_edges[i].source_group_id &&
		    snapshot->group_edges[group_edge_count - 1].target_group_id ==
			    snapshot->group_edges[i].target_group_id)
			continue;
		snapshot->group_edges[group_edge_count++] = snapshot->group_edges[i];
	}
	snapshot->public.group_edge_count = group_edge_count;

	for (i = 0; i < builder->node_prefix_count; i++) {
		const struct midr_ted_node_prefix *prefix = &builder->node_prefixes[i];
		const struct midr_ted_node *node = midr_ted_builder_find_node(builder,
									      prefix->node_id);

		if (!node || !node->group_id) {
			snapshot->pending_node_prefix_count++;
			continue;
		}
		if (node->group_id == builder->local_group_id)
			snapshot->node_prefixes[snapshot->public.node_prefix_count++] = *prefix;
	}

	for (i = 0; i < builder->prefix_group_count; i++) {
		const struct midr_ted_prefix_group *prefix = &builder->prefix_groups[i];

		if (!midr_ted_builder_group_exists(builder, prefix->group_id)) {
			snapshot->pending_prefix_group_count++;
			continue;
		}
		snapshot->prefix_groups[snapshot->public.prefix_group_count++] = *prefix;
	}

	if (snapshot->public.node_count > 1)
		qsort(snapshot->nodes, snapshot->public.node_count, sizeof(*snapshot->nodes),
		      midr_ted_node_cmp);
	if (snapshot->public.intra_link_count > 1)
		qsort(snapshot->intra_links, snapshot->public.intra_link_count,
		      sizeof(*snapshot->intra_links), midr_ted_link_cmp);
	if (snapshot->public.egress_link_count > 1)
		qsort(snapshot->egress_links, snapshot->public.egress_link_count,
		      sizeof(*snapshot->egress_links), midr_ted_link_cmp);
	if (snapshot->public.node_prefix_count > 1)
		qsort(snapshot->node_prefixes, snapshot->public.node_prefix_count,
		      sizeof(*snapshot->node_prefixes), midr_ted_node_prefix_cmp);
	if (snapshot->public.prefix_group_count > 1)
		qsort(snapshot->prefix_groups, snapshot->public.prefix_group_count,
		      sizeof(*snapshot->prefix_groups), midr_ted_prefix_group_cmp);

	snapshot->public.nodes = snapshot->nodes;
	snapshot->public.intra_links = snapshot->intra_links;
	snapshot->public.egress_links = snapshot->egress_links;
	snapshot->public.node_prefixes = snapshot->node_prefixes;
	snapshot->public.group_edges = snapshot->group_edges;
	snapshot->public.prefix_groups = snapshot->prefix_groups;

	*out = snapshot;
	return 0;

fail:
	midr_ted_snapshot_free(snapshot);
	return ret;
}

static bool midr_ted_nodes_same(const struct midr_ted_snapshot *a,
				const struct midr_ted_snapshot *b)
{
	size_t i;

	if (a->node_count != b->node_count)
		return false;
	for (i = 0; i < a->node_count; i++)
		if (!midr_ted_node_same(&a->nodes[i], &b->nodes[i]))
			return false;
	return true;
}

static bool midr_ted_links_same(const struct midr_ted_link *a, size_t a_count,
				const struct midr_ted_link *b, size_t b_count)
{
	size_t i;

	if (a_count != b_count)
		return false;
	for (i = 0; i < a_count; i++)
		if (!midr_ted_link_same(&a[i], &b[i]))
			return false;
	return true;
}

static bool midr_ted_node_prefixes_same(const struct midr_ted_snapshot *a,
					const struct midr_ted_snapshot *b)
{
	size_t i;

	if (a->node_prefix_count != b->node_prefix_count)
		return false;
	for (i = 0; i < a->node_prefix_count; i++)
		if (!midr_ted_node_prefix_same(&a->node_prefixes[i], &b->node_prefixes[i]))
			return false;
	return true;
}

static bool midr_ted_group_edges_same(const struct midr_ted_snapshot *a,
				      const struct midr_ted_snapshot *b)
{
	size_t i;

	if (a->group_edge_count != b->group_edge_count)
		return false;
	for (i = 0; i < a->group_edge_count; i++)
		if (!midr_ted_group_edge_same(&a->group_edges[i], &b->group_edges[i]))
			return false;
	return true;
}

static bool midr_ted_prefix_groups_same(const struct midr_ted_snapshot *a,
					const struct midr_ted_snapshot *b)
{
	size_t i;

	if (a->prefix_group_count != b->prefix_group_count)
		return false;
	for (i = 0; i < a->prefix_group_count; i++)
		if (!midr_ted_prefix_group_same(&a->prefix_groups[i], &b->prefix_groups[i]))
			return false;
	return true;
}

static uint32_t midr_ted_snapshot_changes(const struct midr_ted_snapshot *old,
					  const struct midr_ted_snapshot *new)
{
	uint32_t changes = MIDR_TED_CHANGE_NONE;

	if (!old)
		return MIDR_TED_CHANGE_ALL;
	if (old->local_node_id != new->local_node_id || old->local_group_id != new->local_group_id)
		changes |= MIDR_TED_CHANGE_LOCAL;
	if (!midr_ted_nodes_same(old, new))
		changes |= MIDR_TED_CHANGE_NODES;
	if (!midr_ted_links_same(old->intra_links, old->intra_link_count, new->intra_links,
				 new->intra_link_count) ||
	    !midr_ted_links_same(old->egress_links, old->egress_link_count, new->egress_links,
				 new->egress_link_count))
		changes |= MIDR_TED_CHANGE_LINKS;
	if (!midr_ted_group_edges_same(old, new))
		changes |= MIDR_TED_CHANGE_GROUP_EDGES;
	if (!midr_ted_node_prefixes_same(old, new) || !midr_ted_prefix_groups_same(old, new))
		changes |= MIDR_TED_CHANGE_PREFIXES;
	if (old->ready != new->ready || old->sync_reason_flags != new->sync_reason_flags)
		changes |= MIDR_TED_CHANGE_SYNC;
	return changes;
}

static void midr_ted_consumer_free(void *data)
{
	XFREE(MTYPE_MIDR_TED_CONSUMER, data);
}

static void midr_ted_consumers_sweep(struct midr_ted_store *store)
{
	struct listnode *node;
	struct listnode *next;
	struct midr_ted_consumer *consumer;

	for (ALL_LIST_ELEMENTS(store->consumers, node, next, consumer)) {
		if (!consumer->removed)
			continue;
		list_delete_node(store->consumers, node);
		midr_ted_consumer_free(consumer);
	}
}

static void midr_ted_notify(struct midr_ted_store *store, uint64_t generation,
			    uint32_t change_flags)
{
	struct listnode *node;
	struct listnode *next;
	struct midr_ted_consumer *consumer;
	uint64_t max_serial = store->consumer_serial;

	store->notifying = true;
	for (ALL_LIST_ELEMENTS(store->consumers, node, next, consumer)) {
		if (consumer->removed || consumer->serial > max_serial)
			continue;
		consumer->ops.snapshot_changed(store->ctx, generation, change_flags, consumer->arg);
	}
	store->notifying = false;
	midr_ted_consumers_sweep(store);
}

int midr_ted_context_init(struct midr_context *ctx)
{
	struct midr_ted_store *store;

	if (!ctx)
		return -EINVAL;
	if (ctx->ted_store)
		return -EALREADY;

	store = XCALLOC(MTYPE_MIDR_TED_STORE, sizeof(*store));
	store->ctx = ctx;
	store->consumers = list_new();
	store->consumers->del = midr_ted_consumer_free;
	ctx->ted_store = store;
	return 0;
}

void midr_ted_context_finish(struct midr_context *ctx)
{
	struct midr_ted_store *store;
	struct listnode *node;
	struct midr_ted_consumer *consumer;

	if (!ctx || !ctx->ted_store)
		return;

	store = ctx->ted_store;
	for (ALL_LIST_ELEMENTS_RO(store->consumers, node, consumer))
		consumer->store = NULL;
	list_delete(&store->consumers);
	midr_ted_snapshot_put(store->current);
	ctx->ted_store = NULL;
	XFREE(MTYPE_MIDR_TED_STORE, store);
}

int midr_ted_builder_create(uint32_t local_node_id, uint32_t local_group_id,
			    struct midr_ted_builder **out)
{
	struct midr_ted_builder *builder;

	if (!out || *out || !local_node_id || !local_group_id)
		return -EINVAL;

	builder = XCALLOC(MTYPE_MIDR_TED_BUILDER, sizeof(*builder));
	builder->local_node_id = local_node_id;
	builder->local_group_id = local_group_id;
	*out = builder;
	return 0;
}

void midr_ted_builder_destroy(struct midr_ted_builder **builder)
{
	if (!builder || !*builder)
		return;

	XFREE(MTYPE_MIDR_TED_ARRAY, (*builder)->nodes);
	XFREE(MTYPE_MIDR_TED_ARRAY, (*builder)->links);
	XFREE(MTYPE_MIDR_TED_ARRAY, (*builder)->node_prefixes);
	XFREE(MTYPE_MIDR_TED_ARRAY, (*builder)->prefix_groups);
	XFREE(MTYPE_MIDR_TED_BUILDER, *builder);
}

int midr_ted_builder_add_node(struct midr_ted_builder *builder, const struct midr_ted_node *node)
{
	struct midr_ted_node normalized = {};
	size_t i;
	int ret;

	if (!builder || !node || !node->node_id)
		return -EINVAL;
	for (i = 0; i < builder->node_count; i++)
		if (builder->nodes[i].node_id == node->node_id)
			return -EEXIST;

	ret = midr_ted_array_reserve((void **)&builder->nodes, &builder->node_capacity,
				     builder->node_count + 1, sizeof(*builder->nodes));
	if (ret)
		return ret;

	normalized.node_id = node->node_id;
	normalized.group_id = node->group_id;
	normalized.cap_flags = node->cap_flags;
	normalized.policy_tags = node->policy_tags;
	builder->nodes[builder->node_count++] = normalized;
	return 0;
}

int midr_ted_builder_add_link(struct midr_ted_builder *builder,
			      const struct midr_ted_link_input *link)
{
	struct midr_ted_link_input normalized = {};
	size_t i;
	int ret;

	if (!builder || !link || !link->local_node_id || !link->remote_node_id ||
	    link->local_node_id == link->remote_node_id || !link->canonical_cost ||
	    link->canonical_cost > MIDR_TED_LINK_COST_MAX || !link->available_bandwidth_kbps ||
	    link->local_ifindex < 0 ||
	    (link->local_node_id != builder->local_node_id && link->local_ifindex != 0) ||
	    (link->link_local_address.ipa_type != IPADDR_V4 &&
	     link->link_local_address.ipa_type != IPADDR_V6) ||
	    link->link_local_address.ipa_type != link->link_remote_address.ipa_type)
		return -EINVAL;

	for (i = 0; i < builder->link_count; i++)
		if (builder->links[i].local_node_id == link->local_node_id &&
		    builder->links[i].remote_node_id == link->remote_node_id &&
		    builder->links[i].link_id == link->link_id)
			return -EEXIST;

	ret = midr_ted_array_reserve((void **)&builder->links, &builder->link_capacity,
				     builder->link_count + 1, sizeof(*builder->links));
	if (ret)
		return ret;

	normalized.local_node_id = link->local_node_id;
	normalized.remote_node_id = link->remote_node_id;
	normalized.link_id = link->link_id;
	normalized.canonical_cost = link->canonical_cost;
	normalized.available_bandwidth_kbps = link->available_bandwidth_kbps;
	normalized.policy_tags = link->policy_tags;
	normalized.link_local_address = link->link_local_address;
	normalized.link_remote_address = link->link_remote_address;
	normalized.local_ifindex = link->local_ifindex;
	builder->links[builder->link_count++] = normalized;
	return 0;
}

int midr_ted_builder_add_node_prefix(struct midr_ted_builder *builder,
				     const struct midr_ted_node_prefix *prefix)
{
	struct midr_ted_node_prefix normalized = {};
	size_t i;
	int ret;

	if (!builder || !prefix || !prefix->node_id)
		return -EINVAL;
	ret = midr_ted_prefix_key_normalize(&prefix->key, &normalized.key);
	if (ret)
		return ret;
	normalized.node_id = prefix->node_id;

	for (i = 0; i < builder->node_prefix_count; i++)
		if (midr_ted_node_prefix_same(&builder->node_prefixes[i], &normalized))
			return -EEXIST;

	ret = midr_ted_array_reserve((void **)&builder->node_prefixes,
				     &builder->node_prefix_capacity, builder->node_prefix_count + 1,
				     sizeof(*builder->node_prefixes));
	if (ret)
		return ret;
	builder->node_prefixes[builder->node_prefix_count++] = normalized;
	return 0;
}

int midr_ted_builder_add_prefix_group(struct midr_ted_builder *builder,
				      const struct midr_ted_prefix_group *prefix)
{
	struct midr_ted_prefix_group normalized = {};
	size_t i;
	int ret;

	if (!builder || !prefix || !prefix->group_id)
		return -EINVAL;
	ret = midr_ted_prefix_key_normalize(&prefix->key, &normalized.key);
	if (ret)
		return ret;
	normalized.group_id = prefix->group_id;

	for (i = 0; i < builder->prefix_group_count; i++)
		if (midr_ted_prefix_group_same(&builder->prefix_groups[i], &normalized))
			return -EEXIST;

	ret = midr_ted_array_reserve((void **)&builder->prefix_groups,
				     &builder->prefix_group_capacity,
				     builder->prefix_group_count + 1,
				     sizeof(*builder->prefix_groups));
	if (ret)
		return ret;
	builder->prefix_groups[builder->prefix_group_count++] = normalized;
	return 0;
}

int midr_ted_builder_publish(struct midr_context *ctx, const struct midr_ted_builder *builder,
			     uint64_t sync_reason_flags)
{
	struct midr_ted_snapshot_internal *candidate = NULL;
	struct midr_ted_snapshot_internal *old;
	struct midr_ted_store *store;
	uint32_t changes;
	int ret;

	if (!ctx || !ctx->ted_store)
		return -ENOENT;

	ret = midr_ted_snapshot_build(builder, sync_reason_flags, &candidate);
	if (ret)
		return ret;

	store = ctx->ted_store;
	old = store->current;
	changes = midr_ted_snapshot_changes(old ? &old->public : NULL, &candidate->public);
	if (!changes) {
		store->pending_link_count = candidate->pending_link_count;
		store->pending_node_prefix_count = candidate->pending_node_prefix_count;
		store->pending_prefix_group_count = candidate->pending_prefix_group_count;
		midr_ted_snapshot_put(candidate);
		return 0;
	}
	if (old && old->public.generation == UINT64_MAX) {
		midr_ted_snapshot_put(candidate);
		return -EOVERFLOW;
	}

	candidate->public.generation = old ? old->public.generation + 1 : 1;
	store->current = candidate;
	store->pending_link_count = candidate->pending_link_count;
	store->pending_node_prefix_count = candidate->pending_node_prefix_count;
	store->pending_prefix_group_count = candidate->pending_prefix_group_count;
	midr_ted_notify(store, candidate->public.generation, changes);
	midr_ted_snapshot_put(old);
	return 0;
}

int midr_ted_snapshot_get(struct midr_context *ctx, const struct midr_ted_snapshot **out)
{
	struct midr_ted_snapshot_internal *snapshot;

	if (!out || *out)
		return -EINVAL;
	if (!ctx || !ctx->ted_store)
		return -ENOENT;

	snapshot = ctx->ted_store->current;
	if (!snapshot || !snapshot->public.ready)
		return -EAGAIN;

	snapshot->refcount++;
	*out = &snapshot->public;
	return 0;
}

void midr_ted_snapshot_release(const struct midr_ted_snapshot **snapshot)
{
	struct midr_ted_snapshot_internal *internal;

	if (!snapshot || !*snapshot)
		return;

	internal = (struct midr_ted_snapshot_internal *)(uintptr_t)*snapshot;
	*snapshot = NULL;
	midr_ted_snapshot_put(internal);
}

bool midr_ted_generation_is_current(struct midr_context *ctx, uint64_t generation)
{
	if (!ctx || !ctx->ted_store || !ctx->ted_store->current || !generation)
		return false;

	return ctx->ted_store->current->public.ready &&
	       ctx->ted_store->current->public.generation == generation;
}

int midr_ted_consumer_register(struct midr_context *ctx, const struct midr_ted_consumer_ops *ops,
			       void *arg, struct midr_ted_consumer **out)
{
	struct midr_ted_consumer *consumer;
	struct midr_ted_store *store;

	if (!out || *out || !ops || !ops->snapshot_changed)
		return -EINVAL;
	if (!ctx || !ctx->ted_store)
		return -ENOENT;

	store = ctx->ted_store;
	if (store->consumer_serial == UINT64_MAX)
		return -EOVERFLOW;
	consumer = XCALLOC(MTYPE_MIDR_TED_CONSUMER, sizeof(*consumer));
	consumer->store = store;
	consumer->ops = *ops;
	consumer->arg = arg;
	consumer->serial = ++store->consumer_serial;
	listnode_add(store->consumers, consumer);
	*out = consumer;
	return 0;
}

void midr_ted_consumer_unregister(struct midr_context *ctx, struct midr_ted_consumer **consumer)
{
	struct midr_ted_consumer *target;
	struct midr_ted_store *store;

	if (!consumer || !*consumer)
		return;

	target = *consumer;
	if (!ctx || !ctx->ted_store || target->store != ctx->ted_store)
		return;

	*consumer = NULL;
	store = ctx->ted_store;
	if (store->notifying) {
		target->removed = true;
		return;
	}

	listnode_delete(store->consumers, target);
	midr_ted_consumer_free(target);
}

int midr_ted_status_get(struct midr_context *ctx, struct midr_ted_status *status)
{
	struct midr_ted_snapshot_internal *snapshot;

	if (!status)
		return -EINVAL;
	memset(status, 0, sizeof(*status));
	if (!ctx || !ctx->ted_store)
		return -ENOENT;

	snapshot = ctx->ted_store->current;
	status->consumer_count = listcount(ctx->ted_store->consumers);
	if (!snapshot)
		return 0;

	status->ready = snapshot->public.ready;
	status->generation = snapshot->public.generation;
	status->sync_reason_flags = snapshot->public.sync_reason_flags;
	status->pending_link_count = ctx->ted_store->pending_link_count;
	status->pending_node_prefix_count = ctx->ted_store->pending_node_prefix_count;
	status->pending_prefix_group_count = ctx->ted_store->pending_prefix_group_count;
	return 0;
}

int midr_ted_test_generation_set(struct midr_context *ctx, uint64_t generation)
{
	if (!ctx || !ctx->ted_store)
		return -ENOENT;
	if (!generation)
		return -EINVAL;
	if (!ctx->ted_store->current)
		return -EAGAIN;

	ctx->ted_store->current->public.generation = generation;
	return 0;
}
