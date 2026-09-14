/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Prefix source contract.  Providers publish neutral Prefix events; the core
 * does not know whether the source is static configuration, kernel state or a
 * later bgpd IPC adapter.
 */
#ifndef MIDRD_PREFIX_PROVIDER_H
#define MIDRD_PREFIX_PROVIDER_H

#include <stddef.h>
#include <stdint.h>

#include "midr-core.h"

enum midr_prefix_event_kind {
	MIDR_PREFIX_SNAPSHOT_BEGIN = 1,
	MIDR_PREFIX_UPSERT = 2,
	MIDR_PREFIX_WITHDRAW = 3,
	MIDR_PREFIX_SNAPSHOT_END = 4,
};

struct midr_prefix {
	uint8_t family;
	uint8_t prefix_len;
	uint8_t reserved[2];
	uint8_t address[MIDR_CORE_ADDR_BYTES];
	uint32_t metric;
};

struct midr_prefix_event {
	enum midr_prefix_event_kind kind;
	uint64_t generation;
	uint32_t originator;
	struct midr_prefix prefix;
};

typedef int (*midr_prefix_event_cb)(void *arg,
				    const struct midr_prefix_event *event);

struct midr_prefix_provider_config {
	uint32_t originator;
	uint64_t generation;
	midr_prefix_event_cb on_event;
	void *arg;
};

struct midr_prefix_provider;

int midr_prefix_validate(const struct midr_prefix *prefix);
int midr_prefix_event_validate(const struct midr_prefix_event *event);
int midr_prefix_provider_create(
	const struct midr_prefix_provider_config *config,
	struct midr_prefix_provider **out);
void midr_prefix_provider_destroy(struct midr_prefix_provider **provider);
int midr_prefix_provider_snapshot(struct midr_prefix_provider *provider);
int midr_prefix_provider_upsert(struct midr_prefix_provider *provider,
				const struct midr_prefix *prefix);
int midr_prefix_provider_withdraw(struct midr_prefix_provider *provider,
				  const struct midr_prefix *prefix);
uint64_t midr_prefix_provider_generation(
	const struct midr_prefix_provider *provider);

#endif /* MIDRD_PREFIX_PROVIDER_H */
