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
	object.identity.group = group;
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
	return object;
}

int main(void)
{
	const struct midr_scope_config config = {
		.local_node_id = 99,
		.max_memberships = 4,
	};
	struct midr_scope *scope = NULL;
	struct midr_scope *clone = NULL;
	struct midr_core_object m1 = membership(1, 10, MIDR_CORE_ACTIVE);
	struct midr_core_object m2 = membership(2, 10, MIDR_CORE_ACTIVE);
	struct midr_core_object m3 = membership(3, 20, MIDR_CORE_ACTIVE);
	struct midr_core_object m2_withdrawn =
		membership(2, 10, MIDR_CORE_WITHDRAWN);
	struct midr_core_object link = link_object(1, 2);
	struct midr_core_object global_link = link_object(1, 3);
	struct midr_core_object group_prefix = {0};
	struct midr_core_object non_representative_prefix;
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
	assert(!midr_scope_export(scope, &link, 3));
	assert(midr_scope_export(scope, &global_link, 3));
	group_prefix.identity.type = MIDR_CORE_GROUP_PREFIX;
	group_prefix.identity.family = MIDR_CORE_AF_IPV4;
	group_prefix.identity.prefix_len = 24;
	group_prefix.identity.originator = 1;
	group_prefix.identity.group = 10;
	group_prefix.state = MIDR_CORE_ACTIVE;
	group_prefix.sequence = 1;
	assert(midr_scope_export(scope, &group_prefix, 2));
	assert(!midr_scope_export(scope, &group_prefix, 3));
	non_representative_prefix = group_prefix;
	non_representative_prefix.identity.originator = 2;
	assert(!midr_scope_export(scope, &non_representative_prefix, 2));
	assert(midr_scope_clone(scope, &clone) == 0);
	assert(midr_scope_apply(scope, &m2_withdrawn) == 0);
	assert(midr_scope_export(scope, &link, 2));
	assert(midr_scope_export(scope, &group_prefix, 2));
	assert(midr_scope_membership(scope, 2, &group) == -ENOENT);
	assert(midr_scope_membership(clone, 2, &group) == 0 && group == 10);
	assert(midr_scope_apply(clone, &m2_withdrawn) == 0);
	assert(midr_scope_membership(clone, 2, &group) == -ENOENT);
	midr_scope_destroy(&clone);
	midr_scope_destroy(&scope);
	puts("midrd-scope-test: PASS");
	return 0;
}
