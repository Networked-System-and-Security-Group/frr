#define main midrd_program_main
#include "midrd.c"
#undef main

#include <assert.h>

struct consumer_probe {
	size_t snapshots;
};

static void probe_consumer(void *arg,
			   const struct midr_consumer_event *event)
{
	struct consumer_probe *probe = arg;

	assert(event);
	if (event->kind == MIDR_CONSUMER_SNAPSHOT_BEGIN)
		probe->snapshots++;
}

static struct midr_local_event local_control(enum midr_local_event_kind kind,
					     uint64_t generation)
{
	struct midr_local_event event = {
		.kind = kind,
		.generation = generation,
		.originator = 77,
	};

	return event;
}

static struct midr_local_event membership_event(uint64_t generation,
						uint64_t version,
						uint32_t group)
{
	struct midr_local_event event = local_control(MIDR_LOCAL_MEMBERSHIP,
							generation);

	event.fact.membership.group = group;
	event.fact.membership.version = version;
	return event;
}

static struct midr_local_event link_event(uint64_t generation,
					  uint64_t version,
					  uint32_t remote,
					  uint8_t family,
					  uint32_t rtt_us,
					  uint64_t measurement_sequence)
{
	struct midr_local_event event = local_control(MIDR_LOCAL_LINK,
							generation);

	event.fact.link.remote_node_id = remote;
	event.fact.link.family = family;
	event.fact.link.link_id = remote;
	event.fact.link.version = version;
	event.fact.link.rtt_us = rtt_us;
	event.fact.link.loss_ppm = 1000;
	event.fact.link.available_bandwidth_kbps = 1000000;
	event.fact.link.measurement_sequence = measurement_sequence;
	event.fact.link.measurement_timestamp_ms = measurement_sequence * 10U;
	if (family == MIDR_CORE_AF_IPV4) {
		event.fact.link.local_address[0] = 192;
		event.fact.link.local_address[1] = 0;
		event.fact.link.local_address[2] = 2;
		event.fact.link.local_address[3] = (uint8_t)remote;
		event.fact.link.remote_address[0] = 198;
		event.fact.link.remote_address[1] = 51;
		event.fact.link.remote_address[2] = 100;
		event.fact.link.remote_address[3] = (uint8_t)remote;
	} else {
		event.fact.link.local_address[0] = 0x20;
		event.fact.link.local_address[1] = 0x01;
		event.fact.link.local_address[2] = 0x0d;
		event.fact.link.local_address[3] = 0xb8;
		event.fact.link.local_address[15] = (uint8_t)remote;
		event.fact.link.remote_address[0] = 0x20;
		event.fact.link.remote_address[1] = 0x01;
		event.fact.link.remote_address[2] = 0x0d;
		event.fact.link.remote_address[3] = 0xb8;
		event.fact.link.remote_address[15] = (uint8_t)(remote + 1U);
	}
	return event;
}

static void initialize_daemon(struct midrd *daemon,
			      struct consumer_probe *probe, size_t capacity)
{
	struct midr_engine_config engine_config = {
		.node_id = 77,
		.max_objects = capacity,
		.lifetime_ms = 600000,
	};
	struct midr_owned_config owned_config = {
		.originator = 77,
		.max_objects = capacity,
		.lifetime_ms = 600000,
	};
	struct midr_consumer_config consumer_config = {
		.on_event = probe_consumer,
		.arg = probe,
	};

	memset(daemon, 0, sizeof(*daemon));
	memset(probe, 0, sizeof(*probe));
	daemon->node_id = 77;
	daemon->lifetime_ms = 600000;
	assert(midr_engine_create(&engine_config, &daemon->engine) == 0);
	assert(midr_consumer_create(&consumer_config, &daemon->consumer) == 0);
	assert(midr_engine_attach_consumer(daemon->engine, daemon->consumer) == 0);
	assert(midr_owned_create(&owned_config, publish_owned, daemon,
				 &daemon->owned) == 0);
}

static void destroy_daemon(struct midrd *daemon)
{
	midr_owned_destroy(&daemon->owned);
	midr_consumer_destroy(&daemon->consumer);
	midr_engine_destroy(&daemon->engine);
}

static uint64_t consumer_generation(struct midrd *daemon, size_t *count)
{
	struct midr_consumer_snapshot snapshot = {0};
	uint64_t generation;

	assert(midr_consumer_snapshot_acquire(daemon->consumer, &snapshot) == 0);
	generation = snapshot.generation;
	if (count)
		*count = snapshot.count;
	midr_consumer_snapshot_release(&snapshot);
	return generation;
}

static void drain_consumer_events(struct midr_consumer *consumer)
{
	struct midr_consumer_event event;

	while (midr_consumer_event_next(consumer, &event) == 0)
		;
}

static size_t fill_consumer(struct midr_consumer *consumer)
{
	const struct midr_consumer_event event = {
		.kind = MIDR_CONSUMER_NODE_PREFIX,
		.generation = 1,
		.originator = 77,
		.family = MIDR_CORE_AF_IPV4,
		.prefix_len = 24,
	};
	size_t count = 0;
	int ret;

	while ((ret = midr_consumer_publish(consumer, &event)) == 0)
		count++;
	assert(ret == -ENOSPC);
	return count;
}

static void assert_no_engine_events(struct midrd *daemon)
{
	struct midr_core_object event;

	assert(midr_engine_event_next(daemon->engine, &event) == -ENOENT);
}

static void link_identity_for(struct midrd *daemon,
			      const struct midr_local_event *event,
			      struct midr_core_identity *identity)
{
	struct midrd_link_config config = {
		.remote = event->fact.link.remote_node_id,
		.link_id = event->fact.link.link_id,
	};

	link_identity(daemon, &config, identity);
}

static struct midr_core_object owned_link(
	struct midrd *daemon, const struct midr_local_event *event)
{
	struct midr_core_identity identity;
	struct midr_core_object object;

	link_identity_for(daemon, event, &identity);
	assert(midr_owned_lookup(daemon->owned, &identity, &object) == 0);
	return object;
}

static int deliver_snapshot(struct midrd *daemon, uint64_t now_ms,
			    uint64_t generation,
			    const struct midr_local_event *membership,
			    const struct midr_local_event *links, size_t link_count)
{
	struct midr_local_event event;
	int ret;

	event = local_control(MIDR_LOCAL_SNAPSHOT_BEGIN, generation);
	ret = local_event_at(daemon, &event, now_ms);
	if (ret)
		return ret;
	if (membership) {
		event = *membership;
		event.generation = generation;
		ret = local_event_at(daemon, &event, now_ms);
		if (ret)
			return ret;
	}
	for (size_t i = 0; i < link_count; i++) {
		event = links[i];
		event.generation = generation;
		ret = local_event_at(daemon, &event, now_ms);
		if (ret)
			return ret;
	}
	event = local_control(MIDR_LOCAL_SNAPSHOT_END, generation);
	ret = local_event_at(daemon, &event, now_ms);
	if (ret)
		return ret;
	event = local_control(MIDR_LOCAL_EOR, generation);
	return local_event_at(daemon, &event, now_ms);
}

static void test_atomic_and_cost(void)
{
	const uint64_t start = 1000;
	struct midrd daemon;
	struct consumer_probe probe;
	struct midr_local_event membership = membership_event(1, 1, 9);
	struct midr_local_event links[2];
	struct midr_core_object object;
	uint64_t generation;
	uint64_t view_generation;
	uint64_t sequence;
	uint64_t failed_sequence;
	uint32_t baseline_cost;
	uint32_t candidate_cost;
	size_t pending;
	size_t count;

	initialize_daemon(&daemon, &probe, 16);
	links[0] = link_event(1, 1, 88, MIDR_CORE_AF_IPV4, 1000000, 1);
	links[1] = link_event(1, 1, 89, MIDR_CORE_AF_IPV6, 1200000, 1);
	assert(deliver_snapshot(&daemon, start, 1, &membership, links, 2) == 0);
	assert(daemon.local_generation == 1 && daemon.group_id == 9);
	assert(daemon.membership_version == 1 && daemon.link_count == 2);
	assert(daemon.links[0].address_family == MIDR_CORE_AF_IPV4);
	assert(daemon.links[1].address_family == MIDR_CORE_AF_IPV6);
	object = owned_link(&daemon, &links[0]);
	assert(object.address_family == MIDR_CORE_AF_IPV4);
	assert(object.local_address[3] == 88);
	baseline_cost = object.metric;
	generation = consumer_generation(&daemon, &count);
	sequence = midr_owned_last_sequence(daemon.owned);

	/* A higher input version and fresh diagnostics are committed locally,
	 * but a sub-deadband cost change does not create a new LS object. */
	membership = membership_event(2, 2, 9);
	links[0] = link_event(2, 2, 88, MIDR_CORE_AF_IPV4, 1000100, 2);
	links[1].generation = 2;
	links[1].fact.link.version = 2;
	links[1].fact.link.measurement_sequence = 2;
	links[1].fact.link.measurement_timestamp_ms = 20;
	assert(deliver_snapshot(&daemon, start + 1000, 2, &membership, links, 2) ==
	       0);
	assert(daemon.links[0].input_version == 2);
	assert(daemon.links[0].measurement_sequence == 2);
	assert(!daemon.links[0].cost_pending);
	assert(owned_link(&daemon, &links[0]).metric == baseline_cost);
	assert(midr_owned_last_sequence(daemon.owned) == sequence);
	assert(consumer_generation(&daemon, &count) == generation);

	/* A significant measurement change is retained as the latest candidate
	 * until the five-minute owner-side interval expires. */
	membership = membership_event(3, 3, 9);
	links[0] = link_event(3, 3, 88, MIDR_CORE_AF_IPV4, 1100000, 3);
	links[1].generation = 3;
	links[1].fact.link.version = 3;
	links[1].fact.link.measurement_sequence = 3;
	links[1].fact.link.measurement_timestamp_ms = 30;
	assert(deliver_snapshot(&daemon, start + 2000, 3, &membership, links, 2) ==
	       0);
	assert(daemon.links[0].cost_pending);
	candidate_cost = daemon.links[0].candidate_metric;
	assert(candidate_cost != baseline_cost);
	assert(owned_link(&daemon, &links[0]).metric == baseline_cost);
	drain_consumer_events(daemon.consumer);
	pending = fill_consumer(daemon.consumer);
	generation = midr_engine_generation(daemon.engine);
	view_generation = consumer_generation(&daemon, &count);
	sequence = midr_owned_last_sequence(daemon.owned);
	assert(publish_pending_link_costs_at(
		       &daemon,
		       start + MIDR_LINK_COST_MIN_ADVERTISEMENT_INTERVAL_MS - 1U) ==
	       0);
	assert(owned_link(&daemon, &links[0]).metric == baseline_cost);
	assert(midr_consumer_pending(daemon.consumer) == pending);
	assert(publish_pending_link_costs_at(
		       &daemon,
		       start + MIDR_LINK_COST_MIN_ADVERTISEMENT_INTERVAL_MS) ==
	       -ENOSPC);
	assert(daemon.links[0].cost_pending);
	assert(daemon.links[0].metric == baseline_cost);
	assert(owned_link(&daemon, &links[0]).metric == baseline_cost);
	assert(midr_engine_generation(daemon.engine) == generation);
	assert(consumer_generation(&daemon, &count) == view_generation);
	assert(midr_consumer_pending(daemon.consumer) == pending);
	assert_no_engine_events(&daemon);
	failed_sequence = midr_owned_last_sequence(daemon.owned);
	assert(failed_sequence > sequence);
	drain_consumer_events(daemon.consumer);
	assert(publish_pending_link_costs_at(
		       &daemon,
		       start + MIDR_LINK_COST_MIN_ADVERTISEMENT_INTERVAL_MS) == 0);
	assert(!daemon.links[0].cost_pending);
	assert(owned_link(&daemon, &links[0]).metric == candidate_cost);
	assert(midr_owned_last_sequence(daemon.owned) > failed_sequence);

	/* The latest candidate may cancel a pending update. */
	membership = membership_event(4, 4, 9);
	links[0] = link_event(4, 4, 88, MIDR_CORE_AF_IPV4, 1250000, 4);
	links[1].generation = 4;
	links[1].fact.link.version = 4;
	assert(deliver_snapshot(
		       &daemon,
		       start + MIDR_LINK_COST_MIN_ADVERTISEMENT_INTERVAL_MS + 1000,
		       4, &membership, links, 2) == 0);
	assert(daemon.links[0].cost_pending);
	membership = membership_event(5, 5, 9);
	links[0] = link_event(5, 5, 88, MIDR_CORE_AF_IPV4, 1100100, 5);
	links[1].generation = 5;
	links[1].fact.link.version = 5;
	assert(deliver_snapshot(
		       &daemon,
		       start + MIDR_LINK_COST_MIN_ADVERTISEMENT_INTERVAL_MS + 2000,
		       5, &membership, links, 2) == 0);
	assert(!daemon.links[0].cost_pending);
	assert(owned_link(&daemon, &links[0]).metric == candidate_cost);

	/* Address-family/address changes bypass measurement suppression. */
	sequence = midr_owned_last_sequence(daemon.owned);
	membership = membership_event(6, 6, 9);
	links[0] = link_event(6, 6, 88, MIDR_CORE_AF_IPV6, 1100100, 6);
	links[1].generation = 6;
	links[1].fact.link.version = 6;
	assert(deliver_snapshot(
		       &daemon,
		       start + MIDR_LINK_COST_MIN_ADVERTISEMENT_INTERVAL_MS + 3000,
		       6, &membership, links, 2) == 0);
	object = owned_link(&daemon, &links[0]);
	assert(object.address_family == MIDR_CORE_AF_IPV6);
	assert(object.local_address[15] == 88);
	assert(midr_owned_last_sequence(daemon.owned) == sequence + 1U);

	/* Refresh republishes the advertised metric, never a suppressed latest
	 * candidate. */
	membership = membership_event(7, 7, 9);
	links[0] = link_event(7, 7, 88, MIDR_CORE_AF_IPV6, 1300000, 7);
	links[1].generation = 7;
	links[1].fact.link.version = 7;
	assert(deliver_snapshot(
		       &daemon,
		       start + MIDR_LINK_COST_MIN_ADVERTISEMENT_INTERVAL_MS + 4000,
		       7, &membership, links, 2) == 0);
	assert(daemon.links[0].cost_pending);
	baseline_cost = owned_link(&daemon, &links[0]).metric;
	refresh_owned(&daemon);
	assert(owned_link(&daemon, &links[0]).metric == baseline_cost);
	assert(daemon.links[0].candidate_metric != baseline_cost);

	/* A partial snapshot and an invalid object never alter committed state. */
	generation = midr_engine_generation(daemon.engine);
	sequence = midr_owned_last_sequence(daemon.owned);
	assert(local_event_at(&daemon,
			      &(struct midr_local_event){
				      .kind = MIDR_LOCAL_SNAPSHOT_BEGIN,
				      .generation = 8,
				      .originator = 77,
			      },
			      start + 400000) == 0);
	{
		struct midr_local_event invalid = link_event(
			8, 8, 90, MIDR_CORE_AF_IPV4, 1000000, 8);

		invalid.fact.link.local_address[4] = 1;
		assert(local_event_at(&daemon, &invalid, start + 400000) ==
		       -EINVAL);
	}
	local_ipc_disconnect(&daemon, -EBADMSG);
	assert(daemon.link_count == 2 && daemon.group_id == 9);
	assert(midr_engine_generation(daemon.engine) == generation);
	assert(midr_owned_last_sequence(daemon.owned) == sequence);
	destroy_daemon(&daemon);
}

static void test_failed_commits_and_version_floors(void)
{
	struct midrd daemon;
	struct consumer_probe probe;
	struct midr_local_event membership = membership_event(1, 1, 9);
	struct midr_local_event links[2];
	struct midr_core_object before;
	uint64_t generation;
	uint64_t view_generation;
	uint64_t sequence;
	uint64_t failed_sequence;
	size_t pending;
	size_t count;

	initialize_daemon(&daemon, &probe, 2);
	links[0] = link_event(1, 1, 88, MIDR_CORE_AF_IPV4, 1000000, 1);
	assert(deliver_snapshot(&daemon, 1000, 1, &membership, links, 1) == 0);
	before = owned_link(&daemon, &links[0]);
	generation = midr_engine_generation(daemon.engine);
	view_generation = consumer_generation(&daemon, &count);
	sequence = midr_owned_last_sequence(daemon.owned);

	membership = membership_event(2, 2, 9);
	links[0].generation = 2;
	links[0].fact.link.version = 2;
	links[1] = link_event(2, 1, 89, MIDR_CORE_AF_IPV6, 1000000, 1);
	assert(deliver_snapshot(&daemon, 2000, 2, &membership, links, 2) ==
	       -ENOSPC);
	assert(daemon.local_generation == 1 && daemon.link_count == 1);
	assert(midr_engine_generation(daemon.engine) == generation);
	assert(consumer_generation(&daemon, &count) == view_generation);
	assert(owned_link(&daemon, &links[0]).metric == before.metric);
	assert(midr_owned_last_sequence(daemon.owned) == sequence);
	assert_no_engine_events(&daemon);

	/* Consumer queue exhaustion is detected before publication.  The staged
	 * core and owned state are discarded, while the consumed sequence numbers
	 * remain reserved for the retry. */
	drain_consumer_events(daemon.consumer);
	pending = fill_consumer(daemon.consumer);
	membership = membership_event(3, 3, 10);
	links[0].generation = 3;
	links[0].fact.link.version = 3;
	links[0].fact.link.rtt_us = 1500000;
	assert(deliver_snapshot(&daemon, 3000, 3, &membership, links, 1) ==
	       -ENOSPC);
	assert(daemon.local_generation == 1 && daemon.group_id == 9);
	assert(daemon.links[0].input_version == 1);
	assert(midr_engine_generation(daemon.engine) == generation);
	assert(consumer_generation(&daemon, &count) == view_generation);
	assert(owned_link(&daemon, &links[0]).metric == before.metric);
	assert(midr_consumer_pending(daemon.consumer) == pending);
	assert_no_engine_events(&daemon);
	failed_sequence = midr_owned_last_sequence(daemon.owned);
	assert(failed_sequence > sequence);
	drain_consumer_events(daemon.consumer);
	assert(deliver_snapshot(&daemon, 3000, 3, &membership, links, 1) == 0);
	assert(daemon.local_generation == 3 && daemon.group_id == 10);
	assert(daemon.links[0].input_version == 3);
	assert(midr_owned_last_sequence(daemon.owned) > failed_sequence);

	/* Absence records an input-version floor; the same old version cannot
	 * resurrect the Link in the same provider epoch. */
	membership = membership_event(4, 4, 10);
	assert(deliver_snapshot(&daemon, 4000, 4, &membership, NULL, 0) == 0);
	assert(daemon.link_count == 0);
	membership = membership_event(5, 5, 10);
	links[0] = link_event(5, 3, 88, MIDR_CORE_AF_IPV4, 1000000, 5);
	assert(deliver_snapshot(&daemon, 5000, 5, &membership, links, 1) ==
	       -ESTALE);
	assert(daemon.local_generation == 4 && daemon.link_count == 0);
	links[0].fact.link.version = 4;
	assert(deliver_snapshot(&daemon, 6000, 5, &membership, links, 1) == 0);
	assert(daemon.link_count == 1 && daemon.links[0].input_version == 4);

	/* Membership absence keeps its version floor.  The same or an older
	 * version cannot recreate a removed Membership within this epoch. */
	assert(deliver_snapshot(&daemon, 7000, 6, NULL, links, 1) == 0);
	assert(daemon.group_id == 0 && daemon.membership_version == 5);
	membership = membership_event(7, 5, 10);
	assert(deliver_snapshot(&daemon, 8000, 7, &membership, links, 1) ==
	       -ESTALE);
	membership.fact.membership.version = 4;
	assert(deliver_snapshot(&daemon, 9000, 7, &membership, links, 1) ==
	       -ESTALE);
	membership.fact.membership.version = 6;
	assert(deliver_snapshot(&daemon, 10000, 7, &membership, links, 1) == 0);
	assert(daemon.group_id == 10 && daemon.membership_version == 6);

	/* A restarted provider begins a new input-version epoch only after a
	 * complete snapshot commits. */
	local_ipc_disconnect(&daemon, -ECONNRESET);
	membership = membership_event(1, 1, 9);
	links[0] = link_event(1, 1, 88, MIDR_CORE_AF_IPV4, 1000000, 1);
	assert(deliver_snapshot(&daemon, 11000, 1, &membership, links, 1) == 0);
	assert(daemon.local_generation == 1 && daemon.membership_version == 1);
	assert(daemon.links[0].input_version == 1);
	destroy_daemon(&daemon);
}

static void test_snapshot_generation_guards(void)
{
	struct midrd daemon;
	struct consumer_probe probe;
	struct midr_local_event membership = membership_event(1, 1, 9);
	struct midr_local_event link =
		link_event(1, 1, 88, MIDR_CORE_AF_IPV4, 1000000, 1);
	struct midr_local_event event;
	uint64_t generation;
	uint64_t view_generation;
	size_t count;

	initialize_daemon(&daemon, &probe, 8);
	assert(deliver_snapshot(&daemon, 1000, 1, &membership, &link, 1) == 0);
	generation = midr_engine_generation(daemon.engine);
	view_generation = consumer_generation(&daemon, &count);

	/* An old EOR cannot complete a newer active transaction. */
	event = local_control(MIDR_LOCAL_SNAPSHOT_BEGIN, 3);
	assert(local_event_at(&daemon, &event, 2000) == 0);
	membership = membership_event(3, 2, 10);
	assert(local_event_at(&daemon, &membership, 2000) == 0);
	event = local_control(MIDR_LOCAL_EOR, 2);
	assert(local_event_at(&daemon, &event, 2000) == 0);
	assert(daemon.local_stage.active && !daemon.local_stage.ended);
	assert(daemon.local_generation == 1);
	assert(midr_engine_generation(daemon.engine) == generation);
	assert(consumer_generation(&daemon, &count) == view_generation);
	event = local_control(MIDR_LOCAL_SNAPSHOT_END, 3);
	assert(local_event_at(&daemon, &event, 2000) == 0);
	event = local_control(MIDR_LOCAL_EOR, 3);
	assert(local_event_at(&daemon, &event, 2000) == 0);
	assert(daemon.local_generation == 3 && daemon.group_id == 10);

	/* Duplicate EOR and late objects are inert after commit. */
	generation = midr_engine_generation(daemon.engine);
	view_generation = consumer_generation(&daemon, &count);
	assert(local_event_at(&daemon, &event, 2100) == 0);
	link.generation = 2;
	link.fact.link.version = 2;
	assert(local_event_at(&daemon, &link, 2100) == -EINVAL);
	assert(midr_engine_generation(daemon.engine) == generation);
	assert(consumer_generation(&daemon, &count) == view_generation);

	/* Repeating BEGIN for the same generation explicitly replaces the partial
	 * stage; objects received before the second BEGIN are discarded. */
	event = local_control(MIDR_LOCAL_SNAPSHOT_BEGIN, 4);
	assert(local_event_at(&daemon, &event, 3000) == 0);
	link.generation = 4;
	link.fact.link.version = 2;
	assert(local_event_at(&daemon, &link, 3000) == 0);
	assert(local_event_at(&daemon, &event, 3000) == 0);
	membership = membership_event(4, 3, 11);
	assert(local_event_at(&daemon, &membership, 3000) == 0);
	event = local_control(MIDR_LOCAL_SNAPSHOT_END, 4);
	assert(local_event_at(&daemon, &event, 3000) == 0);
	event = local_control(MIDR_LOCAL_EOR, 4);
	assert(local_event_at(&daemon, &event, 3000) == 0);
	assert(daemon.local_generation == 4 && daemon.group_id == 11);
	assert(daemon.link_count == 0);
	destroy_daemon(&daemon);
}

int main(void)
{
	test_atomic_and_cost();
	test_failed_commits_and_version_floors();
	test_snapshot_generation_guards();
	puts("midrd-local-transaction-test: PASS");
	return 0;
}
