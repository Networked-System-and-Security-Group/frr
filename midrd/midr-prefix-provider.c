/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "midr-prefix-provider.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define MIDR_PREFIX_PROVIDER_MAX 4096U

struct midr_prefix_provider {
	struct midr_prefix_provider_config config;
	struct midr_prefix prefixes[MIDR_PREFIX_PROVIDER_MAX];
	size_t count;
};

static unsigned int prefix_bytes(const struct midr_prefix *prefix)
{
	return prefix->family == MIDR_CORE_AF_IPV4 ? 4U : 16U;
}

static void normalize_prefix(struct midr_prefix *prefix)
{
	unsigned int bytes = prefix_bytes(prefix);
	unsigned int first_zero = (prefix->prefix_len + 7U) / 8U;
	unsigned int remainder = prefix->prefix_len % 8U;

	if (remainder && first_zero <= bytes)
		prefix->address[first_zero - 1U] &=
			(uint8_t)(0xffU << (8U - remainder));
	for (unsigned int i = first_zero; i < bytes; i++)
		prefix->address[i] = 0;
	for (unsigned int i = bytes; i < MIDR_CORE_ADDR_BYTES; i++)
		prefix->address[i] = 0;
}

int midr_prefix_validate(const struct midr_prefix *prefix)
{
	struct midr_prefix normalized;

	if (!prefix || (prefix->family != MIDR_CORE_AF_IPV4 &&
		       prefix->family != MIDR_CORE_AF_IPV6))
		return -EINVAL;
	if ((prefix->family == MIDR_CORE_AF_IPV4 && prefix->prefix_len > 32U) ||
	    (prefix->family == MIDR_CORE_AF_IPV6 && prefix->prefix_len > 128U))
		return -EINVAL;
	normalized = *prefix;
	normalize_prefix(&normalized);
	return memcmp(&normalized, prefix, sizeof(normalized)) ? -EINVAL : 0;
}

int midr_prefix_event_validate(const struct midr_prefix_event *event)
{
	if (!event || !event->originator || !event->generation)
		return -EINVAL;
	if (event->kind == MIDR_PREFIX_SNAPSHOT_BEGIN ||
	    event->kind == MIDR_PREFIX_SNAPSHOT_END ||
	    event->kind == MIDR_PREFIX_EOR)
		return 0;
	if (event->kind != MIDR_PREFIX_UPSERT &&
	    event->kind != MIDR_PREFIX_WITHDRAW)
		return -EINVAL;
	return midr_prefix_validate(&event->prefix);
}

static int emit(struct midr_prefix_provider *provider,
		 enum midr_prefix_event_kind kind,
		 const struct midr_prefix *prefix)
{
	struct midr_prefix_event event = {0};

	event.kind = kind;
	event.generation = provider->config.generation;
	event.originator = provider->config.originator;
	if (prefix)
		event.prefix = *prefix;
	if (midr_prefix_event_validate(&event))
		return -EINVAL;
	return provider->config.on_event(provider->config.arg, &event);
}

static ssize_t find_prefix(const struct midr_prefix_provider *provider,
			   const struct midr_prefix *prefix)
{
	for (size_t i = 0; i < provider->count; i++)
		if (!memcmp(&provider->prefixes[i], prefix, sizeof(*prefix)))
			return (ssize_t)i;
	return -1;
}

int midr_prefix_provider_create(
	const struct midr_prefix_provider_config *config,
	struct midr_prefix_provider **out)
{
	struct midr_prefix_provider *provider;

	if (!config || !out || *out || !config->originator || !config->on_event)
		return -EINVAL;
	provider = calloc(1, sizeof(*provider));
	if (!provider)
		return -ENOMEM;
	provider->config = *config;
	*out = provider;
	return 0;
}

void midr_prefix_provider_destroy(struct midr_prefix_provider **providerp)
{
	if (!providerp || !*providerp)
		return;
	free(*providerp);
	*providerp = NULL;
}

int midr_prefix_provider_snapshot(struct midr_prefix_provider *provider)
{
	int ret;

	if (!provider)
		return -EINVAL;
	if (!provider->config.generation)
		provider->config.generation = 1;
	ret = emit(provider, MIDR_PREFIX_SNAPSHOT_BEGIN, NULL);
	if (ret)
		return ret;
	for (size_t i = 0; i < provider->count; i++) {
		ret = emit(provider, MIDR_PREFIX_UPSERT,
			   &provider->prefixes[i]);
		if (ret)
			return ret;
	}
	ret = emit(provider, MIDR_PREFIX_SNAPSHOT_END, NULL);
	if (ret)
		return ret;
	/* EOR is the commit boundary for consumers that stage a provider
	 * snapshot.  SNAPSHOT_END alone is only framing and must not publish a
	 * partial view after a reconnect. */
	return emit(provider, MIDR_PREFIX_EOR, NULL);
}

int midr_prefix_provider_upsert(struct midr_prefix_provider *provider,
				const struct midr_prefix *prefix)
{
	struct midr_prefix normalized;
	ssize_t index;
	int ret;

	if (!provider || !prefix || midr_prefix_validate(prefix))
		return -EINVAL;
	normalized = *prefix;
	normalize_prefix(&normalized);
	index = find_prefix(provider, &normalized);
	if (index < 0 && provider->count == MIDR_PREFIX_PROVIDER_MAX)
		return -ENOSPC;
	if (provider->config.generation == UINT64_MAX)
		return -ERANGE;
	provider->config.generation++;
	ret = emit(provider, MIDR_PREFIX_UPSERT, &normalized);
	if (ret)
		return ret;
	if (index >= 0)
		provider->prefixes[index] = normalized;
	else {
		if (provider->count == MIDR_PREFIX_PROVIDER_MAX)
			return -ENOSPC;
		provider->prefixes[provider->count++] = normalized;
	}
	return 0;
}

int midr_prefix_provider_withdraw(struct midr_prefix_provider *provider,
				  const struct midr_prefix *prefix)
{
	struct midr_prefix normalized;
	ssize_t index;
	int ret;

	if (!provider || !prefix || midr_prefix_validate(prefix))
		return -EINVAL;
	normalized = *prefix;
	normalize_prefix(&normalized);
	index = find_prefix(provider, &normalized);
	if (index < 0)
		return -ENOENT;
	if (provider->config.generation == UINT64_MAX)
		return -ERANGE;
	provider->config.generation++;
	ret = emit(provider, MIDR_PREFIX_WITHDRAW, &normalized);
	if (ret)
		return ret;
	provider->prefixes[index] = provider->prefixes[--provider->count];
	return 0;
}

uint64_t midr_prefix_provider_generation(
	const struct midr_prefix_provider *provider)
{
	return provider ? provider->config.generation : 0;
}
