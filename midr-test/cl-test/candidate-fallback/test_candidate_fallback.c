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
	nds.join_member_list_received = true;
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

static void deliver_member_list(uint32_t gid)
{
	struct listnode *n;
	struct midr_node_entry *node;
	assert(nds.join_group_id == gid);
	/* Model the validated list input. Parsing/PM remain outside this harness. */
	for (ALL_LIST_ELEMENTS_RO(view.nodes, n, node))
		if (node->group_id == gid)
			node->is_adjacent = true;
	nds.join_member_list_received = true;
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
 * the selected representative, just as the RECOMMEND consumer would do. */
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
	for (size_t i = 0; i < decisions[0].anchor_count; i++)
		nds.anchor_group_id[i] = decisions[0].anchors[i].group_id;
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
	trigger(MIDR_TRIGGER_MEMBER_PROBE_DONE);
	expect_decision(1, MIDR_DECISION_JOIN, 2);
	destroy_fixture();
}

static void regression(void)
{
	const char *names[] = {
		"bad-member-rtt-retry-second-group",
		"bad-member-loss-retry-second-group",
		"four-of-five-good-links-retry-second-group",
		"empty-first-member-list-retry-second-group",
		"bad-first-group-blocked-second-retry-third",
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
			add_members(1, 4, 5000, 0.01, true);
			link_for(add_node(1, 5000, 0.01, true))->status = MIDR_LINK_DOWN;
		}

		/* Runner-up members are known/probed as anchors (not adjacent).
 * Retry must fetch/evaluate them as the next main candidate; their mere
 * existence must not silently count them toward group 1's JOIN threshold. */
		add_members(scenario == 4 ? 3 : 2, 5, 5000, 0.01, false);
		if (select_first_group()) {
			trigger(MIDR_TRIGGER_MEMBER_PROBE_DONE);
			/* Desired retry contract: try the next allowed representative
 * before CREATE. These are ordinary failing assertions, not xfails. */
			expect_decision(2, MIDR_DECISION_RECOMMEND, scenario == 4 ? 3 : 2);
		}
		destroy_fixture();
	}
}

static void state_flow(void)
{
	/* Real CL -> real NDS RECOMMEND case -> real Control request/timer. */
	reset_fixture();
	case_name = "retry-second-group-then-join";
	execute_nds = true;
	add_rep(1, 1000);
	add_rep(2, 2000);
	add_members(1, 5, 25000, 0.01, false);
	add_members(2, 5, 5000, 0.01, false);
	trigger(MIDR_TRIGGER_REP_PROBE_DONE);
	expect_decision(1, MIDR_DECISION_RECOMMEND, 1);
	expect_state(nds.join_phase == MIDR_JOIN_PROBING_MEMBERS &&
		     nds.join_group_id == 1 && nds.t_member_probe_done &&
		     !nds.join_member_list_received && requested_groups[0] == 1,
		     "NDS requests group 1 with a list-wait timer");
	{
		struct midr_cluster_decision premature = {
			.decision_type = MIDR_DECISION_RECOMMEND,
			.new_group_id = 2, .failed_group_id = 1,
			.recommended_rep_id = node_id(2),
		};
		size_t before = request_count;
		test_apply_recommend(&owner, &premature);
		expect_state(nds.join_group_id == 1 && request_count == before,
			     "unfinished member evaluation rejects premature retry");
	}
	deliver_member_list(1);
	fire_member_timer();
	expect_decision(2, MIDR_DECISION_RECOMMEND, 2);
	expect_state(decisions[1].failed_gid == 1 && nds.join_group_id == 2 &&
		     nds.t_member_probe_done && !nds.join_member_list_received,
		     "NDS accepts retry, requests group 2 and resets fresh-list state");
	{
		struct midr_cluster_decision late = {
			.decision_type = MIDR_DECISION_RECOMMEND,
			.new_group_id = 2, .failed_group_id = 1,
			.recommended_rep_id = node_id(2),
		};
		size_t before = request_count;
		test_apply_recommend(&owner, &late);
		expect_state(nds.join_group_id == 2 && request_count == before,
			     "duplicate retry does not restart group 2");
	}
	deliver_member_list(2);
	fire_member_timer();
	expect_decision(3, MIDR_DECISION_JOIN, 2);
	destroy_fixture();

	reset_fixture();
	case_name = "create-only-after-all-four-groups-fail";
	execute_nds = true;
	for (uint32_t gid = 1; gid <= 4; gid++) {
		add_rep(gid, gid * 1000);
		add_members(gid, 5, 25000, 0.01, false);
	}
	trigger(MIDR_TRIGGER_REP_PROBE_DONE);
	for (uint32_t gid = 1; gid <= 4; gid++) {
		expect_decision(gid, MIDR_DECISION_RECOMMEND, gid);
		expect_state(nds.join_group_id == gid && nds.t_member_probe_done,
			     "candidate is actually requested before it is rejected");
		deliver_member_list(gid);
		fire_member_timer();
	}
	expect_decision(5, MIDR_DECISION_CREATE, 5);
	destroy_fixture();

	reset_fixture();
	case_name = "list-timeout-retries-without-stale-member-join";
	execute_nds = true;
	add_rep(1, 1000);
	add_rep(2, 2000);
	add_members(1, 5, 5000, 0.01, true); /* Previous attempt's good data. */
	trigger(MIDR_TRIGGER_REP_PROBE_DONE);
	fire_member_timer(); /* No fresh list received. */
	expect_decision(2, MIDR_DECISION_RECOMMEND, 2);
	expect_state(!nds.join_member_list_received && nds.t_member_probe_done,
		     "second group also has a bounded list wait");
	fire_member_timer();
	expect_decision(3, MIDR_DECISION_CREATE, 3);
	destroy_fixture();

	reset_fixture();
	case_name = "duplicate-representatives-do-not-retry-a-failed-group";
	execute_nds = true;
	add_rep(1, 1000);
	add_rep(1, 1500);
	add_rep(2, 2000);
	add_members(1, 5, 25000, 0.01, false);
	trigger(MIDR_TRIGGER_REP_PROBE_DONE);
	deliver_member_list(1);
	fire_member_timer();
	expect_decision(2, MIDR_DECISION_RECOMMEND, 2);
	expect_state(((struct midr_rep_entry *)nds.rep_dir->head->data)->join_failed &&
		     ((struct midr_rep_entry *)nds.rep_dir->head->next->data)->join_failed,
		     "every representative of the rejected group is marked");
	fire_member_timer();
	expect_decision(3, MIDR_DECISION_CREATE, 3);
	destroy_fixture();

	reset_fixture();
	case_name = "new-round-clears-rejected-groups-and-old-timers";
	add_rep(1, 1000)->join_failed = true;
	nds.join_phase = MIDR_JOIN_PROBING_MEMBERS;
	nds.join_group_id = 1;
	nds.join_member_list_received = true;
	nds.join_in_progress = true;
	event_add_timer(NULL, midr_ctrl_member_probe_done_cb, &owner, 60,
			&nds.t_member_probe_done);
	midr_join_round_start(&owner);
	expect_state(nds.join_phase == MIDR_JOIN_IDLE && !nds.join_in_progress &&
		     nds.join_group_id == 0 && !nds.join_member_list_received &&
		     !nds.t_member_probe_done && nds.join_intent &&
		     !((struct midr_rep_entry *)nds.rep_dir->head->data)->join_failed,
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
	trigger(MIDR_TRIGGER_REP_PROBE_DONE);
	deliver_member_list(1);
	fire_member_timer();
	expect_decision(2, MIDR_DECISION_CREATE, 3);
	expect_state(nds.join_group_id == 1, "CL does not redirect the pinned group to group 2");
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
	printf("RESULT: %u checks, %u passed, %u failed\n", checks,
	       checks - failures, failures);
	return failures ? 1 : 0;
}
