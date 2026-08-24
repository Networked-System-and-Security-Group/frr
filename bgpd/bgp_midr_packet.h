// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR MP_REACH/MP_UNREACH adaptation.
 */

#ifndef _FRR_BGP_MIDR_PACKET_H
#define _FRR_BGP_MIDR_PACKET_H

#include <zebra.h>

#include "bgpd/bgp_route.h"

struct attr;
struct bgp_dest;
struct bgp_path_info;
struct peer;
struct stream;

extern int bgp_nlri_parse_midr(struct peer *peer, struct attr *attr, struct bgp_nlri *packet);
extern int bgp_midr_packet_attributes(struct stream *stream, struct bgp *bgp,
				      struct bgp_path_info *path);
extern int bgp_midr_packet_nlri(struct stream *stream, const struct bgp_dest *dest);
extern size_t bgp_midr_packet_nlri_size(const struct bgp_dest *dest);

#endif /* _FRR_BGP_MIDR_PACKET_H */
