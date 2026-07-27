// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * libFuzzer target for MIDR NLRI and attribute value codecs.
 */

#include <zebra.h>

#include <stdint.h>

#include "privs.h"

#include "bgpd/bgp_midr_codec.h"

/* Required variables when linking against libfrr. */
struct zebra_privs_t bgpd_privs = {};
struct event_loop *master;

static struct midr_ls_object_key fuzz_key(enum midr_nlri_type type)
{
	struct midr_ls_object_key key = {
		.type = type,
		.originator_node_id = htonl(0x01010101),
	};

	switch (type) {
	case MIDR_NLRI_TYPE_LINK:
		key.u.link.remote_node_id = htonl(0x02020202);
		key.u.link.link_id = 1;
		break;
	case MIDR_NLRI_TYPE_NODE_PREFIX:
		key.u.node_prefix.afi = AFI_IP;
		key.u.node_prefix.safi = SAFI_UNICAST;
		key.u.node_prefix.prefix.family = AF_INET;
		key.u.node_prefix.prefix.prefixlen = 24;
		key.u.node_prefix.prefix.u.prefix4.s_addr = htonl(0xc0000200);
		break;
	case MIDR_NLRI_TYPE_GROUP_PREFIX:
		key.u.group_prefix.group_id = 100;
		key.u.group_prefix.prefix.afi = AFI_IP6;
		key.u.group_prefix.prefix.safi = SAFI_UNICAST;
		key.u.group_prefix.prefix.prefix.family = AF_INET6;
		key.u.group_prefix.prefix.prefix.prefixlen = 32;
		key.u.group_prefix.prefix.prefix.u.prefix6.s6_addr[0] = 0x20;
		key.u.group_prefix.prefix.prefix.u.prefix6.s6_addr[1] = 0x01;
		key.u.group_prefix.prefix.prefix.u.prefix6.s6_addr[2] = 0x0d;
		key.u.group_prefix.prefix.prefix.u.prefix6.s6_addr[3] = 0xb8;
		break;
	case MIDR_NLRI_TYPE_MEMBERSHIP:
	case MIDR_NLRI_TYPE_RESERVED:
	default:
		break;
	}
	return key;
}

static void fuzz_nlri(const uint8_t *data, size_t size)
{
	struct midr_ls_object_key decoded;
	struct midr_ls_object_key round_trip;
	struct stream *input = stream_new(size ? size : 1);
	struct stream *output;

	if (size)
		stream_put(input, data, size);
	if (midr_nlri_decode(input, &decoded) == MIDR_CODEC_OK) {
		output = stream_new(size + 32);
		assert(midr_nlri_encode(output, &decoded) == MIDR_CODEC_OK);
		assert(midr_nlri_decode(output, &round_trip) == MIDR_CODEC_OK);
		assert(midr_ls_object_key_same(&decoded, &round_trip));
		stream_free(output);
	}
	stream_free(input);
}

static void fuzz_ls_attribute(const uint8_t *data, size_t size)
{
	struct midr_ls_attributes attributes;
	struct midr_ls_object object;
	struct midr_ls_object_key key;
	struct stream *input = stream_new(size ? size : 1);
	struct stream *output;
	enum midr_nlri_type type;

	if (size)
		stream_put(input, data, size);
	if (midr_ls_attribute_decode(input, size, &attributes) == MIDR_CODEC_OK) {
		for (type = MIDR_NLRI_TYPE_MEMBERSHIP; type <= MIDR_NLRI_TYPE_GROUP_PREFIX;
		     type++) {
			key = fuzz_key(type);
			if (midr_ls_object_from_wire(&key, &attributes, &object) != MIDR_CODEC_OK)
				continue;
			output = stream_new(256);
			assert(midr_ls_attribute_encode(output, &object) == MIDR_CODEC_OK);
			stream_free(output);
		}
	}
	stream_free(input);
}

static void fuzz_propagation_path(const uint8_t *data, size_t size)
{
	struct midr_propagation_path path = {};
	struct stream *input = stream_new(size ? size : 1);
	struct stream *output;

	if (size)
		stream_put(input, data, size);
	if (midr_propagation_path_decode(input, size, &path) == MIDR_CODEC_OK) {
		output = stream_new(size);
		assert(midr_propagation_path_encode(output, &path) == MIDR_CODEC_OK);
		assert(stream_get_endp(output) == size);
		assert(memcmp(STREAM_DATA(output), data, size) == 0);
		stream_free(output);
		midr_propagation_path_fini(&path);
	}
	stream_free(input);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (!size)
		return 0;

	switch (data[0] % 3) {
	case 0:
		fuzz_nlri(data + 1, size - 1);
		break;
	case 1:
		fuzz_ls_attribute(data + 1, size - 1);
		break;
	case 2:
		fuzz_propagation_path(data + 1, size - 1);
		break;
	}
	return 0;
}
