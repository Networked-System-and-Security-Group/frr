/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Included AFTER the unchanged production CL source by run_test.py. */

static struct test_owner owner;
static struct test_nds nds;
struct midr_global_view view;
static uint32_t next_id;
static unsigned int failures, checks;
static const char *case_name;

static struct prefix node_id(uint32_t id)
{
	struct prefix prefix = { .family = AF_INET, .prefixlen = 32 };
	prefix.u.prefix4.s_addr = htonl(0x0a000000U + id);
	return prefix;
}

static void reset_fixture(void)
{
	memset(&owner, 0, sizeof(owner));
	memset(&nds, 0, sizeof(nds));
	memset(&view, 0, sizeof(view));
	memset(decisions, 0, sizeof(decisions));
	decision_count = blocked_count = 0;
	request_count = 0;
	execute_nds = false;
	next_id = 1;
	nds.rep_dir = list_new();
	nds.bootstrap_list = list_new();
	view.nodes = list_new();
	view.links = list_new();
	owner.midr_nds_info = &nds;
	nds.join_phase = MIDR_JOIN_PROBING_REPS;
	midr_cl_init(&owner);
	assert(nds.cl_callback);
}

static void free_entries(struct list **list)
{
	struct listnode *node;
	void *data;
	for (ALL_LIST_ELEMENTS_RO(*list, node, data))
		free(data);
	list_delete(list);
}

static void destroy_fixture(void)
{
	event_cancel(&nds.t_member_probe_done);
	event_cancel(&nds.t_rep_probe_done);
	event_cancel(&nds.t_rep_refresh);
	event_cancel(&nds.t_anchor_probe_done);
	free_entries(&nds.rep_dir);
	free_entries(&nds.bootstrap_list);
	free_entries(&view.nodes);
	free_entries(&view.links);
}

static struct midr_node_entry *add_node(uint32_t gid, uint32_t rtt,
				       double loss, bool adjacent)
{
	struct midr_node_entry *node = test_calloc(1, sizeof(*node));
	struct midr_link_entry *link = test_calloc(1, sizeof(*link));
	node->node_id = node_id(next_id++);
	node->group_id = gid;
	node->is_adjacent = adjacent;
	node->has_transport_addr = true;
	node->transport_addr.v4 = node->node_id.u.prefix4.s_addr;
	link->remote_node_id = node->node_id;
	link->status = MIDR_LINK_UP;
	link->long_term.rtt_us = rtt;
	link->long_term.loss_rate = loss;
	link->long_term.bw_score = 100;
	listnode_add(view.nodes, node);
	listnode_add(view.links, link);
	return node;
}

static struct midr_link_entry *link_for(const struct midr_node_entry *node)
{
	return cl_find_link_by_prefix(&view, &node->node_id);
}

static struct midr_rep_entry *add_rep(uint32_t gid, uint32_t rtt)
{
	struct midr_node_entry *node = add_node(gid, rtt, 0.0, false);
	struct midr_rep_entry *rep = test_calloc(1, sizeof(*rep));
	rep->group_id = gid;
	rep->rep_rid = node->node_id.u.prefix4;
	rep->rep_transport = node->transport_addr;
	listnode_add(nds.rep_dir, rep);
	return rep;
}

static void add_members(uint32_t gid, unsigned int count, uint32_t rtt,
			double loss, bool adjacent)
{
	for (unsigned int i = 0; i < count; i++)
		add_node(gid, rtt, loss, adjacent);
}

static void block_rep(const struct midr_rep_entry *rep)
{
	assert(blocked_count < array_size(blocked));
	blocked[blocked_count++] = rep->rep_transport;
}

static void trigger(enum midr_trigger_type event)
{
	nds.cl_callback(&owner, event, &view);
}

static const char *decision_name(enum midr_decision_type type)
{
	switch (type) {
	case MIDR_DECISION_RECOMMEND: return "RECOMMEND";
	case MIDR_DECISION_JOIN: return "JOIN";
	case MIDR_DECISION_CREATE: return "CREATE";
	case MIDR_DECISION_LEAVE: return "LEAVE";
	case MIDR_DECISION_REJOIN: return "REJOIN";
	case MIDR_DECISION_REP_RESIGN: return "REP_RESIGN";
	default: return "OTHER";
	}
}

static void expect_decision(size_t count, enum midr_decision_type type,
			    uint32_t gid)
{
	checks++;
	if (decision_count == count && decisions[count - 1].type == type &&
	    decisions[count - 1].gid == gid) {
		printf("PASS %s: %s(%u)\n", case_name, decision_name(type), gid);
		return;
	}
	failures++;
	printf("FAIL %s: expected %zu decisions ending in %s(%u); actual ",
	       case_name, count, decision_name(type), gid);
	if (!decision_count)
		printf("no decision");
	for (size_t i = 0; i < decision_count; i++)
		printf("%s%s(%u)", i ? " -> " : "",
		       decision_name(decisions[i].type), decisions[i].gid);
	printf("\n");
}

static void expect_state(bool condition, const char *description)
{
	checks++;
	if (!condition)
		failures++;
	printf("%s %s: %s\n", condition ? "PASS" : "FAIL", case_name, description);
}

static bool in_batch(uint32_t gid)
{
	for (size_t i = 0; i < MIDR_JOIN_BATCH_MAX; i++)
		if (nds.join_batch_gid[i] == gid)
			return true;
	return false;
}

static bool batch_is(uint32_t a, uint32_t b, uint32_t c)
{
	return nds.join_batch_gid[0] == a && nds.join_batch_gid[1] == b &&
	       nds.join_batch_gid[2] == c;
}

static bool group_listed(uint32_t gid)
{
	struct listnode *n;
	struct midr_rep_entry *rep;
	for (ALL_LIST_ELEMENTS_RO(nds.rep_dir, n, rep))
		if (rep->group_id == gid && rep->members_listed)
			return true;
	return false;
}

/* Anchor groups attached to decision index (1-based), in order; 0 = none. */
static bool anchors_are(size_t index, uint32_t a, uint32_t b)
{
	size_t count = (a != 0) + (b != 0);
	return decisions[index - 1].anchor_count == count &&
	       (!a || decisions[index - 1].anchors[0].group_id == a) &&
	       (!b || decisions[index - 1].anchors[1].group_id == b);
}

static void deliver_member_list(uint32_t gid)
{
	struct listnode *n;
	struct midr_node_entry *node;
	struct midr_rep_entry *rep;
	assert(nds.join_phase == MIDR_JOIN_PROBING_MEMBERS && in_batch(gid));
	/* Model the validated list input (midr_ctrl_recv_member_list): members
	 * become join candidates and the group is marked listed. Parsing/PM
	 * remain outside this harness. */
	for (ALL_LIST_ELEMENTS_RO(view.nodes, n, node))
		if (node->group_id == gid)
			node->is_adjacent = true;
	for (ALL_LIST_ELEMENTS_RO(nds.rep_dir, n, rep))
		if (rep->group_id == gid)
			rep->members_listed = true;
}

static void fire_member_timer(void)
{
	struct event event;
	assert(nds.t_member_probe_done);
	event = *nds.t_member_probe_done;
	event_cancel(&nds.t_member_probe_done);
	event.callback(&event);
}

/* The sink does not execute NDS. Advance its inputs explicitly after verifying
 * the selected batch, just as the RECOMMEND consumer would do, and deliver
 * every candidate's member list. */
static bool select_first_group(void)
{
	trigger(MIDR_TRIGGER_REP_PROBE_DONE);
	if (decision_count != 1 || decisions[0].type != MIDR_DECISION_RECOMMEND ||
	    decisions[0].gid != 1) {
		expect_decision(1, MIDR_DECISION_RECOMMEND, 1);
		return false;
	}
	nds.join_phase = MIDR_JOIN_PROBING_MEMBERS;
	nds.join_group_id = 1;
	for (size_t i = 0; i < decisions[0].candidate_count; i++)
		nds.join_batch_gid[i] = decisions[0].candidates[i];
	for (size_t i = 0; i < decisions[0].candidate_count; i++)
		deliver_member_list(decisions[0].candidates[i]);
	return true;
}

static void baseline(void)
{
	/* No early exit just because the first directory entry has no PM data. */
	for (unsigned int down = 0; down < 2; down++) {
		reset_fixture();
		case_name = down ? "first-rep-down-scan-next" : "first-rep-unprobed-scan-next";
		add_rep(1, down ? 1000 : 0);
		if (down)
			((struct midr_link_entry *)view.links->head->data)->status = MIDR_LINK_DOWN;
		add_rep(2, 2000);
		trigger(MIDR_TRIGGER_REP_PROBE_DONE);
		expect_decision(1, MIDR_DECISION_RECOMMEND, 2);
		destroy_fixture();
	}

	reset_fixture();
	case_name = "blocked-best-rep-scan-next";
	block_rep(add_rep(1, 1000));
	add_rep(2, 2000);
	trigger(MIDR_TRIGGER_REP_PROBE_DONE);
	expect_decision(1, MIDR_DECISION_RECOMMEND, 2);
	destroy_fixture();

	reset_fixture();
	case_name = "directory-order-does-not-override-rtt";
	add_rep(1, 9000);
	add_rep(2, 2000);
	add_rep(3, 6000);
	trigger(MIDR_TRIGGER_REP_PROBE_DONE);
	expect_decision(1, MIDR_DECISION_RECOMMEND, 2);
	destroy_fixture();

	/* Exercise all tiebreaks through real representative selection. */
	for (unsigned int tie = 0; tie < 3; tie++) {
		struct midr_rep_entry *second;
		struct midr_link_entry *first_link, *second_link;
		reset_fixture();
		case_name = tie == 0 ? "equal-rtt-prefers-lower-loss" :
			    tie == 1 ? "equal-rtt-loss-prefers-bandwidth" :
				       "equal-metrics-prefers-smaller-group-id";
		add_rep(2, 1000);
		second = add_rep(1, 1000);
		first_link = view.links->head->data;
		second_link = view.links->tail->data;
		if (tie == 0)
			first_link->long_term.loss_rate = 0.01;
		else if (tie == 1)
			second_link->long_term.bw_score = 101;
		trigger(MIDR_TRIGGER_REP_PROBE_DONE);
		expect_decision(1, MIDR_DECISION_RECOMMEND, second->group_id);
		destroy_fixture();
	}

	for (unsigned int count = 1; count <= 5; count += 4) {
		reset_fixture();
		case_name = count == 1 ? "small-eligible-group-joins" : "five-good-links-join";
		add_rep(1, 1000);
		add_members(1, count, 5000, 0.01, true);
		if (select_first_group()) {
			trigger(MIDR_TRIGGER_MEMBER_PROBE_DONE);
			expect_decision(2, MIDR_DECISION_JOIN, 1);
		}
		destroy_fixture();
	}

	reset_fixture();
	case_name = "empty-rep-directory-creates";
	trigger(MIDR_TRIGGER_REP_PROBE_DONE);
	expect_decision(1, MIDR_DECISION_CREATE, 1);
	destroy_fixture();

	reset_fixture();
	case_name = "all-representatives-blocked-create";
	block_rep(add_rep(1, 1000));
	block_rep(add_rep(2, 2000));
	trigger(MIDR_TRIGGER_REP_PROBE_DONE);
	expect_decision(1, MIDR_DECISION_CREATE, 3);
	destroy_fixture();

	reset_fixture();
	case_name = "only-group-fails-create";
	add_rep(1, 1000);
	add_members(1, 5, 25000, 0.01, true);
	if (select_first_group()) {
		trigger(MIDR_TRIGGER_MEMBER_PROBE_DONE);
		expect_decision(2, MIDR_DECISION_CREATE, 2);
	}
	destroy_fixture();

	/* Positive control: the exact successor fixture used below really joins
 * once its member list becomes the main candidate, rather than an anchor. */
	reset_fixture();
	case_name = "successor-group-is-eligible-after-member-probing";
	add_rep(1, 1000);
	add_rep(2, 2000);
	add_members(1, 5, 25000, 0.01, false);
	add_members(2, 5, 5000, 0.01, true);
	nds.join_phase = MIDR_JOIN_PROBING_MEMBERS;
	nds.join_group_id = 2;
	nds.join_batch_gid[0] = 2;
	deliver_member_list(2);
	trigger(MIDR_TRIGGER_MEMBER_PROBE_DONE);
	expect_decision(1, MIDR_DECISION_JOIN, 2);
	destroy_fixture();
}

static void regression(void)
{
	const char *names[] = {
		"bad-member-rtt-join-second-group-in-batch",
		"bad-member-loss-join-second-group-in-batch",
		"four-of-five-good-links-join-second-group-in-batch",
		"empty-first-member-list-join-second-group-in-batch",
		"bad-first-group-blocked-second-join-third-in-batch",
	};
	for (unsigned int scenario = 0; scenario < array_size(names); scenario++) {
		reset_fixture();
		case_name = names[scenario];
		add_rep(1, 1000); /* Best representative, but group members fail. */
		add_rep(2, 2000);
		if (scenario == 4) {
			block_rep(nds.rep_dir->tail->data);
			add_rep(3, 3000);
		}
		if (scenario == 0 || scenario == 4)
			add_members(1, 5, 20000, 0.01, true); /* strict RTT bound */
		else if (scenario == 1)
			add_members(1, 5, 5000, 0.05, true); /* strict loss bound */
		else if (scenario == 2) {
			/* The representative is a listed member too: 4 good of 5. */
			add_members(1, 3, 5000, 0.01, true);
			link_for(add_node(1, 5000, 0.01, true))->status = MIDR_LINK_DOWN;
		}

		/* The runner-up is evaluated in the same batch window; its members
		 * count only toward its own group, never toward group 1. */
		add_members(scenario == 4 ? 3 : 2, 5, 5000, 0.01, false);
		if (select_first_group()) {
			if (scenario == 3) {
				/* An empty MEMBER_LIST_RESP is dropped by Control, so
				 * group 1 is never marked listed. */
				((struct midr_rep_entry *)nds.rep_dir->head->data)
					->members_listed = false;
			}
			trigger(MIDR_TRIGGER_MEMBER_PROBE_DONE);
			expect_decision(2, MIDR_DECISION_JOIN, scenario == 4 ? 3 : 2);
		}
		destroy_fixture();
	}
}

static void state_flow(void)
{
	/* Real CL -> real NDS RECOMMEND case -> real Control request/timer. */
	reset_fixture();
	case_name = "batch-of-three-joins-second-with-anchors-first-and-third";
	execute_nds = true;
	for (uint32_t gid = 1; gid <= 4; gid++)
		add_rep(gid, gid * 1000);
	add_members(1, 5, 25000, 0.01, false);
	add_members(2, 5, 5000, 0.01, false);
	add_members(3, 5, 5000, 0.01, false);
	trigger(MIDR_TRIGGER_REP_PROBE_DONE);
	expect_decision(1, MIDR_DECISION_RECOMMEND, 1);
	expect_state(nds.join_phase == MIDR_JOIN_PROBING_MEMBERS &&
		     nds.join_group_id == 1 && batch_is(1, 2, 3) &&
		     nds.t_member_probe_done && request_count == 3 &&
		     requested_groups[0] == 1 && requested_groups[1] == 2 &&
		     requested_groups[2] == 3,
		     "NDS requests the whole batch with one list-wait timer");
	{
		struct midr_cluster_decision late = {
			.decision_type = MIDR_DECISION_RECOMMEND,
			.new_group_id = 4,
			.recommended_rep_id = node_id(4),
		};
		size_t before = request_count;
		test_apply_recommend(&owner, &late);
		expect_state(batch_is(1, 2, 3) && request_count == before,
			     "a RECOMMEND during member evaluation is ignored");
	}
	deliver_member_list(1);
	deliver_member_list(2);
	deliver_member_list(3);
	fire_member_timer();
	expect_decision(2, MIDR_DECISION_JOIN, 2);
	expect_state(anchors_are(2, 1, 3),
		     "anchors are the best other groups: 1 (failed) and 3");
	destroy_fixture();

	reset_fixture();
	case_name = "whole-batch-fails-creates-without-trying-later-groups";
	execute_nds = true;
	for (uint32_t gid = 1; gid <= 5; gid++) {
		add_rep(gid, gid * 1000);
		add_members(gid, 5, gid == 5 ? 5000 : 25000, 0.01, false);
	}
	trigger(MIDR_TRIGGER_REP_PROBE_DONE);
	expect_decision(1, MIDR_DECISION_RECOMMEND, 1);
	for (uint32_t gid = 1; gid <= 3; gid++)
		deliver_member_list(gid);
	fire_member_timer();
	expect_decision(2, MIDR_DECISION_CREATE, 6);
	expect_state(batch_is(1, 2, 3) && request_count == 3 &&
		     !group_listed(4) && !group_listed(5),
		     "groups 4 and 5 are never asked, although group 5 would pass");
	expect_state(anchors_are(2, 1, 2), "CREATE carries anchors 1 and 2");
	{
		struct midr_cluster_decision late = {
			.decision_type = MIDR_DECISION_RECOMMEND,
			.new_group_id = 4,
			.recommended_rep_id = node_id(4),
		};
		size_t before = request_count;
		test_apply_recommend(&owner, &late);
		expect_state(batch_is(1, 2, 3) && request_count == before,
			     "a late RECOMMEND does not start a second batch");
	}
	destroy_fixture();

	reset_fixture();
	case_name = "list-timeout-does-not-join-on-stale-members";
	execute_nds = true;
	add_rep(1, 1000);
	add_rep(2, 2000);
	add_members(1, 5, 5000, 0.01, true); /* Previous attempt's good data. */
	trigger(MIDR_TRIGGER_REP_PROBE_DONE);
	expect_state(batch_is(1, 2, 0) && nds.t_member_probe_done,
		     "batch has a bounded list wait");
	fire_member_timer(); /* No fresh list received. */
	expect_decision(2, MIDR_DECISION_CREATE, 3);
	expect_state(anchors_are(2, 0, 0),
		     "groups whose lists never arrived are not anchors");
	destroy_fixture();

	reset_fixture();
	case_name = "silent-group-skipped-as-anchor";
	execute_nds = true;
	for (uint32_t gid = 1; gid <= 4; gid++)
		add_rep(gid, gid * 1000);
	add_members(2, 5, 5000, 0.01, false);
	add_members(3, 5, 25000, 0.01, false);
	trigger(MIDR_TRIGGER_REP_PROBE_DONE);
	deliver_member_list(2); /* Group 1's representative never answers. */
	deliver_member_list(3);
	fire_member_timer();
	expect_decision(2, MIDR_DECISION_JOIN, 2);
	expect_state(anchors_are(2, 3, 4),
		     "silent group 1 is skipped; untried group 4 fills the slot");
	destroy_fixture();

	reset_fixture();
	case_name = "unresolvable-candidate-is-skipped-in-its-batch";
	execute_nds = true;
	add_rep(1, 1000)->rep_transport.v4 = 0; /* no usable locator */
	add_rep(2, 2000);
	add_members(2, 5, 5000, 0.01, false);
	trigger(MIDR_TRIGGER_REP_PROBE_DONE);
	expect_state(batch_is(1, 2, 0) && request_count == 1 &&
		     requested_groups[0] == 2 && nds.t_member_probe_done &&
		     nds.t_member_probe_done->seconds == MIDR_JOIN_PROBE_WAIT_SECS,
		     "group 1 stays in the batch without a request; group 2 is asked");
	deliver_member_list(2);
	fire_member_timer();
	expect_decision(2, MIDR_DECISION_JOIN, 2);
	destroy_fixture();

	reset_fixture();
	case_name = "fully-unresolvable-batch-creates-at-once";
	execute_nds = true;
	for (uint32_t gid = 1; gid <= 4; gid++) {
		struct midr_rep_entry *rep = add_rep(gid, gid * 1000);
		if (gid <= 3)
			rep->rep_transport.v4 = 0; /* no usable locator */
	}
	add_members(4, 5, 5000, 0.01, false);
	trigger(MIDR_TRIGGER_REP_PROBE_DONE);
	expect_state(batch_is(1, 2, 3) && request_count == 0 &&
		     nds.t_member_probe_done && nds.t_member_probe_done->seconds == 0,
		     "no request could be sent, so evaluation is due immediately");
	fire_member_timer();
	expect_decision(2, MIDR_DECISION_CREATE, 5);
	expect_state(request_count == 0 && anchors_are(2, 4, 0),
		     "group 4 is not tried; it is the only anchor with a reachable list");
	destroy_fixture();

	reset_fixture();
	case_name = "duplicate-representatives-take-one-batch-slot";
	execute_nds = true;
	add_rep(1, 1000);
	add_rep(1, 1500);
	add_rep(2, 2000);
	add_members(1, 5, 25000, 0.01, false);
	trigger(MIDR_TRIGGER_REP_PROBE_DONE);
	expect_state(batch_is(1, 2, 0), "one batch slot per group");
	deliver_member_list(1);
	fire_member_timer();
	expect_decision(2, MIDR_DECISION_CREATE, 3);
	destroy_fixture();

	reset_fixture();
	case_name = "new-round-clears-listed-groups-and-old-timers";
	add_rep(1, 1000)->members_listed = true;
	nds.join_phase = MIDR_JOIN_PROBING_MEMBERS;
	nds.join_group_id = 1;
	nds.join_batch_gid[0] = 1;
	nds.join_in_progress = true;
	event_add_timer(NULL, midr_ctrl_member_probe_done_cb, &owner, 60,
			&nds.t_member_probe_done);
	midr_join_round_start(&owner);
	expect_state(nds.join_phase == MIDR_JOIN_IDLE && !nds.join_in_progress &&
		     nds.join_group_id == 0 && batch_is(0, 0, 0) &&
		     !nds.t_member_probe_done && nds.join_intent &&
		     !group_listed(1),
		     "new round discards the previous attempt's failure state");
	nds.join_phase = MIDR_JOIN_PROBING_REPS;
	trigger(MIDR_TRIGGER_REP_PROBE_DONE);
	expect_decision(1, MIDR_DECISION_RECOMMEND, 1);
	destroy_fixture();

	reset_fixture();
	case_name = "configured-group-does-not-switch-automatically";
	execute_nds = true;
	nds.config_group_id = 1;
	add_rep(1, 1000);
	add_rep(2, 2000);
	add_members(1, 5, 25000, 0.01, false);
	add_members(2, 5, 5000, 0.01, false);
	trigger(MIDR_TRIGGER_REP_PROBE_DONE);
	expect_state(batch_is(1, 0, 0) && request_count == 1,
		     "a pinned node evaluates only its configured group");
	deliver_member_list(1);
	fire_member_timer();
	expect_decision(2, MIDR_DECISION_CREATE, 3);
	expect_state(nds.join_group_id == 1 && anchors_are(2, 2, 0),
		     "CL does not redirect the pinned group; group 2 is an anchor");
	destroy_fixture();
}

static void add_bootstrap(void)
{
	listnode_add(nds.bootstrap_list,
		     test_calloc(1, sizeof(struct midr_bootstrap_entry)));
}

/* Node restart scenarios from the FITI rejoin report (2026-10-19). */
static void restart(void)
{
	/* D1/D2: the directory lists reps but none was measured (they expired or
	 * restarted during the window). Re-fetch the directory instead of
	 * creating an island group, up to the retry limit. */
	reset_fixture();
	case_name = "unmeasured-reps-rejoin-before-create";
	add_rep(1, 0);
	add_rep(2, 0);
	add_bootstrap();
	trigger(MIDR_TRIGGER_REP_PROBE_DONE);
	expect_decision(1, MIDR_DECISION_REJOIN, 0);
	nds.join_retry_rounds = 3;
	trigger(MIDR_TRIGGER_REP_PROBE_DONE);
	expect_decision(2, MIDR_DECISION_CREATE, 3);
	destroy_fixture();

	reset_fixture();
	case_name = "unmeasured-reps-without-bootstrap-create";
	add_rep(1, 0);
	trigger(MIDR_TRIGGER_REP_PROBE_DONE);
	expect_decision(1, MIDR_DECISION_CREATE, 2);
	destroy_fixture();

	/* D1: every representative of the batch stayed silent (restarted inside
	 * the window, or answered for a new group). */
	reset_fixture();
	case_name = "silent-batch-rejoins";
	add_rep(1, 1000);
	add_rep(2, 2000);
	add_members(1, 5, 5000, 0.01, false);
	add_bootstrap();
	trigger(MIDR_TRIGGER_REP_PROBE_DONE);
	nds.join_phase = MIDR_JOIN_PROBING_MEMBERS;
	nds.join_group_id = 1;
	for (size_t i = 0; i < decisions[0].candidate_count; i++)
		nds.join_batch_gid[i] = decisions[0].candidates[i];
	trigger(MIDR_TRIGGER_MEMBER_PROBE_DONE);
	expect_decision(2, MIDR_DECISION_REJOIN, 0);
	destroy_fixture();

	/* A listed group that fails the bar is a real answer: still CREATE. */
	reset_fixture();
	case_name = "listed-but-bad-batch-still-creates";
	add_rep(1, 1000);
	add_members(1, 5, 25000, 0.01, false);
	add_bootstrap();
	if (select_first_group()) {
		trigger(MIDR_TRIGGER_MEMBER_PROBE_DONE);
		expect_decision(2, MIDR_DECISION_CREATE, 2);
	}
	destroy_fixture();

	/* D3: the node's view was emptied while the directory still lists
	 * groups 1..6; the new group must not reuse one of them. */
	reset_fixture();
	case_name = "create-id-avoids-directory-groups";
	for (uint32_t gid = 1; gid <= 6; gid++)
		add_rep(gid, 0);
	free_entries(&view.nodes);
	free_entries(&view.links);
	view.nodes = list_new();
	view.links = list_new();
	trigger(MIDR_TRIGGER_REP_PROBE_DONE);
	expect_decision(1, MIDR_DECISION_CREATE, 7);
	destroy_fixture();

	/* D4: a representative restart degrades the links for a short while;
	 * only consecutive failures make a member leave. */
	reset_fixture();
	case_name = "leave-is-debounced";
	nds.join_phase = MIDR_JOIN_IDLE;
	nds.local_group_id = 1;
	add_members(1, 2, 30000, 0.0, true);
	trigger(MIDR_TRIGGER_PERIODIC_SYNC);
	trigger(MIDR_TRIGGER_PERIODIC_SYNC);
	expect_state(decision_count == 0, "two bad syncs do not LEAVE");
	trigger(MIDR_TRIGGER_PERIODIC_SYNC);
	expect_decision(1, MIDR_DECISION_LEAVE, 0);
	destroy_fixture();

	reset_fixture();
	case_name = "recovered-links-reset-leave-debounce";
	nds.join_phase = MIDR_JOIN_IDLE;
	nds.local_group_id = 1;
	add_members(1, 2, 30000, 0.0, true);
	trigger(MIDR_TRIGGER_PERIODIC_SYNC);
	trigger(MIDR_TRIGGER_PERIODIC_SYNC);
	{
		struct listnode *n;
		struct midr_link_entry *link;
		for (ALL_LIST_ELEMENTS_RO(view.links, n, link))
			link->long_term.rtt_us = 5000;
		trigger(MIDR_TRIGGER_PERIODIC_SYNC);
		for (ALL_LIST_ELEMENTS_RO(view.links, n, link))
			link->long_term.rtt_us = 30000;
	}
	trigger(MIDR_TRIGGER_PERIODIC_SYNC);
	trigger(MIDR_TRIGGER_PERIODIC_SYNC);
	expect_state(decision_count == 0 && nds.leave_pending_syncs == 2,
		     "a good sync restarts the count");
	destroy_fixture();

	/* D3 residue: two nodes created group 7 at the same time and met. The
	 * larger router-id resigns; the smaller keeps the role. */
	for (unsigned int scenario = 0; scenario < 4; scenario++) {
		static const char *names[] = {
			"duplicate-rep-larger-router-id-resigns",
			"duplicate-rep-smaller-router-id-keeps-role-while-peer-yields",
			"duplicate-rep-configured-group-keeps-role",
			"duplicate-rep-gone-resets-count",
		};
		struct midr_node_entry *other;

		reset_fixture();
		case_name = names[scenario];
		nds.join_phase = MIDR_JOIN_IDLE;
		nds.local_group_id = 7;
		nds.local_capabilities = MIDR_CAP_GROUP_REP;
		/* Node ids are 10.0.0.1 and up. */
		owner.router_id.s_addr = htonl(scenario == 1 ? 0x0a000000U
							     : 0x0a0000c8U);
		if (scenario == 2)
			nds.config_group_id = 7;
		other = add_node(7, 5000, 0.0, true);
		other->capabilities = MIDR_CAP_GROUP_REP;
		trigger(MIDR_TRIGGER_PERIODIC_SYNC);
		if (scenario == 3)
			other->capabilities = 0;
		trigger(MIDR_TRIGGER_PERIODIC_SYNC);
		if (scenario == 0)
			expect_decision(1, MIDR_DECISION_REP_RESIGN, 7);
		else if (scenario == 1)
			expect_state(decision_count == 0 && nds.dup_rep_syncs == 2,
				     "no REP_RESIGN while the larger peer may still yield");
		else
			expect_state(decision_count == 0 && !nds.dup_rep_syncs,
				     "no REP_RESIGN");
		destroy_fixture();
	}

	/* The larger peer never yields (it has a configured group): the smaller
	 * one gives up the role after the longer wait. */
	reset_fixture();
	case_name = "duplicate-rep-yields-to-a-peer-that-never-resigns";
	nds.join_phase = MIDR_JOIN_IDLE;
	nds.local_group_id = 7;
	nds.local_capabilities = MIDR_CAP_GROUP_REP;
	owner.router_id.s_addr = htonl(0x0a000000U);
	add_node(7, 5000, 0.0, true)->capabilities = MIDR_CAP_GROUP_REP;
	for (int i = 0; i < 5; i++)
		trigger(MIDR_TRIGGER_PERIODIC_SYNC);
	expect_state(decision_count == 0, "five syncs leave room for the peer to yield");
	trigger(MIDR_TRIGGER_PERIODIC_SYNC);
	expect_decision(1, MIDR_DECISION_REP_RESIGN, 7);
	destroy_fixture();
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	if (strcmp(argv[1], "all") == 0 || strcmp(argv[1], "baseline") == 0)
		baseline();
	if (strcmp(argv[1], "all") == 0 || strcmp(argv[1], "regression") == 0)
		regression();
	if (strcmp(argv[1], "all") == 0)
		state_flow();
	if (strcmp(argv[1], "all") == 0 || strcmp(argv[1], "restart") == 0)
		restart();
	printf("RESULT: %u checks, %u passed, %u failed\n", checks,
	       checks - failures, failures);
	return failures ? 1 : 0;
}
