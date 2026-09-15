#include "midr-scope.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>

static struct midr_core_object membership(uint32_t owner, uint32_t group,
						  uint8_t state)
{
	struct midr_core_object object = {0};

	object.identity.type = MIDR_CORE_MEMBERSHIP;
	object.identity.originator = owner;
	object.group = group;
	object.state = state;
	object.sequence = 1;
	return object;
}

static struct midr_core_object link_object(uint32_t owner, uint32_t remote)
{
	struct midr_core_object object = {0};

	object.identity.type = MIDR_CORE_LINK;
	object.identity.originator = owner;
	object.identity.remote = remote;
	object.identity.link_id = 1;
	object.state = MIDR_CORE_ACTIVE;
	object.sequence = 1;
	object.metric = 1;
	return object;
}

int main(void)
{
	const struct midr_scope_config config = {
		.local_node_id = 1,
		.max_memberships = 4,
	};
	struct midr_scope *scope = NULL;
	struct midr_scope *clone = NULL;
	struct midr_core_object m1 = membership(1, 10, MIDR_CORE_ACTIVE);
	struct midr_core_object m2 = membership(2, 10, MIDR_CORE_ACTIVE);
	struct midr_core_object m2_moved = membership(2, 20, MIDR_CORE_ACTIVE);
	struct midr_core_object m3 = membership(3, 20, MIDR_CORE_ACTIVE);
	struct midr_core_object m4 = membership(4, 30, MIDR_CORE_ACTIVE);
	struct midr_core_object m2_withdrawn =
		membership(2, 10, MIDR_CORE_WITHDRAWN);
	struct midr_core_object link = link_object(1, 2);
	struct midr_core_object global_link = link_object(1, 3);
	struct midr_core_object group_prefix = {0};
	struct midr_core_object non_representative_prefix;
	struct midr_core_object node_prefix = {0};
	uint32_t group;
	uint32_t representative;

	assert(midr_scope_create(&config, &scope) == 0);
	assert(midr_scope_apply(scope, &m1) == 0);
	assert(midr_scope_apply(scope, &m2) == 0);
	assert(midr_scope_apply(scope, &m3) == 0);
	assert(midr_scope_membership(scope, 2, &group) == 0 && group == 10);
	assert(midr_scope_representative(scope, 10, &representative) == 0 &&
	       representative == 1);
	assert(midr_scope_export(scope, &link, 2));
	assert(!midr_scope_export(scope, &link, 0));
	assert(!midr_scope_export(scope, &link, 3));
	assert(midr_scope_export(scope, &global_link, 3));
	/* A global link is exportable even when the receiving peer has no
	 * Membership yet; the two link endpoints are already classified. */
	assert(midr_scope_export(scope, &global_link, 0));
	assert(midr_scope_usable(scope, &link));
	assert(midr_scope_usable(scope, &global_link));
	node_prefix.identity.type = MIDR_CORE_NODE_PREFIX;
	node_prefix.identity.family = MIDR_CORE_AF_IPV4;
	node_prefix.identity.prefix_len = 24;
	node_prefix.identity.originator = 1;
	node_prefix.state = MIDR_CORE_ACTIVE;
	node_prefix.sequence = 1;
	assert(midr_scope_export(scope, &node_prefix, 2));
	assert(!midr_scope_export(scope, &node_prefix, 3));
	assert(!midr_scope_export(scope, &node_prefix, 0));
	assert(midr_scope_usable(scope, &node_prefix));
	group_prefix.identity.type = MIDR_CORE_GROUP_PREFIX;
	group_prefix.identity.family = MIDR_CORE_AF_IPV4;
	group_prefix.identity.prefix_len = 24;
	group_prefix.identity.originator = 1;
	group_prefix.identity.group = 10;
	group_prefix.state = MIDR_CORE_ACTIVE;
	group_prefix.sequence = 1;
	assert(midr_scope_export(scope, &group_prefix, 2));
	assert(midr_scope_export(scope, &group_prefix, 3));
	assert(midr_scope_export(scope, &group_prefix, 0));
	assert(midr_scope_usable(scope, &group_prefix));
	non_representative_prefix = group_prefix;
	non_representative_prefix.identity.originator = 2;
	assert(!midr_scope_export(scope, &non_representative_prefix, 2));
	assert(!midr_scope_usable(scope, &non_representative_prefix));
	/* Membership group changes keep the same node identity and move the node
	 * between representatives without allocating another slot. */
	assert(midr_scope_apply(scope, &m2_moved) == 0);
	assert(midr_scope_membership(scope, 2, &group) == 0 && group == 20);
	assert(midr_scope_representative(scope, 20, &representative) == 0 &&
	       representative == 2);
	assert(!midr_scope_export(scope, &node_prefix, 2));
	assert(midr_scope_export(scope, &link, 0));
	assert(midr_scope_clone(scope, &clone) == 0);
	assert(midr_scope_apply(scope, &m2_withdrawn) == 0);
	assert(!midr_scope_export(scope, &link, 2));
	assert(midr_scope_export(scope, &group_prefix, 2));
	assert(!midr_scope_usable(scope, &link));
	assert(midr_scope_membership(scope, 2, &group) == -ENOENT);
	/* The inactive slot left by node 2 is reusable after withdraw. */
	assert(midr_scope_apply(scope, &m4) == 0);
	assert(midr_scope_membership(scope, 4, &group) == 0 && group == 30);
	assert(midr_scope_membership(clone, 2, &group) == 0 && group == 20);
	assert(midr_scope_apply(clone, &m2_withdrawn) == 0);
	assert(midr_scope_membership(clone, 2, &group) == -ENOENT);
	midr_scope_destroy(&clone);
	midr_scope_destroy(&scope);
	puts("midrd-scope-test: PASS");
	return 0;
}
