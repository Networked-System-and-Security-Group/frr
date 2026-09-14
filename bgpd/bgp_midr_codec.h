// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR NLRI and path-attribute value codecs.
 */

#ifndef _FRR_BGP_MIDR_CODEC_H
#define _FRR_BGP_MIDR_CODEC_H

#include <zebra.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "stream.h"

#include "bgpd/bgp_midr_ls.h"
#include "bgpd/bgp_midr_instance.h"

enum midr_codec_result {
	MIDR_CODEC_OK = 0,
	MIDR_CODEC_UNKNOWN_NLRI_TYPE,
	MIDR_CODEC_UNKNOWN_TLV,
	MIDR_CODEC_MALFORMED_NLRI,
	MIDR_CODEC_MALFORMED_ATTRIBUTE,
	MIDR_CODEC_NO_SPACE,
	MIDR_CODEC_OVERFLOW,
};

enum midr_ls_attribute_presence {
	MIDR_LS_ATTR_HAS_SEQUENCE = (1U << 0),
	MIDR_LS_ATTR_HAS_POLICY_TAGS = (1U << 1),
	MIDR_LS_ATTR_HAS_GROUP_ID = (1U << 2),
	MIDR_LS_ATTR_HAS_TRANSPORT_ADDRESS = (1U << 3),
	MIDR_LS_ATTR_HAS_CAP_FLAGS = (1U << 4),
	MIDR_LS_ATTR_HAS_LINK_LOCAL_ADDRESS = (1U << 5),
	MIDR_LS_ATTR_HAS_LINK_REMOTE_ADDRESS = (1U << 6),
	MIDR_LS_ATTR_HAS_LINK_CANONICAL_COST = (1U << 7),
};

struct midr_ls_attributes {
	uint32_t present;
	uint64_t ls_sequence;
	uint64_t policy_tags;
	uint32_t group_id;
	struct ipaddr transport_address;
	uint64_t cap_flags;
	struct ipaddr link_local_address;
	struct ipaddr link_remote_address;
	uint32_t link_canonical_cost;
};

/* Explicit new-format entrypoints; legacy callers are not auto-upgraded. */
struct midr_instance_attributes {
	struct midr_ls_attributes ls;
	enum midr_instance_state state;
	uint32_t age_ms;
};

#define MIDR_INSTANCE_TLV_STATE 4U
#define MIDR_INSTANCE_TLV_AGE 5U

extern enum midr_codec_result midr_instance_attribute_encode(
	struct stream *stream, const struct midr_instance *instance, uint32_t age_ms);
extern enum midr_codec_result midr_instance_attribute_encode_tracked(
	struct stream *stream, const struct midr_instance *instance,
	uint32_t age_ms, size_t *age_offset);
extern enum midr_codec_result midr_instance_attribute_decode(
	struct stream *stream, size_t length, struct midr_instance_attributes *attributes);
extern enum midr_codec_result midr_instance_from_wire(
	const struct midr_ls_object_key *key, const struct midr_instance_attributes *attributes,
	struct midr_instance *instance);

extern enum midr_codec_result midr_nlri_encode(struct stream *stream,
					       const struct midr_ls_object_key *key);
extern enum midr_codec_result midr_nlri_decode(struct stream *stream,
					       struct midr_ls_object_key *key);

#endif /* _FRR_BGP_MIDR_CODEC_H */
