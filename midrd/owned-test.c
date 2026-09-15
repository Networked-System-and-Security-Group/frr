#include "midr-owned.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct publish_log {
	struct midr_core_object objects[64];
	size_t count;
	bool fail;
};

static int publish(void *arg, const struct midr_core_object *object)
{
	struct publish_log *log = arg;

	assert(log->count < sizeof(log->objects) / sizeof(log->objects[0]));
	if (log->fail)
		return -EIO;
	log->objects[log->count++] = *object;
	return 0;
}

static struct midr_core_object prefix(uint32_t originator, uint32_t metric)
{
	struct midr_core_object object = {0};

	object.identity.type = MIDR_CORE_NODE_PREFIX;
	object.identity.family = MIDR_CORE_AF_IPV4;
	object.identity.prefix_len = 24;
	object.identity.originator = originator;
	object.identity.prefix[0] = 192;
	object.identity.prefix[1] = 0;
	object.identity.prefix[2] = 2;
	object.metric = metric;
	object.state = MIDR_CORE_ACTIVE;
	return object;
}

int main(void)
{
	const char *path = "/tmp/midrd-owned-test.seq";
	struct midr_owned_config config = {
		.originator = 77,
		.max_objects = 4,
		.lifetime_ms = 1000,
		.sequence_file = path,
	};
	struct publish_log log = {0};
	struct publish_log staged_log = {0};
	struct publish_log reuse_log = {0};
	struct midr_owned *owned = NULL;
	struct midr_owned *staged = NULL;
	struct midr_core_object object = prefix(77, 10);
	struct midr_core_object saved;

	(void)unlink(path);
	assert(midr_owned_create(&config, publish, &log, &owned) == 0);
	assert(midr_owned_upsert(owned, &object) == 0);
	assert(log.count == 1 && log.objects[0].sequence == 1);
	assert(midr_owned_refresh(owned, &object.identity) == 0);
	assert(log.count == 2 && log.objects[1].sequence == 2);
	assert(midr_owned_withdraw(owned, &object.identity) == 0);
	assert(log.count == 3 && log.objects[2].state == MIDR_CORE_WITHDRAWN &&
	       log.objects[2].sequence == 3);
	assert(midr_owned_count(owned) == 0);
	assert(midr_owned_upsert(owned, &object) == 0);
	assert(log.count == 4 && log.objects[3].sequence == 4);
	assert(midr_owned_lookup(owned, &object.identity, &saved) == 0);
	assert(saved.sequence == 4);
	log.fail = true;
	object.metric = 11;
	assert(midr_owned_upsert(owned, &object) == -EIO);
	assert(midr_owned_lookup(owned, &object.identity, &saved) == 0);
	assert(saved.sequence == 4 && saved.metric == 10);
	assert(midr_owned_last_sequence(owned) == 5);
	midr_owned_destroy(&owned);

	log.fail = false;
	assert(midr_owned_create(&config, publish, &log, &owned) == 0);
	assert(midr_owned_last_sequence(owned) == 5);
	assert(midr_owned_upsert(owned, &object) == 0);
	assert(log.objects[4].sequence == 6);

	assert(midr_owned_clone(owned, publish, &staged_log, &staged) == 0);
	assert(midr_owned_lookup(staged, &object.identity, &saved) == 0);
	assert(saved.sequence == 6 && saved.metric == 11);
	object.metric = 12;
	assert(midr_owned_upsert(staged, &object) == 0);
	assert(staged_log.count == 1 && staged_log.objects[0].sequence == 7);
	assert(midr_owned_lookup(staged, &object.identity, &saved) == 0);
	assert(saved.sequence == 7 && saved.metric == 12);
	assert(midr_owned_lookup(owned, &object.identity, &saved) == 0);
	assert(saved.sequence == 6 && saved.metric == 11);
	assert(midr_owned_sequence_floor(owned,
					 midr_owned_last_sequence(staged)) == 0);
	midr_owned_destroy(&staged);
	assert(midr_owned_refresh(owned, &object.identity) == 0);
	assert(log.objects[5].sequence == 8);

	staged_log.fail = true;
	assert(midr_owned_clone(owned, publish, &staged_log, &staged) == 0);
	object.metric = 13;
	assert(midr_owned_upsert(staged, &object) == -EIO);
	assert(midr_owned_lookup(owned, &object.identity, &saved) == 0);
	assert(saved.sequence == 8 && saved.metric == 11);
	assert(midr_owned_sequence_floor(owned,
					 midr_owned_last_sequence(staged)) == 0);
	assert(midr_owned_last_sequence(owned) == 9);
	midr_owned_destroy(&staged);
	assert(midr_owned_refresh(owned, &object.identity) == 0);
	assert(log.objects[6].sequence == 10);

	staged_log.fail = false;
	staged_log.count = 0;
	assert(midr_owned_clone(owned, publish, &staged_log, &staged) == 0);
	assert(midr_owned_refresh(staged, &object.identity) == 0);
	assert(midr_owned_withdraw(staged, &object.identity) == 0);
	assert(staged_log.count == 2);
	assert(staged_log.objects[0].sequence == 11);
	assert(staged_log.objects[1].sequence == 12);
	assert(midr_owned_count(staged) == 0);
	assert(midr_owned_lookup(owned, &object.identity, &saved) == 0);
	assert(saved.sequence == 10 && saved.metric == 11);
	assert(midr_owned_sequence_floor(owned,
					 midr_owned_last_sequence(staged)) == 0);
	midr_owned_destroy(&staged);

	assert(midr_owned_upsert(owned, &(struct midr_core_object){
		.identity = {
			.type = MIDR_CORE_NODE_PREFIX,
			.family = MIDR_CORE_AF_IPV4,
			.prefix_len = 24,
			.originator = 88,
		},
		.state = MIDR_CORE_ACTIVE,
	}) == -EINVAL);
	midr_owned_destroy(&owned);
	config.max_objects = 1;
	config.sequence_file = NULL;
	assert(midr_owned_create(&config, publish, &reuse_log, &owned) == 0);
	assert(midr_owned_upsert(owned, &object) == 0);
	assert(midr_owned_withdraw(owned, &object.identity) == 0);
	object.identity.prefix[2] = 3;
	assert(midr_owned_upsert(owned, &object) == 0);
	assert(midr_owned_count(owned) == 1);
	assert(midr_owned_lookup(owned, &object.identity, &saved) == 0);
	midr_owned_destroy(&owned);
	(void)unlink(path);
	puts("midrd-owned-test: PASS");
	return 0;
}
