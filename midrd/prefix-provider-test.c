/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "midr-prefix-provider.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct event_log {
	struct midr_prefix_event events[8];
	size_t count;
};

static int record_event(void *arg, const struct midr_prefix_event *event)
{
	struct event_log *log = arg;

	assert(log->count < sizeof(log->events) / sizeof(log->events[0]));
	log->events[log->count++] = *event;
	return 0;
}

int main(void)
{
	struct event_log log = {0};
	struct midr_prefix_provider *provider = NULL;
	struct midr_prefix_provider_config config = {
		.originator = 77,
		.on_event = record_event,
		.arg = &log,
	};
	struct midr_prefix prefix = {
		.family = MIDR_CORE_AF_IPV4,
		.prefix_len = 24,
		.address = {192, 0, 2},
		.metric = 10,
	};
	struct midr_prefix invalid = prefix;

	assert(midr_prefix_provider_create(&config, &provider) == 0);
	assert(midr_prefix_provider_upsert(provider, &prefix) == 0);
	assert(log.count == 1 && log.events[0].kind == MIDR_PREFIX_UPSERT);
	assert(log.events[0].generation == 1);
	assert(midr_prefix_provider_snapshot(provider) == 0);
	assert(log.count == 5);
	assert(log.events[1].kind == MIDR_PREFIX_SNAPSHOT_BEGIN);
	assert(log.events[2].kind == MIDR_PREFIX_UPSERT);
	assert(log.events[3].kind == MIDR_PREFIX_SNAPSHOT_END);
	assert(log.events[4].kind == MIDR_PREFIX_EOR);
	invalid.address[3] = 1;
	assert(midr_prefix_provider_upsert(provider, &invalid) == -EINVAL);
	assert(midr_prefix_provider_withdraw(provider, &prefix) == 0);
	assert(log.count == 6 && log.events[5].kind == MIDR_PREFIX_WITHDRAW);
	midr_prefix_provider_destroy(&provider);
	assert(provider == NULL);
	puts("midr-prefix-provider-test: PASS");
	return 0;
}
