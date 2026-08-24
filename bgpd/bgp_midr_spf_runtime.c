// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR SPF runtime and TED consumer integration.
 */

#include <zebra.h>

#include <errno.h>

#include "lib/frrevent.h"
#include "lib/memory.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_memory.h"
#include "bgpd/bgp_midr_private.h"
#include "bgpd/bgp_midr_spf.h"
#include "bgpd/bgp_midr_spf_install.h"

#define MIDR_SPF_DEBOUNCE_MSEC 50

DEFINE_MTYPE_STATIC(BGPD, MIDR_SPF_RUNTIME, "MIDR SPF runtime");

struct midr_spf_runtime {
	struct midr_context *ctx;
	struct midr_ted_consumer *consumer;
	struct event *t_recompute;
	const struct midr_spf_results *cached;
	uint64_t pending_generation;
	uint32_t pending_change_flags;
	uint64_t recompute_count;
	int last_error;
};

static void midr_spf_recompute_schedule(struct midr_spf_runtime *runtime, unsigned long delay_msec);

static void midr_spf_recompute_cb(struct event *event)
{
	const struct midr_spf_results *candidate = NULL;
	const struct midr_ted_snapshot *snapshot = NULL;
	const struct midr_spf_results *old;
	struct midr_spf_runtime *runtime = EVENT_ARG(event);
	int ret;

	runtime->t_recompute = NULL;
	ret = midr_ted_snapshot_get(runtime->ctx, &snapshot);
	if (ret == -EAGAIN) {
		runtime->last_error = 0;
		return;
	}
	if (ret) {
		runtime->last_error = ret;
		return;
	}

	ret = midr_spf_compute_all(snapshot, &candidate);
	if (ret) {
		runtime->last_error = ret;
		midr_ted_snapshot_release(&snapshot);
		return;
	}
	if (!midr_ted_generation_is_current(runtime->ctx, snapshot->generation)) {
		midr_spf_results_release(&candidate);
		midr_ted_snapshot_release(&snapshot);
		midr_spf_recompute_schedule(runtime, 0);
		return;
	}

	old = runtime->cached;
	runtime->cached = candidate;
	midr_spf_install_results(runtime->ctx, old, candidate);
	runtime->pending_generation = 0;
	runtime->pending_change_flags = MIDR_TED_CHANGE_NONE;
	runtime->recompute_count++;
	runtime->last_error = 0;
	midr_spf_results_release(&old);
	midr_ted_snapshot_release(&snapshot);
}

static void midr_spf_recompute_schedule(struct midr_spf_runtime *runtime, unsigned long delay_msec)
{
	if (!runtime || runtime->t_recompute)
		return;
	event_add_timer_msec(bm->master, midr_spf_recompute_cb, runtime, delay_msec,
			     &runtime->t_recompute);
}

static void midr_spf_snapshot_changed(struct midr_context *ctx, uint64_t generation,
				      uint32_t change_flags, void *arg)
{
	struct midr_spf_runtime *runtime = arg;

	if (!runtime || runtime->ctx != ctx)
		return;
	runtime->pending_generation = generation;
	runtime->pending_change_flags |= change_flags;
	midr_spf_recompute_schedule(runtime, MIDR_SPF_DEBOUNCE_MSEC);
}

int midr_spf_context_init(struct midr_context *ctx)
{
	static const struct midr_ted_consumer_ops consumer_ops = {
		.snapshot_changed = midr_spf_snapshot_changed,
	};
	struct midr_spf_runtime *runtime;
	int ret;

	if (!ctx || !ctx->ted_store)
		return -ENOENT;
	if (ctx->spf)
		return -EALREADY;

	runtime = XCALLOC(MTYPE_MIDR_SPF_RUNTIME, sizeof(*runtime));
	runtime->ctx = ctx;
	ret = midr_ted_consumer_register(ctx, &consumer_ops, runtime, &runtime->consumer);
	if (ret) {
		XFREE(MTYPE_MIDR_SPF_RUNTIME, runtime);
		return ret;
	}

	ctx->spf = runtime;
	midr_spf_recompute_schedule(runtime, 0);
	return 0;
}

void midr_spf_context_finish(struct midr_context *ctx)
{
	const struct midr_spf_results *cached;
	struct midr_spf_runtime *runtime;

	if (!ctx || !ctx->spf)
		return;
	runtime = ctx->spf;
	ctx->spf = NULL;

	event_cancel(&runtime->t_recompute);
	midr_ted_consumer_unregister(ctx, &runtime->consumer);
	cached = runtime->cached;
	midr_spf_install_results(ctx, cached, NULL);
	midr_spf_results_release(&cached);
	XFREE(MTYPE_MIDR_SPF_RUNTIME, runtime);
}

int midr_spf_results_get(struct midr_context *ctx, const struct midr_spf_results **out)
{
	if (!out || *out)
		return -EINVAL;
	if (!ctx || !ctx->spf)
		return -ENOENT;
	if (!ctx->spf->cached)
		return -EAGAIN;

	*out = midr_spf_results_acquire(ctx->spf->cached);
	return 0;
}

int midr_spf_runtime_status_get(struct midr_context *ctx, struct midr_spf_runtime_status *status)
{
	struct midr_spf_runtime *runtime;

	if (!status)
		return -EINVAL;
	memset(status, 0, sizeof(*status));
	if (!ctx || !ctx->spf)
		return -ENOENT;

	runtime = ctx->spf;
	status->cached_generation = midr_spf_results_generation(runtime->cached);
	status->pending_generation = runtime->pending_generation;
	status->pending_change_flags = runtime->pending_change_flags;
	status->recompute_count = runtime->recompute_count;
	status->last_error = runtime->last_error;
	status->recompute_pending = runtime->t_recompute != NULL;
	return 0;
}
