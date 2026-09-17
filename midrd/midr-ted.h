/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MIDRD_TED_H
#define MIDRD_TED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "midr-consumer.h"
#include "midr-lsdb.h"

enum midr_ted_state {
	MIDR_TED_NOT_READY = 0,
	MIDR_TED_READY = 1,
};

struct midr_ted_config {
	size_t max_events;
	uint32_t (*local_ifindex_lookup)(void *arg, uint32_t local_node_id,
					 uint32_t remote_node_id,
					 uint64_t link_id);
	void *local_ifindex_arg;
};

struct midr_ted_prefix_key {
	uint8_t family;
	uint8_t prefix_len;
	uint8_t reserved[2];
	uint8_t prefix[MIDR_CORE_ADDR_BYTES];
};

struct midr_ted_node {
	uint32_t node_id;
	uint32_t group_id;
};

struct midr_ted_link {
	uint32_t local_node_id;
	uint32_t remote_node_id;
	uint32_t local_group_id;
	uint32_t remote_group_id;
	uint64_t link_id;
	uint32_t canonical_cost;
	uint32_t local_ifindex;
	uint8_t family;
	uint8_t reserved[3];
	uint8_t local_address[MIDR_CORE_ADDR_BYTES];
	uint8_t remote_address[MIDR_CORE_ADDR_BYTES];
};

struct midr_ted_node_prefix {
	struct midr_ted_prefix_key key;
	uint32_t node_id;
};

struct midr_ted_group_edge {
	uint32_t source_group_id;
	uint32_t target_group_id;
	uint64_t aggregate_cost;
};

struct midr_ted_prefix_group {
	struct midr_ted_prefix_key key;
	uint32_t group_id;
};

struct midr_ted_view {
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
};

struct midr_ted;
struct midr_ted_stage;
struct midr_ted_consumer;

enum midr_ted_change_flags {
	MIDR_TED_CHANGE_NONE = 0,
	MIDR_TED_CHANGE_LOCAL = 1U << 0,
	MIDR_TED_CHANGE_NODES = 1U << 1,
	MIDR_TED_CHANGE_LINKS = 1U << 2,
	MIDR_TED_CHANGE_GROUP_EDGES = 1U << 3,
	MIDR_TED_CHANGE_PREFIXES = 1U << 4,
	MIDR_TED_CHANGE_SYNC = 1U << 5,
	MIDR_TED_CHANGE_ALL = (1U << 6) - 1U,
};

struct midr_ted_consumer_ops {
	void (*snapshot_changed)(struct midr_ted *ted, uint64_t generation,
					 uint32_t change_flags, void *arg);
};

int midr_ted_create(const struct midr_ted_config *config,
		    struct midr_ted **out);
void midr_ted_destroy(struct midr_ted **ted);

/* Build a complete candidate view and publish it with one pointer swap. */
int midr_ted_apply_snapshot(struct midr_ted *ted,
			    const struct midr_consumer_snapshot *snapshot);
int midr_ted_prepare_snapshot(struct midr_ted *ted,
			      const struct midr_consumer_snapshot *snapshot,
			      struct midr_ted_stage **stage);
int midr_ted_prepare_lsdb(struct midr_ted *ted, uint32_t local_node_id,
			  const struct midr_lsdb_stage *lsdb_stage,
			  const struct midr_consumer_snapshot *snapshot,
			  bool local_metadata_update,
			  struct midr_ted_stage **stage);
void midr_ted_commit_prepared(struct midr_ted *ted,
			      struct midr_ted_stage **stage);
void midr_ted_abort_prepared(struct midr_ted_stage **stage);

/* Hide the active view after an upstream staging failure. */
int midr_ted_invalidate(struct midr_ted *ted, int error);

/* Returns -EAGAIN while the current derived view is not usable. */
int midr_ted_snapshot_acquire(const struct midr_ted *ted,
			      struct midr_consumer_snapshot *snapshot);
int midr_ted_view_acquire(const struct midr_ted *ted,
			  struct midr_ted_view *view);
void midr_ted_view_release(struct midr_ted_view *view);

bool midr_ted_generation_is_current(const struct midr_ted *ted,
				    uint64_t generation);
int midr_ted_consumer_register(struct midr_ted *ted,
				       const struct midr_ted_consumer_ops *ops,
				       void *arg,
				       struct midr_ted_consumer **consumer);
void midr_ted_consumer_unregister(struct midr_ted *ted,
					  struct midr_ted_consumer **consumer);

enum midr_ted_state midr_ted_state(const struct midr_ted *ted);
uint64_t midr_ted_generation(const struct midr_ted *ted);
uint64_t midr_ted_source_generation(const struct midr_ted *ted);
int midr_ted_last_error(const struct midr_ted *ted);

/* Test-only fault injection.  The next apply fails and consumes the error. */
int midr_ted_test_fail_next(struct midr_ted *ted, int error);

#endif /* MIDRD_TED_H */
