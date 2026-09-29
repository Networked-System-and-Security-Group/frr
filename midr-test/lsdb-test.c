#include "midr-lsdb.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>

static struct midr_core_object membership(uint32_t owner, uint32_t group)
{
	struct midr_core_object object = {0};

	object.identity.type = MIDR_CORE_MEMBERSHIP;
	object.identity.originator = owner;
	object.state = MIDR_CORE_ACTIVE;
	object.sequence = 1;
	object.group = group;
	return object;
}

static struct midr_core_object link_object(uint32_t owner, uint32_t remote,
					    uint64_t link_id)
{
	struct midr_core_object object = {0};

	object.identity.type = MIDR_CORE_LINK;
	object.identity.originator = owner;
	object.identity.remote = remote;
	object.identity.link_id = link_id;
	object.state = MIDR_CORE_ACTIVE;
	object.address_family = MIDR_CORE_AF_IPV4;
	object.sequence = 1;
	object.metric = 10;
	object.local_address[0] = 192;
	object.local_address[1] = 0;
	object.local_address[2] = 2;
	object.local_address[3] = (uint8_t)owner;
	object.remote_address[0] = 198;
	object.remote_address[1] = 51;
	object.remote_address[2] = 100;
	object.remote_address[3] = (uint8_t)remote;
	return object;
}

static struct midr_core_object prefix(uint32_t owner, uint32_t group,
				      uint8_t suffix, bool group_prefix)
{
	struct midr_core_object object = {0};

	object.identity.type = group_prefix ? MIDR_CORE_GROUP_PREFIX :
		MIDR_CORE_NODE_PREFIX;
	object.identity.family = MIDR_CORE_AF_IPV4;
	object.identity.prefix_len = 24;
	object.identity.originator = owner;
	object.identity.group = group_prefix ? group : 0;
	object.identity.prefix[0] = 203;
	object.identity.prefix[1] = 0;
	object.identity.prefix[2] = suffix;
	object.state = MIDR_CORE_ACTIVE;
	object.sequence = 1;
	return object;
}

int main(void)
{
	const struct midr_scope_config scope_config = {
		.local_node_id = 1,
		.max_memberships = 16,
	};
	const struct midr_lsdb_config lsdb_config = {.max_objects = 16};
	struct midr_scope *scope = NULL;
	struct midr_lsdb *lsdb = NULL;
	struct midr_lsdb_stage *stage = NULL;
	struct midr_consumer_snapshot consumer = {0};
	struct midr_lsdb_snapshot snapshot = {0};
	struct midr_core_object objects[9];

	objects[0] = membership(1, 10);
	objects[1] = membership(2, 10);
	objects[2] = membership(3, 20);
	objects[3] = link_object(1, 2, 1);
	objects[4] = link_object(1, 4, 2);
	objects[5] = prefix(2, 0, 1, false);
	objects[6] = prefix(3, 0, 2, false);
	objects[7] = prefix(3, 20, 3, true);
	objects[8] = prefix(2, 0, 4, false);
	objects[8].state = MIDR_CORE_WITHDRAWN;

	assert(midr_scope_create(&scope_config, &scope) == 0);
	for (size_t i = 0; i < 3; i++)
		assert(midr_scope_apply(scope, &objects[i]) == 0);
	assert(midr_lsdb_create(&lsdb_config, &lsdb) == 0);
	assert(midr_lsdb_prepare(lsdb, 7, objects, 9, scope, &stage) == 0);
	assert(midr_lsdb_stage_consumer_snapshot(stage, &consumer) == 0);
	assert(consumer.generation == 7 && consumer.count == 3);
	midr_lsdb_commit_prepared(lsdb, &stage);
	assert(midr_lsdb_snapshot_acquire(lsdb, &snapshot) == 0);
	assert(snapshot.generation == 7 && snapshot.count == 8);
	assert(snapshot.usable_count == 6 && snapshot.pending_count == 1);
	midr_lsdb_snapshot_release(&snapshot);

	objects[5].sequence = 2;
	objects[5].identity.prefix[2] = 9;
	assert(midr_lsdb_prepare(lsdb, 8, objects, 9, scope, &stage) == 0);
	midr_lsdb_abort_prepared(&stage);
	assert(midr_lsdb_generation(lsdb) == 7);
	assert(midr_lsdb_test_fail_next(lsdb, -ENOMEM) == 0);
	assert(midr_lsdb_prepare(lsdb, 8, objects, 9, scope, &stage) == -ENOMEM);
	assert(!stage && midr_lsdb_generation(lsdb) == 7);

	objects[8] = objects[7];
	assert(midr_lsdb_prepare(lsdb, 8, objects, 9, scope, &stage) == -EEXIST);
	assert(!stage && midr_lsdb_generation(lsdb) == 7);
	midr_lsdb_destroy(&lsdb);
	midr_scope_destroy(&scope);
	puts("midrd-lsdb-test: PASS");
	return 0;
}
