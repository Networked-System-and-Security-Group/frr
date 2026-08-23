// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR MP_REACH/MP_UNREACH adaptation.
 */

#include <zebra.h>

#include <errno.h>

#include "stream.h"
#include "linklist.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_attr.h"
#include "bgpd/bgp_midr_attr.h"
#include "bgpd/bgp_midr_codec.h"
#include "bgpd/bgp_midr_owned.h"
#include "bgpd/bgp_midr_packet.h"
#include "bgpd/bgp_midr_private.h"
#include "bgpd/bgp_midr_rib.h"

static int midr_packet_withdraw(struct midr_context *ctx, struct peer *peer,
				const struct midr_ls_object_key *key)
{
	int ret = midr_rib_path_withdraw(ctx, peer, key);

	return ret == -ENOENT ? 0 : ret;
}

static bool midr_packet_peer_identity_valid(const struct peer *peer)
{
	struct listnode *node;
	struct peer *candidate;

	if (!peer->remote_id.s_addr || peer->remote_id.s_addr == peer->bgp->router_id.s_addr)
		return false;
	for (ALL_LIST_ELEMENTS_RO(peer->bgp->peer, node, candidate)) {
		if (candidate == peer || !candidate->connection ||
		    !peer_established(candidate->connection) ||
		    !candidate->afc_nego[AFI_BGP_LS][SAFI_MIDR_LS])
			continue;
		if (candidate->remote_id.s_addr == peer->remote_id.s_addr)
			return false;
	}
	return true;
}

int bgp_nlri_parse_midr(struct peer *peer, struct attr *attr, struct bgp_nlri *packet)
{
	const struct midr_propagation_path *path = NULL;
	const struct midr_ls_attributes *attributes = NULL;
	struct midr_context *ctx;
	struct stream *stream;
	int ret = BGP_NLRI_PARSE_OK;

	if (!peer || !peer->bgp || !peer->bgp->midr_info || !packet || packet->afi != AFI_BGP_LS ||
	    packet->safi != SAFI_MIDR_LS)
		return BGP_NLRI_PARSE_ERROR;
	if (!midr_packet_peer_identity_valid(peer))
		return BGP_NLRI_PARSE_ERROR;

	ctx = &peer->bgp->midr_info->ctx;
	if (attr) {
		attributes = bgp_midr_ls_attr_value(attr->midr_ls);
		path = bgp_midr_propagation_path_attr_value(attr->midr_propagation_path);
		if (!attributes || !path)
			return BGP_NLRI_PARSE_ERROR;
	}

	stream = stream_new(packet->length ? packet->length : 1);
	stream_put(stream, packet->nlri, packet->length);
	stream_set_getp(stream, 0);

	while (STREAM_READABLE(stream)) {
		struct midr_ls_object_key key;
		enum midr_codec_result result;

		result = midr_nlri_decode(stream, &key);
		if (result == MIDR_CODEC_UNKNOWN_NLRI_TYPE)
			continue;
		if (result != MIDR_CODEC_OK) {
			ret = bgp_midr_nlri_codec_result(result);
			break;
		}

		if (!attr) {
			(void)midr_packet_withdraw(ctx, peer, &key);
			continue;
		}

		struct midr_ls_object object;

		result = midr_ls_object_from_wire(&key, attributes, &object);
		if (result != MIDR_CODEC_OK ||
		    midr_propagation_path_validate(path, key.originator_node_id,
						   peer->remote_id.s_addr,
						   peer->bgp->router_id.s_addr) != 0) {
			(void)midr_packet_withdraw(ctx, peer, &key);
			continue;
		}

		if (key.originator_node_id == peer->bgp->router_id.s_addr) {
			(void)midr_owned_observe_self_sequence(ctx, &object);
			(void)midr_packet_withdraw(ctx, peer, &key);
			continue;
		}

		if (midr_rib_path_upsert(ctx, peer, &object, path) == -EINVAL)
			(void)midr_packet_withdraw(ctx, peer, &key);
	}

	stream_free(stream);
	return ret;
}

int bgp_midr_packet_attributes(struct stream *stream, struct bgp *bgp, struct bgp_path_info *path)
{
	const struct midr_propagation_path *stored_path;
	struct midr_propagation_path propagated = {};
	struct midr_ls_object object;
	size_t start;
	size_t length_pos;
	size_t value_start;
	int ret = -EINVAL;

	if (!stream || !bgp || !path || !path->net ||
	    midr_rib_path_object(path->net, path, &object) != 0)
		return -EINVAL;

	stored_path = bgp_midr_propagation_path_attr_value(path->attr->midr_propagation_path);
	if (!stored_path)
		return -EINVAL;

	start = stream_get_endp(stream);
	stream_putc(stream, BGP_ATTR_FLAG_OPTIONAL | BGP_ATTR_FLAG_EXTLEN);
	stream_putc(stream, BGP_ATTR_MIDR_LS);
	length_pos = stream_get_endp(stream);
	stream_putw(stream, 0);
	value_start = stream_get_endp(stream);
	if (midr_ls_attribute_encode(stream, &object) != MIDR_CODEC_OK)
		goto rollback;
	if (stream_get_endp(stream) - value_start > UINT16_MAX)
		goto rollback;
	stream_putw_at(stream, length_pos, stream_get_endp(stream) - value_start);

	if (path->peer != bgp->peer_self) {
		uint16_t index;

		if (midr_propagation_path_init(&propagated, stored_path->nodes[0]) != 0)
			goto rollback;
		for (index = 1; index < stored_path->node_count; index++)
			if (midr_propagation_path_append(&propagated, stored_path->nodes[index]) !=
			    0)
				goto rollback;
		if (midr_propagation_path_append(&propagated, bgp->router_id.s_addr) != 0)
			goto rollback;
	} else {
		propagated = *stored_path;
	}

	stream_putc(stream, BGP_ATTR_FLAG_OPTIONAL | BGP_ATTR_FLAG_EXTLEN);
	stream_putc(stream, BGP_ATTR_MIDR_PROPAGATION_PATH);
	length_pos = stream_get_endp(stream);
	stream_putw(stream, 0);
	value_start = stream_get_endp(stream);
	if (midr_propagation_path_encode(stream, &propagated) != MIDR_CODEC_OK)
		goto rollback;
	if (stream_get_endp(stream) - value_start > UINT16_MAX)
		goto rollback;
	stream_putw_at(stream, length_pos, stream_get_endp(stream) - value_start);
	ret = stream_get_endp(stream) - start;

rollback:
	if (path->peer != bgp->peer_self)
		midr_propagation_path_fini(&propagated);
	if (ret < 0)
		stream_set_endp(stream, start);
	return ret;
}

int bgp_midr_packet_nlri(struct stream *stream, const struct bgp_dest *dest)
{
	const struct midr_ls_object_key *key = midr_rib_dest_key(dest);

	if (!key)
		return -EINVAL;
	return midr_nlri_encode(stream, key) == MIDR_CODEC_OK ? 0 : -EINVAL;
}

size_t bgp_midr_packet_nlri_size(const struct bgp_dest *dest)
{
	struct stream *stream;
	size_t size = 0;

	stream = stream_new(64);
	if (bgp_midr_packet_nlri(stream, dest) == 0)
		size = stream_get_endp(stream);
	stream_free(stream);
	return size;
}
