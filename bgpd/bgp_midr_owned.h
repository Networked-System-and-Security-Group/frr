// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Locally originated MIDR Membership and Link objects.
 */

#ifndef _FRR_BGP_MIDR_OWNED_H
#define _FRR_BGP_MIDR_OWNED_H

#include <zebra.h>

#include "bgpd/bgp_midr.h"
#include "bgpd/bgp_midr_ls.h"
#include "bgpd/bgp_midr_sequence.h"

struct midr_context;
struct vty;

#define MIDR_GROUP_PREFIX_TAKEOVER_DELAY_DEFAULT_MSEC 3000U
#define MIDR_GROUP_PREFIX_TAKEOVER_DELAY_MAX_MSEC 600000U

struct midr_owned_summary {
	bool ready;
	uint32_t owner_node_id;
	size_t membership_count;
	size_t link_count;
	size_t node_prefix_count;
	size_t group_prefix_count;
	size_t suppressed_link_count;
	size_t pending_timer_count;
	uint32_t representative_group_id;
	uint32_t representative_candidate;
	uint32_t takeover_delay_msec;
	bool representative_committed;
	bool takeover_timer_pending;
	uint64_t sequence_failures;
	uint64_t fightbacks;
};

extern int midr_owned_init(struct midr_context *ctx);
extern void midr_owned_finish(struct midr_context *ctx);
extern void midr_owned_reconcile(struct midr_context *ctx);
extern void midr_owned_prefix_reconcile(struct midr_context *ctx);
extern void midr_owned_group_reconcile(struct midr_context *ctx);
extern void midr_owned_input_state_changed(struct midr_context *ctx);
extern void midr_owned_identity_withdraw(struct midr_context *ctx);
extern void midr_owned_identity_start(struct midr_context *ctx, uint32_t node_id);

extern int midr_owned_observe_self_sequence(struct midr_context *ctx,
					    const struct midr_ls_object *object);
extern int midr_owned_link_metadata_get(struct midr_context *ctx,
					const struct midr_ls_object_key *key,
					ifindex_t *local_ifindex);
extern int midr_owned_summary_get(struct midr_context *ctx,
				  struct midr_owned_summary *summary);
extern void midr_show_owned(struct vty *vty, struct midr_context *ctx);
extern int midr_owned_takeover_delay_set(struct midr_context *ctx, uint32_t delay_msec);
extern int midr_owned_takeover_delay_get(struct midr_context *ctx, uint32_t *delay_msec);

extern int midr_owned_test_set_sequence_store(
	struct midr_context *ctx, const struct midr_sequence_store_ops *ops,
	void *arg);
extern void midr_owned_test_fire_timers(struct midr_context *ctx);
extern void midr_owned_test_fire_takeover(struct midr_context *ctx);

#endif /* _FRR_BGP_MIDR_OWNED_H */
