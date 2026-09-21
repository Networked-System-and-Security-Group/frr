/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MIDRD_DP_BACKEND_H
#define MIDRD_DP_BACKEND_H

/*
 * MIDR data-plane backend: the third-group implementation of
 * struct midr_zebra_backend_ops.
 *
 * The backend owns the midrd zclient, the VRF, the pending batch, the
 * installed-route hash and the 100 ms deferred timer, and translates the
 * technology-neutral midr_path_result produced by the SPF adapter into
 * ZAPI route messages for zebra.
 *
 * See doc/midr-doc/MIDR-TED路径计算接口规范.md for the contract and
 * doc/midr-doc/dp-doc/ for the migration notes.
 *
 * Deferred-batch failure semantics (frozen contract section 6, review 2.2):
 * update_deferred() reports success as soon as the batch is queued, so a
 * failure detected later by the batch timer happens after the SPF adapter has
 * already advanced its desired generation.  The backend therefore never drops
 * the un-applied queue remainder in that case: those operations are the only
 * replay of the missing installs and are retained (and re-driven) until they
 * are accepted, after which midr_spf_install_resync() reconciles.  Dropping
 * them would leave the FIB permanently behind the adapter's desired state.
 * abort_pending() still discards the operations staged by the round the
 * adapter just rejected; retained recovery operations from an earlier,
 * already-committed round are preserved.
 *
 * Recovery liveness (review 2.3): the retained batch is driven by an explicit
 * state machine (active fast retries, then a slow heartbeat) and is re-armed by
 * a connection-state change or fresh adapter work as well, so a socket that
 * stays up while sends keep failing can no longer stall recovery forever.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vrf.h"

struct event_loop;
struct midr_context;
struct midr_dp_backend;
struct zclient;

/* Runtime diagnostics for operators and integration gates. */
struct midr_dp_status {
	bool zebra_up;
	/* Next batch bypasses the 100 ms window (cold path: start/reconnect). */
	bool immediate_next;
	uint64_t add_staged;
	uint64_t del_staged;
	uint64_t batches;
	uint64_t route_adds;
	uint64_t route_deletes;
	uint64_t send_failures;
	uint64_t replays;
	uint64_t resyncs;
	/* Recovery state machine (see midr-dp-backend.c): recovering is true
	 * while a retained deferred batch is being driven to convergence, and
	 * recovery_stalled is true once it fell back to the slow heartbeat. */
	bool recovering;
	bool recovery_stalled;
	uint32_t recovery_attempts;
	size_t pending;
	size_t installed;
	int last_error;
};

/*
 * Create the backend, open the midrd zclient and register the ops table with
 * the SPF installation adapter.  Registration subscribes to the committed TED
 * and immediately reconciles the current READY result, if any.
 *
 * @master is the midrd event loop (owned by the caller) and @vrf_id is the VRF
 * the installed routes belong to.
 */
int midr_dp_backend_start(struct midr_context *ctx, struct event_loop *master,
			  vrf_id_t vrf_id);

/*
 * Stop the backend in contract order: cancel new work, unregister the ops
 * table (which withdraws the accepted SPF routes and flushes immediately),
 * then destroy the zclient and the private state.
 */
void midr_dp_backend_stop(void);

/* NULL until midr_dp_backend_start() succeeded and after stop. */
struct midr_dp_backend *midr_dp_backend_get(void);

/* Shared zclient; also used by the GRE provisioning module. */
struct zclient *midr_dp_backend_zclient(void);
vrf_id_t midr_dp_backend_vrf_id(void);
bool midr_dp_backend_ready(void);

void midr_dp_backend_status_get(struct midr_dp_status *status);

/* One-line runtime summary for the midrd log (shutdown/diagnostic gates). */
void midr_dp_backend_log_status(const char *tag);

#endif /* MIDRD_DP_BACKEND_H */
