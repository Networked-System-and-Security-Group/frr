// SPDX-License-Identifier: GPL-2.0-or-later
/* Scheduler lifecycle/cache tests using a manually completed engine. */
#include <zebra.h>
#include "privs.h"
#include "bgpd/midr_ip2asn.h"
#include "bgpd/midr_tier1_list.h"
#include "bgpd/midr_trace_engine.h"
#include "bgpd/midr_trace_scheduler.h"

struct zebra_privs_t bgpd_privs = {};
struct event_loop *master;
struct midr_trace_engine {
	midr_trace_engine_done_cb done;
	void *arg;
	struct midr_trace_job_result result;
};
static struct midr_trace_engine fake;
static unsigned int starts, destroys, deliveries, canceled;
static bool finish_in_callback;
static struct midr_trace_net_context captured_context;

bool __wrap_midr_trace_engine_supported(int family);
int __wrap_midr_trace_engine_start(struct event_loop *loop, const struct prefix *target,
	const struct midr_trace_net_context *context,
	midr_trace_engine_done_cb done, void *arg, struct midr_trace_engine **out);
void __wrap_midr_trace_engine_destroy(struct midr_trace_engine **engine);
void __wrap_midr_trace_engine_snapshot(const struct midr_trace_engine *engine,
				     struct midr_trace_job_result *result);

bool __wrap_midr_trace_engine_supported(int family)
{
	return family == AF_INET || family == AF_INET6;
}

int __wrap_midr_trace_engine_start(struct event_loop *loop, const struct prefix *target,
	const struct midr_trace_net_context *context,
	midr_trace_engine_done_cb done, void *arg, struct midr_trace_engine **out)
{
	(void)loop;
	captured_context = *context;
	memset(&fake, 0, sizeof(fake));
	fake.done = done;
	fake.arg = arg;
	fake.result.target = *target;
	*out = &fake;
	starts++;
	return 0;
}

void __wrap_midr_trace_engine_destroy(struct midr_trace_engine **engine)
{
	if (*engine) {
		destroys++;
		*engine = NULL;
	}
}

void __wrap_midr_trace_engine_snapshot(const struct midr_trace_engine *engine,
				     struct midr_trace_job_result *result)
{
	*result = engine->result;
}

static void received(const struct midr_trace_delivery *delivery, void *arg)
{
	(void)arg;
	deliveries++;
	if (delivery->status == MIDR_TRACE_ERR_CANCELED)
		canceled++;
	if (finish_in_callback)
		midr_trace_scheduler_fini();
}

static void tick(void)
{
	struct event event;

	assert(event_fetch(master, &event));
	event_call(&event);
}

static void load_data(void)
{
	char file[] = "/tmp/midr-scheduler-XXXXXX", error[256];
	int fd = mkstemp(file);
	const char contents[] = "192.0.2.0/24 174\n";

	assert(fd >= 0);
	assert(write(fd, contents, sizeof(contents) - 1) == sizeof(contents) - 1);
	close(fd);
	assert(midr_ip2asn_load_file(file, error, sizeof(error)) == 0);
	unlink(file);
}

static void watchdog(struct event *event)
{
	(void)event;
	assert(!"scheduler test timed out");
}

int main(void)
{
	struct prefix target;
	struct midr_trace_scheduler_stats stats;
	struct midr_trace_request_options options = {};
	struct midr_trace_query_view view;
	struct midr_trace_job_snapshot snapshot;
	enum midr_trace_ensure_state state;
	struct event *timeout = NULL;
	uint64_t a, b, job;
	unsigned int expected;

	master = event_master_create("MIDR scheduler test");
	load_data();
	assert(str2prefix("192.0.2.10", &target));
	assert(midr_trace_scheduler_init(master) == 0);
	event_add_timer(master, watchdog, NULL, 5, &timeout);
	assert(midr_trace_request_async(&target, &options, received, NULL, &a) == MIDR_TRACE_SUBMIT_ACCEPTED);
	assert(midr_trace_request_async(&target, &options, received, NULL, &b) == MIDR_TRACE_SUBMIT_ACCEPTED);
	assert(a != b && deliveries == 0);
	while (!starts)
		tick();
	assert(midr_trace_ensure_job(&target, &options, &job, &state) == MIDR_TRACE_SUBMIT_ACCEPTED);
	assert(state == MIDR_TRACE_ENSURE_RUNNING && starts == 1);
	assert(midr_trace_cancel(a));
	assert(!midr_trace_cancel(a));
	while (!canceled)
		tick();
	fake.result.status = MIDR_TRACE_OK;
	fake.result.target_reached = true;
	fake.result.raw_path.hop_count = 1;
	fake.result.raw_path.hops[0].visible = true;
	fake.result.raw_path.hops[0].address = target;
	fake.done(&fake.result, fake.arg);
	while (deliveries != 2)
		tick();
	assert(destroys == 1);
	midr_trace_scheduler_stats_get(&stats);
	assert(stats.active_slots == 0 && stats.completed == 1);
	assert(midr_trace_job_lookup(job, &snapshot) == MIDR_TRACE_QUERY_COMPLETED);
	assert(midr_trace_cache_lookup(&target, &options, &view, NULL) == MIDR_TRACE_LOOKUP_HIT);
	assert(view.observation.observed_asns[0] == 174);
	/* Identical destination, different source or instance: never reuse the
	 * diagnostic cache. Unsupported VRFs fail closed at submission. */
	{
		struct midr_trace_request_options scoped = {};
		assert(str2prefix("192.0.2.1", &scoped.context.source));
		assert(midr_trace_cache_lookup(&target, &scoped, &view, NULL) == MIDR_TRACE_LOOKUP_MISS);
		assert(midr_trace_request_async(&target, &scoped, received, NULL, &a) == MIDR_TRACE_SUBMIT_ACCEPTED);
		while (starts < 2)
			tick();
		assert(prefix_same(&captured_context.source, &scoped.context.source));
		expected = deliveries + 1;
		fake.result.status = MIDR_TRACE_OK;
		fake.done(&fake.result, fake.arg);
		while (deliveries < expected)
			tick();
		assert(midr_trace_cache_lookup(&target, &scoped, &view, NULL) == MIDR_TRACE_LOOKUP_HIT);
		scoped.context.instance_cookie = 123;
		assert(midr_trace_cache_lookup(&target, &scoped, &view, NULL) == MIDR_TRACE_LOOKUP_MISS);
		scoped.context.vrf_id = 1;
		assert(midr_trace_request_async(&target, &scoped, received, NULL, &a) == MIDR_TRACE_SUBMIT_UNSUPPORTED);
		scoped.context.vrf_id = 0;
		assert(str2prefix("2001:db8::1", &scoped.context.source));
		assert(midr_trace_request_async(&target, &scoped, received, NULL, &a) == MIDR_TRACE_SUBMIT_INVALID);
	}
	/* Cache delivery remains deferred, even when teardown is reentered. */
	expected = deliveries + 1;
	finish_in_callback = true;
	assert(midr_trace_request_async(&target, &options, received, NULL, &a) == MIDR_TRACE_SUBMIT_ACCEPTED);
	assert(deliveries + 1 == expected);
	while (deliveries != expected)
		tick();
	assert(!midr_trace_scheduler_is_ready());
	event_cancel(&timeout);
	midr_trace_scheduler_fini();
	midr_ip2asn_clear();
	event_master_free(master);
	puts("MIDR scheduler tests passed");
	return 0;
}
