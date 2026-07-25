// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Private MIDR declarations used by bgpd, VTY, and unit tests.
 */

#ifndef _FRR_BGP_MIDR_PRIVATE_H
#define _FRR_BGP_MIDR_PRIVATE_H

#include "bgpd/bgp_midr.h"
struct bgp;
struct bgp_midr;
struct midr_input_store;
struct midr_ted_store;
struct vty;

struct midr_context {
	struct bgp *bgp;
	struct bgp_midr *midr;
	struct midr_input_store *input_store;
	struct midr_ted_store *ted_store;
};

struct bgp_midr {
	struct bgp *bgp;
	struct midr_context ctx;
	uint64_t peer_hook_events;
	uint64_t route_hook_events;
	struct midr_remote_view_callbacks remote_callbacks;
	bool remote_callbacks_registered;
};

enum midr_input_state {
	MIDR_INPUT_NORMAL,
	MIDR_INPUT_RESYNCING,
	MIDR_INPUT_OUT_OF_SYNC,
	MIDR_INPUT_IDENTITY_RESTART,
};

struct midr_input_status {
	enum midr_input_state state;
	enum midr_topology_resync_reason reason;
	uint32_t owner_node_id;
	bool provider_available;
	uint64_t snapshot_version;
	size_t normal_queue_count;
	size_t resync_queue_count;
	size_t normal_queue_limit;
	size_t resync_queue_limit;
	size_t active_node_count;
	size_t active_link_count;
	size_t node_tombstone_count;
	size_t link_tombstone_count;
	uint64_t event_enqueued;
	uint64_t event_processed;
	uint64_t event_ignored_old;
	uint64_t event_rejected_full;
	uint64_t event_rejected_sync;
	uint64_t event_dropped_resync;
	uint64_t resync_attempts;
	uint64_t resync_commits;
	uint64_t resync_failures;
	uint64_t queue_overflows;
	uint64_t identity_restarts;
};

extern int midr_validate_node_update(uint32_t local_node_id, const struct midr_node_update *node);
extern int midr_validate_node_withdraw(uint32_t local_node_id, uint32_t node_id);
extern int midr_validate_link_update(uint32_t local_node_id, const struct midr_link_update *link);
extern int midr_validate_link_withdraw(uint32_t local_node_id, const struct midr_link_key *key);

extern int midr_input_init(struct midr_context *ctx);
extern void midr_input_finish(struct midr_context *ctx);
extern int midr_input_router_id_update(struct bgp *bgp, bool withdraw);
extern int midr_input_status_get(struct midr_context *ctx, struct midr_input_status *status);
extern int midr_local_fact_node_get(struct midr_context *ctx, uint32_t node_id,
				    struct midr_node_update *node, bool *active);
extern int midr_local_fact_link_get(struct midr_context *ctx, const struct midr_link_key *key,
				    struct midr_link_update *link, bool *active);
extern int midr_input_test_set_queue_limits(struct midr_context *ctx, size_t normal_limit,
					    size_t resync_limit);
extern void midr_input_test_resync_now(struct midr_context *ctx);

extern void bgp_midr_init(struct bgp *bgp);
extern void bgp_midr_finish(struct bgp *bgp);

extern void midr_topology_process_pending(struct midr_context *ctx);
extern void midr_show_topology_nodes(struct vty *vty, struct midr_context *ctx);
extern void midr_show_topology_links(struct vty *vty, struct midr_context *ctx);
extern void midr_show_topology_tombstones(struct vty *vty, struct midr_context *ctx);
extern void midr_show_topology_sync(struct vty *vty, struct midr_context *ctx);
extern void midr_show_events(struct vty *vty, struct midr_context *ctx);

#endif /* _FRR_BGP_MIDR_PRIVATE_H */
