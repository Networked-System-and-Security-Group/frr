/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Standalone MIDR object core.
 *
 * This header deliberately contains no FRR or BGP type.  The standalone
 * daemon and its tests use this API as the protocol-neutral boundary.
 */
#ifndef MIDRD_CORE_H
#define MIDRD_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MIDR_CORE_ADDR_BYTES 16U

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

struct midr_core_key {
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
	struct midr_core_key key;
	uint8_t state;
	uint8_t reserved[3];
	uint64_t sequence;
	uint32_t lifetime_ms;
	uint32_t metric;
	uint8_t local_address[MIDR_CORE_ADDR_BYTES];
	uint8_t remote_address[MIDR_CORE_ADDR_BYTES];
};

struct midr_core;

struct midr_core_config {
	size_t max_objects;
	uint32_t lifetime_ms;
};

int midr_core_key_normalize(const struct midr_core_key *input,
				   struct midr_core_key *output);
int midr_core_key_validate(const struct midr_core_key *key);
bool midr_core_key_equal(const struct midr_core_key *a,
				const struct midr_core_key *b);
bool midr_core_object_semantic_equal(const struct midr_core_object *a,
					     const struct midr_core_object *b);

int midr_core_create(const struct midr_core_config *config,
			    struct midr_core **out);
void midr_core_destroy(struct midr_core **core);

int midr_core_upsert(struct midr_core *core,
			 const struct midr_core_object *object,
			 uint64_t now_ms, enum midr_core_result *result);
int midr_core_refresh(struct midr_core *core,
			      const struct midr_core_key *key, uint64_t now_ms);
int midr_core_withdraw(struct midr_core *core,
			       const struct midr_core_key *key, uint64_t now_ms);

/* Copy the current object and its remaining lifetime. */
int midr_core_lookup(const struct midr_core *core,
			     const struct midr_core_key *key, uint64_t now_ms,
			     struct midr_core_object *object, uint32_t *remaining_ms);
int midr_core_snapshot(struct midr_core *core, uint64_t now_ms,
			       struct midr_core_object *objects, size_t capacity,
			       size_t *count);
int midr_core_expire(struct midr_core *core, uint64_t now_ms,
			    size_t *expired);
size_t midr_core_count(const struct midr_core *core);

/* Accepted updates are emitted exactly once. */
int midr_core_event_next(struct midr_core *core,
			 struct midr_core_object *object);

#endif
