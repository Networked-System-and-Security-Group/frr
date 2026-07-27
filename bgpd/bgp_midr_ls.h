// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR link-state object definitions independent of the BGP RIB.
 */

#ifndef _FRR_BGP_MIDR_LS_H
#define _FRR_BGP_MIDR_LS_H

#include <zebra.h>

#include <stdbool.h>
#include <stdint.h>

#include "ipaddr.h"
#include "prefix.h"

#define BGP_ATTR_MIDR_LS 253U
#define BGP_ATTR_MIDR_PROPAGATION_PATH 254U

#define MIDR_PROPAGATION_PATH_MAX_NODES 512U

#define MIDR_METRIC_PRESENT_RTT 0x00000001U
#define MIDR_METRIC_PRESENT_LOSS 0x00000002U
#define MIDR_METRIC_PRESENT_AVAILABLE_BW 0x00000004U
#define MIDR_METRIC_REQUIRED_MASK                                                                 \
	(MIDR_METRIC_PRESENT_RTT | MIDR_METRIC_PRESENT_LOSS | MIDR_METRIC_PRESENT_AVAILABLE_BW)

enum midr_nlri_type {
	MIDR_NLRI_TYPE_RESERVED = 0,
	MIDR_NLRI_TYPE_MEMBERSHIP = 1,
	MIDR_NLRI_TYPE_LINK = 2,
	MIDR_NLRI_TYPE_NODE_PREFIX = 3,
	MIDR_NLRI_TYPE_GROUP_PREFIX = 4,
};

enum midr_ls_tlv_type {
	MIDR_LS_TLV_SEQUENCE = 1,
	MIDR_LS_TLV_POLICY_TAGS = 3,
	MIDR_LS_TLV_GROUP_ID = 100,
	MIDR_LS_TLV_TRANSPORT_ADDRESS = 101,
	MIDR_LS_TLV_CAP_FLAGS = 102,
	MIDR_LS_TLV_LINK_LOCAL_ADDRESS = 200,
	MIDR_LS_TLV_LINK_REMOTE_ADDRESS = 201,
	MIDR_LS_TLV_LINK_METRICS = 202,
};

struct midr_ls_prefix_key {
	afi_t afi;
	safi_t safi;
	struct prefix prefix;
};

struct midr_ls_object_key {
	enum midr_nlri_type type;
	uint32_t originator_node_id;
	union {
		struct {
			uint32_t remote_node_id;
			uint64_t link_id;
		} link;
		struct midr_ls_prefix_key node_prefix;
		struct {
			uint32_t group_id;
			struct midr_ls_prefix_key prefix;
		} group_prefix;
	} u;
};

struct midr_ls_metrics {
	uint32_t present_flags;
	uint32_t rtt_us;
	uint32_t loss_ppm;
	uint32_t available_bandwidth_kbps;
};

struct midr_ls_membership_payload {
	uint32_t group_id;
	bool has_transport_address;
	struct ipaddr transport_address;
	uint64_t cap_flags;
};

struct midr_ls_link_payload {
	struct ipaddr link_local_address;
	struct ipaddr link_remote_address;
	struct midr_ls_metrics metrics;
};

struct midr_ls_object {
	struct midr_ls_object_key key;
	uint64_t ls_sequence;
	uint64_t policy_tags;
	union {
		struct midr_ls_membership_payload membership;
		struct midr_ls_link_payload link;
	} payload;
};

extern int midr_ls_prefix_key_normalize(const struct midr_ls_prefix_key *input,
					struct midr_ls_prefix_key *output);
extern bool midr_ls_prefix_key_is_canonical(const struct midr_ls_prefix_key *key);
extern int midr_ls_object_key_validate(const struct midr_ls_object_key *key);
extern int midr_ls_object_validate(const struct midr_ls_object *object);

extern int midr_ls_object_key_cmp(const struct midr_ls_object_key *a,
				  const struct midr_ls_object_key *b);
extern bool midr_ls_object_key_same(const struct midr_ls_object_key *a,
				    const struct midr_ls_object_key *b);
extern unsigned int midr_ls_object_key_hash(const struct midr_ls_object_key *key);
extern bool midr_ls_object_same(const struct midr_ls_object *a, const struct midr_ls_object *b);

#endif /* _FRR_BGP_MIDR_LS_H */
