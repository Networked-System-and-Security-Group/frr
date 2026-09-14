/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "midr-core.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static struct midr_core *core;

static struct midr_core_identity node_key(uint8_t family)
{
	struct midr_core_identity key = {
		.type = MIDR_CORE_NODE_PREFIX,
		.family = family,
		.prefix_len = family == MIDR_CORE_AF_IPV4 ? 24 : 64,
		.originator = 101,
	};

	key.prefix[0] = 192;
	key.prefix[1] = 0;
	key.prefix[2] = 2;
	key.prefix[3] = 0xff;
	if (family == MIDR_CORE_AF_IPV6) {
		memset(key.prefix, 0, sizeof(key.prefix));
		key.prefix[0] = 0x20;
		key.prefix[1] = 0x01;
		key.prefix[2] = 0x0d;
		key.prefix[3] = 0xb8;
		key.prefix[8] = 0xff;
	}
	return key;
}

static struct midr_core_object active(const struct midr_core_identity *key,
					      uint64_t sequence)
{
	struct midr_core_object object = {
		.identity = *key,
		.state = MIDR_CORE_ACTIVE,
		.sequence = sequence,
		.metric = 10,
	};

	object.local_address[0] = 1;
	return object;
}

static void drain_one(uint64_t sequence, uint8_t state)
{
	struct midr_core_object event;

	assert(midr_core_event_next(core, &event) == 0);
	assert(event.sequence == sequence);
	assert(event.state == state);
}

static void test_versions_and_conflict(void)
{
	struct midr_core_identity key = node_key(MIDR_CORE_AF_IPV4);
	struct midr_core_object object = active(&key, 1);
	struct midr_core_object conflict = object;
	struct midr_core_object old = object;
	enum midr_core_result result;

	assert(midr_core_upsert(core, &object, 100, &result) == 0);
	assert(result == MIDR_CORE_ACCEPTED);
	drain_one(1, MIDR_CORE_ACTIVE);
	assert(midr_core_upsert(core, &object, 101, &result) == 0);
	assert(result == MIDR_CORE_DUPLICATE);
	assert(midr_core_event_next(core, &object) == -ENOENT);
	conflict.metric++;
	assert(midr_core_upsert(core, &conflict, 102, &result) == 0);
	assert(result == MIDR_CORE_CONFLICT);
	old.sequence = 0;
	assert(midr_core_upsert(core, &old, 103, &result) == -EINVAL);
	old.sequence = 1;
	assert(midr_core_upsert(core, &old, 103, &result) == 0);
	assert(result == MIDR_CORE_DUPLICATE);
	object.sequence = 2;
	assert(midr_core_upsert(core, &object, 104, &result) == 0);
	assert(result == MIDR_CORE_ACCEPTED);
	drain_one(2, MIDR_CORE_ACTIVE);
}

static void test_ipv6_and_normalization(void)
{
	struct midr_core_identity key = node_key(MIDR_CORE_AF_IPV6);
	struct midr_core_object object = active(&key, 1);
	struct midr_core_identity noncanonical = key;
	enum midr_core_result result;

	noncanonical.prefix[8] = 0xff;
	assert(midr_core_identity_validate(&noncanonical) == -EINVAL);
	assert(midr_core_upsert(core, &object, 110, &result) == 0);
	assert(result == MIDR_CORE_ACCEPTED);
	drain_one(1, MIDR_CORE_ACTIVE);
	assert(midr_core_count(core) == 2);
}

static void test_refresh_withdraw_expire(void)
{
	struct midr_core_identity key = node_key(MIDR_CORE_AF_IPV4);
	struct midr_core_object object;
	uint32_t remaining;
	size_t expired;

	assert(midr_core_refresh(core, &key, 200) == 0);
	drain_one(3, MIDR_CORE_ACTIVE);
	assert(midr_core_lookup(core, &key, 250, &object, &remaining) == 0);
	assert(object.sequence == 3 && remaining == 950);
	assert(midr_core_withdraw(core, &key, 300) == 0);
	drain_one(4, MIDR_CORE_WITHDRAWN);
	assert(midr_core_expire(core, 1299, &expired) == 0);
	assert(expired == 1);
	assert(midr_core_expire(core, 1300, &expired) == 0);
	assert(expired == 1);
	assert(midr_core_count(core) == 0);
	assert(midr_core_lookup(core, &key, 1300, &object, NULL) == -ENOENT);
}

int main(void)
{
	const struct midr_core_config config = {
		.max_objects = 32,
		.lifetime_ms = 1000,
	};

	assert(midr_core_create(&config, &core) == 0);
	test_versions_and_conflict();
	test_ipv6_and_normalization();
	test_refresh_withdraw_expire();
	midr_core_destroy(&core);
	puts("midr-core-test: PASS");
	return 0;
}
