/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "midr-local-provider.h"

#include <errno.h>
#include <stdbool.h>

static bool address_is_zero(const uint8_t address[MIDR_CORE_ADDR_BYTES])
{
	for (size_t i = 0; i < MIDR_CORE_ADDR_BYTES; i++)
		if (address[i])
			return false;
	return true;
}

static bool ipv4_padding_is_zero(
	const uint8_t address[MIDR_CORE_ADDR_BYTES])
{
	for (size_t i = 4; i < MIDR_CORE_ADDR_BYTES; i++)
		if (address[i])
			return false;
	return true;
}

int midr_local_event_validate(const struct midr_local_event *event)
{
	const struct midr_local_link *link;

	if (!event || !event->generation || !event->originator)
		return -EINVAL;
	switch (event->kind) {
	case MIDR_LOCAL_SNAPSHOT_BEGIN:
	case MIDR_LOCAL_SNAPSHOT_END:
	case MIDR_LOCAL_EOR:
		return 0;
	case MIDR_LOCAL_MEMBERSHIP:
		return event->fact.membership.group &&
		       event->fact.membership.version
			       ? 0
			       : -EINVAL;
	case MIDR_LOCAL_MEMBERSHIP_WITHDRAW:
		return !event->fact.membership.group &&
		       event->fact.membership.version ? 0 : -EINVAL;
	case MIDR_LOCAL_LINK:
		link = &event->fact.link;
		if (!link->remote_node_id ||
		    link->remote_node_id == event->originator || !link->version ||
		    link->reserved[0] || link->reserved[1] || link->reserved[2] ||
		    !link->rtt_us || link->loss_ppm >= 1000000U ||
		    !link->available_bandwidth_kbps ||
		    (link->family != MIDR_CORE_AF_IPV4 &&
		     link->family != MIDR_CORE_AF_IPV6) ||
		    address_is_zero(link->local_address) ||
		    address_is_zero(link->remote_address))
			return -EINVAL;
		if (link->family == MIDR_CORE_AF_IPV4 &&
		    (!ipv4_padding_is_zero(link->local_address) ||
		     !ipv4_padding_is_zero(link->remote_address)))
			return -EINVAL;
		return 0;
	case MIDR_LOCAL_LINK_WITHDRAW:
		link = &event->fact.link;
		if (!link->remote_node_id ||
		    link->remote_node_id == event->originator || !link->version ||
		    link->local_ifindex || link->family || link->reserved[0] ||
		    link->reserved[1] ||
		    link->reserved[2] || link->rtt_us || link->loss_ppm ||
		    link->available_bandwidth_kbps || link->measurement_sequence ||
		    link->measurement_timestamp_ms ||
		    !address_is_zero(link->local_address) ||
		    !address_is_zero(link->remote_address))
			return -EINVAL;
		return 0;
	default:
		return -EINVAL;
	}
}
