/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "midr-scope.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

struct midr_scope_membership {
	uint32_t node_id;
	uint32_t group;
	bool active;
};

struct midr_scope {
	struct midr_scope_config config;
	struct midr_scope_membership *memberships;
	size_t count;
};

static ssize_t find_membership(const struct midr_scope *scope,
				       uint32_t node_id)
{
	for (size_t i = 0; i < scope->count; i++)
		if (scope->memberships[i].node_id == node_id)
			return (ssize_t)i;
	return -1;
}

int midr_scope_create(const struct midr_scope_config *config,
			     struct midr_scope **out)
{
	struct midr_scope *scope;

	if (!config || !out || *out || !config->local_node_id ||
	    !config->max_memberships)
		return -EINVAL;
	scope = calloc(1, sizeof(*scope));
	if (!scope)
		return -ENOMEM;
	scope->memberships = calloc(config->max_memberships,
					    sizeof(*scope->memberships));
	if (!scope->memberships) {
		free(scope);
		return -ENOMEM;
	}
	scope->config = *config;
	*out = scope;
	return 0;
}

int midr_scope_clone(const struct midr_scope *source,
			    struct midr_scope **out)
{
	struct midr_scope *clone = NULL;

	if (!source || !out || *out)
		return -EINVAL;
	if (midr_scope_create(&source->config, &clone))
		return -ENOMEM;
	clone->count = source->count;
	memcpy(clone->memberships, source->memberships,
	       source->config.max_memberships * sizeof(*source->memberships));
	*out = clone;
	return 0;
}

void midr_scope_destroy(struct midr_scope **scopep)
{
	if (!scopep || !*scopep)
		return;
	free((*scopep)->memberships);
	free(*scopep);
	*scopep = NULL;
}

int midr_scope_apply(struct midr_scope *scope,
			    const struct midr_core_object *object)
{
	ssize_t index;

	if (!scope || !object)
		return -EINVAL;
	if (object->identity.type != MIDR_CORE_MEMBERSHIP)
		return 0;
	if (midr_core_identity_validate(&object->identity) ||
	    !object->identity.originator)
		return -EINVAL;
	index = find_membership(scope, object->identity.originator);
	if (object->state == MIDR_CORE_WITHDRAWN) {
		if (index >= 0)
			scope->memberships[index].active = false;
		return 0;
	}
	if (object->state != MIDR_CORE_ACTIVE || !object->group)
		return -EINVAL;
	if (index < 0) {
		for (size_t i = 0; i < scope->count; i++)
			if (!scope->memberships[i].active) {
				index = (ssize_t)i;
				break;
			}
		if (index < 0) {
			if (scope->count == scope->config.max_memberships)
				return -ENOSPC;
			index = (ssize_t)scope->count++;
		}
	}
	scope->memberships[index].node_id = object->identity.originator;
	scope->memberships[index].group = object->group;
	scope->memberships[index].active = true;
	return 0;
}

int midr_scope_membership(const struct midr_scope *scope,
				  uint32_t node_id, uint32_t *group)
{
	ssize_t index;

	if (!scope || !node_id || !group)
		return -EINVAL;
	index = find_membership(scope, node_id);
	if (index < 0 || !scope->memberships[index].active)
		return -ENOENT;
	*group = scope->memberships[index].group;
	return 0;
}

int midr_scope_representative(const struct midr_scope *scope,
				      uint32_t group, uint32_t *node_id)
{
	uint32_t representative = 0;

	if (!scope || !group || !node_id)
		return -EINVAL;
	for (size_t i = 0; i < scope->count; i++) {
		const struct midr_scope_membership *membership =
			&scope->memberships[i];

		if (!membership->active || membership->group != group)
			continue;
		if (!representative || membership->node_id < representative)
			representative = membership->node_id;
	}
	if (!representative)
		return -ENOENT;
	*node_id = representative;
	return 0;
}

bool midr_scope_export(const struct midr_scope *scope,
			      const struct midr_core_object *object,
			      uint32_t peer_node_id)
{
	uint32_t owner_group, remote_group, peer_group, representative;

	if (!scope || !object || object->state == MIDR_CORE_WITHDRAWN)
		return object && object->state == MIDR_CORE_WITHDRAWN;
	if (object->identity.type == MIDR_CORE_MEMBERSHIP)
		return true;
	if (object->identity.type == MIDR_CORE_GROUP_PREFIX) {
		return !midr_scope_representative(scope, object->identity.group,
						  &representative) &&
		       representative == object->identity.originator;
	}
	if (object->identity.type == MIDR_CORE_NODE_PREFIX) {
		if (midr_scope_membership(scope, object->identity.originator,
					  &owner_group))
			return false;
		if (midr_scope_membership(scope, peer_node_id, &peer_group))
			return false;
		return peer_group == owner_group;
	}
	if (object->identity.type != MIDR_CORE_LINK)
		return false;
	if (midr_scope_membership(scope, object->identity.originator,
				 &owner_group) ||
	    midr_scope_membership(scope, object->identity.remote, &remote_group))
		return false;
	if (owner_group != remote_group)
		return true;
	if (midr_scope_membership(scope, peer_node_id, &peer_group))
		return false;
	return peer_group == owner_group;
}

bool midr_scope_usable(const struct midr_scope *scope,
			      const struct midr_core_object *object)
{
	uint32_t local_group, owner_group, remote_group, representative;

	if (!scope || !object || object->state != MIDR_CORE_ACTIVE)
		return false;
	if (object->identity.type == MIDR_CORE_MEMBERSHIP)
		return true;
	if (object->identity.type == MIDR_CORE_GROUP_PREFIX)
		return !midr_scope_representative(scope, object->identity.group,
						  &representative) &&
		       representative == object->identity.originator;
	if (object->identity.type == MIDR_CORE_NODE_PREFIX) {
		if (midr_scope_membership(scope, scope->config.local_node_id,
					  &local_group))
			return false;
		if (midr_scope_membership(scope, object->identity.originator,
					  &owner_group))
			return false;
		return local_group == owner_group;
	}
	if (object->identity.type != MIDR_CORE_LINK ||
	    midr_scope_membership(scope, object->identity.originator,
				  &owner_group) ||
	    midr_scope_membership(scope, object->identity.remote, &remote_group))
		return false;
	if (owner_group != remote_group)
		return true;
	if (midr_scope_membership(scope, scope->config.local_node_id,
				  &local_group))
		return false;
	return local_group == owner_group;
}
