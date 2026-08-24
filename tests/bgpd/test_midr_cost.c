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

static struct midr_link_metrics valid_metrics(void)
{
	return (struct midr_link_metrics){
		.has_rtt_us = true,
		.rtt_us = 1000,
		.has_loss_ppm = true,
		.loss_ppm = 0,
		.has_available_bandwidth_kbps = true,
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
	struct midr_link_metrics metrics = valid_metrics();
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
	struct midr_link_metrics metrics = valid_metrics();
	uint32_t cost = 123;

	assert(midr_cost_from_metrics(NULL, &cost) == -EINVAL);
	assert(midr_cost_from_metrics(&metrics, NULL) == -EINVAL);
	metrics.has_loss_ppm = false;
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

static void test_cost_deadband(void)
{
	assert(!midr_cost_change_significant(100, 100));
	assert(!midr_cost_change_significant(100, 104));
	assert(midr_cost_change_significant(100, 105));
	assert(midr_cost_change_significant(100, 95));

	assert(!midr_cost_change_significant(101, 106));
	assert(midr_cost_change_significant(101, 107));
	assert(!midr_cost_change_significant(1, 1));
	assert(midr_cost_change_significant(1, 2));

	assert(!midr_cost_change_significant(MIDR_LINK_COST_MAX,
					     MIDR_LINK_COST_MAX - 214748364U));
	assert(midr_cost_change_significant(MIDR_LINK_COST_MAX,
					    MIDR_LINK_COST_MAX - 214748365U));
}

static void test_saturating_path_cost(void)
{
	assert(midr_path_cost_add(10, 20) == 30);
	assert(midr_path_cost_add(UINT64_MAX, 1) == UINT64_MAX);
	assert(midr_path_cost_add(1, UINT64_MAX) == UINT64_MAX);
	assert(midr_path_cost_add(UINT64_MAX - 1, 2) == UINT64_MAX);
	assert(midr_path_cost_add(UINT64_MAX - 1, 1) == UINT64_MAX);
}

int main(void)
{
	test_integer_helpers();
	test_reference_cost();
	test_invalid_metrics();
	test_cost_deadband();
	test_saturating_path_cost();
	printf("MIDR cost tests passed\n");
	return 0;
}
