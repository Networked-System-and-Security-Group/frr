// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR Local Fact, topology snapshot, and resynchronization tests.
 */

#include <zebra.h>

#include <errno.h>

#include "bgpd/bgpd.h"
#include "bgpd/bgp_midr_private.h"

/* Required variables when linking against bgpd/libbgp.a. */
struct zebra_privs_t bgpd_privs = {};
struct event_loop *master;

struct provider_fixture {
	struct midr_node_update nodes[2];
	struct midr_link_update links[2];
	size_t node_count;
	size_t link_count;
	uint64_t snapshot_version;
	int result;
	bool override_nodes;
	bool override_links;
	const struct midr_node_update *nodes_override;
	const struct midr_link_update *links_override;
	void (*during_get)(struct midr_context *ctx);
	uint64_t get_count;
	uint64_t release_count;
};

struct test_env {
	struct bgp bgp;
	struct bgp_midr midr;
	struct midr_context *ctx;
};

static struct provider_fixture provider;
static uint32_t callback_owner;

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

static struct midr_node_update node_update(uint32_t owner, uint32_t group_id, uint64_t version)
{
	struct midr_node_update node = {
		.node_id = owner,
		.group_id = group_id,
		.policy_state = MIDR_POLICY_ALLOWED,
		.version = version,
	};

	SET_IPADDR_NONE(&node.transport_address);
	return node;
}

static struct midr_link_update link_update(uint32_t owner, uint32_t remote, uint64_t link_id,
					   uint64_t version)
{
	return (struct midr_link_update){
		.key =
			{
				.local_node_id = owner,
				.remote_node_id = remote,
				.link_id = link_id,
			},
		.link_local_address = ip_address("192.0.2.1"),
		.link_remote_address = ip_address("192.0.2.2"),
		.metrics =
			{
				.has_rtt_us = true,
				.rtt_us = 100,
				.has_loss_ppm = true,
				.loss_ppm = 10,
				.has_available_bandwidth_kbps = true,
				.available_bandwidth_kbps = 100000,
			},
		.policy_state = MIDR_POLICY_ALLOWED,
		.version = version,
	};
}

static void provider_reset(uint32_t owner, uint32_t group_id, uint64_t object_version,
			   uint64_t snapshot_version)
{
	memset(&provider, 0, sizeof(provider));
	provider.nodes[0] = node_update(owner, group_id, object_version);
	provider.node_count = 1;
	provider.snapshot_version = snapshot_version;
}

static int test_snapshot_get(struct midr_context *ctx,
			     struct midr_topology_snapshot *snapshot)
{
	void (*during_get)(struct midr_context *ctx);

	assert(ctx != NULL);
	assert(snapshot != NULL);
	provider.get_count++;
	memset(snapshot, 0, sizeof(*snapshot));
	if (provider.result)
		return provider.result;

	snapshot->nodes = provider.override_nodes ? provider.nodes_override
						  : (provider.node_count ? provider.nodes : NULL);
	snapshot->node_count = provider.node_count;
	snapshot->links = provider.override_links ? provider.links_override
						  : (provider.link_count ? provider.links : NULL);
	snapshot->link_count = provider.link_count;
	snapshot->snapshot_version = provider.snapshot_version;

	during_get = provider.during_get;
	provider.during_get = NULL;
	if (during_get)
		during_get(ctx);
	return 0;
}

static void test_snapshot_release(struct midr_context *ctx,
				  struct midr_topology_snapshot *snapshot)
{
	assert(ctx != NULL);
	assert(snapshot != NULL);
	provider.release_count++;
	memset(snapshot, 0, sizeof(*snapshot));
}

static struct midr_input_status input_status(struct midr_context *ctx)
{
	struct midr_input_status status;

	assert(midr_input_status_get(ctx, &status) == 0);
	return status;
}

static struct midr_node_update get_node(struct midr_context *ctx, uint32_t owner, bool active)
{
	struct midr_node_update node;
	bool actual_active;

	assert(midr_local_fact_node_get(ctx, owner, &node, &actual_active) == 0);
	assert(actual_active == active);
	return node;
}

static struct midr_link_update get_link(struct midr_context *ctx, const struct midr_link_key *key,
					bool active)
{
	struct midr_link_update link;
	bool actual_active;

	assert(midr_local_fact_link_get(ctx, key, &link, &actual_active) == 0);
	assert(actual_active == active);
	return link;
}

static void env_init(struct test_env *env, uint32_t owner)
{
	struct midr_input_status status;

	memset(env, 0, sizeof(*env));
	env->bgp.router_id.s_addr = owner;
	env->midr.bgp = &env->bgp;
	env->midr.ctx.bgp = &env->bgp;
	env->midr.ctx.midr = &env->midr;
	env->bgp.midr_info = &env->midr;
	env->ctx = &env->midr.ctx;

	assert(midr_input_init(env->ctx) == 0);
	assert(midr_input_init(env->ctx) == -EALREADY);
	assert(midr_input_test_set_snapshot_provider(
		       env->ctx, test_snapshot_get, test_snapshot_release) == 0);
	status = input_status(env->ctx);
	assert(status.state == MIDR_INPUT_RESYNCING);
	midr_input_test_resync_now(env->ctx);
	status = input_status(env->ctx);
	assert(status.state == MIDR_INPUT_NORMAL);
	assert(status.provider_available);
	assert(status.resync_commits == 1);
}

static void env_finish(struct test_env *env)
{
	midr_input_finish(env->ctx);
	assert(env->ctx->input_store == NULL);
	env->bgp.midr_info = NULL;
}

static void callback_node_v2(struct midr_context *ctx)
{
	struct midr_node_update node = node_update(callback_owner, 30, 2);

	assert(midr_topology_node_upsert(ctx, &node) == 0);
}

static void callback_node_withdraw_v2(struct midr_context *ctx)
{
	assert(midr_topology_node_withdraw(ctx, callback_owner, 2) == 0);
}

static void callback_overflow_resync_queue(struct midr_context *ctx)
{
	struct midr_node_update node = node_update(callback_owner, 20, 2);

	assert(midr_topology_node_upsert(ctx, &node) == 0);
	node.group_id = 30;
	node.version = 3;
	assert(midr_topology_node_upsert(ctx, &node) == -ENOSPC);
}

static void test_fifo_version_and_tombstone(void)
{
	const uint32_t owner = router_id("1.1.1.1");
	const uint32_t remote = router_id("2.2.2.2");
	struct midr_link_update link = link_update(owner, remote, 7, 4);
	struct midr_link_update active_link = link_update(owner, remote, 8, 1);
	struct midr_node_update node;
	struct midr_input_status status;
	struct test_env env;

	provider_reset(owner, 10, 1, 100);
	env_init(&env, owner);

	node = node_update(owner, 20, 2);
	assert(midr_topology_node_upsert(env.ctx, &node) == 0);
	node.group_id = 30;
	node.version = 3;
	assert(midr_topology_node_upsert(env.ctx, &node) == 0);
	midr_topology_process_pending(env.ctx);
	node = get_node(env.ctx, owner, true);
	assert(node.group_id == 30);
	assert(node.version == 3);

	assert(midr_topology_node_withdraw(env.ctx, owner, 4) == 0);
	midr_topology_process_pending(env.ctx);
	node = get_node(env.ctx, owner, false);
	assert(node.version == 4);

	node = node_update(owner, 40, 3);
	assert(midr_topology_node_upsert(env.ctx, &node) == 0);
	midr_topology_process_pending(env.ctx);
	node = get_node(env.ctx, owner, false);
	assert(node.version == 4);

	node = node_update(owner, 50, 5);
	assert(midr_topology_node_upsert(env.ctx, &node) == 0);
	midr_topology_process_pending(env.ctx);
	node = get_node(env.ctx, owner, true);
	assert(node.group_id == 50);
	assert(node.version == 5);

	assert(midr_topology_link_withdraw(env.ctx, &link.key, 5) == 0);
	assert(midr_topology_link_upsert(env.ctx, &link) == 0);
	assert(midr_topology_link_withdraw(env.ctx, &link.key, 3) == 0);
	midr_topology_process_pending(env.ctx);
	link = get_link(env.ctx, &link.key, false);
	assert(link.version == 5);

	assert(midr_topology_link_upsert(env.ctx, &active_link) == 0);
	active_link.version = 2;
	assert(midr_topology_link_upsert(env.ctx, &active_link) == 0);
	assert(midr_topology_link_withdraw(env.ctx, &active_link.key, 3) == 0);
	midr_topology_process_pending(env.ctx);
	active_link = get_link(env.ctx, &active_link.key, false);
	assert(active_link.version == 3);

	status = input_status(env.ctx);
	assert(status.event_enqueued == 11);
	assert(status.event_processed == 11);
	assert(status.event_ignored_old == 3);
	assert(status.active_node_count == 1);
	assert(status.link_tombstone_count == 2);
	env_finish(&env);
}

static void test_normal_queue_overflow_and_recovery(void)
{
	const uint32_t owner = router_id("1.1.1.1");
	struct midr_node_update node;
	struct midr_input_status status;
	struct test_env env;

	provider_reset(owner, 10, 1, 100);
	env_init(&env, owner);
	assert(midr_input_test_set_queue_limits(env.ctx, 2, 2) == 0);

	node = node_update(owner, 20, 2);
	assert(midr_topology_node_upsert(env.ctx, &node) == 0);
	node.group_id = 30;
	node.version = 3;
	assert(midr_topology_node_upsert(env.ctx, &node) == 0);
	assert(midr_input_test_set_queue_limits(env.ctx, 1, 2) == -EBUSY);
	node.group_id = 40;
	node.version = 4;
	assert(midr_topology_node_upsert(env.ctx, &node) == -ENOSPC);

	status = input_status(env.ctx);
	assert(status.state == MIDR_INPUT_OUT_OF_SYNC);
	assert(status.reason == MIDR_TOPOLOGY_RESYNC_STATE_INCONSISTENT);
	assert(status.normal_queue_count == 0);
	assert(status.event_rejected_full == 1);
	assert(status.event_dropped_resync == 2);
	assert(status.queue_overflows == 1);
	node = get_node(env.ctx, owner, true);
	assert(node.group_id == 10);
	assert(node.version == 1);

	node.group_id = 50;
	node.version = 5;
	assert(midr_topology_node_upsert(env.ctx, &node) == -EAGAIN);
	assert(midr_topology_resync_begin(env.ctx, MIDR_TOPOLOGY_RESYNC_STATE_INCONSISTENT) == 0);
	provider.nodes[0] = node_update(owner, 60, 1);
	provider.snapshot_version = 101;
	midr_input_test_resync_now(env.ctx);
	node = get_node(env.ctx, owner, true);
	assert(node.group_id == 60);
	assert(node.version == 1);
	status = input_status(env.ctx);
	assert(status.state == MIDR_INPUT_NORMAL);
	assert(status.snapshot_version == 101);
	assert(status.resync_commits == 2);
	env_finish(&env);
}

static void test_snapshot_barrier_and_atomic_replay(void)
{
	const uint32_t owner = router_id("1.1.1.1");
	struct midr_node_update node;
	struct midr_input_status status;
	struct test_env env;

	provider_reset(owner, 10, 10, 100);
	env_init(&env, owner);

	node = node_update(owner, 11, 11);
	assert(midr_topology_node_upsert(env.ctx, &node) == 0);
	provider.nodes[0] = node_update(owner, 20, 1);
	provider.snapshot_version = 200;
	callback_owner = owner;
	provider.during_get = callback_node_v2;
	assert(midr_topology_resync_begin(env.ctx, MIDR_TOPOLOGY_RESYNC_VERSION_LOST) == 0);
	status = input_status(env.ctx);
	assert(status.normal_queue_count == 1);
	assert(status.resync_queue_count == 0);
	assert(midr_topology_resync_begin(env.ctx, MIDR_TOPOLOGY_RESYNC_VERSION_LOST) == -EBUSY);
	midr_topology_process_pending(env.ctx);
	assert(input_status(env.ctx).normal_queue_count == 0);

	midr_input_test_resync_now(env.ctx);
	node = get_node(env.ctx, owner, true);
	assert(node.group_id == 30);
	assert(node.version == 2);
	status = input_status(env.ctx);
	assert(status.state == MIDR_INPUT_NORMAL);
	assert(status.normal_queue_count == 0);
	assert(status.resync_queue_count == 0);
	assert(status.snapshot_version == 200);
	env_finish(&env);
}

static void test_snapshot_withdraw_and_lower_baseline(void)
{
	const uint32_t owner = router_id("1.1.1.1");
	struct midr_node_update node;
	struct midr_input_status status;
	struct test_env env;

	provider_reset(owner, 10, 100, 100);
	env_init(&env, owner);
	assert(midr_topology_node_withdraw(env.ctx, owner, 101) == 0);
	midr_topology_process_pending(env.ctx);
	node = get_node(env.ctx, owner, false);
	assert(node.version == 101);

	provider.nodes[0] = node_update(owner, 20, 1);
	provider.links[0] = link_update(owner, router_id("2.2.2.2"), 7, 1);
	provider.link_count = 1;
	provider.snapshot_version = 200;
	assert(midr_topology_resync_begin(env.ctx, MIDR_TOPOLOGY_RESYNC_PROVIDER_RESTART) == 0);
	midr_input_test_resync_now(env.ctx);
	node = get_node(env.ctx, owner, true);
	assert(node.group_id == 20);
	assert(node.version == 1);
	assert(get_link(env.ctx, &provider.links[0].key, true).version == 1);

	provider.nodes[0] = node_update(owner, 30, 1);
	provider.link_count = 0;
	provider.snapshot_version = 201;
	callback_owner = owner;
	provider.during_get = callback_node_withdraw_v2;
	assert(midr_topology_resync_begin(env.ctx, MIDR_TOPOLOGY_RESYNC_VERSION_LOST) == 0);
	midr_input_test_resync_now(env.ctx);
	node = get_node(env.ctx, owner, false);
	assert(node.version == 2);

	provider.node_count = 0;
	provider.snapshot_version = 202;
	assert(midr_topology_resync_begin(env.ctx, MIDR_TOPOLOGY_RESYNC_VERSION_LOST) == 0);
	midr_input_test_resync_now(env.ctx);
	assert(midr_local_fact_node_get(env.ctx, owner, &node, &(bool){ false }) == -ENOENT);
	status = input_status(env.ctx);
	assert(status.active_node_count == 0);
	assert(status.node_tombstone_count == 0);
	assert(status.snapshot_version == 202);

	assert(midr_topology_node_withdraw(env.ctx, owner, 3) == 0);
	assert(midr_topology_node_withdraw(env.ctx, owner, 2) == 0);
	midr_topology_process_pending(env.ctx);
	node = get_node(env.ctx, owner, false);
	assert(node.version == 3);
	assert(input_status(env.ctx).event_ignored_old >= 1);
	env_finish(&env);
}

static void test_provider_retry_and_queue_preservation(void)
{
	const uint32_t owner = router_id("1.1.1.1");
	struct midr_node_update node;
	struct midr_input_status status;
	struct test_env env;
	uint64_t releases;

	provider_reset(owner, 10, 1, 100);
	env_init(&env, owner);
	releases = provider.release_count;

	provider.result = -EAGAIN;
	assert(midr_topology_resync_begin(env.ctx, MIDR_TOPOLOGY_RESYNC_VERSION_LOST) == 0);
	node = node_update(owner, 20, 2);
	assert(midr_topology_node_upsert(env.ctx, &node) == 0);
	midr_input_test_resync_now(env.ctx);
	status = input_status(env.ctx);
	assert(status.state == MIDR_INPUT_RESYNCING);
	assert(status.resync_queue_count == 1);
	assert(status.resync_failures == 1);
	assert(provider.release_count == releases);
	node = get_node(env.ctx, owner, true);
	assert(node.group_id == 10);

	provider.result = 0;
	provider.nodes[0] = node_update(owner, 15, 1);
	provider.snapshot_version = 101;
	midr_input_test_resync_now(env.ctx);
	node = get_node(env.ctx, owner, true);
	assert(node.group_id == 20);
	assert(node.version == 2);
	status = input_status(env.ctx);
	assert(status.state == MIDR_INPUT_NORMAL);
	assert(status.resync_queue_count == 0);
	assert(status.resync_commits == 2);
	assert(provider.release_count == releases + 1);
	env_finish(&env);
}

static void test_invalid_snapshot_rollback(void)
{
	const uint32_t owner = router_id("1.1.1.1");
	const uint32_t other = router_id("2.2.2.2");
	struct midr_node_update node;
	struct midr_input_status status;
	struct test_env env;
	uint64_t failures;
	uint64_t releases;

	provider_reset(owner, 10, 1, 100);
	env_init(&env, owner);
	assert(midr_topology_resync_begin(env.ctx, MIDR_TOPOLOGY_RESYNC_VERSION_LOST) == 0);
	failures = input_status(env.ctx).resync_failures;
	releases = provider.release_count;

	provider.nodes[1] = node_update(owner, 20, 2);
	provider.node_count = 2;
	midr_input_test_resync_now(env.ctx);
	assert(input_status(env.ctx).state == MIDR_INPUT_RESYNCING);

	provider.node_count = 1;
	provider.override_nodes = true;
	provider.nodes_override = NULL;
	midr_input_test_resync_now(env.ctx);
	assert(input_status(env.ctx).state == MIDR_INPUT_RESYNCING);

	provider.override_nodes = false;
	provider.node_count = 0;
	provider.override_nodes = true;
	provider.nodes_override = provider.nodes;
	midr_input_test_resync_now(env.ctx);
	assert(input_status(env.ctx).state == MIDR_INPUT_RESYNCING);

	provider.override_nodes = false;
	provider.node_count = 1;
	provider.link_count = 1;
	provider.override_links = true;
	provider.links_override = NULL;
	midr_input_test_resync_now(env.ctx);
	assert(input_status(env.ctx).state == MIDR_INPUT_RESYNCING);

	provider.override_links = false;
	provider.nodes[0] = node_update(owner, 10, 1);
	provider.links[0] = link_update(owner, other, 7, 1);
	provider.links[1] = provider.links[0];
	provider.link_count = 2;
	midr_input_test_resync_now(env.ctx);
	assert(input_status(env.ctx).state == MIDR_INPUT_RESYNCING);

	provider.link_count = 0;
	provider.nodes[0] = node_update(other, 10, 1);
	midr_input_test_resync_now(env.ctx);
	assert(input_status(env.ctx).state == MIDR_INPUT_RESYNCING);

	provider.nodes[0] = node_update(owner, 10, 1);
	provider.links[0] = link_update(owner, other, 8, 1);
	provider.links[0].metrics.rtt_us = 0;
	provider.link_count = 1;
	midr_input_test_resync_now(env.ctx);
	assert(input_status(env.ctx).state == MIDR_INPUT_RESYNCING);

	node = get_node(env.ctx, owner, true);
	assert(node.group_id == 10);
	assert(node.version == 1);
	status = input_status(env.ctx);
	assert(status.resync_failures == failures + 7);
	assert(provider.release_count == releases + 7);

	provider.link_count = 0;
	provider.nodes[0] = node_update(owner, 20, 2);
	provider.snapshot_version = 200;
	midr_input_test_resync_now(env.ctx);
	node = get_node(env.ctx, owner, true);
	assert(node.group_id == 20);
	assert(node.version == 2);
	status = input_status(env.ctx);
	assert(status.state == MIDR_INPUT_NORMAL);
	assert(status.snapshot_version == 200);
	assert(provider.release_count == releases + 8);
	env_finish(&env);
}

static void test_provider_unavailable_paths(void)
{
	const uint32_t owner = router_id("1.1.1.1");
	const uint32_t new_owner = router_id("9.9.9.9");
	struct midr_node_update node;
	struct midr_input_status status;
	struct test_env env;

	provider_reset(owner, 10, 1, 100);
	provider.result = -ENOSYS;
	memset(&env, 0, sizeof(env));
	env.bgp.router_id.s_addr = owner;
	env.midr.bgp = &env.bgp;
	env.midr.ctx.bgp = &env.bgp;
	env.midr.ctx.midr = &env.midr;
	env.bgp.midr_info = &env.midr;
	env.ctx = &env.midr.ctx;
	assert(midr_input_init(env.ctx) == 0);
	assert(midr_input_test_set_snapshot_provider(
		       env.ctx, test_snapshot_get, test_snapshot_release) == 0);
	midr_input_test_resync_now(env.ctx);
	status = input_status(env.ctx);
	assert(status.state == MIDR_INPUT_NORMAL);
	assert(!status.provider_available);
	assert(status.resync_commits == 0);
	assert(midr_topology_resync_begin(env.ctx, MIDR_TOPOLOGY_RESYNC_VERSION_LOST) == -EAGAIN);
	env_finish(&env);

	provider_reset(owner, 10, 1, 100);
	env_init(&env, owner);
	assert(midr_topology_resync_begin(env.ctx, MIDR_TOPOLOGY_RESYNC_PROVIDER_RESTART) == 0);
	provider.result = -ENOSYS;
	midr_input_test_resync_now(env.ctx);
	status = input_status(env.ctx);
	assert(status.state == MIDR_INPUT_OUT_OF_SYNC);
	assert(!status.provider_available);
	assert(status.resync_failures == 1);
	assert(midr_input_router_id_update(&env.bgp, true) == 0);
	env.bgp.router_id.s_addr = new_owner;
	assert(midr_input_router_id_update(&env.bgp, false) == 0);
	status = input_status(env.ctx);
	assert(status.state == MIDR_INPUT_IDENTITY_RESTART);
	assert(status.owner_node_id == new_owner);
	node = node_update(new_owner, 20, 1);
	assert(midr_topology_node_upsert(env.ctx, &node) == -EAGAIN);
	env_finish(&env);
}

static void test_resync_queue_overflow_is_atomic(void)
{
	const uint32_t owner = router_id("1.1.1.1");
	struct midr_node_update node;
	struct midr_input_status status;
	struct test_env env;
	uint64_t releases;

	provider_reset(owner, 10, 1, 100);
	env_init(&env, owner);
	assert(midr_input_test_set_queue_limits(env.ctx, 2, 1) == 0);

	provider.nodes[0] = node_update(owner, 50, 1);
	provider.snapshot_version = 200;
	callback_owner = owner;
	provider.during_get = callback_overflow_resync_queue;
	releases = provider.release_count;
	assert(midr_topology_resync_begin(env.ctx, MIDR_TOPOLOGY_RESYNC_VERSION_LOST) == 0);
	midr_input_test_resync_now(env.ctx);

	status = input_status(env.ctx);
	assert(status.state == MIDR_INPUT_OUT_OF_SYNC);
	assert(status.resync_queue_count == 0);
	assert(status.event_rejected_full == 1);
	assert(status.event_dropped_resync == 1);
	assert(status.queue_overflows == 1);
	assert(status.resync_commits == 1);
	assert(provider.release_count == releases + 1);
	node = get_node(env.ctx, owner, true);
	assert(node.group_id == 10);
	assert(node.version == 1);

	assert(midr_input_test_set_queue_limits(env.ctx, 2, 2) == 0);
	provider.nodes[0] = node_update(owner, 60, 1);
	provider.snapshot_version = 201;
	assert(midr_topology_resync_begin(env.ctx, MIDR_TOPOLOGY_RESYNC_STATE_INCONSISTENT) == 0);
	midr_input_test_resync_now(env.ctx);
	node = get_node(env.ctx, owner, true);
	assert(node.group_id == 60);
	status = input_status(env.ctx);
	assert(status.state == MIDR_INPUT_NORMAL);
	assert(status.resync_commits == 2);
	env_finish(&env);
}

static void test_router_id_identity_restart(void)
{
	const uint32_t old_owner = router_id("1.1.1.1");
	const uint32_t new_owner = router_id("9.9.9.9");
	const uint32_t remote = router_id("2.2.2.2");
	struct midr_link_update link = link_update(old_owner, remote, 7, 101);
	struct midr_node_update node;
	struct midr_input_status status;
	struct test_env env;

	provider_reset(old_owner, 10, 100, 100);
	env_init(&env, old_owner);
	assert(midr_topology_link_withdraw(env.ctx, &link.key, 101) == 0);
	midr_topology_process_pending(env.ctx);
	assert(input_status(env.ctx).link_tombstone_count == 1);

	assert(midr_input_router_id_update(&env.bgp, true) == 0);
	status = input_status(env.ctx);
	assert(status.state == MIDR_INPUT_IDENTITY_RESTART);
	assert(status.owner_node_id == 0);
	assert(status.active_node_count == 0);
	assert(status.link_tombstone_count == 0);
	assert(status.identity_restarts == 1);

	node = node_update(old_owner, 20, 102);
	assert(midr_topology_node_upsert(env.ctx, &node) == -EAGAIN);
	env.bgp.router_id.s_addr = 0;
	assert(midr_topology_node_upsert(env.ctx, &node) == -ENOENT);

	env.bgp.router_id.s_addr = new_owner;
	provider_reset(new_owner, 30, 1, 1);
	assert(midr_input_router_id_update(&env.bgp, false) == 0);
	status = input_status(env.ctx);
	assert(status.state == MIDR_INPUT_RESYNCING);
	assert(status.owner_node_id == new_owner);

	node = node_update(new_owner, 40, 2);
	assert(midr_topology_node_upsert(env.ctx, &node) == 0);
	midr_input_test_resync_now(env.ctx);
	node = get_node(env.ctx, new_owner, true);
	assert(node.group_id == 40);
	assert(node.version == 2);
	assert(midr_local_fact_node_get(env.ctx, old_owner, &node, &(bool){ false }) == -ENOENT);
	status = input_status(env.ctx);
	assert(status.state == MIDR_INPUT_NORMAL);
	assert(status.owner_node_id == new_owner);
	assert(status.snapshot_version == 1);
	env_finish(&env);
}

static void test_router_id_initial_establishment(void)
{
	const uint32_t owner = router_id("9.9.9.9");
	struct midr_node_update node;
	struct midr_input_status status;
	struct test_env env;

	provider_reset(owner, 30, 1, 1);
	memset(&env, 0, sizeof(env));
	env.midr.bgp = &env.bgp;
	env.midr.ctx.bgp = &env.bgp;
	env.midr.ctx.midr = &env.midr;
	env.bgp.midr_info = &env.midr;
	env.ctx = &env.midr.ctx;
	assert(midr_input_init(env.ctx) == 0);
	assert(midr_input_test_set_snapshot_provider(
		       env.ctx, test_snapshot_get, test_snapshot_release) == 0);
	assert(input_status(env.ctx).state == MIDR_INPUT_IDENTITY_RESTART);

	env.bgp.router_id.s_addr = owner;
	assert(midr_input_router_id_update(&env.bgp, false) == 0);
	status = input_status(env.ctx);
	assert(status.state == MIDR_INPUT_RESYNCING);
	assert(status.owner_node_id == owner);

	node = node_update(owner, 40, 2);
	assert(midr_topology_node_upsert(env.ctx, &node) == 0);
	midr_input_test_resync_now(env.ctx);
	node = get_node(env.ctx, owner, true);
	assert(node.group_id == 40);
	assert(node.version == 2);
	status = input_status(env.ctx);
	assert(status.state == MIDR_INPUT_NORMAL);
	assert(status.snapshot_version == 1);
	assert(status.identity_restarts == 0);
	env_finish(&env);
}

static void test_finish_cancels_pending_work(void)
{
	const uint32_t owner = router_id("1.1.1.1");
	struct midr_node_update node;
	struct test_env env;

	provider_reset(owner, 10, 1, 100);
	env_init(&env, owner);
	node = node_update(owner, 20, 2);
	assert(midr_topology_node_upsert(env.ctx, &node) == 0);
	assert(midr_topology_resync_begin(env.ctx, MIDR_TOPOLOGY_RESYNC_VERSION_LOST) == 0);
	node.group_id = 30;
	node.version = 3;
	assert(midr_topology_node_upsert(env.ctx, &node) == 0);
	assert(input_status(env.ctx).normal_queue_count == 1);
	assert(input_status(env.ctx).resync_queue_count == 1);
	env_finish(&env);
}

static void test_private_api_validation(void)
{
	const uint32_t owner = router_id("1.1.1.1");
	const uint32_t remote = router_id("2.2.2.2");
	struct midr_node_update node = node_update(owner, 10, 1);
	struct midr_link_update link = link_update(owner, remote, 1, 1);
	struct midr_input_status status;
	struct midr_context empty = {};
	struct bgp bgp = {};
	struct bgp_midr midr = {};
	struct test_env env;
	bool active;

	assert(midr_input_init(NULL) == -EINVAL);
	assert(midr_input_init(&empty) == -EINVAL);
	assert(midr_input_status_get(NULL, &status) == -ENOENT);
	assert(midr_input_status_get(&empty, &status) == -ENOENT);
	assert(midr_local_fact_node_get(NULL, owner, &node, &active) == -ENOENT);
	assert(midr_local_fact_link_get(NULL, &link.key, &link, &active) == -ENOENT);
	assert(midr_input_test_set_queue_limits(NULL, 1, 1) == -ENOENT);
	assert(midr_input_test_set_snapshot_provider(NULL, test_snapshot_get,
						     test_snapshot_release) == -ENOENT);
	midr_input_test_resync_now(NULL);
	midr_topology_process_pending(NULL);
	midr_input_finish(NULL);

	assert(midr_topology_node_upsert(NULL, &node) == -ENOENT);
	assert(midr_topology_node_withdraw(NULL, owner, 1) == -ENOENT);
	assert(midr_topology_link_upsert(NULL, &link) == -ENOENT);
	assert(midr_topology_link_withdraw(NULL, &link.key, 1) == -ENOENT);
	assert(midr_topology_resync_begin(NULL, MIDR_TOPOLOGY_RESYNC_VERSION_LOST) == -ENOENT);
	assert(midr_topology_node_upsert(&empty, &node) == -ENOENT);
	empty.bgp = &bgp;
	assert(midr_topology_node_upsert(&empty, &node) == -ENOENT);
	assert(midr_input_router_id_update(NULL, true) == 0);
	assert(midr_input_router_id_update(&bgp, true) == 0);
	midr.ctx.bgp = &bgp;
	bgp.midr_info = &midr;
	assert(midr_input_router_id_update(&bgp, true) == 0);

	provider_reset(owner, 10, 1, 100);
	env_init(&env, owner);
	assert(midr_input_status_get(env.ctx, NULL) == -EINVAL);
	assert(midr_local_fact_node_get(env.ctx, owner, NULL, &active) == -EINVAL);
	assert(midr_local_fact_node_get(env.ctx, owner, &node, NULL) == -EINVAL);
	assert(midr_local_fact_link_get(env.ctx, NULL, &link, &active) == -EINVAL);
	assert(midr_local_fact_link_get(env.ctx, &link.key, NULL, &active) == -EINVAL);
	assert(midr_local_fact_link_get(env.ctx, &link.key, &link, NULL) == -EINVAL);
	assert(midr_input_test_set_queue_limits(env.ctx, 0, 1) == -EINVAL);
	assert(midr_input_test_set_queue_limits(env.ctx, 1, 0) == -EINVAL);
	assert(midr_input_test_set_snapshot_provider(env.ctx, NULL,
						     test_snapshot_release) == -EINVAL);
	assert(midr_input_test_set_snapshot_provider(env.ctx, test_snapshot_get,
						     NULL) == -EINVAL);
	assert(midr_topology_resync_begin(env.ctx, (enum midr_topology_resync_reason)99) ==
	       -EINVAL);

	node.node_id = remote;
	assert(midr_topology_node_upsert(env.ctx, &node) == -EINVAL);
	assert(midr_topology_node_withdraw(env.ctx, remote, 2) == -EINVAL);
	link.key.local_node_id = remote;
	assert(midr_topology_link_upsert(env.ctx, &link) == -EINVAL);
	assert(midr_topology_link_withdraw(env.ctx, &link.key, 2) == -EINVAL);

	assert(midr_input_router_id_update(&env.bgp, true) == 0);
	env.bgp.router_id.s_addr = 0;
	assert(midr_input_router_id_update(&env.bgp, false) == 0);
	assert(input_status(env.ctx).state == MIDR_INPUT_IDENTITY_RESTART);
	env_finish(&env);
	midr_input_finish(env.ctx);
}

struct resync_test_case {
	const char *id;
	void (*run)(void);
};

static const struct resync_test_case resync_test_cases[] = {
	{ "M2-INPUT-001", test_snapshot_barrier_and_atomic_replay },
	{ "M2-INPUT-002", test_provider_retry_and_queue_preservation },
	{ "M2-INPUT-003", test_invalid_snapshot_rollback },
	{ "M2-INPUT-004", test_resync_queue_overflow_is_atomic },
	{ "M2-INPUT-005", test_router_id_identity_restart },
};

static bool run_selected_test(const char *id)
{
	size_t i;

	for (i = 0; i < array_size(resync_test_cases); i++) {
		if (strcmp(id, resync_test_cases[i].id) != 0)
			continue;
		resync_test_cases[i].run();
		printf("PASS @%s\n", id);
		return true;
	}
	return false;
}

int main(int argc, char **argv)
{
	size_t i;

	if (argc == 2 && strcmp(argv[1], "--list") == 0) {
		for (i = 0; i < array_size(resync_test_cases); i++)
			printf("%s\n", resync_test_cases[i].id);
		return 0;
	}
	if (argc == 2)
		return run_selected_test(argv[1]) ? 0 : 2;
	if (argc != 1)
		return 2;

	test_fifo_version_and_tombstone();
	test_normal_queue_overflow_and_recovery();
	test_snapshot_withdraw_and_lower_baseline();
	test_provider_unavailable_paths();
	test_router_id_initial_establishment();
	test_finish_cancels_pending_work();
	test_private_api_validation();
	for (i = 0; i < array_size(resync_test_cases); i++)
		resync_test_cases[i].run();
	printf("MIDR resynchronization tests passed\n");
	return 0;
}
