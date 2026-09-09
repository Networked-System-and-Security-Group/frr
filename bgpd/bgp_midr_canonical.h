// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _FRR_BGP_MIDR_CANONICAL_H
#define _FRR_BGP_MIDR_CANONICAL_H

#include "bgpd/bgp_midr_instance.h"

struct midr_canonical;
struct midr_instance_ref;
struct midr_canonical_event;

enum midr_canonical_result {
	MIDR_CANONICAL_ACCEPTED,
	MIDR_CANONICAL_DUPLICATE,
	MIDR_CANONICAL_OLDER,
	MIDR_CANONICAL_CONFLICT,
	MIDR_CANONICAL_EXPIRED,
};

enum midr_canonical_state {
	MIDR_CANONICAL_CURRENT,
	MIDR_CANONICAL_QUARANTINED,
	MIDR_CANONICAL_FLOOR,
};

enum midr_canonical_change {
	MIDR_CANONICAL_REPLACE,
	MIDR_CANONICAL_ISOLATE,
	MIDR_CANONICAL_EXPIRE,
};

/* All calls run on the owner event loop. Allocator arg outlives held refs. */
struct midr_canonical_config {
	size_t identity_limit;
	size_t event_limit;
	uint32_t max_age_ms;
	uint64_t (*now_ns)(void *arg);
	void *clock_arg;
	/* Optional pair: zeroed allocation, NULL on failure, and matching free. */
	void *(*alloc)(size_t size, void *arg);
	void (*free)(void *ptr, void *arg);
	void *alloc_arg;
};

struct midr_canonical_view {
	uint64_t sequence;
	enum midr_canonical_state state;
	uint32_t age_ms;
	uint64_t observed_ns;
	/* Borrowed, only present for a nonexpired CURRENT instance. */
	const struct midr_instance_ref *current;
};

extern int midr_canonical_create(const struct midr_canonical_config *config,
				struct midr_canonical **out);
extern void midr_canonical_destroy(struct midr_canonical **store);
extern int midr_canonical_accept(struct midr_canonical *store,
				 const struct midr_instance *instance,
				 uint32_t age_ms, enum midr_canonical_result *result);
extern int midr_canonical_lookup(const struct midr_canonical *store,
				 const struct midr_ls_object_key *key,
				 struct midr_canonical_view *view);
/* Expire locally; floor GC is deliberately left to the proven P3 policy. */
extern int midr_canonical_expire(struct midr_canonical *store,
				 const struct midr_ls_object_key *key);
extern size_t midr_canonical_identity_count(const struct midr_canonical *store);
extern size_t midr_canonical_event_count(const struct midr_canonical *store);

/* Peek then ack only after downstream work is secured; failed work can retry. */
extern const struct midr_canonical_event *midr_canonical_event_peek(const struct midr_canonical *store);
extern void midr_canonical_event_ack(struct midr_canonical *store);
extern enum midr_canonical_change midr_canonical_event_change(const struct midr_canonical_event *event);
extern const struct midr_ls_object_key *midr_canonical_event_key(const struct midr_canonical_event *event);
extern uint64_t midr_canonical_event_sequence(const struct midr_canonical_event *event);
extern const struct midr_instance_ref *midr_canonical_event_before(const struct midr_canonical_event *event);
extern const struct midr_instance_ref *midr_canonical_event_after(const struct midr_canonical_event *event);

extern const struct midr_instance *midr_instance_ref_value(const struct midr_instance_ref *ref);
extern int midr_instance_ref_acquire(const struct midr_instance_ref *ref);
extern void midr_instance_ref_release(const struct midr_instance_ref **ref);
extern int midr_instance_ref_age(const struct midr_instance_ref *ref,
				 uint64_t now_ns, uint32_t budget_ms,
				 uint32_t max_age_ms, uint32_t *age_ms);

#endif
