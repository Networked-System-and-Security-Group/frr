// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _FRR_MIDR_TRACE_ENGINE_H
#define _FRR_MIDR_TRACE_ENGINE_H

#include "frrevent.h"
#include "bgpd/midr_trace_types.h"

#define MIDR_TRACE_MAX_TTL 30U
#define MIDR_TRACE_PROBE_WAIT_MSEC 1000U
#define MIDR_TRACE_SEND_INTERVAL_MSEC 10U

struct midr_trace_engine;
typedef void (*midr_trace_engine_done_cb)(
	const struct midr_trace_job_result *result, void *arg);

bool midr_trace_engine_supported(int family);
bool midr_trace_engine_target_valid(const struct prefix *target);
/* Main-event-thread only. Success schedules work; failure has no callback.
 * Result is borrowed for the callback. Do not destroy engine inside done;
 * defer owner finalization to an event. No list/database pointers are kept.
 */
int midr_trace_engine_start(struct event_loop *master,
	const struct prefix *target, midr_trace_engine_done_cb done, void *arg,
	struct midr_trace_engine **out);
void midr_trace_engine_snapshot(const struct midr_trace_engine *engine,
			       struct midr_trace_job_result *out);
/* Cancel ALL pending events, close fd and suppress completion; idempotent. */
void midr_trace_engine_destroy(struct midr_trace_engine **engine);

#endif
