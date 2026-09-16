/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MIDRD_LSDB_H
#define MIDRD_LSDB_H

#include <stddef.h>
#include <stdint.h>

#include "midr-consumer.h"
#include "midr-core.h"
#include "midr-scope.h"

enum midr_lsdb_entry_state {
	MIDR_LSDB_USABLE = 1,
	MIDR_LSDB_PENDING = 2,
	MIDR_LSDB_OUT_OF_SCOPE = 3,
};

struct midr_lsdb_entry {
	struct midr_core_object object;
	enum midr_lsdb_entry_state state;
};

struct midr_lsdb_config {
	size_t max_objects;
};

struct midr_lsdb_snapshot {
	uint64_t generation;
	struct midr_lsdb_entry *entries;
	size_t count;
	size_t usable_count;
	size_t pending_count;
};

struct midr_lsdb;
struct midr_lsdb_stage;

int midr_lsdb_create(const struct midr_lsdb_config *config,
		     struct midr_lsdb **out);
void midr_lsdb_destroy(struct midr_lsdb **lsdb);

int midr_lsdb_prepare(struct midr_lsdb *lsdb, uint64_t generation,
		      const struct midr_core_object *objects, size_t count,
		      const struct midr_scope *scope,
		      struct midr_lsdb_stage **stage);
void midr_lsdb_commit_prepared(struct midr_lsdb *lsdb,
			       struct midr_lsdb_stage **stage);
void midr_lsdb_abort_prepared(struct midr_lsdb_stage **stage);

/* The returned event array is borrowed from stage until commit or abort. */
int midr_lsdb_stage_consumer_snapshot(
	const struct midr_lsdb_stage *stage,
	struct midr_consumer_snapshot *snapshot);
/* Entries are borrowed from stage and remain valid until commit or abort. */
int midr_lsdb_stage_snapshot(const struct midr_lsdb_stage *stage,
			     struct midr_lsdb_snapshot *snapshot);

int midr_lsdb_snapshot_acquire(const struct midr_lsdb *lsdb,
			       struct midr_lsdb_snapshot *snapshot);
void midr_lsdb_snapshot_release(struct midr_lsdb_snapshot *snapshot);
uint64_t midr_lsdb_generation(const struct midr_lsdb *lsdb);

/* Test-only fault injection.  The next prepare fails before publication. */
int midr_lsdb_test_fail_next(struct midr_lsdb *lsdb, int error);

#endif /* MIDRD_LSDB_H */
