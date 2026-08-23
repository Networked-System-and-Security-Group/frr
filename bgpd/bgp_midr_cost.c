// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Deterministic MIDR link and path cost helpers.
 */

#include <zebra.h>

#include <errno.h>

#include "bgpd/bgp_midr_cost.h"

_Static_assert(MIDR_REFERENCE_TRANSFER_BYTES > 0, "reference transfer size must be non-zero");
_Static_assert(MIDR_COST_QUANTUM_US > 0, "cost quantum must be non-zero");

int midr_cost_ceil_div_u64(uint64_t dividend, uint64_t divisor, uint64_t *result)
{
	if (!divisor || !result)
		return -EINVAL;

	*result = dividend / divisor + (dividend % divisor != 0);
	return 0;
}

int midr_cost_ceil_mul_div_u64(uint64_t multiplicand, uint64_t multiplier, uint64_t divisor,
			       uint64_t *result)
{
	uint64_t product;

	if (!divisor || !result)
		return -EINVAL;
	if (multiplicand && multiplier > UINT64_MAX / multiplicand)
		return -EOVERFLOW;

	product = multiplicand * multiplier;
	return midr_cost_ceil_div_u64(product, divisor, result);
}

int midr_cost_from_metrics(const struct midr_ls_metrics *metrics, uint32_t *cost)
{
	uint64_t reference_bits;
	uint64_t serialization_us;
	uint64_t base_time_us;
	uint64_t effective_time_us;
	uint64_t cost_units;
	uint32_t computed_cost;
	int result;

	if (!metrics || !cost || metrics->present_flags != MIDR_METRIC_REQUIRED_MASK ||
	    !metrics->rtt_us || !metrics->available_bandwidth_kbps || metrics->loss_ppm >= 1000000U)
		return -EINVAL;

	reference_bits = (uint64_t)MIDR_REFERENCE_TRANSFER_BYTES * 8;
	result = midr_cost_ceil_mul_div_u64(reference_bits, 1000,
					    metrics->available_bandwidth_kbps, &serialization_us);
	if (result)
		return result;
	if (serialization_us > UINT64_MAX - metrics->rtt_us)
		return -EOVERFLOW;
	base_time_us = serialization_us + metrics->rtt_us;

	result = midr_cost_ceil_mul_div_u64(base_time_us, 1000000, 1000000 - metrics->loss_ppm,
					    &effective_time_us);
	if (result)
		return result;
	result = midr_cost_ceil_div_u64(effective_time_us, MIDR_COST_QUANTUM_US, &cost_units);
	if (result)
		return result;

	if (cost_units < 1)
		computed_cost = 1;
	else if (cost_units > MIDR_LINK_COST_MAX)
		computed_cost = MIDR_LINK_COST_MAX;
	else
		computed_cost = cost_units;

	*cost = computed_cost;
	return 0;
}

uint64_t midr_path_cost_add(uint64_t left, uint64_t right)
{
	if (left == MIDR_PATH_COST_INFINITY || right == MIDR_PATH_COST_INFINITY ||
	    left > UINT64_MAX - right)
		return MIDR_PATH_COST_INFINITY;
	return left + right;
}

bool midr_cost_should_advertise(uint32_t advertised_cost, uint32_t candidate_cost,
				bool first_publish, bool non_measurement_change)
{
	uint64_t threshold;
	uint32_t delta;

	if (first_publish || non_measurement_change)
		return true;

	delta = advertised_cost > candidate_cost ? advertised_cost - candidate_cost
						 : candidate_cost - advertised_cost;
	threshold = ((uint64_t)advertised_cost * 5) / 100;
	if (((uint64_t)advertised_cost * 5) % 100)
		threshold++;
	if (threshold < 1)
		threshold = 1;
	return delta >= threshold;
}
