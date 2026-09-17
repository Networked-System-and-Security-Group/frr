/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MIDRD_SPF_INSTALL_H
#define MIDRD_SPF_INSTALL_H

#include <stdbool.h>
#include <stdint.h>

struct midr_context;
struct midr_spf_results;

struct midr_spf_install_status {
	uint64_t desired_generation;
	uint64_t reconcile_count;
	uint64_t withdraw_count;
	int last_error;
	bool active;
};

/* Compare two immutable SPF result sets and stage one complete route diff.
 * Either pointer may be NULL to represent an empty set. */
int midr_spf_install_results(struct midr_context *ctx,
			     const struct midr_spf_results *old_results,
			     const struct midr_spf_results *new_results);

/* The Zebra backend registration normally starts and stops this runtime.
 * resync reconciles a failed submission against the last accepted result.
 * replay emits the complete current result after the backend has discarded
 * stale installed state on a Zebra reconnect. */
int midr_spf_install_start(struct midr_context *ctx);
int midr_spf_install_stop(struct midr_context *ctx);
int midr_spf_install_resync(struct midr_context *ctx);
int midr_spf_install_replay(struct midr_context *ctx);
int midr_spf_install_status_get(struct midr_context *ctx,
				struct midr_spf_install_status *status);

#endif /* MIDRD_SPF_INSTALL_H */
