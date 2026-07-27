// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR path-attribute containers and parser adaptation.
 */

#ifndef _FRR_BGP_MIDR_ATTR_H
#define _FRR_BGP_MIDR_ATTR_H

#include <zebra.h>

#include <stddef.h>
#include <stdint.h>

#include "bgpd/bgp_attr.h"
#include "bgpd/bgp_midr_codec.h"

extern void bgp_midr_attr_init(void);
extern void bgp_midr_attr_finish(void);

extern struct bgp_midr_ls_attr *
bgp_midr_ls_attr_intern(const struct midr_ls_attributes *attributes);
extern struct bgp_midr_ls_attr *
bgp_midr_ls_attr_new(const struct midr_ls_attributes *attributes);
extern void
bgp_midr_ls_attr_intern_ref(struct bgp_midr_ls_attr **attr);
extern void bgp_midr_ls_attr_lock(struct bgp_midr_ls_attr *attr);
extern void bgp_midr_ls_attr_unintern(struct bgp_midr_ls_attr **attr);
extern void bgp_midr_ls_attr_flush(struct bgp_midr_ls_attr **attr);
extern const struct midr_ls_attributes *
bgp_midr_ls_attr_value(const struct bgp_midr_ls_attr *attr);
extern unsigned int
bgp_midr_ls_attr_hash_key(const struct bgp_midr_ls_attr *attr);
extern bool bgp_midr_ls_attr_same(const struct bgp_midr_ls_attr *a,
				 const struct bgp_midr_ls_attr *b);

extern struct bgp_midr_propagation_path_attr *
bgp_midr_propagation_path_attr_intern(
	const struct midr_propagation_path *path);
extern struct bgp_midr_propagation_path_attr *
bgp_midr_propagation_path_attr_new(const struct midr_propagation_path *path);
extern void bgp_midr_propagation_path_attr_intern_ref(
	struct bgp_midr_propagation_path_attr **attr);
extern void bgp_midr_propagation_path_attr_lock(
	struct bgp_midr_propagation_path_attr *attr);
extern void bgp_midr_propagation_path_attr_unintern(
	struct bgp_midr_propagation_path_attr **attr);
extern void bgp_midr_propagation_path_attr_flush(
	struct bgp_midr_propagation_path_attr **attr);
extern const struct midr_propagation_path *
bgp_midr_propagation_path_attr_value(
	const struct bgp_midr_propagation_path_attr *attr);
extern unsigned int bgp_midr_propagation_path_attr_hash_key(
	const struct bgp_midr_propagation_path_attr *attr);
extern bool bgp_midr_propagation_path_attr_same(
	const struct bgp_midr_propagation_path_attr *a,
	const struct bgp_midr_propagation_path_attr *b);

extern enum bgp_attr_parse_ret
bgp_midr_attr_decode(struct attr *attr, uint8_t type, uint8_t flags,
		     const uint8_t *value, size_t length);
extern enum bgp_attr_parse_ret
bgp_midr_attr_codec_result(enum midr_codec_result result);
extern int bgp_midr_nlri_codec_result(enum midr_codec_result result);
extern bool bgp_midr_attr_family_is_midr(bool has_mp_reach, afi_t afi,
					 safi_t safi);

#endif /* _FRR_BGP_MIDR_ATTR_H */
