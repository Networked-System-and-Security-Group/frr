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
#include "bgpd/bgp_midr_sync.h"

static int midr_packet_withdraw(struct midr_context *ctx, struct peer *peer,
				const struct midr_ls_object_key *key)
{
	int ret = midr_rib_peer_withdraw(ctx, peer, key);

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
	const struct midr_ls_attributes *attributes = NULL;
	struct midr_context *ctx;
	struct stream *stream;
	struct midr_ls_object_key key;
	enum midr_codec_result result;
	int ret = BGP_NLRI_PARSE_OK;

	if (!peer || !peer->bgp || !peer->bgp->midr_info || !packet || packet->afi != AFI_BGP_LS ||
	    packet->safi != SAFI_MIDR_LS)
		return BGP_NLRI_PARSE_ERROR;
	if (!midr_packet_peer_identity_valid(peer))
		return BGP_NLRI_PARSE_ERROR;

	ctx = &peer->bgp->midr_info->ctx;
	if (attr) {
		attributes = bgp_midr_ls_attr_value(attr->midr_ls);
		if (!attributes || !attr->midr_ls)
			return BGP_NLRI_PARSE_ERROR;
	}

	stream = stream_new(packet->length ? packet->length : 1);
	stream_put(stream, packet->nlri, packet->length);
	stream_set_getp(stream, 0);

	/* P2 deliberately accepts one semantic object per UPDATE.  Decode the
	 * complete NLRI before changing any RIB state. */
	result = midr_nlri_decode(stream, &key);
	if (result != MIDR_CODEC_OK) {
		ret = bgp_midr_nlri_codec_result(result);
		goto done;
	}
	if (STREAM_READABLE(stream)) {
		ret = BGP_NLRI_PARSE_ERROR;
		goto done;
	}

	if (!attr) {
		ret = midr_packet_withdraw(ctx, peer, &key) == 0
			      ? BGP_NLRI_PARSE_OK
			      : BGP_NLRI_PARSE_ERROR;
		goto done;
	}

	{
		struct midr_instance_attributes wire = {
			.ls = *attributes,
			.state = bgp_midr_ls_attr_state(attr->midr_ls),
			.age_ms = bgp_midr_ls_attr_age(attr->midr_ls),
		};
		struct midr_instance instance;

		result = midr_instance_from_wire(&key, &wire, &instance);
		if (result != MIDR_CODEC_OK) {
			ret = bgp_midr_nlri_codec_result(result);
			goto done;
		}

		if (key.originator_node_id == peer->bgp->router_id.s_addr) {
			ret = midr_owned_observe_self_instance(
				ctx, &key, instance.object.ls_sequence);
			goto done;
		}

		ret = midr_rib_instance_upsert(ctx, peer, &instance,
					       wire.age_ms);
		if (ret == 0)
			ret = BGP_NLRI_PARSE_OK;
		else if (ret == -ENOSPC || ret == -ENOMEM || ret == -ERANGE) {
			/* A well-formed UPDATE that cannot be admitted for
			 * resource reasons must not reset the session.  The
			 * packet is counted as received; the unadmitted input
			 * is recovered by resync/refresh with backoff. */
			midr_sync_input_rejected(ctx, peer->connection);
			ret = BGP_NLRI_PARSE_OK;
		} else
			ret = BGP_NLRI_PARSE_ERROR;
	}

done:
	stream_free(stream);
	return ret;
}

int bgp_midr_packet_attributes(struct stream *stream, struct bgp *bgp, struct bgp_path_info *path)
{
	struct midr_instance instance;
	uint32_t age_ms;
	size_t start;
	size_t length_pos;
	size_t value_start;
	int ret = -EINVAL;

	if (!stream || !bgp || !bgp->midr_info || !path || !path->net ||
	    midr_rib_path_instance(&bgp->midr_info->ctx, path->net, path,
				   &instance, &age_ms) != 0)
		return -EINVAL;

	start = stream_get_endp(stream);
	stream_putc(stream, BGP_ATTR_FLAG_OPTIONAL | BGP_ATTR_FLAG_EXTLEN);
	stream_putc(stream, BGP_ATTR_MIDR_LS);
	length_pos = stream_get_endp(stream);
	stream_putw(stream, 0);
	value_start = stream_get_endp(stream);
	if (midr_instance_attribute_encode(stream, &instance, age_ms) !=
	    MIDR_CODEC_OK)
		goto rollback;
	if (stream_get_endp(stream) - value_start > UINT16_MAX)
		goto rollback;
	stream_putw_at(stream, length_pos, stream_get_endp(stream) - value_start);

	ret = stream_get_endp(stream) - start;

rollback:
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
