/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MIDRD_SPF_H
#define MIDRD_SPF_H

#include <stddef.h>
#include <stdint.h>

#include "midr-consumer.h"

struct midr_spf_route {
	uint8_t family;
	uint8_t prefix_len;
	uint8_t reserved[2];
	uint8_t prefix[MIDR_CORE_ADDR_BYTES];
	uint32_t originator;
	uint32_t metric;
	uint64_t generation;
};

int midr_spf_compute(const struct midr_consumer_snapshot *snapshot,
			    struct midr_spf_route *routes, size_t capacity,
			    size_t *count);

#endif /* MIDRD_SPF_H */
