// SPDX-License-Identifier: GPL-2.0-or-later
#include <zebra.h>
#include <errno.h>

#include "bgpd/bgp_midr_canonical.h"

#define MIDR_CANONICAL_BUCKETS 1024U

struct midr_instance_ref {
	struct midr_instance value;
	uint64_t received_ns;
	uint32_t received_ms;
	size_t refs;
	void (*free)(void *, void *);
	void *alloc_arg;
};

struct midr_canonical_entry {
	struct midr_canonical_entry *next;
	struct midr_ls_object_key key;
	uint64_t sequence;
	uint64_t observed_ns;
	uint32_t received_ms;
	enum midr_canonical_state state;
	const struct midr_instance_ref *current;
	uint64_t floor_ns;
};

struct midr_canonical_event {
	struct midr_canonical_event *next;
	struct midr_ls_object_key key;
	uint64_t sequence;
	enum midr_canonical_change change;
	const struct midr_instance_ref *before;
	const struct midr_instance_ref *after;
};

struct midr_canonical {
	struct midr_canonical_config config;
	struct midr_canonical_entry *buckets[MIDR_CANONICAL_BUCKETS];
	struct midr_canonical_event *head;
	struct midr_canonical_event *tail;
	size_t identities;
	size_t events;
	bool gc_enabled;
	size_t sweep_bucket;
	size_t sweep_offset;
};

static void *midr_canonical_alloc(size_t size, void *arg)
{
	(void)arg;
	return calloc(1, size);
}

static void midr_canonical_free(void *ptr, void *arg)
{
	(void)arg;
	free(ptr);
}

int midr_canonical_create(const struct midr_canonical_config *config,
			  struct midr_canonical **out)
{
	struct midr_canonical_config c;
	struct midr_canonical *store;

	if (!config || !out || *out || !config->now_ns || !config->max_age_ms ||
	    !config->identity_limit || !config->event_limit || !!config->alloc != !!config->free)
		return -EINVAL;
	c = *config;
	if (!c.alloc) {
		c.alloc = midr_canonical_alloc;
		c.free = midr_canonical_free;
	}
	store = c.alloc(sizeof(*store), c.alloc_arg);
	if (!store)
		return -ENOMEM;
	store->config = c;
	*out = store;
	return 0;
}

static struct midr_canonical_entry *midr_canonical_find(const struct midr_canonical *store,
						     const struct midr_ls_object_key *key)
{
	struct midr_canonical_entry *entry;
	unsigned int bucket = midr_ls_object_key_hash(key) % MIDR_CANONICAL_BUCKETS;

	for (entry = store->buckets[bucket]; entry; entry = entry->next)
		if (midr_ls_object_key_same(&entry->key, key))
			return entry;
	return NULL;
}

const struct midr_instance *midr_instance_ref_value(const struct midr_instance_ref *ref)
{
	return ref ? &ref->value : NULL;
}

int midr_instance_ref_acquire(const struct midr_instance_ref *ref)
{
	struct midr_instance_ref *r = (struct midr_instance_ref *)ref;

	if (!r)
		return -EINVAL;
	if (r->refs == SIZE_MAX)
		return -EOVERFLOW;
	r->refs++;
	return 0;
}

void midr_instance_ref_release(const struct midr_instance_ref **ref)
{
	struct midr_instance_ref *r;

	if (!ref || !*ref)
		return;
	r = (struct midr_instance_ref *)*ref;
	*ref = NULL;
	assert(r->refs);
	if (!--r->refs)
		r->free(r, r->alloc_arg);
}

int midr_instance_ref_age(const struct midr_instance_ref *ref, uint64_t now_ns,
			  uint32_t budget_ms, uint32_t max_age_ms, uint32_t *age_ms)
{
	if (!ref)
		return -EINVAL;
	return midr_instance_age(ref->received_ms, ref->received_ns, now_ns,
				 budget_ms, max_age_ms, age_ms);
}

static void midr_canonical_enqueue(struct midr_canonical *store, struct midr_canonical_event *event)
{
	if (store->tail)
		store->tail->next = event;
	else
		store->head = event;
	store->tail = event;
	store->events++;
}

static bool midr_canonical_has_event_for(
		const struct midr_canonical *store,
		const struct midr_ls_object_key *key)
{
	const struct midr_canonical_event *event;

	for (event = store->head; event; event = event->next)
		if (midr_ls_object_key_same(&event->key, key))
			return true;
	return false;
}

int midr_canonical_accept(struct midr_canonical *store, const struct midr_instance *instance,
			  uint32_t age_ms, enum midr_canonical_result *result)
{
	struct midr_canonical_entry *entry, *fresh = NULL;
	struct midr_canonical_event *event;
	struct midr_instance_ref *record = NULL;
	struct midr_canonical_config *c;
	uint64_t now;
	bool conflict = false;
	int ret;

	if (!store || !result || midr_instance_validate(instance))
		return -EINVAL;
	c = &store->config;
	now = c->now_ns(c->clock_arg);
	entry = midr_canonical_find(store, &instance->object.key);
	if (entry && now < entry->observed_ns)
		return -ERANGE;
	if (age_ms >= c->max_age_ms) {
		*result = MIDR_CANONICAL_EXPIRED;
		return 0;
	}
	if (entry && instance->object.ls_sequence <= entry->sequence) {
		uint32_t current_age;

		if (instance->object.ls_sequence < entry->sequence) {
			*result = MIDR_CANONICAL_OLDER;
			return 0;
		}
		ret = midr_instance_age(entry->received_ms, entry->observed_ns, now,
					0, c->max_age_ms, &current_age);
		if (ret)
			return ret;
		if (entry->state == MIDR_CANONICAL_FLOOR || current_age == c->max_age_ms) {
			*result = MIDR_CANONICAL_EXPIRED;
			return 0;
		}
		if (entry->state == MIDR_CANONICAL_QUARANTINED) {
			*result = MIDR_CANONICAL_CONFLICT;
			return 0;
		}
		if (midr_instance_same(&entry->current->value, instance)) {
			*result = MIDR_CANONICAL_DUPLICATE;
			return 0;
		}
		conflict = true;
	}
	if (store->events >= c->event_limit || (!entry && store->identities >= c->identity_limit))
		return -ENOSPC;
	/* Prepare every allocation before changing the accepted version. */
	if (!entry) {
		fresh = c->alloc(sizeof(*fresh), c->alloc_arg);
		if (!fresh)
			return -ENOMEM;
		fresh->key = instance->object.key;
	}
	event = c->alloc(sizeof(*event), c->alloc_arg);
	if (!event) {
		c->free(fresh, c->alloc_arg);
		return -ENOMEM;
	}
	if (!conflict) {
		record = c->alloc(sizeof(*record), c->alloc_arg);
		if (!record) {
			c->free(event, c->alloc_arg);
			c->free(fresh, c->alloc_arg);
			return -ENOMEM;
		}
		record->value = *instance;
		record->received_ms = age_ms;
		record->received_ns = now;
		record->refs = 2; /* entry and pending event */
		record->free = c->free;
		record->alloc_arg = c->alloc_arg;
	}
	if (fresh) {
		unsigned int bucket = midr_ls_object_key_hash(&fresh->key) % MIDR_CANONICAL_BUCKETS;

		entry = fresh;
		entry->next = store->buckets[bucket];
		store->buckets[bucket] = entry;
		store->identities++;
	}
	event->key = entry->key;
	event->sequence = instance->object.ls_sequence;
	event->before = entry->current; /* transfer entry's previous reference */
	event->after = record;
	event->change = conflict ? MIDR_CANONICAL_ISOLATE : MIDR_CANONICAL_REPLACE;
	entry->current = record;
	entry->sequence = instance->object.ls_sequence;
	entry->state = conflict ? MIDR_CANONICAL_QUARANTINED : MIDR_CANONICAL_CURRENT;
	if (!conflict) {
		entry->observed_ns = now;
		entry->received_ms = age_ms;
	}
	midr_canonical_enqueue(store, event);
	*result = conflict ? MIDR_CANONICAL_CONFLICT : MIDR_CANONICAL_ACCEPTED;
	return 0;
}

int midr_canonical_lookup(const struct midr_canonical *store,
			  const struct midr_ls_object_key *key, struct midr_canonical_view *view)
{
	struct midr_canonical_entry *entry;
	uint32_t age;
	int ret;

	if (!store || !view || midr_ls_object_key_validate(key))
		return -EINVAL;
	memset(view, 0, sizeof(*view));
	entry = midr_canonical_find(store, key);
	if (!entry)
		return -ENOENT;
	ret = midr_instance_age(entry->received_ms, entry->observed_ns,
				store->config.now_ns(store->config.clock_arg), 0,
				store->config.max_age_ms, &age);
	if (ret)
		return ret;
	view->sequence = entry->sequence;
	view->observed_ns = entry->observed_ns;
	view->age_ms = age;
	view->state = age == store->config.max_age_ms ? MIDR_CANONICAL_FLOOR : entry->state;
	if (view->state == MIDR_CANONICAL_CURRENT)
		view->current = entry->current;
	return 0;
}

int midr_canonical_expire(struct midr_canonical *store, const struct midr_ls_object_key *key)
{
	struct midr_canonical_view view;
	struct midr_canonical_entry *entry;
	struct midr_canonical_event *event;
	int ret = midr_canonical_lookup(store, key, &view);

	if (ret)
		return ret;
	entry = midr_canonical_find(store, key);
	if (entry->state == MIDR_CANONICAL_FLOOR)
		return 0;
	if (view.age_ms != store->config.max_age_ms)
		return -EAGAIN;
	if (store->events >= store->config.event_limit)
		return -ENOSPC;
	event = store->config.alloc(sizeof(*event), store->config.alloc_arg);
	if (!event)
		return -ENOMEM;
	event->key = *key;
	event->sequence = entry->sequence;
	event->change = MIDR_CANONICAL_EXPIRE;
	event->before = entry->current;
	entry->current = NULL;
	entry->state = MIDR_CANONICAL_FLOOR;
	/* The retention clock starts when this version was first accepted, not
	 * when its active lifetime is later observed to have elapsed. */
	entry->floor_ns = entry->observed_ns;
	midr_canonical_enqueue(store, event);
	return 0;
}

int midr_canonical_sweep(struct midr_canonical *store, size_t limit,
			 size_t *expired)
{
	size_t inspected = 0;
	size_t done = 0;
	size_t bucket;
	size_t offset;
	size_t buckets_visited = 0;
	int result = 0;

	if (!store || !limit)
		return -EINVAL;
	bucket = store->sweep_bucket % MIDR_CANONICAL_BUCKETS;
	offset = store->sweep_offset;
	while (inspected < limit && buckets_visited < MIDR_CANONICAL_BUCKETS) {
		struct midr_canonical_entry *entry;

		entry = store->buckets[bucket];
		while (entry && offset) {
			entry = entry->next;
			offset--;
		}
		if (!entry) {
			bucket = (bucket + 1) % MIDR_CANONICAL_BUCKETS;
			offset = 0;
			buckets_visited++;
			continue;
		}

		while (entry && inspected < limit) {
			struct midr_canonical_entry *next = entry->next;
			int ret;

			inspected++;
			offset++;
			if (entry->state == MIDR_CANONICAL_FLOOR) {
				entry = next;
				continue;
			}
			ret = midr_canonical_expire(store, &entry->key);
			if (!ret)
				done++;
			else if (ret != -EAGAIN && ret != -ENOENT && !result)
				result = ret;
			entry = next;
		}
		if (!entry) {
			bucket = (bucket + 1) % MIDR_CANONICAL_BUCKETS;
			offset = 0;
			buckets_visited++;
		}
	}
	store->sweep_bucket = bucket;
	store->sweep_offset = offset;
	if (expired)
		*expired = done;
	return result;
}

int midr_canonical_gc(struct midr_canonical *store, size_t limit,
			  size_t *collected)
{
	size_t done = 0;
	uint64_t now;

	if (!store || !limit)
		return -EINVAL;
	if (!store->gc_enabled) {
		if (collected)
			*collected = 0;
		return 0;
	}
	now = store->config.now_ns(store->config.clock_arg);
	for (size_t i = 0; i < MIDR_CANONICAL_BUCKETS && done < limit; i++) {
		struct midr_canonical_entry **link = &store->buckets[i];

		while (*link && done < limit) {
			struct midr_canonical_entry *entry = *link;
			uint64_t retention_ns;

			if (entry->state != MIDR_CANONICAL_FLOOR || entry->current ||
			    midr_canonical_has_event_for(store, &entry->key)) {
				link = &entry->next;
				continue;
			}
			retention_ns = (uint64_t)store->config.max_age_ms * 1000000ULL;
			if (now < entry->floor_ns ||
			    now - entry->floor_ns < retention_ns) {
				link = &entry->next;
				continue;
			}
			*link = entry->next;
			store->identities--;
			store->config.free(entry, store->config.alloc_arg);
			done++;
			store->sweep_bucket = 0;
			store->sweep_offset = 0;
		}
	}
	if (collected)
		*collected = done;
	return 0;
}

int midr_canonical_gc_enable(struct midr_canonical *store, bool enabled)
{
	if (!store)
		return -EINVAL;
	store->gc_enabled = enabled;
	return 0;
}

uint32_t midr_canonical_max_age_ms(const struct midr_canonical *store)
{
	return store ? store->config.max_age_ms : 0;
}

size_t midr_canonical_identity_count(const struct midr_canonical *store)
{
	return store ? store->identities : 0;
}

size_t midr_canonical_event_count(const struct midr_canonical *store)
{
	return store ? store->events : 0;
}

const struct midr_canonical_event *midr_canonical_event_peek(const struct midr_canonical *store)
{
	return store ? store->head : NULL;
}

void midr_canonical_event_ack(struct midr_canonical *store)
{
	struct midr_canonical_event *event;

	if (!store || !store->head)
		return;
	event = store->head;
	store->head = event->next;
	if (!store->head)
		store->tail = NULL;
	store->events--;
	midr_instance_ref_release(&event->before);
	midr_instance_ref_release(&event->after);
	store->config.free(event, store->config.alloc_arg);
}

enum midr_canonical_change midr_canonical_event_change(const struct midr_canonical_event *event)
{
	return event->change;
}

const struct midr_ls_object_key *midr_canonical_event_key(const struct midr_canonical_event *event)
{
	return &event->key;
}

uint64_t midr_canonical_event_sequence(const struct midr_canonical_event *event)
{
	return event->sequence;
}

const struct midr_instance_ref *midr_canonical_event_before(const struct midr_canonical_event *event)
{
	return event->before;
}

const struct midr_instance_ref *midr_canonical_event_after(const struct midr_canonical_event *event)
{
	return event->after;
}

void midr_canonical_destroy(struct midr_canonical **storep)
{
	struct midr_canonical *store;

	if (!storep || !*storep)
		return;
	store = *storep;
	*storep = NULL;
	while (store->head)
		midr_canonical_event_ack(store);
	for (size_t i = 0; i < MIDR_CANONICAL_BUCKETS; i++) {
		struct midr_canonical_entry *entry = store->buckets[i];

		while (entry) {
			struct midr_canonical_entry *next = entry->next;

			midr_instance_ref_release(&entry->current);
			store->config.free(entry, store->config.alloc_arg);
			entry = next;
		}
	}
	store->config.free(store, store->config.alloc_arg);
}
