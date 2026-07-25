// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR topology provider input validation tests.
 */

#include <zebra.h>

#include <errno.h>

#include "bgpd/bgp_midr_private.h"

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

static struct midr_node_update valid_node(uint32_t local_node_id)
{
	struct midr_node_update node = {
		.node_id = local_node_id,
		.group_id = 0,
		.policy_state = MIDR_POLICY_ALLOWED,
		.version = 1,
	};

	SET_IPADDR_NONE(&node.transport_address);
	return node;
}

static struct midr_link_update valid_link(uint32_t local_node_id, uint32_t remote_node_id)
{
	struct midr_link_update link = {
		.key =
			{
				.local_node_id = local_node_id,
				.remote_node_id = remote_node_id,
				.link_id = 0,
			},
		.local_ifindex = 0,
		.link_local_address = ip_address("192.0.2.1"),
		.link_remote_address = ip_address("192.0.2.2"),
		.metrics =
			{
				.has_rtt_us = true,
				.rtt_us = 1,
				.has_loss_ppm = true,
				.loss_ppm = 0,
				.has_available_bandwidth_kbps = true,
				.available_bandwidth_kbps = 1,
			},
		.policy_state = MIDR_POLICY_ALLOWED,
		.version = 1,
	};

	return link;
}

static void test_node_validation(void)
{
	const uint32_t local = router_id("1.1.1.1");
	const uint32_t other = router_id("2.2.2.2");
	struct midr_node_update node = valid_node(local);

	assert(midr_validate_node_update(local, &node) == 0);
	assert(node.group_id == 0);
	assert(midr_validate_node_update(0, &node) == -ENOENT);
	assert(midr_validate_node_update(local, NULL) == -EINVAL);

	node.node_id = 0;
	assert(midr_validate_node_update(local, &node) == -EINVAL);
	node.node_id = other;
	assert(midr_validate_node_update(local, &node) == -EINVAL);
	node.node_id = local;

	node.has_transport_address = true;
	assert(midr_validate_node_update(local, &node) == -EINVAL);
	node.transport_address = ip_address("192.0.2.10");
	assert(midr_validate_node_update(local, &node) == 0);
	node.transport_address = ip_address("2001:db8::10");
	assert(midr_validate_node_update(local, &node) == 0);

	node.has_transport_address = false;
	assert(midr_validate_node_update(local, &node) == -EINVAL);
	SET_IPADDR_NONE(&node.transport_address);
	assert(midr_validate_node_update(local, &node) == 0);

	node.policy_state = MIDR_POLICY_BLOCKED;
	assert(midr_validate_node_update(local, &node) == 0);
	node.policy_state = (enum midr_policy_state)99;
	assert(midr_validate_node_update(local, &node) == -EINVAL);

	assert(midr_validate_node_withdraw(local, local) == 0);
	assert(midr_validate_node_withdraw(0, local) == -ENOENT);
	assert(midr_validate_node_withdraw(local, 0) == -EINVAL);
	assert(midr_validate_node_withdraw(local, other) == -EINVAL);
}

static void test_link_identity_and_address_validation(void)
{
	const uint32_t local = router_id("1.1.1.1");
	const uint32_t remote = router_id("2.2.2.2");
	const uint32_t other = router_id("3.3.3.3");
	struct midr_link_update link = valid_link(local, remote);

	assert(link.key.link_id == 0);
	assert(midr_validate_link_update(local, &link) == 0);
	assert(midr_validate_link_update(0, &link) == -ENOENT);
	assert(midr_validate_link_update(local, NULL) == -EINVAL);

	link.key.local_node_id = 0;
	assert(midr_validate_link_update(local, &link) == -EINVAL);
	link.key.local_node_id = other;
	assert(midr_validate_link_update(local, &link) == -EINVAL);
	link.key.local_node_id = local;

	link.key.remote_node_id = 0;
	assert(midr_validate_link_update(local, &link) == -EINVAL);
	link.key.remote_node_id = local;
	assert(midr_validate_link_update(local, &link) == -EINVAL);
	link.key.remote_node_id = remote;

	link.link_local_address = ip_address("2001:db8::1");
	link.link_remote_address = ip_address("2001:db8::2");
	assert(midr_validate_link_update(local, &link) == 0);
	link.link_remote_address = ip_address("192.0.2.2");
	assert(midr_validate_link_update(local, &link) == -EINVAL);
	SET_IPADDR_NONE(&link.link_local_address);
	assert(midr_validate_link_update(local, &link) == -EINVAL);

	link = valid_link(local, remote);
	link.local_ifindex = 1;
	assert(midr_validate_link_update(local, &link) == 0);
	link.local_ifindex = -1;
	assert(midr_validate_link_update(local, &link) == -EINVAL);

	link = valid_link(local, remote);
	link.policy_state = MIDR_POLICY_BLOCKED;
	assert(midr_validate_link_update(local, &link) == 0);
	link.policy_state = (enum midr_policy_state)99;
	assert(midr_validate_link_update(local, &link) == -EINVAL);

	assert(midr_validate_link_withdraw(local, &link.key) == 0);
	assert(midr_validate_link_withdraw(0, &link.key) == -ENOENT);
	assert(midr_validate_link_withdraw(local, NULL) == -EINVAL);
	link.key.local_node_id = other;
	assert(midr_validate_link_withdraw(local, &link.key) == -EINVAL);
	link.key.local_node_id = local;
	link.key.remote_node_id = 0;
	assert(midr_validate_link_withdraw(local, &link.key) == -EINVAL);
	link.key.remote_node_id = local;
	assert(midr_validate_link_withdraw(local, &link.key) == -EINVAL);
}

static void test_link_measurement_validation(void)
{
	const uint32_t local = router_id("1.1.1.1");
	const uint32_t remote = router_id("2.2.2.2");
	struct midr_link_update link = valid_link(local, remote);

	link.metrics.has_rtt_us = false;
	assert(midr_validate_link_update(local, &link) == -EINVAL);
	link = valid_link(local, remote);
	link.metrics.has_loss_ppm = false;
	assert(midr_validate_link_update(local, &link) == -EINVAL);
	link = valid_link(local, remote);
	link.metrics.has_available_bandwidth_kbps = false;
	assert(midr_validate_link_update(local, &link) == -EINVAL);

	link = valid_link(local, remote);
	link.metrics.rtt_us = 0;
	assert(midr_validate_link_update(local, &link) == -EINVAL);
	link = valid_link(local, remote);
	link.metrics.available_bandwidth_kbps = 0;
	assert(midr_validate_link_update(local, &link) == -EINVAL);

	link = valid_link(local, remote);
	link.metrics.loss_ppm = 999999;
	assert(midr_validate_link_update(local, &link) == 0);
	link.metrics.loss_ppm = 1000000;
	assert(midr_validate_link_update(local, &link) == -EINVAL);
	link.metrics.loss_ppm = 1000001;
	assert(midr_validate_link_update(local, &link) == -EINVAL);
}

int main(void)
{
	test_node_validation();
	test_link_identity_and_address_validation();
	test_link_measurement_validation();
	printf("MIDR input validation tests passed\n");
	return 0;
}
