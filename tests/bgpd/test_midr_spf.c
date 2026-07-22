// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR shortest path first computation tests
 *
 * Copyright (C) 2026
 */

#include <zebra.h>

#include "lib/link_state.h"
#include "lib/linklist.h"
#include "lib/privs.h"

#include "bgpd/bgp_midr_spf.h"

struct zebra_privs_t bgpd_privs = {};

static struct ls_ted *test_ted_new(void)
{
	struct ls_ted *ted = ls_ted_new(1, "MIDR SPF test", 65000);

	assert(ted);
	return ted;
}

static struct ls_vertex *test_vertex_add(struct ls_ted *ted, uint32_t id)
{
	struct ls_node_id node_id = {
		.origin = OSPFv2,
		.id.ip.addr.s_addr = htonl(id),
	};
	struct in_addr router_id = {
		.s_addr = htonl(id),
	};
	struct ls_node *node;
	struct ls_vertex *vertex;

	node = ls_node_new(node_id, router_id, in6addr_any);
	assert(node);
	vertex = ls_vertex_add(ted, node);
	assert(vertex);

	return vertex;
}

static struct ls_edge *test_edge_add(struct ls_ted *ted,
				     struct ls_vertex *source,
				     struct ls_vertex *destination,
				     uint32_t link_id, uint32_t metric)
{
	struct ls_attributes *attributes;
	struct in_addr no_address = {};
	struct ls_edge *edge;

	attributes = ls_attributes_new(source->node->adv, no_address,
				       in6addr_any, link_id);
	assert(attributes);
	attributes->metric = metric;
	SET_FLAG(attributes->flags, LS_ATTR_METRIC);

	edge = ls_edge_add(ted, attributes);
	assert(edge);
	ls_connect_vertices(source, destination, edge);

	return edge;
}

static const struct ls_edge *test_list_edge(const struct list *list,
					    unsigned int index)
{
	const struct listnode *node = listhead(list);

	while (node && index--)
		node = listnextnode(node);

	return node ? listgetdata(node) : NULL;
}

static void test_shortest_path_and_unreachable(void)
{
	struct midr_spf_path *path = NULL;
	struct midr_spf_tree *tree = NULL;
	const struct midr_spf_vertex_result *result;
	const struct list *edges;
	struct ls_vertex *a;
	struct ls_vertex *b;
	struct ls_vertex *c;
	struct ls_vertex *d;
	struct ls_vertex *invalid_destination;
	struct ls_edge *a_b;
	struct ls_edge *b_c;
	struct ls_edge *invalid;
	struct ls_ted *ted = test_ted_new();

	a = test_vertex_add(ted, 0x0a000001);
	b = test_vertex_add(ted, 0x0a000002);
	c = test_vertex_add(ted, 0x0a000003);
	d = test_vertex_add(ted, 0x0a000004);
	invalid_destination = test_vertex_add(ted, 0x0a000005);

	a_b = test_edge_add(ted, a, b, 1, 2);
	b_c = test_edge_add(ted, b, c, 2, 3);
	test_edge_add(ted, a, c, 3, 10);
	invalid = test_edge_add(ted, a, invalid_destination, 4, 1);
	ls_disconnect(invalid_destination, invalid, false);

	assert(midr_dijkstra(ted, a, NULL, NULL, &tree) == 0);
	assert(tree);

	result = midr_spf_tree_lookup(tree, a->key);
	assert(result && result->reachable && result->distance == 0);
	assert(listcount(result->first_hops) == 0);

	result = midr_spf_tree_lookup(tree, c->key);
	assert(result && result->reachable && result->distance == 5);
	assert(listcount(result->first_hops) == 1);
	assert(test_list_edge(result->first_hops, 0) == a_b);

	result = midr_spf_tree_lookup(tree, d->key);
	assert(result && !result->reachable);
	assert(result->distance == UINT64_MAX);
	assert(listcount(result->first_hops) == 0);
	assert(midr_spf_path_build(tree, d->key, &path) == -ENOENT);
	assert(!path);

	result = midr_spf_tree_lookup(tree, invalid_destination->key);
	assert(result && !result->reachable);

	assert(midr_spf_path_build(tree, c->key, &path) == 0);
	assert(path);
	assert(midr_spf_path_distance(path) == 5);
	edges = midr_spf_path_edges(path);
	assert(listcount(edges) == 2);
	assert(test_list_edge(edges, 0) == a_b);
	assert(test_list_edge(edges, 1) == b_c);

	midr_spf_path_free(path);
	midr_spf_tree_free(tree);
	ls_connect_vertices(a, invalid_destination, invalid);
	ls_ted_del_all(&ted);
}

static void test_zero_cost_ecmp(void)
{
	struct midr_spf_path *path = NULL;
	struct midr_spf_tree *tree = NULL;
	const struct midr_spf_vertex_result *result;
	const struct list *edges;
	struct ls_vertex *a;
	struct ls_vertex *b;
	struct ls_vertex *c;
	struct ls_vertex *d;
	struct ls_edge *a_b;
	struct ls_edge *a_c;
	struct ls_edge *b_d;
	struct ls_ted *ted = test_ted_new();

	a = test_vertex_add(ted, 0x0a000001);
	b = test_vertex_add(ted, 0x0a000002);
	c = test_vertex_add(ted, 0x0a000003);
	d = test_vertex_add(ted, 0x0a000004);

	a_b = test_edge_add(ted, a, b, 1, 0);
	a_c = test_edge_add(ted, a, c, 2, 0);
	b_d = test_edge_add(ted, b, d, 3, 0);
	test_edge_add(ted, c, d, 4, 0);

	assert(midr_dijkstra(ted, a, NULL, NULL, &tree) == 0);
	result = midr_spf_tree_lookup(tree, d->key);
	assert(result && result->reachable && result->distance == 0);
	assert(listcount(result->first_hops) == 2);
	assert(test_list_edge(result->first_hops, 0) == a_b);
	assert(test_list_edge(result->first_hops, 1) == a_c);

	assert(midr_spf_path_build(tree, d->key, &path) == 0);
	edges = midr_spf_path_edges(path);
	assert(listcount(edges) == 2);
	assert(test_list_edge(edges, 0) == a_b);
	assert(test_list_edge(edges, 1) == b_d);

	midr_spf_path_free(path);
	midr_spf_tree_free(tree);
	ls_ted_del_all(&ted);
}

struct overflow_cost_context {
	const struct ls_edge *large;
	const struct ls_edge *overflow;
};

static int test_overflow_cost(const struct ls_edge *edge, uint64_t *cost,
			      void *arg)
{
	const struct overflow_cost_context *context = arg;

	if (edge == context->large) {
		*cost = UINT64_MAX;
		return 0;
	}
	if (edge == context->overflow) {
		*cost = 1;
		return 0;
	}

	return -ENOENT;
}

static void test_cost_overflow(void)
{
	struct midr_spf_ops ops = {
		.edge_cost = test_overflow_cost,
	};
	struct overflow_cost_context context;
	struct midr_spf_tree *tree = NULL;
	const struct midr_spf_vertex_result *result;
	struct ls_vertex *a;
	struct ls_vertex *b;
	struct ls_vertex *c;
	struct ls_ted *ted = test_ted_new();

	a = test_vertex_add(ted, 0x0a000001);
	b = test_vertex_add(ted, 0x0a000002);
	c = test_vertex_add(ted, 0x0a000003);
	context.large = test_edge_add(ted, a, b, 1, 1);
	context.overflow = test_edge_add(ted, b, c, 2, 1);

	assert(midr_dijkstra(ted, a, &ops, &context, &tree) == 0);
	result = midr_spf_tree_lookup(tree, b->key);
	assert(result && result->reachable);
	assert(result->distance == UINT64_MAX);
	result = midr_spf_tree_lookup(tree, c->key);
	assert(result && !result->reachable);

	midr_spf_tree_free(tree);
	ls_ted_del_all(&ted);
}

int main(void)
{
	test_shortest_path_and_unreachable();
	test_zero_cost_ecmp();
	test_cost_overflow();
	printf("MIDR SPF tests passed\n");

	return 0;
}
