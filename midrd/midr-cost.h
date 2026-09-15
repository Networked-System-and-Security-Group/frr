/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MIDRD_COST_H
#define MIDRD_COST_H

#include <stdbool.h>
#include <stdint.h>

#define MIDR_REFERENCE_TRANSFER_BYTES 65536U
#define MIDR_COST_QUANTUM_US 100U
#define MIDR_LINK_COST_DEADBAND_PERCENT 5U
#define MIDR_LINK_COST_MIN_ADVERTISEMENT_INTERVAL_MS 300000U
#define MIDR_LINK_COST_MAX (UINT32_MAX - 1U)
#define MIDR_PATH_COST_INFINITY UINT64_MAX

struct midr_cost_metrics {
	uint32_t rtt_us;
	uint32_t loss_ppm;
	uint32_t available_bandwidth_kbps;
};

int midr_cost_ceil_div_u64(uint64_t dividend, uint64_t divisor,
			   uint64_t *result);
int midr_cost_ceil_mul_div_u64(uint64_t multiplicand, uint64_t multiplier,
			       uint64_t divisor, uint64_t *result);
int midr_cost_from_metrics(const struct midr_cost_metrics *metrics,
			   uint32_t *cost);
bool midr_cost_change_significant(uint32_t advertised_cost,
				  uint32_t candidate_cost);
uint64_t midr_path_cost_add(uint64_t left, uint64_t right);

#endif /* MIDRD_COST_H */
