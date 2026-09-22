// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef _FRR_BGP_MIDR_PM_NET_H
#define _FRR_BGP_MIDR_PM_NET_H

#include "bgpd/bgp_midr_addr.h"

static inline bool midr_pm_net_set_port(union sockunion *su, uint16_t port)
{
	switch (sockunion_family(su)) {
	case AF_INET:
		su->sin.sin_port = htons(port);
		return true;
	case AF_INET6:
		su->sin6.sin6_port = htons(port);
		return true;
	default:
		return false;
	}
}

static inline uint16_t midr_pm_net_get_port(const union sockunion *su)
{
	switch (sockunion_family(su)) {
	case AF_INET:
		return ntohs(su->sin.sin_port);
	case AF_INET6:
		return ntohs(su->sin6.sin6_port);
	default:
		return 0;
	}
}

static inline socklen_t midr_pm_net_sockaddr_size(const union sockunion *su)
{
	switch (sockunion_family(su)) {
	case AF_INET:
		return sizeof(struct sockaddr_in);
	case AF_INET6:
		return sizeof(struct sockaddr_in6);
	default:
		return 0;
	}
}

static inline bool
midr_pm_net_transport_to_sockunion(const struct ipaddr *transport,
				   uint16_t port, union sockunion *su)
{
	return midr_ipaddr_valid_locator(transport)
	       && midr_ipaddr_to_sockunion(transport, su)
	       && midr_pm_net_set_port(su, port);
}

static inline bool
midr_pm_net_socket_matches(int sock, const struct ipaddr *transport,
			   uint16_t port)
{
	union sockunion bound = {};
	struct ipaddr bound_addr;
	socklen_t bound_len = sizeof(bound);

	if (sock < 0 || !midr_ipaddr_valid_locator(transport))
		return false;
	if (getsockname(sock, &bound.sa, &bound_len) < 0)
		return false;
	if (!midr_sockunion_to_ipaddr(&bound, &bound_addr))
		return false;

	return midr_pm_net_get_port(&bound) == port
	       && midr_ipaddr_same(&bound_addr, transport);
}

static inline bool
midr_pm_net_source_valid(const struct ipaddr *local,
			 const struct ipaddr *source, uint16_t source_port,
			 uint16_t expected_port)
{
	return midr_ipaddr_valid_locator(local)
	       && midr_ipaddr_valid_locator(source)
	       && local->ipa_type == source->ipa_type
	       && source_port == expected_port;
}

static inline int midr_pm_net_enable_v6only(int family, int sock)
{
	int enabled = 1;

	if (family != AF_INET6)
		return 0;
#ifdef IPV6_V6ONLY
	return setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &enabled,
			  sizeof(enabled));
#else
	errno = EOPNOTSUPP;
	return -1;
#endif
}

#endif /* _FRR_BGP_MIDR_PM_NET_H */
