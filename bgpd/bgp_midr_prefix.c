// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR local prefix contributor input.
 */

#include <zebra.h>

#include <errno.h>

#include "command.h"
#include "hash.h"
#include "memory.h"
#include "prefix.h"
#include "routemap.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_attr.h"
#include "bgpd/bgp_midr_prefix.h"
#include "bgpd/bgp_midr_private.h"
#include "bgpd/bgp_midr_owned.h"
#include "bgpd/bgp_midr_lsdb.h"
#include "bgpd/bgp_route.h"
#include "bgpd/bgp_table.h"

DEFINE_MTYPE_STATIC(BGPD, MIDR_PREFIX_STORE, "MIDR prefix store");
DEFINE_MTYPE_STATIC(BGPD, MIDR_PREFIX_ENTRY, "MIDR prefix entry");
DEFINE_MTYPE_STATIC(BGPD, MIDR_PREFIX_CONFIG, "MIDR prefix configuration");

struct midr_prefix_entry {
	struct prefix prefix;
	size_t eligible_path_count;
};

struct midr_prefix_family {
	char *route_map_name;
	uint32_t local_sources;
};

struct midr_prefix_store {
	struct midr_context *ctx;
	struct hash *contributors;
	struct hash *dirty;
	struct midr_prefix_family family[AFI_MAX];
	enum midr_prefix_state state;
	uint64_t generation;
	uint64_t scans;
	uint64_t route_events;
	uint64_t policy_rechecks;
	uint64_t rejected_midr;
};

static bool midr_prefix_afi_valid(afi_t afi)
{
	return afi == AFI_IP || afi == AFI_IP6;
}

static unsigned int midr_prefix_hash_key(const void *arg)
{
	const struct midr_prefix_entry *entry = arg;

	return prefix_hash_key(&entry->prefix);
}

static bool midr_prefix_hash_cmp(const void *a, const void *b)
{
	const struct midr_prefix_entry *left = a;
	const struct midr_prefix_entry *right = b;

	return prefix_same(&left->prefix, &right->prefix);
}

static void *midr_prefix_hash_alloc(void *arg)
{
	const struct midr_prefix_entry *source = arg;
	struct midr_prefix_entry *entry;

	entry = XCALLOC(MTYPE_MIDR_PREFIX_ENTRY, sizeof(*entry));
	prefix_copy(&entry->prefix, &source->prefix);
	entry->eligible_path_count = source->eligible_path_count;
	return entry;
}

static void midr_prefix_entry_free(void *arg)
{
	XFREE(MTYPE_MIDR_PREFIX_ENTRY, arg);
}

static struct hash *midr_prefix_table_new(const char *name)
{
	return hash_create(midr_prefix_hash_key, midr_prefix_hash_cmp, name);
}

static void midr_prefix_key_init(struct midr_prefix_entry *key, const struct prefix *prefix)
{
	memset(key, 0, sizeof(*key));
	prefix_copy(&key->prefix, prefix);
	apply_mask(&key->prefix);
}

static struct midr_prefix_entry *midr_prefix_table_lookup(struct hash *table,
							  const struct prefix *prefix)
{
	struct midr_prefix_entry key;

	midr_prefix_key_init(&key, prefix);
	return hash_lookup(table, &key);
}

static void midr_prefix_table_set(struct hash *table, const struct prefix *prefix,
				  size_t eligible_path_count)
{
	struct midr_prefix_entry key;
	struct midr_prefix_entry *entry;

	midr_prefix_key_init(&key, prefix);
	key.eligible_path_count = eligible_path_count;
	entry = hash_get(table, &key, midr_prefix_hash_alloc);
	entry->eligible_path_count = eligible_path_count;
}

static void midr_prefix_table_unset(struct hash *table, const struct prefix *prefix)
{
	struct midr_prefix_entry key;
	struct midr_prefix_entry *entry;

	midr_prefix_key_init(&key, prefix);
	entry = hash_release(table, &key);
	if (entry)
		midr_prefix_entry_free(entry);
}

static bool midr_prefix_tables_same(struct hash *left, struct hash *right)
{
	unsigned int index;
	struct hash_bucket *bucket;

	if (left->count != right->count)
		return false;

	for (index = 0; index < left->size; index++)
		for (bucket = left->index[index]; bucket; bucket = bucket->next) {
			struct midr_prefix_entry *entry = bucket->data;

			if (!midr_prefix_table_lookup(right, &entry->prefix))
				return false;
		}
	return true;
}

static bool midr_prefix_path_is_local_source(const struct midr_prefix_store *store, afi_t afi,
					     const struct bgp_path_info *path)
{
	uint32_t sources = store->family[afi].local_sources;

	if (path->peer != store->ctx->bgp->peer_self)
		return false;
	if (path->type == ZEBRA_ROUTE_BGP && path->sub_type == BGP_ROUTE_STATIC)
		return CHECK_FLAG(sources, MIDR_PREFIX_SOURCE_NETWORK);
	if (path->sub_type != BGP_ROUTE_REDISTRIBUTE)
		return false;
	if (path->type == ZEBRA_ROUTE_CONNECT)
		return CHECK_FLAG(sources, MIDR_PREFIX_SOURCE_CONNECTED);
	if (path->type == ZEBRA_ROUTE_STATIC)
		return CHECK_FLAG(sources, MIDR_PREFIX_SOURCE_STATIC);
	return false;
}

static bool midr_prefix_path_source_allowed(const struct midr_prefix_store *store, afi_t afi,
					    const struct bgp_path_info *path)
{
	if (midr_prefix_path_is_local_source(store, afi, path))
		return true;
	if (!path->peer || path->peer == store->ctx->bgp->peer_self)
		return false;
	return peer_af_flag_check(path->peer, afi, SAFI_UNICAST,
				  PEER_FLAG_MIDR_EXTERNAL_PREFIX_SOURCE);
}

static bool midr_prefix_route_map_permits(const struct midr_prefix_store *store, afi_t afi,
					  struct bgp_dest *dest, struct bgp_path_info *path)
{
	const char *name = store->family[afi].route_map_name;
	const struct prefix *prefix = bgp_dest_get_prefix(dest);
	struct bgp_path_info_extra extra = {};
	struct bgp_path_info candidate = {};
	struct route_map *route_map;
	struct attr attr;
	route_map_result_t result;

	if (!name)
		return false;
	route_map = route_map_lookup_by_name(name);
	if (!route_map)
		return false;

	attr = *path->attr;
	prep_for_rmap_apply(&candidate, &extra, dest, path, path->peer, NULL, &attr);
	result = route_map_apply(route_map, prefix, &candidate);
	bgp_attr_flush(&attr);
	return result == RMAP_PERMITMATCH;
}

static bool midr_prefix_path_eligible(struct midr_prefix_store *store, afi_t afi,
				      struct bgp_dest *dest, struct bgp_path_info *path)
{
	if (path->type == ZEBRA_ROUTE_MIDR) {
		store->rejected_midr++;
		return false;
	}
	if (!midr_prefix_path_source_allowed(store, afi, path))
		return false;
	if (!CHECK_FLAG(path->flags, BGP_PATH_VALID) || CHECK_FLAG(path->flags, BGP_PATH_STALE))
		return false;
	if (!CHECK_FLAG(path->flags, BGP_PATH_SELECTED) &&
	    !CHECK_FLAG(path->flags, BGP_PATH_MULTIPATH))
		return false;
	return midr_prefix_route_map_permits(store, afi, dest, path);
}

static size_t midr_prefix_dest_eligible_count(struct midr_prefix_store *store, afi_t afi,
					      struct bgp_dest *dest)
{
	struct bgp_path_info *path;
	size_t count = 0;

	for (path = bgp_dest_get_bgp_path_info(dest); path; path = path->next)
		if (midr_prefix_path_eligible(store, afi, dest, path))
			count++;
	return count;
}

static void midr_prefix_dirty_add(struct midr_prefix_store *store, const struct prefix *prefix)
{
	struct midr_prefix_entry key;

	midr_prefix_key_init(&key, prefix);
	(void)hash_get(store->dirty, &key, midr_prefix_hash_alloc);
}

static afi_t midr_prefix_family_to_afi(uint8_t family)
{
	if (family == AF_INET)
		return AFI_IP;
	if (family == AF_INET6)
		return AFI_IP6;
	return AFI_UNSPEC;
}

static void midr_prefix_evaluate_dest(struct midr_prefix_store *store, struct hash *table,
				      afi_t afi, struct bgp_dest *dest)
{
	const struct prefix *prefix = bgp_dest_get_prefix(dest);
	size_t count;

	if (!prefix)
		return;
	count = midr_prefix_dest_eligible_count(store, afi, dest);
	if (count)
		midr_prefix_table_set(table, prefix, count);
	else
		midr_prefix_table_unset(table, prefix);
}

struct midr_prefix_dirty_replay {
	struct midr_prefix_store *store;
	struct hash *staging;
};

static void midr_prefix_replay_dirty(struct hash_bucket *bucket, void *arg)
{
	struct midr_prefix_dirty_replay *replay = arg;
	struct midr_prefix_entry *entry = bucket->data;
	afi_t afi = midr_prefix_family_to_afi(entry->prefix.family);
	struct bgp_table *table;
	struct bgp_dest *dest;

	if (!midr_prefix_afi_valid(afi))
		return;
	table = replay->store->ctx->bgp->rib[afi][SAFI_UNICAST];
	if (!table) {
		midr_prefix_table_unset(replay->staging, &entry->prefix);
		return;
	}
	dest = bgp_node_lookup(table, &entry->prefix);
	if (!dest) {
		midr_prefix_table_unset(replay->staging, &entry->prefix);
		return;
	}
	midr_prefix_evaluate_dest(replay->store, replay->staging, afi, dest);
	bgp_dest_unlock_node(dest);
}

int midr_prefix_rescan(struct midr_context *ctx)
{
	struct midr_prefix_dirty_replay replay;
	struct midr_prefix_store *store;
	struct hash *staging;
	struct hash *old;
	enum midr_prefix_state previous_state;
	bool changed;
	afi_t afi;

	if (!ctx || !ctx->prefix_store)
		return -ENOENT;
	store = ctx->prefix_store;
	if (store->state == MIDR_PREFIX_SCANNING)
		return -EBUSY;
	previous_state = store->state;

	staging = midr_prefix_table_new("MIDR prefix scan staging");
	store->state = MIDR_PREFIX_SCANNING;
	hash_clean(store->dirty, midr_prefix_entry_free);
	store->scans++;

	for (afi = AFI_IP; afi < AFI_MAX; afi++) {
		struct bgp_table *table;
		struct bgp_dest *dest;

		if (!midr_prefix_afi_valid(afi))
			continue;
		table = ctx->bgp->rib[afi][SAFI_UNICAST];
		if (!table)
			continue;
		for (dest = bgp_table_top(table); dest; dest = bgp_route_next(dest))
			midr_prefix_evaluate_dest(store, staging, afi, dest);
	}

	replay.store = store;
	replay.staging = staging;
	hash_iterate(store->dirty, midr_prefix_replay_dirty, &replay);
	hash_clean(store->dirty, midr_prefix_entry_free);

	changed = !midr_prefix_tables_same(store->contributors, staging);
	if (changed)
		store->generation++;
	old = store->contributors;
	store->contributors = staging;
	store->state = MIDR_PREFIX_READY;
	hash_clean_and_free(&old, midr_prefix_entry_free);
	if (changed || previous_state != MIDR_PREFIX_READY) {
		midr_owned_prefix_reconcile(ctx);
		midr_lsdb_local_metadata_changed(ctx);
	}
	return 0;
}

void midr_prefix_route_changed(struct midr_context *ctx, afi_t afi, safi_t safi,
			       struct bgp_dest *dest, struct bgp_path_info *old_route,
			       struct bgp_path_info *new_route)
{
	struct midr_prefix_store *store;
	const struct prefix *prefix;
	bool was_present;
	bool is_present;

	(void)old_route;
	(void)new_route;
	if (!ctx || !ctx->prefix_store || safi != SAFI_UNICAST || !midr_prefix_afi_valid(afi))
		return;
	store = ctx->prefix_store;
	store->route_events++;
	if (!dest) {
		midr_prefix_mark_out_of_sync(ctx);
		return;
	}
	prefix = bgp_dest_get_prefix(dest);
	if (!prefix) {
		midr_prefix_mark_out_of_sync(ctx);
		return;
	}

	if (store->state == MIDR_PREFIX_SCANNING) {
		midr_prefix_dirty_add(store, prefix);
		return;
	}
	if (store->state != MIDR_PREFIX_READY)
		return;

	was_present = midr_prefix_table_lookup(store->contributors, prefix) != NULL;
	midr_prefix_evaluate_dest(store, store->contributors, afi, dest);
	is_present = midr_prefix_table_lookup(store->contributors, prefix) != NULL;
	if (was_present != is_present) {
		store->generation++;
		midr_owned_prefix_reconcile(ctx);
	}
}

void midr_prefix_mark_out_of_sync(struct midr_context *ctx)
{
	struct midr_prefix_store *store;
	bool had_contributors;

	if (!ctx || !ctx->prefix_store)
		return;
	store = ctx->prefix_store;
	if (store->state == MIDR_PREFIX_OUT_OF_SYNC)
		return;
	had_contributors = store->contributors->count != 0;
	hash_clean(store->contributors, midr_prefix_entry_free);
	hash_clean(store->dirty, midr_prefix_entry_free);
	store->state = MIDR_PREFIX_OUT_OF_SYNC;
	if (had_contributors)
		store->generation++;
	midr_owned_prefix_reconcile(ctx);
	midr_owned_group_reconcile(ctx);
	midr_lsdb_local_metadata_changed(ctx);
}

static void midr_prefix_schedule_policy_recheck(struct midr_prefix_store *store)
{
	store->policy_rechecks++;
	(void)midr_prefix_rescan(store->ctx);
}

void midr_prefix_route_map_changed(struct bgp *bgp, const char *route_map_name)
{
	struct midr_prefix_store *store;
	afi_t afi;

	if (!bgp || !bgp->midr_info || !route_map_name)
		return;
	store = bgp->midr_info->ctx.prefix_store;
	if (!store)
		return;
	for (afi = AFI_IP; afi < AFI_MAX; afi++) {
		if (!midr_prefix_afi_valid(afi) || !store->family[afi].route_map_name)
			continue;
		if (strcmp(store->family[afi].route_map_name, route_map_name) == 0) {
			midr_prefix_schedule_policy_recheck(store);
			return;
		}
	}
}

int midr_prefix_route_map_set(struct midr_context *ctx, afi_t afi, const char *name)
{
	struct midr_prefix_store *store;

	if (!ctx || !ctx->prefix_store)
		return -ENOENT;
	if (!midr_prefix_afi_valid(afi) || !name || !name[0])
		return -EINVAL;
	store = ctx->prefix_store;
	if (store->family[afi].route_map_name &&
	    strcmp(store->family[afi].route_map_name, name) == 0)
		return 0;
	XFREE(MTYPE_MIDR_PREFIX_CONFIG, store->family[afi].route_map_name);
	store->family[afi].route_map_name = XSTRDUP(MTYPE_MIDR_PREFIX_CONFIG, name);
	midr_prefix_schedule_policy_recheck(store);
	return 0;
}

int midr_prefix_route_map_unset(struct midr_context *ctx, afi_t afi)
{
	struct midr_prefix_store *store;

	if (!ctx || !ctx->prefix_store)
		return -ENOENT;
	if (!midr_prefix_afi_valid(afi))
		return -EINVAL;
	store = ctx->prefix_store;
	if (!store->family[afi].route_map_name)
		return 0;
	XFREE(MTYPE_MIDR_PREFIX_CONFIG, store->family[afi].route_map_name);
	midr_prefix_schedule_policy_recheck(store);
	return 0;
}

const char *midr_prefix_route_map_name(struct midr_context *ctx, afi_t afi)
{
	if (!ctx || !ctx->prefix_store || !midr_prefix_afi_valid(afi))
		return NULL;
	return ctx->prefix_store->family[afi].route_map_name;
}

int midr_prefix_local_source_set(struct midr_context *ctx, afi_t afi, uint32_t source, bool enabled)
{
	struct midr_prefix_store *store;
	uint32_t valid = MIDR_PREFIX_SOURCE_NETWORK | MIDR_PREFIX_SOURCE_CONNECTED |
			 MIDR_PREFIX_SOURCE_STATIC;

	if (!ctx || !ctx->prefix_store)
		return -ENOENT;
	if (!midr_prefix_afi_valid(afi) || !source || (source & ~valid) || (source & (source - 1)))
		return -EINVAL;
	store = ctx->prefix_store;
	if (enabled)
		SET_FLAG(store->family[afi].local_sources, source);
	else
		UNSET_FLAG(store->family[afi].local_sources, source);
	midr_prefix_schedule_policy_recheck(store);
	return 0;
}

uint32_t midr_prefix_local_sources(struct midr_context *ctx, afi_t afi)
{
	if (!ctx || !ctx->prefix_store || !midr_prefix_afi_valid(afi))
		return 0;
	return ctx->prefix_store->family[afi].local_sources;
}

int midr_prefix_external_peer_set(struct midr_context *ctx, struct peer *peer, afi_t afi,
				  bool enabled)
{
	int ret;

	if (!ctx || !ctx->prefix_store)
		return -ENOENT;
	if (!peer || peer->bgp != ctx->bgp || !midr_prefix_afi_valid(afi))
		return -EINVAL;
	ret = enabled ? peer_af_flag_set(peer, afi, SAFI_UNICAST,
					 PEER_FLAG_MIDR_EXTERNAL_PREFIX_SOURCE)
		      : peer_af_flag_unset(peer, afi, SAFI_UNICAST,
					   PEER_FLAG_MIDR_EXTERNAL_PREFIX_SOURCE);
	if (ret)
		return -EINVAL;
	midr_prefix_schedule_policy_recheck(ctx->prefix_store);
	return 0;
}

int midr_prefix_status_get(struct midr_context *ctx, struct midr_prefix_status *status)
{
	struct midr_prefix_store *store;

	if (!status)
		return -EINVAL;
	memset(status, 0, sizeof(*status));
	if (!ctx || !ctx->prefix_store)
		return -ENOENT;
	store = ctx->prefix_store;
	status->state = store->state;
	status->generation = store->generation;
	status->contributor_count = store->contributors->count;
	status->scans = store->scans;
	status->route_events = store->route_events;
	status->policy_rechecks = store->policy_rechecks;
	status->rejected_midr = store->rejected_midr;
	return 0;
}

struct midr_prefix_foreach_state {
	midr_prefix_contributor_cb cb;
	void *arg;
	int result;
};

static void midr_prefix_foreach_iter(struct hash_bucket *bucket, void *arg)
{
	struct midr_prefix_foreach_state *state = arg;
	struct midr_prefix_entry *entry = bucket->data;

	if (!state->result)
		state->result = state->cb(&entry->prefix, state->arg);
}

int midr_prefix_contributor_foreach(struct midr_context *ctx, midr_prefix_contributor_cb cb,
				    void *arg)
{
	struct midr_prefix_foreach_state state = {
		.cb = cb,
		.arg = arg,
	};

	if (!ctx || !ctx->prefix_store)
		return -ENOENT;
	if (!cb)
		return -EINVAL;
	if (ctx->prefix_store->state != MIDR_PREFIX_READY)
		return -EAGAIN;
	hash_iterate(ctx->prefix_store->contributors, midr_prefix_foreach_iter, &state);
	return state.result;
}

bool midr_prefix_is_ready(struct midr_context *ctx)
{
	return ctx && ctx->prefix_store && ctx->prefix_store->state == MIDR_PREFIX_READY;
}

static const char *midr_prefix_state_name(enum midr_prefix_state state)
{
	switch (state) {
	case MIDR_PREFIX_NOT_READY:
		return "NOT_READY";
	case MIDR_PREFIX_SCANNING:
		return "SCANNING";
	case MIDR_PREFIX_READY:
		return "READY";
	case MIDR_PREFIX_OUT_OF_SYNC:
		return "OUT_OF_SYNC";
	}
	return "UNKNOWN";
}

void midr_show_prefix_summary(struct vty *vty, struct midr_context *ctx)
{
	struct midr_prefix_status status;

	if (midr_prefix_status_get(ctx, &status) != 0) {
		vty_out(vty, "MIDR prefix input is unavailable\n");
		return;
	}
	vty_out(vty, "MIDR prefix input:\n");
	vty_out(vty, "  state:             %s\n", midr_prefix_state_name(status.state));
	vty_out(vty, "  generation:        %" PRIu64 "\n", status.generation);
	vty_out(vty, "  contributors:      %zu\n", status.contributor_count);
	vty_out(vty, "  scans:             %" PRIu64 "\n", status.scans);
	vty_out(vty, "  route events:      %" PRIu64 "\n", status.route_events);
	vty_out(vty, "  policy rechecks:   %" PRIu64 "\n", status.policy_rechecks);
	vty_out(vty, "  MIDR paths denied: %" PRIu64 "\n", status.rejected_midr);
}

static int midr_show_prefix_cb(const struct prefix *prefix, void *arg)
{
	struct vty *vty = arg;

	vty_out(vty, "  %pFX\n", prefix);
	return 0;
}

void midr_show_prefix_contributors(struct vty *vty, struct midr_context *ctx)
{
	int ret;

	vty_out(vty, "MIDR local prefix contributors:\n");
	ret = midr_prefix_contributor_foreach(ctx, midr_show_prefix_cb, vty);
	if (ret == -EAGAIN)
		vty_out(vty, "  not ready\n");
	else if (ret)
		vty_out(vty, "  unavailable (%d)\n", ret);
}

void midr_prefix_config_write_family(struct vty *vty, struct bgp *bgp, afi_t afi, safi_t safi)
{
	struct midr_context *ctx;
	struct listnode *node;
	struct peer *peer;
	const char *route_map;
	uint32_t sources;

	if (!bgp || !bgp->midr_info || safi != SAFI_UNICAST || !midr_prefix_afi_valid(afi))
		return;
	ctx = &bgp->midr_info->ctx;
	route_map = midr_prefix_route_map_name(ctx, afi);
	sources = midr_prefix_local_sources(ctx, afi);

	if (route_map)
		vty_out(vty, "  midr prefix-export route-map %s\n", route_map);
	if (CHECK_FLAG(sources, MIDR_PREFIX_SOURCE_NETWORK))
		vty_out(vty, "  midr prefix-export local-source network\n");
	if (CHECK_FLAG(sources, MIDR_PREFIX_SOURCE_CONNECTED))
		vty_out(vty, "  midr prefix-export local-source connected\n");
	if (CHECK_FLAG(sources, MIDR_PREFIX_SOURCE_STATIC))
		vty_out(vty, "  midr prefix-export local-source static\n");

	for (ALL_LIST_ELEMENTS_RO(bgp->peer, node, peer))
		if (peer_is_config_node(peer) &&
		    peer_af_flag_check(peer, afi, SAFI_UNICAST,
				       PEER_FLAG_MIDR_EXTERNAL_PREFIX_SOURCE))
			vty_out(vty, "  neighbor %s midr external-prefix-source\n", peer->host);
}

int midr_prefix_init(struct midr_context *ctx)
{
	struct midr_prefix_store *store;

	if (!ctx || !ctx->bgp)
		return -EINVAL;
	if (ctx->prefix_store)
		return -EALREADY;

	store = XCALLOC(MTYPE_MIDR_PREFIX_STORE, sizeof(*store));
	store->ctx = ctx;
	store->contributors = midr_prefix_table_new("MIDR prefix contributors");
	store->dirty = midr_prefix_table_new("MIDR prefix scan dirty");
	store->state = MIDR_PREFIX_NOT_READY;
	ctx->prefix_store = store;
	if (ctx->bgp->router_id.s_addr)
		return midr_prefix_rescan(ctx);
	return 0;
}

void midr_prefix_identity_withdraw(struct midr_context *ctx)
{
	struct midr_prefix_store *store;

	if (!ctx || !ctx->prefix_store)
		return;
	store = ctx->prefix_store;
	if (store->contributors->count)
		store->generation++;
	hash_clean(store->contributors, midr_prefix_entry_free);
	hash_clean(store->dirty, midr_prefix_entry_free);
	store->state = MIDR_PREFIX_NOT_READY;
	midr_lsdb_local_metadata_changed(ctx);
}

void midr_prefix_identity_start(struct midr_context *ctx)
{
	if (!ctx || !ctx->prefix_store)
		return;
	if (!ctx->bgp->router_id.s_addr) {
		midr_prefix_identity_withdraw(ctx);
		return;
	}
	if (midr_prefix_rescan(ctx) != 0)
		ctx->prefix_store->state = MIDR_PREFIX_OUT_OF_SYNC;
}

void midr_prefix_finish(struct midr_context *ctx)
{
	struct midr_prefix_store *store;
	afi_t afi;

	if (!ctx || !ctx->prefix_store)
		return;
	store = ctx->prefix_store;
	for (afi = AFI_IP; afi < AFI_MAX; afi++)
		XFREE(MTYPE_MIDR_PREFIX_CONFIG, store->family[afi].route_map_name);
	hash_clean_and_free(&store->contributors, midr_prefix_entry_free);
	hash_clean_and_free(&store->dirty, midr_prefix_entry_free);
	ctx->prefix_store = NULL;
	XFREE(MTYPE_MIDR_PREFIX_STORE, store);
}
