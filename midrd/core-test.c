/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "midr-core.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static struct midr_core *core;

static bool reclaim_allowed;

static bool reclaim_floor(const struct midr_core_identity *identity, void *arg)
{
	struct midr_core_identity *seen = arg;

	assert(identity);
	if (seen)
		*seen = *identity;
	return reclaim_allowed;
}

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

static void test_age_arithmetic(void)
{
	uint32_t value;

	assert(midr_core_age(10, 50, 50, 0, 1000, &value) == 0 &&
	       value == 10);
	assert(midr_core_age(10, 50, 51, 3, 1000, &value) == 0 &&
	       value == 14);
	assert(midr_core_age(10, 0, 1000000, 0, 1000, &value) == 0 &&
	       value == 11);
	assert(midr_core_age(999, 0, 1, 0, 1000, &value) == 0 &&
	       value == 1000);
	assert(midr_core_age(0, 0, UINT64_MAX, 0, UINT32_MAX, &value) == 0 &&
	       value == UINT32_MAX);
	assert(midr_core_age(UINT32_MAX, 0, 0, UINT32_MAX, 1000,
			     &value) == 0 && value == 1000);
	assert(midr_core_age(0, 2, 1, 0, 1000, &value) == -ERANGE);
	assert(midr_core_age(0, 0, 0, 0, 0, &value) == -EINVAL);
	assert(midr_core_lifetime_remaining(990, 50, 51, 3, 1000,
					    &value) == 0 && value == 986);
	assert(midr_core_lifetime_remaining(999, 0, 1, 0, 1000,
					    &value) == 0 && value == 998);
	assert(midr_core_lifetime_remaining(1000, 0, UINT64_MAX, 0, 1000,
					    &value) == 0 && value == 0);
}

static void test_floor_retention_and_gc(void)
{
	const struct midr_core_config config = {
		.max_objects = 1,
		.lifetime_ms = 1000,
	};
	struct midr_core_identity key = node_key(MIDR_CORE_AF_IPV4);
	struct midr_core_identity seen = {0};
	struct midr_core_object object = active(&key, 1);
	struct midr_core *store = NULL;
	enum midr_core_result result;
	size_t count;

	object.lifetime_ms = 10;
	assert(midr_core_create(&config, &store) == 0);
	assert(midr_core_upsert(store, &object, 100, &result) == 0);
	assert(result == MIDR_CORE_ACCEPTED);
	assert(midr_core_expire(store, 110, &count) == 0 && count == 1);
	assert(midr_core_count(store) == 0);
	assert(midr_core_identity_count(store) == 1);
	/* GC is disabled by default and the queued acceptance event is also a
	 * reference to this identity.  Neither condition may be bypassed. */
	assert(midr_core_gc(store, 1100, 1, &count, reclaim_floor, &seen) == 0 &&
	       count == 0);
	assert(midr_core_gc_enable(store, true) == 0);
	assert(midr_core_gc(store, 1100, 1, &count, reclaim_floor, &seen) == 0 &&
	       count == 0);
	assert(midr_core_event_next(store, &object) == 0);
	assert(midr_core_gc(store, 1099, 1, &count, reclaim_floor, &seen) == 0 &&
	       count == 0);
	reclaim_allowed = false;
	assert(midr_core_gc(store, 1100, 1, &count, reclaim_floor, &seen) == 0 &&
	       count == 0);
	reclaim_allowed = true;
	assert(midr_core_gc(store, 1100, 1, &count, reclaim_floor, &seen) == 0 &&
	       count == 1);
	assert(midr_core_identity_equal(&seen, &key));
	assert(midr_core_identity_count(store) == 0);
	midr_core_destroy(&store);
}

static void test_newer_version_replaces_floor(void)
{
	const struct midr_core_config config = {
		.max_objects = 1,
		.lifetime_ms = 1000,
	};
	struct midr_core_identity key = node_key(MIDR_CORE_AF_IPV4);
	struct midr_core_object object = active(&key, 1);
	struct midr_core_object current;
	struct midr_core *store = NULL;
	enum midr_core_result result;
	size_t count;

	object.lifetime_ms = 10;
	assert(midr_core_create(&config, &store) == 0);
	assert(midr_core_upsert(store, &object, 100, &result) == 0);
	assert(midr_core_event_next(store, &current) == 0);
	assert(midr_core_expire(store, 110, &count) == 0 && count == 1);
	assert(midr_core_upsert(store, &object, 111, &result) == 0);
	assert(result == MIDR_CORE_DUPLICATE);
	object.sequence = 2;
	object.lifetime_ms = 1000;
	assert(midr_core_upsert(store, &object, 111, &result) == 0);
	assert(result == MIDR_CORE_ACCEPTED);
	assert(midr_core_lookup(store, &key, 111, &current, NULL) == 0);
	assert(current.sequence == 2);
	assert(midr_core_count(store) == 1);
	assert(midr_core_identity_count(store) == 1);
	midr_core_destroy(&store);
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
	struct midr_core_identity remote_key = node_key(MIDR_CORE_AF_IPV6);
	struct midr_core_object object;
	uint32_t remaining;
	size_t expired;

	assert(midr_core_refresh(core, &key, 200) == 0);
	drain_one(3, MIDR_CORE_ACTIVE);
	/* Remote expiry only removes the object from the usable view.  It must
	 * not fabricate an owner WITHDRAWN event or advance the remote sequence. */
	assert(midr_core_expire(core, 1110, &expired) == 0);
	assert(expired == 1);
	assert(midr_core_event_next(core, &object) == -ENOENT);
	assert(midr_core_lookup(core, &remote_key, 1110, &object, NULL) == -ENOENT);
	assert(midr_core_lookup(core, &key, 250, &object, &remaining) == 0);
	assert(object.sequence == 3 && remaining == 950);
	assert(midr_core_withdraw(core, &key, 300) == 0);
	drain_one(4, MIDR_CORE_WITHDRAWN);
	assert(midr_core_expire(core, 1299, &expired) == 0);
	assert(expired == 0);
	assert(midr_core_expire(core, 1300, &expired) == 0);
	assert(expired == 1);
	assert(midr_core_count(core) == 0);
	assert(midr_core_lookup(core, &key, 1300, &object, NULL) == -ENOENT);
}

static void test_membership_group_is_payload(void)
{
	struct midr_core_identity key = {
		.type = MIDR_CORE_MEMBERSHIP,
		.originator = 200,
	};
	struct midr_core_identity invalid = key;
	struct midr_core_object object = {
		.identity = key,
		.state = MIDR_CORE_ACTIVE,
		.sequence = 1,
		.group = 10,
	};
	struct midr_core_object current;
	enum midr_core_result result;

	invalid.group = 10;
	assert(midr_core_identity_validate(&invalid) == -EINVAL);
	assert(midr_core_upsert(core, &object, 400, &result) == 0);
	assert(result == MIDR_CORE_ACCEPTED);
	drain_one(1, MIDR_CORE_ACTIVE);
	object.sequence = 2;
	object.group = 20;
	assert(midr_core_upsert(core, &object, 401, &result) == 0);
	assert(result == MIDR_CORE_ACCEPTED);
	drain_one(2, MIDR_CORE_ACTIVE);
	assert(midr_core_lookup(core, &key, 401, &current, NULL) == 0);
	assert(current.group == 20);
	assert(midr_core_count(core) == 1);
}

static void test_link_cost_validation(void)
{
	struct midr_core_object object = {0};
	struct midr_core_object changed;
	enum midr_core_result result;

	object.identity.type = MIDR_CORE_LINK;
	object.identity.originator = 300;
	object.identity.remote = 301;
	object.identity.link_id = 1;
	object.state = MIDR_CORE_ACTIVE;
	object.sequence = 1;
	object.address_family = MIDR_CORE_AF_IPV4;
	object.local_address[0] = 192;
	object.local_address[1] = 0;
	object.local_address[2] = 2;
	object.local_address[3] = 1;
	object.remote_address[0] = 192;
	object.remote_address[1] = 0;
	object.remote_address[2] = 2;
	object.remote_address[3] = 2;
	assert(midr_core_upsert(core, &object, 500, &result) == -EINVAL);
	object.metric = UINT32_MAX;
	assert(midr_core_upsert(core, &object, 501, &result) == -EINVAL);
	object.metric = 1;
	assert(midr_core_upsert(core, &object, 502, &result) == 0);
	assert(result == MIDR_CORE_ACCEPTED);
	drain_one(1, MIDR_CORE_ACTIVE);
	changed = object;
	changed.address_family = MIDR_CORE_AF_IPV6;
	assert(!midr_core_object_semantic_equal(&object, &changed));
	changed = object;
	changed.local_address[4] = 1;
	changed.sequence = 2;
	assert(midr_core_upsert(core, &changed, 503, &result) == -EINVAL);
}

int main(void)
{
	const struct midr_core_config config = {
		.max_objects = 32,
		.lifetime_ms = 1000,
	};

	assert(midr_core_create(&config, &core) == 0);
	test_age_arithmetic();
	test_versions_and_conflict();
	test_ipv6_and_normalization();
	test_refresh_withdraw_expire();
	/* The earlier prefix entries have expired; reuse the same core to verify
	 * a Membership group change remains one canonical identity. */
	test_membership_group_is_payload();
	test_link_cost_validation();
	midr_core_destroy(&core);
	test_floor_retention_and_gc();
	test_newer_version_replaces_floor();
	puts("midr-core-test: PASS");
	return 0;
}
