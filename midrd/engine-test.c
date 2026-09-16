#include "midr-engine.h"
#include "midr-spf.h"
#include "midr-ted.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static struct midr_core_object prefix(uint32_t originator, uint8_t family);

static struct midr_core_object membership(uint32_t originator, uint32_t group)
{
	struct midr_core_object object = {0};

	object.identity.type = MIDR_CORE_MEMBERSHIP;
	object.identity.originator = originator;
	object.state = MIDR_CORE_ACTIVE;
	object.sequence = 1;
	object.group = group;
	return object;
}

static struct midr_core_object link_object(uint32_t originator,
					   uint32_t remote)
{
	struct midr_core_object object = {0};

	object.identity.type = MIDR_CORE_LINK;
	object.identity.originator = originator;
	object.identity.remote = remote;
	object.identity.link_id = 1;
	object.state = MIDR_CORE_ACTIVE;
	object.address_family = MIDR_CORE_AF_IPV4;
	object.sequence = 1;
	object.lifetime_ms = 1000;
	object.metric = 1;
	return object;
}

static void drain(struct midr_consumer *consumer)
{
	struct midr_consumer_event event;

	while (midr_consumer_event_next(consumer, &event) == 0)
		;
}

struct ted_callback_observer {
	size_t count;
	uint64_t generation;
	uint32_t flags;
};

static void ted_changed(struct midr_ted *ted, uint64_t generation,
				uint32_t flags, void *arg)
{
	struct ted_callback_observer *observer = arg;

	assert(ted);
	observer->count++;
	observer->generation = generation;
	observer->flags |= flags;
}

static uint32_t test_local_ifindex(void *arg, uint32_t local_node_id,
				   uint32_t remote_node_id, uint64_t link_id)
{
	(void)arg;
	(void)local_node_id;
	(void)remote_node_id;
	(void)link_id;
	return 42;
}

static size_t fill_pending(struct midr_consumer *consumer)
{
	const struct midr_consumer_event event = {
		.kind = MIDR_CONSUMER_NODE_PREFIX,
		.generation = 1,
		.originator = 999,
		.family = MIDR_CORE_AF_IPV4,
		.prefix_len = 32,
	};
	size_t count = 0;
	int ret;

	while ((ret = midr_consumer_publish(consumer, &event)) == 0)
		count++;
	assert(ret == -ENOSPC);
	return count;
}

static void test_publication_failure_keeps_canonical(void)
{
	struct midr_engine_config config = {
		.node_id = 203,
		.max_objects = 8,
		.lifetime_ms = 1000,
	};
	struct midr_consumer_config consumer_config = {0};
	struct midr_engine *engine = NULL;
	struct midr_consumer *consumer = NULL;
	struct midr_core_object local = membership(203, 1);
	struct midr_core_object owner1 = membership(31, 1);
	struct midr_core_object owner2 = membership(32, 1);
	struct midr_core_object first = prefix(31, MIDR_CORE_AF_IPV4);
	struct midr_core_object second = prefix(32, MIDR_CORE_AF_IPV4);
	struct midr_core_object current;
	struct midr_consumer_snapshot snapshot = {0};
	enum midr_core_result result;

	assert(midr_consumer_create(&consumer_config, &consumer) == 0);
	assert(midr_engine_create(&config, &engine) == 0);
	assert(midr_engine_attach_consumer(engine, consumer) == 0);
	drain(consumer);
	assert(midr_engine_apply(engine, &local, 1, &result) == 0);
	assert(midr_engine_apply(engine, &owner1, 1, &result) == 0);
	assert(midr_engine_apply(engine, &owner2, 1, &result) == 0);
	drain(consumer);
	assert(fill_pending(consumer) > 0);
	assert(midr_engine_apply(engine, &first, 1, &result) == 0);
	assert(result == MIDR_CORE_ACCEPTED);
	assert(midr_engine_lookup(engine, &first.identity, 1, &current, NULL) == 0);
	assert(midr_engine_publication_pending(engine));
	assert(midr_engine_publication_error(engine) == -ENOSPC);
	assert(midr_consumer_snapshot_acquire(consumer, &snapshot) == 0);
	assert(snapshot.count == 0);
	midr_consumer_snapshot_release(&snapshot);
	drain(consumer);
	assert(midr_engine_retry_publication(engine, 1) == 0);
	assert(!midr_engine_publication_pending(engine));
	assert(midr_consumer_snapshot_acquire(consumer, &snapshot) == 0);
	assert(snapshot.count == 1);
	midr_consumer_snapshot_release(&snapshot);
	drain(consumer);

	assert(fill_pending(consumer) > 0);
	assert(midr_engine_begin_batch(engine) == 0);
	assert(midr_engine_apply(engine, &second, 2, &result) == 0);
	assert(midr_engine_end_batch(engine, 2) == 0);
	assert(midr_engine_count(engine) == 5);
	assert(midr_engine_lookup(engine, &second.identity, 2, &current, NULL) == 0);
	assert(midr_engine_publication_pending(engine));
	assert(midr_engine_publication_error(engine) == -ENOSPC);
	drain(consumer);
	assert(midr_engine_retry_publication(engine, 2) == 0);
	assert(!midr_engine_publication_pending(engine));
	assert(midr_consumer_snapshot_acquire(consumer, &snapshot) == 0);
	assert(snapshot.count == 2);
	midr_consumer_snapshot_release(&snapshot);
	midr_engine_destroy(&engine);
	midr_consumer_destroy(&consumer);
}

static void test_ted_failure_preserves_old_view(void)
{
	struct midr_engine_config engine_config = {
		.node_id = 300,
		.max_objects = 8,
		.lifetime_ms = 1000,
	};
	struct midr_ted_config ted_config = {.max_events = 8};
	struct midr_consumer_config consumer_config = {0};
	struct midr_engine *engine = NULL;
	struct midr_consumer *consumer = NULL;
	struct midr_ted *ted = NULL;
	struct midr_core_object local = membership(300, 1);
	struct midr_core_object owner = membership(301, 1);
	struct midr_core_object object = prefix(301, MIDR_CORE_AF_IPV4);
	struct midr_core_object current;
	struct midr_consumer_snapshot snapshot = {0};
	struct midr_lsdb_snapshot lsdb = {0};
	enum midr_core_result result;
	uint64_t old_consumer_generation;
	uint64_t old_lsdb_generation;
	uint64_t old_ted_generation;

	assert(midr_engine_create(&engine_config, &engine) == 0);
	assert(midr_ted_create(&ted_config, &ted) == 0);
	assert(midr_consumer_create(&consumer_config, &consumer) == 0);
	assert(midr_engine_attach_ted(engine, ted) == 0);
	assert(midr_engine_attach_consumer(engine, consumer) == 0);
	assert(midr_ted_state(ted) == MIDR_TED_READY);
	assert(midr_engine_apply(engine, &local, 1, &result) == 0);
	assert(midr_engine_apply(engine, &owner, 1, &result) == 0);
	assert(midr_engine_apply(engine, &object, 1, &result) == 0);
	drain(consumer);
	assert(midr_consumer_snapshot_acquire(consumer, &snapshot) == 0);
	assert(snapshot.count == 1 && snapshot.events[0].metric == 301);
	old_consumer_generation = snapshot.generation;
	midr_consumer_snapshot_release(&snapshot);
	assert(midr_engine_lsdb_snapshot_acquire(engine, &lsdb) == 0);
	old_lsdb_generation = lsdb.generation;
	midr_lsdb_snapshot_release(&lsdb);
	old_ted_generation = midr_ted_generation(ted);
	assert(midr_ted_test_fail_next(ted, -ENOMEM) == 0);
	object.sequence++;
	object.metric++;
	assert(midr_engine_apply(engine, &object, 2, &result) == 0);
	assert(result == MIDR_CORE_ACCEPTED);
	assert(midr_engine_lookup(engine, &object.identity, 2, &current, NULL) == 0);
	assert(current.metric == object.metric);
	assert(midr_engine_publication_pending(engine));
	assert(midr_engine_publication_error(engine) == -ENOMEM);
	assert(midr_consumer_snapshot_acquire(consumer, &snapshot) == 0);
	assert(snapshot.generation == old_consumer_generation &&
	       snapshot.count == 1 && snapshot.events[0].metric == 301);
	midr_consumer_snapshot_release(&snapshot);
	assert(midr_engine_lsdb_snapshot_acquire(engine, &lsdb) == 0);
	assert(lsdb.generation == old_lsdb_generation);
	midr_lsdb_snapshot_release(&lsdb);
	assert(midr_ted_generation(ted) == old_ted_generation + 1U);
	assert(midr_ted_state(ted) == MIDR_TED_NOT_READY);
	assert(midr_ted_snapshot_acquire(ted, &snapshot) == -EAGAIN);

	assert(midr_engine_retry_publication(engine, 2) == 0);
	assert(!midr_engine_publication_pending(engine));
	assert(midr_ted_state(ted) == MIDR_TED_READY);
	assert(midr_consumer_snapshot_acquire(consumer, &snapshot) == 0);
	assert(snapshot.generation == midr_engine_generation(engine) &&
	       snapshot.events[0].metric == object.metric);
	assert(midr_ted_source_generation(ted) == snapshot.generation);
	assert(midr_ted_generation(ted) == old_ted_generation + 2U);
	midr_consumer_snapshot_release(&snapshot);
	assert(midr_engine_lsdb_snapshot_acquire(engine, &lsdb) == 0);
	assert(lsdb.generation == midr_engine_generation(engine));
	midr_lsdb_snapshot_release(&lsdb);
	midr_ted_destroy(&ted);
	midr_consumer_destroy(&consumer);
	midr_engine_destroy(&engine);
}

static void test_lsdb_pending_and_failure_split(void)
{
	struct midr_engine_config engine_config = {
		.node_id = 1,
		.max_objects = 8,
		.lifetime_ms = 1000,
	};
	struct midr_ted_config ted_config = {.max_events = 8};
	struct midr_consumer_config consumer_config = {0};
	struct midr_engine *engine = NULL;
	struct midr_consumer *consumer = NULL;
	struct midr_ted *ted = NULL;
	struct midr_core_object local = membership(1, 10);
	struct midr_core_object remote = membership(2, 10);
	struct midr_core_object link = link_object(1, 2);
	struct midr_core_object current;
	struct midr_consumer_snapshot consumer_snapshot = {0};
	struct midr_lsdb_snapshot lsdb = {0};
	enum midr_core_result result;
	uint64_t lsdb_generation;
	uint64_t ted_generation;

	assert(midr_engine_create(&engine_config, &engine) == 0);
	assert(midr_ted_create(&ted_config, &ted) == 0);
	assert(midr_consumer_create(&consumer_config, &consumer) == 0);
	assert(midr_engine_attach_ted(engine, ted) == 0);
	assert(midr_engine_attach_consumer(engine, consumer) == 0);
	drain(consumer);
	assert(midr_engine_apply(engine, &local, 1, &result) == 0);
	drain(consumer);
	assert(midr_engine_apply(engine, &link, 1, &result) == 0);
	assert(result == MIDR_CORE_ACCEPTED);
	assert(!midr_engine_publication_pending(engine));
	assert(midr_ted_state(ted) == MIDR_TED_READY);
	assert(midr_engine_lsdb_snapshot_acquire(engine, &lsdb) == 0);
	assert(lsdb.count == 2 && lsdb.pending_count == 1 &&
	       lsdb.usable_count == 1);
	midr_lsdb_snapshot_release(&lsdb);
	assert(midr_consumer_snapshot_acquire(consumer, &consumer_snapshot) == 0);
	assert(consumer_snapshot.count == 0);
	midr_consumer_snapshot_release(&consumer_snapshot);

	assert(midr_engine_apply(engine, &remote, 2, &result) == 0);
	assert(!midr_engine_publication_pending(engine));
	assert(midr_engine_lsdb_snapshot_acquire(engine, &lsdb) == 0);
	assert(lsdb.count == 3 && lsdb.pending_count == 0 &&
	       lsdb.usable_count == 3);
	lsdb_generation = lsdb.generation;
	midr_lsdb_snapshot_release(&lsdb);
	assert(midr_consumer_snapshot_acquire(consumer, &consumer_snapshot) == 0);
	assert(consumer_snapshot.count == 1 &&
	       consumer_snapshot.events[0].kind == MIDR_CONSUMER_LINK &&
	       consumer_snapshot.events[0].metric == 1);
	midr_consumer_snapshot_release(&consumer_snapshot);
	ted_generation = midr_ted_generation(ted);

	assert(midr_engine_test_fail_next_lsdb(engine, -ENOMEM) == 0);
	link.sequence++;
	link.metric = 20;
	assert(midr_engine_apply(engine, &link, 3, &result) == 0);
	assert(result == MIDR_CORE_ACCEPTED);
	assert(midr_engine_lookup(engine, &link.identity, 3, &current, NULL) == 0);
	assert(current.metric == 20);
	assert(midr_engine_publication_pending(engine));
	assert(midr_engine_publication_error(engine) == -ENOMEM);
	assert(midr_ted_state(ted) == MIDR_TED_NOT_READY);
	assert(midr_ted_generation(ted) == ted_generation + 1U);
	assert(midr_engine_lsdb_snapshot_acquire(engine, &lsdb) == 0);
	assert(lsdb.generation == lsdb_generation);
	midr_lsdb_snapshot_release(&lsdb);
	assert(midr_consumer_snapshot_acquire(consumer, &consumer_snapshot) == 0);
	assert(consumer_snapshot.events[0].metric == 1);
	midr_consumer_snapshot_release(&consumer_snapshot);

	drain(consumer);
	assert(midr_engine_retry_publication(engine, 3) == 0);
	assert(!midr_engine_publication_pending(engine));
	assert(midr_ted_state(ted) == MIDR_TED_READY);
	assert(midr_ted_generation(ted) == ted_generation + 2U);
	assert(midr_consumer_snapshot_acquire(consumer, &consumer_snapshot) == 0);
	assert(consumer_snapshot.events[0].metric == 20);
	assert(midr_ted_source_generation(ted) == consumer_snapshot.generation);
	midr_consumer_snapshot_release(&consumer_snapshot);
	midr_ted_destroy(&ted);
	midr_consumer_destroy(&consumer);
	midr_engine_destroy(&engine);
}

static void test_formal_ted_derivation(void)
{
	struct midr_engine_config engine_config = {
		.node_id = 1,
		.max_objects = 16,
		.lifetime_ms = 1000,
	};
	struct midr_ted_config ted_config = {
		.max_events = 16,
		.local_ifindex_lookup = test_local_ifindex,
	};
	struct midr_consumer_config consumer_config = {0};
	struct midr_engine *engine = NULL;
	struct midr_consumer *consumer = NULL;
	struct midr_ted *ted = NULL;
	struct midr_core_object objects[11];
	struct midr_ted_view view = {0};
	struct midr_spf_route routes[8] = {0};
	struct midr_ted_consumer *ted_consumer = NULL;
	struct ted_callback_observer observer = {0};
	const struct midr_ted_consumer_ops ted_ops = {
		.snapshot_changed = ted_changed,
	};
	enum midr_core_result result;
	size_t route_count = 0;
	uint64_t spf_generation;

	objects[0] = membership(1, 10);
	objects[1] = membership(2, 10);
	objects[2] = membership(3, 20);
	objects[3] = membership(4, 20);
	objects[4] = link_object(1, 2);
	objects[4].metric = 5;
	objects[5] = link_object(1, 3);
	objects[5].identity.link_id = 2;
	objects[5].metric = 30;
	objects[6] = link_object(2, 4);
	objects[6].identity.link_id = 3;
	objects[6].metric = 20;
	objects[7] = link_object(3, 1);
	objects[7].identity.link_id = 4;
	objects[7].metric = 7;
	objects[8] = prefix(2, MIDR_CORE_AF_IPV4);
	objects[9] = prefix(3, MIDR_CORE_AF_IPV6);
	objects[10] = prefix(3, MIDR_CORE_AF_IPV6);
	objects[10].identity.type = MIDR_CORE_GROUP_PREFIX;
	objects[10].identity.group = 20;
	objects[10].identity.prefix[15] = 1;

	assert(midr_engine_create(&engine_config, &engine) == 0);
	assert(midr_ted_create(&ted_config, &ted) == 0);
	assert(midr_ted_consumer_register(ted, &ted_ops, &observer,
					 &ted_consumer) == 0);
	assert(observer.count == 0);
	assert(midr_consumer_create(&consumer_config, &consumer) == 0);
	assert(midr_engine_attach_ted(engine, ted) == 0);
	assert(midr_engine_attach_consumer(engine, consumer) == 0);
	for (size_t i = 0; i < sizeof(objects) / sizeof(objects[0]); i++) {
		assert(midr_engine_apply(engine, &objects[i], 1, &result) == 0);
		assert(result == MIDR_CORE_ACCEPTED);
		drain(consumer);
	}
	assert(midr_ted_view_acquire(ted, &view) == 0);
	assert(view.local_node_id == 1 && view.local_group_id == 10);
	assert(view.node_count == 2 && view.nodes[0].node_id == 1 &&
	       view.nodes[1].node_id == 2);
	assert(view.intra_link_count == 1 &&
	       view.intra_links[0].canonical_cost == 5 &&
	       view.intra_links[0].local_ifindex == 42);
	assert(view.egress_link_count == 2 &&
	       view.egress_links[0].canonical_cost == 30 &&
	       view.egress_links[0].local_ifindex == 42 &&
	       view.egress_links[1].canonical_cost == 20 &&
	       view.egress_links[1].local_ifindex == 0);
	assert(view.node_prefix_count == 1 &&
	       view.node_prefixes[0].node_id == 2);
	assert(view.group_edge_count == 2);
	assert(view.group_edges[0].source_group_id == 10 &&
	       view.group_edges[0].target_group_id == 20 &&
	       view.group_edges[0].aggregate_cost == 20);
	assert(view.group_edges[1].source_group_id == 20 &&
	       view.group_edges[1].target_group_id == 10 &&
	       view.group_edges[1].aggregate_cost == 7);
	assert(view.prefix_group_count == 1 &&
	       view.prefix_groups[0].group_id == 20);
	assert(observer.count > 0 && observer.generation == view.generation &&
	       (observer.flags & MIDR_TED_CHANGE_ALL) == MIDR_TED_CHANGE_ALL &&
	       midr_ted_generation_is_current(ted, view.generation));
	assert(midr_spf_compute_ted(&view, routes, 8, &route_count) == 0);
	assert(route_count == 2);
	midr_spf_routes_clear(routes, route_count);
	spf_generation = view.generation;
	midr_ted_view_release(&view);
	objects[4].sequence++;
	objects[4].metric++;
	assert(midr_engine_apply(engine, &objects[4], 2, &result) == 0);
	assert(result == MIDR_CORE_ACCEPTED);
	assert(!midr_ted_generation_is_current(ted, spf_generation));
	midr_ted_consumer_unregister(ted, &ted_consumer);
	midr_engine_destroy(&engine);
	midr_consumer_destroy(&consumer);
	midr_ted_destroy(&ted);
}

static void test_batch_abort_is_atomic(void)
{
	struct midr_engine_config config = {
		.node_id = 200,
		.max_objects = 4,
		.lifetime_ms = 1000,
	};
	struct midr_engine *engine = NULL;
	struct midr_consumer *consumer = NULL;
	struct midr_consumer_config consumer_config = {0};
	struct midr_core_object object = prefix(20, MIDR_CORE_AF_IPV4);
	struct midr_core_object objects[1];
	size_t count = 0;

	assert(midr_consumer_create(&consumer_config, &consumer) == 0);
	assert(midr_engine_create(&config, &engine) == 0);
	assert(midr_engine_attach_consumer(engine, consumer) == 0);
	drain(consumer);
	assert(midr_engine_begin_batch(engine) == 0);
	assert(midr_engine_apply(engine, &object, 1,
				&(enum midr_core_result){0}) == 0);
	assert(midr_engine_count(engine) == 0);
	assert(midr_engine_snapshot(engine, 1, objects, 1, &count) == 0 &&
	       count == 0);
	assert(midr_engine_batch_snapshot(engine, 1, objects, 1, &count) == 0 &&
	       count == 1);
	assert(midr_engine_generation(engine) == 2);
	assert(midr_consumer_pending(consumer) == 0);
	assert(midr_engine_abort_batch(engine) == 0);
	assert(midr_engine_count(engine) == 0);
	assert(midr_engine_generation(engine) == 1);
	assert(midr_consumer_pending(consumer) == 0);
	midr_engine_destroy(&engine);
	midr_consumer_destroy(&consumer);
}

static void test_batch_failure_does_not_publish(void)
{
	struct midr_engine_config config = {
		.node_id = 201,
		.max_objects = 1,
		.lifetime_ms = 1000,
	};
	struct midr_engine *engine = NULL;
	struct midr_consumer *consumer = NULL;
	struct midr_consumer_config consumer_config = {0};
	struct midr_core_object first = prefix(21, MIDR_CORE_AF_IPV4);
	struct midr_core_object second = prefix(22, MIDR_CORE_AF_IPV4);
	enum midr_core_result result;

	assert(midr_consumer_create(&consumer_config, &consumer) == 0);
	assert(midr_engine_create(&config, &engine) == 0);
	assert(midr_engine_attach_consumer(engine, consumer) == 0);
	drain(consumer);
	assert(midr_engine_begin_batch(engine) == 0);
	assert(midr_engine_apply(engine, &first, 1, &result) == 0);
	assert(midr_engine_apply(engine, &second, 1, &result) == -ENOSPC);
	assert(midr_engine_end_batch(engine, 1) == -ENOSPC);
	assert(midr_engine_count(engine) == 0);
	assert(midr_engine_generation(engine) == 1);
	assert(midr_consumer_pending(consumer) == 0);
	/* The failed transaction was discarded by end_batch(). */
	assert(midr_engine_begin_batch(engine) == 0);
	assert(midr_engine_apply(engine, &first, 2, &result) == 0);
	assert(midr_engine_end_batch(engine, 2) == 0);
	assert(midr_engine_count(engine) == 1);
	assert(midr_engine_generation(engine) == 2);
	midr_engine_destroy(&engine);
	midr_consumer_destroy(&consumer);
}

static void test_batch_invalid_is_atomic(void)
{
	struct midr_engine_config config = {
		.node_id = 202,
		.max_objects = 4,
		.lifetime_ms = 1000,
	};
	struct midr_engine *engine = NULL;
	struct midr_consumer *consumer = NULL;
	struct midr_consumer_config consumer_config = {0};
	struct midr_core_object invalid = prefix(23, MIDR_CORE_AF_IPV4);
	enum midr_core_result result;

	invalid.identity.originator = 0;
	assert(midr_consumer_create(&consumer_config, &consumer) == 0);
	assert(midr_engine_create(&config, &engine) == 0);
	assert(midr_engine_attach_consumer(engine, consumer) == 0);
	drain(consumer);
	assert(midr_engine_begin_batch(engine) == 0);
	assert(midr_engine_apply(engine, &invalid, 1, &result) == -EINVAL);
	assert(midr_engine_end_batch(engine, 1) == -EINVAL);
	assert(midr_engine_count(engine) == 0);
	assert(midr_engine_generation(engine) == 1);
	assert(midr_consumer_pending(consumer) == 0);
	midr_engine_destroy(&engine);
	midr_consumer_destroy(&consumer);
}

static size_t snapshot_count(struct midr_consumer *consumer)
{
	struct midr_consumer_snapshot snapshot = {0};
	size_t count;

	assert(midr_consumer_snapshot_acquire(consumer, &snapshot) == 0);
	count = snapshot.count;
	midr_consumer_snapshot_release(&snapshot);
	return count;
}

static void test_membership_scope_lifecycle(void)
{
	struct midr_engine_config config = {
		.node_id = 1,
		.max_objects = 16,
		.lifetime_ms = 1000,
	};
	struct midr_consumer_config consumer_config = {0};
	struct midr_engine *engine = NULL;
	struct midr_consumer *consumer = NULL;
	struct midr_core_object m1 = membership(1, 10);
	struct midr_core_object m2 = membership(2, 10);
	struct midr_core_object m3 = membership(3, 20);
	struct midr_core_object p2 = prefix(2, MIDR_CORE_AF_IPV4);
	struct midr_core_object p3 = prefix(3, MIDR_CORE_AF_IPV4);
	struct midr_core_object l12 = link_object(1, 2);
	struct midr_core_object l13 = link_object(1, 3);
	struct midr_core_object m2_moved = m2;
	struct midr_core_object current;
	struct midr_core_identity key = m2.identity;
	struct midr_core_object memberships[] = {m1, m2, m3};
	enum midr_core_result result;
	size_t expired = 0;

	assert(midr_consumer_create(&consumer_config, &consumer) == 0);
	assert(midr_engine_create(&config, &engine) == 0);
	assert(midr_engine_attach_consumer(engine, consumer) == 0);
	drain(consumer);
	for (size_t i = 0; i < sizeof(memberships) / sizeof(memberships[0]); i++) {
		assert(midr_engine_apply(engine, &memberships[i], 1, &result) == 0);
		assert(result == MIDR_CORE_ACCEPTED);
		drain(consumer);
	}
	assert(midr_engine_apply(engine, &p2, 2, &result) == 0);
	assert(midr_engine_apply(engine, &p3, 2, &result) == 0);
	assert(midr_engine_apply(engine, &l12, 2, &result) == 0);
	assert(midr_engine_apply(engine, &l13, 2, &result) == 0);
	assert(snapshot_count(consumer) == 3);
	drain(consumer);

	/* A same-identity Membership update changes scope atomically. */
	m2_moved.sequence = 2;
	m2_moved.group = 20;
	assert(midr_engine_apply(engine, &m2_moved, 3, &result) == 0);
	assert(midr_engine_membership(engine, 2, &current.group) == 0 &&
	       current.group == 20);
	assert(snapshot_count(consumer) == 2);
	drain(consumer);

	/* Withdrawal removes the peer's local-only objects but leaves the global
	 * link usable; the canonical store still retains the withdrawn identity. */
	assert(midr_engine_withdraw(engine, &key, 4) == 0);
	assert(snapshot_count(consumer) == 1);
	assert(midr_engine_count(engine) == 7);
	drain(consumer);

	/* Membership expiry rebuilds scope and immediately withdraws dependent
	 * objects from the committed usable snapshot. */
	assert(midr_engine_expire(engine, 2000, &expired) == 0);
	assert(expired == 7);
	assert(snapshot_count(consumer) == 0);
	assert(midr_engine_membership(engine, 1, &current.group) == -ENOENT);
	midr_engine_destroy(&engine);
	midr_consumer_destroy(&consumer);
}

static void test_scope_rollback_is_atomic(void)
{
	struct midr_engine_config config = {
		.node_id = 10,
		.max_objects = 2,
		.lifetime_ms = 1000,
	};
	struct midr_consumer_config consumer_config = {0};
	struct midr_engine *engine = NULL;
	struct midr_consumer *consumer = NULL;
	struct midr_core_object m = membership(10, 1);
	struct midr_core_object p1 = prefix(11, MIDR_CORE_AF_IPV4);
	struct midr_core_object p2 = prefix(12, MIDR_CORE_AF_IPV4);
	enum midr_core_result result;

	assert(midr_consumer_create(&consumer_config, &consumer) == 0);
	assert(midr_engine_create(&config, &engine) == 0);
	assert(midr_engine_attach_consumer(engine, consumer) == 0);
	drain(consumer);
	assert(midr_engine_begin_batch(engine) == 0);
	assert(midr_engine_apply(engine, &m, 1, &result) == 0);
	assert(midr_engine_apply(engine, &p1, 1, &result) == 0);
	assert(midr_engine_apply(engine, &p2, 1, &result) == -ENOSPC);
	assert(midr_engine_end_batch(engine, 1) == -ENOSPC);
	assert(midr_engine_count(engine) == 0);
	assert(midr_engine_membership(engine, 10, &(uint32_t){0}) == -ENOENT);
	assert(snapshot_count(consumer) == 0);
	midr_engine_destroy(&engine);
	midr_consumer_destroy(&consumer);
}

static void test_snapshot_rebuilds_expired_scope(void)
{
	struct midr_engine_config config = {
		.node_id = 1,
		.max_objects = 8,
		.lifetime_ms = 100,
	};
	struct midr_consumer_config consumer_config = {0};
	struct midr_engine *engine = NULL;
	struct midr_consumer *consumer = NULL;
	struct midr_core_object m = membership(1, 10);
	struct midr_core_object p = prefix(1, MIDR_CORE_AF_IPV4);
	struct midr_core_object later = prefix(2, MIDR_CORE_AF_IPV4);
	enum midr_core_result result;

	assert(midr_consumer_create(&consumer_config, &consumer) == 0);
	assert(midr_engine_create(&config, &engine) == 0);
	assert(midr_engine_attach_consumer(engine, consumer) == 0);
	drain(consumer);
	assert(midr_engine_apply(engine, &m, 1, &result) == 0);
	assert(midr_engine_apply(engine, &p, 1, &result) == 0);
	assert(snapshot_count(consumer) == 1);
	drain(consumer);
	/* The read-side expiry must rebuild the scope before publishing the new
	 * object, so the expired local Membership cannot authorize it. */
	assert(midr_engine_apply(engine, &later, 200, &result) == 0);
	assert(snapshot_count(consumer) == 0);
	assert(midr_engine_membership(engine, 1, &(uint32_t){0}) == -ENOENT);
	midr_engine_destroy(&engine);
	midr_consumer_destroy(&consumer);
}

static void test_refresh_and_scope_queries(void)
{
	struct midr_engine_config config = {
		.node_id = 1,
		.max_objects = 8,
		.lifetime_ms = 1000,
	};
	struct midr_engine *engine = NULL;
	struct midr_core_object local = membership(1, 10);
	struct midr_core_object peer = membership(2, 10);
	struct midr_core_object object = prefix(2, MIDR_CORE_AF_IPV4);
	struct midr_core_object withdrawn = object;
	struct midr_core_object current;
	struct midr_core_identity missing = object.identity;
	enum midr_core_result result;
	uint64_t generation;

	assert(midr_engine_create(&config, &engine) == 0);
	assert(midr_engine_apply(engine, &local, 1, &result) == 0);
	assert(midr_engine_apply(engine, &peer, 1, &result) == 0);
	assert(midr_engine_apply(engine, &object, 1, &result) == 0);
	assert(midr_engine_export(engine, &local, 2));
	assert(midr_engine_usable(engine, &local));
	assert(midr_engine_export(engine, &object, 2));
	assert(midr_engine_usable(engine, &object));

	withdrawn.state = MIDR_CORE_WITHDRAWN;
	withdrawn.address_family = MIDR_CORE_AF_NONE;
	withdrawn.group = 0;
	withdrawn.metric = 0;
	memset(withdrawn.local_address, 0, sizeof(withdrawn.local_address));
	memset(withdrawn.remote_address, 0, sizeof(withdrawn.remote_address));
	assert(midr_engine_export(engine, &withdrawn, 99));
	assert(!midr_engine_usable(engine, &withdrawn));

	generation = midr_engine_generation(engine);
	assert(midr_engine_refresh(engine, &object.identity, 100) == 0);
	assert(midr_engine_generation(engine) == generation);
	assert(midr_engine_lookup(engine, &object.identity, 100, &current, NULL) == 0);
	assert(current.sequence == object.sequence + 1U);
	assert(midr_core_object_semantic_equal(&current, &object));

	missing.originator = 3;
	assert(midr_engine_begin_batch(engine) == 0);
	assert(midr_engine_refresh(engine, &missing, 101) == -ENOENT);
	assert(midr_engine_end_batch(engine, 101) == -ENOENT);
	assert(midr_engine_lookup(engine, &object.identity, 101, &current, NULL) == 0);
	assert(current.sequence == object.sequence + 1U);
	midr_engine_destroy(&engine);
}

static struct midr_prefix_event prefix_event(
	enum midr_prefix_event_kind kind, uint64_t generation,
	uint32_t originator, uint8_t family)
{
	struct midr_prefix_event event = {
		.kind = kind,
		.generation = generation,
		.originator = originator,
		.prefix = {
			.family = family,
			.prefix_len = family == MIDR_CORE_AF_IPV4 ? 32U : 128U,
			.metric = 10,
		},
	};

	if (family == MIDR_CORE_AF_IPV4) {
		event.prefix.address[0] = 192;
		event.prefix.address[1] = 0;
		event.prefix.address[2] = 2;
		event.prefix.address[3] = (uint8_t)originator;
	} else {
		event.prefix.address[0] = 0x20;
		event.prefix.address[1] = 0x01;
		event.prefix.address[2] = 0x0d;
		event.prefix.address[3] = 0xb8;
		event.prefix.address[15] = (uint8_t)originator;
	}
	return event;
}

static void test_prefix_events(void)
{
	struct midr_engine_config config = {
		.node_id = 1,
		.max_objects = 4,
		.lifetime_ms = 1000,
	};
	struct midr_engine *engine = NULL;
	struct midr_prefix_event v4 = prefix_event(
		MIDR_PREFIX_UPSERT, 1, 2, MIDR_CORE_AF_IPV4);
	struct midr_prefix_event v6 = prefix_event(
		MIDR_PREFIX_UPSERT, 1, 3, MIDR_CORE_AF_IPV6);
	struct midr_prefix_event control = {
		.kind = MIDR_PREFIX_SNAPSHOT_BEGIN,
		.generation = 1,
		.originator = 1,
	};
	struct midr_core_object current;
	struct midr_core_identity v4_key = {
		.type = MIDR_CORE_NODE_PREFIX,
		.family = MIDR_CORE_AF_IPV4,
		.prefix_len = 32,
		.originator = 2,
		.prefix = {192, 0, 2, 2},
	};
	struct midr_core_identity v6_key = {
		.type = MIDR_CORE_NODE_PREFIX,
		.family = MIDR_CORE_AF_IPV6,
		.prefix_len = 128,
		.originator = 3,
		.prefix = {0x20, 0x01, 0x0d, 0xb8},
	};

	v6_key.prefix[15] = 3;
	assert(midr_engine_create(&config, &engine) == 0);
	assert(midr_engine_apply_prefix_event(engine, &control, 1) == 0);
	assert(midr_engine_count(engine) == 0);
	assert(midr_engine_apply_prefix_event(engine, &v4, 1) == 0);
	assert(midr_engine_apply_prefix_event(engine, &v6, 1) == 0);
	assert(midr_engine_lookup(engine, &v4_key, 1, &current, NULL) == 0);
	assert(current.state == MIDR_CORE_ACTIVE && current.metric == 10);
	assert(midr_engine_lookup(engine, &v6_key, 1, &current, NULL) == 0);
	assert(current.state == MIDR_CORE_ACTIVE && current.metric == 10);

	v4.kind = MIDR_PREFIX_WITHDRAW;
	v4.generation = 2;
	v6.kind = MIDR_PREFIX_WITHDRAW;
	v6.generation = 2;
	assert(midr_engine_apply_prefix_event(engine, &v4, 2) == 0);
	assert(midr_engine_apply_prefix_event(engine, &v6, 2) == 0);
	assert(midr_engine_lookup(engine, &v4_key, 2, &current, NULL) == 0);
	assert(current.state == MIDR_CORE_WITHDRAWN && current.metric == 0);
	assert(midr_engine_lookup(engine, &v6_key, 2, &current, NULL) == 0);
	assert(current.state == MIDR_CORE_WITHDRAWN && current.metric == 0);
	midr_engine_destroy(&engine);
}

static struct midr_core_object prefix(uint32_t originator, uint8_t family)
{
	struct midr_core_object object = {0};

	object.identity.type = MIDR_CORE_NODE_PREFIX;
	object.identity.family = family;
	object.identity.prefix_len = family == MIDR_CORE_AF_IPV4 ? 24 : 64;
	object.identity.originator = originator;
	object.identity.prefix[0] = family == MIDR_CORE_AF_IPV4 ? 192 : 0x20;
	object.identity.prefix[1] = family == MIDR_CORE_AF_IPV4 ? 0 : 1;
	object.identity.prefix[2] = family == MIDR_CORE_AF_IPV4 ? 2 : 0x0d;
	object.sequence = 1;
	object.state = MIDR_CORE_ACTIVE;
	object.lifetime_ms = 1000;
	object.metric = originator;
	return object;
}

int main(void)
{
	test_batch_abort_is_atomic();
	test_batch_failure_does_not_publish();
	test_batch_invalid_is_atomic();
	test_publication_failure_keeps_canonical();
	test_membership_scope_lifecycle();
	test_scope_rollback_is_atomic();
	test_snapshot_rebuilds_expired_scope();
	test_refresh_and_scope_queries();
	test_prefix_events();
	test_ted_failure_preserves_old_view();
	test_lsdb_pending_and_failure_split();
	test_formal_ted_derivation();
	struct midr_engine_config engine_config = {
		.node_id = 100,
		.max_objects = 16,
		.lifetime_ms = 1000,
	};
	struct midr_consumer_config consumer_config = {0};
	struct midr_engine *engine = NULL;
	struct midr_consumer *consumer = NULL;
	struct midr_consumer_snapshot snapshot = {0};
	struct midr_spf_route routes[4] = {0};
	struct midr_core_object v4 = prefix(10, MIDR_CORE_AF_IPV4);
	struct midr_core_object v6 = prefix(11, MIDR_CORE_AF_IPV6);
	struct midr_core_object m_local = membership(100, 10);
	struct midr_core_object m_v4 = membership(10, 10);
	struct midr_core_object m_v6 = membership(11, 10);
	size_t count = 0;

	assert(midr_consumer_create(&consumer_config, &consumer) == 0);
	assert(midr_engine_create(&engine_config, &engine) == 0);
	assert(midr_engine_attach_consumer(engine, consumer) == 0);
	drain(consumer);
	assert(midr_engine_apply(engine, &m_local, 1,
				&(enum midr_core_result){0}) == 0);
	drain(consumer);
	assert(midr_engine_apply(engine, &m_v4, 1,
				&(enum midr_core_result){0}) == 0);
	drain(consumer);
	assert(midr_engine_apply(engine, &m_v6, 1,
				&(enum midr_core_result){0}) == 0);
	drain(consumer);
	assert(midr_engine_begin_batch(engine) == 0);
	assert(midr_engine_apply(engine, &v4, 1, &(enum midr_core_result){0}) == 0);
	assert(midr_engine_apply(engine, &v6, 1, &(enum midr_core_result){0}) == 0);
	assert(midr_consumer_pending(consumer) == 0);
	assert(midr_engine_end_batch(engine, 1) == 0);
	assert(midr_consumer_pending(consumer) == 4);
	assert(midr_consumer_snapshot_acquire(consumer, &snapshot) == 0);
	assert(snapshot.count == 2 && snapshot.generation == 6);
	assert(midr_spf_compute(&snapshot, engine_config.node_id, routes, 4,
				       &count) == 0 && count == 2);
	midr_spf_routes_clear(routes, count);
	midr_consumer_snapshot_release(&snapshot);
	drain(consumer);
	assert(midr_engine_withdraw(engine, &v4.identity, 2) == 0);
	assert(midr_consumer_snapshot_acquire(consumer, &snapshot) == 0);
	assert(snapshot.count == 1 && snapshot.events[0].family == MIDR_CORE_AF_IPV6);
	midr_consumer_snapshot_release(&snapshot);
	midr_engine_destroy(&engine);
	midr_consumer_destroy(&consumer);
	puts("midrd-engine-test: PASS");
	return 0;
}
