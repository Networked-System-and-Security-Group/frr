// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Deterministic MIDR link and path cost helpers.
 */

#ifndef _FRR_BGP_MIDR_COST_H
#define _FRR_BGP_MIDR_COST_H

#include <zebra.h>

#include <stdbool.h>
#include <stdint.h>

#include "bgpd/bgp_midr_ls.h"

#define MIDR_REFERENCE_TRANSFER_BYTES 65536U
#define MIDR_COST_QUANTUM_US 100U
#define MIDR_LINK_COST_MAX (UINT32_MAX - 1U)
#define MIDR_PATH_COST_INFINITY UINT64_MAX

extern int midr_cost_ceil_div_u64(uint64_t dividend, uint64_t divisor, uint64_t *result);
extern int midr_cost_ceil_mul_div_u64(uint64_t multiplicand, uint64_t multiplier, uint64_t divisor,
				      uint64_t *result);
extern int midr_cost_from_metrics(const struct midr_ls_metrics *metrics, uint32_t *cost);
extern uint64_t midr_path_cost_add(uint64_t left, uint64_t right);
extern bool midr_cost_should_advertise(uint32_t advertised_cost, uint32_t candidate_cost,
				       bool first_publish, bool non_measurement_change);

#endif /* _FRR_BGP_MIDR_COST_H */
