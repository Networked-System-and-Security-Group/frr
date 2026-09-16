/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "midr-spf.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define MIDR_SPF_INFINITY UINT64_MAX

struct midr_spf_vertex {
	uint32_t node_id;
	uint64_t distance;
	size_t *first_hops;
	size_t first_hop_count;
	bool settled;
};

struct midr_spf_group_vertex {
	uint32_t group_id;
	uint64_t distance;
	uint32_t *first_hops;
	size_t first_hop_count;
	bool settled;
};

static int route_key_equal(const struct midr_spf_route *route,
			   const struct midr_consumer_event *event)
{
	return route->family == event->family &&
	       route->prefix_len == event->prefix_len &&
	       !memcmp(route->prefix, event->prefix, sizeof(route->prefix));
}

static int vertex_find(const struct midr_spf_vertex *vertices, size_t count,
			      uint32_t node_id)
{
	for (size_t i = 0; i < count; i++)
		if (vertices[i].node_id == node_id)
			return (int)i;
	return -1;
}

static int vertex_add(struct midr_spf_vertex *vertices, size_t *count,
			      size_t capacity, uint32_t node_id)
{
	if (!node_id)
		return -EINVAL;
	if (vertex_find(vertices, *count, node_id) >= 0)
		return 0;
	if (*count == capacity)
		return -ENOSPC;
	vertices[*count].node_id = node_id;
	vertices[*count].distance = MIDR_SPF_INFINITY;
	vertices[*count].settled = false;
	(*count)++;
	return 0;
}

static bool link_cost_valid(const struct midr_consumer_event *event)
{
	return event->kind == MIDR_CONSUMER_LINK && event->originator &&
	       event->remote && event->originator != event->remote &&
	       event->metric > 0 && event->metric < UINT32_MAX;
}

static uint64_t add_cost(uint64_t left, uint64_t right)
{
	if (left == MIDR_SPF_INFINITY ||
	    right > UINT64_MAX - left)
		return MIDR_SPF_INFINITY;
	return left + right;
}

void midr_spf_routes_clear(struct midr_spf_route *routes, size_t count)
{
	if (!routes)
		return;
	for (size_t i = 0; i < count; i++) {
		free(routes[i].nexthops);
		routes[i].nexthops = NULL;
		routes[i].nexthop_count = 0;
	}
}

static void route_nexthops_clear(struct midr_spf_route *route)
{
	free(route->nexthops);
	route->nexthops = NULL;
	route->nexthop_count = 0;
}

static bool nexthop_same(const struct midr_spf_nexthop *left,
			 const struct midr_spf_nexthop *right)
{
	return left->family == right->family && left->ifindex == right->ifindex &&
	       !memcmp(left->address, right->address, sizeof(left->address));
}

static int route_nexthop_add(struct midr_spf_route *route,
			     const struct midr_spf_nexthop *nexthop)
{
	struct midr_spf_nexthop *grown;

	for (size_t i = 0; i < route->nexthop_count; i++)
		if (nexthop_same(&route->nexthops[i], nexthop))
			return 0;
	if (route->nexthop_count == SIZE_MAX / sizeof(*route->nexthops))
		return -EOVERFLOW;
	grown = realloc(route->nexthops,
			(route->nexthop_count + 1U) * sizeof(*grown));
	if (!grown)
		return -ENOMEM;
	route->nexthops = grown;
	route->nexthops[route->nexthop_count++] = *nexthop;
	return 0;
}

static int nexthop_compare(const void *leftp, const void *rightp)
{
	const struct midr_spf_nexthop *left = leftp;
	const struct midr_spf_nexthop *right = rightp;
	int ret;

	ret = (left->family > right->family) - (left->family < right->family);
	if (ret)
		return ret;
	ret = memcmp(left->address, right->address, sizeof(left->address));
	if (ret)
		return ret;
	ret = (left->ifindex > right->ifindex) -
	      (left->ifindex < right->ifindex);
	if (ret)
		return ret;
	ret = (left->remote_node_id > right->remote_node_id) -
	      (left->remote_node_id < right->remote_node_id);
	if (ret)
		return ret;
	return (left->link_id > right->link_id) -
	       (left->link_id < right->link_id);
}

static void route_nexthops_sort(struct midr_spf_route *route)
{
	if (route->nexthop_count > 1U)
		qsort(route->nexthops, route->nexthop_count,
		      sizeof(*route->nexthops), nexthop_compare);
}

static int vertex_first_hop_add(struct midr_spf_vertex *vertex,
				size_t first_hop)
{
	size_t *grown;

	for (size_t i = 0; i < vertex->first_hop_count; i++)
		if (vertex->first_hops[i] == first_hop)
			return 0;
	if (vertex->first_hop_count == SIZE_MAX / sizeof(*vertex->first_hops))
		return -EOVERFLOW;
	grown = realloc(vertex->first_hops,
			(vertex->first_hop_count + 1U) * sizeof(*grown));
	if (!grown)
		return -ENOMEM;
	vertex->first_hops = grown;
	vertex->first_hops[vertex->first_hop_count++] = first_hop;
	return 0;
}

static int vertex_first_hops_merge(struct midr_spf_vertex *target,
				   const struct midr_spf_vertex *source)
{
	int ret;

	for (size_t i = 0; i < source->first_hop_count; i++) {
		ret = vertex_first_hop_add(target, source->first_hops[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static void vertices_free(struct midr_spf_vertex *vertices, size_t count)
{
	if (!vertices)
		return;
	for (size_t i = 0; i < count; i++)
		free(vertices[i].first_hops);
	free(vertices);
}

static int graph_build(const struct midr_consumer_snapshot *snapshot,
			       uint32_t local_node_id,
			       struct midr_spf_vertex *vertices, size_t *vertex_count,
			       bool *have_link)
{
	int ret;
	size_t capacity = snapshot->count;

	if (capacity > (SIZE_MAX - 1U) / 2U)
		return -EOVERFLOW;
	capacity = capacity * 2U + 1U;

	*vertex_count = 0;
	*have_link = false;
	ret = vertex_add(vertices, vertex_count, capacity,
			local_node_id);
	if (ret)
		return ret;
	for (size_t i = 0; i < snapshot->count; i++) {
		const struct midr_consumer_event *event = &snapshot->events[i];

		if (event->kind == MIDR_CONSUMER_LINK) {
			if (!link_cost_valid(event))
				continue;
			*have_link = true;
			ret = vertex_add(vertices, vertex_count, capacity,
					 event->originator);
			if (ret)
				return ret;
			ret = vertex_add(vertices, vertex_count, capacity,
					 event->remote);
			if (ret)
				return ret;
		} else if (event->kind == MIDR_CONSUMER_NODE_PREFIX ||
			   event->kind == MIDR_CONSUMER_GROUP_PREFIX) {
			ret = vertex_add(vertices, vertex_count, capacity,
					 event->originator);
			if (ret)
				return ret;
		}
	}
	return 0;
}

static int graph_run(const struct midr_consumer_snapshot *snapshot,
			     struct midr_spf_vertex *vertices, size_t vertex_count,
			     uint32_t local_node_id)
{
	int source = vertex_find(vertices, vertex_count, local_node_id);

	if (source < 0)
		return -ENOENT;
	vertices[source].distance = 0;
	for (;;) {
		int current = -1;
		uint64_t best = MIDR_SPF_INFINITY;

		for (size_t i = 0; i < vertex_count; i++)
			if (!vertices[i].settled && vertices[i].distance < best) {
				current = (int)i;
				best = vertices[i].distance;
			}
		if (current < 0)
			break;
		vertices[current].settled = true;
		for (size_t i = 0; i < snapshot->count; i++) {
			const struct midr_consumer_event *event =
				&snapshot->events[i];
			int target;
			uint64_t candidate;

			if (!link_cost_valid(event) ||
			    vertices[current].node_id != event->originator)
				continue;
			target = vertex_find(vertices, vertex_count, event->remote);
			if (target < 0 || vertices[target].settled)
				continue;
			candidate = add_cost(vertices[current].distance,
					     event->metric);
			if (candidate < vertices[target].distance) {
				vertices[target].distance = candidate;
				free(vertices[target].first_hops);
				vertices[target].first_hops = NULL;
				vertices[target].first_hop_count = 0;
			}
			if (candidate != vertices[target].distance)
				continue;
			if (current == source) {
				int ret = vertex_first_hop_add(&vertices[target], i);

				if (ret)
					return ret;
			} else {
				int ret = vertex_first_hops_merge(&vertices[target],
							  &vertices[current]);

				if (ret)
					return ret;
			}
		}
	}
	return 0;
}

static uint64_t route_metric(uint64_t distance, uint32_t prefix_metric)
{
	if (distance == MIDR_SPF_INFINITY)
		return prefix_metric;
	return add_cost(distance, prefix_metric);
}

static int route_add_event_nexthop(struct midr_spf_route *route,
				   const struct midr_consumer_event *event)
{
	struct midr_spf_nexthop nexthop = {
		.family = event->family,
		.local_node_id = event->originator,
		.remote_node_id = event->remote,
		.link_id = event->link_id,
	};

	if (event->family != route->family)
		return 0;
	memcpy(nexthop.address, event->remote_address,
	       sizeof(nexthop.address));
	return route_nexthop_add(route, &nexthop);
}

int midr_spf_compute(const struct midr_consumer_snapshot *snapshot,
			    uint32_t local_node_id,
			    struct midr_spf_route *routes, size_t capacity,
			    size_t *count)
{
	struct midr_spf_vertex *vertices = NULL;
	size_t vertex_count = 0;
	bool have_link = false;
	size_t route_count = 0;
	int ret;

	if (!snapshot || !local_node_id || !count || (capacity && !routes) ||
	    (snapshot->count && !snapshot->events))
		return -EINVAL;
	*count = 0;
	if (snapshot->count > (SIZE_MAX - 1U) / 2U)
		return -EOVERFLOW;
	vertices = calloc(snapshot->count * 2U + 1U, sizeof(*vertices));
	if (!vertices)
		return -ENOMEM;
	ret = graph_build(snapshot, local_node_id, vertices, &vertex_count,
			  &have_link);
	if (ret)
		goto done;
	if (have_link) {
		ret = graph_run(snapshot, vertices, vertex_count, local_node_id);
		if (ret)
			goto done;
	}
	for (size_t i = 0; i < snapshot->count; i++) {
		const struct midr_consumer_event *event = &snapshot->events[i];
		struct midr_spf_route *route = NULL;
		int vertex;
		uint64_t metric;
		bool reachable;

		if (event->kind != MIDR_CONSUMER_NODE_PREFIX &&
		    event->kind != MIDR_CONSUMER_GROUP_PREFIX)
			continue;
		for (size_t j = 0; j < route_count; j++)
			if (route_key_equal(&routes[j], event)) {
				route = &routes[j];
				break;
			}
		if (!route) {
			if (route_count == capacity) {
				ret = -ENOSPC;
				goto done;
			}
			route = &routes[route_count++];
			memset(route, 0, sizeof(*route));
			route->family = event->family;
			route->prefix_len = event->prefix_len;
			memcpy(route->prefix, event->prefix,
			       sizeof(route->prefix));
		}
		vertex = vertex_find(vertices, vertex_count, event->originator);
		metric = vertex >= 0 && have_link
			? route_metric(vertices[vertex].distance, event->metric)
			: event->metric;
		reachable = !have_link ||
			(vertex >= 0 &&
			 vertices[vertex].distance != MIDR_SPF_INFINITY);
		if (!route->generation || (reachable && !route->reachable) ||
		    (reachable == route->reachable && metric < route->metric) ||
		    (reachable && route->reachable && metric == route->metric &&
		     event->originator == local_node_id &&
		     !route->local_destination)) {
			route_nexthops_clear(route);
			route->originator = event->originator;
			route->metric = metric;
			route->generation = snapshot->generation;
			route->reachable = reachable;
			route->local_destination =
				event->originator == local_node_id;
			route->scope = route->local_destination
				? MIDR_SPF_ROUTE_LOCAL
				: (reachable ? MIDR_SPF_ROUTE_INTRA_GROUP
					     : MIDR_SPF_ROUTE_UNREACHABLE);
			route->local_cost = metric;
		}
		if (!reachable || metric != route->metric ||
		    (route->local_destination &&
		     event->originator != local_node_id))
			continue;
		if (event->originator < route->originator)
			route->originator = event->originator;
		if (route->local_destination || !have_link)
			continue;
		for (size_t j = 0; j < vertices[vertex].first_hop_count; j++) {
			ret = route_add_event_nexthop(
				route,
				&snapshot->events[vertices[vertex].first_hops[j]]);
			if (ret)
				goto done;
		}
	}
	for (size_t i = 0; i < route_count; i++)
		route_nexthops_sort(&routes[i]);
	*count = route_count;
	ret = 0;
done:
	if (ret) {
		midr_spf_routes_clear(routes, route_count);
		*count = 0;
	}
	vertices_free(vertices, vertex_count);
	return ret;
}

static bool ted_prefix_valid(const struct midr_ted_prefix_key *key)
{
	if (!key)
		return false;
	if (key->family == MIDR_CORE_AF_IPV4)
		return key->prefix_len <= 32U;
	if (key->family == MIDR_CORE_AF_IPV6)
		return key->prefix_len <= 128U;
	return false;
}

static bool ted_route_key_equal(const struct midr_spf_route *route,
				const struct midr_ted_prefix_key *key)
{
	return route->family == key->family &&
	       route->prefix_len == key->prefix_len &&
	       !memcmp(route->prefix, key->prefix, sizeof(route->prefix));
}

static int ted_route_get(struct midr_spf_route *routes, size_t capacity,
			 size_t *route_count,
			 const struct midr_ted_prefix_key *key,
			 uint64_t generation, struct midr_spf_route **out)
{
	struct midr_spf_route *route;

	if (!ted_prefix_valid(key))
		return -EINVAL;
	for (size_t i = 0; i < *route_count; i++)
		if (ted_route_key_equal(&routes[i], key)) {
			*out = &routes[i];
			return 0;
		}
	if (*route_count == capacity)
		return -ENOSPC;
	route = &routes[(*route_count)++];
	memset(route, 0, sizeof(*route));
	route->family = key->family;
	route->prefix_len = key->prefix_len;
	memcpy(route->prefix, key->prefix, sizeof(route->prefix));
	route->metric = MIDR_SPF_INFINITY;
	route->generation = generation;
	route->scope = MIDR_SPF_ROUTE_UNREACHABLE;
	*out = route;
	return 0;
}

static int route_add_ted_nexthop(struct midr_spf_route *route,
				 const struct midr_ted_link *link,
				 uint32_t destination_group_id,
				 uint32_t next_group_id)
{
	struct midr_spf_nexthop nexthop = {
		.family = link->family,
		.ifindex = link->local_ifindex,
		.local_node_id = link->local_node_id,
		.remote_node_id = link->remote_node_id,
		.local_group_id = link->local_group_id,
		.remote_group_id = link->remote_group_id,
		.destination_group_id = destination_group_id,
		.next_group_id = next_group_id,
		.link_id = link->link_id,
	};

	if (link->family != route->family)
		return 0;
	memcpy(nexthop.address, link->remote_address,
	       sizeof(nexthop.address));
	return route_nexthop_add(route, &nexthop);
}

static int ted_route_consider_local(const struct midr_ted_view *view,
				    const struct midr_spf_vertex *vertices,
				    size_t vertex_count, uint32_t node_id,
				    struct midr_spf_route *route)
{
	uint64_t metric;
	bool better;
	int node;
	int ret;

	node = vertex_find(vertices, vertex_count, node_id);
	if (node < 0 || vertices[node].distance == MIDR_SPF_INFINITY)
		return 0;
	metric = vertices[node].distance;
	better = !route->reachable || metric < route->metric ||
		 (metric == route->metric && node_id == view->local_node_id &&
		  !route->local_destination);
	if (better) {
		route_nexthops_clear(route);
		route->originator = node_id;
		route->metric = metric;
		route->group_score = 0;
		route->local_cost = metric;
		route->reachable = true;
		route->local_destination = node_id == view->local_node_id;
		route->scope = route->local_destination
			? MIDR_SPF_ROUTE_LOCAL
			: MIDR_SPF_ROUTE_INTRA_GROUP;
	} else if (metric != route->metric ||
		   route->scope == MIDR_SPF_ROUTE_LOCAL) {
		return 0;
	}
	if (node_id < route->originator)
		route->originator = node_id;
	if (route->local_destination)
		return 0;
	for (size_t i = 0; i < vertices[node].first_hop_count; i++) {
		size_t first_hop = vertices[node].first_hops[i];

		if (first_hop >= view->intra_link_count)
			return -EPROTO;
		ret = route_add_ted_nexthop(route, &view->intra_links[first_hop],
					     view->local_group_id, 0);
		if (ret)
			return ret;
	}
	return 0;
}

static bool ted_link_cost_valid(const struct midr_ted_link *link)
{
	return link->local_node_id && link->remote_node_id &&
	       link->local_node_id != link->remote_node_id &&
	       link->canonical_cost > 0 &&
	       link->canonical_cost < UINT32_MAX;
}

static int ted_node_graph_build(const struct midr_ted_view *view,
				struct midr_spf_vertex *vertices,
				size_t capacity, size_t *vertex_count)
{
	int ret;

	*vertex_count = 0;
	ret = vertex_add(vertices, vertex_count, capacity, view->local_node_id);
	if (ret)
		return ret;
	for (size_t i = 0; i < view->node_count; i++) {
		if (!view->nodes[i].node_id ||
		    view->nodes[i].group_id != view->local_group_id)
			return -EINVAL;
		ret = vertex_add(vertices, vertex_count, capacity,
				 view->nodes[i].node_id);
		if (ret)
			return ret;
	}
	for (size_t i = 0; i < view->intra_link_count; i++) {
		const struct midr_ted_link *link = &view->intra_links[i];

		if (!ted_link_cost_valid(link) ||
		    link->local_group_id != view->local_group_id ||
		    link->remote_group_id != view->local_group_id)
			return -EINVAL;
		ret = vertex_add(vertices, vertex_count, capacity,
				 link->local_node_id);
		if (ret)
			return ret;
		ret = vertex_add(vertices, vertex_count, capacity,
				 link->remote_node_id);
		if (ret)
			return ret;
	}
	for (size_t i = 0; i < view->egress_link_count; i++) {
		const struct midr_ted_link *link = &view->egress_links[i];

		if (!ted_link_cost_valid(link) ||
		    link->local_group_id != view->local_group_id ||
		    !link->remote_group_id ||
		    link->remote_group_id == view->local_group_id)
			return -EINVAL;
		ret = vertex_add(vertices, vertex_count, capacity,
				 link->local_node_id);
		if (ret)
			return ret;
	}
	for (size_t i = 0; i < view->node_prefix_count; i++) {
		if (!ted_prefix_valid(&view->node_prefixes[i].key) ||
		    !view->node_prefixes[i].node_id)
			return -EINVAL;
		ret = vertex_add(vertices, vertex_count, capacity,
				 view->node_prefixes[i].node_id);
		if (ret)
			return ret;
	}
	return 0;
}

static int ted_node_graph_run(const struct midr_ted_view *view,
			      struct midr_spf_vertex *vertices,
			      size_t vertex_count)
{
	int source = vertex_find(vertices, vertex_count, view->local_node_id);

	if (source < 0)
		return -ENOENT;
	vertices[source].distance = 0;
	for (;;) {
		int current = -1;
		uint64_t best = MIDR_SPF_INFINITY;

		for (size_t i = 0; i < vertex_count; i++)
			if (!vertices[i].settled && vertices[i].distance < best) {
				current = (int)i;
				best = vertices[i].distance;
			}
		if (current < 0)
			break;
		vertices[current].settled = true;
		for (size_t i = 0; i < view->intra_link_count; i++) {
			const struct midr_ted_link *link = &view->intra_links[i];
			int target;
			uint64_t candidate;

			if (vertices[current].node_id != link->local_node_id)
				continue;
			target = vertex_find(vertices, vertex_count,
					     link->remote_node_id);
			if (target < 0 || vertices[target].settled)
				continue;
			candidate = add_cost(vertices[current].distance,
					     link->canonical_cost);
			if (candidate < vertices[target].distance) {
				vertices[target].distance = candidate;
				free(vertices[target].first_hops);
				vertices[target].first_hops = NULL;
				vertices[target].first_hop_count = 0;
			}
			if (candidate != vertices[target].distance)
				continue;
			if (current == source) {
				int ret = vertex_first_hop_add(&vertices[target], i);

				if (ret)
					return ret;
			} else {
				int ret = vertex_first_hops_merge(&vertices[target],
							  &vertices[current]);

				if (ret)
					return ret;
			}
		}
	}
	return 0;
}

static int group_vertex_find(const struct midr_spf_group_vertex *vertices,
			     size_t count, uint32_t group_id)
{
	for (size_t i = 0; i < count; i++)
		if (vertices[i].group_id == group_id)
			return (int)i;
	return -1;
}

static int group_vertex_add(struct midr_spf_group_vertex *vertices,
			    size_t *count, size_t capacity,
			    uint32_t group_id)
{
	if (!group_id)
		return -EINVAL;
	if (group_vertex_find(vertices, *count, group_id) >= 0)
		return 0;
	if (*count == capacity)
		return -ENOSPC;
	vertices[*count].group_id = group_id;
	vertices[*count].distance = MIDR_SPF_INFINITY;
	(*count)++;
	return 0;
}

static int group_first_hop_add(struct midr_spf_group_vertex *vertex,
			       uint32_t first_hop)
{
	uint32_t *grown;

	for (size_t i = 0; i < vertex->first_hop_count; i++)
		if (vertex->first_hops[i] == first_hop)
			return 0;
	if (vertex->first_hop_count == SIZE_MAX / sizeof(*vertex->first_hops))
		return -EOVERFLOW;
	grown = realloc(vertex->first_hops,
			(vertex->first_hop_count + 1U) * sizeof(*grown));
	if (!grown)
		return -ENOMEM;
	vertex->first_hops = grown;
	vertex->first_hops[vertex->first_hop_count++] = first_hop;
	return 0;
}

static int group_first_hops_merge(struct midr_spf_group_vertex *target,
				  const struct midr_spf_group_vertex *source)
{
	int ret;

	for (size_t i = 0; i < source->first_hop_count; i++) {
		ret = group_first_hop_add(target, source->first_hops[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static void group_vertices_free(struct midr_spf_group_vertex *vertices,
				size_t count)
{
	if (!vertices)
		return;
	for (size_t i = 0; i < count; i++)
		free(vertices[i].first_hops);
	free(vertices);
}

static int ted_group_graph_build(const struct midr_ted_view *view,
				 struct midr_spf_group_vertex *vertices,
				 size_t capacity, size_t *vertex_count)
{
	int ret;

	*vertex_count = 0;
	ret = group_vertex_add(vertices, vertex_count, capacity,
			       view->local_group_id);
	if (ret)
		return ret;
	for (size_t i = 0; i < view->group_edge_count; i++) {
		const struct midr_ted_group_edge *edge = &view->group_edges[i];

		if (!edge->source_group_id || !edge->target_group_id ||
		    edge->source_group_id == edge->target_group_id ||
		    !edge->aggregate_cost ||
		    edge->aggregate_cost == MIDR_SPF_INFINITY)
			return -EINVAL;
		ret = group_vertex_add(vertices, vertex_count, capacity,
				       edge->source_group_id);
		if (ret)
			return ret;
		ret = group_vertex_add(vertices, vertex_count, capacity,
				       edge->target_group_id);
		if (ret)
			return ret;
	}
	for (size_t i = 0; i < view->prefix_group_count; i++) {
		if (!ted_prefix_valid(&view->prefix_groups[i].key) ||
		    !view->prefix_groups[i].group_id)
			return -EINVAL;
		ret = group_vertex_add(vertices, vertex_count, capacity,
				       view->prefix_groups[i].group_id);
		if (ret)
			return ret;
	}
	return 0;
}

static int ted_group_graph_run(const struct midr_ted_view *view,
			       struct midr_spf_group_vertex *vertices,
			       size_t vertex_count)
{
	int source = group_vertex_find(vertices, vertex_count,
				       view->local_group_id);

	if (source < 0)
		return -ENOENT;
	vertices[source].distance = 0;
	for (;;) {
		int current = -1;
		uint64_t best = MIDR_SPF_INFINITY;

		for (size_t i = 0; i < vertex_count; i++)
			if (!vertices[i].settled &&
			    (vertices[i].distance < best ||
			     (vertices[i].distance == best && current >= 0 &&
			      vertices[i].group_id < vertices[current].group_id))) {
				current = (int)i;
				best = vertices[i].distance;
			}
		if (current < 0 || best == MIDR_SPF_INFINITY)
			break;
		vertices[current].settled = true;
		for (size_t i = 0; i < view->group_edge_count; i++) {
			const struct midr_ted_group_edge *edge =
				&view->group_edges[i];
			uint64_t candidate;
			int target;

			if (vertices[current].group_id != edge->source_group_id)
				continue;
			target = group_vertex_find(vertices, vertex_count,
						   edge->target_group_id);
			if (target < 0 || vertices[target].settled)
				continue;
			candidate = add_cost(vertices[current].distance,
					     edge->aggregate_cost);
			if (candidate < vertices[target].distance) {
				vertices[target].distance = candidate;
				free(vertices[target].first_hops);
				vertices[target].first_hops = NULL;
				vertices[target].first_hop_count = 0;
			}
			if (candidate != vertices[target].distance)
				continue;
			if (current == source) {
				int ret = group_first_hop_add(&vertices[target],
							      edge->target_group_id);

				if (ret)
					return ret;
			} else {
				int ret = group_first_hops_merge(&vertices[target],
							 &vertices[current]);

				if (ret)
					return ret;
			}
		}
	}
	return 0;
}

static uint64_t ted_first_group_edge_cost(const struct midr_ted_view *view,
					  uint32_t first_hop)
{
	uint64_t cost = MIDR_SPF_INFINITY;

	for (size_t i = 0; i < view->group_edge_count; i++) {
		const struct midr_ted_group_edge *edge = &view->group_edges[i];

		if (edge->source_group_id == view->local_group_id &&
		    edge->target_group_id == first_hop &&
		    edge->aggregate_cost < cost)
			cost = edge->aggregate_cost;
	}
	return cost;
}

static int ted_consider_remote_group(
	const struct midr_ted_view *view,
	const struct midr_spf_vertex *node_vertices, size_t node_count,
	const struct midr_spf_group_vertex *group_vertices, size_t group_count,
	uint32_t target_group, struct midr_spf_route *route)
{
	uint64_t first_edge_cost;
	uint64_t suffix_cost;
	int target;
	int ret;

	if (route->reachable &&
	    (route->scope == MIDR_SPF_ROUTE_LOCAL ||
	     route->scope == MIDR_SPF_ROUTE_INTRA_GROUP))
		return 0;
	target = group_vertex_find(group_vertices, group_count, target_group);
	if (target < 0 ||
	    group_vertices[target].distance == MIDR_SPF_INFINITY ||
	    !group_vertices[target].first_hop_count)
		return 0;
	for (size_t hop = 0; hop < group_vertices[target].first_hop_count;
	     hop++) {
		uint32_t first_hop = group_vertices[target].first_hops[hop];

		first_edge_cost = ted_first_group_edge_cost(view, first_hop);
		if (first_edge_cost == MIDR_SPF_INFINITY ||
		    first_edge_cost > group_vertices[target].distance)
			continue;
		suffix_cost = group_vertices[target].distance - first_edge_cost;
		for (size_t i = 0; i < view->egress_link_count; i++) {
			const struct midr_ted_link *link = &view->egress_links[i];
			uint64_t local_cost;
			uint64_t metric;
			bool better;
			int local;

			if (link->remote_group_id != first_hop)
				continue;
			local = vertex_find(node_vertices, node_count,
					    link->local_node_id);
			if (local < 0 ||
			    node_vertices[local].distance == MIDR_SPF_INFINITY)
				continue;
			local_cost = add_cost(node_vertices[local].distance,
					      link->canonical_cost);
			metric = add_cost(local_cost, suffix_cost);
			if (local_cost == MIDR_SPF_INFINITY ||
			    metric == MIDR_SPF_INFINITY)
				continue;
			better = !route->reachable ||
				 group_vertices[target].distance < route->group_score ||
				 (group_vertices[target].distance == route->group_score &&
				  local_cost < route->local_cost);
			if (better) {
				route_nexthops_clear(route);
				route->originator = link->local_node_id;
				route->metric = metric;
				route->group_score =
					group_vertices[target].distance;
				route->local_cost = local_cost;
				route->reachable = true;
				route->local_destination = false;
				route->scope = MIDR_SPF_ROUTE_INTER_GROUP;
			} else if (group_vertices[target].distance !=
					   route->group_score ||
				   local_cost != route->local_cost) {
				continue;
			}
			if (link->local_node_id < route->originator)
				route->originator = link->local_node_id;
			if (link->local_node_id == view->local_node_id) {
				ret = route_add_ted_nexthop(route, link, target_group,
							     first_hop);
				if (ret)
					return ret;
				continue;
			}
			for (size_t j = 0;
			     j < node_vertices[local].first_hop_count; j++) {
				size_t first_link =
					node_vertices[local].first_hops[j];

				if (first_link >= view->intra_link_count)
					return -EPROTO;
				ret = route_add_ted_nexthop(
					route, &view->intra_links[first_link],
					target_group, first_hop);
				if (ret)
					return ret;
			}
		}
	}
	return 0;
}

int midr_spf_compute_ted(const struct midr_ted_view *view,
			 struct midr_spf_route *routes, size_t capacity,
			 size_t *count)
{
	struct midr_spf_group_vertex *group_vertices = NULL;
	struct midr_spf_vertex *node_vertices = NULL;
	size_t group_capacity;
	size_t group_count = 0;
	size_t node_capacity;
	size_t node_count = 0;
	size_t route_count = 0;
	int ret;

	if (!view || !count || (capacity && !routes) || !view->generation ||
	    !view->local_node_id || !view->local_group_id ||
	    (view->node_count && !view->nodes) ||
	    (view->intra_link_count && !view->intra_links) ||
	    (view->egress_link_count && !view->egress_links) ||
	    (view->node_prefix_count && !view->node_prefixes) ||
	    (view->group_edge_count && !view->group_edges) ||
	    (view->prefix_group_count && !view->prefix_groups))
		return -EINVAL;
	*count = 0;
	node_capacity = 1U;
	if (view->node_count > SIZE_MAX - node_capacity)
		return -EOVERFLOW;
	node_capacity += view->node_count;
	if (view->intra_link_count > (SIZE_MAX - node_capacity) / 2U)
		return -EOVERFLOW;
	node_capacity += view->intra_link_count * 2U;
	if (view->egress_link_count > SIZE_MAX - node_capacity)
		return -EOVERFLOW;
	node_capacity += view->egress_link_count;
	group_capacity = 1U;
	if (view->group_edge_count > (SIZE_MAX - group_capacity) / 2U)
		return -EOVERFLOW;
	group_capacity += view->group_edge_count * 2U;
	if (view->prefix_group_count > SIZE_MAX - group_capacity)
		return -EOVERFLOW;
	group_capacity += view->prefix_group_count;
	node_vertices = calloc(node_capacity, sizeof(*node_vertices));
	group_vertices = calloc(group_capacity, sizeof(*group_vertices));
	if (!node_vertices || !group_vertices) {
		ret = -ENOMEM;
		goto done;
	}
	ret = ted_node_graph_build(view, node_vertices, node_capacity,
				   &node_count);
	if (ret)
		goto done;
	ret = ted_group_graph_build(view, group_vertices, group_capacity,
				    &group_count);
	if (ret)
		goto done;
	ret = ted_node_graph_run(view, node_vertices, node_count);
	if (ret)
		goto done;
	ret = ted_group_graph_run(view, group_vertices, group_count);
	if (ret)
		goto done;

	for (size_t i = 0; i < view->node_prefix_count; i++) {
		const struct midr_ted_node_prefix *prefix =
			&view->node_prefixes[i];
		struct midr_spf_route *route;
		int node;

		ret = ted_route_get(routes, capacity, &route_count, &prefix->key,
				    view->generation, &route);
		if (ret)
			goto done;
		node = vertex_find(node_vertices, node_count, prefix->node_id);
		if (node >= 0) {
			ret = ted_route_consider_local(view, node_vertices, node_count,
					       prefix->node_id, route);
			if (ret)
				goto done;
		}
	}
	for (size_t i = 0; i < view->prefix_group_count; i++) {
		const struct midr_ted_prefix_group *prefix =
			&view->prefix_groups[i];
		struct midr_spf_route *route;

		ret = ted_route_get(routes, capacity, &route_count, &prefix->key,
				    view->generation, &route);
		if (ret)
			goto done;
		if (prefix->group_id != view->local_group_id) {
			ret = ted_consider_remote_group(
				view, node_vertices, node_count, group_vertices,
				group_count, prefix->group_id, route);
			if (ret)
				goto done;
		}
	}
	for (size_t i = 0; i < route_count; i++)
		route_nexthops_sort(&routes[i]);
	*count = route_count;
	ret = 0;

done:
	if (ret) {
		midr_spf_routes_clear(routes, route_count);
		*count = 0;
	}
	group_vertices_free(group_vertices, group_count);
	vertices_free(node_vertices, node_count);
	return ret;
}
