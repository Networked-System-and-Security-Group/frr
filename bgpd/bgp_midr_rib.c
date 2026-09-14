// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR SAFI RIB identity and path selection.
 */

#include <zebra.h>

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <time.h>

#include "command.h"
#include "hash.h"
#include "id_alloc.h"
#include "memory.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_aspath.h"
#include "bgpd/bgp_attr.h"
#include "bgpd/bgp_midr_attr.h"
#include "bgpd/bgp_midr_canonical.h"
#include "bgpd/bgp_midr_instance.h"
#include "bgpd/bgp_midr_private.h"
#include "bgpd/bgp_midr_rib.h"
#include "bgpd/bgp_route.h"
#include "bgpd/bgp_table.h"

DEFINE_MTYPE_STATIC(BGPD, MIDR_RIB_STORE, "MIDR RIB store");
DEFINE_MTYPE_STATIC(BGPD, MIDR_RIB_IDENTITY, "MIDR RIB identity");
DEFINE_MTYPE_STATIC(BGPD, MIDR_RIB_ADVERTISEMENT, "MIDR peer advertisement");

#define MIDR_RIB_LIFETIME_SWEEP_MSEC 1000U
#define MIDR_RIB_LIFETIME_SWEEP_LIMIT 256U

struct midr_rib_advertisement {
	struct midr_rib_advertisement *next;
	struct peer *peer;
	uint64_t sequence;
	enum midr_instance_state state;
};

struct midr_rib_identity {
	struct midr_ls_object_key key;
	uint32_t synthetic_id;
	struct bgp_dest *dest;
	enum midr_rib_identity_state state;
	struct bgp_path_info *selected;
	struct bgp_path_info *canonical_path;
	struct midr_rib_advertisement *advertisements;
	size_t path_count;
};

struct midr_rib_store {
	struct midr_context *ctx;
	struct hash *identities;
	struct id_alloc *allocator;
	struct midr_canonical *canonical;
	size_t identity_limit;
	size_t active_identity_count;
	size_t path_count;
	size_t selected_count;
	size_t conflict_count;
	uint64_t rejected_limit;
	uint64_t rejected_payload_conflict;
	uint64_t rejected_resource;
	uint64_t rejected_internal;
	struct event *lifetime_timer;
	bool test_clock_enabled;
	uint64_t test_now_ns;
};

static void midr_rib_lifetime_event(struct event *event);

static uint64_t midr_rib_clock_now_ns(void *arg)
{
	struct midr_rib_store *store = arg;
	struct timespec ts;

	if (store && store->test_clock_enabled)
		return store->test_now_ns;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

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
	struct midr_rib_advertisement *advertisement;

	if (identity->dest)
		identity->dest->midr_identity = NULL;
	while ((advertisement = identity->advertisements) != NULL) {
		identity->advertisements = advertisement->next;
		XFREE(MTYPE_MIDR_RIB_ADVERTISEMENT, advertisement);
	}
	XFREE(MTYPE_MIDR_RIB_IDENTITY, identity);
}

static bool midr_rib_advertisement_has(
	const struct midr_rib_identity *identity, const struct peer *peer)
{
	const struct midr_rib_advertisement *advertisement;

	for (advertisement = identity->advertisements; advertisement;
	     advertisement = advertisement->next)
		if (advertisement->peer == peer)
			return true;
	return false;
}

static struct midr_rib_advertisement *midr_rib_advertisement_lookup(
	struct midr_rib_identity *identity, const struct peer *peer)
{
	struct midr_rib_advertisement *advertisement;

	for (advertisement = identity->advertisements; advertisement;
	     advertisement = advertisement->next)
		if (advertisement->peer == peer)
			return advertisement;
	return NULL;
}

static int midr_rib_advertisement_update(
	struct midr_rib_identity *identity, struct peer *peer,
	const struct midr_instance *instance)
{
	struct midr_rib_advertisement *advertisement;

	advertisement = midr_rib_advertisement_lookup(identity, peer);
	if (advertisement) {
		advertisement->sequence = instance->object.ls_sequence;
		advertisement->state = instance->state;
		return 0;
	}
	advertisement = XCALLOC(MTYPE_MIDR_RIB_ADVERTISEMENT, sizeof(*advertisement));
	if (!advertisement)
		return -ENOMEM;
	advertisement->peer = peer;
	advertisement->sequence = instance->object.ls_sequence;
	advertisement->state = instance->state;
	advertisement->next = identity->advertisements;
	identity->advertisements = advertisement;
	return 0;
}

static bool midr_rib_advertisement_remove(struct midr_rib_identity *identity,
						const struct peer *peer)
{
	struct midr_rib_advertisement **link;

	for (link = &identity->advertisements; *link; link = &(*link)->next)
		if ((*link)->peer == peer) {
			struct midr_rib_advertisement *removed = *link;

			*link = removed->next;
			XFREE(MTYPE_MIDR_RIB_ADVERTISEMENT, removed);
			return true;
		}
	return false;
}

bool midr_rib_peer_advertisement_has(
	struct midr_context *ctx, const struct midr_ls_object_key *key,
	const struct peer *peer)
{
	struct midr_rib_identity *identity;

	if (!ctx || !ctx->rib_store || !key || !peer)
		return false;
	identity = midr_rib_identity_lookup(ctx->rib_store, key);
	return identity && midr_rib_advertisement_has(identity, peer);
}

bool midr_rib_peer_advertisement_current(
	struct midr_context *ctx, const struct midr_instance *instance,
	const struct peer *peer)
{
	struct midr_rib_advertisement *advertisement;
	struct midr_rib_identity *identity;

	if (!ctx || !ctx->rib_store || !instance || !peer)
		return false;
	identity = midr_rib_identity_lookup(ctx->rib_store,
					    &instance->object.key);
	if (!identity)
		return false;
	for (advertisement = identity->advertisements; advertisement;
	     advertisement = advertisement->next)
		if (advertisement->peer == peer)
			return advertisement->sequence ==
				       instance->object.ls_sequence &&
			       advertisement->state == instance->state;
	return false;
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
	{
		struct midr_canonical_config config = {
			.identity_limit = MIDR_RIB_MAX_IDENTITIES,
			.event_limit = MIDR_RIB_MAX_IDENTITIES * 2U,
			.max_age_ms = MIDR_CANONICAL_MAX_AGE_MS,
			.now_ns = midr_rib_clock_now_ns,
			.clock_arg = store,
		};

		if (midr_canonical_create(&config, &store->canonical) != 0) {
			idalloc_destroy(store->allocator);
			hash_clean_and_free(&store->identities,
					    midr_rib_identity_free);
			XFREE(MTYPE_MIDR_RIB_STORE, store);
			return -ENOMEM;
		}
	}
	ctx->rib_store = store;
	if (bm && bm->master)
		event_add_timer_msec(bm->master, midr_rib_lifetime_event, store,
				     MIDR_RIB_LIFETIME_SWEEP_MSEC,
				     &store->lifetime_timer);
	return 0;
}

void midr_rib_finish(struct midr_context *ctx)
{
	struct midr_rib_store *store;

	if (!ctx || !ctx->rib_store)
		return;
	store = ctx->rib_store;
	ctx->rib_store = NULL;
	event_cancel(&store->lifetime_timer);
	midr_canonical_destroy(&store->canonical);
	hash_clean_and_free(&store->identities, midr_rib_identity_free);
	idalloc_destroy(store->allocator);
	XFREE(MTYPE_MIDR_RIB_STORE, store);
}

static struct attr *
midr_rib_instance_attr_intern(struct bgp *bgp,
			       const struct midr_instance *instance,
			       uint32_t age_ms)
{
	struct attr parsed;
	struct attr *interned;

	if (!bgp || !instance || midr_instance_validate(instance))
		return NULL;
	bgp_attr_default_set(&parsed, bgp, BGP_ORIGIN_IGP);
	parsed.mp_nexthop_len = IPV4_MAX_BYTELEN;
	parsed.mp_nexthop_global_in = bgp->router_id;
	parsed.midr_ls = bgp_midr_instance_attr_intern(instance, age_ms);
	if (!parsed.midr_ls) {
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

static int midr_rib_path_instance_decode(const struct bgp_dest *dest,
						 const struct bgp_path_info *path,
						 struct midr_instance *instance)
{
	const struct midr_ls_attributes *attributes;
	const struct midr_ls_object_key *key;
	struct midr_instance_attributes wire;

	if (!dest || !path || !path->attr || !instance)
		return -EINVAL;
	key = midr_rib_dest_key(dest);
	attributes = bgp_midr_ls_attr_value(path->attr->midr_ls);
	if (!key || !attributes)
		return -EINVAL;
	memset(&wire, 0, sizeof(wire));
	wire.ls = *attributes;
	wire.state = bgp_midr_ls_attr_state(path->attr->midr_ls);
	wire.age_ms = bgp_midr_ls_attr_age(path->attr->midr_ls);
	if (midr_instance_from_wire(key, &wire, instance) != MIDR_CODEC_OK)
		return -EINVAL;
	return 0;
}

int midr_rib_path_instance(struct midr_context *ctx,
				 const struct bgp_dest *dest,
				 const struct bgp_path_info *path,
				 struct midr_instance *instance, uint32_t *age_ms)
{
	const struct midr_rib_identity *identity;
	struct midr_canonical_view view;
	int ret;

	if (!ctx || !ctx->rib_store || !dest || !path || !instance)
		return -EINVAL;
	ret = midr_rib_path_instance_decode(dest, path, instance);
	if (ret)
		return ret;
	if (!age_ms)
		return 0;
	*age_ms = bgp_midr_ls_attr_age(path->attr->midr_ls);
	identity = dest->midr_identity;
	if (!identity || !identity->canonical_path ||
	    identity->canonical_path != path)
		return 0;
	ret = midr_canonical_lookup(ctx->rib_store->canonical,
				    &identity->key, &view);
	if (ret || view.state != MIDR_CANONICAL_CURRENT || !view.current)
		return ret;
	ret = midr_instance_ref_age(view.current, midr_rib_now_ns(ctx),
				    0,
				    midr_canonical_max_age_ms(
					    ctx->rib_store->canonical),
				    age_ms);
	return ret;
}

int midr_rib_path_instance_ref(
	struct midr_context *ctx, const struct bgp_dest *dest,
	const struct bgp_path_info *path,
	const struct midr_instance_ref **instance_ref)
{
	const struct midr_rib_identity *identity;
	struct midr_canonical_view view;
	int ret;

	if (!ctx || !ctx->rib_store || !dest || !path || !instance_ref)
		return -EINVAL;
	*instance_ref = NULL;
	identity = dest->midr_identity;
	if (!identity || identity->canonical_path != path)
		return -ESTALE;
	ret = midr_canonical_lookup(ctx->rib_store->canonical,
				    &identity->key, &view);
	if (ret)
		return ret;
	if (view.state != MIDR_CANONICAL_CURRENT || !view.current)
		return -ESTALE;
	*instance_ref = view.current;
	return 0;
}

uint64_t midr_rib_now_ns(struct midr_context *ctx)
{
	if (!ctx || !ctx->rib_store)
		return 0;
	return midr_rib_clock_now_ns(ctx->rib_store);
}

uint32_t midr_rib_max_age_ms(struct midr_context *ctx)
{
	if (!ctx || !ctx->rib_store)
		return 0;
	return midr_canonical_max_age_ms(ctx->rib_store->canonical);
}

int midr_rib_instance_ref_age_at(
	struct midr_context *ctx, const struct midr_instance_ref *instance_ref,
	uint64_t now_ns, uint32_t budget_ms, uint32_t *age_ms)
{
	if (!ctx || !ctx->rib_store)
		return -ENOENT;
	return midr_instance_ref_age(instance_ref, now_ns, budget_ms,
				     midr_rib_max_age_ms(ctx), age_ms);
}

static void midr_rib_apply_lifetime_events(struct midr_rib_store *store)
{
	const struct midr_canonical_event *event;

	while ((event = midr_canonical_event_peek(store->canonical)) != NULL) {
		const struct midr_ls_object_key *key;
		struct midr_rib_identity *identity;

		if (midr_canonical_event_change(event) != MIDR_CANONICAL_EXPIRE)
			break;
		key = midr_canonical_event_key(event);
		identity = midr_rib_identity_lookup(store, key);
		if (identity && identity->dest && identity->canonical_path &&
		    !CHECK_FLAG(identity->canonical_path->flags, BGP_PATH_REMOVED)) {
			struct bgp_dest *dest = bgp_dest_lock_node(identity->dest);

			bgp_path_info_mark_for_delete(dest, identity->canonical_path);
			bgp_process_main_one(store->ctx->bgp, dest, AFI_BGP_LS,
					     SAFI_MIDR_LS);
			bgp_dest_unlock_node(dest);
		}
		midr_canonical_event_ack(store->canonical);
	}
}

static void midr_rib_lifetime_event(struct event *event)
{
	struct midr_rib_store *store = EVENT_ARG(event);

	if (!store || !store->ctx)
		return;
	store->lifetime_timer = NULL;
	(void)midr_canonical_sweep(store->canonical,
				   MIDR_RIB_LIFETIME_SWEEP_LIMIT, NULL);
	midr_rib_apply_lifetime_events(store);
	(void)midr_canonical_gc(store->canonical,
				MIDR_RIB_LIFETIME_SWEEP_LIMIT, NULL);
	if (bm && bm->master)
		event_add_timer_msec(bm->master, midr_rib_lifetime_event, store,
				     MIDR_RIB_LIFETIME_SWEEP_MSEC,
				     &store->lifetime_timer);
}

int midr_rib_path_object(const struct bgp_dest *dest,
			 const struct bgp_path_info *path,
			 struct midr_ls_object *object)
{
	struct midr_instance instance;

	if (!object || midr_rib_path_instance_decode(dest, path, &instance))
		return -EINVAL;
	*object = instance.object;
	return 0;
}

void bgp_midr_rib_process_main(struct bgp *bgp, struct bgp_dest *dest)
{
	struct midr_rib_store *store;
	struct midr_rib_identity *identity;
	struct bgp_path_info *old_selected = NULL;
	struct bgp_path_info *new_selected;
	struct bgp_path_info *path;
	struct bgp_path_info *next;

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

	new_selected = identity->canonical_path;
	if (!new_selected || CHECK_FLAG(new_selected->flags, BGP_PATH_REMOVED) ||
	    CHECK_FLAG(new_selected->flags, BGP_PATH_STALE) ||
	    !CHECK_FLAG(new_selected->flags, BGP_PATH_VALID))
		new_selected = NULL;
	if (old_selected != new_selected) {
		if (old_selected)
			bgp_path_info_unset_flag(dest, old_selected, BGP_PATH_SELECTED);
		if (new_selected)
			bgp_path_info_set_flag(dest, new_selected, BGP_PATH_SELECTED);
		identity->selected = new_selected;
		if (old_selected)
			store->selected_count--;
		if (new_selected)
			store->selected_count++;
		identity->state = new_selected ? MIDR_RIB_IDENTITY_SELECTED
					       : MIDR_RIB_IDENTITY_NO_PATH;
		bgp_bump_version(dest);
		bgp_midr_rib_route_update_notify(bgp, dest, old_selected,
						 new_selected);
	} else if (new_selected &&
		   CHECK_FLAG(new_selected->flags, BGP_PATH_ATTR_CHANGED)) {
		UNSET_FLAG(new_selected->flags, BGP_PATH_ATTR_CHANGED);
		bgp_bump_version(dest);
		bgp_midr_rib_route_update_notify(bgp, dest, new_selected,
						 new_selected);
	}

	for (path = bgp_dest_get_bgp_path_info(dest); path;
	     path = next) {
		next = path->next;
		if (!CHECK_FLAG(path->flags, BGP_PATH_REMOVED))
			continue;
		assert(identity->path_count && store->path_count);
		if (identity->canonical_path == path)
			identity->canonical_path = NULL;
		identity->path_count--;
		store->path_count--;
		assert(bgp_path_info_reap(dest, path));
	}
	if (!identity->path_count) {
		identity->selected = NULL;
		identity->state = MIDR_RIB_IDENTITY_NO_PATH;
		assert(store->active_identity_count);
		store->active_identity_count--;
	}
	UNSET_FLAG(dest->flags, BGP_NODE_PROCESS_SCHEDULED);
}

int midr_rib_instance_upsert(struct midr_context *ctx, struct peer *peer,
			     const struct midr_instance *instance, uint32_t age_ms)
{
	struct midr_rib_store *store;
	struct midr_rib_identity *identity;
	struct bgp_path_info *old_path;
	struct bgp_path_info *new_path;
	struct bgp_dest *dest = NULL;
	struct attr *new_attr;
	enum midr_canonical_result result;
	bool event_pending;
	bool advertisement_was_present;
	uint64_t old_advertisement_sequence = 0;
	enum midr_instance_state old_advertisement_state = 0;
	struct midr_rib_advertisement *advertisement;
	int ret;

	if (!ctx || !ctx->bgp || !ctx->rib_store)
		return -ENOENT;
	if (!peer || peer->bgp != ctx->bgp || !instance ||
	    midr_instance_validate(instance))
		return -EINVAL;
	if (peer == ctx->bgp->peer_self) {
		if (instance->object.key.originator_node_id !=
		    ctx->bgp->router_id.s_addr)
			return -EINVAL;
	} else if (!peer->remote_id.s_addr ||
		   instance->object.key.originator_node_id ==
			ctx->bgp->router_id.s_addr) {
		return -EINVAL;
	}

	store = ctx->rib_store;
	identity = midr_rib_identity_get(ctx, &instance->object.key, &dest);
	if (!identity)
		return -ENOSPC;
	advertisement = midr_rib_advertisement_lookup(identity, peer);
	advertisement_was_present = advertisement != NULL;
	if (advertisement) {
		old_advertisement_sequence = advertisement->sequence;
		old_advertisement_state = advertisement->state;
	}
	ret = midr_rib_advertisement_update(identity, peer, instance);
	if (ret) {
		bgp_dest_unlock_node(dest);
		return ret;
	}
	/* Build and intern the immutable path attribute before changing the
	 * canonical store.  A failed allocation must leave both views unchanged. */
	new_attr = midr_rib_instance_attr_intern(ctx->bgp, instance, age_ms);
	if (!new_attr) {
		store->rejected_resource++;
		if (!advertisement_was_present)
			midr_rib_advertisement_remove(identity, peer);
		else {
			advertisement = midr_rib_advertisement_lookup(identity, peer);
			advertisement->sequence = old_advertisement_sequence;
			advertisement->state = old_advertisement_state;
		}
		bgp_dest_unlock_node(dest);
		return -ENOMEM;
	}
	ret = midr_canonical_accept(store->canonical, instance, age_ms,
				    &result);
	if (ret) {
		if (ret == -ENOMEM || ret == -ENOSPC)
			store->rejected_resource++;
		else
			store->rejected_internal++;
		bgp_attr_unintern(&new_attr);
		if (!advertisement_was_present)
			midr_rib_advertisement_remove(identity, peer);
		else {
			advertisement = midr_rib_advertisement_lookup(identity, peer);
			advertisement->sequence = old_advertisement_sequence;
			advertisement->state = old_advertisement_state;
		}
		bgp_dest_unlock_node(dest);
		return ret;
	}
	if (result == MIDR_CANONICAL_DUPLICATE ||
	    result == MIDR_CANONICAL_OLDER ||
	    result == MIDR_CANONICAL_EXPIRED) {
		bgp_attr_unintern(&new_attr);
		bgp_dest_unlock_node(dest);
		return 0;
	}
	event_pending = result == MIDR_CANONICAL_ACCEPTED ||
			result == MIDR_CANONICAL_CONFLICT;

	old_path = identity->canonical_path;
	if (result == MIDR_CANONICAL_CONFLICT) {
		bgp_attr_unintern(&new_attr);
		if (old_path && !CHECK_FLAG(old_path->flags, BGP_PATH_REMOVED))
			bgp_path_info_mark_for_delete(dest, old_path);
		identity->canonical_path = NULL;
		if (identity->state != MIDR_RIB_IDENTITY_CONFLICT_QUARANTINED) {
			identity->state = MIDR_RIB_IDENTITY_CONFLICT_QUARANTINED;
			store->conflict_count++;
		}
		bgp_process_main_one(ctx->bgp, dest, AFI_BGP_LS,
				     SAFI_MIDR_LS);
		/* bgp_midr_rib_process_main() clears the selected path and reaps
		 * the old candidate. Preserve the quarantine state after that
		 * transition so it is not mistaken for an ordinary NO_PATH. */
		identity->state = MIDR_RIB_IDENTITY_CONFLICT_QUARANTINED;
		bgp_dest_unlock_node(dest);
		if (event_pending)
			midr_canonical_event_ack(store->canonical);
		return 0;
	}

	if (identity->state == MIDR_RIB_IDENTITY_CONFLICT_QUARANTINED) {
		assert(store->conflict_count);
		store->conflict_count--;
		identity->state = MIDR_RIB_IDENTITY_NO_PATH;
	}
	new_path = info_make(ZEBRA_ROUTE_BGP, BGP_ROUTE_NORMAL, 0,
				     peer, new_attr, dest);
	SET_FLAG(new_path->flags, BGP_PATH_VALID);
	new_path->from = peer;
	bgp_path_info_add(dest, new_path);
	identity->canonical_path = new_path;
	if (!identity->path_count)
		store->active_identity_count++;
	identity->path_count++;
	store->path_count++;
	if (old_path && old_path != new_path &&
	    !CHECK_FLAG(old_path->flags, BGP_PATH_REMOVED))
		bgp_path_info_mark_for_delete(dest, old_path);
	bgp_process_main_one(ctx->bgp, dest, AFI_BGP_LS,
			     SAFI_MIDR_LS);
	bgp_dest_unlock_node(dest);
	midr_canonical_event_ack(store->canonical);
	return 0;
}

int midr_rib_instance_upsert_received(
	struct midr_context *ctx, struct peer *peer,
	const struct midr_instance *instance, uint32_t age_ms,
	uint64_t received_ns)
{
	struct midr_rib_store *store;
	uint32_t current_age;
	uint64_t now_ns;
	int ret;

	if (!ctx || !ctx->rib_store)
		return -ENOENT;
	store = ctx->rib_store;
	now_ns = midr_rib_now_ns(ctx);
	if (!received_ns)
		received_ns = now_ns;
	ret = midr_instance_age(age_ms, received_ns, now_ns, 0,
				midr_rib_max_age_ms(ctx), &current_age);
	if (ret) {
		store->rejected_internal++;
		return ret;
	}
	return midr_rib_instance_upsert(ctx, peer, instance, current_age);
}

int midr_rib_peer_withdraw(struct midr_context *ctx, struct peer *peer,
			   const struct midr_ls_object_key *key)
{
	struct midr_rib_identity *identity;

	if (!ctx || !ctx->bgp || !ctx->rib_store)
		return -ENOENT;
	if (!peer || peer->bgp != ctx->bgp || !key ||
	    midr_ls_object_key_validate(key) != 0)
		return -EINVAL;
	identity = midr_rib_identity_lookup(ctx->rib_store, key);
	if (!identity)
		return -ENOENT;
	/* MP_UNREACH removes only this peer's advertisement relationship.  The
	 * canonical instance remains authoritative until a newer instance arrives. */
	return midr_rib_advertisement_remove(identity, peer) ? 0 : -ENOENT;
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

int midr_rib_selected_instance_get(
	struct midr_context *ctx, const struct midr_ls_object_key *key,
	struct midr_instance *instance, uint32_t *age_ms, struct peer **peer)
{
	struct midr_rib_identity *identity;

	if (!ctx || !ctx->rib_store)
		return -ENOENT;
	if (!key || !instance || !peer)
		return -EINVAL;
	identity = midr_rib_identity_lookup(ctx->rib_store, key);
	if (!identity || !identity->selected)
		return -ENOENT;
	if (midr_rib_path_instance(ctx, identity->dest, identity->selected,
				   instance, age_ms) != 0)
		return -EINVAL;
	/* This accessor is the active-view API. A WITHDRAWN canonical
	 * instance remains available to the flooding path through the selected
	 * entry iterator, but must not be presented as usable state. */
	if (instance->state != MIDR_INSTANCE_ACTIVE)
		return -ENOENT;
	*peer = identity->selected->peer;
	return 0;
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
	struct midr_instance instance;

	if (state->result || !identity->selected)
		return;
	if (midr_rib_path_instance_decode(identity->dest, identity->selected,
					 &instance) != 0) {
		state->result = -EINVAL;
		return;
	}
	if (instance.state != MIDR_INSTANCE_ACTIVE)
		return;
	state->result = state->callback(
		&instance, identity->selected->peer,
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
	struct midr_instance instance;

	if (state->result || !identity->selected)
		return;
	if (midr_rib_path_instance_decode(identity->dest, identity->selected,
					 &instance) != 0) {
		state->result = -EINVAL;
		return;
	}
	state->result = state->callback(
		&instance, identity->selected->peer,
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
		/* The identity limit includes floors and quarantined identities, not
		 * only objects currently usable by the active view. */
		.identity_count = store->identities->count,
		.path_count = store->path_count,
		.selected_count = store->selected_count,
		.conflict_count = store->conflict_count,
		.identity_limit = store->identity_limit,
		.rejected_limit = store->rejected_limit,
		.rejected_payload_conflict =
			store->rejected_payload_conflict,
		.rejected_resource = store->rejected_resource,
		.rejected_internal = store->rejected_internal,
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

static const char *midr_rib_canonical_state_name(
		enum midr_canonical_state state)
{
	switch (state) {
	case MIDR_CANONICAL_CURRENT:
		return "CURRENT";
	case MIDR_CANONICAL_QUARANTINED:
		return "QUARANTINED";
	case MIDR_CANONICAL_FLOOR:
		return "FLOOR";
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
	struct midr_canonical_view canonical = {};
	int canonical_ret;

	canonical_ret = midr_canonical_lookup(show->ctx->rib_store->canonical,
					     &identity->key, &canonical);
	if (!identity->path_count && canonical_ret)
		return;
	show->identity_count++;
	vty_out(show->vty,
		"identity synthetic-id=%u state=%s paths=%zu",
		identity->synthetic_id,
		midr_rib_identity_state_name(identity->state),
		identity->path_count);
	if (!canonical_ret) {
		uint32_t remaining = canonical.age_ms <
					     midr_canonical_max_age_ms(
						     show->ctx->rib_store->canonical)
						 ? midr_canonical_max_age_ms(
							   show->ctx->rib_store->canonical) -
							   canonical.age_ms
						 : 0;

		vty_out(show->vty, " canonical=%s sequence=%" PRIu64
				 " age-ms=%u remaining-ms=%u\n",
				midr_rib_canonical_state_name(canonical.state),
				canonical.sequence, canonical.age_ms, remaining);
	} else {
		vty_out(show->vty, " canonical=UNAVAILABLE\n");
	}
	for (path = bgp_dest_get_bgp_path_info(identity->dest); path;
	     path = path->next) {
		struct midr_ls_object object;
		struct in_addr peer_id;

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
		vty_out(show->vty, " state=%s age-ms=%u\n",
			bgp_midr_ls_attr_state(path->attr->midr_ls) ==
				MIDR_INSTANCE_ACTIVE ? "ACTIVE" : "WITHDRAWN",
			bgp_midr_ls_attr_age(path->attr->midr_ls));
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
	if (!limit || limit > MIDR_RIB_MAX_IDENTITIES)
		return -EINVAL;
	/* Tests may lower the limit below the live identity count to drive
	 * the new-identity rejection path; existing identities stay usable. */
	ctx->rib_store->identity_limit = limit;
	return 0;
}

int midr_rib_test_set_now_ns(struct midr_context *ctx, uint64_t now_ns)
{
	if (!ctx || !ctx->rib_store)
		return -ENOENT;
	ctx->rib_store->test_clock_enabled = true;
	ctx->rib_store->test_now_ns = now_ns;
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
