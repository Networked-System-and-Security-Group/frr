// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR shortest path first computation
 *
 * Copyright (C) 2026
 */

#ifndef _FRR_BGP_MIDR_SPF_H
#define _FRR_BGP_MIDR_SPF_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct list;
struct ls_edge;
struct ls_ted;
struct ls_vertex;

/*
 * Optional policy hooks used while computing a shortest-path tree.  The
 * built-in validity checks always run first; these callbacks may impose
 * additional MIDR policy and topology-view restrictions.
 */
struct midr_spf_ops {
	bool (*vertex_allowed)(const struct ls_vertex *vertex, void *arg);
	bool (*edge_allowed)(const struct ls_edge *edge, void *arg);
	int (*edge_cost)(const struct ls_edge *edge, uint64_t *cost, void *arg);
};

/*
 * Result for one destination vertex.  The object and first_hops list are
 * owned by the containing SPF tree and remain valid only until that tree is
 * freed.  The list elements are borrowed const struct ls_edge pointers owned
 * by the TED.
 */
struct midr_spf_vertex_result {
	uint64_t vertex_key;
	uint64_t distance;
	bool reachable;
	const struct list *first_hops;
};

struct midr_spf_tree;
struct midr_spf_path;

/*
 * Compute a single-source shortest-path tree without modifying the TED.
 *
 * Returns 0 on success or a negative errno value for invalid input.  An
 * unreachable destination is represented by its vertex result and is not a
 * global computation failure.
 */
extern int midr_dijkstra(const struct ls_ted *ted,
			 const struct ls_vertex *source,
			 const struct midr_spf_ops *ops, void *ops_arg,
			 struct midr_spf_tree **result);

/* Look up the result for a TED vertex key.  Returns NULL for an unknown key. */
extern const struct midr_spf_vertex_result *
midr_spf_tree_lookup(const struct midr_spf_tree *tree,
		     uint64_t destination_key);

extern void midr_spf_tree_free(struct midr_spf_tree *tree);

/*
 * Build one deterministic representative path from the source to a
 * destination.  ECMP is reported separately through first_hops; this function
 * deliberately returns only one edge sequence.
 */
extern int midr_spf_path_build(const struct midr_spf_tree *tree,
			       uint64_t destination_key,
			       struct midr_spf_path **result);

extern uint64_t midr_spf_path_distance(const struct midr_spf_path *path);
extern const struct list *midr_spf_path_edges(const struct midr_spf_path *path);
extern void midr_spf_path_free(struct midr_spf_path *path);

#ifdef __cplusplus
}
#endif

#endif /* _FRR_BGP_MIDR_SPF_H */
