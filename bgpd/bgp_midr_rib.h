// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR SAFI RIB identity and path selection.
 */

#ifndef _FRR_BGP_MIDR_RIB_H
#define _FRR_BGP_MIDR_RIB_H

#include <zebra.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bgpd/bgp_midr_codec.h"
#include "bgpd/bgp_midr_instance.h"

#define MIDR_RIB_MAX_IDENTITIES 65536U

struct bgp;
struct bgp_dest;
struct bgp_path_info;
struct midr_context;
struct midr_instance_ref;
struct peer;
struct vty;

enum midr_rib_identity_state {
	MIDR_RIB_IDENTITY_NO_PATH,
	MIDR_RIB_IDENTITY_SELECTED,
	MIDR_RIB_IDENTITY_CONFLICT_QUARANTINED,
};

struct midr_rib_summary {
	size_t identity_count;
	size_t path_count;
	size_t selected_count;
	size_t conflict_count;
	size_t identity_limit;
	uint64_t rejected_limit;
	uint64_t rejected_payload_conflict;
	uint64_t rejected_resource;
	uint64_t rejected_internal;
};

typedef int (*midr_rib_selected_cb)(
	const struct midr_instance *instance, struct peer *peer, void *arg);
typedef int (*midr_rib_selected_entry_cb)(
	const struct midr_instance *instance, struct peer *peer,
	struct bgp_dest *dest, struct bgp_path_info *selected, void *arg);

extern int midr_rib_init(struct midr_context *ctx);
extern void midr_rib_finish(struct midr_context *ctx);

/* Production MIDR input: one canonical instance per object identity. */
extern int midr_rib_instance_upsert(
	struct midr_context *ctx, struct peer *peer,
	const struct midr_instance *instance, uint32_t age_ms);
extern int midr_rib_instance_upsert_received(
	struct midr_context *ctx, struct peer *peer,
	const struct midr_instance *instance, uint32_t age_ms,
	uint64_t received_ns);
extern int midr_rib_peer_withdraw(
	struct midr_context *ctx, struct peer *peer,
	const struct midr_ls_object_key *key);
extern bool midr_rib_peer_advertisement_has(
	struct midr_context *ctx, const struct midr_ls_object_key *key,
	const struct peer *peer);
extern bool midr_rib_peer_advertisement_current(
	struct midr_context *ctx, const struct midr_instance *instance,
	const struct peer *peer);
extern int midr_rib_path_instance(
	struct midr_context *ctx, const struct bgp_dest *dest,
	const struct bgp_path_info *path, struct midr_instance *instance,
	uint32_t *age_ms);
extern int midr_rib_path_instance_ref(
	struct midr_context *ctx, const struct bgp_dest *dest,
	const struct bgp_path_info *path,
	const struct midr_instance_ref **instance_ref);
extern uint64_t midr_rib_now_ns(struct midr_context *ctx);
extern uint32_t midr_rib_max_age_ms(struct midr_context *ctx);
extern int midr_rib_instance_ref_age_at(
	struct midr_context *ctx, const struct midr_instance_ref *instance_ref,
	uint64_t now_ns, uint32_t budget_ms, uint32_t *age_ms);
extern int midr_rib_selected_instance_get(
	struct midr_context *ctx, const struct midr_ls_object_key *key,
	struct midr_instance *instance, uint32_t *age_ms,
	struct peer **peer);

extern void bgp_midr_rib_process_main(struct bgp *bgp,
				      struct bgp_dest *dest);
extern void bgp_midr_rib_route_update_notify(
	struct bgp *bgp, struct bgp_dest *dest,
	struct bgp_path_info *old_selected,
	struct bgp_path_info *new_selected);
extern void midr_rib_dest_cleanup(struct bgp *bgp,
				  struct bgp_dest *dest);

extern const struct midr_ls_object_key *
midr_rib_dest_key(const struct bgp_dest *dest);
extern int midr_rib_path_object(const struct bgp_dest *dest,
				const struct bgp_path_info *path,
				struct midr_ls_object *object);
extern int midr_rib_selected_foreach(struct midr_context *ctx,
				     midr_rib_selected_cb callback,
				     void *arg);
extern int midr_rib_selected_entry_foreach(
	struct midr_context *ctx, midr_rib_selected_entry_cb callback,
	void *arg);
extern int midr_rib_summary_get(struct midr_context *ctx,
				struct midr_rib_summary *summary);
extern void midr_show_rib_paths(struct vty *vty, struct midr_context *ctx);

extern int midr_rib_test_set_identity_limit(struct midr_context *ctx,
					     size_t limit);
extern int midr_rib_test_set_now_ns(struct midr_context *ctx,
				     uint64_t now_ns);
extern int midr_rib_test_set_path_stale(
	struct midr_context *ctx, const struct midr_ls_object_key *key,
	struct peer *peer, bool stale);

#endif /* _FRR_BGP_MIDR_RIB_H */
