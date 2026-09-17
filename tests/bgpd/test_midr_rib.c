// SPDX-License-Identifier: GPL-2.0-or-later
/* MIDR canonical RIB tests. */

#include <zebra.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>

#include "command.h"
#include "privs.h"
#include "qobj.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_attr.h"
#include "bgpd/bgp_midr_canonical.h"
#include "bgpd/bgp_midr_lsdb.h"
#include "bgpd/bgp_midr_private.h"
#include "bgpd/bgp_midr_rib.h"
#include "bgpd/bgp_network.h"
#include "bgpd/bgp_vty.h"

struct zebra_privs_t bgpd_privs = {};
struct event_loop *master;

static struct bgp *bgp;
static struct midr_context *ctx;
static struct peer *peer_two;
static struct peer *peer_three;

static uint32_t router_id(const char *text)
{
	struct in_addr address;

	assert(inet_pton(AF_INET, text, &address) == 1);
	return address.s_addr;
}

static struct peer *test_peer(const char *text)
{
	struct peer *peer = peer_create_accept(bgp, NULL);

	assert(peer);
	peer->remote_id.s_addr = router_id(text);
	return peer;
}

static struct midr_instance membership(uint32_t originator, uint64_t sequence,
				       uint32_t group_id)
{
	return (struct midr_instance){
		.state = MIDR_INSTANCE_ACTIVE,
		.object = {
			.key = {
				.type = MIDR_NLRI_TYPE_MEMBERSHIP,
				.originator_node_id = originator,
			},
			.ls_sequence = sequence,
			.payload.membership = {
				.group_id = group_id,
				.cap_flags = 1,
			},
		},
	};
}

static struct midr_instance withdrawal(const struct midr_instance *active,
					       uint64_t sequence)
{
	return (struct midr_instance){
		.state = MIDR_INSTANCE_WITHDRAWN,
		.object = {
			.key = active->object.key,
			.ls_sequence = sequence,
		},
	};
}

static struct peer *selected_peer(const struct midr_ls_object_key *key,
					 uint64_t *sequence,
					 enum midr_instance_state *state)
{
	struct midr_instance instance;
	struct peer *peer;
	uint32_t age;

	assert(midr_rib_selected_instance_get(ctx, key, &instance, &age, &peer) == 0);
	if (sequence)
		*sequence = instance.object.ls_sequence;
	if (state)
		*state = instance.state;
	return peer;
}

struct selected_entry_lookup {
	const struct midr_ls_object_key *key;
	struct midr_instance instance;
	struct peer *peer;
	bool found;
};

static int selected_entry_find(const struct midr_instance *instance,
				       struct peer *peer, struct bgp_dest *dest,
				       struct bgp_path_info *selected, void *arg)
{
	struct selected_entry_lookup *lookup = arg;

	(void)dest;
	(void)selected;
	if (!midr_ls_object_key_same(&instance->object.key, lookup->key))
		return 0;
	lookup->instance = *instance;
	lookup->peer = peer;
	lookup->found = true;
	return 0;
}

static void selected_flooding(const struct midr_ls_object_key *key,
				      struct midr_instance *instance,
				      struct peer **peer)
{
	struct selected_entry_lookup lookup = {.key = key};

	assert(midr_rib_selected_entry_foreach(ctx, selected_entry_find,
						&lookup) == 0);
	assert(lookup.found);
	*instance = lookup.instance;
	*peer = lookup.peer;
}

static void test_sequence_admission_and_peer_withdraw(void)
{
	struct midr_instance current = membership(router_id("1.1.1.1"), 10, 10);
	struct midr_instance older = current;
	struct midr_instance newer = current;
	struct midr_instance withdrawn;
	uint64_t sequence;
	enum midr_instance_state state;
	struct midr_instance flooding;
	struct peer *flooding_peer;

	older.object.ls_sequence = 9;
	newer.object.ls_sequence = 11;
	newer.object.payload.membership.cap_flags = 2;
	assert(midr_rib_instance_upsert(ctx, peer_two, &current, 0) == 0);
	assert(selected_peer(&current.object.key, &sequence, &state) == peer_two);
	assert(sequence == 10 && state == MIDR_INSTANCE_ACTIVE);
	assert(midr_rib_instance_upsert(ctx, peer_three, &current, 0) == 0);
	assert(midr_rib_peer_advertisement_has(ctx, &current.object.key, peer_three));
	assert(selected_peer(&current.object.key, NULL, NULL) == peer_two);
	assert(midr_rib_instance_upsert(ctx, peer_two, &older, 0) == 0);
	assert(selected_peer(&current.object.key, &sequence, NULL) == peer_two);
	assert(sequence == 10);
	assert(midr_rib_instance_upsert(ctx, peer_three, &newer, 0) == 0);
	assert(selected_peer(&current.object.key, &sequence, NULL) == peer_three);
	assert(sequence == 11);

	/* A peer disconnect removes only its advertisement relationship.  The
	 * canonical path and another peer's relationship remain intact. */
	midr_rib_peer_cleanup(ctx, peer_three);
	assert(!midr_rib_peer_advertisement_has(ctx, &current.object.key,
					       peer_three));
	assert(midr_rib_peer_advertisement_has(ctx, &current.object.key,
					       peer_two));
	assert(selected_peer(&current.object.key, &sequence, NULL) == peer_three);
	assert(sequence == 11);
	midr_rib_peer_cleanup(ctx, peer_three);
	assert(selected_peer(&current.object.key, NULL, NULL) == peer_three);
	assert(midr_rib_instance_upsert(ctx, peer_three, &newer, 0) == 0);

	/* MP_UNREACH only removes the sender's advertisement relationship. */
	assert(midr_rib_peer_withdraw(ctx, peer_three, &current.object.key) == 0);
	assert(!midr_rib_peer_advertisement_has(ctx, &current.object.key, peer_three));
	assert(selected_peer(&current.object.key, &sequence, NULL) == peer_three);
	assert(sequence == 11);

	withdrawn = withdrawal(&current, 12);
	assert(midr_rib_instance_upsert(ctx, peer_two, &withdrawn, 0) == 0);
	/* The active accessor hides WITHDRAWN; the flooding view retains it. */
	assert(midr_rib_selected_instance_get(ctx, &current.object.key,
						 &(struct midr_instance){}, &(uint32_t){0},
						 &(struct peer *){0}) == -ENOENT);
	selected_flooding(&current.object.key, &flooding, &flooding_peer);
	assert(flooding_peer == peer_two);
	assert(flooding.object.ls_sequence == 12 &&
	       flooding.state == MIDR_INSTANCE_WITHDRAWN);
	assert(midr_lsdb_test_process(ctx) == 0);
	assert(midr_rib_peer_withdraw(ctx, peer_two, &current.object.key) == 0);
	assert(midr_rib_peer_withdraw(ctx, peer_two, &current.object.key) == -ENOENT);
	assert(midr_lsdb_test_process(ctx) == 0);
}

static void test_duplicate_conflict_and_resolution(void)
{
	struct midr_instance first = membership(router_id("2.2.2.2"), 20, 20);
	struct midr_instance duplicate = first;
	struct midr_instance conflict = first;
	struct midr_rib_summary summary;

	assert(midr_rib_instance_upsert(ctx, peer_two, &first, 0) == 0);
	assert(midr_rib_instance_upsert(ctx, peer_two, &duplicate, 0) == 0);
	assert(midr_rib_summary_get(ctx, &summary) == 0);
	assert(summary.conflict_count == 0 && summary.selected_count == 2);

	conflict.object.payload.membership.group_id = 21;
	assert(midr_rib_instance_upsert(ctx, peer_three, &conflict, 0) == 0);
	assert(midr_rib_summary_get(ctx, &summary) == 0);
	assert(summary.conflict_count == 1 && summary.selected_count == 1);

	conflict.object.ls_sequence++;
	assert(midr_rib_instance_upsert(ctx, peer_three, &conflict, 0) == 0);
	assert(selected_peer(&first.object.key, NULL, NULL) == peer_three);
	assert(midr_rib_summary_get(ctx, &summary) == 0);
	assert(summary.conflict_count == 0 && summary.selected_count == 2);

	assert(midr_rib_peer_withdraw(ctx, peer_two, &first.object.key) == 0);
	assert(midr_rib_peer_withdraw(ctx, peer_three, &first.object.key) == 0);
}

struct foreach_state {
	size_t count;
	int result;
};

static int selected_callback(const struct midr_instance *instance,
				     struct peer *peer, void *arg)
{
	struct foreach_state *state = arg;

	assert(instance && peer);
	state->count++;
	return state->result;
}

static int selected_entry_callback(const struct midr_instance *instance,
					   struct peer *peer, struct bgp_dest *dest,
					   struct bgp_path_info *selected, void *arg)
{
	struct foreach_state *state = arg;

	assert(instance && peer && dest && selected);
	state->count++;
	return state->result;
}

static void test_iteration_limits_and_validation(void)
{
	struct midr_instance first = membership(router_id("3.3.3.3"), 1, 30);
	struct midr_instance second = membership(router_id("4.4.4.4"), 1, 40);
	struct foreach_state state = {};
	struct midr_rib_summary summary;
	struct midr_ls_object_key unknown = second.object.key;
	unknown.originator_node_id = router_id("5.5.5.5");

	assert(midr_rib_instance_upsert(ctx, peer_two, &first, 0) == 0);
	assert(midr_rib_instance_upsert(ctx, peer_two, &second, 0) == 0);
	assert(midr_rib_selected_foreach(ctx, selected_callback, &state) == 0);
	assert(state.count == 3);
	state.count = 0;
	assert(midr_rib_selected_entry_foreach(ctx, selected_entry_callback, &state) == 0);
	assert(state.count == 4);
	state.count = 0;
	state.result = -ECANCELED;
	assert(midr_rib_selected_foreach(ctx, selected_callback, &state) == -ECANCELED);
	assert(state.count == 1);
	assert(midr_rib_selected_foreach(ctx, NULL, NULL) == -EINVAL);
	assert(midr_rib_selected_entry_foreach(ctx, NULL, NULL) == -EINVAL);
	assert(midr_rib_selected_instance_get(ctx, &unknown, &(struct midr_instance){},
						       &(uint32_t){0}, &(struct peer *){0}) == -ENOENT);
	assert(midr_rib_summary_get(ctx, &summary) == 0 && summary.identity_count == 4);
	assert(midr_rib_test_set_identity_limit(ctx, 0) == -EINVAL);
	assert(midr_rib_test_set_identity_limit(
		       ctx, MIDR_RIB_MAX_IDENTITIES + 1) == -EINVAL);
	assert(midr_rib_test_set_identity_limit(ctx, MIDR_RIB_MAX_IDENTITIES) == 0);
	assert(midr_rib_peer_withdraw(ctx, peer_two, &first.object.key) == 0);
	assert(midr_rib_peer_withdraw(ctx, peer_two, &second.object.key) == 0);
}

static void test_identity_limit_rejection_and_recovery(void)
{
	struct midr_instance extra = membership(router_id("6.6.6.6"), 1, 60);
	struct midr_instance update = membership(router_id("3.3.3.3"), 99, 31);
	struct midr_instance selected;
	struct midr_rib_summary summary;
	struct peer *peer;
	uint32_t age;
	uint64_t rejected;

	assert(midr_rib_summary_get(ctx, &summary) == 0);
	rejected = summary.rejected_limit;

	/* Lowering the limit below the live count only blocks new identities;
	 * updates of existing identities stay admissible. */
	assert(midr_rib_test_set_identity_limit(ctx, 1) == 0);
	assert(midr_rib_instance_upsert(ctx, peer_two, &update, 0) == 0);
	assert(midr_rib_instance_upsert(ctx, peer_two, &extra, 0) == -ENOSPC);
	assert(midr_rib_summary_get(ctx, &summary) == 0);
	assert(summary.rejected_limit == rejected + 1);
	assert(midr_rib_selected_instance_get(ctx, &extra.object.key, &selected,
					      &age, &peer) == -ENOENT);

	/* Restoring capacity admits the previously rejected identity. */
	assert(midr_rib_test_set_identity_limit(ctx, MIDR_RIB_MAX_IDENTITIES) == 0);
	assert(midr_rib_instance_upsert(ctx, peer_two, &extra, 0) == 0);
	assert(midr_rib_selected_instance_get(ctx, &extra.object.key, &selected,
					      &age, &peer) == 0);
	assert(selected.object.ls_sequence == 1);
}

/* One lifetime cycle at the given virtual time: sweep -> expire/reap ->
 * (optional) floor reclaim through the canonical GC callback. */
static void run_lifetime_at(uint64_t now_ns)
{
	assert(midr_rib_test_set_now_ns(ctx, now_ns) == 0);
	assert(midr_rib_test_run_lifetime(ctx) == 0);
}

static void test_lifetime_event_capacity_failure(void)
{
	const uint64_t life_ns =
		(uint64_t)MIDR_CANONICAL_MAX_AGE_MS * 1000000ULL;
	const struct midr_instance object =
		membership(router_id("12.12.12.12"), 1, 12);
	struct midr_rib_summary before, after;
	struct timespec ts;
	uint64_t now_ns;

	assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
	now_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec + life_ns;
	assert(midr_rib_summary_get(ctx, &before) == 0);
	assert(midr_rib_instance_upsert(ctx, peer_two, &object, 0) == 0);
	assert(midr_lsdb_test_process(ctx) == 0);
	assert(midr_rib_test_set_event_limit(ctx, 0) == 0);
	assert(midr_rib_test_set_now_ns(ctx, now_ns) == 0);
	assert(midr_rib_test_run_lifetime(ctx) == -ENOSPC);
	assert(midr_rib_summary_get(ctx, &after) == 0);
	assert(after.lifetime_failures == before.lifetime_failures + 1);
	assert(after.last_lifetime_error == -ENOSPC);
	assert(midr_rib_test_set_event_limit(ctx, MIDR_RIB_MAX_IDENTITIES * 2U) == 0);
	assert(midr_rib_test_run_lifetime(ctx) == 0);
}

static void test_identity_reclaim_and_slot_reuse(void)
{
	const uint64_t life_ns =
		(uint64_t)MIDR_CANONICAL_MAX_AGE_MS * 1000000ULL;
	struct midr_instance stub = membership(router_id("9.9.9.9"), 1, 90);
	struct midr_rib_summary summary;
	struct foreach_state state = {};
	struct timespec ts;
	uint64_t reclaimed_base;
	uint64_t base_ns;
	size_t baseline;

	/* Anchor the virtual clock just past the real monotonic time so the
	 * identities left by earlier tests still age forward, never regress. */
	assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
	base_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec +
		  1000000000ULL;
	assert(midr_rib_test_set_now_ns(ctx, base_ns) == 0);

	assert(midr_rib_summary_get(ctx, &summary) == 0);
	baseline = summary.identity_count;
	reclaimed_base = summary.identities_reclaimed;

	/* Expire everything, including identities left by earlier tests. */
	run_lifetime_at(base_ns + life_ns);
	assert(midr_rib_summary_get(ctx, &summary) == 0);
	assert(summary.identity_count == baseline);
	assert(summary.path_count == 0 && summary.selected_count == 0);
	assert(midr_rib_selected_foreach(ctx, selected_callback, &state) == 0);
	assert(state.count == 0);
	/* The first pass only expires the path and creates the LSDB dirty
	 * obligation.  Reclamation must wait until that commit drains. */
	assert(midr_lsdb_test_process(ctx) == 0);

	/* GC stays off by default: floors past their retention window are
	 * still retained, exactly as production would keep them. */
	run_lifetime_at(base_ns + 2 * life_ns + 1);
	assert(midr_rib_summary_get(ctx, &summary) == 0);
	assert(summary.identity_count == baseline);
	assert(summary.identities_reclaimed == reclaimed_base);
	assert(summary.canonical_identity_count == baseline);

	/* With GC enabled the RIB identities, canonical floors and synthetic
	 * IDs are reclaimed together and the slots become reusable. */
	assert(midr_rib_test_set_gc_enabled(ctx, true) == 0);
	run_lifetime_at(base_ns + 2 * life_ns + 2);
	assert(midr_rib_summary_get(ctx, &summary) == 0);
	assert(summary.identity_count == 0);
	assert(summary.canonical_identity_count == 0);
	assert(summary.identities_reclaimed == reclaimed_base + baseline);
	state.count = 0;
	assert(midr_rib_selected_entry_foreach(ctx, selected_entry_callback,
					       &state) == 0);
	assert(state.count == 0);

	/* Churn: with a tiny limit, repeated fill/expire/reclaim rounds must
	 * not permanently exhaust the identity budget.  Floor retention is
	 * measured from acceptance, so expiry and reclaimability coincide:
	 * one lifetime pass with GC enabled drops each round completely. */
	assert(midr_rib_test_set_identity_limit(ctx, 3) == 0);
	for (unsigned int round = 0; round < 3; round++) {
		uint64_t round_ns = base_ns + (round + 4) * 4 * life_ns;
		struct midr_instance batch[3];
		uint64_t reclaimed_before;

		for (unsigned int n = 0; n < 3; n++)
			batch[n] = membership(
				htonl(0x14000001 + round * 16 + n),
				round * 10 + n + 1, 30 + n);
		assert(midr_rib_summary_get(ctx, &summary) == 0);
		reclaimed_before = summary.identities_reclaimed;
		assert(midr_rib_test_set_now_ns(ctx, round_ns) == 0);
		for (unsigned int n = 0; n < 3; n++)
			assert(midr_rib_instance_upsert(ctx, peer_two,
							&batch[n], 0) == 0);
		/* Mid-life nothing has expired yet (age accounting rounds
		 * elapsed nanoseconds up to whole milliseconds). */
		run_lifetime_at(round_ns + life_ns / 2);
		assert(midr_rib_summary_get(ctx, &summary) == 0);
		assert(summary.identity_count == 3);
		assert(summary.path_count == 3);
		/* At L the round expires and is reclaimed in the same pass. */
		run_lifetime_at(round_ns + life_ns);
	assert(midr_lsdb_test_process(ctx) == 0);
		run_lifetime_at(round_ns + life_ns + 1);
		assert(midr_rib_summary_get(ctx, &summary) == 0);
		assert(summary.identity_count == 0);
		assert(summary.identities_reclaimed == reclaimed_before + 3);
	}
	assert(midr_rib_test_set_identity_limit(ctx, MIDR_RIB_MAX_IDENTITIES) == 0);

	/* A resource-rejected or already-expired input must not leave an
	 * empty identity stub behind: those have no canonical entry, so no
	 * floor-GC callback could ever reach them. */
	assert(midr_rib_summary_get(ctx, &summary) == 0);
	baseline = summary.identity_count;
	assert(midr_rib_instance_upsert(ctx, peer_two, &stub,
					MIDR_CANONICAL_MAX_AGE_MS) == 0);
	assert(midr_rib_summary_get(ctx, &summary) == 0);
	assert(summary.identity_count == baseline);
	assert(midr_rib_test_set_identity_limit(ctx, 1) == 0);
	{
		struct midr_instance live = membership(router_id("7.7.7.7"), 5, 70);

		assert(midr_rib_instance_upsert(ctx, peer_two, &live, 0) == 0);
		baseline++;
	}
	stub.object.key.originator_node_id = router_id("9.9.9.8");
	assert(midr_rib_instance_upsert(ctx, peer_two, &stub, 0) == -ENOSPC);
	assert(midr_rib_test_set_identity_limit(ctx, MIDR_RIB_MAX_IDENTITIES) == 0);
	assert(midr_rib_summary_get(ctx, &summary) == 0);
	assert(summary.identity_count == baseline);
}

int main(void)
{
	as_t asn = 65000;

	qobj_init();
	cmd_init(0);
	bgp_vty_init();
	master = event_master_create("test MIDR RIB");
	bgp_master_init(master, BGP_SOCKET_SNDBUF_SIZE, list_new());
	vrf_init(NULL, NULL, NULL, NULL);
	bgp_option_set(BGP_OPT_NO_LISTEN);
	bgp_attr_init();
	assert(bgp_get(&bgp, &asn, NULL, BGP_INSTANCE_TYPE_DEFAULT, NULL,
		       ASNOTATION_PLAIN) >= 0);
	bgp->router_id.s_addr = router_id("10.0.0.1");
	ctx = &bgp->midr_info->ctx;
	peer_two = test_peer("2.2.2.2");
	peer_three = test_peer("3.3.3.3");

	test_sequence_admission_and_peer_withdraw();
	test_duplicate_conflict_and_resolution();
	test_iteration_limits_and_validation();
	test_identity_limit_rejection_and_recovery();
	test_lifetime_event_capacity_failure();
	test_identity_reclaim_and_slot_reuse();
	puts("MIDR RIB tests passed");
	return 0;
}
