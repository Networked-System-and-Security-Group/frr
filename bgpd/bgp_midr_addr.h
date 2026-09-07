// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Address-family-neutral helpers for MIDR transport locators.
 */

#ifndef _FRR_BGP_MIDR_ADDR_H
#define _FRR_BGP_MIDR_ADDR_H

#include "ipaddr.h"
#include "prefix.h"
#include "sockunion.h"

static inline struct ipaddr midr_ipaddr_none(void)
{
	struct ipaddr addr = {};

	addr.ipa_type = IPADDR_NONE;
	return addr;
}

static inline struct ipaddr midr_ipaddr_from_ipv4(struct in_addr addr)
{
	struct ipaddr locator = {};

	locator.ipa_type = IPADDR_V4;
	locator.ipaddr_v4 = addr;
	return locator;
}

static inline bool midr_ipaddr_to_ipv4(const struct ipaddr *locator,
				       struct in_addr *addr)
{
	if (!locator || !addr || !IS_IPADDR_V4(locator))
		return false;

	*addr = locator->ipaddr_v4;
	return true;
}

static inline bool midr_ipaddr_valid_locator(const struct ipaddr *locator)
{
	if (!locator || ipaddr_is_zero(locator) || ipaddr_is_mcast(locator))
		return false;

	if (IS_IPADDR_V4(locator))
		return true;

	if (!IS_IPADDR_V6(locator))
		return false;

	return !IN6_IS_ADDR_LINKLOCAL(&locator->ipaddr_v6) &&
	       !IN6_IS_ADDR_V4MAPPED(&locator->ipaddr_v6);
}

static inline bool midr_ipaddr_same(const struct ipaddr *left,
				     const struct ipaddr *right)
{
	return left && right && ipaddr_is_same(left, right);
}

static inline bool midr_ipaddr_from_prefix(const struct prefix *prefix,
					   struct ipaddr *locator)
{
	if (!prefix || !locator)
		return false;

	memset(locator, 0, sizeof(*locator));
	switch (prefix->family) {
	case AF_INET:
		locator->ipa_type = IPADDR_V4;
		locator->ipaddr_v4 = prefix->u.prefix4;
		return true;
	case AF_INET6:
		locator->ipa_type = IPADDR_V6;
		locator->ipaddr_v6 = prefix->u.prefix6;
		return true;
	default:
		locator->ipa_type = IPADDR_NONE;
		return false;
	}
}

static inline bool midr_ipaddr_to_host_prefix(const struct ipaddr *locator,
					      struct prefix *prefix)
{
	if (!locator || !prefix)
		return false;

	memset(prefix, 0, sizeof(*prefix));
	switch (locator->ipa_type) {
	case IPADDR_V4:
		prefix->family = AF_INET;
		prefix->prefixlen = IPV4_MAX_BITLEN;
		prefix->u.prefix4 = locator->ipaddr_v4;
		return true;
	case IPADDR_V6:
		prefix->family = AF_INET6;
		prefix->prefixlen = IPV6_MAX_BITLEN;
		prefix->u.prefix6 = locator->ipaddr_v6;
		return true;
	case IPADDR_NONE:
		return false;
	}

	return false;
}

static inline bool midr_ipaddr_to_sockunion(const struct ipaddr *locator,
					    union sockunion *su)
{
	if (!locator || !su)
		return false;

	memset(su, 0, sizeof(*su));
	switch (locator->ipa_type) {
	case IPADDR_V4:
		su->sin.sin_family = AF_INET;
		su->sin.sin_addr = locator->ipaddr_v4;
		return true;
	case IPADDR_V6:
		su->sin6.sin6_family = AF_INET6;
		su->sin6.sin6_addr = locator->ipaddr_v6;
		return true;
	case IPADDR_NONE:
		return false;
	}

	return false;
}

static inline bool midr_sockunion_to_ipaddr(const union sockunion *su,
					    struct ipaddr *locator)
{
	if (!su || !locator)
		return false;

	memset(locator, 0, sizeof(*locator));
	switch (sockunion_family(su)) {
	case AF_INET:
		locator->ipa_type = IPADDR_V4;
		locator->ipaddr_v4 = su->sin.sin_addr;
		return true;
	case AF_INET6:
		locator->ipa_type = IPADDR_V6;
		locator->ipaddr_v6 = su->sin6.sin6_addr;
		return true;
	default:
		locator->ipa_type = IPADDR_NONE;
		return false;
	}
}

#endif /* _FRR_BGP_MIDR_ADDR_H */
