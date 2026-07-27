// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR peer End-of-RIB synchronization barrier.
 */

#ifndef _FRR_BGP_MIDR_SYNC_H
#define _FRR_BGP_MIDR_SYNC_H

#include <zebra.h>

#include <stdbool.h>
#include <stdint.h>

struct midr_context;
struct peer;

#define MIDR_EOR_TIMEOUT_DEFAULT 60U
#define MIDR_EOR_TIMEOUT_MIN 1U
#define MIDR_EOR_TIMEOUT_MAX 3600U

enum midr_sync_state {
	MIDR_SYNC_LOCAL_WAIT,
	MIDR_SYNC_REMOTE_WAIT,
	MIDR_SYNC_READY,
};

struct midr_sync_status {
	enum midr_sync_state state;
	uint32_t timeout_seconds;
	size_t initial_peer_count;
	size_t waiting_peer_count;
	size_t timed_out_peer_count;
	uint64_t barrier_count;
	uint64_t timeout_count;
};

extern int midr_sync_init(struct midr_context *ctx);
extern void midr_sync_finish(struct midr_context *ctx);
extern bool midr_sync_view_ready(struct midr_context *ctx, bool local_ready,
				 uint64_t *reason_flags);
extern bool midr_sync_local_ready(const struct midr_context *ctx);
extern void midr_sync_peer_status_changed(struct midr_context *ctx, struct peer *peer);
extern void midr_sync_peer_eor(struct midr_context *ctx, struct peer *peer);
extern int midr_sync_timeout_set(struct midr_context *ctx, uint32_t seconds);
extern int midr_sync_status_get(struct midr_context *ctx, struct midr_sync_status *status);
extern void midr_sync_test_timeout(struct midr_context *ctx);

#endif /* _FRR_BGP_MIDR_SYNC_H */
