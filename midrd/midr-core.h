/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Protocol-neutral MIDR object core contract.
 *
 * This header is deliberately independent of FRR, BGP and socket headers.
 * Inputs and outputs are caller-owned copies.  The R7 implementation may
 * choose its internal storage, but it must preserve these value semantics.
 */
#ifndef MIDRD_CORE_H
#define MIDRD_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MIDR_CORE_ADDR_BYTES 16U
#define MIDR_CORE_WIRE_VERSION 1U

enum midr_core_object_type {
	MIDR_CORE_MEMBERSHIP = 1,
	MIDR_CORE_LINK = 2,
	MIDR_CORE_NODE_PREFIX = 3,
	MIDR_CORE_GROUP_PREFIX = 4,
};

enum midr_core_family {
	MIDR_CORE_AF_NONE = 0,
	MIDR_CORE_AF_IPV4 = 4,
	MIDR_CORE_AF_IPV6 = 6,
};

enum midr_core_state {
	MIDR_CORE_ACTIVE = 1,
	MIDR_CORE_WITHDRAWN = 2,
};

enum midr_core_result {
	MIDR_CORE_ACCEPTED = 0,
	MIDR_CORE_DUPLICATE,
	MIDR_CORE_OLDER,
	MIDR_CORE_CONFLICT,
};

enum midr_core_error {
	MIDR_CORE_ERR_INVALID = 1,
	MIDR_CORE_ERR_NOT_FOUND = 2,
	MIDR_CORE_ERR_CAPACITY = 3,
	MIDR_CORE_ERR_RETRY = 4,
};

/*
 * Identity is the canonical key.  For LINK/MEMBERSHIP, family/prefix are
 * NONE/0; NODE_PREFIX and GROUP_PREFIX carry an IPv4 or IPv6 prefix.
 * originator is the stable node identity; remote/group/link_id carry the
 * object-specific relationship dimensions.  Membership's group is a payload
 * field on midr_core_object, rather than an identity dimension.
 */
struct midr_core_identity {
	uint8_t type;
	uint8_t family;
	uint8_t prefix_len;
	uint8_t reserved;
	uint32_t originator;
	uint32_t remote;
	uint32_t group;
	uint64_t link_id;
	uint8_t prefix[MIDR_CORE_ADDR_BYTES];
};

struct midr_core_object {
	struct midr_core_identity identity;
	uint8_t state;
	/* Link endpoint address family.  It is payload, not identity. */
	uint8_t address_family;
	uint8_t reserved[2];
	uint64_t sequence;
	uint32_t lifetime_ms;
	/* Membership payload.  group is not part of Membership identity. */
	uint32_t group;
	uint32_t metric;
	uint8_t local_address[MIDR_CORE_ADDR_BYTES];
	uint8_t remote_address[MIDR_CORE_ADDR_BYTES];
};

struct midr_core_config {
	size_t max_objects;
	uint32_t lifetime_ms;
	/* Floor reclamation is disabled unless explicitly enabled. */
	bool gc_enabled;
};

struct midr_core;

typedef bool (*midr_core_reclaim_cb)(
	const struct midr_core_identity *identity, void *arg);

int midr_core_identity_normalize(const struct midr_core_identity *input,
				 struct midr_core_identity *output);
int midr_core_identity_validate(const struct midr_core_identity *identity);
bool midr_core_identity_equal(const struct midr_core_identity *a,
			      const struct midr_core_identity *b);
bool midr_core_object_semantic_equal(const struct midr_core_object *a,
				     const struct midr_core_object *b);

/*
 * Add elapsed monotonic time and a forwarding budget to an encoded age.
 * Sub-millisecond elapsed time is rounded up and the result saturates at
 * max_age_ms.  A monotonic clock rollback is reported as -ERANGE.
 */
int midr_core_age(uint32_t received_ms, uint64_t received_ns,
		  uint64_t now_ns, uint32_t budget_ms,
		  uint32_t max_age_ms, uint32_t *age_ms);
/* Convert an encoded remaining lifetime using the same age arithmetic. */
int midr_core_lifetime_remaining(uint32_t received_remaining_ms,
				 uint64_t received_ns, uint64_t now_ns,
				 uint32_t budget_ms,
				 uint32_t max_lifetime_ms,
				 uint32_t *remaining_ms);

int midr_core_create(const struct midr_core_config *config,
			     struct midr_core **out);
/* Create an independent value copy, including pending events. */
int midr_core_clone(const struct midr_core *source,
			   struct midr_core **out);
void midr_core_destroy(struct midr_core **core);

/*
 * Upsert takes a value copy.  An accepted object is emitted once by
 * midr_core_event_next(); duplicate, older and same-sequence conflicts do not
 * emit an event.  now_ms is a monotonic timestamp supplied by the caller.
 */
int midr_core_upsert(struct midr_core *core,
		     const struct midr_core_object *object,
		     uint64_t now_ms, enum midr_core_result *result);
int midr_core_refresh(struct midr_core *core,
		      const struct midr_core_identity *identity,
		      uint64_t now_ms);
int midr_core_withdraw(struct midr_core *core,
		       const struct midr_core_identity *identity,
		       uint64_t now_ms);
int midr_core_lookup(const struct midr_core *core,
		     const struct midr_core_identity *identity,
		     uint64_t now_ms, struct midr_core_object *object,
		     uint32_t *remaining_ms);
int midr_core_snapshot(struct midr_core *core, uint64_t now_ms,
		       struct midr_core_object *objects, size_t capacity,
		       size_t *count);
int midr_core_expire(struct midr_core *core, uint64_t now_ms,
		     size_t *expired);
int midr_core_gc_enable(struct midr_core *core, bool enabled);
int midr_core_gc(struct midr_core *core, uint64_t now_ms, size_t limit,
		 size_t *collected, midr_core_reclaim_cb reclaim,
		 void *reclaim_arg);
size_t midr_core_count(const struct midr_core *core);
size_t midr_core_identity_count(const struct midr_core *core);
int midr_core_event_next(struct midr_core *core,
			 struct midr_core_object *object);

#endif /* MIDRD_CORE_H */
