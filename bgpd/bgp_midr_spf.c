// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR hierarchical shortest path computation
 *
 * Copyright (C) 2026
 */

#include <zebra.h>

#include <errno.h>

#include "lib/linklist.h"
#include "lib/memory.h"
#include "lib/typesafe.h"

#include "bgpd/bgp_memory.h"
#include "bgpd/bgp_midr_spf.h"

#define MIDR_SPF_INFINITY UINT64_MAX

DEFINE_MTYPE_STATIC(BGPD, MIDR_SPF_GRAPH, "MIDR SPF graph");
DEFINE_MTYPE_STATIC(BGPD, MIDR_SPF_VERTICES, "MIDR SPF vertices");
DEFINE_MTYPE_STATIC(BGPD, MIDR_SPF_EDGES, "MIDR SPF edges");
DEFINE_MTYPE_STATIC(BGPD, MIDR_SPF_ROUTE, "MIDR SPF route");
DEFINE_MTYPE_STATIC(BGPD, MIDR_SPF_NEXTHOPS, "MIDR SPF nexthops");
DEFINE_MTYPE_STATIC(BGPD, MIDR_SPF_RESULTS, "MIDR SPF results");
DEFINE_MTYPE_STATIC(BGPD, MIDR_SPF_PREFIXES, "MIDR SPF prefixes");

enum midr_spf_graph_kind {
	MIDR_SPF_GRAPH_ROUTER,
	MIDR_SPF_GRAPH_GROUP,
};

enum midr_spf_edge_kind {
	MIDR_SPF_EDGE_LINK,
	MIDR_SPF_EDGE_GROUP,
};

PREDECL_HEAP(midr_spf_queue);

struct midr_spf_graph_edge {
	uint32_t source_id;
	uint32_t target_id;
	uint32_t source_order;
	uint32_t target_order;
	uint64_t stable_id;
	uint64_t cost;
	enum midr_spf_edge_kind kind;
	union {
		const struct midr_ted_link *link;
		const struct midr_ted_group_edge *group;
	} payload;
};

struct midr_spf_vertex {
	struct midr_spf_queue_item queue_item;
	uint32_t id;
	uint32_t order;
	uint64_t distance;
	size_t first_edge;
	size_t edge_count;
	struct list *predecessors;
	struct list *first_hops;
	bool reachable;
	bool queued;
	bool settled;
};

struct midr_spf_graph {
	enum midr_spf_graph_kind kind;
	const struct midr_ted_snapshot *snapshot;
	struct midr_spf_vertex *vertices;
	size_t vertex_count;
	struct midr_spf_graph_edge *edges;
	size_t edge_count;
	struct midr_spf_vertex *source;
	struct midr_spf_queue_head queue;
};

struct midr_spf_results {
	uint32_t refcount;
	uint64_t generation;
	uint64_t sync_reason_flags;
	struct midr_spf_route *routes;
	size_t route_count;
};

static int midr_spf_u32_cmp(uint32_t a, uint32_t b)
{
	return (a > b) - (a < b);
}

static int midr_spf_u64_cmp(uint64_t a, uint64_t b)
{
	return (a > b) - (a < b);
}

static uint32_t midr_spf_node_order(uint32_t node_id)
{
	return ntohl(node_id);
}

static bool midr_spf_add_cost(uint64_t left, uint64_t right, uint64_t *result)
{
	if (left > UINT64_MAX - right)
		return false;

	*result = left + right;
	return true;
}

static int midr_spf_queue_cmp(const struct midr_spf_vertex *a, const struct midr_spf_vertex *b)
{
	int ret;

	ret = midr_spf_u64_cmp(a->distance, b->distance);
	if (ret)
		return ret;
	ret = midr_spf_u32_cmp(a->order, b->order);
	if (ret)
		return ret;
	return midr_spf_u32_cmp(a->id, b->id);
}

DECLARE_HEAP(midr_spf_queue, struct midr_spf_vertex, queue_item, midr_spf_queue_cmp);

static int midr_spf_vertex_cmp(const void *data1, const void *data2)
{
	const struct midr_spf_vertex *a = data1;
	const struct midr_spf_vertex *b = data2;
	int ret;

	ret = midr_spf_u32_cmp(a->order, b->order);
	if (ret)
		return ret;
	return midr_spf_u32_cmp(a->id, b->id);
}

static int midr_spf_edge_cmp_data(const struct midr_spf_graph_edge *a,
				  const struct midr_spf_graph_edge *b)
{
	int ret;

	ret = midr_spf_u32_cmp(a->source_order, b->source_order);
	if (ret)
		return ret;
	ret = midr_spf_u32_cmp(a->source_id, b->source_id);
	if (ret)
		return ret;
	ret = midr_spf_u32_cmp(a->target_order, b->target_order);
	if (ret)
		return ret;
	ret = midr_spf_u32_cmp(a->target_id, b->target_id);
	if (ret)
		return ret;
	ret = midr_spf_u64_cmp(a->stable_id, b->stable_id);
	if (ret)
		return ret;
	return midr_spf_u64_cmp(a->cost, b->cost);
}

static int midr_spf_edge_cmp(const void *data1, const void *data2)
{
	return midr_spf_edge_cmp_data(data1, data2);
}

static int midr_spf_edge_list_cmp(void *data1, void *data2)
{
	return midr_spf_edge_cmp_data(data1, data2);
}

static struct midr_spf_vertex *midr_spf_vertex_lookup(struct midr_spf_graph *graph, uint32_t id)
{
	uint32_t order = graph->kind == MIDR_SPF_GRAPH_ROUTER ? midr_spf_node_order(id) : id;
	size_t left = 0;
	size_t right = graph->vertex_count;

	while (left < right) {
		size_t middle = left + (right - left) / 2;
		struct midr_spf_vertex *vertex = &graph->vertices[middle];

		if (vertex->order < order || (vertex->order == order && vertex->id < id))
			left = middle + 1;
		else
			right = middle;
	}

	if (left == graph->vertex_count || graph->vertices[left].id != id)
		return NULL;
	return &graph->vertices[left];
}

static const struct midr_spf_vertex *
midr_spf_vertex_lookup_const(const struct midr_spf_graph *graph, uint32_t id)
{
	return midr_spf_vertex_lookup((struct midr_spf_graph *)graph, id);
}

static void midr_spf_graph_free(struct midr_spf_graph **graphp)
{
	struct midr_spf_graph *graph;
	struct midr_spf_vertex *vertex;
	size_t i;

	if (!graphp || !*graphp)
		return;
	graph = *graphp;
	*graphp = NULL;

	while ((vertex = midr_spf_queue_pop(&graph->queue)) != NULL)
		vertex->queued = false;
	midr_spf_queue_fini(&graph->queue);

	for (i = 0; i < graph->vertex_count; i++) {
		list_delete(&graph->vertices[i].predecessors);
		list_delete(&graph->vertices[i].first_hops);
	}
	XFREE(MTYPE_MIDR_SPF_VERTICES, graph->vertices);
	XFREE(MTYPE_MIDR_SPF_EDGES, graph->edges);
	XFREE(MTYPE_MIDR_SPF_GRAPH, graph);
}

static int midr_spf_graph_vertices_init(struct midr_spf_graph *graph, const uint32_t *ids,
					size_t count)
{
	size_t i;
	size_t unique;

	if (!count)
		return -ENOENT;
	graph->vertices = XCALLOC(MTYPE_MIDR_SPF_VERTICES, count * sizeof(*graph->vertices));
	graph->vertex_count = count;

	for (i = 0; i < count; i++) {
		graph->vertices[i].id = ids[i];
		graph->vertices[i].order = graph->kind == MIDR_SPF_GRAPH_ROUTER
						   ? midr_spf_node_order(ids[i])
						   : ids[i];
	}
	qsort(graph->vertices, count, sizeof(*graph->vertices), midr_spf_vertex_cmp);

	unique = 0;
	for (i = 0; i < count; i++) {
		if (unique && graph->vertices[unique - 1].id == graph->vertices[i].id)
			continue;
		graph->vertices[unique++] = graph->vertices[i];
	}
	graph->vertex_count = unique;

	for (i = 0; i < unique; i++) {
		struct midr_spf_vertex *vertex = &graph->vertices[i];

		vertex->distance = MIDR_SPF_INFINITY;
		vertex->predecessors = list_new();
		vertex->predecessors->cmp = midr_spf_edge_list_cmp;
		vertex->first_hops = list_new();
		vertex->first_hops->cmp = midr_spf_edge_list_cmp;
	}

	return 0;
}

static void midr_spf_graph_adjacency_build(struct midr_spf_graph *graph)
{
	struct midr_spf_vertex *source;
	size_t i;

	qsort(graph->edges, graph->edge_count, sizeof(*graph->edges), midr_spf_edge_cmp);

	for (i = 0; i < graph->edge_count; i++) {
		source = midr_spf_vertex_lookup(graph, graph->edges[i].source_id);
		if (!source)
			continue;
		if (!source->edge_count)
			source->first_edge = i;
		source->edge_count++;
	}
}

static bool midr_spf_router_link_valid(const struct midr_ted_snapshot *snapshot,
				       const struct midr_ted_link *link)
{
	return link && link->local_node_id && link->remote_node_id &&
	       link->local_node_id != link->remote_node_id &&
	       link->local_group_id == snapshot->local_group_id &&
	       link->remote_group_id == snapshot->local_group_id && link->canonical_cost > 0 &&
	       link->canonical_cost <= MIDR_TED_LINK_COST_MAX;
}

static int midr_spf_router_graph_build(const struct midr_ted_snapshot *snapshot,
				       struct midr_spf_graph **out)
{
	struct midr_spf_graph *graph;
	uint32_t *ids;
	size_t i;
	int ret;

	if (!snapshot->node_count)
		return -ENOENT;

	graph = XCALLOC(MTYPE_MIDR_SPF_GRAPH, sizeof(*graph));
	graph->kind = MIDR_SPF_GRAPH_ROUTER;
	graph->snapshot = snapshot;
	midr_spf_queue_init(&graph->queue);

	ids = XCALLOC(MTYPE_MIDR_SPF_VERTICES, snapshot->node_count * sizeof(*ids));
	for (i = 0; i < snapshot->node_count; i++)
		ids[i] = snapshot->nodes[i].node_id;
	ret = midr_spf_graph_vertices_init(graph, ids, snapshot->node_count);
	XFREE(MTYPE_MIDR_SPF_VERTICES, ids);
	if (ret)
		goto fail;

	graph->edges = XCALLOC(MTYPE_MIDR_SPF_EDGES,
			       snapshot->intra_link_count * sizeof(*graph->edges));
	for (i = 0; i < snapshot->intra_link_count; i++) {
		const struct midr_ted_link *link = &snapshot->intra_links[i];
		struct midr_spf_graph_edge *edge;

		if (!midr_spf_router_link_valid(snapshot, link) ||
		    !midr_spf_vertex_lookup(graph, link->local_node_id) ||
		    !midr_spf_vertex_lookup(graph, link->remote_node_id))
			continue;

		edge = &graph->edges[graph->edge_count++];
		edge->source_id = link->local_node_id;
		edge->target_id = link->remote_node_id;
		edge->source_order = midr_spf_node_order(link->local_node_id);
		edge->target_order = midr_spf_node_order(link->remote_node_id);
		edge->stable_id = link->link_id;
		edge->cost = link->canonical_cost;
		edge->kind = MIDR_SPF_EDGE_LINK;
		edge->payload.link = link;
	}
	midr_spf_graph_adjacency_build(graph);

	*out = graph;
	return 0;

fail:
	midr_spf_graph_free(&graph);
	return ret;
}

static bool midr_spf_group_edge_valid(const struct midr_ted_group_edge *edge)
{
	return edge && edge->source_group_id && edge->target_group_id &&
	       edge->source_group_id != edge->target_group_id && edge->aggregate_cost > 0;
}

static bool midr_spf_cross_candidate_valid(const struct midr_ted_snapshot *snapshot,
					   const struct midr_ted_link *egress,
					   uint32_t next_group_id)
{
	return egress && egress->local_node_id && egress->remote_node_id &&
	       egress->local_group_id == snapshot->local_group_id &&
	       egress->remote_group_id == next_group_id && egress->canonical_cost > 0 &&
	       egress->canonical_cost <= MIDR_TED_LINK_COST_MAX;
}

static bool midr_spf_local_group_edge_executable(const struct midr_ted_snapshot *snapshot,
						 const struct midr_spf_graph *router,
						 uint32_t next_group_id)
{
	const struct midr_spf_vertex *border;
	const struct midr_ted_link *egress;
	size_t i;

	for (i = 0; i < snapshot->egress_link_count; i++) {
		egress = &snapshot->egress_links[i];
		if (!midr_spf_cross_candidate_valid(snapshot, egress, next_group_id))
			continue;
		border = midr_spf_vertex_lookup_const(router, egress->local_node_id);
		if (!border || !border->reachable)
			continue;
		if (border != router->source && listcount(border->first_hops) == 0)
			continue;
		return true;
	}
	return false;
}

static int midr_spf_group_graph_build(const struct midr_ted_snapshot *snapshot,
				      const struct midr_spf_graph *router,
				      struct midr_spf_graph **out)
{
	struct midr_spf_graph *graph;
	uint32_t *ids;
	size_t capacity;
	size_t count = 0;
	size_t i;
	int ret;

	if (snapshot->group_edge_count > (SIZE_MAX - 1 - snapshot->prefix_group_count) / 2)
		return -EOVERFLOW;
	capacity = 1 + snapshot->group_edge_count * 2 + snapshot->prefix_group_count;

	graph = XCALLOC(MTYPE_MIDR_SPF_GRAPH, sizeof(*graph));
	graph->kind = MIDR_SPF_GRAPH_GROUP;
	graph->snapshot = snapshot;
	midr_spf_queue_init(&graph->queue);

	ids = XCALLOC(MTYPE_MIDR_SPF_VERTICES, capacity * sizeof(*ids));
	ids[count++] = snapshot->local_group_id;
	for (i = 0; i < snapshot->group_edge_count; i++) {
		ids[count++] = snapshot->group_edges[i].source_group_id;
		ids[count++] = snapshot->group_edges[i].target_group_id;
	}
	for (i = 0; i < snapshot->prefix_group_count; i++)
		ids[count++] = snapshot->prefix_groups[i].group_id;

	ret = midr_spf_graph_vertices_init(graph, ids, count);
	XFREE(MTYPE_MIDR_SPF_VERTICES, ids);
	if (ret)
		goto fail;

	graph->edges = XCALLOC(MTYPE_MIDR_SPF_EDGES,
			       snapshot->group_edge_count * sizeof(*graph->edges));
	for (i = 0; i < snapshot->group_edge_count; i++) {
		const struct midr_ted_group_edge *group = &snapshot->group_edges[i];
		struct midr_spf_graph_edge *edge;

		if (!midr_spf_group_edge_valid(group) ||
		    !midr_spf_vertex_lookup(graph, group->source_group_id) ||
		    !midr_spf_vertex_lookup(graph, group->target_group_id))
			continue;
		if (group->source_group_id == snapshot->local_group_id &&
		    !midr_spf_local_group_edge_executable(snapshot, router, group->target_group_id))
			continue;

		edge = &graph->edges[graph->edge_count++];
		edge->source_id = group->source_group_id;
		edge->target_id = group->target_group_id;
		edge->source_order = group->source_group_id;
		edge->target_order = group->target_group_id;
		edge->cost = group->aggregate_cost;
		edge->kind = MIDR_SPF_EDGE_GROUP;
		edge->payload.group = group;
	}
	midr_spf_graph_adjacency_build(graph);

	*out = graph;
	return 0;

fail:
	midr_spf_graph_free(&graph);
	return ret;
}

static void midr_spf_queue_update(struct midr_spf_graph *graph, struct midr_spf_vertex *vertex,
				  uint64_t distance)
{
	if (vertex->settled)
		return;
	if (vertex->queued)
		midr_spf_queue_del(&graph->queue, vertex);

	vertex->distance = distance;
	vertex->reachable = true;
	midr_spf_queue_add(&graph->queue, vertex);
	vertex->queued = true;
}

static void midr_spf_predecessors_build(struct midr_spf_graph *graph)
{
	size_t i;

	for (i = 0; i < graph->edge_count; i++) {
		struct midr_spf_graph_edge *edge = &graph->edges[i];
		struct midr_spf_vertex *source = midr_spf_vertex_lookup(graph, edge->source_id);
		struct midr_spf_vertex *target = midr_spf_vertex_lookup(graph, edge->target_id);
		uint64_t candidate;

		if (!source || !target || !source->reachable || !target->reachable ||
		    target == graph->source)
			continue;
		if (!midr_spf_add_cost(source->distance, edge->cost, &candidate))
			continue;
		if (candidate == target->distance)
			listnode_add_sort_nodup(target->predecessors, edge);
	}
}

static bool midr_spf_first_hops_merge(struct midr_spf_vertex *destination,
				      const struct midr_spf_vertex *source)
{
	struct midr_spf_graph_edge *edge;
	struct listnode *node;
	bool changed = false;

	for (ALL_LIST_ELEMENTS_RO(source->first_hops, node, edge))
		changed |= listnode_add_sort_nodup(destination->first_hops, edge);
	return changed;
}

static void midr_spf_first_hops_build(struct midr_spf_graph *graph)
{
	struct midr_spf_graph_edge *edge;
	struct midr_spf_vertex *predecessor;
	struct midr_spf_vertex *vertex;
	struct listnode *node;
	size_t i;
	bool changed;

	do {
		changed = false;
		for (i = 0; i < graph->vertex_count; i++) {
			vertex = &graph->vertices[i];
			if (!vertex->reachable || vertex == graph->source)
				continue;

			for (ALL_LIST_ELEMENTS_RO(vertex->predecessors, node, edge)) {
				predecessor = midr_spf_vertex_lookup(graph, edge->source_id);
				if (!predecessor)
					continue;
				if (predecessor == graph->source)
					changed |= listnode_add_sort_nodup(vertex->first_hops,
									   edge);
				else
					changed |= midr_spf_first_hops_merge(vertex, predecessor);
			}
		}
	} while (changed);
}

static int midr_spf_graph_run(struct midr_spf_graph *graph, uint32_t source_id)
{
	struct midr_spf_vertex *current;
	size_t i;

	graph->source = midr_spf_vertex_lookup(graph, source_id);
	if (!graph->source)
		return -ENOENT;

	midr_spf_queue_update(graph, graph->source, 0);
	while ((current = midr_spf_queue_pop(&graph->queue)) != NULL) {
		current->queued = false;
		if (current->settled)
			continue;
		current->settled = true;

		for (i = current->first_edge; i < current->first_edge + current->edge_count; i++) {
			struct midr_spf_graph_edge *edge = &graph->edges[i];
			struct midr_spf_vertex *target;
			uint64_t candidate;

			if (edge->source_id != current->id)
				continue;
			target = midr_spf_vertex_lookup(graph, edge->target_id);
			if (!target ||
			    !midr_spf_add_cost(current->distance, edge->cost, &candidate))
				continue;
			if (!target->reachable || candidate < target->distance)
				midr_spf_queue_update(graph, target, candidate);
		}
	}

	midr_spf_predecessors_build(graph);
	midr_spf_first_hops_build(graph);
	return 0;
}

static bool midr_spf_prefix_key_valid(const struct midr_ted_prefix_key *key)
{
	if (!key || key->safi != SAFI_UNICAST)
		return false;
	if (key->afi == AFI_IP)
		return key->prefix.family == AF_INET && key->prefix.prefixlen <= IPV4_MAX_BITLEN;
	if (key->afi == AFI_IP6)
		return key->prefix.family == AF_INET6 && key->prefix.prefixlen <= IPV6_MAX_BITLEN;
	return false;
}

static int midr_spf_prefix_key_normalize(const struct midr_ted_prefix_key *input,
					 struct midr_ted_prefix_key *output)
{
	if (!output || !midr_spf_prefix_key_valid(input))
		return -EINVAL;
	*output = *input;
	apply_mask(&output->prefix);
	return 0;
}

static bool midr_spf_prefix_key_same(const struct midr_ted_prefix_key *a,
				     const struct midr_ted_prefix_key *b)
{
	return a->afi == b->afi && a->safi == b->safi && prefix_same(&a->prefix, &b->prefix);
}

static int midr_spf_prefix_key_cmp_data(const struct midr_ted_prefix_key *a,
					const struct midr_ted_prefix_key *b)
{
	int ret;

	ret = midr_spf_u32_cmp(a->afi, b->afi);
	if (ret)
		return ret;
	ret = midr_spf_u32_cmp(a->safi, b->safi);
	if (ret)
		return ret;
	return prefix_cmp(&a->prefix, &b->prefix);
}

static int midr_spf_prefix_key_cmp(const void *data1, const void *data2)
{
	return midr_spf_prefix_key_cmp_data(data1, data2);
}

static int midr_spf_nexthop_cmp(const void *data1, const void *data2)
{
	const struct midr_spf_nexthop *a = data1;
	const struct midr_spf_nexthop *b = data2;
	int ret;

	ret = ipaddr_cmp(&a->address, &b->address);
	if (ret)
		return ret;
	ret = midr_spf_u32_cmp(a->ifindex, b->ifindex);
	if (ret)
		return ret;
	ret = midr_spf_u32_cmp(midr_spf_node_order(a->local_node_id),
			       midr_spf_node_order(b->local_node_id));
	if (ret)
		return ret;
	ret = midr_spf_u32_cmp(midr_spf_node_order(a->remote_node_id),
			       midr_spf_node_order(b->remote_node_id));
	if (ret)
		return ret;
	ret = midr_spf_u64_cmp(a->link_id, b->link_id);
	if (ret)
		return ret;
	ret = midr_spf_u32_cmp(a->destination_group_id, b->destination_group_id);
	if (ret)
		return ret;
	return midr_spf_u32_cmp(a->next_group_id, b->next_group_id);
}

static bool midr_spf_nexthop_same(const struct midr_spf_nexthop *a,
				  const struct midr_spf_nexthop *b)
{
	return a->ifindex == b->ifindex && ipaddr_cmp(&a->address, &b->address) == 0;
}

static void midr_spf_route_nexthops_clear(struct midr_spf_route *route)
{
	XFREE(MTYPE_MIDR_SPF_NEXTHOPS, route->nexthops);
	route->nexthop_count = 0;
}

static void midr_spf_route_nexthop_add(struct midr_spf_route *route,
				       const struct midr_ted_link *link,
				       uint32_t destination_group_id, uint32_t next_group_id)
{
	struct midr_spf_nexthop *nexthop;
	size_t count = route->nexthop_count + 1;

	route->nexthops = XREALLOC(MTYPE_MIDR_SPF_NEXTHOPS, route->nexthops,
				   count * sizeof(*route->nexthops));
	nexthop = &route->nexthops[route->nexthop_count++];
	*nexthop = (struct midr_spf_nexthop){
		.address = link->link_remote_address,
		.ifindex = link->local_ifindex,
		.local_node_id = link->local_node_id,
		.remote_node_id = link->remote_node_id,
		.local_group_id = link->local_group_id,
		.remote_group_id = link->remote_group_id,
		.destination_group_id = destination_group_id,
		.next_group_id = next_group_id,
		.link_id = link->link_id,
		.available_bandwidth_kbps = 0,
	};
}

static void midr_spf_route_nexthops_sort(struct midr_spf_route *route)
{
	size_t input;
	size_t output;

	if (route->nexthop_count < 2)
		return;
	qsort(route->nexthops, route->nexthop_count, sizeof(*route->nexthops),
	      midr_spf_nexthop_cmp);

	output = 0;
	for (input = 0; input < route->nexthop_count; input++) {
		if (output &&
		    midr_spf_nexthop_same(&route->nexthops[output - 1], &route->nexthops[input]))
			continue;
		if (output != input)
			route->nexthops[output] = route->nexthops[input];
		output++;
	}
	route->nexthop_count = output;
}

static bool midr_spf_local_paths_build(const struct midr_ted_snapshot *snapshot,
				       const struct midr_ted_prefix_key *prefix,
				       const struct midr_spf_graph *router,
				       struct midr_spf_route *route)
{
	const struct midr_spf_vertex *destination;
	struct midr_spf_graph_edge *first_hop;
	struct listnode *node;
	uint64_t best = MIDR_SPF_INFINITY;
	size_t i;
	bool mapping_found = false;
	bool reachable = false;

	for (i = 0; i < snapshot->node_prefix_count; i++) {
		const struct midr_ted_node_prefix *mapping = &snapshot->node_prefixes[i];

		if (!midr_spf_prefix_key_same(prefix, &mapping->key))
			continue;
		mapping_found = true;
		destination = midr_spf_vertex_lookup_const(router, mapping->node_id);
		if (!destination || !destination->reachable)
			continue;

		if (!reachable || destination->distance < best) {
			midr_spf_route_nexthops_clear(route);
			best = destination->distance;
			reachable = true;
			route->local_destination = mapping->node_id == snapshot->local_node_id;
		} else if (destination->distance > best) {
			continue;
		}

		if (mapping->node_id == snapshot->local_node_id)
			continue;
		for (ALL_LIST_ELEMENTS_RO(destination->first_hops, node, first_hop)) {
			if (first_hop->kind != MIDR_SPF_EDGE_LINK)
				continue;
			midr_spf_route_nexthop_add(route, first_hop->payload.link,
						   snapshot->local_group_id, 0);
		}
	}

	if (reachable) {
		route->reachable = true;
		route->scope = route->local_destination ? MIDR_SPF_ROUTE_LOCAL
							: MIDR_SPF_ROUTE_INTRA_GROUP;
		route->group_score = 0;
		route->local_cost = best;
		midr_spf_route_nexthops_sort(route);
	}
	return mapping_found && reachable;
}

static void midr_spf_cross_paths_build(const struct midr_ted_snapshot *snapshot,
				       const struct midr_ted_prefix_key *prefix,
				       const struct midr_spf_graph *router,
				       const struct midr_spf_graph *groups,
				       struct midr_spf_route *route)
{
	const struct midr_spf_vertex *border;
	const struct midr_spf_vertex *destination;
	struct midr_spf_graph_edge *group_first_hop;
	struct midr_spf_graph_edge *router_first_hop;
	struct listnode *group_node;
	struct listnode *router_node;
	uint64_t best_group_score = MIDR_SPF_INFINITY;
	uint64_t best_local_cost = MIDR_SPF_INFINITY;
	size_t i;
	size_t j;
	bool reachable = false;

	for (i = 0; i < snapshot->prefix_group_count; i++) {
		const struct midr_ted_prefix_group *mapping = &snapshot->prefix_groups[i];

		if (!midr_spf_prefix_key_same(prefix, &mapping->key) ||
		    mapping->group_id == snapshot->local_group_id)
			continue;
		destination = midr_spf_vertex_lookup_const(groups, mapping->group_id);
		if (!destination || !destination->reachable)
			continue;

		for (ALL_LIST_ELEMENTS_RO(destination->first_hops, group_node, group_first_hop)) {
			uint32_t next_group_id;

			if (group_first_hop->kind != MIDR_SPF_EDGE_GROUP ||
			    group_first_hop->source_id != snapshot->local_group_id)
				continue;
			next_group_id = group_first_hop->target_id;

			for (j = 0; j < snapshot->egress_link_count; j++) {
				const struct midr_ted_link *egress = &snapshot->egress_links[j];
				uint64_t local_cost;

				if (!midr_spf_cross_candidate_valid(snapshot, egress,
								    next_group_id))
					continue;
				border = midr_spf_vertex_lookup_const(router,
								      egress->local_node_id);
				if (!border || !border->reachable)
					continue;
				if (border != router->source && listcount(border->first_hops) == 0)
					continue;
				if (!midr_spf_add_cost(border->distance, egress->canonical_cost,
						       &local_cost))
					continue;
				if (reachable && (destination->distance > best_group_score ||
						  (destination->distance == best_group_score &&
						   local_cost > best_local_cost)))
					continue;
				if (!reachable || destination->distance < best_group_score ||
				    (destination->distance == best_group_score &&
				     local_cost < best_local_cost)) {
					midr_spf_route_nexthops_clear(route);
					best_group_score = destination->distance;
					best_local_cost = local_cost;
					reachable = true;
				}

				if (border == router->source) {
					midr_spf_route_nexthop_add(route, egress, mapping->group_id,
								   next_group_id);
					continue;
				}
				for (ALL_LIST_ELEMENTS_RO(border->first_hops, router_node,
							  router_first_hop)) {
					if (router_first_hop->kind != MIDR_SPF_EDGE_LINK)
						continue;
					midr_spf_route_nexthop_add(route,
								   router_first_hop->payload.link,
								   mapping->group_id,
								   next_group_id);
				}
			}
		}
	}

	if (reachable) {
		route->reachable = true;
		route->scope = MIDR_SPF_ROUTE_INTER_GROUP;
		route->group_score = best_group_score;
		route->local_cost = best_local_cost;
		midr_spf_route_nexthops_sort(route);
	}
}

static bool midr_spf_snapshot_valid(const struct midr_ted_snapshot *snapshot)
{
	return snapshot && snapshot->ready && snapshot->generation && snapshot->local_node_id &&
	       snapshot->local_group_id && (snapshot->node_count == 0 || snapshot->nodes) &&
	       (snapshot->intra_link_count == 0 || snapshot->intra_links) &&
	       (snapshot->egress_link_count == 0 || snapshot->egress_links) &&
	       (snapshot->node_prefix_count == 0 || snapshot->node_prefixes) &&
	       (snapshot->group_edge_count == 0 || snapshot->group_edges) &&
	       (snapshot->prefix_group_count == 0 || snapshot->prefix_groups);
}

static int midr_spf_route_compute(const struct midr_ted_snapshot *snapshot,
				  const struct midr_ted_prefix_key *prefix,
				  const struct midr_spf_graph *router,
				  const struct midr_spf_graph *groups, struct midr_spf_route **out)
{
	struct midr_spf_route *route;

	route = XCALLOC(MTYPE_MIDR_SPF_ROUTE, sizeof(*route));
	route->prefix = *prefix;
	route->generation = snapshot->generation;
	route->sync_reason_flags = snapshot->sync_reason_flags;
	route->scope = MIDR_SPF_ROUTE_UNREACHABLE;
	route->group_score = MIDR_SPF_INFINITY;
	route->local_cost = MIDR_SPF_INFINITY;

	if (!midr_spf_local_paths_build(snapshot, prefix, router, route))
		midr_spf_cross_paths_build(snapshot, prefix, router, groups, route);

	*out = route;
	return 0;
}

int midr_compute_path(const struct midr_ted_snapshot *snapshot,
		      const struct midr_ted_prefix_key *prefix, struct midr_spf_route **out)
{
	struct midr_ted_prefix_key normalized;
	struct midr_spf_graph *groups = NULL;
	struct midr_spf_graph *router = NULL;
	int ret;

	if (!out || *out)
		return -EINVAL;
	if (!midr_spf_snapshot_valid(snapshot))
		return -EINVAL;
	ret = midr_spf_prefix_key_normalize(prefix, &normalized);
	if (ret)
		return ret;

	ret = midr_spf_router_graph_build(snapshot, &router);
	if (ret)
		goto fail;
	ret = midr_spf_graph_run(router, snapshot->local_node_id);
	if (ret)
		goto fail;

	ret = midr_spf_group_graph_build(snapshot, router, &groups);
	if (ret)
		goto fail;
	ret = midr_spf_graph_run(groups, snapshot->local_group_id);
	if (ret)
		goto fail;
	ret = midr_spf_route_compute(snapshot, &normalized, router, groups, out);

	midr_spf_graph_free(&groups);
	midr_spf_graph_free(&router);
	return ret;

fail:
	midr_spf_graph_free(&groups);
	midr_spf_graph_free(&router);
	return ret;
}

void midr_spf_route_free(struct midr_spf_route **routep)
{
	struct midr_spf_route *route;

	if (!routep || !*routep)
		return;
	route = *routep;
	*routep = NULL;
	midr_spf_route_nexthops_clear(route);
	XFREE(MTYPE_MIDR_SPF_ROUTE, route);
}

static void midr_spf_results_free(struct midr_spf_results *results)
{
	size_t i;

	if (!results)
		return;
	for (i = 0; i < results->route_count; i++)
		midr_spf_route_nexthops_clear(&results->routes[i]);
	XFREE(MTYPE_MIDR_SPF_ROUTE, results->routes);
	XFREE(MTYPE_MIDR_SPF_RESULTS, results);
}

const struct midr_spf_results *midr_spf_results_acquire(const struct midr_spf_results *results)
{
	assert(results && results->refcount < UINT32_MAX);
	((struct midr_spf_results *)(uintptr_t)results)->refcount++;
	return results;
}

void midr_spf_results_release(const struct midr_spf_results **resultsp)
{
	struct midr_spf_results *results;

	if (!resultsp || !*resultsp)
		return;
	results = (struct midr_spf_results *)(uintptr_t)*resultsp;
	*resultsp = NULL;
	assert(results->refcount > 0);
	if (--results->refcount == 0)
		midr_spf_results_free(results);
}

int midr_spf_compute_all(const struct midr_ted_snapshot *snapshot,
			 const struct midr_spf_results **out)
{
	struct midr_ted_prefix_key *keys = NULL;
	struct midr_spf_graph *groups = NULL;
	struct midr_spf_graph *router = NULL;
	struct midr_spf_results *results = NULL;
	size_t candidate_count;
	size_t unique_count;
	size_t i;
	int ret = 0;

	if (!out || *out)
		return -EINVAL;
	if (!midr_spf_snapshot_valid(snapshot))
		return -EINVAL;
	if (snapshot->node_prefix_count > SIZE_MAX - snapshot->prefix_group_count)
		return -EOVERFLOW;
	candidate_count = snapshot->node_prefix_count + snapshot->prefix_group_count;

	results = XCALLOC(MTYPE_MIDR_SPF_RESULTS, sizeof(*results));
	results->refcount = 1;
	results->generation = snapshot->generation;
	results->sync_reason_flags = snapshot->sync_reason_flags;
	if (!candidate_count)
		goto done;

	keys = XCALLOC(MTYPE_MIDR_SPF_PREFIXES, candidate_count * sizeof(*keys));
	for (i = 0; i < snapshot->node_prefix_count; i++)
		keys[i] = snapshot->node_prefixes[i].key;
	for (i = 0; i < snapshot->prefix_group_count; i++)
		keys[snapshot->node_prefix_count + i] = snapshot->prefix_groups[i].key;
	qsort(keys, candidate_count, sizeof(*keys), midr_spf_prefix_key_cmp);

	unique_count = 0;
	for (i = 0; i < candidate_count; i++) {
		if (unique_count && midr_spf_prefix_key_same(&keys[unique_count - 1], &keys[i]))
			continue;
		keys[unique_count++] = keys[i];
	}

	results->routes = XCALLOC(MTYPE_MIDR_SPF_ROUTE, unique_count * sizeof(*results->routes));

	ret = midr_spf_router_graph_build(snapshot, &router);
	if (ret)
		goto fail;
	ret = midr_spf_graph_run(router, snapshot->local_node_id);
	if (ret)
		goto fail;
	ret = midr_spf_group_graph_build(snapshot, router, &groups);
	if (ret)
		goto fail;
	ret = midr_spf_graph_run(groups, snapshot->local_group_id);
	if (ret)
		goto fail;

	for (i = 0; i < unique_count; i++) {
		struct midr_spf_route *route = NULL;

		ret = midr_spf_route_compute(snapshot, &keys[i], router, groups, &route);
		if (ret)
			goto fail;
		results->routes[results->route_count++] = *route;
		XFREE(MTYPE_MIDR_SPF_ROUTE, route);
	}

done:
	midr_spf_graph_free(&groups);
	midr_spf_graph_free(&router);
	XFREE(MTYPE_MIDR_SPF_PREFIXES, keys);
	*out = results;
	return 0;

fail:
	midr_spf_graph_free(&groups);
	midr_spf_graph_free(&router);
	XFREE(MTYPE_MIDR_SPF_PREFIXES, keys);
	midr_spf_results_free(results);
	return ret;
}

uint64_t midr_spf_results_generation(const struct midr_spf_results *results)
{
	return results ? results->generation : 0;
}

uint64_t midr_spf_results_sync_reason_flags(const struct midr_spf_results *results)
{
	return results ? results->sync_reason_flags : 0;
}

size_t midr_spf_results_count(const struct midr_spf_results *results)
{
	return results ? results->route_count : 0;
}

const struct midr_spf_route *midr_spf_results_at(const struct midr_spf_results *results,
						 size_t index)
{
	if (!results || index >= results->route_count)
		return NULL;
	return &results->routes[index];
}

const struct midr_spf_route *midr_spf_results_lookup(const struct midr_spf_results *results,
						     const struct midr_ted_prefix_key *prefix)
{
	struct midr_ted_prefix_key normalized;
	size_t left = 0;
	size_t right;

	if (!results || midr_spf_prefix_key_normalize(prefix, &normalized) != 0)
		return NULL;
	right = results->route_count;
	while (left < right) {
		size_t middle = left + (right - left) / 2;
		int ret = midr_spf_prefix_key_cmp_data(&results->routes[middle].prefix,
						       &normalized);

		if (ret < 0)
			left = middle + 1;
		else
			right = middle;
	}
	if (left == results->route_count ||
	    !midr_spf_prefix_key_same(&results->routes[left].prefix, &normalized))
		return NULL;
	return &results->routes[left];
}
