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

static uint64_t add_cost(uint64_t left, uint32_t right)
{
	if (left == MIDR_SPF_INFINITY ||
	    right > UINT64_MAX - left)
		return MIDR_SPF_INFINITY;
	return left + right;
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

static void graph_run(const struct midr_consumer_snapshot *snapshot,
			      struct midr_spf_vertex *vertices, size_t vertex_count,
			      uint32_t local_node_id)
{
	int source = vertex_find(vertices, vertex_count, local_node_id);

	if (source < 0)
		return;
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
			if (candidate < vertices[target].distance)
				vertices[target].distance = candidate;
		}
	}
}

static uint32_t route_metric(uint64_t distance, uint32_t prefix_metric)
{
	uint64_t total;

	if (distance == MIDR_SPF_INFINITY)
		return prefix_metric;
	total = add_cost(distance, prefix_metric);
	return total >= UINT32_MAX ? UINT32_MAX : (uint32_t)total;
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
	if (snapshot->count > (SIZE_MAX - 1U) / 2U)
		return -EOVERFLOW;
	vertices = calloc(snapshot->count * 2U + 1U, sizeof(*vertices));
	if (!vertices)
		return -ENOMEM;
	ret = graph_build(snapshot, local_node_id, vertices, &vertex_count,
			  &have_link);
	if (ret)
		goto done;
	if (have_link)
		graph_run(snapshot, vertices, vertex_count, local_node_id);
	for (size_t i = 0; i < snapshot->count; i++) {
		const struct midr_consumer_event *event = &snapshot->events[i];
		struct midr_spf_route *route = NULL;
		int vertex;
		uint32_t metric;
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
		    (reachable == route->reachable &&
		     (metric < route->metric ||
		      (metric == route->metric &&
		       event->originator < route->originator)))) {
			route->originator = event->originator;
			route->metric = metric;
			route->generation = event->generation;
			route->reachable = reachable;
		}
	}
	*count = route_count;
	ret = 0;
done:
	free(vertices);
	return ret;
}
