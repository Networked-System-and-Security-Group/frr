/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "midr-ted.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct midr_ted {
	struct midr_ted_config config;
	struct midr_consumer_event *events;
	size_t count;
	uint64_t generation;
	uint64_t source_generation;
	uint32_t local_node_id;
	uint32_t local_group_id;
	struct midr_ted_node *nodes;
	size_t node_count;
	struct midr_ted_link *intra_links;
	size_t intra_link_count;
	struct midr_ted_link *egress_links;
	size_t egress_link_count;
	struct midr_ted_node_prefix *node_prefixes;
	size_t node_prefix_count;
	struct midr_ted_group_edge *group_edges;
	size_t group_edge_count;
	struct midr_ted_prefix_group *prefix_groups;
	size_t prefix_group_count;
	bool formal_view;
	enum midr_ted_state state;
	int last_error;
	int fail_next;
	struct midr_ted_consumer *consumers;
	size_t notify_depth;
};

struct midr_ted_consumer {
	struct midr_ted_consumer *next;
	struct midr_ted *ted;
	struct midr_ted_consumer_ops ops;
	void *arg;
	bool removed;
};

struct midr_ted_stage {
	struct midr_consumer_event *events;
	size_t count;
	uint64_t generation;
	uint64_t source_generation;
	uint32_t local_node_id;
	uint32_t local_group_id;
	struct midr_ted_node *nodes;
	size_t node_count;
	struct midr_ted_link *intra_links;
	size_t intra_link_count;
	struct midr_ted_link *egress_links;
	size_t egress_link_count;
	struct midr_ted_node_prefix *node_prefixes;
	size_t node_prefix_count;
	struct midr_ted_group_edge *group_edges;
	size_t group_edge_count;
	struct midr_ted_prefix_group *prefix_groups;
	size_t prefix_group_count;
	bool formal_view;
	bool no_change;
	bool local_metadata_update;
};

static int compare_u32(uint32_t left, uint32_t right)
{
	return (left > right) - (left < right);
}

static int compare_u64(uint64_t left, uint64_t right)
{
	return (left > right) - (left < right);
}

static int event_key_compare(const void *leftp, const void *rightp)
{
	const struct midr_consumer_event *left = leftp;
	const struct midr_consumer_event *right = rightp;
	int ret;

	ret = compare_u32((uint32_t)left->kind, (uint32_t)right->kind);
	if (ret)
		return ret;
	ret = compare_u32(left->originator, right->originator);
	if (ret)
		return ret;
	switch (left->kind) {
	case MIDR_CONSUMER_LINK:
		ret = compare_u32(left->remote, right->remote);
		return ret ? ret : compare_u64(left->link_id, right->link_id);
	case MIDR_CONSUMER_NODE_PREFIX:
		ret = compare_u32(left->family, right->family);
		if (ret)
			return ret;
		ret = compare_u32(left->prefix_len, right->prefix_len);
		return ret ? ret : memcmp(left->prefix, right->prefix,
					  sizeof(left->prefix));
	case MIDR_CONSUMER_GROUP_PREFIX:
		ret = compare_u32(left->group, right->group);
		if (ret)
			return ret;
		ret = compare_u32(left->family, right->family);
		if (ret)
			return ret;
		ret = compare_u32(left->prefix_len, right->prefix_len);
		return ret ? ret : memcmp(left->prefix, right->prefix,
					  sizeof(left->prefix));
	case MIDR_CONSUMER_SNAPSHOT_BEGIN:
	case MIDR_CONSUMER_SNAPSHOT_END:
	default:
		return 0;
	}
}

static bool event_semantic_equal(const struct midr_consumer_event *left,
				 const struct midr_consumer_event *right)
{
	if (event_key_compare(left, right))
		return false;
	if (left->metric != right->metric || left->family != right->family ||
	    memcmp(left->local_address, right->local_address,
		   sizeof(left->local_address)) ||
	    memcmp(left->remote_address, right->remote_address,
		   sizeof(left->remote_address)))
		return false;
	return true;
}

static bool snapshot_semantic_equal(
	const struct midr_ted *ted, const struct midr_consumer_event *events,
	size_t count)
{
	if (ted->count != count)
		return false;
	for (size_t i = 0; i < count; i++)
		if (!event_semantic_equal(&ted->events[i], &events[i]))
			return false;
	return true;
}

static int prefix_key_compare(const struct midr_ted_prefix_key *left,
			      const struct midr_ted_prefix_key *right)
{
	int ret = compare_u32(left->family, right->family);

	if (ret)
		return ret;
	ret = compare_u32(left->prefix_len, right->prefix_len);
	return ret ? ret : memcmp(left->prefix, right->prefix,
				 sizeof(left->prefix));
}

static int node_compare(const void *leftp, const void *rightp)
{
	const struct midr_ted_node *left = leftp;
	const struct midr_ted_node *right = rightp;
	int ret = compare_u32(left->node_id, right->node_id);

	return ret ? ret : compare_u32(left->group_id, right->group_id);
}

static int link_compare(const void *leftp, const void *rightp)
{
	const struct midr_ted_link *left = leftp;
	const struct midr_ted_link *right = rightp;
	int ret = compare_u32(left->local_node_id, right->local_node_id);

	if (ret)
		return ret;
	ret = compare_u32(left->remote_node_id, right->remote_node_id);
	return ret ? ret : compare_u64(left->link_id, right->link_id);
}

static int node_prefix_compare(const void *leftp, const void *rightp)
{
	const struct midr_ted_node_prefix *left = leftp;
	const struct midr_ted_node_prefix *right = rightp;
	int ret = prefix_key_compare(&left->key, &right->key);

	return ret ? ret : compare_u32(left->node_id, right->node_id);
}

static int group_edge_compare(const void *leftp, const void *rightp)
{
	const struct midr_ted_group_edge *left = leftp;
	const struct midr_ted_group_edge *right = rightp;
	int ret = compare_u32(left->source_group_id, right->source_group_id);

	return ret ? ret : compare_u32(left->target_group_id,
					       right->target_group_id);
}

static int prefix_group_compare(const void *leftp, const void *rightp)
{
	const struct midr_ted_prefix_group *left = leftp;
	const struct midr_ted_prefix_group *right = rightp;
	int ret = prefix_key_compare(&left->key, &right->key);

	return ret ? ret : compare_u32(left->group_id, right->group_id);
}

static void stage_formal_free(struct midr_ted_stage *stage)
{
	if (!stage)
		return;
	free(stage->nodes);
	free(stage->intra_links);
	free(stage->egress_links);
	free(stage->node_prefixes);
	free(stage->group_edges);
	free(stage->prefix_groups);
	stage->nodes = NULL;
	stage->intra_links = NULL;
	stage->egress_links = NULL;
	stage->node_prefixes = NULL;
	stage->group_edges = NULL;
	stage->prefix_groups = NULL;
}

static void ted_formal_free(struct midr_ted *ted)
{
	if (!ted)
		return;
	free(ted->nodes);
	free(ted->intra_links);
	free(ted->egress_links);
	free(ted->node_prefixes);
	free(ted->group_edges);
	free(ted->prefix_groups);
	ted->nodes = NULL;
	ted->intra_links = NULL;
	ted->egress_links = NULL;
	ted->node_prefixes = NULL;
	ted->group_edges = NULL;
	ted->prefix_groups = NULL;
}

static bool array_equal(const void *left, const void *right, size_t count,
			size_t element_size)
{
	return !count || !memcmp(left, right, count * element_size);
}

static bool formal_semantic_equal(const struct midr_ted *ted,
				  const struct midr_ted_stage *stage)
{
	return ted->formal_view == stage->formal_view &&
	       (!stage->formal_view ||
		(ted->local_node_id == stage->local_node_id &&
		 ted->local_group_id == stage->local_group_id &&
		 ted->node_count == stage->node_count &&
		 ted->intra_link_count == stage->intra_link_count &&
		 ted->egress_link_count == stage->egress_link_count &&
		 ted->node_prefix_count == stage->node_prefix_count &&
		 ted->group_edge_count == stage->group_edge_count &&
		 ted->prefix_group_count == stage->prefix_group_count &&
		 array_equal(ted->nodes, stage->nodes, ted->node_count,
			     sizeof(*ted->nodes)) &&
		 array_equal(ted->intra_links, stage->intra_links,
			     ted->intra_link_count, sizeof(*ted->intra_links)) &&
		 array_equal(ted->egress_links, stage->egress_links,
			     ted->egress_link_count, sizeof(*ted->egress_links)) &&
		 array_equal(ted->node_prefixes, stage->node_prefixes,
			     ted->node_prefix_count, sizeof(*ted->node_prefixes)) &&
		 array_equal(ted->group_edges, stage->group_edges,
			     ted->group_edge_count, sizeof(*ted->group_edges)) &&
		 array_equal(ted->prefix_groups, stage->prefix_groups,
			     ted->prefix_group_count, sizeof(*ted->prefix_groups))));
}

static uint32_t formal_change_flags(const struct midr_ted *ted,
				    const struct midr_ted_stage *stage)
{
	uint32_t flags = MIDR_TED_CHANGE_NONE;

	if (ted->formal_view != stage->formal_view)
		return MIDR_TED_CHANGE_ALL;
	if (!stage->formal_view)
		return MIDR_TED_CHANGE_ALL;
	if (ted->local_node_id != stage->local_node_id ||
	    ted->local_group_id != stage->local_group_id)
		flags |= MIDR_TED_CHANGE_LOCAL;
	if (ted->node_count != stage->node_count ||
	    !array_equal(ted->nodes, stage->nodes, ted->node_count,
			 sizeof(*ted->nodes)))
		flags |= MIDR_TED_CHANGE_NODES;
	if (ted->intra_link_count != stage->intra_link_count ||
	    ted->egress_link_count != stage->egress_link_count ||
	    !array_equal(ted->intra_links, stage->intra_links,
			 ted->intra_link_count, sizeof(*ted->intra_links)) ||
	    !array_equal(ted->egress_links, stage->egress_links,
			 ted->egress_link_count, sizeof(*ted->egress_links)))
		flags |= MIDR_TED_CHANGE_LINKS;
	if (ted->group_edge_count != stage->group_edge_count ||
	    !array_equal(ted->group_edges, stage->group_edges,
			 ted->group_edge_count, sizeof(*ted->group_edges)))
		flags |= MIDR_TED_CHANGE_GROUP_EDGES;
	if (ted->node_prefix_count != stage->node_prefix_count ||
	    ted->prefix_group_count != stage->prefix_group_count ||
	    !array_equal(ted->node_prefixes, stage->node_prefixes,
			 ted->node_prefix_count, sizeof(*ted->node_prefixes)) ||
	    !array_equal(ted->prefix_groups, stage->prefix_groups,
			 ted->prefix_group_count, sizeof(*ted->prefix_groups)))
		flags |= MIDR_TED_CHANGE_PREFIXES;
	return flags;
}

static void ted_consumer_prune(struct midr_ted *ted)
{
	struct midr_ted_consumer **cursor = &ted->consumers;

	while (*cursor) {
		struct midr_ted_consumer *consumer = *cursor;

		if (!consumer->removed) {
			cursor = &consumer->next;
			continue;
		}
		*cursor = consumer->next;
		free(consumer);
	}
}

static void ted_consumer_notify(struct midr_ted *ted, uint64_t generation,
				uint32_t change_flags)
{
	struct midr_ted_consumer *consumer;

	if (!change_flags)
		return;
	ted->notify_depth++;
	for (consumer = ted->consumers; consumer; consumer = consumer->next)
		if (!consumer->removed && consumer->ops.snapshot_changed)
			consumer->ops.snapshot_changed(ted, generation, change_flags,
						       consumer->arg);
	if (!ted->notify_depth)
		abort();
	ted->notify_depth--;
	if (!ted->notify_depth)
		ted_consumer_prune(ted);
}

static int membership_group(const struct midr_lsdb_snapshot *lsdb,
			    uint32_t node_id, uint32_t *group)
{
	for (size_t i = 0; i < lsdb->count; i++) {
		const struct midr_core_object *object = &lsdb->entries[i].object;

		if (object->identity.type == MIDR_CORE_MEMBERSHIP &&
		    object->identity.originator == node_id) {
			*group = object->group;
			return 0;
		}
	}
	return -ENOENT;
}

static void prefix_key_from_object(const struct midr_core_object *object,
				   struct midr_ted_prefix_key *key)
{
	key->family = object->identity.family;
	key->prefix_len = object->identity.prefix_len;
	memcpy(key->prefix, object->identity.prefix, sizeof(key->prefix));
}

static int add_group_edge(struct midr_ted_stage *stage, uint32_t source,
			  uint32_t target, uint32_t cost)
{
	for (size_t i = 0; i < stage->group_edge_count; i++) {
		struct midr_ted_group_edge *edge = &stage->group_edges[i];

		if (edge->source_group_id != source ||
		    edge->target_group_id != target)
			continue;
		if (cost < edge->aggregate_cost)
			edge->aggregate_cost = cost;
		return 0;
	}
	if (stage->group_edge_count == SIZE_MAX)
		return -EOVERFLOW;
	stage->group_edges[stage->group_edge_count++] =
		(struct midr_ted_group_edge){
			.source_group_id = source,
			.target_group_id = target,
			.aggregate_cost = cost,
		};
	return 0;
}

static int derive_formal_view(struct midr_ted *ted, uint32_t local_node_id,
			      const struct midr_lsdb_stage *lsdb_stage,
			      struct midr_ted_stage *stage)
{
	struct midr_lsdb_snapshot lsdb = {0};
	size_t capacity;
	int ret;

	ret = midr_lsdb_stage_snapshot(lsdb_stage, &lsdb);
	if (ret)
		return ret;
	capacity = lsdb.count;
	if (capacity > ted->config.max_events)
		return -ENOSPC;
	if (capacity) {
		stage->nodes = calloc(capacity, sizeof(*stage->nodes));
		stage->intra_links = calloc(capacity,
					    sizeof(*stage->intra_links));
		stage->egress_links = calloc(capacity,
					     sizeof(*stage->egress_links));
		stage->node_prefixes = calloc(capacity,
					      sizeof(*stage->node_prefixes));
		stage->group_edges = calloc(capacity,
					    sizeof(*stage->group_edges));
		stage->prefix_groups = calloc(capacity,
					      sizeof(*stage->prefix_groups));
		if (!stage->nodes || !stage->intra_links || !stage->egress_links ||
		    !stage->node_prefixes || !stage->group_edges ||
		    !stage->prefix_groups)
			return -ENOMEM;
	}
	stage->formal_view = true;
	stage->local_node_id = local_node_id;
	(void)membership_group(&lsdb, local_node_id, &stage->local_group_id);
	for (size_t i = 0; i < lsdb.count; i++) {
		const struct midr_lsdb_entry *entry = &lsdb.entries[i];
		const struct midr_core_object *object = &entry->object;
		uint32_t owner_group;
		uint32_t remote_group;

		if (object->identity.type == MIDR_CORE_MEMBERSHIP) {
			if (object->group == stage->local_group_id)
				stage->nodes[stage->node_count++] =
					(struct midr_ted_node){
						.node_id = object->identity.originator,
						.group_id = object->group,
					};
			continue;
		}
		if (entry->state != MIDR_LSDB_USABLE)
			continue;
		if (object->identity.type == MIDR_CORE_LINK) {
			struct midr_ted_link link = {0};

			if (membership_group(&lsdb, object->identity.originator,
					     &owner_group) ||
			    membership_group(&lsdb, object->identity.remote,
					     &remote_group))
				continue;
			link.local_node_id = object->identity.originator;
			link.remote_node_id = object->identity.remote;
			link.local_group_id = owner_group;
			link.remote_group_id = remote_group;
			link.link_id = object->identity.link_id;
			link.canonical_cost = object->metric;
			if (object->identity.originator == local_node_id &&
			    ted->config.local_ifindex_lookup)
				link.local_ifindex =
					ted->config.local_ifindex_lookup(
						ted->config.local_ifindex_arg,
						local_node_id,
						object->identity.remote,
						object->identity.link_id);
			link.family = object->address_family;
			memcpy(link.local_address, object->local_address,
			       sizeof(link.local_address));
			memcpy(link.remote_address, object->remote_address,
			       sizeof(link.remote_address));
			if (owner_group == stage->local_group_id) {
				if (owner_group == remote_group)
					stage->intra_links[stage->intra_link_count++] =
						link;
				else
					stage->egress_links[stage->egress_link_count++] =
						link;
			}
			if (owner_group != remote_group) {
				ret = add_group_edge(stage, owner_group, remote_group,
						     object->metric);
				if (ret)
					return ret;
			}
		} else if (object->identity.type == MIDR_CORE_NODE_PREFIX) {
			struct midr_ted_node_prefix *prefix =
				&stage->node_prefixes[stage->node_prefix_count++];

			prefix_key_from_object(object, &prefix->key);
			prefix->node_id = object->identity.originator;
		} else if (object->identity.type == MIDR_CORE_GROUP_PREFIX) {
			struct midr_ted_prefix_group *prefix =
				&stage->prefix_groups[stage->prefix_group_count++];

			prefix_key_from_object(object, &prefix->key);
			prefix->group_id = object->identity.group;
		}
	}
	if (stage->node_count > 1U)
		qsort(stage->nodes, stage->node_count, sizeof(*stage->nodes),
		      node_compare);
	if (stage->intra_link_count > 1U)
		qsort(stage->intra_links, stage->intra_link_count,
		      sizeof(*stage->intra_links), link_compare);
	if (stage->egress_link_count > 1U)
		qsort(stage->egress_links, stage->egress_link_count,
		      sizeof(*stage->egress_links), link_compare);
	if (stage->node_prefix_count > 1U)
		qsort(stage->node_prefixes, stage->node_prefix_count,
		      sizeof(*stage->node_prefixes), node_prefix_compare);
	if (stage->group_edge_count > 1U)
		qsort(stage->group_edges, stage->group_edge_count,
		      sizeof(*stage->group_edges), group_edge_compare);
	if (stage->prefix_group_count > 1U)
		qsort(stage->prefix_groups, stage->prefix_group_count,
		      sizeof(*stage->prefix_groups), prefix_group_compare);
	return 0;
}

static int prepare_candidate(const struct midr_ted *ted,
			     const struct midr_consumer_snapshot *snapshot,
			     struct midr_consumer_event **out)
{
	struct midr_consumer_event *candidate = NULL;

	if (!ted || !snapshot || !out || *out || !snapshot->generation ||
	    snapshot->count > ted->config.max_events ||
	    (snapshot->count && !snapshot->events))
		return -EINVAL;
	if (snapshot->count > SIZE_MAX / sizeof(*candidate))
		return -EOVERFLOW;
	if (snapshot->count) {
		candidate = calloc(snapshot->count, sizeof(*candidate));
		if (!candidate)
			return -ENOMEM;
		memcpy(candidate, snapshot->events,
		       snapshot->count * sizeof(*candidate));
	}
	for (size_t i = 0; i < snapshot->count; i++) {
		const struct midr_consumer_event *event = &candidate[i];

		if (event->generation != snapshot->generation ||
		    midr_consumer_event_validate(event) ||
		    event->kind == MIDR_CONSUMER_SNAPSHOT_BEGIN ||
		    event->kind == MIDR_CONSUMER_SNAPSHOT_END) {
			free(candidate);
			return -EINVAL;
		}
	}
	if (snapshot->count > 1U)
		qsort(candidate, snapshot->count, sizeof(*candidate),
		      event_key_compare);
	for (size_t i = 1; i < snapshot->count; i++)
		if (!event_key_compare(&candidate[i - 1], &candidate[i])) {
			free(candidate);
			return -EEXIST;
		}
	*out = candidate;
	return 0;
}

static int finalize_stage(struct midr_ted *ted,
				  const struct midr_consumer_snapshot *snapshot,
				  struct midr_ted_stage *stage)
{
	bool events_equal;
	bool equal;

	if (ted->fail_next) {
		int error = ted->fail_next;

		ted->fail_next = 0;
		return error;
	}
	events_equal = snapshot_semantic_equal(ted, stage->events,
					 snapshot->count);
	equal = events_equal && formal_semantic_equal(ted, stage);
	if (ted->state == MIDR_TED_READY && equal) {
		stage->no_change = true;
		stage->generation = ted->generation;
	} else {
		if (ted->state == MIDR_TED_READY &&
		    snapshot->generation == ted->source_generation &&
		    (!stage->local_metadata_update || !events_equal))
			return -EPROTO;
		if (ted->generation == UINT64_MAX)
			return -ERANGE;
		stage->generation = ted->generation + 1U;
		for (size_t i = 0; i < snapshot->count; i++)
			stage->events[i].generation = stage->generation;
	}
	stage->count = snapshot->count;
	stage->source_generation = snapshot->generation;
	return 0;
}

int midr_ted_create(const struct midr_ted_config *config,
		    struct midr_ted **out)
{
	struct midr_ted *ted;

	if (!config || !out || *out || !config->max_events)
		return -EINVAL;
	ted = calloc(1, sizeof(*ted));
	if (!ted)
		return -ENOMEM;
	ted->config = *config;
	ted->state = MIDR_TED_NOT_READY;
	*out = ted;
	return 0;
}

void midr_ted_destroy(struct midr_ted **tedp)
{
	struct midr_ted_consumer *consumer;

	if (!tedp || !*tedp)
		return;
	while ((consumer = (*tedp)->consumers)) {
		(*tedp)->consumers = consumer->next;
		free(consumer);
	}
	free((*tedp)->events);
	ted_formal_free(*tedp);
	free(*tedp);
	*tedp = NULL;
}

void midr_ted_abort_prepared(struct midr_ted_stage **stagep)
{
	if (!stagep || !*stagep)
		return;
	free((*stagep)->events);
	stage_formal_free(*stagep);
	free(*stagep);
	*stagep = NULL;
}

int midr_ted_prepare_snapshot(struct midr_ted *ted,
			      const struct midr_consumer_snapshot *snapshot,
			      struct midr_ted_stage **stagep)
{
	struct midr_ted_stage *stage;
	int ret;

	if (!ted || !snapshot || !stagep || *stagep)
		return -EINVAL;
	if (snapshot->generation < ted->source_generation)
		return -ESTALE;
	stage = calloc(1, sizeof(*stage));
	if (!stage)
		return -ENOMEM;
	ret = prepare_candidate(ted, snapshot, &stage->events);
	if (ret)
		goto failed;
	ret = finalize_stage(ted, snapshot, stage);
	if (ret)
		goto failed;
	*stagep = stage;
	return 0;

failed:
	midr_ted_abort_prepared(&stage);
	return ret ? ret : -EINVAL;
}

int midr_ted_prepare_lsdb(struct midr_ted *ted, uint32_t local_node_id,
			  const struct midr_lsdb_stage *lsdb_stage,
			  const struct midr_consumer_snapshot *snapshot,
			  bool local_metadata_update,
			  struct midr_ted_stage **stagep)
{
	struct midr_ted_stage *stage;
	int ret;

	if (!ted || !local_node_id || !lsdb_stage || !snapshot || !stagep ||
	    *stagep)
		return -EINVAL;
	if (snapshot->generation < ted->source_generation)
		return -ESTALE;
	stage = calloc(1, sizeof(*stage));
	if (!stage)
		return -ENOMEM;
	ret = prepare_candidate(ted, snapshot, &stage->events);
	if (ret)
		goto failed;
	ret = derive_formal_view(ted, local_node_id, lsdb_stage, stage);
	if (ret)
		goto failed;
	stage->local_metadata_update = local_metadata_update;
	ret = finalize_stage(ted, snapshot, stage);
	if (ret)
		goto failed;
	*stagep = stage;
	return 0;

failed:
	midr_ted_abort_prepared(&stage);
	return ret ? ret : -EINVAL;
}

void midr_ted_commit_prepared(struct midr_ted *ted,
			      struct midr_ted_stage **stagep)
{
	struct midr_ted_stage *stage;
	uint32_t change_flags = MIDR_TED_CHANGE_NONE;
	enum midr_ted_state previous_state;

	if (!ted || !stagep || !*stagep)
		return;
	stage = *stagep;
	previous_state = ted->state;
	if (!stage->no_change)
		change_flags = formal_change_flags(ted, stage);
	if (!stage->no_change) {
		free(ted->events);
		ted_formal_free(ted);
		ted->events = stage->events;
		ted->count = stage->count;
		ted->generation = stage->generation;
		ted->local_node_id = stage->local_node_id;
		ted->local_group_id = stage->local_group_id;
		ted->nodes = stage->nodes;
		ted->node_count = stage->node_count;
		ted->intra_links = stage->intra_links;
		ted->intra_link_count = stage->intra_link_count;
		ted->egress_links = stage->egress_links;
		ted->egress_link_count = stage->egress_link_count;
		ted->node_prefixes = stage->node_prefixes;
		ted->node_prefix_count = stage->node_prefix_count;
		ted->group_edges = stage->group_edges;
		ted->group_edge_count = stage->group_edge_count;
		ted->prefix_groups = stage->prefix_groups;
		ted->prefix_group_count = stage->prefix_group_count;
		ted->formal_view = stage->formal_view;
		stage->events = NULL;
		stage->nodes = NULL;
		stage->intra_links = NULL;
		stage->egress_links = NULL;
		stage->node_prefixes = NULL;
		stage->group_edges = NULL;
		stage->prefix_groups = NULL;
	}
	ted->source_generation = stage->source_generation;
	free(stage->events);
	stage_formal_free(stage);
	ted->state = MIDR_TED_READY;
	ted->last_error = 0;
	if (previous_state != MIDR_TED_READY)
		change_flags |= MIDR_TED_CHANGE_SYNC;
	free(stage);
	*stagep = NULL;
	ted_consumer_notify(ted, ted->generation, change_flags);
}

int midr_ted_apply_snapshot(struct midr_ted *ted,
			    const struct midr_consumer_snapshot *snapshot)
{
	struct midr_ted_stage *stage = NULL;
	int ret;

	if (!ted || !snapshot)
		return -EINVAL;
	if (snapshot->generation < ted->source_generation)
		return -ESTALE;
	ret = midr_ted_prepare_snapshot(ted, snapshot, &stage);
	if (ret)
		goto failed;
	midr_ted_commit_prepared(ted, &stage);
	return 0;

failed:
	midr_ted_abort_prepared(&stage);
	if (!ret)
		ret = -EINVAL;
	(void)midr_ted_invalidate(ted, ret);
	return ret;
}

int midr_ted_invalidate(struct midr_ted *ted, int error)
{
	enum midr_ted_state previous_state;
	uint64_t previous_generation;
	uint32_t change_flags = MIDR_TED_CHANGE_NONE;

	if (!ted || error >= 0)
		return -EINVAL;
	previous_state = ted->state;
	previous_generation = ted->generation;
	if (ted->state == MIDR_TED_READY && ted->generation < UINT64_MAX)
		ted->generation++;
	ted->state = MIDR_TED_NOT_READY;
	ted->last_error = error;
	if (previous_state != MIDR_TED_NOT_READY ||
	    previous_generation != ted->generation)
		change_flags |= MIDR_TED_CHANGE_SYNC;
	ted_consumer_notify(ted, ted->generation, change_flags);
	return 0;
}

int midr_ted_snapshot_acquire(const struct midr_ted *ted,
			      struct midr_consumer_snapshot *snapshot)
{
	if (!ted || !snapshot)
		return -EINVAL;
	if (ted->state != MIDR_TED_READY)
		return -EAGAIN;
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->generation = ted->generation;
	snapshot->count = ted->count;
	if (!snapshot->count)
		return 0;
	snapshot->events = calloc(snapshot->count, sizeof(*snapshot->events));
	if (!snapshot->events)
		return -ENOMEM;
	memcpy(snapshot->events, ted->events,
	       snapshot->count * sizeof(*snapshot->events));
	return 0;
}

void midr_ted_view_release(struct midr_ted_view *view)
{
	if (!view)
		return;
	free(view->nodes);
	free(view->intra_links);
	free(view->egress_links);
	free(view->node_prefixes);
	free(view->group_edges);
	free(view->prefix_groups);
	memset(view, 0, sizeof(*view));
}

bool midr_ted_generation_is_current(const struct midr_ted *ted,
				    uint64_t generation)
{
	return ted && generation && ted->state == MIDR_TED_READY &&
	       ted->generation == generation;
}

int midr_ted_consumer_register(struct midr_ted *ted,
				       const struct midr_ted_consumer_ops *ops,
				       void *arg,
				       struct midr_ted_consumer **consumerp)
{
	struct midr_ted_consumer *consumer;

	if (!ted || !ops || !ops->snapshot_changed || !consumerp || *consumerp)
		return -EINVAL;
	consumer = calloc(1, sizeof(*consumer));
	if (!consumer)
		return -ENOMEM;
	consumer->ted = ted;
	consumer->ops = *ops;
	consumer->arg = arg;
	consumer->next = ted->consumers;
	ted->consumers = consumer;
	*consumerp = consumer;
	return 0;
}

void midr_ted_consumer_unregister(struct midr_ted *ted,
					  struct midr_ted_consumer **consumerp)
{
	struct midr_ted_consumer *consumer;

	if (!ted || !consumerp || !*consumerp)
		return;
	consumer = *consumerp;
	if (consumer->ted != ted)
		return;
	*consumerp = NULL;
	consumer->removed = true;
	if (!ted->notify_depth)
		ted_consumer_prune(ted);
}

static void *copy_array(const void *source, size_t count, size_t element_size,
			int *error)
{
	void *copy;

	if (!count)
		return NULL;
	if (count > SIZE_MAX / element_size) {
		*error = -EOVERFLOW;
		return NULL;
	}
	copy = calloc(count, element_size);
	if (!copy) {
		*error = -ENOMEM;
		return NULL;
	}
	memcpy(copy, source, count * element_size);
	return copy;
}

int midr_ted_view_acquire(const struct midr_ted *ted,
			  struct midr_ted_view *view)
{
	int ret = 0;

	if (!ted || !view)
		return -EINVAL;
	if (ted->state != MIDR_TED_READY)
		return -EAGAIN;
	memset(view, 0, sizeof(*view));
	view->generation = ted->generation;
	view->source_generation = ted->source_generation;
	view->local_node_id = ted->local_node_id;
	view->local_group_id = ted->local_group_id;
	view->node_count = ted->node_count;
	view->intra_link_count = ted->intra_link_count;
	view->egress_link_count = ted->egress_link_count;
	view->node_prefix_count = ted->node_prefix_count;
	view->group_edge_count = ted->group_edge_count;
	view->prefix_group_count = ted->prefix_group_count;
	view->nodes = copy_array(ted->nodes, ted->node_count,
				 sizeof(*ted->nodes), &ret);
	if (ret)
		goto failed;
	view->intra_links = copy_array(ted->intra_links,
				       ted->intra_link_count,
				       sizeof(*ted->intra_links), &ret);
	if (ret)
		goto failed;
	view->egress_links = copy_array(ted->egress_links,
					ted->egress_link_count,
					sizeof(*ted->egress_links), &ret);
	if (ret)
		goto failed;
	view->node_prefixes = copy_array(ted->node_prefixes,
					 ted->node_prefix_count,
					 sizeof(*ted->node_prefixes), &ret);
	if (ret)
		goto failed;
	view->group_edges = copy_array(ted->group_edges,
				       ted->group_edge_count,
				       sizeof(*ted->group_edges), &ret);
	if (ret)
		goto failed;
	view->prefix_groups = copy_array(ted->prefix_groups,
					 ted->prefix_group_count,
					 sizeof(*ted->prefix_groups), &ret);
	if (ret)
		goto failed;
	return 0;

failed:
	midr_ted_view_release(view);
	return ret;
}

enum midr_ted_state midr_ted_state(const struct midr_ted *ted)
{
	return ted ? ted->state : MIDR_TED_NOT_READY;
}

uint64_t midr_ted_generation(const struct midr_ted *ted)
{
	return ted ? ted->generation : 0;
}

uint64_t midr_ted_source_generation(const struct midr_ted *ted)
{
	return ted ? ted->source_generation : 0;
}

int midr_ted_last_error(const struct midr_ted *ted)
{
	return ted ? ted->last_error : -EINVAL;
}

int midr_ted_test_fail_next(struct midr_ted *ted, int error)
{
	if (!ted || error >= 0)
		return -EINVAL;
	ted->fail_next = error;
	return 0;
}
