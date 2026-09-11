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
struct peer_connection;
struct stream;

#define MIDR_EOR_TIMEOUT_DEFAULT 60U
#define MIDR_EOR_TIMEOUT_MIN 1U
#define MIDR_EOR_TIMEOUT_MAX 3600U

enum midr_sync_state {
	MIDR_SYNC_LOCAL_WAIT,
	MIDR_SYNC_REMOTE_WAIT,
	MIDR_SYNC_READY,
};

enum midr_sync_session_state {
	MIDR_SYNC_SESSION_SYNCING,
	MIDR_SYNC_SESSION_READY,
	MIDR_SYNC_SESSION_DOWN,
	MIDR_SYNC_SESSION_RESYNC,
};

enum midr_sync_shutdown_state {
	MIDR_SYNC_SHUTDOWN_IDLE,
	MIDR_SYNC_SHUTDOWN_WAITING,
	MIDR_SYNC_SHUTDOWN_COMPLETE,
	MIDR_SYNC_SHUTDOWN_DEGRADED,
	MIDR_SYNC_SHUTDOWN_GENERATION_FAILED,
};

struct midr_sync_status {
	enum midr_sync_state state;
	uint32_t timeout_seconds;
	size_t initial_peer_count;
	size_t waiting_peer_count;
	size_t timed_out_peer_count;
	uint64_t barrier_count;
	uint64_t timeout_count;
	size_t active_session_count;
	size_t syncing_session_count;
	size_t eor_received_count;
	size_t eor_written_count;
	size_t receive_drained_count;
	size_t resync_required_count;
	size_t snapshot_active_count;
	size_t snapshot_completed_count;
	size_t shutdown_pending_count;
	bool shutdown_active;
	enum midr_sync_shutdown_state shutdown_state;
	size_t pending_update_count;
	size_t pending_input_count;
	uint64_t next_session_generation;
	uint64_t reconnect_count;
	uint64_t stale_event_count;
};

extern int midr_sync_init(struct midr_context *ctx);
extern void midr_sync_finish(struct midr_context *ctx);
extern bool midr_sync_view_ready(struct midr_context *ctx, bool local_ready,
				 uint64_t *reason_flags);
extern bool midr_sync_local_ready(const struct midr_context *ctx);
extern void midr_sync_peer_status_changed(struct midr_context *ctx, struct peer *peer);
extern void midr_sync_peer_eor(struct midr_context *ctx, struct peer *peer);
extern void midr_sync_peer_eor_connection(struct midr_context *ctx,
						  struct peer_connection *connection);
extern bool midr_sync_can_send_eor(struct midr_context *ctx,
						 struct peer_connection *connection);
extern void midr_sync_packet_queued(struct peer_connection *connection,
						 struct stream *stream, bool eor);
extern void midr_sync_packet_written(struct peer_connection *connection,
						 struct stream *stream);
extern void midr_sync_packet_dropped(struct peer_connection *connection,
						 struct stream *stream);
extern void midr_sync_snapshot_begin(struct peer_connection *connection);
extern void midr_sync_snapshot_end(struct peer_connection *connection);
extern bool midr_sync_snapshot_requested(struct peer_connection *connection);
extern void midr_sync_request_resync(struct peer_connection *connection);
extern void midr_sync_shutdown_begin(struct midr_context *ctx);
extern void midr_sync_shutdown_announce_complete(struct midr_context *ctx);
extern void midr_sync_shutdown_generation_failed(struct midr_context *ctx);
extern void midr_sync_shutdown_expired(struct midr_context *ctx);
extern bool midr_sync_shutdown_ready(struct midr_context *ctx);
extern bool midr_sync_shutdown_degraded(struct midr_context *ctx);
extern void midr_sync_shutdown_finish(struct midr_context *ctx);
extern void midr_sync_shutdown_cancel(struct midr_context *ctx);
extern int midr_sync_connection_generation(struct midr_context *ctx,
					    struct peer_connection *connection,
					    uint64_t *generation);
extern void midr_sync_input_begin(struct midr_context *ctx,
						 struct peer_connection *connection);
extern void midr_sync_input_end(struct midr_context *ctx,
			       struct peer_connection *connection, bool accepted);
extern void midr_sync_derivation_complete(struct midr_context *ctx);
extern void midr_sync_connection_down(struct midr_context *ctx,
						 struct peer_connection *connection);
extern void midr_sync_test_session_start(struct midr_context *ctx,
						 struct peer *peer,
						 struct peer_connection *connection);
extern void midr_sync_test_session_down(struct midr_context *ctx,
						 struct peer_connection *connection);
extern int midr_sync_timeout_set(struct midr_context *ctx, uint32_t seconds);
extern int midr_sync_status_get(struct midr_context *ctx, struct midr_sync_status *status);
extern void midr_sync_test_timeout(struct midr_context *ctx);
extern void midr_sync_test_drain_completions(struct midr_context *ctx);

#endif /* _FRR_BGP_MIDR_SYNC_H */
