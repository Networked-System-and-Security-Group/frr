// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR SAFI RIB identity and path selection.
 */

#include <zebra.h>

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <string.h>

#include "command.h"
#include "hash.h"
#include "id_alloc.h"
#include "memory.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_aspath.h"
#include "bgpd/bgp_attr.h"
#include "bgpd/bgp_midr_attr.h"
#include "bgpd/bgp_midr_private.h"
#include "bgpd/bgp_midr_rib.h"
#include "bgpd/bgp_route.h"
#include "bgpd/bgp_table.h"

DEFINE_MTYPE_STATIC(BGPD, MIDR_RIB_STORE, "MIDR RIB store");
DEFINE_MTYPE_STATIC(BGPD, MIDR_RIB_IDENTITY, "MIDR RIB identity");

struct midr_rib_identity {
	struct midr_ls_object_key key;
	uint32_t synthetic_id;
	struct bgp_dest *dest;
	enum midr_rib_identity_state state;
	struct bgp_path_info *selected;
	size_t path_count;
};

struct midr_rib_store {
	struct midr_context *ctx;
	struct hash *identities;
	struct id_alloc *allocator;
	size_t identity_limit;
	size_t active_identity_count;
	size_t path_count;
	size_t selected_count;
	size_t conflict_count;
	uint64_t rejected_limit;
	uint64_t rejected_payload_conflict;
};

static unsigned int midr_rib_identity_hash_key(const void *arg)
{
	const struct midr_rib_identity *identity = arg;

	return midr_ls_object_key_hash(&identity->key);
}

static bool midr_rib_identity_hash_cmp(const void *a, const void *b)
{
	const struct midr_rib_identity *left = a;
	const struct midr_rib_identity *right = b;

	return midr_ls_object_key_same(&left->key, &right->key);
}

static void *midr_rib_identity_hash_alloc(void *arg)
{
	const struct midr_rib_identity *source = arg;
	struct midr_rib_identity *identity;

	identity = XCALLOC(MTYPE_MIDR_RIB_IDENTITY, sizeof(*identity));
	*identity = *source;
	return identity;
}

static struct midr_rib_identity *
midr_rib_identity_lookup(struct midr_rib_store *store,
			 const struct midr_ls_object_key *key)
{
	struct midr_rib_identity lookup = {
		.key = *key,
	};

	return hash_lookup(store->identities, &lookup);
}

static void midr_rib_identity_free(void *arg)
{
	struct midr_rib_identity *identity = arg;

	if (identity->dest)
		identity->dest->midr_identity = NULL;
	XFREE(MTYPE_MIDR_RIB_IDENTITY, identity);
}

int midr_rib_init(struct midr_context *ctx)
{
	struct midr_rib_store *store;

	if (!ctx || !ctx->bgp)
		return -EINVAL;
	if (ctx->rib_store)
		return -EALREADY;
	if (!ctx->bgp->rib[AFI_BGP_LS][SAFI_MIDR_LS])
		return -ENOENT;

	store = XCALLOC(MTYPE_MIDR_RIB_STORE, sizeof(*store));
	store->ctx = ctx;
	store->identity_limit = MIDR_RIB_MAX_IDENTITIES;
	store->identities =
		hash_create(midr_rib_identity_hash_key,
			    midr_rib_identity_hash_cmp,
			    "MIDR RIB identities");
	store->allocator = idalloc_new("MIDR synthetic identity IDs");
	ctx->rib_store = store;
	return 0;
}

void midr_rib_finish(struct midr_context *ctx)
{
	struct midr_rib_store *store;

	if (!ctx || !ctx->rib_store)
		return;
	store = ctx->rib_store;
	ctx->rib_store = NULL;
	hash_clean_and_free(&store->identities, midr_rib_identity_free);
	idalloc_destroy(store->allocator);
	XFREE(MTYPE_MIDR_RIB_STORE, store);
}

static int midr_rib_path_validate(struct midr_context *ctx,
				  struct peer *peer,
				  const struct midr_ls_object *object,
				  const struct midr_propagation_path *path)
{
	uint32_t local_node_id;

	if (!ctx || !ctx->bgp || !ctx->rib_store)
		return -ENOENT;
	if (!peer || peer->bgp != ctx->bgp || !object || !path)
		return -EINVAL;
	if (midr_ls_object_validate(object) != 0)
		return -EINVAL;

	local_node_id = ctx->bgp->router_id.s_addr;
	if (!local_node_id)
		return -ENOENT;
	if (peer == ctx->bgp->peer_self) {
		if (object->key.originator_node_id != local_node_id ||
		    path->node_count != 1 || !path->nodes ||
		    path->nodes[0] != local_node_id)
			return -EINVAL;
		return 0;
	}
	if (!peer->remote_id.s_addr)
		return -EINVAL;
	return midr_propagation_path_validate(
		path, object->key.originator_node_id,
		peer->remote_id.s_addr, local_node_id);
}

static void
midr_rib_attributes_from_object(const struct midr_ls_object *object,
				struct midr_ls_attributes *attributes)
{
	memset(attributes, 0, sizeof(*attributes));
	attributes->present = MIDR_LS_ATTR_HAS_SEQUENCE;
	attributes->ls_sequence = object->ls_sequence;
	if (object->policy_tags) {
		attributes->present |= MIDR_LS_ATTR_HAS_POLICY_TAGS;
		attributes->policy_tags = object->policy_tags;
	}

	switch (object->key.type) {
	case MIDR_NLRI_TYPE_MEMBERSHIP:
		attributes->present |= MIDR_LS_ATTR_HAS_GROUP_ID |
				       MIDR_LS_ATTR_HAS_CAP_FLAGS;
		attributes->group_id = object->payload.membership.group_id;
		attributes->cap_flags =
			object->payload.membership.cap_flags;
		if (object->payload.membership.has_transport_address) {
			attributes->present |=
				MIDR_LS_ATTR_HAS_TRANSPORT_ADDRESS;
			attributes->transport_address =
				object->payload.membership.transport_address;
		}
		break;
	case MIDR_NLRI_TYPE_LINK:
		attributes->present |=
			MIDR_LS_ATTR_HAS_LINK_LOCAL_ADDRESS |
			MIDR_LS_ATTR_HAS_LINK_REMOTE_ADDRESS |
			MIDR_LS_ATTR_HAS_LINK_CANONICAL_COST;
		attributes->link_local_address =
			object->payload.link.link_local_address;
		attributes->link_remote_address =
			object->payload.link.link_remote_address;
		attributes->link_canonical_cost =
			object->payload.link.canonical_cost;
		break;
	case MIDR_NLRI_TYPE_NODE_PREFIX:
	case MIDR_NLRI_TYPE_GROUP_PREFIX:
		break;
	case MIDR_NLRI_TYPE_RESERVED:
		break;
	}
}

static struct attr *
midr_rib_attr_intern(struct bgp *bgp, const struct midr_ls_object *object,
		     const struct midr_propagation_path *path)
{
	struct midr_ls_attributes attributes;
	struct attr parsed;
	struct attr *interned;

	bgp_attr_default_set(&parsed, bgp, BGP_ORIGIN_IGP);
	parsed.mp_nexthop_len = IPV4_MAX_BYTELEN;
	parsed.mp_nexthop_global_in = bgp->router_id;

	midr_rib_attributes_from_object(object, &attributes);
	parsed.midr_ls = bgp_midr_ls_attr_new(&attributes);
	parsed.midr_propagation_path = bgp_midr_propagation_path_attr_new(path);
	if (!parsed.midr_ls || !parsed.midr_propagation_path) {
		aspath_unintern(&parsed.aspath);
		bgp_attr_flush(&parsed);
		return NULL;
	}
	interned = bgp_attr_intern(&parsed);
	aspath_unintern(&parsed.aspath);
	return interned;
}

static struct midr_rib_identity *
midr_rib_identity_get(struct midr_context *ctx,
		      const struct midr_ls_object_key *key,
		      struct bgp_dest **dest_out)
{
	struct midr_rib_store *store = ctx->rib_store;
	struct midr_rib_identity *identity;
	struct midr_rib_identity candidate = {
		.key = *key,
	};
	struct bgp_dest *dest;
	struct prefix synthetic = {};

	identity = midr_rib_identity_lookup(store, key);
	if (identity) {
		*dest_out = bgp_dest_lock_node(identity->dest);
		return identity;
	}
	if (store->identities->count >= store->identity_limit) {
		store->rejected_limit++;
		return NULL;
	}
	if (!ctx->bgp->rib[AFI_BGP_LS][SAFI_MIDR_LS])
		return NULL;

	candidate.synthetic_id = idalloc_allocate(store->allocator);
	if (!candidate.synthetic_id) {
		store->rejected_limit++;
		return NULL;
	}
	identity = hash_get(store->identities, &candidate,
			    midr_rib_identity_hash_alloc);

	synthetic.family = AF_UNSPEC;
	synthetic.prefixlen = 32;
	synthetic.u.val32[0] = identity->synthetic_id;
	dest = bgp_afi_node_get(
		ctx->bgp->rib[AFI_BGP_LS][SAFI_MIDR_LS],
		AFI_BGP_LS, SAFI_MIDR_LS, &synthetic, NULL);
	if (!dest) {
		hash_release(store->identities, identity);
		idalloc_free(store->allocator, identity->synthetic_id);
		XFREE(MTYPE_MIDR_RIB_IDENTITY, identity);
		return NULL;
	}
	assert(!dest->midr_identity);
	dest->midr_identity = identity;
	identity->dest = dest;
	*dest_out = dest;
	return identity;
}

static struct bgp_path_info *
midr_rib_peer_path(struct bgp_dest *dest, struct peer *peer)
{
	struct bgp_path_info *path;

	for (path = bgp_dest_get_bgp_path_info(dest); path;
	     path = path->next)
		if (path->peer == peer)
			return path;
	return NULL;
}

const struct midr_ls_object_key *
midr_rib_dest_key(const struct bgp_dest *dest)
{
	const struct midr_rib_identity *identity;

	if (!dest || !dest->midr_identity)
		return NULL;
	identity = dest->midr_identity;
	return &identity->key;
}

int midr_rib_path_object(const struct bgp_dest *dest,
			 const struct bgp_path_info *path,
			 struct midr_ls_object *object)
{
	const struct midr_ls_attributes *attributes;
	const struct midr_ls_object_key *key;

	if (!dest || !path || !path->attr || !object)
		return -EINVAL;
	key = midr_rib_dest_key(dest);
	attributes = bgp_midr_ls_attr_value(path->attr->midr_ls);
	if (!key || !attributes)
		return -EINVAL;
	if (midr_ls_object_from_wire(key, attributes, object) !=
	    MIDR_CODEC_OK)
		return -EINVAL;
	return 0;
}

static const struct midr_propagation_path *
midr_rib_path_propagation(const struct bgp_path_info *path)
{
	if (!path || !path->attr)
		return NULL;
	return bgp_midr_propagation_path_attr_value(
		path->attr->midr_propagation_path);
}

static bool midr_rib_path_eligible(struct bgp_dest *dest,
				   struct bgp_path_info *path,
				   struct midr_ls_object *object)
{
	if (!path || CHECK_FLAG(path->flags, BGP_PATH_REMOVED) ||
	    CHECK_FLAG(path->flags, BGP_PATH_STALE) ||
	    !CHECK_FLAG(path->flags, BGP_PATH_VALID) ||
	    !midr_rib_path_propagation(path))
		return false;
	return midr_rib_path_object(dest, path, object) == 0;
}

static unsigned int midr_rib_owner_rank(struct bgp *bgp,
					const struct midr_ls_object *object,
					const struct bgp_path_info *path)
{
	if (path->peer == bgp->peer_self ||
	    (path->peer && path->peer->remote_id.s_addr ==
				   object->key.originator_node_id))
		return 0;
	return 1;
}

static uint32_t midr_rib_peer_node_id(struct bgp *bgp,
				      const struct bgp_path_info *path)
{
	if (path->peer == bgp->peer_self)
		return bgp->router_id.s_addr;
	return path->peer ? path->peer->remote_id.s_addr : UINT32_MAX;
}

static struct bgp_path_info *
midr_rib_select(struct bgp *bgp, struct bgp_dest *dest, bool *conflict)
{
	struct bgp_path_info *current = NULL;
	struct bgp_path_info *candidate;
	struct bgp_path_info *best = NULL;
	struct midr_ls_object candidate_object;
	struct midr_ls_object best_object;
	unsigned int best_owner_rank = UINT_MAX;
	uint64_t best_sequence = 0;

	*conflict = false;
	for (candidate = bgp_dest_get_bgp_path_info(dest); candidate;
	     candidate = candidate->next)
		if (CHECK_FLAG(candidate->flags, BGP_PATH_SELECTED))
			current = candidate;

	for (candidate = bgp_dest_get_bgp_path_info(dest); candidate;
	     candidate = candidate->next) {
		unsigned int owner_rank;

		if (!midr_rib_path_eligible(dest, candidate,
					    &candidate_object))
			continue;
		owner_rank = midr_rib_owner_rank(
			bgp, &candidate_object, candidate);
		if (owner_rank < best_owner_rank ||
		    (owner_rank == best_owner_rank &&
		     candidate_object.ls_sequence > best_sequence)) {
			best_owner_rank = owner_rank;
			best_sequence = candidate_object.ls_sequence;
			best = candidate;
			best_object = candidate_object;
			*conflict = false;
			continue;
		}
		if (owner_rank != best_owner_rank ||
		    candidate_object.ls_sequence != best_sequence)
			continue;
		if (!midr_ls_object_same(&best_object,
					 &candidate_object)) {
			*conflict = true;
			continue;
		}
		if (candidate == current) {
			best = candidate;
			best_object = candidate_object;
			continue;
		}
		if (best == current)
			continue;
		if (midr_rib_path_propagation(candidate)->node_count <
			    midr_rib_path_propagation(best)->node_count ||
		    (midr_rib_path_propagation(candidate)->node_count ==
			     midr_rib_path_propagation(best)->node_count &&
		     ntohl(midr_rib_peer_node_id(bgp, candidate)) <
			     ntohl(midr_rib_peer_node_id(bgp, best)))) {
			best = candidate;
			best_object = candidate_object;
		}
	}

	return *conflict ? NULL : best;
}

static void midr_rib_identity_set_selection(
	struct midr_rib_store *store, struct midr_rib_identity *identity,
	struct bgp_path_info *selected, bool conflict)
{
	if (identity->selected)
		store->selected_count--;
	if (identity->state == MIDR_RIB_IDENTITY_CONFLICT_QUARANTINED)
		store->conflict_count--;

	identity->selected = selected;
	if (conflict) {
		identity->state =
			MIDR_RIB_IDENTITY_CONFLICT_QUARANTINED;
		store->conflict_count++;
	} else if (selected) {
		identity->state = MIDR_RIB_IDENTITY_SELECTED;
		store->selected_count++;
	} else {
		identity->state = MIDR_RIB_IDENTITY_NO_PATH;
	}
}

void bgp_midr_rib_process_main(struct bgp *bgp, struct bgp_dest *dest)
{
	struct midr_rib_store *store;
	struct midr_rib_identity *identity;
	struct bgp_path_info *old_selected = NULL;
	struct bgp_path_info *new_selected;
	struct bgp_path_info *path;
	struct bgp_path_info *next;
	bool attr_changed;
	bool conflict;

	if (!bgp || !bgp->midr_info || !dest ||
	    !dest->midr_identity)
		return;
	store = bgp->midr_info->ctx.rib_store;
	if (!store)
		return;
	identity = dest->midr_identity;
	for (path = bgp_dest_get_bgp_path_info(dest); path;
	     path = path->next)
		if (CHECK_FLAG(path->flags, BGP_PATH_SELECTED))
			old_selected = path;

	new_selected = midr_rib_select(bgp, dest, &conflict);
	attr_changed = old_selected &&
		       CHECK_FLAG(old_selected->flags,
				  BGP_PATH_ATTR_CHANGED);
	midr_rib_identity_set_selection(store, identity, new_selected,
					conflict);

	if (old_selected != new_selected || attr_changed) {
		if (old_selected)
			bgp_path_info_unset_flag(
				dest, old_selected, BGP_PATH_SELECTED);
		if (new_selected) {
			bgp_path_info_set_flag(
				dest, new_selected, BGP_PATH_SELECTED);
			UNSET_FLAG(new_selected->flags,
				   BGP_PATH_ATTR_CHANGED);
		}
		bgp_bump_version(dest);
		bgp_midr_rib_route_update_notify(
			bgp, dest, old_selected, new_selected);
	}

	for (path = bgp_dest_get_bgp_path_info(dest); path;
	     path = next) {
		next = path->next;
		if (!CHECK_FLAG(path->flags, BGP_PATH_REMOVED))
			continue;
		assert(identity->path_count && store->path_count);
		identity->path_count--;
		store->path_count--;
		assert(bgp_path_info_reap(dest, path));
	}
	if (!identity->path_count) {
		assert(!identity->selected);
		assert(identity->state == MIDR_RIB_IDENTITY_NO_PATH);
		assert(store->active_identity_count);
		store->active_identity_count--;
	}
	UNSET_FLAG(dest->flags, BGP_NODE_PROCESS_SCHEDULED);
}

int midr_rib_path_upsert(struct midr_context *ctx, struct peer *peer,
			 const struct midr_ls_object *object,
			 const struct midr_propagation_path *path)
{
	struct midr_rib_store *store;
	struct midr_rib_identity *identity;
	struct bgp_path_info *existing;
	struct bgp_path_info *new_path;
	struct midr_ls_object old_object;
	struct attr *new_attr;
	struct attr *old_attr;
	struct bgp_dest *dest = NULL;
	int ret;

	ret = midr_rib_path_validate(ctx, peer, object, path);
	if (ret)
		return ret;
	store = ctx->rib_store;
	identity = midr_rib_identity_get(ctx, &object->key, &dest);
	if (!identity)
		return -ENOSPC;

	existing = midr_rib_peer_path(dest, peer);
	if (existing &&
	    midr_rib_path_object(dest, existing, &old_object) == 0 &&
	    old_object.ls_sequence == object->ls_sequence &&
	    !midr_ls_object_same(&old_object, object)) {
		store->rejected_payload_conflict++;
		bgp_path_info_mark_for_delete(dest, existing);
		bgp_process_main_one(ctx->bgp, dest, AFI_BGP_LS,
				     SAFI_MIDR_LS);
		bgp_dest_unlock_node(dest);
		return -EINVAL;
	}

	new_attr = midr_rib_attr_intern(ctx->bgp, object, path);
	if (!new_attr) {
		bgp_dest_unlock_node(dest);
		return -ENOMEM;
	}
	if (existing) {
		if (existing->attr == new_attr) {
			bgp_attr_unintern(&new_attr);
			bgp_dest_unlock_node(dest);
			return 0;
		}
		old_attr = existing->attr;
		existing->attr = new_attr;
		bgp_attr_unintern(&old_attr);
		SET_FLAG(existing->flags, BGP_PATH_VALID |
					      BGP_PATH_ATTR_CHANGED);
		UNSET_FLAG(existing->flags,
			   BGP_PATH_REMOVED | BGP_PATH_STALE);
		bgp_process_main_one(ctx->bgp, dest, AFI_BGP_LS,
				     SAFI_MIDR_LS);
		bgp_dest_unlock_node(dest);
		return 0;
	}

	new_path = info_make(ZEBRA_ROUTE_BGP, BGP_ROUTE_NORMAL, 0,
			     peer, new_attr, dest);
	SET_FLAG(new_path->flags, BGP_PATH_VALID);
	new_path->from = peer;
	bgp_path_info_add(dest, new_path);
	if (!identity->path_count)
		store->active_identity_count++;
	identity->path_count++;
	store->path_count++;
	bgp_process_main_one(ctx->bgp, dest, AFI_BGP_LS,
			     SAFI_MIDR_LS);
	bgp_dest_unlock_node(dest);
	return 0;
}

int midr_rib_path_withdraw(struct midr_context *ctx, struct peer *peer,
			   const struct midr_ls_object_key *key)
{
	struct midr_rib_identity *identity;
	struct bgp_path_info *path;
	struct bgp_dest *dest;

	if (!ctx || !ctx->bgp || !ctx->rib_store)
		return -ENOENT;
	if (!peer || peer->bgp != ctx->bgp || !key ||
	    midr_ls_object_key_validate(key) != 0)
		return -EINVAL;
	if (peer == ctx->bgp->peer_self &&
	    key->originator_node_id != ctx->bgp->router_id.s_addr)
		return -EINVAL;

	identity = midr_rib_identity_lookup(ctx->rib_store, key);
	if (!identity)
		return -ENOENT;
	dest = bgp_dest_lock_node(identity->dest);
	path = midr_rib_peer_path(dest, peer);
	if (!path) {
		bgp_dest_unlock_node(dest);
		return -ENOENT;
	}
	bgp_path_info_mark_for_delete(dest, path);
	bgp_process_main_one(ctx->bgp, dest, AFI_BGP_LS,
			     SAFI_MIDR_LS);
	bgp_dest_unlock_node(dest);
	return 0;
}

void midr_rib_dest_cleanup(struct bgp *bgp, struct bgp_dest *dest)
{
	struct midr_rib_store *store;
	struct midr_rib_identity *identity;

	if (!dest || !dest->midr_identity)
		return;
	identity = dest->midr_identity;
	dest->midr_identity = NULL;
	identity->dest = NULL;
	if (!bgp || !bgp->midr_info ||
	    !bgp->midr_info->ctx.rib_store)
		return;
	store = bgp->midr_info->ctx.rib_store;
	if (identity->selected)
		store->selected_count--;
	if (identity->state == MIDR_RIB_IDENTITY_CONFLICT_QUARANTINED)
		store->conflict_count--;
	if (identity->path_count <= store->path_count)
		store->path_count -= identity->path_count;
	assert(hash_release(store->identities, identity) ==
	       identity);
	idalloc_free(store->allocator, identity->synthetic_id);
	XFREE(MTYPE_MIDR_RIB_IDENTITY, identity);
}

int midr_rib_selected_get(
	struct midr_context *ctx, const struct midr_ls_object_key *key,
	struct midr_ls_object *object,
	const struct midr_propagation_path **path, struct peer **peer)
{
	struct midr_rib_identity *identity;

	if (!ctx || !ctx->rib_store)
		return -ENOENT;
	if (!key || !object || !path || !peer)
		return -EINVAL;
	identity = midr_rib_identity_lookup(ctx->rib_store, key);
	if (!identity || !identity->selected)
		return -ENOENT;
	if (midr_rib_path_object(identity->dest, identity->selected,
				 object) != 0)
		return -EINVAL;
	*path = midr_rib_path_propagation(identity->selected);
	*peer = identity->selected->peer;
	return *path ? 0 : -EINVAL;
}

struct midr_rib_foreach_state {
	midr_rib_selected_cb callback;
	void *arg;
	int result;
};

static void midr_rib_selected_iter(struct hash_bucket *bucket,
				   void *arg)
{
	struct midr_rib_foreach_state *state = arg;
	struct midr_rib_identity *identity = bucket->data;
	const struct midr_propagation_path *propagation;
	struct midr_ls_object object;

	if (state->result || !identity->selected)
		return;
	if (midr_rib_path_object(identity->dest,
				 identity->selected, &object) != 0) {
		state->result = -EINVAL;
		return;
	}
	propagation =
		midr_rib_path_propagation(identity->selected);
	if (!propagation) {
		state->result = -EINVAL;
		return;
	}
	state->result = state->callback(
		&object, propagation, identity->selected->peer,
		state->arg);
}

int midr_rib_selected_foreach(struct midr_context *ctx,
			      midr_rib_selected_cb callback, void *arg)
{
	struct midr_rib_foreach_state state = {
		.callback = callback,
		.arg = arg,
	};

	if (!ctx || !ctx->rib_store)
		return -ENOENT;
	if (!callback)
		return -EINVAL;
	hash_iterate(ctx->rib_store->identities,
		     midr_rib_selected_iter, &state);
	return state.result;
}

struct midr_rib_entry_foreach_state {
	midr_rib_selected_entry_cb callback;
	void *arg;
	int result;
};

static void midr_rib_selected_entry_iter(struct hash_bucket *bucket,
					 void *arg)
{
	struct midr_rib_entry_foreach_state *state = arg;
	struct midr_rib_identity *identity = bucket->data;
	const struct midr_propagation_path *propagation;
	struct midr_ls_object object;

	if (state->result || !identity->selected)
		return;
	if (midr_rib_path_object(identity->dest, identity->selected,
				 &object) != 0) {
		state->result = -EINVAL;
		return;
	}
	propagation = midr_rib_path_propagation(identity->selected);
	if (!propagation) {
		state->result = -EINVAL;
		return;
	}
	state->result = state->callback(
		&object, propagation, identity->selected->peer,
		identity->dest, identity->selected, state->arg);
}

int midr_rib_selected_entry_foreach(
	struct midr_context *ctx, midr_rib_selected_entry_cb callback,
	void *arg)
{
	struct midr_rib_entry_foreach_state state = {
		.callback = callback,
		.arg = arg,
	};

	if (!ctx || !ctx->rib_store)
		return -ENOENT;
	if (!callback)
		return -EINVAL;
	hash_iterate(ctx->rib_store->identities,
		     midr_rib_selected_entry_iter, &state);
	return state.result;
}

int midr_rib_summary_get(struct midr_context *ctx,
			 struct midr_rib_summary *summary)
{
	struct midr_rib_store *store;

	if (!ctx || !ctx->rib_store)
		return -ENOENT;
	if (!summary)
		return -EINVAL;
	store = ctx->rib_store;
	*summary = (struct midr_rib_summary){
		.identity_count = store->active_identity_count,
		.path_count = store->path_count,
		.selected_count = store->selected_count,
		.conflict_count = store->conflict_count,
		.identity_limit = store->identity_limit,
		.rejected_limit = store->rejected_limit,
		.rejected_payload_conflict =
			store->rejected_payload_conflict,
	};
	return 0;
}

static const char *midr_rib_identity_state_name(enum midr_rib_identity_state state)
{
	switch (state) {
	case MIDR_RIB_IDENTITY_NO_PATH:
		return "NO_PATH";
	case MIDR_RIB_IDENTITY_SELECTED:
		return "SELECTED";
	case MIDR_RIB_IDENTITY_CONFLICT_QUARANTINED:
		return "CONFLICT_QUARANTINED";
	}
	return "UNKNOWN";
}

static const char *midr_rib_object_type_name(enum midr_nlri_type type)
{
	switch (type) {
	case MIDR_NLRI_TYPE_MEMBERSHIP:
		return "MEMBERSHIP";
	case MIDR_NLRI_TYPE_LINK:
		return "LINK";
	case MIDR_NLRI_TYPE_NODE_PREFIX:
		return "NODE_PREFIX";
	case MIDR_NLRI_TYPE_GROUP_PREFIX:
		return "GROUP_PREFIX";
	case MIDR_NLRI_TYPE_RESERVED:
		break;
	}
	return "UNKNOWN";
}

static void midr_rib_show_object(struct vty *vty,
				 const struct midr_ls_object *object)
{
	struct in_addr origin = {.s_addr = object->key.originator_node_id};
	struct in_addr remote;

	vty_out(vty, " object=%s origin=%pI4 sequence=%" PRIu64
		     " policy=0x%" PRIx64,
		midr_rib_object_type_name(object->key.type), &origin,
		object->ls_sequence, object->policy_tags);
	switch (object->key.type) {
	case MIDR_NLRI_TYPE_MEMBERSHIP:
		vty_out(vty, " group=%u transport=",
			object->payload.membership.group_id);
		if (object->payload.membership.has_transport_address)
			vty_out(vty, "%pIA",
				&object->payload.membership.transport_address);
		else
			vty_out(vty, "none");
		vty_out(vty, " caps=0x%" PRIx64,
			object->payload.membership.cap_flags);
		break;
	case MIDR_NLRI_TYPE_LINK:
		remote.s_addr = object->key.u.link.remote_node_id;
		vty_out(vty,
			" remote=%pI4 link-id=%" PRIu64
			" addresses=%pIA->%pIA cost=%u",
			&remote, object->key.u.link.link_id,
			&object->payload.link.link_local_address,
			&object->payload.link.link_remote_address,
			object->payload.link.canonical_cost);
		break;
	case MIDR_NLRI_TYPE_NODE_PREFIX:
		vty_out(vty, " afi=%u safi=%u prefix=%pFX",
			object->key.u.node_prefix.afi,
			object->key.u.node_prefix.safi,
			&object->key.u.node_prefix.prefix);
		break;
	case MIDR_NLRI_TYPE_GROUP_PREFIX:
		vty_out(vty, " group=%u afi=%u safi=%u prefix=%pFX",
			object->key.u.group_prefix.group_id,
			object->key.u.group_prefix.prefix.afi,
			object->key.u.group_prefix.prefix.safi,
			&object->key.u.group_prefix.prefix.prefix);
		break;
	case MIDR_NLRI_TYPE_RESERVED:
		break;
	}
}

struct midr_rib_show_state {
	struct vty *vty;
	struct midr_context *ctx;
	size_t identity_count;
	size_t path_count;
};

static void midr_rib_show_identity(struct hash_bucket *bucket, void *arg)
{
	struct midr_rib_show_state *show = arg;
	struct midr_rib_identity *identity = bucket->data;
	struct bgp_path_info *path;

	if (!identity->path_count)
		return;
	show->identity_count++;
	vty_out(show->vty,
		"identity synthetic-id=%u state=%s paths=%zu\n",
		identity->synthetic_id,
		midr_rib_identity_state_name(identity->state),
		identity->path_count);
	for (path = bgp_dest_get_bgp_path_info(identity->dest); path;
	     path = path->next) {
		const struct midr_propagation_path *propagation;
		struct midr_ls_object object;
		struct in_addr peer_id;
		size_t index;

		show->path_count++;
		vty_out(show->vty, "  path peer=");
		if (path->peer == show->ctx->bgp->peer_self)
			vty_out(show->vty, "self");
		else if (path->peer && path->peer->remote_id.s_addr) {
			peer_id = path->peer->remote_id;
			vty_out(show->vty, "%pI4", &peer_id);
		} else
			vty_out(show->vty, "%s",
				path->peer && path->peer->host ? path->peer->host
							       : "unknown");
		vty_out(show->vty,
			" selected=%s valid=%s stale=%s removed=%s",
			CHECK_FLAG(path->flags, BGP_PATH_SELECTED) ? "yes" : "no",
			CHECK_FLAG(path->flags, BGP_PATH_VALID) ? "yes" : "no",
			CHECK_FLAG(path->flags, BGP_PATH_STALE) ? "yes" : "no",
			CHECK_FLAG(path->flags, BGP_PATH_REMOVED) ? "yes" : "no");
		if (midr_rib_path_object(identity->dest, path, &object) != 0) {
			vty_out(show->vty, " object=INVALID\n");
			continue;
		}
		midr_rib_show_object(show->vty, &object);
		propagation = midr_rib_path_propagation(path);
		vty_out(show->vty, " propagation=[");
		if (propagation)
			for (index = 0; index < propagation->node_count; index++) {
				struct in_addr node = {
					.s_addr = propagation->nodes[index],
				};

				vty_out(show->vty, "%s%pI4",
					index ? "," : "", &node);
			}
		vty_out(show->vty, "]\n");
	}
}

void midr_show_rib_paths(struct vty *vty, struct midr_context *ctx)
{
	struct midr_rib_show_state show = {
		.vty = vty,
		.ctx = ctx,
	};

	if (!vty || !ctx || !ctx->rib_store) {
		if (vty)
			vty_out(vty, "MIDR RIB is unavailable\n");
		return;
	}
	vty_out(vty, "MIDR RIB paths:\n");
	hash_iterate(ctx->rib_store->identities, midr_rib_show_identity,
		     &show);
	if (!show.identity_count)
		vty_out(vty, "  none\n");
	vty_out(vty, "MIDR RIB path totals: identities=%zu paths=%zu\n",
		show.identity_count, show.path_count);
}

int midr_rib_test_set_identity_limit(struct midr_context *ctx,
				      size_t limit)
{
	if (!ctx || !ctx->rib_store)
		return -ENOENT;
	if (!limit || limit > MIDR_RIB_MAX_IDENTITIES ||
	    limit < ctx->rib_store->identities->count)
		return -EINVAL;
	ctx->rib_store->identity_limit = limit;
	return 0;
}

int midr_rib_test_set_path_stale(
	struct midr_context *ctx, const struct midr_ls_object_key *key,
	struct peer *peer, bool stale)
{
	struct midr_rib_identity *identity;
	struct bgp_path_info *path;
	struct bgp_dest *dest;

	if (!ctx || !ctx->bgp || !ctx->rib_store)
		return -ENOENT;
	if (!key || !peer)
		return -EINVAL;
	identity = midr_rib_identity_lookup(ctx->rib_store, key);
	if (!identity)
		return -ENOENT;
	dest = bgp_dest_lock_node(identity->dest);
	path = midr_rib_peer_path(dest, peer);
	if (!path) {
		bgp_dest_unlock_node(dest);
		return -ENOENT;
	}
	if (stale)
		SET_FLAG(path->flags, BGP_PATH_STALE);
	else
		UNSET_FLAG(path->flags, BGP_PATH_STALE);
	bgp_process_main_one(ctx->bgp, dest, AFI_BGP_LS,
			     SAFI_MIDR_LS);
	bgp_dest_unlock_node(dest);
	return 0;
}
