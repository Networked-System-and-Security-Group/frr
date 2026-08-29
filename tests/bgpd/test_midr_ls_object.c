// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR link-state object core tests.
 */

#include <zebra.h>

#include <errno.h>

#include "bgpd/bgp_midr_ls.h"

/* Required variables when linking against bgpd/libbgp.a. */
struct zebra_privs_t bgpd_privs = {};
struct event_loop *master;

static uint32_t router_id(const char *text)
{
	struct in_addr address;

	assert(inet_pton(AF_INET, text, &address) == 1);
	return address.s_addr;
}

static struct ipaddr ip_address(const char *text)
{
	struct ipaddr address;

	assert(str2ipaddr(text, &address) == 0);
	return address;
}

static struct midr_ls_prefix_key prefix_key(const char *text)
{
	struct midr_ls_prefix_key key = {
		.safi = SAFI_UNICAST,
	};

	assert(str2prefix(text, &key.prefix) > 0);
	key.afi = key.prefix.family == AF_INET ? AFI_IP : AFI_IP6;
	return key;
}

static struct midr_ls_object membership_object(void)
{
	struct midr_ls_object object = {
		.key =
			{
				.type = MIDR_NLRI_TYPE_MEMBERSHIP,
				.originator_node_id = router_id("1.1.1.1"),
			},
		.ls_sequence = 10,
		.policy_tags = 20,
		.payload.membership =
			{
				.group_id = 100,
				.has_transport_address = true,
				.transport_address = ip_address("192.0.2.1"),
				.cap_flags = 0,
			},
	};

	return object;
}

static struct midr_ls_object link_object(void)
{
	struct midr_ls_object object = {
		.key =
			{
				.type = MIDR_NLRI_TYPE_LINK,
				.originator_node_id = router_id("1.1.1.1"),
				.u.link =
					{
						.remote_node_id =
							router_id("2.2.2.2"),
						.link_id = 0,
					},
			},
		.ls_sequence = 11,
		.payload.link =
			{
				.link_local_address =
					ip_address("2001:db8::1"),
					.link_remote_address =
						ip_address("2001:db8::2"),
					.canonical_cost = 100,
			},
	};

	return object;
}

static void test_prefix_normalization(void)
{
	struct midr_ls_prefix_key input = prefix_key("192.0.2.129/24");
	struct midr_ls_prefix_key normalized;
	struct midr_ls_prefix_key expected;

	assert(!midr_ls_prefix_key_is_canonical(&input));
	assert(midr_ls_prefix_key_normalize(&input, &normalized) == 0);
	assert(midr_ls_prefix_key_is_canonical(&normalized));
	expected = prefix_key("192.0.2.0/24");
	assert(prefix_same(&normalized.prefix, &expected.prefix));

	input = prefix_key("2001:db8::1234/64");
	assert(midr_ls_prefix_key_normalize(&input, &normalized) == 0);
	expected = prefix_key("2001:db8::/64");
	assert(prefix_same(&normalized.prefix, &expected.prefix));

	input = prefix_key("2001:db8:abcd:ef01:80ff::/73");
	assert(midr_ls_prefix_key_normalize(&input, &normalized) == 0);
	expected = prefix_key("2001:db8:abcd:ef01:8080::/73");
	assert(prefix_same(&normalized.prefix, &expected.prefix));

	input.safi = SAFI_MULTICAST;
	assert(midr_ls_prefix_key_normalize(&input, &normalized) == -EINVAL);
}

static void test_identity(void)
{
	struct midr_ls_object membership = membership_object();
	struct midr_ls_object other = membership;
	struct midr_ls_object link = link_object();
	struct midr_ls_object_key prefix = {
		.type = MIDR_NLRI_TYPE_NODE_PREFIX,
		.originator_node_id = router_id("1.1.1.1"),
		.u.node_prefix = prefix_key("198.51.100.0/24"),
	};
	struct midr_ls_object_key group_prefix = {
		.type = MIDR_NLRI_TYPE_GROUP_PREFIX,
		.originator_node_id = router_id("1.1.1.1"),
		.u.group_prefix =
			{
				.group_id = 100,
				.prefix = prefix_key("2001:db8:1::/48"),
			},
	};
	struct midr_ls_object_key ipv6_prefix = {
		.type = MIDR_NLRI_TYPE_NODE_PREFIX,
		.originator_node_id = router_id("1.1.1.1"),
		.u.node_prefix = prefix_key("::ffff:198.51.100.0/120"),
	};

	assert(midr_ls_object_key_validate(&membership.key) == 0);
	assert(midr_ls_object_key_validate(&link.key) == 0);
	assert(midr_ls_object_key_validate(&prefix) == 0);
	assert(midr_ls_object_key_validate(&ipv6_prefix) == 0);
	assert(midr_ls_object_key_validate(&group_prefix) == 0);
	assert(!midr_ls_object_key_same(&prefix, &ipv6_prefix));
	assert(midr_ls_object_key_cmp(&prefix, &ipv6_prefix) != 0);

	assert(midr_ls_object_key_same(&membership.key, &other.key));
	assert(midr_ls_object_key_hash(&membership.key) == midr_ls_object_key_hash(&other.key));
	assert(midr_ls_object_key_hash(&link.key) != 0);
	assert(midr_ls_object_key_hash(&prefix) != 0);
	assert(midr_ls_object_key_hash(&group_prefix) != 0);
	assert(midr_ls_object_key_cmp(&membership.key, &link.key) < 0);

	other.key.originator_node_id = router_id("2.2.2.2");
	assert(!midr_ls_object_key_same(&membership.key, &other.key));

	prefix.u.node_prefix = prefix_key("198.51.100.1/24");
	assert(midr_ls_object_key_validate(&prefix) == -EINVAL);
	group_prefix.u.group_prefix.group_id = 0;
	assert(midr_ls_object_key_validate(&group_prefix) == -EINVAL);
}

static void test_membership_validation_and_equality(void)
{
	struct midr_ls_object object = membership_object();
	struct midr_ls_object copy = object;

	assert(midr_ls_object_validate(&object) == 0);
	assert(midr_ls_object_same(&object, &copy));

	copy.ls_sequence++;
	assert(!midr_ls_object_same(&object, &copy));
	copy = object;
	copy.payload.membership.cap_flags = 1;
	assert(!midr_ls_object_same(&object, &copy));

	object.payload.membership.group_id = 0;
	assert(midr_ls_object_validate(&object) == -EINVAL);
	object = membership_object();
	object.payload.membership.has_transport_address = false;
	assert(midr_ls_object_validate(&object) == -EINVAL);
	SET_IPADDR_NONE(&object.payload.membership.transport_address);
	assert(midr_ls_object_validate(&object) == 0);
}

static void test_link_validation_and_equality(void)
{
	struct midr_ls_object object = link_object();
	struct midr_ls_object copy = object;

	assert(midr_ls_object_validate(&object) == 0);
	assert(midr_ls_object_same(&object, &copy));
	assert(object.key.u.link.link_id == 0);

	copy.payload.link.canonical_cost++;
	assert(!midr_ls_object_same(&object, &copy));

	object.key.u.link.remote_node_id = object.key.originator_node_id;
	assert(midr_ls_object_validate(&object) == -EINVAL);
	object = link_object();
	object.payload.link.link_remote_address = ip_address("192.0.2.2");
	assert(midr_ls_object_validate(&object) == -EINVAL);
	object = link_object();
	object.payload.link.canonical_cost = 0;
	assert(midr_ls_object_validate(&object) == -EINVAL);
	object = link_object();
	object.payload.link.canonical_cost = UINT32_MAX;
	assert(midr_ls_object_validate(&object) == -EINVAL);
}

static void test_defensive_arguments(void)
{
	struct midr_ls_prefix_key input = prefix_key("192.0.2.0/24");
	struct midr_ls_prefix_key normalized;
	struct midr_ls_object membership = membership_object();
	struct midr_ls_object link = link_object();
	struct midr_ls_object invalid_copy;
	struct midr_ls_object_key key = membership.key;

	assert(midr_ls_prefix_key_normalize(NULL, &normalized) == -EINVAL);
	assert(midr_ls_prefix_key_normalize(&input, NULL) == -EINVAL);
	input.afi = AFI_IP6;
	assert(midr_ls_prefix_key_normalize(&input, &normalized) == -EINVAL);
	input = prefix_key("192.0.2.0/24");
	input.afi = AFI_UNSPEC;
	assert(midr_ls_prefix_key_normalize(&input, &normalized) == -EINVAL);
	input = prefix_key("2001:db8::/64");
	input.prefix.prefixlen = IPV6_MAX_BITLEN + 1;
	assert(midr_ls_prefix_key_normalize(&input, &normalized) == -EINVAL);
	assert(!midr_ls_prefix_key_is_canonical(NULL));

	assert(midr_ls_object_key_validate(NULL) == -EINVAL);
	key.originator_node_id = 0;
	assert(midr_ls_object_key_validate(&key) == -EINVAL);
	key = link.key;
	key.u.link.remote_node_id = 0;
	assert(midr_ls_object_key_validate(&key) == -EINVAL);
	key.type = MIDR_NLRI_TYPE_RESERVED;
	assert(midr_ls_object_key_validate(&key) == -EINVAL);

	assert(midr_ls_object_key_cmp(NULL, NULL) == 0);
	assert(midr_ls_object_key_cmp(NULL, &membership.key) < 0);
	assert(midr_ls_object_key_cmp(&membership.key, NULL) > 0);
	assert(midr_ls_object_key_hash(NULL) == 0);
	assert(!midr_ls_object_key_same(&membership.key, &link.key));

	assert(midr_ls_object_validate(NULL) == -EINVAL);
	membership.payload.membership.transport_address.ipa_type = IPADDR_NONE;
	assert(midr_ls_object_validate(&membership) == -EINVAL);
	link.payload.link.link_local_address.ipa_type = IPADDR_NONE;
	assert(midr_ls_object_validate(&link) == -EINVAL);

	assert(!midr_ls_object_same(NULL, &membership));
	assert(midr_ls_object_same(NULL, NULL));
	link.key.type = MIDR_NLRI_TYPE_RESERVED;
	invalid_copy = link;
	assert(!midr_ls_object_same(&link, &invalid_copy));
}

int main(void)
{
	test_prefix_normalization();
	test_identity();
	test_membership_validation_and_equality();
	test_link_validation_and_equality();
	test_defensive_arguments();
	printf("MIDR LS object tests passed\n");
	return 0;
}
