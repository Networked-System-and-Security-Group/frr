// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR SPF control-plane result installation adapter.
 */

#ifndef _FRR_BGP_MIDR_SPF_INSTALL_H
#define _FRR_BGP_MIDR_SPF_INSTALL_H

struct midr_context;
struct midr_spf_results;

/*
 * Compare two immutable SPF result sets and stage the required data-plane
 * updates.  Either result pointer may be NULL to represent an empty set.
 * Result pointers are borrowed for the duration of this call.
 */
extern void midr_spf_install_results(struct midr_context *ctx,
				     const struct midr_spf_results *old_results,
				     const struct midr_spf_results *new_results);

#endif /* _FRR_BGP_MIDR_SPF_INSTALL_H */
