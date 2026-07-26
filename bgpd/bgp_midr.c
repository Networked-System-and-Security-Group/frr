// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR lifecycle, session wrappers, and bgpd integration hooks.
 */

#include <zebra.h>

#include <errno.h>

#include "memory.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_midr.h"
#include "bgpd/bgp_midr_lsdb.h"
#include "bgpd/bgp_midr_owned.h"
#include "bgpd/bgp_midr_prefix.h"
#include "bgpd/bgp_midr_private.h"
#include "bgpd/bgp_midr_rib.h"
#include "bgpd/bgp_midr_sync.h"
#include "bgpd/bgp_midr_ted_private.h"
#include "bgpd/bgp_route.h"

DEFINE_MTYPE_STATIC(BGPD, BGP_MIDR, "BGP MIDR instance");

static bool midr_hooks_registered;

static struct midr_context *midr_context_from_bgp(struct bgp *bgp)
{
	if (!bgp || !bgp->midr_info)
		return NULL;

	return &bgp->midr_info->ctx;
}

struct midr_context *midr_context_get_default(void)
{
	return midr_context_from_bgp(bgp_get_default());
}

int midr_peer_session_request(struct midr_context *ctx,
			      const struct midr_peer_session_request_info *req)
{
	struct peer *peer;
	as_t remote_as;
	char remote_as_str[ASN_STRING_MAX_SIZE];
	int ret;

	if (!ctx || !ctx->bgp || !ctx->midr)
		return -ENOENT;
	if (!req)
		return -EINVAL;
	if (sockunion_family(&req->remote_address) != AF_INET &&
	    sockunion_family(&req->remote_address) != AF_INET6)
		return -EINVAL;
	if (!req->remote_as || req->afi <= AFI_UNSPEC || req->afi >= AFI_MAX ||
	    req->safi <= SAFI_UNSPEC || req->safi >= SAFI_MAX)
		return -EINVAL;
	if (req->has_update_source || req->ebgp_multihop || (req->password && req->password[0]) ||
	    req->policy_tags)
		return -ENOTSUP;

	remote_as = req->remote_as;
	snprintf(remote_as_str, sizeof(remote_as_str), "%u", remote_as);
	ret = peer_remote_as(ctx->bgp, (union sockunion *)&req->remote_address, NULL, &remote_as,
			     AS_SPECIFIED, remote_as_str);
	if (ret)
		return -EINVAL;

	peer = peer_lookup(ctx->bgp, (union sockunion *)&req->remote_address);
	if (!peer)
		return -ENOENT;

	ret = peer_activate(peer, req->afi, req->safi);
	if (ret)
		return -EINVAL;

	return 0;
}

int midr_peer_session_release(struct midr_context *ctx, const union sockunion *remote_address,
			      afi_t afi, safi_t safi, enum midr_peer_release_reason reason)
{
	struct peer *peer;
	int ret;

	if (!ctx || !ctx->bgp || !ctx->midr)
		return -ENOENT;
	if (!remote_address)
		return -EINVAL;
	if (sockunion_family(remote_address) != AF_INET &&
	    sockunion_family(remote_address) != AF_INET6)
		return -EINVAL;
	if (afi <= AFI_UNSPEC || afi >= AFI_MAX || safi <= SAFI_UNSPEC || safi >= SAFI_MAX)
		return -EINVAL;
	if (reason != MIDR_PEER_RELEASE_ADMIN && reason != MIDR_PEER_RELEASE_NODE_DOWN &&
	    reason != MIDR_PEER_RELEASE_POLICY)
		return -EINVAL;

	peer = peer_lookup(ctx->bgp, (union sockunion *)remote_address);
	if (!peer)
		return -ENOENT;

	ret = peer_deactivate(peer, afi, safi);
	if (ret)
		return -EINVAL;

	return 0;
}

int midr_remote_view_snapshot_get(struct midr_context *ctx,
				  struct midr_remote_view_snapshot *snapshot)
{
	return midr_lsdb_remote_snapshot_get(ctx, snapshot);
}

void midr_remote_view_snapshot_release(struct midr_context *ctx,
				       struct midr_remote_view_snapshot *snapshot)
{
	(void)ctx;
	midr_lsdb_remote_snapshot_release(snapshot);
}

int midr_remote_view_callbacks_register(struct midr_context *ctx,
					const struct midr_remote_view_callbacks *callbacks)
{
	if (!ctx || !ctx->bgp || !ctx->midr)
		return -ENOENT;
	if (!callbacks)
		return -EINVAL;

	ctx->midr->remote_callbacks = *callbacks;
	ctx->midr->remote_callbacks_registered = true;
	return 0;
}

static int midr_peer_status_changed(struct peer *peer)
{
	struct midr_context *ctx;

	if (!peer || !peer->bgp)
		return 0;

	ctx = midr_context_from_bgp(peer->bgp);
	if (ctx && ctx->midr) {
		ctx->midr->peer_hook_events++;
		midr_sync_peer_status_changed(ctx, peer);
	}

	return 0;
}

static int midr_bgp_route_update(struct bgp *bgp, afi_t afi, safi_t safi, struct bgp_dest *bn,
				 struct bgp_path_info *old_route, struct bgp_path_info *new_route)
{
	struct midr_context *ctx = midr_context_from_bgp(bgp);

	if (ctx && ctx->midr) {
		ctx->midr->route_hook_events++;
		if (afi == AFI_BGP_LS && safi == SAFI_MIDR_LS)
			midr_lsdb_route_changed(ctx, bn, old_route, new_route);
		else if ((afi == AFI_IP || afi == AFI_IP6) && safi == SAFI_UNICAST)
			midr_prefix_route_changed(ctx, afi, safi, bn, old_route, new_route);
	}

	return 0;
}

static void midr_hooks_register_once(void)
{
	if (midr_hooks_registered)
		return;

	hook_register(peer_status_changed, midr_peer_status_changed);
	hook_register(bgp_route_update, midr_bgp_route_update);
	hook_register(bgp_routerid_update, midr_input_router_id_update);
	midr_hooks_registered = true;
}

void bgp_midr_init(struct bgp *bgp)
{
	struct bgp_midr *midr;
	int ret;

	if (!bgp || bgp->midr_info)
		return;
	midr_hooks_register_once();

	midr = XCALLOC(MTYPE_BGP_MIDR, sizeof(*midr));
	midr->bgp = bgp;
	midr->ctx.bgp = bgp;
	midr->ctx.midr = midr;
	ret = midr_rib_init(&midr->ctx);
	if (ret) {
		XFREE(MTYPE_BGP_MIDR, midr);
		return;
	}
	ret = midr_ted_context_init(&midr->ctx);
	if (ret) {
		midr_rib_finish(&midr->ctx);
		XFREE(MTYPE_BGP_MIDR, midr);
		return;
	}
	ret = midr_sync_init(&midr->ctx);
	if (ret) {
		midr_ted_context_finish(&midr->ctx);
		midr_rib_finish(&midr->ctx);
		XFREE(MTYPE_BGP_MIDR, midr);
		return;
	}
	ret = midr_lsdb_init(&midr->ctx);
	if (ret) {
		midr_sync_finish(&midr->ctx);
		midr_ted_context_finish(&midr->ctx);
		midr_rib_finish(&midr->ctx);
		XFREE(MTYPE_BGP_MIDR, midr);
		return;
	}
	ret = midr_owned_init(&midr->ctx);
	if (ret) {
		midr_lsdb_finish(&midr->ctx);
		midr_sync_finish(&midr->ctx);
		midr_ted_context_finish(&midr->ctx);
		midr_rib_finish(&midr->ctx);
		XFREE(MTYPE_BGP_MIDR, midr);
		return;
	}
	ret = midr_input_init(&midr->ctx);
	if (ret) {
		midr_owned_finish(&midr->ctx);
		midr_lsdb_finish(&midr->ctx);
		midr_sync_finish(&midr->ctx);
		midr_ted_context_finish(&midr->ctx);
		midr_rib_finish(&midr->ctx);
		XFREE(MTYPE_BGP_MIDR, midr);
		return;
	}
	bgp->midr_info = midr;
	ret = midr_prefix_init(&midr->ctx);
	if (ret) {
		bgp->midr_info = NULL;
		midr_input_finish(&midr->ctx);
		midr_owned_finish(&midr->ctx);
		midr_lsdb_finish(&midr->ctx);
		midr_sync_finish(&midr->ctx);
		midr_ted_context_finish(&midr->ctx);
		midr_rib_finish(&midr->ctx);
		XFREE(MTYPE_BGP_MIDR, midr);
		return;
	}
}

void bgp_midr_finish(struct bgp *bgp)
{
	struct bgp_midr *midr;

	if (!bgp || !bgp->midr_info)
		return;

	midr = bgp->midr_info;
	midr_prefix_finish(&midr->ctx);
	midr_input_finish(&midr->ctx);
	midr_owned_finish(&midr->ctx);
	midr_lsdb_finish(&midr->ctx);
	midr_sync_finish(&midr->ctx);
	midr_ted_context_finish(&midr->ctx);
	midr_rib_finish(&midr->ctx);
	bgp->midr_info = NULL;
	XFREE(MTYPE_BGP_MIDR, midr);
}
