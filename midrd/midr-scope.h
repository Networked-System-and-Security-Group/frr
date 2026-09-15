/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Protocol-neutral scope and usable export decisions. */
#ifndef MIDRD_SCOPE_H
#define MIDRD_SCOPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "midr-core.h"

struct midr_scope;

struct midr_scope_config {
	uint32_t local_node_id;
	size_t max_memberships;
};

int midr_scope_create(const struct midr_scope_config *config,
			     struct midr_scope **out);
int midr_scope_clone(const struct midr_scope *source,
			    struct midr_scope **out);
void midr_scope_destroy(struct midr_scope **scope);
int midr_scope_apply(struct midr_scope *scope,
			    const struct midr_core_object *object);
bool midr_scope_export(const struct midr_scope *scope,
			      const struct midr_core_object *object,
			      uint32_t peer_node_id);
int midr_scope_membership(const struct midr_scope *scope,
				  uint32_t node_id, uint32_t *group);
int midr_scope_representative(const struct midr_scope *scope,
				      uint32_t group, uint32_t *node_id);

#endif /* MIDRD_SCOPE_H */
