// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR Clustering (CL) module interface.
 *
 * Receives the global view from NDS (I-3) and produces clustering
 * recommendations/decisions back to NDS (I-7).  Skeleton stage: the
 * decision logic is stubbed; only the callback registration is wired.
 */

#ifndef _FRR_BGP_MIDR_CL_H
#define _FRR_BGP_MIDR_CL_H

#include "midrd/group1/midr_nds.h"

struct midr_g1;

/* Register the CL global-view callback on bgp->midr_nds_info. */
extern void midr_cl_register_callback(struct midr_g1 *g1, midr_global_view_cb cb);

/* Initialize the CL module for a BGP instance (registers its callback). */
extern void midr_cl_init(struct midr_g1 *g1);

#endif /* _FRR_BGP_MIDR_CL_H */
