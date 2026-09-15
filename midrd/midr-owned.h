/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Local MIDR ownership and sequence allocation.
 *
 * The owned store contains only local authority state.  It deliberately does
 * not know about transports, BGP or downstream consumers; callers publish the
 * generated object through the callback and may retry a failed operation.
 */
#ifndef MIDRD_OWNED_H
#define MIDRD_OWNED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "midr-core.h"

struct midr_owned;

struct midr_owned_config {
	uint32_t originator;
	size_t max_objects;
	uint32_t lifetime_ms;
	const char *sequence_file;
};

typedef int (*midr_owned_publish_cb)(void *arg,
					     const struct midr_core_object *object);

int midr_owned_create(const struct midr_owned_config *config,
			      midr_owned_publish_cb publish, void *arg,
			      struct midr_owned **out);
int midr_owned_clone(const struct midr_owned *source,
			     midr_owned_publish_cb publish, void *arg,
			     struct midr_owned **out);
void midr_owned_destroy(struct midr_owned **owned);

int midr_owned_upsert(struct midr_owned *owned,
			     const struct midr_core_object *fact);
int midr_owned_refresh(struct midr_owned *owned,
			      const struct midr_core_identity *identity);
int midr_owned_withdraw(struct midr_owned *owned,
			       const struct midr_core_identity *identity);
int midr_owned_withdraw_all(struct midr_owned *owned, size_t *withdrawn);

int midr_owned_lookup(const struct midr_owned *owned,
			     const struct midr_core_identity *identity,
			     struct midr_core_object *object);
size_t midr_owned_count(const struct midr_owned *owned);
uint64_t midr_owned_last_sequence(const struct midr_owned *owned);
/* Preserve a sequence high-water mark after a staged transaction aborts.
 * Gaps are valid; reusing an already persisted value is not. */
int midr_owned_sequence_floor(struct midr_owned *owned, uint64_t sequence);

#endif /* MIDRD_OWNED_H */
