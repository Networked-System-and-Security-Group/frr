// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Persistent sequence allocation for locally originated MIDR objects.
 */

#ifndef _FRR_BGP_MIDR_SEQUENCE_H
#define _FRR_BGP_MIDR_SEQUENCE_H

#include <zebra.h>

#include <stdbool.h>
#include <stdint.h>

struct midr_sequence_store_ops {
	int (*load_epoch)(void *arg, uint32_t node_id, uint32_t *epoch, bool *found);
	int (*save_epoch)(void *arg, uint32_t node_id, uint32_t epoch);
};

struct midr_sequence_allocator {
	uint32_t node_id;
	uint32_t boot_epoch;
	uint32_t origin_counter;
	bool ready;
	const struct midr_sequence_store_ops *store_ops;
	void *store_arg;
};

extern int midr_sequence_allocator_init(struct midr_sequence_allocator *allocator, uint32_t node_id,
					const struct midr_sequence_store_ops *store_ops,
					void *store_arg);
extern int midr_sequence_allocator_next(struct midr_sequence_allocator *allocator,
					uint64_t *sequence);
extern int midr_sequence_allocator_advance_past(struct midr_sequence_allocator *allocator,
						uint64_t observed_sequence);
extern bool midr_sequence_allocator_is_ready(const struct midr_sequence_allocator *allocator);

extern const struct midr_sequence_store_ops midr_sequence_frr_store_ops;

#endif /* _FRR_BGP_MIDR_SEQUENCE_H */
