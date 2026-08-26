// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR selected-object database and production TED derivation.
 */

#include <zebra.h>

#include <errno.h>

#include "command.h"
#include "hash.h"
#include "jhash.h"
#include "linklist.h"
#include "memory.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_midr_attr.h"
#include "bgpd/bgp_midr_lsdb.h"
#include "bgpd/bgp_midr_owned.h"
#include "bgpd/bgp_midr_prefix.h"
#include "bgpd/bgp_midr_private.h"
#include "bgpd/bgp_midr_rib.h"
#include "bgpd/bgp_midr_sync.h"
#include "bgpd/bgp_midr_ted_private.h"
#include "bgpd/bgp_route.h"
#include "bgpd/bgp_updgrp.h"

#define MIDR_LSDB_RETRY_MSEC 1000U

DEFINE_MTYPE_STATIC(BGPD, MIDR_LSDB_STORE, "MIDR LSDB store");
DEFINE_MTYPE_STATIC(BGPD, MIDR_LSDB_STATE, "MIDR LSDB state");
DEFINE_MTYPE_STATIC(BGPD, MIDR_LSDB_ENTRY, "MIDR LSDB entry");
DEFINE_MTYPE_STATIC(BGPD, MIDR_LSDB_INDEX, "MIDR LSDB index");
DEFINE_MTYPE_STATIC(BGPD, MIDR_LSDB_DIRTY, "MIDR LSDB dirty entry");
DEFINE_MTYPE_STATIC(BGPD, MIDR_LSDB_REMOTE, "MIDR remote view");

struct midr_lsdb_entry {
	struct midr_ls_object object;
	struct peer *peer;
	struct bgp_dest *dest;
	struct bgp_path_info *selected;
	enum midr_lsdb_scope scope;
	uint32_t scope_group_id;
	uint32_t canonical_cost;
	ifindex_t local_ifindex;
	enum midr_lsdb_pending_reason pending_reason;
	bool usable;
};

struct midr_lsdb_endpoint_index {
	uint32_t node_id;
	struct midr_ls_object_key link_key;
	struct midr_lsdb_entry *entry;
};

struct midr_lsdb_group_index {
	uint32_t group_id;
	uint32_t representative_node_id;
	size_t member_count;
};

struct midr_lsdb_contributor_index {
	uint32_t group_id;
	struct midr_ls_prefix_key prefix;
	size_t contributor_count;
};

struct midr_lsdb_state {
	struct hash *identities;
	struct hash *memberships;
	struct hash *link_endpoints;
	struct hash *group_members;
	struct hash *groups;
	struct hash *contributors;
	struct hash *pending_links;
	uint64_t generation;
	uint64_t sync_reason_flags;
	uint32_t local_group_id;
	bool ready;
	size_t usable_count;
	size_t pending_count;
	size_t membership_count;
	size_t link_count;
	size_t node_prefix_count;
	size_t group_prefix_count;
};

struct midr_lsdb_dirty {
	struct bgp_dest *dest;
	struct bgp_path_info *old_selected;
	struct bgp_path_info *new_selected;
};

struct midr_lsdb_store {
	struct midr_context *ctx;
	struct midr_lsdb_state *current;
	struct list *dirty;
	struct event *t_commit;
	bool force_rebuild;
	bool scope_rebuild;
	bool fail_next_prepare;
	uint64_t commit_count;
	uint64_t failure_count;
	int last_error;
};

static unsigned int midr_lsdb_identity_hash_key(const void *arg)
{
	const struct midr_lsdb_entry *entry = arg;

	return midr_ls_object_key_hash(&entry->object.key);
}

static bool midr_lsdb_identity_hash_cmp(const void *a, const void *b)
{
	const struct midr_lsdb_entry *left = a;
	const struct midr_lsdb_entry *right = b;

	return midr_ls_object_key_same(&left->object.key,
				       &right->object.key);
}

static unsigned int midr_lsdb_membership_hash_key(const void *arg)
{
	const struct midr_lsdb_entry *entry = arg;

	return jhash_1word(entry->object.key.originator_node_id, 0);
}

static bool midr_lsdb_membership_hash_cmp(const void *a, const void *b)
{
	const struct midr_lsdb_entry *left = a;
	const struct midr_lsdb_entry *right = b;

	return left->object.key.originator_node_id ==
	       right->object.key.originator_node_id;
}

static unsigned int midr_lsdb_group_member_hash_key(const void *arg)
{
	const struct midr_lsdb_entry *entry = arg;
	uint32_t words[2] = {
		entry->object.payload.membership.group_id,
		entry->object.key.originator_node_id,
	};

	return jhash2(words, array_size(words), 0);
}

static bool midr_lsdb_group_member_hash_cmp(const void *a, const void *b)
{
	const struct midr_lsdb_entry *left = a;
	const struct midr_lsdb_entry *right = b;

	return left->object.payload.membership.group_id ==
		       right->object.payload.membership.group_id &&
	       left->object.key.originator_node_id ==
		       right->object.key.originator_node_id;
}

static unsigned int midr_lsdb_group_hash_key(const void *arg)
{
	const struct midr_lsdb_group_index *group = arg;

	return jhash_1word(group->group_id, 0);
}

static bool midr_lsdb_group_hash_cmp(const void *a, const void *b)
{
	const struct midr_lsdb_group_index *left = a;
	const struct midr_lsdb_group_index *right = b;

	return left->group_id == right->group_id;
}

static unsigned int midr_lsdb_contributor_hash_key(const void *arg)
{
	const struct midr_lsdb_contributor_index *index = arg;
	uint32_t words[3] = {
		index->group_id,
		((uint32_t)index->prefix.afi << 16) | index->prefix.safi,
		prefix_hash_key(&index->prefix.prefix),
	};

	return jhash2(words, array_size(words), 0);
}

static bool midr_lsdb_contributor_hash_cmp(const void *a, const void *b)
{
	const struct midr_lsdb_contributor_index *left = a;
	const struct midr_lsdb_contributor_index *right = b;

	return left->group_id == right->group_id && left->prefix.afi == right->prefix.afi &&
	       left->prefix.safi == right->prefix.safi &&
	       prefix_same(&left->prefix.prefix, &right->prefix.prefix);
}

static void *midr_lsdb_contributor_hash_alloc(void *arg)
{
	const struct midr_lsdb_contributor_index *source = arg;
	struct midr_lsdb_contributor_index *index;

	index = XCALLOC(MTYPE_MIDR_LSDB_INDEX, sizeof(*index));
	*index = *source;
	return index;
}

static void *midr_lsdb_group_hash_alloc(void *arg)
{
	const struct midr_lsdb_group_index *source = arg;
	struct midr_lsdb_group_index *group;

	group = XCALLOC(MTYPE_MIDR_LSDB_INDEX, sizeof(*group));
	group->group_id = source->group_id;
	return group;
}

static unsigned int midr_lsdb_endpoint_hash_key(const void *arg)
{
	const struct midr_lsdb_endpoint_index *index = arg;

	return jhash_2words(index->node_id,
			    midr_ls_object_key_hash(&index->link_key), 0);
}

static bool midr_lsdb_endpoint_hash_cmp(const void *a, const void *b)
{
	const struct midr_lsdb_endpoint_index *left = a;
	const struct midr_lsdb_endpoint_index *right = b;

	return left->node_id == right->node_id &&
	       midr_ls_object_key_same(&left->link_key, &right->link_key);
}

static void midr_lsdb_entry_free(void *arg)
{
	struct midr_lsdb_entry *entry = arg;

	if (entry->selected)
		bgp_path_info_unlock(entry->selected);
	if (entry->dest)
		bgp_dest_unlock_node(entry->dest);
	XFREE(MTYPE_MIDR_LSDB_ENTRY, entry);
}

static void midr_lsdb_index_free(void *arg)
{
	XFREE(MTYPE_MIDR_LSDB_INDEX, arg);
}

static struct midr_lsdb_state *midr_lsdb_state_new(void)
{
	struct midr_lsdb_state *state;

	state = XCALLOC(MTYPE_MIDR_LSDB_STATE, sizeof(*state));
	state->identities = hash_create(midr_lsdb_identity_hash_key,
					midr_lsdb_identity_hash_cmp,
					"MIDR LSDB identities");
	state->memberships = hash_create(midr_lsdb_membership_hash_key,
					 midr_lsdb_membership_hash_cmp,
					 "MIDR LSDB memberships");
	state->link_endpoints = hash_create(midr_lsdb_endpoint_hash_key,
					    midr_lsdb_endpoint_hash_cmp,
					    "MIDR LSDB link endpoints");
	state->group_members = hash_create(midr_lsdb_group_member_hash_key,
					   midr_lsdb_group_member_hash_cmp,
					   "MIDR LSDB group members");
	state->groups = hash_create(midr_lsdb_group_hash_key,
				    midr_lsdb_group_hash_cmp,
				    "MIDR LSDB groups");
	state->contributors = hash_create(midr_lsdb_contributor_hash_key,
					  midr_lsdb_contributor_hash_cmp,
					  "MIDR LSDB prefix contributors");
	state->pending_links = hash_create(midr_lsdb_identity_hash_key,
					   midr_lsdb_identity_hash_cmp,
					   "MIDR LSDB pending links");
	return state;
}

static void midr_lsdb_state_free(struct midr_lsdb_state **statep)
{
	struct midr_lsdb_state *state;

	if (!statep || !*statep)
		return;
	state = *statep;
	hash_clean_and_free(&state->memberships, NULL);
	hash_clean_and_free(&state->link_endpoints, midr_lsdb_index_free);
	hash_clean_and_free(&state->group_members, NULL);
	hash_clean_and_free(&state->groups, midr_lsdb_index_free);
	hash_clean_and_free(&state->contributors, midr_lsdb_index_free);
	hash_clean_and_free(&state->pending_links, NULL);
	hash_clean_and_free(&state->identities, midr_lsdb_entry_free);
	XFREE(MTYPE_MIDR_LSDB_STATE, state);
	*statep = NULL;
}

static struct midr_lsdb_entry *
midr_lsdb_membership_lookup(struct midr_lsdb_state *state,
			    uint32_t node_id)
{
	struct midr_lsdb_entry lookup = {
		.object.key.originator_node_id = node_id,
	};

	return hash_lookup(state->memberships, &lookup);
}

static struct midr_lsdb_group_index *
midr_lsdb_group_lookup(struct midr_lsdb_state *state, uint32_t group_id)
{
	struct midr_lsdb_group_index lookup = {
		.group_id = group_id,
	};

	return hash_lookup(state->groups, &lookup);
}

static int midr_lsdb_capture_selected(
	const struct midr_ls_object *object,
	const struct midr_propagation_path *path, struct peer *peer,
	struct bgp_dest *dest, struct bgp_path_info *selected, void *arg)
{
	struct midr_lsdb_state *state = arg;
	struct midr_lsdb_entry *entry;

	(void)path;
	entry = XCALLOC(MTYPE_MIDR_LSDB_ENTRY, sizeof(*entry));
	entry->object = *object;
	entry->peer = peer;
	entry->dest = bgp_dest_lock_node(dest);
	entry->selected = bgp_path_info_lock(selected);
	if (hash_get(state->identities, entry, hash_alloc_intern) != entry) {
		midr_lsdb_entry_free(entry);
		return -EEXIST;
	}

	switch (object->key.type) {
	case MIDR_NLRI_TYPE_MEMBERSHIP:
		state->membership_count++;
		break;
	case MIDR_NLRI_TYPE_LINK:
		state->link_count++;
		break;
	case MIDR_NLRI_TYPE_NODE_PREFIX:
		state->node_prefix_count++;
		break;
	case MIDR_NLRI_TYPE_GROUP_PREFIX:
		state->group_prefix_count++;
		break;
	case MIDR_NLRI_TYPE_RESERVED:
		return -EINVAL;
	}
	return 0;
}

static void midr_lsdb_index_membership(struct hash_bucket *bucket,
				       void *arg)
{
	struct midr_lsdb_state *state = arg;
	struct midr_lsdb_entry *entry = bucket->data;
	struct midr_lsdb_group_index candidate;
	struct midr_lsdb_group_index *group;

	if (entry->object.key.type != MIDR_NLRI_TYPE_MEMBERSHIP ||
	    !entry->object.payload.membership.group_id)
		return;
	entry->usable = true;
	entry->scope = MIDR_LSDB_SCOPE_GLOBAL;
	state->usable_count++;
	(void)hash_get(state->memberships, entry, hash_alloc_intern);
	(void)hash_get(state->group_members, entry, hash_alloc_intern);
	candidate.group_id = entry->object.payload.membership.group_id;
	group = hash_get(state->groups, &candidate,
			 midr_lsdb_group_hash_alloc);
	group->member_count++;
	if (!group->representative_node_id ||
	    ntohl(entry->object.key.originator_node_id) < ntohl(group->representative_node_id))
		group->representative_node_id = entry->object.key.originator_node_id;
}

static void midr_lsdb_contributor_add(struct midr_lsdb_state *state, uint32_t group_id,
				      const struct midr_ls_prefix_key *prefix)
{
	struct midr_lsdb_contributor_index candidate = {
		.group_id = group_id,
		.prefix = *prefix,
	};
	struct midr_lsdb_contributor_index *index;

	index = hash_get(state->contributors, &candidate, midr_lsdb_contributor_hash_alloc);
	index->contributor_count++;
}

static int midr_lsdb_endpoint_add(struct midr_lsdb_state *state,
				  struct midr_lsdb_entry *entry,
				  uint32_t node_id)
{
	struct midr_lsdb_endpoint_index *index;

	index = XCALLOC(MTYPE_MIDR_LSDB_INDEX, sizeof(*index));
	index->node_id = node_id;
	index->link_key = entry->object.key;
	index->entry = entry;
	if (hash_get(state->link_endpoints, index,
		     hash_alloc_intern) != index) {
		midr_lsdb_index_free(index);
		return -EEXIST;
	}
	return 0;
}

static void midr_lsdb_derive_entry(struct hash_bucket *bucket, void *arg)
{
	struct midr_lsdb_state *state = arg;
	struct midr_lsdb_entry *entry = bucket->data;
	struct midr_lsdb_entry *local_membership;
	struct midr_lsdb_entry *remote_membership;

	if (entry->object.key.type == MIDR_NLRI_TYPE_MEMBERSHIP)
		return;

	switch (entry->object.key.type) {
	case MIDR_NLRI_TYPE_LINK:
		local_membership = midr_lsdb_membership_lookup(
			state, entry->object.key.originator_node_id);
		remote_membership = midr_lsdb_membership_lookup(
			state, entry->object.key.u.link.remote_node_id);
		(void)midr_lsdb_endpoint_add(
			state, entry,
			entry->object.key.originator_node_id);
		(void)midr_lsdb_endpoint_add(
			state, entry,
			entry->object.key.u.link.remote_node_id);
		if (!local_membership)
			entry->pending_reason =
				MIDR_LSDB_PENDING_LOCAL_MEMBERSHIP;
		else if (!remote_membership)
			entry->pending_reason =
				MIDR_LSDB_PENDING_REMOTE_MEMBERSHIP;
		else {
			uint32_t local_group =
				local_membership->object.payload.membership.group_id;
			uint32_t remote_group =
				remote_membership->object.payload.membership.group_id;

			entry->canonical_cost =
				entry->object.payload.link.canonical_cost;
			if (local_group == remote_group) {
				entry->scope = MIDR_LSDB_SCOPE_INTRA_GROUP;
				entry->scope_group_id = local_group;
				entry->usable = state->local_group_id == local_group;
				if (!entry->usable)
					entry->pending_reason =
						MIDR_LSDB_PENDING_SCOPE_INELIGIBLE;
			} else {
				entry->scope = MIDR_LSDB_SCOPE_GLOBAL;
				entry->usable = true;
			}
		}
		if (!entry->usable)
			(void)hash_get(state->pending_links, entry,
				       hash_alloc_intern);
		break;
	case MIDR_NLRI_TYPE_NODE_PREFIX:
		local_membership = midr_lsdb_membership_lookup(
			state, entry->object.key.originator_node_id);
		if (!local_membership)
			entry->pending_reason =
				MIDR_LSDB_PENDING_LOCAL_MEMBERSHIP;
		else {
			entry->scope = MIDR_LSDB_SCOPE_INTRA_GROUP;
			entry->scope_group_id =
				local_membership->object.payload.membership.group_id;
			entry->usable = state->local_group_id == entry->scope_group_id;
			if (!entry->usable)
				entry->pending_reason = MIDR_LSDB_PENDING_SCOPE_INELIGIBLE;
			else
				midr_lsdb_contributor_add(state, entry->scope_group_id,
							  &entry->object.key.u.node_prefix);
		}
		break;
	case MIDR_NLRI_TYPE_GROUP_PREFIX: {
		struct midr_lsdb_group_index *group =
			midr_lsdb_group_lookup(
				state,
				entry->object.key.u.group_prefix.group_id);

		if (!group)
			entry->pending_reason = MIDR_LSDB_PENDING_GROUP_MEMBERSHIP;
		else if (entry->object.key.originator_node_id !=
			 group->representative_node_id)
			entry->pending_reason = MIDR_LSDB_PENDING_SCOPE_INELIGIBLE;
		else {
			entry->usable = true;
			entry->scope = MIDR_LSDB_SCOPE_GLOBAL;
		}
	} break;
	case MIDR_NLRI_TYPE_RESERVED:
	case MIDR_NLRI_TYPE_MEMBERSHIP:
		break;
	}

	if (entry->usable)
		state->usable_count++;
	else
		state->pending_count++;
}

static void midr_lsdb_local_metadata(struct hash_bucket *bucket, void *arg)
{
	struct midr_context *ctx = arg;
	struct midr_lsdb_entry *entry = bucket->data;

	if (entry->object.key.type != MIDR_NLRI_TYPE_LINK ||
	    entry->object.key.originator_node_id !=
		    ctx->bgp->router_id.s_addr)
		return;
	(void)midr_owned_link_metadata_get(
		ctx, &entry->object.key, &entry->local_ifindex);
}

static int midr_lsdb_build_state(struct midr_context *ctx,
				 struct midr_lsdb_state **out)
{
	struct midr_lsdb_entry *local_membership;
	struct midr_lsdb_state *state;
	int ret;

	if (!out || *out)
		return -EINVAL;
	state = midr_lsdb_state_new();
	ret = midr_rib_selected_entry_foreach(
		ctx, midr_lsdb_capture_selected, state);
	if (ret)
		goto fail;
	hash_iterate(state->identities, midr_lsdb_index_membership,
		     state);
	local_membership = midr_lsdb_membership_lookup(
		state, ctx->bgp->router_id.s_addr);

	if (local_membership)
		state->local_group_id =
			local_membership->object.payload.membership.group_id;
	hash_iterate(state->identities, midr_lsdb_derive_entry, state);
	hash_iterate(state->identities, midr_lsdb_local_metadata, ctx);
	*out = state;
	return 0;

fail:
	midr_lsdb_state_free(&state);
	return ret;
}

static void midr_lsdb_prefix_key_to_ted(
	const struct midr_ls_prefix_key *source,
	struct midr_ted_prefix_key *target)
{
	target->afi = source->afi;
	target->safi = source->safi;
	target->prefix = source->prefix;
}

struct midr_lsdb_ted_build {
	struct midr_ted_builder *builder;
	int result;
};

static void midr_lsdb_add_ted_entry(struct hash_bucket *bucket, void *arg)
{
	struct midr_lsdb_ted_build *build = arg;
	struct midr_lsdb_entry *entry = bucket->data;

	if (build->result || !entry->usable)
		return;
	switch (entry->object.key.type) {
	case MIDR_NLRI_TYPE_MEMBERSHIP: {
		struct midr_ted_node node = {
			.node_id = entry->object.key.originator_node_id,
			.group_id =
				entry->object.payload.membership.group_id,
			.cap_flags =
				entry->object.payload.membership.cap_flags,
			.policy_tags = entry->object.policy_tags,
		};

		build->result =
			midr_ted_builder_add_node(build->builder, &node);
		break;
	}
	case MIDR_NLRI_TYPE_LINK: {
		struct midr_ted_link_input link = {
			.local_node_id =
				entry->object.key.originator_node_id,
			.remote_node_id =
				entry->object.key.u.link.remote_node_id,
			.link_id = entry->object.key.u.link.link_id,
			.canonical_cost = entry->canonical_cost,
			.policy_tags = entry->object.policy_tags,
			.link_local_address =
				entry->object.payload.link
					.link_local_address,
			.link_remote_address =
				entry->object.payload.link
					.link_remote_address,
			.local_ifindex = entry->local_ifindex,
		};

		build->result =
			midr_ted_builder_add_link(build->builder, &link);
		break;
	}
	case MIDR_NLRI_TYPE_NODE_PREFIX: {
		struct midr_ted_node_prefix prefix = {
			.node_id = entry->object.key.originator_node_id,
		};

		midr_lsdb_prefix_key_to_ted(
			&entry->object.key.u.node_prefix, &prefix.key);
		build->result = midr_ted_builder_add_node_prefix(
			build->builder, &prefix);
		break;
	}
	case MIDR_NLRI_TYPE_GROUP_PREFIX: {
		struct midr_ted_prefix_group prefix = {
			.group_id =
				entry->object.key.u.group_prefix.group_id,
		};

		midr_lsdb_prefix_key_to_ted(
			&entry->object.key.u.group_prefix.prefix,
			&prefix.key);
		build->result = midr_ted_builder_add_prefix_group(
			build->builder, &prefix);
		break;
	}
	case MIDR_NLRI_TYPE_RESERVED:
		build->result = -EINVAL;
		break;
	}
}

static int midr_lsdb_prepare_ted(
	struct midr_context *ctx, struct midr_lsdb_state *state,
	struct midr_ted_prepared **prepared)
{
	struct midr_lsdb_entry lookup = {
		.object.key =
			{
				.type = MIDR_NLRI_TYPE_MEMBERSHIP,
				.originator_node_id =
					ctx->bgp->router_id.s_addr,
			},
	};
	struct midr_lsdb_ted_build build = {};
	struct midr_input_status input = {};
	struct midr_lsdb_entry *local;
	uint64_t reasons = 0;
	int ret;

	(void)midr_input_status_get(ctx, &input);
	if (input.state == MIDR_INPUT_OUT_OF_SYNC)
		reasons |= MIDR_TED_SYNC_REASON_RESYNC_FAILED;
	if (!midr_prefix_is_ready(ctx))
		reasons |= MIDR_TED_SYNC_REASON_RESYNC_FAILED;
	state->sync_reason_flags = reasons;

	local = hash_lookup(state->identities, &lookup);
	if (input.state == MIDR_INPUT_IDENTITY_RESTART || !local ||
	    !local->usable || local->peer != ctx->bgp->peer_self) {
		(void)midr_sync_view_ready(ctx, false, &reasons);
		state->ready = false;
		return midr_ted_prepare_not_ready(
			ctx, ctx->bgp->router_id.s_addr, reasons, prepared);
	}
	if (!midr_prefix_is_ready(ctx)) {
		state->ready = false;
		return midr_ted_prepare_not_ready(ctx, ctx->bgp->router_id.s_addr, reasons,
						  prepared);
	}
	if (!midr_sync_view_ready(ctx, true, &reasons)) {
		state->sync_reason_flags = reasons;
		state->ready = false;
		return midr_ted_prepare_not_ready(
			ctx, ctx->bgp->router_id.s_addr, reasons, prepared);
	}
	state->sync_reason_flags = reasons;

	ret = midr_ted_builder_create(
		ctx->bgp->router_id.s_addr,
		local->object.payload.membership.group_id, &build.builder);
	if (ret)
		return ret;
	hash_iterate(state->identities, midr_lsdb_add_ted_entry, &build);
	if (build.result) {
		midr_ted_builder_destroy(&build.builder);
		return build.result;
	}
	ret = midr_ted_prepare_ready(ctx, build.builder, reasons, prepared);
	midr_ted_builder_destroy(&build.builder);
	if (!ret)
		state->ready = true;
	return ret;
}

static bool midr_lsdb_entry_same(const struct midr_lsdb_entry *left,
				 const struct midr_lsdb_entry *right)
{
	return midr_ls_object_same(&left->object, &right->object) &&
	       left->usable == right->usable &&
	       left->pending_reason == right->pending_reason &&
	       left->scope == right->scope &&
	       left->scope_group_id == right->scope_group_id &&
	       left->canonical_cost == right->canonical_cost &&
	       left->local_ifindex == right->local_ifindex &&
	       left->peer == right->peer;
}

static struct midr_lsdb_entry *
midr_lsdb_state_entry(struct midr_lsdb_state *state,
		      const struct midr_ls_object_key *key);

bool midr_lsdb_export_eligible(struct midr_context *ctx,
			      const struct bgp_dest *dest,
			      const struct bgp_path_info *path,
			      const struct peer *target)
{
	const struct midr_propagation_path *propagation;
	const struct midr_ls_object_key *key;
	struct midr_lsdb_entry *target_membership;
	struct midr_lsdb_entry *entry;
	struct midr_lsdb_state *state;

	if (!ctx || !ctx->lsdb_store || !dest || !path || !target ||
	    target->bgp != ctx->bgp || target == ctx->bgp->peer_self ||
	    !target->remote_id.s_addr ||
	    target->remote_id.s_addr == ctx->bgp->router_id.s_addr)
		return false;
	state = ctx->lsdb_store->current;
	key = midr_rib_dest_key(dest);
	if (!state || !key)
		return false;
	entry = midr_lsdb_state_entry(state, key);
	if (!entry || !entry->usable || entry->selected != path ||
	    entry->scope == MIDR_LSDB_SCOPE_LOCAL_ONLY)
		return false;

	propagation = bgp_midr_propagation_path_attr_value(
		path->attr->midr_propagation_path);
	if (!propagation ||
	    midr_propagation_path_contains(propagation,
					   target->remote_id.s_addr))
		return false;
	if (entry->scope == MIDR_LSDB_SCOPE_GLOBAL)
		return true;

	target_membership = midr_lsdb_membership_lookup(state, target->remote_id.s_addr);
	return target_membership &&
	       target_membership->object.payload.membership.group_id == entry->scope_group_id;
}

static void midr_lsdb_announce_dirty(struct midr_lsdb_store *store)
{
	struct listnode *node;
	struct midr_lsdb_dirty *dirty;

	for (ALL_LIST_ELEMENTS_RO(store->dirty, node, dirty)) {
		struct bgp_path_info *selected = NULL;
		struct bgp_path_info *path;

		for (path = bgp_dest_get_bgp_path_info(dirty->dest); path;
		     path = path->next)
			if (CHECK_FLAG(path->flags, BGP_PATH_SELECTED)) {
				selected = path;
				break;
			}
		group_announce_route(store->ctx->bgp, AFI_BGP_LS,
				     SAFI_MIDR_LS, dirty->dest, selected);
	}
}

static int midr_lsdb_announce_selected(
	const struct midr_ls_object *object,
	const struct midr_propagation_path *path, struct peer *peer,
	struct bgp_dest *dest, struct bgp_path_info *selected, void *arg)
{
	struct midr_lsdb_store *store = arg;

	(void)object;
	(void)path;
	(void)peer;
	group_announce_route(store->ctx->bgp, AFI_BGP_LS, SAFI_MIDR_LS,
			     dest, selected);
	return 0;
}

static void midr_lsdb_announce_committed(struct midr_lsdb_store *store)
{
	if (store->scope_rebuild)
		(void)midr_rib_selected_entry_foreach(
			store->ctx, midr_lsdb_announce_selected, store);
	midr_lsdb_announce_dirty(store);
	store->scope_rebuild = false;
}

struct midr_lsdb_compare {
	struct midr_lsdb_state *other;
	bool same;
};

static void midr_lsdb_compare_entry(struct hash_bucket *bucket, void *arg)
{
	struct midr_lsdb_compare *compare = arg;
	struct midr_lsdb_entry *entry = bucket->data;
	struct midr_lsdb_entry *other;

	if (!compare->same)
		return;
	other = hash_lookup(compare->other->identities, entry);
	if (!other || !midr_lsdb_entry_same(entry, other))
		compare->same = false;
}

static bool midr_lsdb_state_same(struct midr_lsdb_state *left,
				 struct midr_lsdb_state *right)
{
	struct midr_lsdb_compare compare = {
		.other = right,
		.same = true,
	};

	if (!left || !right)
		return left == right;
	if (left->identities->count != right->identities->count ||
	    left->ready != right->ready ||
	    left->sync_reason_flags != right->sync_reason_flags)
		return false;
	hash_iterate(left->identities, midr_lsdb_compare_entry, &compare);
	return compare.same;
}

static struct midr_lsdb_entry *
midr_lsdb_state_entry(struct midr_lsdb_state *state,
		      const struct midr_ls_object_key *key)
{
	struct midr_lsdb_entry lookup = {
		.object.key = *key,
	};

	return state ? hash_lookup(state->identities, &lookup) : NULL;
}

struct midr_lsdb_remote_notify {
	struct midr_lsdb_store *store;
	struct midr_lsdb_state *other;
};

static void midr_lsdb_remote_withdraw_iter(struct hash_bucket *bucket,
					   void *arg)
{
	struct midr_lsdb_remote_notify *notify = arg;
	struct midr_lsdb_store *store = notify->store;
	struct midr_lsdb_entry *old = bucket->data;
	struct midr_lsdb_entry *current;
	const struct midr_remote_view_callbacks *callbacks;
	struct midr_link_key key;

	if (!old->usable ||
	    old->object.key.originator_node_id ==
		    store->ctx->bgp->router_id.s_addr ||
	    (old->object.key.type != MIDR_NLRI_TYPE_MEMBERSHIP &&
	     old->object.key.type != MIDR_NLRI_TYPE_LINK))
		return;
	current = midr_lsdb_state_entry(notify->other, &old->object.key);
	if (current && current->usable)
		return;
	if (!store->ctx->midr->remote_callbacks_registered)
		return;
	callbacks = &store->ctx->midr->remote_callbacks;
	if (old->object.key.type == MIDR_NLRI_TYPE_MEMBERSHIP) {
		if (callbacks->remote_node_withdraw)
			callbacks->remote_node_withdraw(
				old->object.key.originator_node_id,
				old->object.ls_sequence);
		return;
	}
	key.local_node_id = old->object.key.originator_node_id;
	key.remote_node_id = old->object.key.u.link.remote_node_id;
	key.link_id = old->object.key.u.link.link_id;
	if (callbacks->remote_link_withdraw)
		callbacks->remote_link_withdraw(
			&key, old->object.ls_sequence);
}

static void midr_lsdb_remote_update_iter(struct hash_bucket *bucket,
					 void *arg)
{
	struct midr_lsdb_remote_notify *notify = arg;
	struct midr_lsdb_store *store = notify->store;
	struct midr_lsdb_entry *entry = bucket->data;
	struct midr_lsdb_entry *old;
	const struct midr_remote_view_callbacks *callbacks;

	if (!entry->usable ||
	    entry->object.key.originator_node_id ==
		    store->ctx->bgp->router_id.s_addr ||
	    (entry->object.key.type != MIDR_NLRI_TYPE_MEMBERSHIP &&
	     entry->object.key.type != MIDR_NLRI_TYPE_LINK) ||
	    !store->ctx->midr->remote_callbacks_registered)
		return;
	old = midr_lsdb_state_entry(notify->other, &entry->object.key);
	if (old && old->usable &&
	    midr_ls_object_same(&old->object, &entry->object))
		return;
	callbacks = &store->ctx->midr->remote_callbacks;
	if (entry->object.key.type == MIDR_NLRI_TYPE_MEMBERSHIP) {
		struct midr_remote_node_info node = {
			.node_id =
				entry->object.key.originator_node_id,
			.group_id =
				entry->object.payload.membership.group_id,
			.has_transport_address =
				entry->object.payload.membership
					.has_transport_address,
			.transport_address =
				entry->object.payload.membership
					.transport_address,
			.cap_flags =
				entry->object.payload.membership.cap_flags,
			.policy_tags = entry->object.policy_tags,
			.ls_sequence = entry->object.ls_sequence,
		};

		if (callbacks->remote_node_update)
			callbacks->remote_node_update(&node);
		return;
	}
	if (callbacks->remote_link_update) {
		struct midr_remote_link_info link = {
			.key =
				{
					.local_node_id =
						entry->object.key
							.originator_node_id,
					.remote_node_id =
						entry->object.key.u.link
							.remote_node_id,
					.link_id =
						entry->object.key.u.link
							.link_id,
				},
			.link_local_address =
				entry->object.payload.link
					.link_local_address,
			.link_remote_address =
				entry->object.payload.link
					.link_remote_address,
			.canonical_cost =
				entry->object.payload.link.canonical_cost,
			.policy_tags = entry->object.policy_tags,
			.ls_sequence = entry->object.ls_sequence,
		};

		callbacks->remote_link_update(&link);
	}
}

static void midr_lsdb_dirty_free(void *arg)
{
	struct midr_lsdb_dirty *dirty = arg;

	if (dirty->old_selected)
		bgp_path_info_unlock(dirty->old_selected);
	if (dirty->new_selected)
		bgp_path_info_unlock(dirty->new_selected);
	if (dirty->dest)
		bgp_dest_unlock_node(dirty->dest);
	XFREE(MTYPE_MIDR_LSDB_DIRTY, dirty);
}

static void midr_lsdb_dirty_clear(struct midr_lsdb_store *store)
{
	list_delete_all_node(store->dirty);
	store->force_rebuild = false;
}

static int midr_lsdb_process(struct midr_lsdb_store *store)
{
	struct midr_ted_prepared *prepared = NULL;
	struct midr_lsdb_state *staging = NULL;
	struct midr_lsdb_state *old;
	struct midr_lsdb_remote_notify notify;
	bool changed;
	int ret;

	if (!store || (!listcount(store->dirty) &&
		       !store->force_rebuild))
		return 0;
	ret = midr_lsdb_build_state(store->ctx, &staging);
	if (ret)
		goto fail;
	if (store->fail_next_prepare) {
		store->fail_next_prepare = false;
		ret = -ENOMEM;
		goto fail;
	}
	ret = midr_lsdb_prepare_ted(store->ctx, staging, &prepared);
	if (ret)
		goto fail;

	old = store->current;
	changed = !midr_lsdb_state_same(old, staging);
	if (changed) {
		if (old && old->generation == UINT64_MAX) {
			ret = -EOVERFLOW;
			goto fail;
		}
		staging->generation = old ? old->generation + 1 : 1;
		store->current = staging;
		staging = NULL;
		store->commit_count++;
	}
	midr_ted_prepared_commit(&prepared);
	midr_lsdb_announce_committed(store);
	midr_lsdb_dirty_clear(store);
	if (changed) {
		notify.store = store;
		notify.other = store->current;
		if (old)
			hash_iterate(old->identities,
				     midr_lsdb_remote_withdraw_iter,
				     &notify);
		notify.other = old;
		hash_iterate(store->current->identities,
			     midr_lsdb_remote_update_iter, &notify);
	}
	if (changed)
		midr_lsdb_state_free(&old);
	else
		midr_lsdb_state_free(&staging);
	if (changed)
		midr_owned_group_reconcile(store->ctx);
	store->last_error = 0;
	return 0;

fail:
	store->failure_count++;
	store->last_error = ret;
	midr_ted_prepared_abort(&prepared);
	midr_lsdb_state_free(&staging);
	return ret;
}

static void midr_lsdb_commit_event(struct event *event)
{
	struct midr_lsdb_store *store = EVENT_ARG(event);

	if (!store)
		return;
	store->t_commit = NULL;
	if (midr_lsdb_process(store) != 0 && bm && bm->master)
		event_add_timer_msec(bm->master, midr_lsdb_commit_event,
				     store, MIDR_LSDB_RETRY_MSEC,
				     &store->t_commit);
}

static void midr_lsdb_schedule(struct midr_lsdb_store *store)
{
	if (!store || store->t_commit || !bm || !bm->master)
		return;
	event_add_event(bm->master, midr_lsdb_commit_event, store, 0,
			&store->t_commit);
}

int midr_lsdb_init(struct midr_context *ctx)
{
	struct midr_lsdb_store *store;

	if (!ctx || !ctx->bgp || !ctx->ted_store)
		return -EINVAL;
	if (ctx->lsdb_store)
		return -EALREADY;
	store = XCALLOC(MTYPE_MIDR_LSDB_STORE, sizeof(*store));
	store->ctx = ctx;
	store->dirty = list_new();
	store->dirty->del = midr_lsdb_dirty_free;
	ctx->lsdb_store = store;
	return 0;
}

void midr_lsdb_finish(struct midr_context *ctx)
{
	struct midr_lsdb_store *store;

	if (!ctx || !ctx->lsdb_store)
		return;
	store = ctx->lsdb_store;
	event_cancel(&store->t_commit);
	list_delete(&store->dirty);
	midr_lsdb_state_free(&store->current);
	ctx->lsdb_store = NULL;
	XFREE(MTYPE_MIDR_LSDB_STORE, store);
}

void midr_lsdb_route_changed(struct midr_context *ctx,
			     struct bgp_dest *dest,
			     struct bgp_path_info *old_selected,
			     struct bgp_path_info *new_selected)
{
	const struct midr_ls_object_key *key;
	struct midr_lsdb_dirty *dirty;
	struct midr_lsdb_store *store;

	if (!ctx || !ctx->lsdb_store || !dest)
		return;
	store = ctx->lsdb_store;
	dirty = XCALLOC(MTYPE_MIDR_LSDB_DIRTY, sizeof(*dirty));
	dirty->dest = bgp_dest_lock_node(dest);
	if (old_selected)
		dirty->old_selected = bgp_path_info_lock(old_selected);
	if (new_selected)
		dirty->new_selected = bgp_path_info_lock(new_selected);
	listnode_add(store->dirty, dirty);
	key = midr_rib_dest_key(dest);

	if (key && key->type == MIDR_NLRI_TYPE_MEMBERSHIP)
		store->scope_rebuild = true;
	midr_lsdb_schedule(store);
}

void midr_lsdb_local_metadata_changed(struct midr_context *ctx)
{
	if (!ctx || !ctx->lsdb_store)
		return;
	ctx->lsdb_store->force_rebuild = true;
	midr_lsdb_schedule(ctx->lsdb_store);
}

void midr_lsdb_input_state_changed(struct midr_context *ctx)
{
	midr_lsdb_local_metadata_changed(ctx);
}

void midr_lsdb_sync_changed(struct midr_context *ctx)
{
	if (!ctx || !ctx->lsdb_store)
		return;
	ctx->lsdb_store->force_rebuild = true;
	ctx->lsdb_store->scope_rebuild = true;
	midr_lsdb_schedule(ctx->lsdb_store);
}

struct midr_lsdb_remote_count {
	struct midr_context *ctx;
	size_t nodes;
	size_t links;
};

static void midr_lsdb_remote_count_iter(struct hash_bucket *bucket,
					void *arg)
{
	struct midr_lsdb_remote_count *count = arg;
	struct midr_lsdb_entry *entry = bucket->data;

	if (!entry->usable ||
	    entry->object.key.originator_node_id ==
		    count->ctx->bgp->router_id.s_addr)
		return;
	if (entry->object.key.type == MIDR_NLRI_TYPE_MEMBERSHIP)
		count->nodes++;
	else if (entry->object.key.type == MIDR_NLRI_TYPE_LINK)
		count->links++;
}

struct midr_lsdb_remote_fill {
	struct midr_context *ctx;
	struct midr_remote_node_info *nodes;
	struct midr_remote_link_info *links;
	size_t node_index;
	size_t link_index;
};

static void midr_lsdb_remote_fill_iter(struct hash_bucket *bucket,
				       void *arg)
{
	struct midr_lsdb_remote_fill *fill = arg;
	struct midr_lsdb_entry *entry = bucket->data;

	if (!entry->usable ||
	    entry->object.key.originator_node_id ==
		    fill->ctx->bgp->router_id.s_addr)
		return;
	if (entry->object.key.type == MIDR_NLRI_TYPE_MEMBERSHIP) {
		fill->nodes[fill->node_index++] =
			(struct midr_remote_node_info){
				.node_id =
					entry->object.key
						.originator_node_id,
				.group_id =
					entry->object.payload.membership
						.group_id,
				.has_transport_address =
					entry->object.payload.membership
						.has_transport_address,
				.transport_address =
					entry->object.payload.membership
						.transport_address,
				.cap_flags =
					entry->object.payload.membership
						.cap_flags,
				.policy_tags = entry->object.policy_tags,
				.ls_sequence = entry->object.ls_sequence,
			};
	} else if (entry->object.key.type == MIDR_NLRI_TYPE_LINK) {
		fill->links[fill->link_index++] =
			(struct midr_remote_link_info){
				.key =
					{
						.local_node_id =
							entry->object.key
								.originator_node_id,
						.remote_node_id =
							entry->object.key.u
								.link
								.remote_node_id,
						.link_id =
							entry->object.key.u
								.link.link_id,
					},
				.link_local_address =
					entry->object.payload.link
						.link_local_address,
				.link_remote_address =
					entry->object.payload.link
						.link_remote_address,
				.canonical_cost =
					entry->object.payload.link
						.canonical_cost,
				.policy_tags = entry->object.policy_tags,
				.ls_sequence = entry->object.ls_sequence,
			};
	}
}

int midr_lsdb_remote_snapshot_get(
	struct midr_context *ctx, struct midr_remote_view_snapshot *snapshot)
{
	struct midr_lsdb_remote_count count = {
		.ctx = ctx,
	};
	struct midr_lsdb_remote_fill fill = {
		.ctx = ctx,
	};
	struct midr_lsdb_state *state;

	if (!snapshot)
		return -EINVAL;
	memset(snapshot, 0, sizeof(*snapshot));
	if (!ctx || !ctx->lsdb_store)
		return -ENOENT;
	state = ctx->lsdb_store->current;
	if (!state || !state->ready)
		return -EAGAIN;
	hash_iterate(state->identities, midr_lsdb_remote_count_iter,
		     &count);
	if (count.nodes > SIZE_MAX / sizeof(*fill.nodes) ||
	    count.links > SIZE_MAX / sizeof(*fill.links))
		return -EOVERFLOW;
	if (count.nodes)
		fill.nodes = XCALLOC(MTYPE_MIDR_LSDB_REMOTE,
				     count.nodes * sizeof(*fill.nodes));
	if (count.links)
		fill.links = XCALLOC(MTYPE_MIDR_LSDB_REMOTE,
				     count.links * sizeof(*fill.links));
	hash_iterate(state->identities, midr_lsdb_remote_fill_iter, &fill);
	snapshot->nodes = fill.nodes;
	snapshot->node_count = fill.node_index;
	snapshot->links = fill.links;
	snapshot->link_count = fill.link_index;
	snapshot->snapshot_version = state->generation;
	return 0;
}

void midr_lsdb_remote_snapshot_release(
	struct midr_remote_view_snapshot *snapshot)
{
	struct midr_remote_node_info *nodes;
	struct midr_remote_link_info *links;

	if (!snapshot)
		return;
	nodes = (struct midr_remote_node_info *)snapshot->nodes;
	links = (struct midr_remote_link_info *)snapshot->links;
	XFREE(MTYPE_MIDR_LSDB_REMOTE, nodes);
	XFREE(MTYPE_MIDR_LSDB_REMOTE, links);
	memset(snapshot, 0, sizeof(*snapshot));
}

int midr_lsdb_summary_get(struct midr_context *ctx,
			  struct midr_lsdb_summary *summary)
{
	struct midr_lsdb_store *store;
	struct midr_lsdb_state *state;

	if (!summary)
		return -EINVAL;
	memset(summary, 0, sizeof(*summary));
	if (!ctx || !ctx->lsdb_store)
		return -ENOENT;
	store = ctx->lsdb_store;
	state = store->current;
	summary->dirty_count = listcount(store->dirty);
	summary->commit_count = store->commit_count;
	summary->failure_count = store->failure_count;
	summary->last_error = store->last_error;
	if (!state)
		return 0;
	summary->generation = state->generation;
	summary->object_count = state->identities->count;
	summary->usable_count = state->usable_count;
	summary->pending_count = state->pending_count;
	summary->membership_count = state->membership_count;
	summary->link_count = state->link_count;
	summary->node_prefix_count = state->node_prefix_count;
	summary->group_prefix_count = state->group_prefix_count;
	return 0;
}

int midr_lsdb_local_group_get(struct midr_context *ctx, uint32_t *group_id,
			      uint32_t *representative_node_id)
{
	struct midr_lsdb_group_index *group;
	struct midr_lsdb_state *state;

	if (!group_id || !representative_node_id)
		return -EINVAL;
	*group_id = 0;
	*representative_node_id = 0;
	if (!ctx || !ctx->lsdb_store)
		return -ENOENT;
	state = ctx->lsdb_store->current;
	if (!state || !state->local_group_id)
		return -EAGAIN;
	group = midr_lsdb_group_lookup(state, state->local_group_id);
	if (!group || !group->representative_node_id)
		return -EAGAIN;
	*group_id = group->group_id;
	*representative_node_id = group->representative_node_id;
	return 0;
}

struct midr_lsdb_group_prefix_foreach_state {
	struct midr_lsdb_state *state;
	midr_lsdb_group_prefix_cb cb;
	void *arg;
	int result;
};

static void midr_lsdb_group_prefix_foreach_iter(struct hash_bucket *bucket, void *arg)
{
	struct midr_lsdb_group_prefix_foreach_state *walk = arg;
	struct midr_lsdb_contributor_index *index = bucket->data;
	struct midr_lsdb_group_index *group;
	struct midr_lsdb_group_prefix_candidate candidate;

	if (walk->result || index->group_id != walk->state->local_group_id)
		return;
	group = midr_lsdb_group_lookup(walk->state, index->group_id);
	if (!group || !group->representative_node_id)
		return;
	candidate = (struct midr_lsdb_group_prefix_candidate){
		.group_id = index->group_id,
		.representative_node_id = group->representative_node_id,
		.prefix = index->prefix,
		.contributor_count = index->contributor_count,
	};
	walk->result = walk->cb(&candidate, walk->arg);
}

int midr_lsdb_local_group_prefix_foreach(struct midr_context *ctx, midr_lsdb_group_prefix_cb cb,
					 void *arg)
{
	struct midr_lsdb_group_prefix_foreach_state walk;
	struct midr_lsdb_state *state;

	if (!ctx || !ctx->lsdb_store)
		return -ENOENT;
	if (!cb)
		return -EINVAL;
	state = ctx->lsdb_store->current;
	if (!state || !state->local_group_id)
		return -EAGAIN;
	walk = (struct midr_lsdb_group_prefix_foreach_state){
		.state = state,
		.cb = cb,
		.arg = arg,
	};
	hash_iterate(state->contributors, midr_lsdb_group_prefix_foreach_iter, &walk);
	return walk.result;
}

void midr_show_lsdb(struct vty *vty, struct midr_context *ctx)
{
	struct midr_lsdb_summary summary;

	if (midr_lsdb_summary_get(ctx, &summary) != 0) {
		vty_out(vty, "MIDR LSDB is unavailable\n");
		return;
	}
	vty_out(vty, "MIDR LSDB summary:\n");
	vty_out(vty, "  generation:       %" PRIu64 "\n",
		summary.generation);
	vty_out(vty, "  objects:          %zu\n", summary.object_count);
	vty_out(vty, "  usable:           %zu\n", summary.usable_count);
	vty_out(vty, "  pending:          %zu\n", summary.pending_count);
	vty_out(vty, "  memberships:      %zu\n",
		summary.membership_count);
	vty_out(vty, "  links:            %zu\n", summary.link_count);
	vty_out(vty, "  node-prefixes:    %zu\n",
		summary.node_prefix_count);
	vty_out(vty, "  group-prefixes:   %zu\n",
		summary.group_prefix_count);
	vty_out(vty, "  dirty entries:    %zu\n", summary.dirty_count);
	vty_out(vty, "  commits/failures: %" PRIu64 "/%" PRIu64 "\n",
		summary.commit_count, summary.failure_count);
	vty_out(vty, "  last error:       %d\n", summary.last_error);
}

static const char *midr_lsdb_scope_name(enum midr_lsdb_scope scope)
{
	switch (scope) {
	case MIDR_LSDB_SCOPE_LOCAL_ONLY:
		return "LOCAL_ONLY";
	case MIDR_LSDB_SCOPE_INTRA_GROUP:
		return "INTRA_GROUP";
	case MIDR_LSDB_SCOPE_GLOBAL:
		return "GLOBAL";
	}
	return "UNKNOWN";
}

static const char *midr_lsdb_object_type_name(enum midr_nlri_type type)
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

static const char *midr_lsdb_pending_name(enum midr_lsdb_pending_reason reason)
{
	switch (reason) {
	case MIDR_LSDB_PENDING_NONE:
		return "NONE";
	case MIDR_LSDB_PENDING_LOCAL_MEMBERSHIP:
		return "LOCAL_MEMBERSHIP";
	case MIDR_LSDB_PENDING_REMOTE_MEMBERSHIP:
		return "REMOTE_MEMBERSHIP";
	case MIDR_LSDB_PENDING_GROUP_MEMBERSHIP:
		return "GROUP_MEMBERSHIP";
	case MIDR_LSDB_PENDING_SCOPE_INELIGIBLE:
		return "SCOPE_INELIGIBLE";
	}
	return "UNKNOWN";
}

struct midr_lsdb_show_state {
	struct vty *vty;
	struct midr_context *ctx;
	size_t count;
};

static void midr_lsdb_show_entry(struct hash_bucket *bucket, void *arg)
{
	struct midr_lsdb_show_state *show = arg;
	struct midr_lsdb_entry *entry = bucket->data;
	const struct midr_ls_object *object = &entry->object;
	struct in_addr origin = {.s_addr = object->key.originator_node_id};
	struct in_addr remote;

	show->count++;
	vty_out(show->vty,
		"object type=%s origin=%pI4 sequence=%" PRIu64
		" policy=0x%" PRIx64 " usable=%s scope=%s scope-group=%u"
		" pending=%s source=",
		midr_lsdb_object_type_name(object->key.type), &origin,
		object->ls_sequence,
		object->policy_tags, entry->usable ? "yes" : "no",
		midr_lsdb_scope_name(entry->scope), entry->scope_group_id,
		midr_lsdb_pending_name(entry->pending_reason));
	if (entry->peer == show->ctx->bgp->peer_self)
		vty_out(show->vty, "self");
	else if (entry->peer && entry->peer->remote_id.s_addr)
		vty_out(show->vty, "%pI4", &entry->peer->remote_id);
	else
		vty_out(show->vty, "%s",
			entry->peer && entry->peer->host ? entry->peer->host
							 : "unknown");

	switch (object->key.type) {
	case MIDR_NLRI_TYPE_MEMBERSHIP:
		vty_out(show->vty, " group=%u transport=",
			object->payload.membership.group_id);
		if (object->payload.membership.has_transport_address)
			vty_out(show->vty, "%pIA",
				&object->payload.membership.transport_address);
		else
			vty_out(show->vty, "none");
		vty_out(show->vty, " caps=0x%" PRIx64,
			object->payload.membership.cap_flags);
		break;
	case MIDR_NLRI_TYPE_LINK:
		remote.s_addr = object->key.u.link.remote_node_id;
		vty_out(show->vty,
			" remote=%pI4 link-id=%" PRIu64
			" addresses=%pIA->%pIA advertised-cost=%u"
			" derived-cost=%u ifindex=%d",
			&remote, object->key.u.link.link_id,
			&object->payload.link.link_local_address,
			&object->payload.link.link_remote_address,
			object->payload.link.canonical_cost,
			entry->canonical_cost, entry->local_ifindex);
		break;
	case MIDR_NLRI_TYPE_NODE_PREFIX:
		vty_out(show->vty, " afi=%u safi=%u prefix=%pFX",
			object->key.u.node_prefix.afi,
			object->key.u.node_prefix.safi,
			&object->key.u.node_prefix.prefix);
		break;
	case MIDR_NLRI_TYPE_GROUP_PREFIX:
		vty_out(show->vty, " group=%u afi=%u safi=%u prefix=%pFX",
			object->key.u.group_prefix.group_id,
			object->key.u.group_prefix.prefix.afi,
			object->key.u.group_prefix.prefix.safi,
			&object->key.u.group_prefix.prefix.prefix);
		break;
	case MIDR_NLRI_TYPE_RESERVED:
		break;
	}
	vty_out(show->vty, "\n");
}

void midr_show_lsdb_objects(struct vty *vty, struct midr_context *ctx)
{
	struct midr_lsdb_show_state show = {
		.vty = vty,
		.ctx = ctx,
	};
	struct midr_lsdb_state *state;

	if (!vty || !ctx || !ctx->lsdb_store) {
		if (vty)
			vty_out(vty, "MIDR LSDB is unavailable\n");
		return;
	}
	state = ctx->lsdb_store->current;
	if (!state) {
		vty_out(vty, "MIDR LSDB objects: no committed state\n");
		return;
	}
	vty_out(vty,
		"MIDR LSDB objects: generation=%" PRIu64 " ready=%s\n",
		state->generation, state->ready ? "yes" : "no");
	hash_iterate(state->identities, midr_lsdb_show_entry, &show);
	if (!show.count)
		vty_out(vty, "  none\n");
	vty_out(vty, "MIDR LSDB object total: %zu\n", show.count);
}

int midr_lsdb_test_process(struct midr_context *ctx)
{
	if (!ctx || !ctx->lsdb_store)
		return -ENOENT;
	event_cancel(&ctx->lsdb_store->t_commit);
	return midr_lsdb_process(ctx->lsdb_store);
}

void midr_lsdb_test_fail_next_prepare(struct midr_context *ctx)
{
	if (ctx && ctx->lsdb_store)
		ctx->lsdb_store->fail_next_prepare = true;
}
