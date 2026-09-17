/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "midr-consumer.h"
#include "midr-prefix-provider.h"
#include "midr-transport.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

_Static_assert(MIDR_CORE_ADDR_BYTES == 16U, "wire address width");
_Static_assert(sizeof(((struct midr_transport_endpoint *)0)->address) ==
	       MIDR_CORE_ADDR_BYTES, "endpoint address width");
_Static_assert(sizeof(((struct midr_core_identity *)0)->prefix) ==
	       MIDR_CORE_ADDR_BYTES, "identity prefix width");

static int noop_prefix(void *arg, const struct midr_prefix_event *event)
{
	(void)arg;
	(void)event;
	return 0;
}

static int noop_frame(void *arg,
		      const struct midr_transport_endpoint *peer,
		      const struct midr_transport_frame *frame)
{
	(void)arg;
	(void)peer;
	(void)frame;
	return 0;
}

int main(void)
{
	struct midr_core_identity identity = {
		.type = MIDR_CORE_NODE_PREFIX,
		.family = MIDR_CORE_AF_IPV6,
		.prefix_len = 64,
		.originator = 101,
	};
	struct midr_core_object object = {
		.identity = identity,
		.state = MIDR_CORE_ACTIVE,
		.sequence = 1,
	};
	struct midr_transport_endpoint endpoint = {
		.family = MIDR_TRANSPORT_AF_IPV6,
		.port = 5859,
		.scope_id = 0,
	};
	struct midr_transport_callbacks transport_cb = {
		.on_frame = noop_frame,
	};
	struct midr_prefix_provider_config provider_cfg = {
		.originator = 101,
		.generation = 1,
		.on_event = noop_prefix,
	};
	struct midr_prefix_event event = {
		.kind = MIDR_PREFIX_UPSERT,
		.generation = 1,
		.originator = 101,
		.prefix = {
			.family = MIDR_CORE_AF_IPV4,
			.prefix_len = 24,
			.metric = 10,
		},
	};
	struct midr_consumer_event consumer_event = {
		.kind = MIDR_CONSUMER_NODE_PREFIX,
		.generation = 1,
		.originator = 101,
		.family = MIDR_CORE_AF_IPV6,
		.prefix_len = 64,
	};
	struct midr_core_config core_cfg = {
		.max_objects = 32,
		.lifetime_ms = 3600000,
	};

	memset(endpoint.address, 0x20, sizeof(endpoint.address));
	memset(identity.prefix, 0x20, sizeof(identity.prefix));
	memset(event.prefix.address, 0x0a, sizeof(event.prefix.address));
	assert(object.identity.family == MIDR_CORE_AF_IPV6);
	assert(endpoint.family == MIDR_TRANSPORT_AF_IPV6);
	assert(provider_cfg.on_event == noop_prefix);
	assert(transport_cb.on_frame == noop_frame);
	assert(event.prefix.family == MIDR_CORE_AF_IPV4);
	assert(consumer_event.family == MIDR_CORE_AF_IPV6);
	assert(core_cfg.max_objects != 0);
	puts("midrd R6-A contract: PASS");
	return 0;
}
