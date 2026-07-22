// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR shortest path first computation
 *
 * Copyright (C) 2026
 */

#include <zebra.h>

#include "lib/link_state.h"
#include "lib/linklist.h"
#include "lib/memory.h"
#include "lib/typesafe.h"

#include "bgpd/bgp_memory.h"
#include "bgpd/bgp_midr_spf.h"

#define MIDR_SPF_INFINITY UINT64_MAX

DEFINE_MTYPE_STATIC(BGPD, MIDR_SPF_TREE, "MIDR SPF tree");
DEFINE_MTYPE_STATIC(BGPD, MIDR_SPF_STATE, "MIDR SPF vertex state");
DEFINE_MTYPE_STATIC(BGPD, MIDR_SPF_PATH, "MIDR SPF path");

PREDECL_HEAP(midr_spf_queue);
PREDECL_RBTREE_UNIQ(midr_spf_states);

struct midr_spf_state {
	struct midr_spf_queue_item queue_item;
	struct midr_spf_states_item state_item;

	const struct ls_vertex *vertex;
	struct midr_spf_vertex_result result;
	struct list *predecessors;
	struct list *first_hops;

	bool queued;
	bool settled;
};

struct midr_spf_tree {
	const struct ls_ted *ted;
	const struct ls_vertex *source;
	struct midr_spf_states_head states;
	struct midr_spf_queue_head queue;
	struct midr_spf_ops ops;
	void *ops_arg;
};

struct midr_spf_path {
	uint64_t distance;
	struct list *edges;
};

static int midr_spf_queue_cmp(const struct midr_spf_state *a,
			      const struct midr_spf_state *b)
{
	if (a->result.distance != b->result.distance)
		return numcmp(a->result.distance, b->result.distance);

	return numcmp(a->result.vertex_key, b->result.vertex_key);
}

static int midr_spf_state_cmp(const struct midr_spf_state *a,
			      const struct midr_spf_state *b)
{
	return numcmp(a->result.vertex_key, b->result.vertex_key);
}

DECLARE_HEAP(midr_spf_queue, struct midr_spf_state, queue_item,
	     midr_spf_queue_cmp);
DECLARE_RBTREE_UNIQ(midr_spf_states, struct midr_spf_state, state_item,
		    midr_spf_state_cmp);

static int midr_spf_edge_cmp(void *a_data, void *b_data)
{
	const struct ls_edge *a = a_data;
	const struct ls_edge *b = b_data;
	uint64_t a_source = a && a->source ? a->source->key : 0;
	uint64_t b_source = b && b->source ? b->source->key : 0;
	uint64_t a_destination = a && a->destination ? a->destination->key : 0;
	uint64_t b_destination = b && b->destination ? b->destination->key : 0;

	if (a_source != b_source)
		return numcmp(a_source, b_source);
	if (a_destination != b_destination)
		return numcmp(a_destination, b_destination);
	if (!a || !b)
		return numcmp((uintptr_t)a, (uintptr_t)b);

	return edge_cmp(a, b);
}

static struct midr_spf_state *
midr_spf_state_new(const struct ls_vertex *vertex)
{
	struct midr_spf_state *state;

	state = XCALLOC(MTYPE_MIDR_SPF_STATE, sizeof(*state));
	state->vertex = vertex;
	state->result.vertex_key = vertex->key;
	state->result.distance = MIDR_SPF_INFINITY;
	state->predecessors = list_new();
	state->predecessors->cmp = midr_spf_edge_cmp;
	state->first_hops = list_new();
	state->first_hops->cmp = midr_spf_edge_cmp;
	state->result.first_hops = state->first_hops;

	return state;
}

static void midr_spf_state_free(struct midr_spf_state *state)
{
	if (!state)
		return;

	list_delete(&state->predecessors);
	list_delete(&state->first_hops);
	XFREE(MTYPE_MIDR_SPF_STATE, state);
}

static struct midr_spf_state *
midr_spf_state_lookup(struct midr_spf_tree *tree, uint64_t vertex_key)
{
	struct midr_spf_state key = {
		.result.vertex_key = vertex_key,
	};

	return midr_spf_states_find(&tree->states, &key);
}

static const struct midr_spf_state *
midr_spf_state_lookup_const(const struct midr_spf_tree *tree,
			    uint64_t vertex_key)
{
	struct midr_spf_state key = {
		.result.vertex_key = vertex_key,
	};

	return midr_spf_states_const_find(&tree->states, &key);
}

static struct midr_spf_tree *
midr_spf_tree_new(const struct ls_ted *ted, const struct ls_vertex *source,
		  const struct midr_spf_ops *ops, void *ops_arg)
{
	const struct ls_vertex *vertex;
	struct midr_spf_state *state;
	struct midr_spf_tree *tree;

	tree = XCALLOC(MTYPE_MIDR_SPF_TREE, sizeof(*tree));
	tree->ted = ted;
	tree->source = source;
	tree->ops_arg = ops_arg;
	if (ops)
		tree->ops = *ops;

	midr_spf_states_init(&tree->states);
	midr_spf_queue_init(&tree->queue);

	frr_each (vertices_const, &ted->vertices, vertex) {
		state = midr_spf_state_new(vertex);
		midr_spf_states_add(&tree->states, state);
	}

	return tree;
}

void midr_spf_tree_free(struct midr_spf_tree *tree)
{
	struct midr_spf_state *state;

	if (!tree)
		return;

	while ((state = midr_spf_queue_pop(&tree->queue)) != NULL)
		state->queued = false;
	midr_spf_queue_fini(&tree->queue);

	frr_each_safe (midr_spf_states, &tree->states, state) {
		midr_spf_states_del(&tree->states, state);
		midr_spf_state_free(state);
	}
	midr_spf_states_fini(&tree->states);

	XFREE(MTYPE_MIDR_SPF_TREE, tree);
}

static bool midr_spf_vertex_allowed(const struct midr_spf_tree *tree,
				    const struct ls_vertex *vertex)
{
	if (!vertex || vertex->key == 0)
		return false;
	if (tree->ops.vertex_allowed)
		return tree->ops.vertex_allowed(vertex, tree->ops_arg);

	return true;
}

static bool midr_spf_edge_allowed(const struct midr_spf_tree *tree,
				  const struct ls_edge *edge)
{
	if (!edge || !edge->source || !edge->destination || !edge->attributes)
		return false;
	if (edge->status == DELETE || edge->status == ORPHAN)
		return false;
	if (tree->ops.edge_allowed &&
	    !tree->ops.edge_allowed(edge, tree->ops_arg))
		return false;

	return true;
}

static int midr_spf_edge_cost(const struct midr_spf_tree *tree,
			      const struct ls_edge *edge, uint64_t *cost)
{
	const struct ls_attributes *attributes = edge->attributes;

	if (!cost)
		return -EINVAL;
	if (tree->ops.edge_cost)
		return tree->ops.edge_cost(edge, cost, tree->ops_arg);

	if (CHECK_FLAG(attributes->flags, LS_ATTR_TE_METRIC)) {
		*cost = attributes->standard.te_metric;
		return 0;
	}
	if (CHECK_FLAG(attributes->flags, LS_ATTR_METRIC)) {
		*cost = attributes->metric;
		return 0;
	}

	return -ENOENT;
}

static int midr_spf_queue_update(struct midr_spf_tree *tree,
				 struct midr_spf_state *state,
				 uint64_t distance)
{
	if (state->settled)
		return 0;
	if (state->queued)
		midr_spf_queue_del(&tree->queue, state);

	state->result.distance = distance;
	state->result.reachable = true;
	midr_spf_queue_add(&tree->queue, state);
	state->queued = true;

	return 0;
}

static void midr_spf_relax(struct midr_spf_tree *tree,
			   struct midr_spf_state *current,
			   const struct ls_edge *edge)
{
	struct midr_spf_state *next;
	uint64_t candidate;
	uint64_t cost;

	if (!midr_spf_edge_allowed(tree, edge))
		return;
	if (edge->source != current->vertex)
		return;
	if (!midr_spf_vertex_allowed(tree, edge->destination))
		return;
	if (midr_spf_edge_cost(tree, edge, &cost) != 0)
		return;
	if (current->result.distance > UINT64_MAX - cost)
		return;

	next = midr_spf_state_lookup(tree, edge->destination->key);
	if (!next)
		return;

	candidate = current->result.distance + cost;
	if (!next->result.reachable || candidate < next->result.distance)
		midr_spf_queue_update(tree, next, candidate);
}

static int midr_spf_run(struct midr_spf_tree *tree)
{
	struct midr_spf_state *current;
	struct listnode *node;
	struct ls_edge *edge;

	while ((current = midr_spf_queue_pop(&tree->queue)) != NULL) {
		current->queued = false;
		if (current->settled)
			continue;
		current->settled = true;

		for (ALL_LIST_ELEMENTS_RO(current->vertex->outgoing_edges, node,
					  edge))
			midr_spf_relax(tree, current, edge);
	}

	return 0;
}

static void midr_spf_paths_clear(struct midr_spf_tree *tree)
{
	struct midr_spf_state *state;

	frr_each (midr_spf_states, &tree->states, state) {
		list_delete_all_node(state->predecessors);
		list_delete_all_node(state->first_hops);
	}
}

static void midr_spf_predecessors_build(struct midr_spf_tree *tree)
{
	struct midr_spf_state *state;
	struct midr_spf_state *next;
	struct midr_spf_state *source;
	struct listnode *node;
	struct ls_edge *edge;
	uint64_t cost;

	midr_spf_paths_clear(tree);
	source = midr_spf_state_lookup(tree, tree->source->key);

	frr_each (midr_spf_states, &tree->states, state) {
		if (!state->result.reachable)
			continue;

		for (ALL_LIST_ELEMENTS_RO(state->vertex->outgoing_edges, node,
					  edge)) {
			if (!midr_spf_edge_allowed(tree, edge))
				continue;
			if (edge->source != state->vertex)
				continue;
			if (!midr_spf_vertex_allowed(tree, edge->destination))
				continue;
			if (midr_spf_edge_cost(tree, edge, &cost) != 0)
				continue;
			if (state->result.distance > UINT64_MAX - cost)
				continue;

			next = midr_spf_state_lookup(tree, edge->destination->key);
			if (!next || !next->result.reachable || next == source)
				continue;
			if (state->result.distance + cost != next->result.distance)
				continue;

			listnode_add_sort_nodup(next->predecessors, edge);
		}
	}
}

static bool midr_spf_first_hops_merge(struct midr_spf_state *destination,
				      const struct midr_spf_state *source)
{
	struct listnode *node;
	struct ls_edge *edge;
	bool changed = false;

	for (ALL_LIST_ELEMENTS_RO(source->result.first_hops, node, edge))
		changed |= listnode_add_sort_nodup(destination->first_hops, edge);

	return changed;
}

static void midr_spf_first_hops_build(struct midr_spf_tree *tree)
{
	struct midr_spf_state *state;
	struct midr_spf_state *predecessor;
	struct midr_spf_state *source;
	struct listnode *node;
	struct ls_edge *edge;
	bool changed;

	source = midr_spf_state_lookup(tree, tree->source->key);
	do {
		changed = false;
		frr_each (midr_spf_states, &tree->states, state) {
			if (!state->result.reachable || state == source)
				continue;

			for (ALL_LIST_ELEMENTS_RO(state->predecessors, node,
						  edge)) {
				predecessor = midr_spf_state_lookup(
					tree, edge->source->key);
				if (!predecessor)
					continue;

				if (predecessor == source)
					changed |= listnode_add_sort_nodup(
						state->first_hops, edge);
				else
					changed |= midr_spf_first_hops_merge(
						state, predecessor);
			}
		}
	} while (changed);
}

int midr_dijkstra(const struct ls_ted *ted, const struct ls_vertex *source,
		  const struct midr_spf_ops *ops, void *ops_arg,
		  struct midr_spf_tree **result)
{
	struct midr_spf_state *source_state;
	struct midr_spf_tree *tree;

	if (!result)
		return -EINVAL;
	*result = NULL;

	if (!ted || !source || source->key == 0)
		return -EINVAL;

	tree = midr_spf_tree_new(ted, source, ops, ops_arg);
	source_state = midr_spf_state_lookup(tree, source->key);
	if (!source_state) {
		midr_spf_tree_free(tree);
		return -ENOENT;
	}
	tree->source = source_state->vertex;
	if (!midr_spf_vertex_allowed(tree, source_state->vertex)) {
		midr_spf_tree_free(tree);
		return -EACCES;
	}

	midr_spf_queue_update(tree, source_state, 0);
	midr_spf_run(tree);
	midr_spf_predecessors_build(tree);
	midr_spf_first_hops_build(tree);

	*result = tree;
	return 0;
}

const struct midr_spf_vertex_result *
midr_spf_tree_lookup(const struct midr_spf_tree *tree,
		     uint64_t destination_key)
{
	const struct midr_spf_state *state;

	if (!tree || destination_key == 0)
		return NULL;

	state = midr_spf_state_lookup_const(tree, destination_key);
	return state ? &state->result : NULL;
}

static int midr_spf_path_build_state(const struct midr_spf_tree *tree,
				     const struct midr_spf_state *state,
				     struct midr_spf_path *path,
				     struct list *visiting)
{
	const struct midr_spf_state *predecessor;
	struct listnode *node;
	struct ls_edge *edge;

	if (state->result.vertex_key == tree->source->key)
		return 0;
	if (listnode_lookup(visiting, state))
		return -ELOOP;

	listnode_add(visiting, (void *)state);
	for (ALL_LIST_ELEMENTS_RO(state->predecessors, node, edge)) {
		predecessor = midr_spf_state_lookup_const(tree,
							  edge->source->key);
		if (!predecessor)
			continue;
		if (midr_spf_path_build_state(tree, predecessor, path,
						  visiting) != 0)
			continue;

		listnode_add(path->edges, edge);
		listnode_delete(visiting, state);
		return 0;
	}

	listnode_delete(visiting, state);
	return -ENOENT;
}

int midr_spf_path_build(const struct midr_spf_tree *tree,
			uint64_t destination_key,
			struct midr_spf_path **result)
{
	const struct midr_spf_state *state;
	struct midr_spf_path *path;
	struct list *visiting;
	int ret;

	if (!result)
		return -EINVAL;
	*result = NULL;
	if (!tree || destination_key == 0)
		return -EINVAL;

	state = midr_spf_state_lookup_const(tree, destination_key);
	if (!state || !state->result.reachable)
		return -ENOENT;

	path = XCALLOC(MTYPE_MIDR_SPF_PATH, sizeof(*path));
	path->distance = state->result.distance;
	path->edges = list_new();
	visiting = list_new();

	ret = midr_spf_path_build_state(tree, state, path, visiting);
	list_delete(&visiting);
	if (ret != 0) {
		midr_spf_path_free(path);
		return ret;
	}

	*result = path;
	return 0;
}

uint64_t midr_spf_path_distance(const struct midr_spf_path *path)
{
	return path ? path->distance : MIDR_SPF_INFINITY;
}

const struct list *midr_spf_path_edges(const struct midr_spf_path *path)
{
	return path ? path->edges : NULL;
}

void midr_spf_path_free(struct midr_spf_path *path)
{
	if (!path)
		return;

	list_delete(&path->edges);
	XFREE(MTYPE_MIDR_SPF_PATH, path);
}
