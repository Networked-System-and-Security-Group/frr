// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR SPF control-plane result installation adapter.
 */

#include <zebra.h>

#include "lib/memory.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_memory.h"
#include "bgpd/bgp_midr_private.h"
#include "bgpd/bgp_midr_spf.h"
#include "bgpd/bgp_midr_spf_install.h"
#include "bgpd/bgp_midr_zebra.h"

DEFINE_MTYPE_STATIC(BGPD, MIDR_SPF_INSTALL_PATHS, "MIDR SPF install paths");

static int midr_spf_install_key_cmp(const struct midr_spf_route *a, const struct midr_spf_route *b)
{
	int ret;

	ret = (a->prefix.afi > b->prefix.afi) - (a->prefix.afi < b->prefix.afi);
	if (ret)
		return ret;
	ret = (a->prefix.safi > b->prefix.safi) - (a->prefix.safi < b->prefix.safi);
	if (ret)
		return ret;
	return prefix_cmp(&a->prefix.prefix, &b->prefix.prefix);
}

static size_t midr_spf_install_path_count(const struct midr_spf_route *route)
{
	size_t count;

	if (!route)
		return 0;
	count = MIN(route->nexthop_count, (size_t)MULTIPATH_NUM);
	return MIN(count, (size_t)UINT8_MAX);
}

/*
 * The current DP interface stores a union g_addr without a family tag and
 * interprets it using the destination prefix family.  Until that interface is
 * changed to struct ipaddr, reject mixed prefix/nexthop address families
 * instead of sending an ambiguous union value.
 */
static bool midr_spf_install_route_supported(const struct midr_spf_route *route)
{
	size_t count;
	size_t i;

	if (!route || !route->reachable || route->local_destination)
		return false;
	if (route->scope != MIDR_SPF_ROUTE_INTRA_GROUP &&
	    route->scope != MIDR_SPF_ROUTE_INTER_GROUP)
		return false;
	count = midr_spf_install_path_count(route);
	if (!count)
		return false;

	for (i = 0; i < count; i++) {
		enum ipaddr_type_t type = route->nexthops[i].address.ipa_type;

		if (route->nexthops[i].ifindex < 0)
			return false;
		if ((route->prefix.prefix.family == AF_INET && type != IPADDR_V4) ||
		    (route->prefix.prefix.family == AF_INET6 && type != IPADDR_V6))
			return false;
	}
	return true;
}

static bool midr_spf_install_forwarding_same(const struct midr_spf_route *a,
					     const struct midr_spf_route *b)
{
	size_t count;
	size_t i;

	if (!midr_spf_install_route_supported(a) || !midr_spf_install_route_supported(b))
		return false;
	count = midr_spf_install_path_count(a);
	if (count != midr_spf_install_path_count(b))
		return false;

	for (i = 0; i < count; i++)
		if (a->nexthops[i].ifindex != b->nexthops[i].ifindex ||
		    ipaddr_cmp(&a->nexthops[i].address, &b->nexthops[i].address) != 0)
			return false;
	return true;
}

static uint32_t midr_spf_install_metric(const struct midr_spf_route *route)
{
	uint64_t cost;

	cost = route->scope == MIDR_SPF_ROUTE_INTER_GROUP ? route->group_score : route->local_cost;
	return cost > UINT32_MAX ? UINT32_MAX : (uint32_t)cost;
}

static void midr_spf_install_add(struct bgp *bgp, const struct midr_spf_route *route)
{
	struct midr_path_result result = {
		.instance = MIDR_INSTANCE_SPF,
	};
	struct prefix prefix;
	struct midr_path *paths;
	uint32_t metric;
	size_t count;
	size_t i;

	if (!midr_spf_install_route_supported(route))
		return;

	count = midr_spf_install_path_count(route);
	paths = XCALLOC(MTYPE_MIDR_SPF_INSTALL_PATHS, count * sizeof(*paths));
	metric = midr_spf_install_metric(route);
	for (i = 0; i < count; i++) {
		const struct midr_spf_nexthop *source = &route->nexthops[i];

		if (source->address.ipa_type == IPADDR_V4)
			paths[i].nexthop.ipv4 = source->address.ipaddr_v4;
		else
			paths[i].nexthop.ipv6 = source->address.ipaddr_v6;
		paths[i].ifindex = (uint32_t)source->ifindex;
		paths[i].metric = metric;
		paths[i].path_avail_bw = source->available_bandwidth_kbps / 1000.0f;
		paths[i].weight = 0;
	}

	result.paths = paths;
	result.path_count = (uint8_t)count;
	result.explicit.sid_count = 0;
	prefix = route->prefix.prefix;
	midr_zebra_route_add(bgp, &prefix, &result);
	XFREE(MTYPE_MIDR_SPF_INSTALL_PATHS, paths);
}

static void midr_spf_install_delete(struct bgp *bgp, const struct midr_spf_route *route)
{
	struct prefix prefix;

	if (!route)
		return;
	prefix = route->prefix.prefix;
	midr_zebra_route_del(bgp, &prefix);
}

void midr_spf_install_results(struct midr_context *ctx, const struct midr_spf_results *old_results,
			      const struct midr_spf_results *new_results)
{
	const struct midr_spf_route *old_route;
	const struct midr_spf_route *new_route;
	struct bgp *bgp;
	size_t old_count;
	size_t new_count;
	size_t old_index = 0;
	size_t new_index = 0;
	bool changed = false;

	if (!ctx || !ctx->bgp || !ctx->bgp->midr_dp || (!old_results && !new_results))
		return;
	bgp = ctx->bgp;
	old_count = midr_spf_results_count(old_results);
	new_count = midr_spf_results_count(new_results);

	while (old_index < old_count || new_index < new_count) {
		int key_cmp;
		bool old_supported;
		bool new_supported;

		old_route = old_index < old_count ? midr_spf_results_at(old_results, old_index)
						  : NULL;
		new_route = new_index < new_count ? midr_spf_results_at(new_results, new_index)
						  : NULL;
		if (!old_route)
			key_cmp = 1;
		else if (!new_route)
			key_cmp = -1;
		else
			key_cmp = midr_spf_install_key_cmp(old_route, new_route);

		if (key_cmp < 0) {
			if (midr_spf_install_route_supported(old_route)) {
				midr_spf_install_delete(bgp, old_route);
				changed = true;
			}
			old_index++;
			continue;
		}
		if (key_cmp > 0) {
			if (midr_spf_install_route_supported(new_route)) {
				midr_spf_install_add(bgp, new_route);
				changed = true;
			}
			new_index++;
			continue;
		}

		old_supported = midr_spf_install_route_supported(old_route);
		new_supported = midr_spf_install_route_supported(new_route);
		if (old_supported && !new_supported) {
			midr_spf_install_delete(bgp, old_route);
			changed = true;
		} else if (!old_supported && new_supported) {
			midr_spf_install_add(bgp, new_route);
			changed = true;
		} else if (old_supported && new_supported) {
			uint32_t old_metric = midr_spf_install_metric(old_route);
			uint32_t new_metric = midr_spf_install_metric(new_route);

			if (old_metric != new_metric &&
			    midr_spf_install_forwarding_same(old_route, new_route)) {
				/*
				 * Current DP diff ignores metric-only changes.
				 * Force a delete/add pair until that implementation
				 * is fixed.
				 */
				midr_spf_install_delete(bgp, old_route);
				midr_spf_install_add(bgp, new_route);
				changed = true;
			} else if (old_metric != new_metric ||
				   !midr_spf_install_forwarding_same(old_route, new_route)) {
				midr_spf_install_add(bgp, new_route);
				changed = true;
			}
		}
		old_index++;
		new_index++;
	}

	if (changed)
		midr_zebra_route_update_deferred(bgp);
}
