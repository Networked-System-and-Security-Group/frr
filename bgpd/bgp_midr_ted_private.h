// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Private MIDR TED builder and lifecycle interfaces.
 */

#ifndef _FRR_BGP_MIDR_TED_PRIVATE_H
#define _FRR_BGP_MIDR_TED_PRIVATE_H

#include "bgpd/bgp_midr_private.h"
#include "bgpd/bgp_midr_ted.h"

struct midr_ted_builder;

struct midr_ted_link_input {
	uint32_t local_node_id;
	uint32_t remote_node_id;
	uint64_t link_id;
	uint32_t canonical_cost;
	uint32_t available_bandwidth_kbps;
	uint64_t policy_tags;
	struct ipaddr link_local_address;
	struct ipaddr link_remote_address;
	ifindex_t local_ifindex;
};

struct midr_ted_status {
	bool ready;
	uint64_t generation;
	uint64_t sync_reason_flags;
	size_t consumer_count;
	size_t pending_link_count;
	size_t pending_node_prefix_count;
	size_t pending_prefix_group_count;
};

extern int midr_ted_context_init(struct midr_context *ctx);
extern void midr_ted_context_finish(struct midr_context *ctx);

extern int midr_ted_builder_create(uint32_t local_node_id, uint32_t local_group_id,
				   struct midr_ted_builder **out);
extern void midr_ted_builder_destroy(struct midr_ted_builder **builder);
extern int midr_ted_builder_add_node(struct midr_ted_builder *builder,
				     const struct midr_ted_node *node);
extern int midr_ted_builder_add_link(struct midr_ted_builder *builder,
				     const struct midr_ted_link_input *link);
extern int midr_ted_builder_add_node_prefix(struct midr_ted_builder *builder,
					    const struct midr_ted_node_prefix *prefix);
extern int midr_ted_builder_add_prefix_group(struct midr_ted_builder *builder,
					     const struct midr_ted_prefix_group *prefix);
extern int midr_ted_builder_publish(struct midr_context *ctx,
				    const struct midr_ted_builder *builder,
				    uint64_t sync_reason_flags);

extern int midr_ted_status_get(struct midr_context *ctx, struct midr_ted_status *status);

/* Unit-test hook for the otherwise unreachable generation wrap boundary. */
extern int midr_ted_test_generation_set(struct midr_context *ctx, uint64_t generation);

#endif /* _FRR_BGP_MIDR_TED_PRIVATE_H */
