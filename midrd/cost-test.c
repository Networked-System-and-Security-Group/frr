/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "midr-cost.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>

int main(void)
{
	struct midr_cost_metrics metrics = {
		.rtt_us = 1000,
		.loss_ppm = 0,
		.available_bandwidth_kbps = 1000000,
	};
	uint64_t value;
	uint32_t cost;

	assert(midr_cost_ceil_div_u64(10, 3, &value) == 0 && value == 4);
	assert(midr_cost_ceil_mul_div_u64(10, 10, 6, &value) == 0 &&
	       value == 17);
	assert(midr_cost_ceil_mul_div_u64(UINT64_MAX, 2, 1, &value) ==
	       -EOVERFLOW);
	assert(midr_cost_from_metrics(&metrics, &cost) == 0 && cost == 16);
	metrics.loss_ppm = 500000;
	assert(midr_cost_from_metrics(&metrics, &cost) == 0 && cost == 31);
	metrics.loss_ppm = 1000000;
	assert(midr_cost_from_metrics(&metrics, &cost) == -EINVAL);
	assert(!midr_cost_change_significant(100, 104));
	assert(midr_cost_change_significant(100, 105));
	assert(!midr_cost_change_significant(1, 1));
	assert(midr_cost_change_significant(1, 2));
	assert(midr_path_cost_add(UINT64_MAX - 1U, 2U) == UINT64_MAX);
	assert(midr_path_cost_add(10, 20) == 30);
	puts("midrd-cost-test: PASS");
	return 0;
}
