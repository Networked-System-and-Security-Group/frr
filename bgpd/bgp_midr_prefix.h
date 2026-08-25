// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR local prefix contributor input.
 */

#ifndef _FRR_BGP_MIDR_PREFIX_H
#define _FRR_BGP_MIDR_PREFIX_H

#include "prefix.h"

struct bgp;
struct bgp_dest;
struct bgp_path_info;
struct midr_context;
struct vty;

#define MIDR_PREFIX_MAX_AS_PATH_LENGTH_DEFAULT 1U

enum midr_prefix_state {
	MIDR_PREFIX_NOT_READY,
	MIDR_PREFIX_SCANNING,
	MIDR_PREFIX_READY,
	MIDR_PREFIX_OUT_OF_SYNC,
};

struct midr_prefix_status {
	enum midr_prefix_state state;
	uint64_t generation;
	size_t contributor_count;
	uint64_t scans;
	uint64_t route_events;
	uint64_t policy_rechecks;
	uint64_t rejected_as_path;
};

typedef int (*midr_prefix_contributor_cb)(const struct prefix *prefix, void *arg);

extern int midr_prefix_init(struct midr_context *ctx);
extern void midr_prefix_finish(struct midr_context *ctx);
extern void midr_prefix_identity_withdraw(struct midr_context *ctx);
extern void midr_prefix_identity_start(struct midr_context *ctx);
extern void midr_prefix_mark_out_of_sync(struct midr_context *ctx);
extern void midr_prefix_route_changed(struct midr_context *ctx, afi_t afi, safi_t safi,
				      struct bgp_dest *dest, struct bgp_path_info *old_route,
				      struct bgp_path_info *new_route);
extern void midr_prefix_route_map_changed(struct bgp *bgp, const char *route_map_name);
extern int midr_prefix_rescan(struct midr_context *ctx);

extern int midr_prefix_route_map_set(struct midr_context *ctx, afi_t afi, const char *name);
extern int midr_prefix_route_map_unset(struct midr_context *ctx, afi_t afi);
extern const char *midr_prefix_route_map_name(struct midr_context *ctx, afi_t afi);
extern int midr_prefix_max_as_path_length_set(struct midr_context *ctx,
					      afi_t afi, uint32_t length);
extern int midr_prefix_max_as_path_length_unset(struct midr_context *ctx,
						afi_t afi);
extern uint32_t midr_prefix_max_as_path_length(struct midr_context *ctx,
					       afi_t afi);

extern int midr_prefix_status_get(struct midr_context *ctx, struct midr_prefix_status *status);
extern int midr_prefix_contributor_foreach(struct midr_context *ctx, midr_prefix_contributor_cb cb,
					   void *arg);
extern bool midr_prefix_is_ready(struct midr_context *ctx);
extern void midr_show_prefix_summary(struct vty *vty, struct midr_context *ctx);
extern void midr_show_prefix_contributors(struct vty *vty, struct midr_context *ctx);
extern void midr_prefix_config_write_family(struct vty *vty, struct bgp *bgp, afi_t afi,
					    safi_t safi);

#endif /* _FRR_BGP_MIDR_PREFIX_H */
