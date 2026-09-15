/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "midr-spf.h"

#include <errno.h>
#include <string.h>

static int route_key_equal(const struct midr_spf_route *route,
			   const struct midr_consumer_event *event)
{
	return route->family == event->family &&
	       route->prefix_len == event->prefix_len &&
	       !memcmp(route->prefix, event->prefix, sizeof(route->prefix));
}

int midr_spf_compute(const struct midr_consumer_snapshot *snapshot,
			    struct midr_spf_route *routes, size_t capacity,
			    size_t *count)
{
	size_t route_count = 0;

	if (!snapshot || !count || (capacity && !routes))
		return -EINVAL;
	for (size_t i = 0; i < snapshot->count; i++) {
		const struct midr_consumer_event *event = &snapshot->events[i];
		struct midr_spf_route *route = NULL;

		if (event->kind != MIDR_CONSUMER_NODE_PREFIX &&
		    event->kind != MIDR_CONSUMER_GROUP_PREFIX)
			continue;
		for (size_t j = 0; j < route_count; j++)
			if (route_key_equal(&routes[j], event)) {
				route = &routes[j];
				break;
			}
		if (!route) {
				if (route_count == capacity)
					return -ENOSPC;
				route = &routes[route_count++];
				memset(route, 0, sizeof(*route));
				route->family = event->family;
				route->prefix_len = event->prefix_len;
				memcpy(route->prefix, event->prefix,
				       sizeof(route->prefix));
			}
		if (!route->generation || event->metric < route->metric ||
		    (event->metric == route->metric &&
		     event->originator < route->originator)) {
			route->originator = event->originator;
			route->metric = event->metric;
			route->generation = event->generation;
		}
	}
	*count = route_count;
	return 0;
}
