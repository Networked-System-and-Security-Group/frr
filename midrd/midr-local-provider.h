/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Protocol-neutral first-group Local Fact snapshot contract. */
#ifndef MIDRD_LOCAL_PROVIDER_H
#define MIDRD_LOCAL_PROVIDER_H

#include <stdint.h>

#include "midr-core.h"

enum midr_local_event_kind {
	MIDR_LOCAL_SNAPSHOT_BEGIN = 1,
	MIDR_LOCAL_MEMBERSHIP = 2,
	MIDR_LOCAL_LINK = 3,
	MIDR_LOCAL_SNAPSHOT_END = 4,
	MIDR_LOCAL_EOR = 5,
};

struct midr_local_membership {
	uint32_t group;
	uint64_t version;
};

struct midr_local_link {
	uint32_t remote_node_id;
	uint8_t family;
	uint8_t reserved[3];
	uint64_t link_id;
	uint64_t version;
	uint32_t rtt_us;
	uint32_t loss_ppm;
	uint32_t available_bandwidth_kbps;
	uint64_t measurement_sequence;
	uint64_t measurement_timestamp_ms;
	uint8_t local_address[MIDR_CORE_ADDR_BYTES];
	uint8_t remote_address[MIDR_CORE_ADDR_BYTES];
};

struct midr_local_event {
	enum midr_local_event_kind kind;
	uint64_t generation;
	uint32_t originator;
	union {
		struct midr_local_membership membership;
		struct midr_local_link link;
	} fact;
};

int midr_local_event_validate(const struct midr_local_event *event);

#endif /* MIDRD_LOCAL_PROVIDER_H */
