// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR link-state object validation, identity, and equality helpers.
 */

#include <zebra.h>

#include <errno.h>

#include "jhash.h"

#include "bgpd/bgp_midr_ls.h"

static int midr_ls_cmp_u32(uint32_t a, uint32_t b)
{
	return (a > b) - (a < b);
}

static int midr_ls_cmp_u64(uint64_t a, uint64_t b)
{
	return (a > b) - (a < b);
}

static int midr_ls_cmp_node_id(uint32_t a, uint32_t b)
{
	return midr_ls_cmp_u32(ntohl(a), ntohl(b));
}

static int midr_ls_prefix_key_cmp(const struct midr_ls_prefix_key *a,
				  const struct midr_ls_prefix_key *b)
{
	int result;

	result = midr_ls_cmp_u32(a->afi, b->afi);
	if (result)
		return result;
	result = midr_ls_cmp_u32(a->safi, b->safi);
	if (result)
		return result;
	return prefix_cmp(&a->prefix, &b->prefix);
}

static bool midr_ls_prefix_key_same(const struct midr_ls_prefix_key *a,
				    const struct midr_ls_prefix_key *b)
{
	return a->afi == b->afi && a->safi == b->safi && prefix_same(&a->prefix, &b->prefix);
}

int midr_ls_prefix_key_normalize(const struct midr_ls_prefix_key *input,
				 struct midr_ls_prefix_key *output)
{
	if (!input || !output || input->safi != SAFI_UNICAST)
		return -EINVAL;

	if ((input->afi == AFI_IP &&
	     (input->prefix.family != AF_INET || input->prefix.prefixlen > IPV4_MAX_BITLEN)) ||
	    (input->afi == AFI_IP6 &&
	     (input->prefix.family != AF_INET6 || input->prefix.prefixlen > IPV6_MAX_BITLEN)) ||
	    (input->afi != AFI_IP && input->afi != AFI_IP6))
		return -EINVAL;

	*output = *input;
	apply_mask(&output->prefix);
	return 0;
}

bool midr_ls_prefix_key_is_canonical(const struct midr_ls_prefix_key *key)
{
	struct midr_ls_prefix_key normalized;

	return midr_ls_prefix_key_normalize(key, &normalized) == 0 &&
	       midr_ls_prefix_key_same(key, &normalized);
}

int midr_ls_object_key_validate(const struct midr_ls_object_key *key)
{
	if (!key || !key->originator_node_id)
		return -EINVAL;

	switch (key->type) {
	case MIDR_NLRI_TYPE_MEMBERSHIP:
		return 0;
	case MIDR_NLRI_TYPE_LINK:
		if (!key->u.link.remote_node_id ||
		    key->u.link.remote_node_id == key->originator_node_id)
			return -EINVAL;
		return 0;
	case MIDR_NLRI_TYPE_NODE_PREFIX:
		return midr_ls_prefix_key_is_canonical(&key->u.node_prefix) ? 0 : -EINVAL;
	case MIDR_NLRI_TYPE_GROUP_PREFIX:
		if (!key->u.group_prefix.group_id ||
		    !midr_ls_prefix_key_is_canonical(&key->u.group_prefix.prefix))
			return -EINVAL;
		return 0;
	case MIDR_NLRI_TYPE_RESERVED:
	default:
		return -EINVAL;
	}
}

static bool midr_ls_address_valid(const struct ipaddr *address)
{
	return address->ipa_type == IPADDR_V4 || address->ipa_type == IPADDR_V6;
}

static int midr_ls_membership_validate(const struct midr_ls_membership_payload *membership)
{
	if (!membership->group_id)
		return -EINVAL;

	if (membership->has_transport_address)
		return midr_ls_address_valid(&membership->transport_address) ? 0 : -EINVAL;

	return membership->transport_address.ipa_type == IPADDR_NONE ? 0 : -EINVAL;
}

static int midr_ls_link_validate(const struct midr_ls_link_payload *link)
{
	if (!midr_ls_address_valid(&link->link_local_address) ||
	    !midr_ls_address_valid(&link->link_remote_address) ||
	    link->link_local_address.ipa_type != link->link_remote_address.ipa_type)
		return -EINVAL;

	if (!link->canonical_cost || link->canonical_cost == UINT32_MAX)
		return -EINVAL;

	return 0;
}

int midr_ls_object_validate(const struct midr_ls_object *object)
{
	int result;

	if (!object)
		return -EINVAL;

	result = midr_ls_object_key_validate(&object->key);
	if (result)
		return result;

	switch (object->key.type) {
	case MIDR_NLRI_TYPE_MEMBERSHIP:
		return midr_ls_membership_validate(&object->payload.membership);
	case MIDR_NLRI_TYPE_LINK:
		return midr_ls_link_validate(&object->payload.link);
	case MIDR_NLRI_TYPE_NODE_PREFIX:
	case MIDR_NLRI_TYPE_GROUP_PREFIX:
		return 0;
	case MIDR_NLRI_TYPE_RESERVED:
	default:
		return -EINVAL;
	}
}

int midr_ls_object_key_cmp(const struct midr_ls_object_key *a, const struct midr_ls_object_key *b)
{
	int result;

	if (a == b)
		return 0;
	if (!a)
		return -1;
	if (!b)
		return 1;

	result = midr_ls_cmp_u32(a->type, b->type);
	if (result)
		return result;
	result = midr_ls_cmp_node_id(a->originator_node_id, b->originator_node_id);
	if (result)
		return result;

	switch (a->type) {
	case MIDR_NLRI_TYPE_MEMBERSHIP:
		return 0;
	case MIDR_NLRI_TYPE_LINK:
		result = midr_ls_cmp_node_id(a->u.link.remote_node_id, b->u.link.remote_node_id);
		if (result)
			return result;
		return midr_ls_cmp_u64(a->u.link.link_id, b->u.link.link_id);
	case MIDR_NLRI_TYPE_NODE_PREFIX:
		return midr_ls_prefix_key_cmp(&a->u.node_prefix, &b->u.node_prefix);
	case MIDR_NLRI_TYPE_GROUP_PREFIX:
		result = midr_ls_cmp_u32(a->u.group_prefix.group_id, b->u.group_prefix.group_id);
		if (result)
			return result;
		return midr_ls_prefix_key_cmp(&a->u.group_prefix.prefix, &b->u.group_prefix.prefix);
	case MIDR_NLRI_TYPE_RESERVED:
	default:
		return 0;
	}
}

bool midr_ls_object_key_same(const struct midr_ls_object_key *a, const struct midr_ls_object_key *b)
{
	return midr_ls_object_key_cmp(a, b) == 0;
}

unsigned int midr_ls_object_key_hash(const struct midr_ls_object_key *key)
{
	uint32_t hash;

	if (!key)
		return 0;

	hash = jhash_2words((uint32_t)key->type, key->originator_node_id, 0);

	switch (key->type) {
	case MIDR_NLRI_TYPE_MEMBERSHIP:
		return hash;
	case MIDR_NLRI_TYPE_LINK:
		hash = jhash_1word(key->u.link.remote_node_id, hash);
		return jhash_2words((uint32_t)(key->u.link.link_id >> 32),
				    (uint32_t)key->u.link.link_id, hash);
	case MIDR_NLRI_TYPE_NODE_PREFIX:
		hash = jhash_2words(key->u.node_prefix.afi, key->u.node_prefix.safi, hash);
		return jhash_1word(prefix_hash_key(&key->u.node_prefix.prefix), hash);
	case MIDR_NLRI_TYPE_GROUP_PREFIX:
		hash = jhash_1word(key->u.group_prefix.group_id, hash);
		hash = jhash_2words(key->u.group_prefix.prefix.afi,
				    key->u.group_prefix.prefix.safi, hash);
		return jhash_1word(prefix_hash_key(&key->u.group_prefix.prefix.prefix), hash);
	case MIDR_NLRI_TYPE_RESERVED:
	default:
		return hash;
	}
}

static bool midr_ls_membership_same(const struct midr_ls_membership_payload *a,
				    const struct midr_ls_membership_payload *b)
{
	if (a->group_id != b->group_id || a->has_transport_address != b->has_transport_address ||
	    a->cap_flags != b->cap_flags)
		return false;

	return !a->has_transport_address ||
	       ipaddr_cmp(&a->transport_address, &b->transport_address) == 0;
}

static bool midr_ls_link_same(const struct midr_ls_link_payload *a,
			      const struct midr_ls_link_payload *b)
{
	return ipaddr_cmp(&a->link_local_address, &b->link_local_address) == 0 &&
	       ipaddr_cmp(&a->link_remote_address, &b->link_remote_address) == 0 &&
	       a->canonical_cost == b->canonical_cost;
}

bool midr_ls_object_same(const struct midr_ls_object *a, const struct midr_ls_object *b)
{
	if (a == b)
		return true;
	if (!a || !b || !midr_ls_object_key_same(&a->key, &b->key) ||
	    a->ls_sequence != b->ls_sequence || a->policy_tags != b->policy_tags)
		return false;

	switch (a->key.type) {
	case MIDR_NLRI_TYPE_MEMBERSHIP:
		return midr_ls_membership_same(&a->payload.membership, &b->payload.membership);
	case MIDR_NLRI_TYPE_LINK:
		return midr_ls_link_same(&a->payload.link, &b->payload.link);
	case MIDR_NLRI_TYPE_NODE_PREFIX:
	case MIDR_NLRI_TYPE_GROUP_PREFIX:
		return true;
	case MIDR_NLRI_TYPE_RESERVED:
	default:
		return false;
	}
}
