// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR custom-TLV setters.  See bgp_midr_tlv.h.
 */

#include <zebra.h>

#include "bgpd/bgpd.h"
#include "bgpd/bgp_ls_nlri.h"
#include "bgpd/bgp_midr_tlv.h"

void midr_tlv_set_group_id(struct bgp_ls_attr *ls_attr, uint32_t group_id)
{
	ls_attr->midr_group_id = group_id;
	SET_FLAG(ls_attr->present_tlvs, BGP_LS_ATTR_MIDR_GROUP_ID_BIT);
}

void midr_tlv_set_node_cap(struct bgp_ls_attr *ls_attr, uint32_t caps,
			   uint64_t seqno)
{
	ls_attr->midr_node_caps = caps;
	ls_attr->midr_cap_seqno = seqno;
	SET_FLAG(ls_attr->present_tlvs, BGP_LS_ATTR_MIDR_NODE_CAPABILITY_BIT);
}

void midr_tlv_set_link_perf(struct bgp_ls_attr *ls_attr, uint32_t delay_us,
			    uint32_t loss_rate, uint32_t bw_score,
			    uint64_t seqno)
{
	ls_attr->midr_delay_us = delay_us;
	ls_attr->midr_loss_rate = loss_rate;
	ls_attr->midr_bw_score = bw_score;
	ls_attr->midr_perf_seqno = seqno;
	SET_FLAG(ls_attr->present_tlvs, BGP_LS_ATTR_MIDR_LINK_PERF_BIT);
}

void midr_tlv_set_transport_addr(struct bgp_ls_attr *ls_attr,
				 struct in_addr addr)
{
	ls_attr->midr_transport_addr = addr;
	SET_FLAG(ls_attr->present_tlvs, BGP_LS_ATTR_MIDR_TRANSPORT_ADDR_BIT);
}
