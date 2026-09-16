/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L

#include "midr-core.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define MIDRD_SCALE_MAX_OBJECTS 4096U
#define MIDRD_SCALE_LIFETIME_MS 60000U

static size_t env_size(const char *name, size_t fallback, size_t maximum)
{
	const char *text = getenv(name);
	char *end = NULL;
	unsigned long long value;

	if (!text || !*text)
		return fallback;
	errno = 0;
	value = strtoull(text, &end, 10);
	if (errno || !end || *end || !value || value > maximum)
		return fallback;
	return (size_t)value;
}

static uint64_t monotonic_ns(void)
{
	struct timespec now;

	assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
	return (uint64_t)now.tv_sec * 1000000000U + (uint64_t)now.tv_nsec;
}

static uint64_t resident_bytes(void)
{
#ifdef __linux__
	FILE *stream;
	unsigned long total_pages = 0;
	unsigned long pages = 0;
	long page_size;

	stream = fopen("/proc/self/statm", "r");
	if (!stream)
		return 0;
	if (fscanf(stream, "%lu %lu", &total_pages, &pages) != 2)
		pages = 0;
	fclose(stream);
	(void)total_pages;
	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0 || pages > UINT64_MAX / (uint64_t)page_size)
		return 0;
	return (uint64_t)pages * (uint64_t)page_size;
#else
	return 0;
#endif
}

static struct midr_core_object prefix_object(size_t index, uint64_t sequence)
{
	uint32_t address = (uint32_t)index + 1U;
	struct midr_core_object object = {
		.identity = {
			.type = MIDR_CORE_NODE_PREFIX,
			.family = MIDR_CORE_AF_IPV4,
			.prefix_len = 32,
			.originator = 1,
		},
		.state = MIDR_CORE_ACTIVE,
		.sequence = sequence,
		.lifetime_ms = MIDRD_SCALE_LIFETIME_MS,
	};

	object.identity.prefix[0] = 10;
	object.identity.prefix[1] = (uint8_t)(address >> 16);
	object.identity.prefix[2] = (uint8_t)(address >> 8);
	object.identity.prefix[3] = (uint8_t)address;
	return object;
}

static size_t drain_events(struct midr_core *core)
{
	struct midr_core_object event;
	size_t count = 0;

	while (midr_core_event_next(core, &event) == 0)
		count++;
	return count;
}

static void emit_row(const char *phase, size_t objects, size_t cycle,
		     const struct midr_core *core, size_t floors,
		     size_t events, uint64_t elapsed_ns)
{
	printf("SCALE\t%s\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%" PRIu64
	       "\t%.6f\n",
	       phase, objects, cycle, midr_core_count(core),
	       midr_core_identity_count(core), floors, events, resident_bytes(),
	       (double)elapsed_ns / 1000000000.0);
}

int main(void)
{
	size_t objects = env_size("MIDRD_SCALE_OBJECTS", 512,
				  MIDRD_SCALE_MAX_OBJECTS);
	size_t cycles = env_size("MIDRD_SCALE_CYCLES", 2, 16);
	struct midr_core_config config = {
		.max_objects = objects,
		.lifetime_ms = MIDRD_SCALE_LIFETIME_MS,
	};
	struct midr_core_object *snapshot;
	struct midr_core *core = NULL;

	snapshot = calloc(objects, sizeof(*snapshot));
	assert(snapshot);
	assert(midr_core_create(&config, &core) == 0);
	puts("SCALE\tphase\tobjects\tcycle\tcanonical\tidentities\tfloors"
	     "\tevents\trss_bytes\tseconds");
	for (size_t cycle = 0; cycle < cycles; cycle++) {
		uint64_t start_ms = 1U + cycle * 2U * MIDRD_SCALE_LIFETIME_MS;
		uint64_t started_ns = monotonic_ns();
		size_t count = 0, expired = 0, collected = 0;

		for (size_t i = 0; i < objects; i++) {
			struct midr_core_object object =
				prefix_object(i, cycle + 1U);
			enum midr_core_result result;

			assert(midr_core_upsert(core, &object, start_ms, &result) == 0);
			assert(result == MIDR_CORE_ACCEPTED);
		}
		assert(midr_core_snapshot(core, start_ms, snapshot, objects,
					  &count) == 0);
		assert(count == objects && midr_core_count(core) == objects);
		assert(midr_core_identity_count(core) == objects);
		{
			struct midr_core_object overflow =
				prefix_object(objects, cycle + 1U);
			enum midr_core_result result;

			assert(midr_core_upsert(core, &overflow, start_ms, &result) ==
			       -ENOSPC);
		}
		count = drain_events(core);
		assert(count == objects);
		emit_row("fill", objects, cycle, core, 0, count,
			 monotonic_ns() - started_ns);

		started_ns = monotonic_ns();
		assert(midr_core_expire(core,
					start_ms + MIDRD_SCALE_LIFETIME_MS,
					&expired) == 0);
		assert(expired == objects && midr_core_count(core) == 0);
		assert(midr_core_identity_count(core) == objects);
		emit_row("expired", objects, cycle, core, objects, 0,
			 monotonic_ns() - started_ns);

		started_ns = monotonic_ns();
		assert(midr_core_gc_enable(core, true) == 0);
		assert(midr_core_gc(core,
				    start_ms + MIDRD_SCALE_LIFETIME_MS,
				    objects, &collected, NULL, NULL) == 0);
		assert(collected == objects && midr_core_count(core) == 0);
		assert(midr_core_identity_count(core) == 0);
		emit_row("reclaimed", objects, cycle, core, 0, 0,
			 monotonic_ns() - started_ns);
	}
	midr_core_destroy(&core);
	free(snapshot);
	puts("midrd-scale-test: PASS");
	return 0;
}
