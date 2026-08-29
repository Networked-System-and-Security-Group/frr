// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR immutable TED snapshot and builder tests.
 */

#include <zebra.h>

#include <errno.h>
#include <limits.h>

#include "bgpd/bgp_midr_ted_private.h"
#include "tests/bgpd/test_midr_ted_consumer.h"

/* Required variables when linking against bgpd/libbgp.a. */
struct zebra_privs_t bgpd_privs = {};
struct event_loop *master;

struct notification_state {
	struct midr_context *ctx;
	struct midr_ted_consumer **self;
	uint64_t generation;
	uint32_t change_flags;
	unsigned int count;
	bool unregister_self;
};

static uint32_t node_id(const char *text)
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

static struct midr_ted_prefix_key prefix_key(const char *text)
{
	struct midr_ted_prefix_key key = {
		.safi = SAFI_UNICAST,
	};

	assert(str2prefix(text, &key.prefix) > 0);
	if (key.prefix.family == AF_INET)
		key.afi = AFI_IP;
	else {
		assert(key.prefix.family == AF_INET6);
		key.afi = AFI_IP6;
	}
	return key;
}

static struct midr_ted_node node(uint32_t id, uint32_t group_id)
{
	return (struct midr_ted_node){
		.node_id = id,
		.group_id = group_id,
	};
}

static struct midr_ted_link_input link_input(uint32_t local_node_id, uint32_t remote_node_id,
					     uint64_t link_id, uint32_t canonical_cost)
{
	return (struct midr_ted_link_input){
		.local_node_id = local_node_id,
		.remote_node_id = remote_node_id,
		.link_id = link_id,
		.canonical_cost = canonical_cost,
		.link_local_address = ip_address("192.0.2.1"),
		.link_remote_address = ip_address("192.0.2.2"),
	};
}

static struct midr_ted_node_prefix node_prefix(const char *text, uint32_t id)
{
	return (struct midr_ted_node_prefix){
		.key = prefix_key(text),
		.node_id = id,
	};
}

static struct midr_ted_prefix_group prefix_group(const char *text, uint32_t group_id)
{
	return (struct midr_ted_prefix_group){
		.key = prefix_key(text),
		.group_id = group_id,
	};
}

static void snapshot_changed(struct midr_context *ctx, uint64_t generation, uint32_t change_flags,
			     void *arg)
{
	struct notification_state *state = arg;
	const struct midr_ted_snapshot *snapshot = NULL;

	assert(state->ctx == ctx);
	assert(midr_ted_snapshot_get(ctx, &snapshot) == 0);
	assert(snapshot->generation == generation);
	midr_ted_snapshot_release(&snapshot);

	state->generation = generation;
	state->change_flags = change_flags;
	state->count++;
	if (state->unregister_self)
		midr_ted_consumer_unregister(ctx, state->self);
}

static void add_nodes(struct midr_ted_builder *builder, const struct midr_ted_node *nodes,
		      size_t count, bool reverse)
{
	size_t i;
	size_t index;

	for (i = 0; i < count; i++) {
		index = reverse ? count - i - 1 : i;
		assert(midr_ted_builder_add_node(builder, &nodes[index]) == 0);
	}
}

static void add_links(struct midr_ted_builder *builder, const struct midr_ted_link_input *links,
		      size_t count, bool reverse)
{
	size_t i;
	size_t index;

	for (i = 0; i < count; i++) {
		index = reverse ? count - i - 1 : i;
		assert(midr_ted_builder_add_link(builder, &links[index]) == 0);
	}
}

static void populate_full_fixture(struct midr_ted_builder *builder, bool reverse)
{
	const uint32_t r1 = node_id("1.1.1.1");
	const uint32_t r2 = node_id("2.2.2.2");
	const uint32_t r3 = node_id("3.3.3.3");
	const uint32_t r4 = node_id("4.4.4.4");
	const uint32_t r5 = node_id("5.5.5.5");
	const uint32_t missing = node_id("6.6.6.6");
	struct midr_ted_node nodes[] = {
		node(r4, 300), node(r2, 100), node(r5, 0), node(r3, 200), node(r1, 100),
	};
	struct midr_ted_link_input links[] = {
		link_input(r3, r4, 1, 40),	  link_input(r4, r3, 1, 41),
		link_input(r1, r3, 100, 50),	  link_input(r2, r3, 101, 20),
		link_input(r3, r1, 102, 30),	  link_input(r2, r1, 2, 11),
		link_input(r1, r2, 1, 10),	  link_input(r1, r5, 103, 60),
		link_input(r1, missing, 104, 70),
	};
	struct midr_ted_node_prefix node_prefixes[] = {
		node_prefix("203.0.113.129/24", r2),  node_prefix("203.0.113.1/24", r1),
		node_prefix("198.51.100.0/24", r3),   node_prefix("192.0.2.0/24", r5),
		node_prefix("192.0.2.0/24", missing),
	};
	struct midr_ted_prefix_group prefix_groups[] = {
		prefix_group("203.0.113.129/24", 200),
		prefix_group("198.51.100.0/24", 300),
		prefix_group("203.0.113.1/24", 100),
		prefix_group("192.0.2.0/24", 999),
	};
	size_t i;
	size_t index;

	links[2].local_ifindex = 7;
	links[6].local_ifindex = 5;
	links[7].local_ifindex = 8;
	links[8].local_ifindex = 9;

	add_nodes(builder, nodes, array_size(nodes), reverse);
	add_links(builder, links, array_size(links), reverse);

	for (i = 0; i < array_size(node_prefixes); i++) {
		index = reverse ? array_size(node_prefixes) - i - 1 : i;
		assert(midr_ted_builder_add_node_prefix(builder, &node_prefixes[index]) == 0);
	}
	for (i = 0; i < array_size(prefix_groups); i++) {
		index = reverse ? array_size(prefix_groups) - i - 1 : i;
		assert(midr_ted_builder_add_prefix_group(builder, &prefix_groups[index]) == 0);
	}
}

static void test_not_ready_and_context_validation(void)
{
	struct midr_context ctx = {};
	struct midr_ted_status status;
	const struct midr_ted_snapshot *snapshot = NULL;
	const struct midr_ted_snapshot *occupied = (const struct midr_ted_snapshot *)(uintptr_t)1;

	assert(midr_ted_snapshot_get(NULL, &snapshot) == -ENOENT);
	assert(midr_ted_snapshot_get(&ctx, &snapshot) == -ENOENT);
	assert(midr_ted_snapshot_get(&ctx, NULL) == -EINVAL);
	assert(midr_ted_snapshot_get(&ctx, &occupied) == -EINVAL);
	assert(!midr_ted_generation_is_current(NULL, 1));
	assert(!midr_ted_generation_is_current(&ctx, 1));
	assert(midr_ted_status_get(NULL, &status) == -ENOENT);
	assert(midr_ted_status_get(&ctx, NULL) == -EINVAL);

	assert(midr_ted_context_init(NULL) == -EINVAL);
	assert(midr_ted_context_init(&ctx) == 0);
	assert(midr_ted_context_init(&ctx) == -EALREADY);
	assert(midr_ted_snapshot_get(&ctx, &snapshot) == -EAGAIN);
	assert(midr_ted_status_get(&ctx, &status) == 0);
	assert(!status.ready);
	assert(status.generation == 0);
	assert(status.consumer_count == 0);
	assert(midr_ted_test_generation_set(&ctx, 1) == -EAGAIN);
	assert(midr_ted_test_generation_set(&ctx, 0) == -EINVAL);

	midr_ted_snapshot_release(NULL);
	midr_ted_snapshot_release(&snapshot);
	midr_ted_context_finish(&ctx);
	midr_ted_context_finish(&ctx);
}

static void test_builder_input_validation(void)
{
	const uint32_t r1 = node_id("1.1.1.1");
	const uint32_t r2 = node_id("2.2.2.2");
	const uint32_t r3 = node_id("3.3.3.3");
	struct midr_ted_builder *builder = NULL;
	struct midr_ted_builder *occupied = (struct midr_ted_builder *)(uintptr_t)1;
	struct midr_ted_node n;
	struct midr_ted_link_input link;
	struct midr_ted_node_prefix attachment;
	struct midr_ted_prefix_group mapping;

	assert(midr_ted_builder_create(0, 100, &builder) == -EINVAL);
	assert(midr_ted_builder_create(r1, 0, &builder) == -EINVAL);
	assert(midr_ted_builder_create(r1, 100, NULL) == -EINVAL);
	assert(midr_ted_builder_create(r1, 100, &occupied) == -EINVAL);
	assert(midr_ted_builder_create(r1, 100, &builder) == 0);

	n = node(0, 100);
	assert(midr_ted_builder_add_node(builder, &n) == -EINVAL);
	assert(midr_ted_builder_add_node(NULL, &n) == -EINVAL);
	assert(midr_ted_builder_add_node(builder, NULL) == -EINVAL);
	n = node(r1, 100);
	assert(midr_ted_builder_add_node(builder, &n) == 0);
	assert(midr_ted_builder_add_node(builder, &n) == -EEXIST);
	n = node(r2, 100);
	assert(midr_ted_builder_add_node(builder, &n) == 0);
	n = node(r3, 200);
	assert(midr_ted_builder_add_node(builder, &n) == 0);

	link = link_input(r1, r2, 0, 1);
	link.local_ifindex = 3;
	assert(midr_ted_builder_add_link(builder, &link) == 0);
	assert(midr_ted_builder_add_link(builder, &link) == -EEXIST);
	assert(midr_ted_builder_add_link(NULL, &link) == -EINVAL);
	assert(midr_ted_builder_add_link(builder, NULL) == -EINVAL);

	link = link_input(0, r2, 1, 1);
	assert(midr_ted_builder_add_link(builder, &link) == -EINVAL);
	link = link_input(r1, 0, 1, 1);
	assert(midr_ted_builder_add_link(builder, &link) == -EINVAL);
	link = link_input(r1, r1, 1, 1);
	assert(midr_ted_builder_add_link(builder, &link) == -EINVAL);
	link = link_input(r1, r2, 1, 0);
	assert(midr_ted_builder_add_link(builder, &link) == -EINVAL);
	link.canonical_cost = UINT32_MAX;
	assert(midr_ted_builder_add_link(builder, &link) == -EINVAL);
	link = link_input(r1, r2, 1, 1);
	link.local_ifindex = -1;
	assert(midr_ted_builder_add_link(builder, &link) == -EINVAL);

	link = link_input(r2, r3, 1, 1);
	link.local_ifindex = 2;
	assert(midr_ted_builder_add_link(builder, &link) == -EINVAL);
	link.local_ifindex = 0;
	assert(midr_ted_builder_add_link(builder, &link) == 0);

	link = link_input(r1, r2, 2, 1);
	SET_IPADDR_NONE(&link.link_local_address);
	assert(midr_ted_builder_add_link(builder, &link) == -EINVAL);
	link = link_input(r1, r2, 2, 1);
	SET_IPADDR_NONE(&link.link_remote_address);
	assert(midr_ted_builder_add_link(builder, &link) == -EINVAL);
	link = link_input(r1, r2, 2, 1);
	link.link_remote_address = ip_address("2001:db8::2");
	assert(midr_ted_builder_add_link(builder, &link) == -EINVAL);
	link.link_local_address = ip_address("2001:db8::1");
	assert(midr_ted_builder_add_link(builder, &link) == 0);

	attachment = node_prefix("203.0.113.129/24", r1);
	assert(midr_ted_builder_add_node_prefix(builder, &attachment) == 0);
	attachment = node_prefix("203.0.113.1/24", r1);
	assert(midr_ted_builder_add_node_prefix(builder, &attachment) == -EEXIST);
	attachment.node_id = 0;
	assert(midr_ted_builder_add_node_prefix(builder, &attachment) == -EINVAL);
	attachment = node_prefix("203.0.113.0/24", r2);
	attachment.key.safi = SAFI_MULTICAST;
	assert(midr_ted_builder_add_node_prefix(builder, &attachment) == -EINVAL);
	attachment = node_prefix("203.0.113.0/24", r2);
	attachment.key.afi = AFI_IP6;
	assert(midr_ted_builder_add_node_prefix(builder, &attachment) == -EINVAL);
	assert(midr_ted_builder_add_node_prefix(NULL, &attachment) == -EINVAL);
	assert(midr_ted_builder_add_node_prefix(builder, NULL) == -EINVAL);

	mapping = prefix_group("2001:db8:1::1/64", 100);
	assert(midr_ted_builder_add_prefix_group(builder, &mapping) == 0);
	mapping = prefix_group("2001:db8:1::ffff/64", 100);
	assert(midr_ted_builder_add_prefix_group(builder, &mapping) == -EEXIST);
	mapping.group_id = 0;
	assert(midr_ted_builder_add_prefix_group(builder, &mapping) == -EINVAL);
	mapping = prefix_group("2001:db8:2::/64", 200);
	mapping.key.afi = AFI_IP;
	assert(midr_ted_builder_add_prefix_group(builder, &mapping) == -EINVAL);
	assert(midr_ted_builder_add_prefix_group(NULL, &mapping) == -EINVAL);
	assert(midr_ted_builder_add_prefix_group(builder, NULL) == -EINVAL);

	midr_ted_builder_destroy(NULL);
	midr_ted_builder_destroy(&builder);
	midr_ted_builder_destroy(&builder);
}

static void test_snapshot_derivation_and_stable_order(void)
{
	const uint32_t r1 = node_id("1.1.1.1");
	const uint32_t r2 = node_id("2.2.2.2");
	struct midr_context ctx = {};
	struct midr_ted_builder *builder = NULL;
	struct midr_ted_status status;
	const struct midr_ted_snapshot *snapshot = NULL;

	assert(midr_ted_context_init(&ctx) == 0);
	assert(midr_ted_builder_create(r1, 100, &builder) == 0);
	populate_full_fixture(builder, false);
	assert(midr_ted_builder_publish(&ctx, builder, MIDR_TED_SYNC_REASON_EOR_TIMEOUT) == 0);
	midr_ted_builder_destroy(&builder);

	assert(midr_ted_snapshot_get(&ctx, &snapshot) == 0);
	assert(snapshot->ready);
	assert(snapshot->generation == 1);
	assert(snapshot->local_node_id == r1);
	assert(snapshot->local_group_id == 100);
	assert(snapshot->sync_reason_flags == MIDR_TED_SYNC_REASON_EOR_TIMEOUT);

	assert(snapshot->node_count == 2);
	assert(snapshot->nodes[0].node_id == r1);
	assert(snapshot->nodes[1].node_id == r2);

	assert(snapshot->intra_link_count == 2);
	assert(snapshot->intra_links[0].local_node_id == r1);
	assert(snapshot->intra_links[0].remote_node_id == r2);
	assert(snapshot->intra_links[0].canonical_cost == 10);
	assert(snapshot->intra_links[0].local_ifindex == 5);
	assert(snapshot->intra_links[1].local_node_id == r2);
	assert(snapshot->intra_links[1].remote_node_id == r1);

	assert(snapshot->egress_link_count == 2);
	assert(snapshot->egress_links[0].local_node_id == r1);
	assert(snapshot->egress_links[0].canonical_cost == 50);
	assert(snapshot->egress_links[1].local_node_id == r2);
	assert(snapshot->egress_links[1].canonical_cost == 20);
	assert(snapshot->egress_links[1].local_ifindex == 0);

	assert(snapshot->group_edge_count == 4);
	assert(snapshot->group_edges[0].source_group_id == 100);
	assert(snapshot->group_edges[0].target_group_id == 200);
	assert(snapshot->group_edges[0].aggregate_cost == 20);
	assert(snapshot->group_edges[1].source_group_id == 200);
	assert(snapshot->group_edges[1].target_group_id == 100);
	assert(snapshot->group_edges[1].aggregate_cost == 30);
	assert(snapshot->group_edges[2].source_group_id == 200);
	assert(snapshot->group_edges[2].target_group_id == 300);
	assert(snapshot->group_edges[2].aggregate_cost == 40);
	assert(snapshot->group_edges[3].source_group_id == 300);
	assert(snapshot->group_edges[3].target_group_id == 200);
	assert(snapshot->group_edges[3].aggregate_cost == 41);

	assert(snapshot->node_prefix_count == 2);
	assert(snapshot->node_prefixes[0].node_id == r1);
	assert(snapshot->node_prefixes[1].node_id == r2);
	assert(prefix_same(&snapshot->node_prefixes[0].key.prefix,
			   &snapshot->node_prefixes[1].key.prefix));

	assert(snapshot->prefix_group_count == 3);
	assert(snapshot->prefix_groups[1].group_id == 100);
	assert(snapshot->prefix_groups[2].group_id == 200);
	assert(prefix_same(&snapshot->prefix_groups[1].key.prefix,
			   &snapshot->prefix_groups[2].key.prefix));

	assert(midr_ted_status_get(&ctx, &status) == 0);
	assert(status.ready);
	assert(status.generation == 1);
	assert(status.pending_link_count == 2);
	assert(status.pending_node_prefix_count == 2);
	assert(status.pending_prefix_group_count == 1);

	midr_ted_snapshot_release(&snapshot);

	assert(midr_ted_builder_create(r1, 100, &builder) == 0);
	populate_full_fixture(builder, true);
	assert(midr_ted_builder_publish(&ctx, builder, MIDR_TED_SYNC_REASON_EOR_TIMEOUT) == 0);
	midr_ted_builder_destroy(&builder);
	assert(midr_ted_snapshot_get(&ctx, &snapshot) == 0);
	assert(snapshot->generation == 1);
	midr_ted_snapshot_release(&snapshot);

	midr_ted_context_finish(&ctx);
}

static void test_ipv6_snapshot_and_address_family_isolation(void)
{
	const uint32_t r1 = node_id("1.1.1.1");
	const uint32_t r2 = node_id("2.2.2.2");
	const uint32_t r3 = node_id("3.3.3.3");
	struct midr_context ctx = {};
	const struct midr_ted_snapshot *snapshot = NULL;
	struct midr_ted_builder *builder = NULL;
	struct midr_ted_link_input link;
	struct midr_ted_node n;
	struct midr_ted_node_prefix attachment;
	struct midr_ted_prefix_group mapping;
	struct midr_ted_prefix_key ipv6_key = prefix_key("2001:db8:100::/64");
	struct midr_ted_prefix_key ipv4_key = prefix_key("192.0.2.0/24");
	size_t ipv6_group_rows = 0;
	size_t index;

	assert(midr_ted_context_init(&ctx) == 0);
	assert(midr_ted_builder_create(r1, 100, &builder) == 0);
	n = node(r1, 100);
	assert(midr_ted_builder_add_node(builder, &n) == 0);
	n = node(r2, 100);
	assert(midr_ted_builder_add_node(builder, &n) == 0);
	n = node(r3, 200);
	assert(midr_ted_builder_add_node(builder, &n) == 0);

	link = link_input(r1, r2, 10, 25);
	link.local_ifindex = 7;
	link.link_local_address = ip_address("2001:db8:12::1");
	link.link_remote_address = ip_address("2001:db8:12::2");
	assert(midr_ted_builder_add_link(builder, &link) == 0);

	attachment = node_prefix("2001:db8:100::/64", r1);
	assert(midr_ted_builder_add_node_prefix(builder, &attachment) == 0);
	attachment = node_prefix("192.0.2.0/24", r2);
	assert(midr_ted_builder_add_node_prefix(builder, &attachment) == 0);

	mapping = prefix_group("2001:db8:100::/64", 100);
	assert(midr_ted_builder_add_prefix_group(builder, &mapping) == 0);
	mapping.group_id = 200;
	assert(midr_ted_builder_add_prefix_group(builder, &mapping) == 0);
	mapping = prefix_group("192.0.2.0/24", 100);
	assert(midr_ted_builder_add_prefix_group(builder, &mapping) == 0);

	assert(midr_ted_builder_publish(&ctx, builder, 0) == 0);
	midr_ted_builder_destroy(&builder);
	assert(midr_ted_snapshot_get(&ctx, &snapshot) == 0);
	assert(snapshot->intra_link_count == 1);
	assert(snapshot->intra_links[0].link_local_address.ipa_type == IPADDR_V6);
	assert(snapshot->intra_links[0].link_remote_address.ipa_type == IPADDR_V6);
	assert(snapshot->node_prefix_count == 2);
	assert(snapshot->prefix_group_count == 3);

	for (index = 0; index < snapshot->node_prefix_count; index++) {
		const struct midr_ted_prefix_key *key = &snapshot->node_prefixes[index].key;

		assert((key->afi == AFI_IP && prefix_same(&key->prefix, &ipv4_key.prefix)) ||
		       (key->afi == AFI_IP6 && prefix_same(&key->prefix, &ipv6_key.prefix)));
	}
	for (index = 0; index < snapshot->prefix_group_count; index++) {
		const struct midr_ted_prefix_group *row = &snapshot->prefix_groups[index];

		if (row->key.afi == AFI_IP6 &&
		    prefix_same(&row->key.prefix, &ipv6_key.prefix)) {
			assert(row->group_id == 100 || row->group_id == 200);
			ipv6_group_rows++;
		} else {
			assert(row->key.afi == AFI_IP);
			assert(prefix_same(&row->key.prefix, &ipv4_key.prefix));
			assert(row->group_id == 100);
		}
	}
	assert(ipv6_group_rows == 2);

	midr_ted_snapshot_release(&snapshot);
	midr_ted_context_finish(&ctx);
}

static void populate_minimal(struct midr_ted_builder *builder, uint32_t r1, uint32_t r2,
			     bool include_r2)
{
	struct midr_ted_node n = node(r1, 100);

	assert(midr_ted_builder_add_node(builder, &n) == 0);
	if (include_r2) {
		n = node(r2, 100);
		assert(midr_ted_builder_add_node(builder, &n) == 0);
	}
}

static void test_generation_consumer_and_lifetime(void)
{
	const uint32_t r1 = node_id("1.1.1.1");
	const uint32_t r2 = node_id("2.2.2.2");
	struct midr_context ctx = {};
	struct midr_context wrong_ctx = {};
	struct midr_ted_builder *builder = NULL;
	struct midr_ted_consumer *consumer = NULL;
	struct midr_ted_consumer_ops ops = {
		.snapshot_changed = snapshot_changed,
	};
	struct notification_state state = {
		.ctx = &ctx,
		.self = &consumer,
	};
	const struct midr_ted_snapshot *generation1 = NULL;
	const struct midr_ted_snapshot *generation2 = NULL;

	assert(midr_ted_context_init(&ctx) == 0);
	assert(midr_ted_context_init(&wrong_ctx) == 0);
	assert(midr_ted_consumer_register(&ctx, &ops, &state, &consumer) == 0);
	assert(state.count == 0);

	assert(midr_ted_builder_create(r1, 100, &builder) == 0);
	populate_minimal(builder, r1, r2, false);
	assert(midr_ted_builder_publish(&ctx, builder, 0) == 0);
	midr_ted_builder_destroy(&builder);
	assert(state.count == 1);
	assert(state.generation == 1);
	assert(state.change_flags == MIDR_TED_CHANGE_ALL);
	assert(midr_ted_snapshot_get(&ctx, &generation1) == 0);

	assert(midr_ted_builder_create(r1, 100, &builder) == 0);
	populate_minimal(builder, r1, r2, false);
	assert(midr_ted_builder_publish(&ctx, builder, 0) == 0);
	midr_ted_builder_destroy(&builder);
	assert(state.count == 1);
	assert(midr_ted_generation_is_current(&ctx, 1));

	assert(midr_ted_builder_create(r1, 100, &builder) == 0);
	populate_minimal(builder, r1, r2, true);
	assert(midr_ted_builder_publish(&ctx, builder, 0) == 0);
	midr_ted_builder_destroy(&builder);
	assert(state.count == 2);
	assert(state.generation == 2);
	assert(state.change_flags == MIDR_TED_CHANGE_NODES);
	assert(!midr_ted_generation_is_current(&ctx, 1));
	assert(midr_ted_generation_is_current(&ctx, 2));
	assert(midr_ted_snapshot_get(&ctx, &generation2) == 0);
	assert(generation1->generation == 1);
	assert(generation1->node_count == 1);
	assert(generation2->generation == 2);
	assert(generation2->node_count == 2);

	assert(midr_ted_builder_create(r1, 100, &builder) == 0);
	populate_minimal(builder, r1, r2, true);
	assert(midr_ted_builder_publish(&ctx, builder, MIDR_TED_SYNC_REASON_RESYNC_FAILED) == 0);
	midr_ted_builder_destroy(&builder);
	assert(state.count == 3);
	assert(state.generation == 3);
	assert(state.change_flags == MIDR_TED_CHANGE_SYNC);

	midr_ted_consumer_unregister(&wrong_ctx, &consumer);
	assert(consumer != NULL);
	midr_ted_consumer_unregister(&ctx, &consumer);
	assert(consumer == NULL);
	midr_ted_consumer_unregister(&ctx, &consumer);

	midr_ted_context_finish(&ctx);
	assert(generation1->generation == 1);
	assert(generation2->generation == 2);
	midr_ted_snapshot_release(&generation1);
	midr_ted_snapshot_release(&generation2);
	midr_ted_snapshot_release(&generation2);
	midr_ted_context_finish(&wrong_ctx);
}

static void test_consumer_self_unregister(void)
{
	const uint32_t r1 = node_id("1.1.1.1");
	const uint32_t r2 = node_id("2.2.2.2");
	struct midr_context ctx = {};
	struct midr_ted_builder *builder = NULL;
	struct midr_ted_consumer *consumer = NULL;
	struct midr_ted_consumer_ops ops = {
		.snapshot_changed = snapshot_changed,
	};
	struct notification_state state = {
		.ctx = &ctx,
		.self = &consumer,
		.unregister_self = true,
	};
	struct midr_ted_status status;

	assert(midr_ted_context_init(&ctx) == 0);
	assert(midr_ted_consumer_register(NULL, &ops, &state, &consumer) == -ENOENT);
	assert(midr_ted_consumer_register(&ctx, NULL, &state, &consumer) == -EINVAL);
	ops.snapshot_changed = NULL;
	assert(midr_ted_consumer_register(&ctx, &ops, &state, &consumer) == -EINVAL);
	ops.snapshot_changed = snapshot_changed;
	assert(midr_ted_consumer_register(&ctx, &ops, &state, NULL) == -EINVAL);
	assert(midr_ted_consumer_register(&ctx, &ops, &state, &consumer) == 0);
	assert(midr_ted_consumer_register(&ctx, &ops, &state, &consumer) == -EINVAL);

	assert(midr_ted_builder_create(r1, 100, &builder) == 0);
	populate_minimal(builder, r1, r2, false);
	assert(midr_ted_builder_publish(&ctx, builder, 0) == 0);
	midr_ted_builder_destroy(&builder);
	assert(state.count == 1);
	assert(consumer == NULL);
	assert(midr_ted_status_get(&ctx, &status) == 0);
	assert(status.consumer_count == 0);

	assert(midr_ted_builder_create(r1, 100, &builder) == 0);
	populate_minimal(builder, r1, r2, true);
	assert(midr_ted_builder_publish(&ctx, builder, 0) == 0);
	midr_ted_builder_destroy(&builder);
	assert(state.count == 1);

	midr_ted_context_finish(&ctx);
}

static void test_group_edge_reselection_and_removal(void)
{
	const uint32_t r1 = node_id("1.1.1.1");
	const uint32_t r2 = node_id("2.2.2.2");
	const uint32_t r3 = node_id("3.3.3.3");
	struct midr_context ctx = {};
	struct midr_ted_builder *builder = NULL;
	struct midr_ted_node nodes[] = {
		node(r1, 100),
		node(r2, 100),
		node(r3, 200),
	};
	struct midr_ted_link_input expensive = link_input(r1, r3, 1, 30);
	struct midr_ted_link_input cheap = link_input(r2, r3, 2, 20);
	const struct midr_ted_snapshot *snapshot = NULL;

	assert(midr_ted_context_init(&ctx) == 0);

	assert(midr_ted_builder_create(r1, 100, &builder) == 0);
	add_nodes(builder, nodes, array_size(nodes), false);
	expensive.local_ifindex = 4;
	assert(midr_ted_builder_add_link(builder, &expensive) == 0);
	assert(midr_ted_builder_add_link(builder, &cheap) == 0);
	assert(midr_ted_builder_publish(&ctx, builder, 0) == 0);
	midr_ted_builder_destroy(&builder);
	assert(midr_ted_snapshot_get(&ctx, &snapshot) == 0);
	assert(snapshot->group_edge_count == 1);
	assert(snapshot->group_edges[0].aggregate_cost == 20);
	midr_ted_snapshot_release(&snapshot);

	assert(midr_ted_builder_create(r1, 100, &builder) == 0);
	add_nodes(builder, nodes, array_size(nodes), false);
	assert(midr_ted_builder_add_link(builder, &expensive) == 0);
	assert(midr_ted_builder_publish(&ctx, builder, 0) == 0);
	midr_ted_builder_destroy(&builder);
	assert(midr_ted_snapshot_get(&ctx, &snapshot) == 0);
	assert(snapshot->generation == 2);
	assert(snapshot->group_edge_count == 1);
	assert(snapshot->group_edges[0].aggregate_cost == 30);
	midr_ted_snapshot_release(&snapshot);

	assert(midr_ted_builder_create(r1, 100, &builder) == 0);
	add_nodes(builder, nodes, array_size(nodes), false);
	assert(midr_ted_builder_publish(&ctx, builder, 0) == 0);
	midr_ted_builder_destroy(&builder);
	assert(midr_ted_snapshot_get(&ctx, &snapshot) == 0);
	assert(snapshot->generation == 3);
	assert(snapshot->egress_link_count == 0);
	assert(snapshot->group_edge_count == 0);
	midr_ted_snapshot_release(&snapshot);

	midr_ted_context_finish(&ctx);
}

static void test_public_path_consumer_stub(void)
{
	const uint32_t r1 = node_id("1.1.1.1");
	const uint32_t r2 = node_id("2.2.2.2");
	struct midr_context ctx = {};
	struct midr_ted_builder *builder = NULL;
	struct midr_ted_path_consumer_stub consumer = {};
	const struct midr_ted_snapshot *snapshot = NULL;
	uint64_t first_generation;

	assert(midr_ted_context_init(&ctx) == 0);
	assert(midr_ted_builder_create(r1, 100, &builder) == 0);
	populate_minimal(builder, r1, r2, false);
	assert(midr_ted_builder_publish(&ctx, builder, 0) == 0);
	midr_ted_builder_destroy(&builder);

	assert(midr_ted_path_consumer_stub_start(&ctx, &consumer) == 0);
	assert(consumer.notification_count == 0);
	assert(midr_ted_path_consumer_stub_snapshot_get(&ctx, &consumer, &snapshot) == 0);
	first_generation = snapshot->generation;
	midr_ted_snapshot_release(&snapshot);
	assert(midr_ted_path_consumer_stub_result_is_current(&ctx, &consumer, first_generation));

	assert(midr_ted_builder_create(r1, 100, &builder) == 0);
	populate_minimal(builder, r1, r2, true);
	assert(midr_ted_builder_publish(&ctx, builder, 0) == 0);
	midr_ted_builder_destroy(&builder);
	assert(consumer.notification_count == 1);
	assert(consumer.last_generation == first_generation + 1);
	assert(consumer.last_change_flags == MIDR_TED_CHANGE_NODES);
	assert(!midr_ted_path_consumer_stub_result_is_current(&ctx, &consumer, first_generation));

	midr_ted_path_consumer_stub_stop(&ctx, &consumer);
	assert(consumer.registration == NULL);
	assert(midr_ted_path_consumer_stub_snapshot_get(&ctx, &consumer, &snapshot) == -ENOENT);
	midr_ted_context_finish(&ctx);
}

static void test_pending_diagnostics_without_generation_change(void)
{
	const uint32_t r1 = node_id("1.1.1.1");
	const uint32_t missing = node_id("9.9.9.9");
	struct midr_context ctx = {};
	struct midr_ted_builder *builder = NULL;
	struct midr_ted_link_input pending = link_input(r1, missing, 1, 10);
	struct midr_ted_status status;
	const struct midr_ted_snapshot *snapshot = NULL;

	pending.local_ifindex = 1;
	assert(midr_ted_context_init(&ctx) == 0);
	assert(midr_ted_builder_create(r1, 100, &builder) == 0);
	assert(midr_ted_builder_add_node(builder, &(struct midr_ted_node){
							  .node_id = r1,
							  .group_id = 100,
						  }) == 0);
	assert(midr_ted_builder_publish(&ctx, builder, 0) == 0);
	midr_ted_builder_destroy(&builder);

	assert(midr_ted_builder_create(r1, 100, &builder) == 0);
	assert(midr_ted_builder_add_node(builder, &(struct midr_ted_node){
							  .node_id = r1,
							  .group_id = 100,
						  }) == 0);
	assert(midr_ted_builder_add_link(builder, &pending) == 0);
	assert(midr_ted_builder_publish(&ctx, builder, 0) == 0);
	midr_ted_builder_destroy(&builder);

	assert(midr_ted_snapshot_get(&ctx, &snapshot) == 0);
	assert(snapshot->generation == 1);
	assert(snapshot->egress_link_count == 0);
	midr_ted_snapshot_release(&snapshot);
	assert(midr_ted_status_get(&ctx, &status) == 0);
	assert(status.pending_link_count == 1);

	midr_ted_context_finish(&ctx);
}

static void test_publish_validation_and_generation_overflow(void)
{
	const uint32_t r1 = node_id("1.1.1.1");
	const uint32_t r2 = node_id("2.2.2.2");
	struct midr_context ctx = {};
	struct midr_ted_builder *builder = NULL;
	struct midr_ted_node n;
	const struct midr_ted_snapshot *snapshot = NULL;

	assert(midr_ted_builder_publish(NULL, NULL, 0) == -ENOENT);
	assert(midr_ted_context_init(&ctx) == 0);
	assert(midr_ted_builder_publish(&ctx, NULL, 0) == -EINVAL);

	assert(midr_ted_builder_create(r1, 100, &builder) == 0);
	n = node(r1, 200);
	assert(midr_ted_builder_add_node(builder, &n) == 0);
	assert(midr_ted_builder_publish(&ctx, builder, 0) == -EINVAL);
	midr_ted_builder_destroy(&builder);

	assert(midr_ted_builder_create(r1, 100, &builder) == 0);
	n = node(r1, 100);
	assert(midr_ted_builder_add_node(builder, &n) == 0);
	assert(midr_ted_builder_publish(&ctx, builder, UINT64_C(1) << 63) == -EINVAL);
	assert(midr_ted_builder_publish(&ctx, builder, 0) == 0);
	midr_ted_builder_destroy(&builder);

	assert(midr_ted_test_generation_set(NULL, 1) == -ENOENT);
	assert(midr_ted_test_generation_set(&ctx, UINT64_MAX) == 0);

	assert(midr_ted_builder_create(r1, 100, &builder) == 0);
	populate_minimal(builder, r1, r2, true);
	assert(midr_ted_builder_publish(&ctx, builder, 0) == -EOVERFLOW);
	midr_ted_builder_destroy(&builder);

	assert(midr_ted_snapshot_get(&ctx, &snapshot) == 0);
	assert(snapshot->generation == UINT64_MAX);
	assert(snapshot->node_count == 1);
	assert(midr_ted_generation_is_current(&ctx, UINT64_MAX));
	midr_ted_snapshot_release(&snapshot);

	midr_ted_context_finish(&ctx);
}

int main(void)
{
	test_not_ready_and_context_validation();
	test_builder_input_validation();
	test_snapshot_derivation_and_stable_order();
	test_ipv6_snapshot_and_address_family_isolation();
	test_generation_consumer_and_lifetime();
	test_consumer_self_unregister();
	test_group_edge_reselection_and_removal();
	test_public_path_consumer_stub();
	test_pending_diagnostics_without_generation_change();
	test_publish_validation_and_generation_overflow();
	printf("MIDR TED snapshot tests passed\n");
	return 0;
}
