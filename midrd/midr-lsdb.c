/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "midr-lsdb.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct midr_lsdb {
	struct midr_lsdb_config config;
	struct midr_lsdb_entry *entries;
	size_t count;
	size_t usable_count;
	size_t pending_count;
	uint64_t generation;
	int fail_next;
};

struct midr_lsdb_stage {
	struct midr_lsdb_entry *entries;
	struct midr_consumer_event *events;
	size_t count;
	size_t event_count;
	size_t usable_count;
	size_t pending_count;
	uint64_t generation;
};

static int compare_u8(uint8_t left, uint8_t right)
{
	return (left > right) - (left < right);
}

static int compare_u32(uint32_t left, uint32_t right)
{
	return (left > right) - (left < right);
}

static int compare_u64(uint64_t left, uint64_t right)
{
	return (left > right) - (left < right);
}

static int identity_compare(const struct midr_core_identity *left,
			    const struct midr_core_identity *right)
{
	int ret;

	ret = compare_u8(left->type, right->type);
	if (ret)
		return ret;
	ret = compare_u8(left->family, right->family);
	if (ret)
		return ret;
	ret = compare_u8(left->prefix_len, right->prefix_len);
	if (ret)
		return ret;
	ret = compare_u32(left->originator, right->originator);
	if (ret)
		return ret;
	ret = compare_u32(left->remote, right->remote);
	if (ret)
		return ret;
	ret = compare_u32(left->group, right->group);
	if (ret)
		return ret;
	ret = compare_u64(left->link_id, right->link_id);
	return ret ? ret : memcmp(left->prefix, right->prefix,
				 sizeof(left->prefix));
}

static int entry_compare(const void *leftp, const void *rightp)
{
	const struct midr_lsdb_entry *left = leftp;
	const struct midr_lsdb_entry *right = rightp;

	return identity_compare(&left->object.identity, &right->object.identity);
}

static bool dependency_missing(const struct midr_scope *scope,
			       const struct midr_core_object *object)
{
	uint32_t value;

	switch (object->identity.type) {
	case MIDR_CORE_LINK:
		return midr_scope_membership(scope, object->identity.originator,
					     &value) == -ENOENT ||
		       midr_scope_membership(scope, object->identity.remote,
					     &value) == -ENOENT;
	case MIDR_CORE_NODE_PREFIX:
		return midr_scope_membership(scope, object->identity.originator,
					     &value) == -ENOENT;
	case MIDR_CORE_GROUP_PREFIX:
		return midr_scope_representative(scope, object->identity.group,
						 &value) == -ENOENT;
	default:
		return false;
	}
}

static enum midr_lsdb_entry_state classify(
	const struct midr_scope *scope, const struct midr_core_object *object)
{
	if (object->identity.type == MIDR_CORE_MEMBERSHIP)
		return MIDR_LSDB_USABLE;
	if (dependency_missing(scope, object))
		return MIDR_LSDB_PENDING;
	return midr_scope_usable(scope, object) ? MIDR_LSDB_USABLE :
		MIDR_LSDB_OUT_OF_SCOPE;
}

static int object_to_consumer(const struct midr_core_object *object,
			      uint64_t generation,
			      struct midr_consumer_event *event)
{
	memset(event, 0, sizeof(*event));
	event->generation = generation;
	event->originator = object->identity.originator;
	event->remote = object->identity.remote;
	event->group = object->identity.group;
	event->link_id = object->identity.link_id;
	event->family = object->identity.type == MIDR_CORE_LINK
			? object->address_family : object->identity.family;
	event->prefix_len = object->identity.prefix_len;
	memcpy(event->prefix, object->identity.prefix, sizeof(event->prefix));
	event->metric = object->metric;
	memcpy(event->local_address, object->local_address,
	       sizeof(event->local_address));
	memcpy(event->remote_address, object->remote_address,
	       sizeof(event->remote_address));
	switch (object->identity.type) {
	case MIDR_CORE_LINK:
		event->kind = MIDR_CONSUMER_LINK;
		return 0;
	case MIDR_CORE_NODE_PREFIX:
		event->kind = MIDR_CONSUMER_NODE_PREFIX;
		return 0;
	case MIDR_CORE_GROUP_PREFIX:
		event->kind = MIDR_CONSUMER_GROUP_PREFIX;
		return 0;
	default:
		return -ENOENT;
	}
}

int midr_lsdb_create(const struct midr_lsdb_config *config,
		     struct midr_lsdb **out)
{
	struct midr_lsdb *lsdb;

	if (!config || !config->max_objects || !out || *out)
		return -EINVAL;
	lsdb = calloc(1, sizeof(*lsdb));
	if (!lsdb)
		return -ENOMEM;
	lsdb->config = *config;
	*out = lsdb;
	return 0;
}

void midr_lsdb_destroy(struct midr_lsdb **lsdbp)
{
	if (!lsdbp || !*lsdbp)
		return;
	free((*lsdbp)->entries);
	free(*lsdbp);
	*lsdbp = NULL;
}

void midr_lsdb_abort_prepared(struct midr_lsdb_stage **stagep)
{
	if (!stagep || !*stagep)
		return;
	free((*stagep)->events);
	free((*stagep)->entries);
	free(*stagep);
	*stagep = NULL;
}

int midr_lsdb_prepare(struct midr_lsdb *lsdb, uint64_t generation,
		      const struct midr_core_object *objects, size_t count,
		      const struct midr_scope *scope,
		      struct midr_lsdb_stage **stagep)
{
	struct midr_lsdb_stage *stage;

	if (!lsdb || !generation || (count && !objects) || !scope || !stagep ||
	    *stagep || count > lsdb->config.max_objects)
		return -EINVAL;
	if (lsdb->fail_next) {
		int error = lsdb->fail_next;

		lsdb->fail_next = 0;
		return error;
	}
	stage = calloc(1, sizeof(*stage));
	if (!stage)
		return -ENOMEM;
	if (count) {
		stage->entries = calloc(count, sizeof(*stage->entries));
		stage->events = calloc(count, sizeof(*stage->events));
		if (!stage->entries || !stage->events) {
			midr_lsdb_abort_prepared(&stage);
			return -ENOMEM;
		}
	}
	for (size_t i = 0; i < count; i++) {
		struct midr_lsdb_entry *entry;
		int ret;

		if (objects[i].state != MIDR_CORE_ACTIVE)
			continue;
		entry = &stage->entries[stage->count++];
		entry->object = objects[i];
		entry->state = classify(scope, &objects[i]);
		if (entry->state == MIDR_LSDB_PENDING)
			stage->pending_count++;
		if (entry->state != MIDR_LSDB_USABLE)
			continue;
		stage->usable_count++;
		ret = object_to_consumer(&objects[i], generation,
					 &stage->events[stage->event_count]);
		if (!ret)
			stage->event_count++;
		else if (ret != -ENOENT) {
			midr_lsdb_abort_prepared(&stage);
			return ret;
		}
	}
	if (stage->count > 1U)
		qsort(stage->entries, stage->count, sizeof(*stage->entries),
		      entry_compare);
	for (size_t i = 1; i < stage->count; i++)
		if (!entry_compare(&stage->entries[i - 1], &stage->entries[i])) {
			midr_lsdb_abort_prepared(&stage);
			return -EEXIST;
		}
	stage->generation = generation;
	*stagep = stage;
	return 0;
}

void midr_lsdb_commit_prepared(struct midr_lsdb *lsdb,
			       struct midr_lsdb_stage **stagep)
{
	struct midr_lsdb_stage *stage;

	if (!lsdb || !stagep || !*stagep)
		return;
	stage = *stagep;
	free(lsdb->entries);
	lsdb->entries = stage->entries;
	lsdb->count = stage->count;
	lsdb->usable_count = stage->usable_count;
	lsdb->pending_count = stage->pending_count;
	lsdb->generation = stage->generation;
	stage->entries = NULL;
	free(stage->events);
	free(stage);
	*stagep = NULL;
}

int midr_lsdb_stage_consumer_snapshot(
	const struct midr_lsdb_stage *stage,
	struct midr_consumer_snapshot *snapshot)
{
	if (!stage || !snapshot)
		return -EINVAL;
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->generation = stage->generation;
	snapshot->count = stage->event_count;
	snapshot->events = stage->events;
	return 0;
}

int midr_lsdb_stage_snapshot(const struct midr_lsdb_stage *stage,
			     struct midr_lsdb_snapshot *snapshot)
{
	if (!stage || !snapshot)
		return -EINVAL;
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->generation = stage->generation;
	snapshot->entries = stage->entries;
	snapshot->count = stage->count;
	snapshot->usable_count = stage->usable_count;
	snapshot->pending_count = stage->pending_count;
	return 0;
}

int midr_lsdb_snapshot_acquire(const struct midr_lsdb *lsdb,
			       struct midr_lsdb_snapshot *snapshot)
{
	if (!lsdb || !snapshot)
		return -EINVAL;
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->generation = lsdb->generation;
	snapshot->count = lsdb->count;
	snapshot->usable_count = lsdb->usable_count;
	snapshot->pending_count = lsdb->pending_count;
	if (!snapshot->count)
		return 0;
	snapshot->entries = calloc(snapshot->count, sizeof(*snapshot->entries));
	if (!snapshot->entries)
		return -ENOMEM;
	memcpy(snapshot->entries, lsdb->entries,
	       snapshot->count * sizeof(*snapshot->entries));
	return 0;
}

void midr_lsdb_snapshot_release(struct midr_lsdb_snapshot *snapshot)
{
	if (!snapshot)
		return;
	free(snapshot->entries);
	memset(snapshot, 0, sizeof(*snapshot));
}

uint64_t midr_lsdb_generation(const struct midr_lsdb *lsdb)
{
	return lsdb ? lsdb->generation : 0;
}

int midr_lsdb_test_fail_next(struct midr_lsdb *lsdb, int error)
{
	if (!lsdb || error >= 0)
		return -EINVAL;
	lsdb->fail_next = error;
	return 0;
}
