// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR reference-transfer cost tests.
 */

#include <zebra.h>

#include <errno.h>

#include "privs.h"

#include "bgpd/bgp_midr_cost.h"

/* Required variables when linking against bgpd/libbgp.a. */
struct zebra_privs_t bgpd_privs = {};
struct event_loop *master;

static struct midr_ls_metrics valid_metrics(void)
{
	return (struct midr_ls_metrics){
		.present_flags = MIDR_METRIC_REQUIRED_MASK,
		.rtt_us = 1000,
		.loss_ppm = 0,
		.available_bandwidth_kbps = 1000000,
	};
}

static void test_integer_helpers(void)
{
	uint64_t result = 99;

	assert(midr_cost_ceil_div_u64(0, 1, &result) == 0);
	assert(result == 0);
	assert(midr_cost_ceil_div_u64(10, 3, &result) == 0);
	assert(result == 4);
	assert(midr_cost_ceil_div_u64(UINT64_MAX, UINT64_MAX, &result) == 0);
	assert(result == 1);
	assert(midr_cost_ceil_div_u64(1, 0, &result) == -EINVAL);
	assert(midr_cost_ceil_div_u64(1, 1, NULL) == -EINVAL);

	assert(midr_cost_ceil_mul_div_u64(10, 10, 6, &result) == 0);
	assert(result == 17);
	assert(midr_cost_ceil_mul_div_u64(0, UINT64_MAX, 7, &result) == 0);
	assert(result == 0);
	assert(midr_cost_ceil_mul_div_u64(UINT64_MAX, 2, 1, &result) == -EOVERFLOW);
	assert(midr_cost_ceil_mul_div_u64(1, 1, 0, &result) == -EINVAL);
	assert(midr_cost_ceil_mul_div_u64(1, 1, 1, NULL) == -EINVAL);
}

static void test_reference_cost(void)
{
	struct midr_ls_metrics metrics = valid_metrics();
	uint32_t cost = 0;

	assert(midr_cost_from_metrics(&metrics, &cost) == 0);
	assert(cost == 16);

	metrics.loss_ppm = 100000;
	assert(midr_cost_from_metrics(&metrics, &cost) == 0);
	assert(cost == 17);

	metrics = valid_metrics();
	metrics.rtt_us = 1;
	metrics.available_bandwidth_kbps = UINT32_MAX;
	assert(midr_cost_from_metrics(&metrics, &cost) == 0);
	assert(cost == 1);

	metrics.rtt_us = UINT32_MAX;
	metrics.available_bandwidth_kbps = 1;
	metrics.loss_ppm = 999999;
	assert(midr_cost_from_metrics(&metrics, &cost) == 0);
	assert(cost == MIDR_LINK_COST_MAX);
}

static void test_invalid_metrics(void)
{
	struct midr_ls_metrics metrics = valid_metrics();
	uint32_t cost = 123;

	assert(midr_cost_from_metrics(NULL, &cost) == -EINVAL);
	assert(midr_cost_from_metrics(&metrics, NULL) == -EINVAL);
	metrics.present_flags &= ~MIDR_METRIC_PRESENT_LOSS;
	assert(midr_cost_from_metrics(&metrics, &cost) == -EINVAL);
	metrics = valid_metrics();
	metrics.present_flags |= 0x8;
	assert(midr_cost_from_metrics(&metrics, &cost) == -EINVAL);
	metrics = valid_metrics();
	metrics.rtt_us = 0;
	assert(midr_cost_from_metrics(&metrics, &cost) == -EINVAL);
	metrics = valid_metrics();
	metrics.available_bandwidth_kbps = 0;
	assert(midr_cost_from_metrics(&metrics, &cost) == -EINVAL);
	metrics = valid_metrics();
	metrics.loss_ppm = 1000000;
	assert(midr_cost_from_metrics(&metrics, &cost) == -EINVAL);
	assert(cost == 123);
}

static void test_saturating_path_cost(void)
{
	assert(midr_path_cost_add(10, 20) == 30);
	assert(midr_path_cost_add(UINT64_MAX, 1) == UINT64_MAX);
	assert(midr_path_cost_add(1, UINT64_MAX) == UINT64_MAX);
	assert(midr_path_cost_add(UINT64_MAX - 1, 2) == UINT64_MAX);
	assert(midr_path_cost_add(UINT64_MAX - 1, 1) == UINT64_MAX);
}

static void test_update_threshold(void)
{
	assert(midr_cost_should_advertise(100, 100, true, false));
	assert(midr_cost_should_advertise(100, 100, false, true));
	assert(!midr_cost_should_advertise(100, 104, false, false));
	assert(midr_cost_should_advertise(100, 105, false, false));
	assert(!midr_cost_should_advertise(100, 96, false, false));
	assert(midr_cost_should_advertise(100, 95, false, false));
	assert(!midr_cost_should_advertise(1, 1, false, false));
	assert(midr_cost_should_advertise(1, 2, false, false));
	assert(!midr_cost_should_advertise(UINT32_MAX, UINT32_MAX - 1, false, false));
	assert(midr_cost_should_advertise(UINT32_MAX, 0, false, false));
}

int main(void)
{
	test_integer_helpers();
	test_reference_cost();
	test_invalid_metrics();
	test_saturating_path_cost();
	test_update_threshold();
	printf("MIDR cost tests passed\n");
	return 0;
}
