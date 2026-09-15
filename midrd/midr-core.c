/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "midr-core.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct midr_core_entry {
	struct midr_core_object object;
	uint64_t updated_ms;
	bool floor;
};

struct midr_core_event {
	struct midr_core_event *next;
	struct midr_core_object object;
};

struct midr_core {
	struct midr_core_config config;
	struct midr_core_entry *entries;
	size_t count;
	size_t capacity;
	struct midr_core_event *events_head;
	struct midr_core_event *events_tail;
};

static uint8_t prefix_byte_mask(unsigned int bits)
{
	if (!bits)
		return 0;
	if (bits >= 8)
		return 0xffU;
	return (uint8_t)(0xffU << (8U - bits));
}

int midr_core_identity_normalize(const struct midr_core_identity *input,
				   struct midr_core_identity *output)
{
	unsigned int bytes;
	unsigned int remainder;

	if (!input || !output)
		return -EINVAL;
	*output = *input;
	output->reserved = 0;
	if (input->family == MIDR_CORE_AF_NONE) {
		if (input->prefix_len)
			return -EINVAL;
		memset(output->prefix, 0, sizeof(output->prefix));
		return 0;
	}
	if (input->family == MIDR_CORE_AF_IPV4) {
		if (input->prefix_len > 32U)
			return -EINVAL;
		bytes = 4U;
	} else if (input->family == MIDR_CORE_AF_IPV6) {
		if (input->prefix_len > 128U)
			return -EINVAL;
		bytes = 16U;
	} else {
		return -EINVAL;
	}
	remainder = input->prefix_len % 8U;
	if (remainder && input->prefix_len / 8U < bytes)
		output->prefix[input->prefix_len / 8U] &= prefix_byte_mask(remainder);
	for (unsigned int i = (input->prefix_len + 7U) / 8U; i < bytes; i++)
		output->prefix[i] = 0;
	for (unsigned int i = bytes; i < sizeof(output->prefix); i++)
		output->prefix[i] = 0;
	return 0;
}

int midr_core_identity_validate(const struct midr_core_identity *key)
{
	struct midr_core_identity normalized;

	if (!key || !key->type || key->type > MIDR_CORE_GROUP_PREFIX)
		return -EINVAL;
	if (!key->originator)
		return -EINVAL;
	if (key->type == MIDR_CORE_NODE_PREFIX ||
	    key->type == MIDR_CORE_GROUP_PREFIX) {
		if (key->family != MIDR_CORE_AF_IPV4 &&
		    key->family != MIDR_CORE_AF_IPV6)
			return -EINVAL;
	} else if (key->family != MIDR_CORE_AF_NONE || key->prefix_len) {
		return -EINVAL;
	}
	if (key->type == MIDR_CORE_LINK && !key->remote)
		return -EINVAL;
	if (key->type == MIDR_CORE_GROUP_PREFIX && !key->group)
		return -EINVAL;
	if (midr_core_identity_normalize(key, &normalized))
		return -EINVAL;
	if (memcmp(&normalized, key, sizeof(normalized)))
		return -EINVAL;
	return 0;
}

bool midr_core_identity_equal(const struct midr_core_identity *a,
				const struct midr_core_identity *b)
{
	struct midr_core_identity na;
	struct midr_core_identity nb;

	if (!a || !b || midr_core_identity_normalize(a, &na) ||
	    midr_core_identity_normalize(b, &nb))
		return false;
	return !memcmp(&na, &nb, sizeof(na));
}

bool midr_core_object_semantic_equal(const struct midr_core_object *a,
					     const struct midr_core_object *b)
{
	if (!a || !b || a->state != b->state ||
	    !midr_core_identity_equal(&a->identity, &b->identity))
		return false;
	if (a->state == MIDR_CORE_WITHDRAWN)
		return true;
	return a->metric == b->metric &&
	       !memcmp(a->local_address, b->local_address,
		       sizeof(a->local_address)) &&
	       !memcmp(a->remote_address, b->remote_address,
		       sizeof(a->remote_address));
}

static struct midr_core_entry *find_entry(struct midr_core *core,
					  const struct midr_core_identity *key)
{
	for (size_t i = 0; i < core->count; i++)
		if (midr_core_identity_equal(&core->entries[i].object.identity, key))
			return &core->entries[i];
	return NULL;
}

static const struct midr_core_entry *find_entry_const(const struct midr_core *core,
						      const struct midr_core_identity *key)
{
	for (size_t i = 0; i < core->count; i++)
		if (midr_core_identity_equal(&core->entries[i].object.identity, key))
			return &core->entries[i];
	return NULL;
}

static int enqueue_event(struct midr_core *core,
				 const struct midr_core_object *object)
{
	struct midr_core_event *event = calloc(1, sizeof(*event));

	if (!event)
		return -ENOMEM;
	event->object = *object;
	if (core->events_tail)
		core->events_tail->next = event;
	else
		core->events_head = event;
	core->events_tail = event;
	return 0;
}

int midr_core_create(const struct midr_core_config *config,
			    struct midr_core **out)
{
	struct midr_core *core;

	if (!config || !out || *out || !config->max_objects ||
	    !config->lifetime_ms)
		return -EINVAL;
	core = calloc(1, sizeof(*core));
	if (!core)
		return -ENOMEM;
	core->entries = calloc(config->max_objects, sizeof(*core->entries));
	if (!core->entries) {
		free(core);
		return -ENOMEM;
	}
	core->config = *config;
	core->capacity = config->max_objects;
	*out = core;
	return 0;
}

int midr_core_clone(const struct midr_core *source,
			   struct midr_core **out)
{
	struct midr_core *clone = NULL;
	const struct midr_core_event *event;

	if (!source || !out || *out)
		return -EINVAL;
	if (midr_core_create(&source->config, &clone))
		return -ENOMEM;
	clone->count = source->count;
	memcpy(clone->entries, source->entries,
	       source->capacity * sizeof(*source->entries));
	for (event = source->events_head; event; event = event->next) {
		if (enqueue_event(clone, &event->object)) {
			midr_core_destroy(&clone);
			return -ENOMEM;
		}
	}
	*out = clone;
	return 0;
}

void midr_core_destroy(struct midr_core **corep)
{
	struct midr_core *core;
	struct midr_core_event *event;

	if (!corep || !*corep)
		return;
	core = *corep;
	while ((event = core->events_head)) {
		core->events_head = event->next;
		free(event);
	}
	free(core->entries);
	free(core);
	*corep = NULL;
}

int midr_core_upsert(struct midr_core *core,
			 const struct midr_core_object *object,
			 uint64_t now_ms, enum midr_core_result *result)
{
	struct midr_core_object normalized;
	struct midr_core_entry *entry;
	bool new_entry;

	if (!core || !object || !result || !object->sequence ||
	    (object->state != MIDR_CORE_ACTIVE &&
	     object->state != MIDR_CORE_WITHDRAWN) ||
	    midr_core_identity_normalize(&object->identity, &normalized.identity) ||
	    midr_core_identity_validate(&normalized.identity))
		return -EINVAL;
	normalized = *object;
	if (midr_core_identity_normalize(&object->identity, &normalized.identity))
		return -EINVAL;
	if (normalized.state == MIDR_CORE_WITHDRAWN) {
		normalized.metric = 0;
		memset(normalized.local_address, 0, sizeof(normalized.local_address));
		memset(normalized.remote_address, 0, sizeof(normalized.remote_address));
	}
	if (!normalized.lifetime_ms || normalized.lifetime_ms > core->config.lifetime_ms)
		normalized.lifetime_ms = core->config.lifetime_ms;
	entry = find_entry(core, &normalized.identity);
	if (entry) {
		if (entry->floor && normalized.sequence <= entry->object.sequence) {
			*result = normalized.sequence == entry->object.sequence
					? MIDR_CORE_DUPLICATE : MIDR_CORE_OLDER;
			return 0;
		}
		if (normalized.sequence < entry->object.sequence) {
			*result = MIDR_CORE_OLDER;
			return 0;
		}
		if (normalized.sequence == entry->object.sequence) {
			*result = midr_core_object_semantic_equal(&normalized,
								 &entry->object)
					? MIDR_CORE_DUPLICATE : MIDR_CORE_CONFLICT;
			return 0;
		}
		new_entry = false;
	} else {
		if (core->count == core->capacity)
			return -ENOSPC;
		new_entry = true;
	}
	if (enqueue_event(core, &normalized))
		return -ENOMEM;
	if (new_entry) {
		entry = &core->entries[core->count++];
		memset(entry, 0, sizeof(*entry));
	}
	entry->object = normalized;
	entry->updated_ms = now_ms;
	entry->floor = false;
	*result = MIDR_CORE_ACCEPTED;
	return 0;
}

int midr_core_refresh(struct midr_core *core,
			      const struct midr_core_identity *key, uint64_t now_ms)
{
	const struct midr_core_entry *entry;
	struct midr_core_object object;

	if (!core || !key)
		return -EINVAL;
	entry = find_entry_const(core, key);
	if (!entry || entry->floor || entry->object.state != MIDR_CORE_ACTIVE)
		return -ENOENT;
	object = entry->object;
	if (object.sequence == UINT64_MAX)
		return -ERANGE;
	object.sequence++;
	object.lifetime_ms = core->config.lifetime_ms;
	return midr_core_upsert(core, &object, now_ms, &(enum midr_core_result){0});
}

int midr_core_withdraw(struct midr_core *core,
			       const struct midr_core_identity *key, uint64_t now_ms)
{
	const struct midr_core_entry *entry;
	struct midr_core_object object;
	struct midr_core_identity normalized;

	if (!core || !key || midr_core_identity_normalize(key, &normalized) ||
	    midr_core_identity_validate(&normalized))
		return -EINVAL;
	memset(&object, 0, sizeof(object));
	object.identity = normalized;
	object.state = MIDR_CORE_WITHDRAWN;
	object.sequence = 1;
	if ((entry = find_entry_const(core, key))) {
		if (entry->object.sequence == UINT64_MAX)
			return -ERANGE;
		object.sequence = entry->object.sequence + 1;
	}
	object.lifetime_ms = core->config.lifetime_ms;
	return midr_core_upsert(core, &object, now_ms, &(enum midr_core_result){0});
}

int midr_core_lookup(const struct midr_core *core,
			     const struct midr_core_identity *key, uint64_t now_ms,
			     struct midr_core_object *object, uint32_t *remaining_ms)
{
	const struct midr_core_entry *entry;
	uint64_t elapsed;

	if (!core || !key || !object)
		return -EINVAL;
	entry = find_entry_const(core, key);
	if (!entry || entry->floor)
		return -ENOENT;
	*object = entry->object;
	if (now_ms < entry->updated_ms)
		return -ERANGE;
	elapsed = now_ms - entry->updated_ms;
	if (elapsed >= entry->object.lifetime_ms)
		return -ENOENT;
	if (remaining_ms)
		*remaining_ms = entry->object.lifetime_ms - (uint32_t)elapsed;
	object->lifetime_ms = entry->object.lifetime_ms - (uint32_t)elapsed;
	return 0;
}

int midr_core_snapshot(struct midr_core *core, uint64_t now_ms,
			       struct midr_core_object *objects, size_t capacity,
			       size_t *count)
{
	size_t copied = 0;

	if (!core || !objects || !count)
		return -EINVAL;
	{
		int ret = midr_core_expire(core, now_ms, NULL);

		if (ret)
			return ret;
	}
	for (size_t i = 0; i < core->count; i++) {
		if (core->entries[i].floor)
			continue;
		if (copied == capacity)
			return -ENOSPC;
		if (midr_core_lookup(core, &core->entries[i].object.identity, now_ms,
				      &objects[copied], NULL))
			continue;
		copied++;
	}
	*count = copied;
	return 0;
}

int midr_core_expire(struct midr_core *core, uint64_t now_ms,
			    size_t *expired)
{
	size_t count = 0;

	if (!core)
		return -EINVAL;
	for (size_t i = 0; i < core->count; i++) {
		struct midr_core_entry *entry = &core->entries[i];

		if (entry->floor)
			continue;
		if (now_ms < entry->updated_ms)
			return -ERANGE;
		if (now_ms - entry->updated_ms < entry->object.lifetime_ms)
			continue;
		if (entry->object.state == MIDR_CORE_ACTIVE) {
			struct midr_core_object withdrawn = entry->object;

			if (withdrawn.sequence == UINT64_MAX)
				return -ERANGE;
			withdrawn.state = MIDR_CORE_WITHDRAWN;
			withdrawn.sequence++;
			withdrawn.metric = 0;
			memset(withdrawn.local_address, 0,
			       sizeof(withdrawn.local_address));
			memset(withdrawn.remote_address, 0,
			       sizeof(withdrawn.remote_address));
			if (enqueue_event(core, &withdrawn))
				return -ENOMEM;
		}
		entry->floor = true;
		count++;
	}
	if (expired)
		*expired = count;
	return 0;
}

size_t midr_core_count(const struct midr_core *core)
{
	size_t count = 0;

	if (!core)
		return 0;
	for (size_t i = 0; i < core->count; i++)
		if (!core->entries[i].floor)
			count++;
	return count;
}

int midr_core_event_next(struct midr_core *core,
			 struct midr_core_object *object)
{
	struct midr_core_event *event;

	if (!core || !object)
		return -EINVAL;
	event = core->events_head;
	if (!event)
		return -ENOENT;
	core->events_head = event->next;
	if (!core->events_head)
		core->events_tail = NULL;
	*object = event->object;
	free(event);
	return 0;
}
