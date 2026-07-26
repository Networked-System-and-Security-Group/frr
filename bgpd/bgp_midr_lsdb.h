// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR selected-object database and production TED derivation.
 */

#ifndef _FRR_BGP_MIDR_LSDB_H
#define _FRR_BGP_MIDR_LSDB_H

#include <zebra.h>

#include "bgpd/bgp_midr.h"

struct bgp_dest;
struct bgp_path_info;
struct midr_context;
struct peer;
struct vty;

enum midr_lsdb_scope {
	MIDR_LSDB_SCOPE_LOCAL_ONLY,
	MIDR_LSDB_SCOPE_INTRA_GROUP,
	MIDR_LSDB_SCOPE_GLOBAL,
};

enum midr_lsdb_pending_reason {
	MIDR_LSDB_PENDING_NONE,
	MIDR_LSDB_PENDING_LOCAL_MEMBERSHIP,
	MIDR_LSDB_PENDING_REMOTE_MEMBERSHIP,
	MIDR_LSDB_PENDING_GROUP_MEMBERSHIP,
	MIDR_LSDB_PENDING_SCOPE_INELIGIBLE,
};

struct midr_lsdb_summary {
	uint64_t generation;
	size_t object_count;
	size_t usable_count;
	size_t pending_count;
	size_t membership_count;
	size_t link_count;
	size_t node_prefix_count;
	size_t group_prefix_count;
	size_t dirty_count;
	uint64_t commit_count;
	uint64_t failure_count;
	int last_error;
};

extern int midr_lsdb_init(struct midr_context *ctx);
extern void midr_lsdb_finish(struct midr_context *ctx);
extern void midr_lsdb_route_changed(struct midr_context *ctx,
				    struct bgp_dest *dest,
				    struct bgp_path_info *old_selected,
				    struct bgp_path_info *new_selected);
extern void midr_lsdb_local_metadata_changed(struct midr_context *ctx);
extern void midr_lsdb_input_state_changed(struct midr_context *ctx);
extern void midr_lsdb_sync_changed(struct midr_context *ctx);
extern bool midr_lsdb_export_eligible(struct midr_context *ctx, const struct bgp_dest *dest,
				      const struct bgp_path_info *path, const struct peer *target);

extern int midr_lsdb_remote_snapshot_get(
	struct midr_context *ctx, struct midr_remote_view_snapshot *snapshot);
extern void midr_lsdb_remote_snapshot_release(
	struct midr_remote_view_snapshot *snapshot);
extern int midr_lsdb_summary_get(struct midr_context *ctx,
				 struct midr_lsdb_summary *summary);
extern void midr_show_lsdb(struct vty *vty, struct midr_context *ctx);

extern int midr_lsdb_test_process(struct midr_context *ctx);
extern void midr_lsdb_test_fail_next_prepare(struct midr_context *ctx);

#endif /* _FRR_BGP_MIDR_LSDB_H */
