// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR custom-TLV setters (thin wrappers over struct bgp_ls_attr).
 *
 * The wire encode/decode of TLV 1185/1186/1187 already lives in
 * bgp_ls_nlri.c.  These helpers only populate the corresponding
 * bgp_ls_attr fields and set the present_tlvs bit so the existing
 * encoder emits them on origination.
 */

#ifndef _FRR_BGP_MIDR_TLV_H
#define _FRR_BGP_MIDR_TLV_H

#include <stdint.h>
#include <netinet/in.h>

struct bgp_ls_attr;

/* TLV 1185: Group ID */
extern void midr_tlv_set_group_id(struct bgp_ls_attr *ls_attr,
				  uint32_t group_id);

/* TLV 1187: Node capabilities + sequence number */
extern void midr_tlv_set_node_cap(struct bgp_ls_attr *ls_attr, uint32_t caps,
				  uint64_t seqno);

/* TLV 1186: Link performance (delay/loss/bw_score) + sequence number */
extern void midr_tlv_set_link_perf(struct bgp_ls_attr *ls_attr,
				   uint32_t delay_us, uint32_t loss_rate,
				   uint32_t bw_score, uint64_t seqno);

/* TLV 1188: Transport address (node's reachable IP, peering/probe target) */
extern void midr_tlv_set_transport_addr(struct bgp_ls_attr *ls_attr,
					struct in_addr addr);

#endif /* _FRR_BGP_MIDR_TLV_H */
