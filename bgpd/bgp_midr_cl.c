// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR 节点分群（CL）模块。
 *
 * 实现说明：
 *   本文件实现了基于链路性能指标的真实分群算法，替换原先的骨架占位逻辑。
 *   与 NDS 的解耦边界：
 *     - 下行 I-3：NDS 调 midr_cl_on_global_view(bgp, trigger, gv)
 *     - 上行 I-7：CL 调 midr_nds_on_cluster_decision(bgp, &decision)
 *   CL 只读 gv；不碰 BGP-LS / 会话 / 定时器。
 *
 * 两个主决策点：
 *   REP_PROBE_DONE    -> 按长期 RTT/丢包率/带宽分数从 mi->rep_dir 选最优群代表
 *                        （RECOMMEND），无可用代表时输出 CREATE。
 *   MEMBER_PROBE_DONE -> 统计目标群内满足长期性能阈值的 is_adjacent 邻居数量，
 *                        达到 MIDR_CL_MIN_GOOD_LINKS 则 JOIN，否则 CREATE。
 *
 * 注意事项：
 *   目前 MEMBER_PROBE_DONE 在 NDS 设计中于成员表灌入后立即触发，若使用真实 UDP
 *   探测，此时可能尚未收到探测回复（rtt_us=0）。CL 将 rtt_us=0 视为"未探测到"，
 *   不计入好链路。此时会输出 CREATE。Stage 1 定时器驱动的稳态优化见 §8 待办。
 */

#include <zebra.h>

#include "log.h"
#include "memory.h"
#include "prefix.h"
#include "linklist.h"
#include "monotime.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_midr_nds.h"
#include "bgpd/bgp_midr_cl.h"

DEFINE_MTYPE_STATIC(BGPD, MIDR_NODE_EVIDENCE, "MIDR node evidence");
DEFINE_MTYPE_STATIC(BGPD, MIDR_REP_IDENTITY, "MIDR representative identity");

/* ===========================================================================
 * CL 策略阈值
 * =========================================================================*/

/* 入群 RTT 阈值：长期平均 RTT 需严格小于此值（设计文档 §3.2）。20 ms。 */
#define MIDR_CL_JOIN_RTT_THRESHOLD_US 20000U

/* 入群丢包率阈值：长期丢包率需严格小于此值。5%。 */
#define MIDR_CL_LOSS_THRESHOLD 0.05

/*
 * 入群所需最少"好链路"数量（设计文档 §3.2）。
 * 需与目标群内 is_adjacent 邻居中满足阈值的数量对比。
 */
#define MIDR_CL_MIN_GOOD_LINKS 5U

/* ===========================================================================
 * 内部辅助函数
 * =========================================================================*/

/* Find a link by its stable 32-bit BGP Identifier. */
static struct midr_link_entry *
cl_find_link_by_rid(const struct midr_global_view *gv,
		    const struct in_addr *rid)
{
	struct listnode *n;
	struct midr_link_entry *link;

	for (ALL_LIST_ELEMENTS_RO(gv->links, n, link)) {
		if (link->remote_node_id.family == AF_INET &&
		    IPV4_ADDR_SAME(&link->remote_node_id.u.prefix4, rid))
			return link;
	}
	return NULL;
}

static void cl_identity_from_rep(struct midr_rep_identity *identity,
				 const struct midr_rep_entry *rep)
{
	memset(identity, 0, sizeof(*identity));
	identity->group_id = rep->group_id;
	identity->node_id.family = AF_INET;
	identity->node_id.prefixlen = IPV4_MAX_BITLEN;
	identity->node_id.u.prefix4 = rep->rep_rid;
}

/*
 * 在全局视图链路列表中按 FRR prefix 查找链路条目。
 * 用于 MEMBER_PROBE_DONE：成员以 node_id（router-id）为探测 key。
 */
static struct midr_link_entry *
cl_find_link_by_prefix(const struct midr_global_view *gv,
		       const struct prefix *node_id)
{
	struct listnode *n;
	struct midr_link_entry *link;

	for (ALL_LIST_ELEMENTS_RO(gv->links, n, link))
		if (prefix_same(&link->remote_node_id, node_id))
			return link;
	return NULL;
}

/*
 * 判断链路是否可用于分群决策：状态 UP 且已收到至少一次探测回复（rtt_us > 0）。
 * rtt_us == 0 表示探测目标已注册但尚未收到任何回复，视为"数据不足"。
 */
static bool cl_link_has_data(const struct midr_link_entry *link)
{
	return link != NULL && link->status == MIDR_LINK_UP &&
	       link->long_term.rtt_us > 0;
}

/*
 * 判断链路的长期指标是否满足入群条件（设计文档 §3.2）：
 *   long_term.rtt_us  < MIDR_CL_JOIN_RTT_THRESHOLD_US （严格小于）
 *   long_term.loss_rate < MIDR_CL_LOSS_THRESHOLD       （严格小于）
 * 同时要求链路有探测数据。
 */
static bool cl_link_is_good_for_join(const struct midr_link_entry *link)
{
	if (!cl_link_has_data(link))
		return false;

	return link->long_term.rtt_us < MIDR_CL_JOIN_RTT_THRESHOLD_US &&
	       link->long_term.loss_rate < MIDR_CL_LOSS_THRESHOLD;
}

/*
 * 比较两组长期指标，判断 cand 是否优于 best：
 *   优先级：RTT 更低 > 丢包率更低 > 带宽分数更高（与 group-demo 一致）。
 */
static bool cl_metrics_is_better(const struct midr_nds_link_metrics *cand,
				  const struct midr_nds_link_metrics *best)
{
	if (cand->rtt_us != best->rtt_us)
		return cand->rtt_us < best->rtt_us;
	if (cand->loss_rate != best->loss_rate)
		return cand->loss_rate < best->loss_rate;
	return cand->bw_score > best->bw_score;
}

/*
 * 遍历节点表，找出其中最大的 group_id，用于 CREATE 分配新群编号。
 * 若节点表为空，返回 0（CREATE 新群 ID 将为 1）。
 */
static uint32_t cl_max_group_id(const struct midr_global_view *gv)
{
	struct list *nodes = midr_nds_cl_nodes_getter(gv);
	struct midr_node_entry *entry;
	struct listnode *n;
	uint32_t max_id = 0;

	for (ALL_LIST_ELEMENTS_RO(nodes, n, entry))
		if (entry->group_id > max_id)
			max_id = entry->group_id;

	list_delete(&nodes);
	return max_id;
}

/* ===========================================================================
 * REP_PROBE_DONE 处理
 * =========================================================================*/

/*
 * 判断 cand（配其链路 cand_link）排名是否严格优于 best（配 best_link）：
 *   长期 RTT 更低 → 丢包率更低 → 带宽分数更高 → group_id 更小（稳定排序）。
 * best 为 NULL（尚无候选）时 cand 总是更优。抽成独立函数是因为前 3 名排序
 * 需要反复用它（不只是找单一最优）。
 */
static bool cl_rep_is_better(const struct midr_rep_entry *cand_r,
			     const struct midr_link_entry *cand_link,
			     const struct midr_rep_entry *best_r,
			     const struct midr_link_entry *best_link)
{
	if (!best_r)
		return true;
	if (cl_metrics_is_better(&cand_link->long_term, &best_link->long_term))
		return true;
	return cand_link->long_term.rtt_us == best_link->long_term.rtt_us &&
	       cand_link->long_term.loss_rate ==
		       best_link->long_term.loss_rate &&
	       cand_link->long_term.bw_score == best_link->long_term.bw_score &&
	       cand_r->group_id < best_r->group_id;
}

/*
 * 从 mi->rep_dir（群代表目录）中，结合 gv->links 的长期探测指标，选出性能最优的
 * 群代表，经 I-7 RECOMMEND 回灌；同时保留第 2、3 名（用于群间锚点连接方案），
 * 随 RECOMMEND 一并回灌给 NDS 供其请求这两个次优群的成员列表。REP_PROBE_DONE
 * 本来就已对目录里每个代表探测过一轮，第 2/3 名不需要任何额外探测代价，只是
 * 原先探完即弃。
 *
 * 候选筛选条件：链路状态 UP 且已有探测数据（rtt_us > 0）。
 * 排序规则见 cl_rep_is_better()。
 *
 * 无可用候选时输出 CREATE（new_group_id = 当前最大 group_id + 1）。
 */
static void cl_handle_rep_probe_done(struct bgp *bgp,
				     const struct midr_global_view *gv)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct listnode *n;
	struct midr_rep_entry *r;
	/* 前 3 名，插入排序维护；[0]=最优（RECOMMEND），[1]/[2]=次优（锚点候选）。 */
	struct midr_rep_entry *top_rep[3] = { NULL, NULL, NULL };
	struct midr_link_entry *top_link[3] = { NULL, NULL, NULL };
	struct midr_cluster_decision d = {};
	int i, j;

	for (ALL_LIST_ELEMENTS_RO(mi->rep_dir, n, r)) {
		struct midr_link_entry *link;

		/* 探测键 = rid：目录条目恒带真名（REP_LIST 的 rid 栏自协议 v2 起
		 * 必填）。rid 为 0 = 条目不完整，跳过。
		 * 〔轮 5 清理：删掉了"查不到就回落按 transport 查"那半——手配目录
		 *   路径 2026-08-11 已删除，目录只剩从节点表推导这一个来源，rid 恒
		 *   非 0，回落永不命中。留着反而会在真出现 rid=0 时静默改用另一个
		 *   键，不如直接跳过来得容易发现。〕 */
		if (r->rep_rid.s_addr == INADDR_ANY)
			continue;
		link = cl_find_link_by_rid(gv, &r->rep_rid);
		if (!cl_link_has_data(link))
			continue;

		for (i = 0; i < 3; i++) {
			if (!cl_rep_is_better(r, link, top_rep[i], top_link[i]))
				continue;
			for (j = 2; j > i; j--) {
				top_rep[j] = top_rep[j - 1];
				top_link[j] = top_link[j - 1];
			}
			top_rep[i] = r;
			top_link[i] = link;
			break;
		}
	}

	if (!top_rep[0]) {
		uint32_t new_gid = cl_max_group_id(gv) + 1;

		d.decision_type = MIDR_DECISION_CREATE;
		d.new_group_id = new_gid;
		d.old_group_id = mi->local_group_id;
		MIDR_FLOW_LOG(
			"MIDR CL: REP_PROBE_DONE → 无可用群代表，CREATE 新群 %u",
			new_gid);
	} else {
		struct listnode *node, *nnode;
		struct midr_rep_identity *identity;

		d.decision_type = MIDR_DECISION_RECOMMEND;
		d.new_group_id = top_rep[0]->group_id;
		d.old_group_id = mi->local_group_id;
		d.recommended_rep_id.family = AF_INET;
		d.recommended_rep_id.prefixlen = IPV4_MAX_BITLEN;
		d.recommended_rep_id.u.prefix4 = top_rep[0]->rep_rid;

		/* Pass only stable identities across I-7. */
		d.anchor_reps = list_new();
		for (i = 1; i < 3; i++) {
			if (!top_rep[i])
				continue;
			identity = XCALLOC(MTYPE_MIDR_REP_IDENTITY,
					   sizeof(*identity));
			cl_identity_from_rep(identity, top_rep[i]);
			listnode_add(d.anchor_reps, identity);
		}

		MIDR_FLOW_LOG(
			"MIDR CL: REP_PROBE_DONE → RECOMMEND 群 %u 代表 %pI4"
			"（rtt=%u us, loss=%.4f, bw=%u），另有 %d 个次优群锚点候选",
			top_rep[0]->group_id, &top_rep[0]->rep_rid,
			top_link[0]->long_term.rtt_us,
			top_link[0]->long_term.loss_rate,
			top_link[0]->long_term.bw_score,
			listcount(d.anchor_reps));

		midr_nds_on_cluster_decision(bgp, &d);

		for (ALL_LIST_ELEMENTS(d.anchor_reps, node, nnode, identity))
			XFREE(MTYPE_MIDR_REP_IDENTITY, identity);
		list_delete(&d.anchor_reps);
		return;
	}

	midr_nds_on_cluster_decision(bgp, &d);
}

/* ===========================================================================
 * MEMBER_PROBE_DONE 处理
 * =========================================================================*/

/*
 * 统计 target_group_id 群内满足入群阈值条件的 is_adjacent 邻居数量，并收集
 * 用于日志的最差 RTT 和最差 loss_rate（便于排查未达标原因）。
 *
 * 两处复用同一把尺子：MEMBER_PROBE_DONE 拿它评估候选群（target=join_group_id）
 * 决定要不要 JOIN；PERIODIC_SYNC 拿它评估本节点当前所在群
 * （target=local_group_id）决定要不要 LEAVE——入群/留群用同一份"好链路"统计，
 * 但两处拿这份统计去比的阈值并不对称，见 cl_handle_periodic_sync() 头注释。
 *
 * out_total（可选）：target_group_id 群内 is_adjacent 邻居总数（不论好坏）。
 * PERIODIC_SYNC 拿它把留群阈值封顶到"群里实际认识的节点数"——群本身天然小
 * （成员数 < MIDR_CL_MIN_GOOD_LINKS）时，入群用的绝对阈值永远够不着。
 */
static size_t cl_count_good_member_links(const struct bgp *bgp,
					 const struct midr_global_view *gv,
					 uint32_t target_group_id,
					 uint32_t *out_worst_rtt_us,
					 double *out_worst_loss,
					 size_t *out_total)
{
	struct list *nodes = midr_nds_cl_nodes_getter(gv);
	struct midr_node_entry *entry;
	struct listnode *n;
	size_t good = 0;
	size_t total = 0;
	uint32_t worst_rtt = 0;
	double worst_loss = 0.0;

	for (ALL_LIST_ELEMENTS_RO(nodes, n, entry)) {
		struct midr_link_entry *link;

		if (!entry->is_adjacent || entry->is_self)
			continue;
		if (entry->group_id != target_group_id)
			continue;

		total++;
		link = cl_find_link_by_prefix(gv, &entry->node_id);
		if (cl_link_is_good_for_join(link)) {
			good++;
		} else {
			/* 收集未达标链路数据用于调试日志 */
			if (link && link->long_term.rtt_us > worst_rtt)
				worst_rtt = link->long_term.rtt_us;
			if (link && link->long_term.loss_rate > worst_loss)
				worst_loss = link->long_term.loss_rate;
		}
		MIDR_LOG("MIDR CL: 成员候选 %pFX group=%u link=%s status=%d rtt=%u loss=%.4f",
			 &entry->node_id, entry->group_id,
			 link ? "found" : "missing",
			 link ? (int)link->status : -1,
			 link ? link->long_term.rtt_us : 0,
			 link ? link->long_term.loss_rate : 0.0);
	}

	list_delete(&nodes);

	if (out_worst_rtt_us)
		*out_worst_rtt_us = worst_rtt;
	if (out_worst_loss)
		*out_worst_loss = worst_loss;
	if (out_total)
		*out_total = total;

	return good;
}

/*
 * 处理 MEMBER_PROBE_DONE：
 *   目标群 = mi->join_group_id（由 RECOMMEND 阶段确定）。
 *   遍历全局视图中该群的 is_adjacent 节点，统计满足长期 RTT/丢包率阈值的数量。
 *   够格则 JOIN；否则 CREATE（分配新群 ID = 最大 + 1）。
 *
 * JOIN threshold is capped the same way the PERIODIC_SYNC stay threshold is
 * (min(MIDR_CL_MIN_GOOD_LINKS, total known members) — see cl_handle_periodic_
 * sync()): a brand-new group only has as many members as have already
 * joined it, so an uncapped absolute floor of MIDR_CL_MIN_GOOD_LINKS means no
 * group smaller than that can ever gain its first member — every arriving
 * node sees too few candidates and just CREATEs another singleton group of
 * its own instead. Capping lets a group grow one good link at a time until
 * it reaches the real floor, at which point the cap no longer applies and
 * the strict absolute bar takes back over.
 *
 * Unlike the stay path, an empty candidate group (total == 0) must NOT pass
 * trivially — joining something we have zero data on is an active decision
 * that needs at least one known good link to justify, whereas staying put
 * on zero data defaults to "don't panic-leave".
 *
 * 注：当前 NDS 不支持次优代表重试流程（RECOMMEND guard 仅在 PROBING_REPS 阶段
 * 生效）。若需要重试，后续可在 NDS 放开 guard 后在此输出 RECOMMEND，CL 保存有
 * 序候选列表即可实现 group-demo 的完整回退逻辑。
 */
static void cl_handle_member_probe_done(struct bgp *bgp,
					const struct midr_global_view *gv)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	uint32_t target_gid = mi->join_group_id;
	uint32_t worst_rtt = 0;
	double worst_loss = 0.0;
	size_t good_links, total_known, join_threshold;
	struct midr_cluster_decision d = {};

	good_links = cl_count_good_member_links(bgp, gv, target_gid,
						&worst_rtt, &worst_loss,
						&total_known);
	join_threshold = total_known < MIDR_CL_MIN_GOOD_LINKS
				  ? total_known
				  : MIDR_CL_MIN_GOOD_LINKS;

	if (total_known > 0 && good_links >= join_threshold) {
		d.decision_type = MIDR_DECISION_JOIN;
		d.new_group_id = target_gid;
		d.old_group_id = mi->local_group_id;
		MIDR_FLOW_LOG(
			"MIDR CL: MEMBER_PROBE_DONE → JOIN 群 %u"
			"（%zu/%zu 条好链路，认识 %zu 个成员）",
			target_gid, good_links, join_threshold, total_known);
	} else {
		uint32_t new_gid = cl_max_group_id(gv) + 1;

		d.decision_type = MIDR_DECISION_CREATE;
		d.new_group_id = new_gid;
		d.old_group_id = mi->local_group_id;
		MIDR_FLOW_LOG(
			"MIDR CL: MEMBER_PROBE_DONE → 群 %u 仅 %zu/%zu 条好链路"
			"（认识 %zu 个成员，最差 rtt=%u us, loss=%.4f），CREATE 新群 %u",
			target_gid, good_links, join_threshold, total_known,
			worst_rtt, worst_loss, new_gid);
	}

	midr_nds_on_cluster_decision(bgp, &d);
}

/* ===========================================================================
 * ANCHOR_PROBE_DONE 处理（群间锚点连接）
 * =========================================================================*/

/*
 * 在 target_group_id 群内按相对排名（cl_rep_is_better 同款比较、复用
 * cl_metrics_is_better）选出连接质量最好的至多 2 个节点，各生成一条
 * struct midr_node_evidence 追加进 out。
 *
 * 不设绝对及格线（不用 cl_link_is_good_for_join 的入群阈值）：跨群链路天然
 * 比群内差，用入群阈值筛几乎总会把候选筛没、选不出锚点——这里要的是"这个群
 * 里连得最好的两个"，不是"够不够格入群"。
 *
 * 不看 is_adjacent：锚点候选是 NDS 为评估而临时探测的非本群节点（只探不
 * 连），不是本群邻居，语义上正相反于 cl_count_good_member_links 的过滤条件。
 */
static void cl_select_anchor_candidates(const struct midr_global_view *gv,
					uint32_t target_group_id,
					struct list *out)
{
	struct list *nodes;
	struct midr_node_entry *entry;
	struct midr_node_entry *top1 = NULL, *top2 = NULL;
	struct midr_nds_link_metrics top1_m = {}, top2_m = {};
	struct listnode *n;

	if (target_group_id == 0)
		return;

	nodes = midr_nds_cl_nodes_getter(gv);

	for (ALL_LIST_ELEMENTS_RO(nodes, n, entry)) {
		struct midr_link_entry *link;

		if (entry->is_self || entry->group_id != target_group_id)
			continue;
		link = cl_find_link_by_prefix(gv, &entry->node_id);
		MIDR_LOG("MIDR CL: 锚点候选 %pFX group=%u link=%s status=%d rtt=%u loss=%.4f",
			 &entry->node_id, entry->group_id,
			 link ? "found" : "missing",
			 link ? (int)link->status : -1,
			 link ? link->long_term.rtt_us : 0,
			 link ? link->long_term.loss_rate : 0.0);
		if (!cl_link_has_data(link))
			continue;

		if (!top1 || cl_metrics_is_better(&link->long_term, &top1_m)) {
			top2 = top1;
			top2_m = top1_m;
			top1 = entry;
			top1_m = link->long_term;
		} else if (!top2 ||
			   cl_metrics_is_better(&link->long_term, &top2_m)) {
			top2 = entry;
			top2_m = link->long_term;
		}
	}

	list_delete(&nodes);

	if (top1) {
		struct midr_node_evidence *ev =
			XCALLOC(MTYPE_MIDR_NODE_EVIDENCE, sizeof(*ev));

		ev->node_id = top1->node_id;
		ev->metrics = top1_m;
		listnode_add(out, ev);
	}
	if (top2) {
		struct midr_node_evidence *ev =
			XCALLOC(MTYPE_MIDR_NODE_EVIDENCE, sizeof(*ev));

		ev->node_id = top2->node_id;
		ev->metrics = top2_m;
		listnode_add(out, ev);
	}
}

/*
 * 评估 mi->anchor_group_id[0]/[1]（NDS 在 RECOMMEND 阶段从次优代表拿到、限时
 * 探测后回调的两个次优群号，0 = 该槽位无候选）：分别选出连接最好的至多 2 个
 * 节点，合并进 evidence，输出 ANCHOR 决策。NDS 收到后对 evidence 里每条直接
 * midr_ctrl_connect()，形成群间锚点连接。不改变本节点的 group_id，
 * new/old_group_id 均取 local_group_id 仅作记录。
 */
static void cl_handle_anchor_probe_done(struct bgp *bgp,
					const struct midr_global_view *gv)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct midr_cluster_decision d = {};
	struct listnode *node, *nnode;
	struct midr_node_evidence *ev;

	d.decision_type = MIDR_DECISION_ANCHOR;
	d.old_group_id = mi->local_group_id;
	d.new_group_id = mi->local_group_id;
	d.evidence = list_new();

	cl_select_anchor_candidates(gv, mi->anchor_group_id[0], d.evidence);
	cl_select_anchor_candidates(gv, mi->anchor_group_id[1], d.evidence);

	MIDR_FLOW_LOG(
		"MIDR CL: ANCHOR_PROBE_DONE → 群 %u/%u 共选出 %d 个锚点候选，ANCHOR",
		mi->anchor_group_id[0], mi->anchor_group_id[1],
		listcount(d.evidence));

	midr_nds_on_cluster_decision(bgp, &d);

	for (ALL_LIST_ELEMENTS(d.evidence, node, nnode, ev))
		XFREE(MTYPE_MIDR_NODE_EVIDENCE, ev);
	list_delete(&d.evidence);
}

/* ===========================================================================
 * PERIODIC_SYNC 处理（稳态退群判定）
 * =========================================================================*/

/*
 * 稳态退群判定：复用 cl_count_good_member_links() 对本节点当前所在群
 * （而非候选群）计好链路数，跌破留群阈值时输出 LEAVE。
 *
 * 留群阈值【不能】直接照抄 MIDR_CL_MIN_GOOD_LINKS（入群用的绝对阈值）：
 * 入群评估的是"值不值得加入一个新群"，绝对阈值合理；但留群评估的是"已经
 * 在群里的节点还要不要留下"，若群本身成员数就小于 MIDR_CL_MIN_GOOD_LINKS
 * （现实里群大小 2、3、4 都合法），绝对阈值永远够不着——任何这么小的群会
 * 在热身期一过就让全体成员集体判定"好链路不够"而 LEAVE，群越小越先散伙，
 * 无法长期存在。留群阈值改为 min(MIDR_CL_MIN_GOOD_LINKS, 群内实际认识的
 * 邻接节点总数)：小群只要"认识的都好"就留，大群仍然要求够格的绝对数量。
 *
 * 两个前置守卫：
 *   - 本节点是群代表时不评估：代表退群目前没有"先卸任再走"的编排
 *     （REP_ELECT/REP_RESIGN 判定算法还没做），贸然退群会让整群瞬间失去
 *     代表、答不了 MEMBER_LIST，留给后续把角色判定接上后再一并处理。
 *   - 群号刚变化不足 MIDR_JOIN_PROBE_WAIT_SECS 秒不评估：给 PM 长期 EWMA 留
 *     够收敛时间，否则刚 JOIN/CREATE 完成时群内链路数据还不够，会被误判成
 *     "好链路不够"立即又 LEAVE，形成抖动。
 */
static void cl_handle_periodic_sync(struct bgp *bgp,
				    const struct midr_global_view *gv)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	uint32_t worst_rtt = 0;
	double worst_loss = 0.0;
	size_t good_links, total_adjacent, stay_threshold;
	struct midr_cluster_decision d = {};

	if (mi->local_group_id == 0) {
		MIDR_LOG("MIDR CL: PERIODIC_SYNC — 本节点当前无群，跳过退群判定");
		return;
	}
	if (mi->local_capabilities & MIDR_CAP_GROUP_REP) {
		MIDR_LOG("MIDR CL: PERIODIC_SYNC — 本节点是群代表，跳过退群判定");
		return;
	}
	if (monotime(NULL) - mi->group_settled_at < MIDR_JOIN_PROBE_WAIT_SECS) {
		MIDR_LOG("MIDR CL: PERIODIC_SYNC — 群号刚变化不足 %d 秒，跳过退群判定（等 EWMA 热身）",
			 MIDR_JOIN_PROBE_WAIT_SECS);
		return;
	}

	good_links = cl_count_good_member_links(bgp, gv, mi->local_group_id,
						&worst_rtt, &worst_loss,
						&total_adjacent);
	stay_threshold = total_adjacent < MIDR_CL_MIN_GOOD_LINKS
				  ? total_adjacent
				  : MIDR_CL_MIN_GOOD_LINKS;
	if (good_links >= stay_threshold) {
		MIDR_LOG("MIDR CL: PERIODIC_SYNC — 群 %u 仍有 %zu/%zu 条好链路（认识 %zu 个邻接节点），留群",
			 mi->local_group_id, good_links, stay_threshold,
			 total_adjacent);
		return;
	}

	d.decision_type = MIDR_DECISION_LEAVE;
	d.old_group_id = mi->local_group_id;
	d.new_group_id = 0;
	MIDR_FLOW_LOG("MIDR CL: PERIODIC_SYNC → 群 %u 仅 %zu/%zu 条好链路"
		      "（认识 %zu 个邻接节点，最差 rtt=%u us, loss=%.4f），LEAVE",
		      mi->local_group_id, good_links, stay_threshold,
		      total_adjacent, worst_rtt, worst_loss);
	midr_nds_on_cluster_decision(bgp, &d);
}

/* ===========================================================================
 * I-3 主回调
 * =========================================================================*/

/*
 * I-3 处理：NDS 把全局视图连同触发事件交给 CL。
 *
 * REP_PROBE_DONE / MEMBER_PROBE_DONE 路由到对应实现函数；其余触发事件当前为 stub
 * （仅日志），预留接口供后续稳态周期重评估和能力变更响应扩展。
 */
static void midr_cl_on_global_view(struct bgp *bgp,
				   enum midr_trigger_type trigger,
				   const struct midr_global_view *gv)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;

	if (!mi || !gv)
		return;

	switch (trigger) {
	case MIDR_TRIGGER_REP_PROBE_DONE:
		/*
		 * 守卫：仅在"探群代表"阶段（PROBING_REPS）响应。
		 * 同一批代表可能因 PM 同步/异步产生多次 notify，幂等处理。
		 */
		if (mi->join_phase != MIDR_JOIN_PROBING_REPS) {
			MIDR_LOG("MIDR CL: REP_PROBE_DONE 但不在探代表阶段（join_phase=%d），忽略",
				 mi->join_phase);
			break;
		}
		if (!mi->rep_dir) {
			MIDR_LOG("MIDR CL: REP_PROBE_DONE 但群代表目录为空");
			break;
		}
		cl_handle_rep_probe_done(bgp, gv);
		break;

	case MIDR_TRIGGER_MEMBER_PROBE_DONE:
		/*
		 * 守卫：仅在"探成员"阶段（PROBING_MEMBERS）响应。
		 */
		if (mi->join_phase != MIDR_JOIN_PROBING_MEMBERS) {
			MIDR_LOG("MIDR CL: MEMBER_PROBE_DONE 但不在探成员阶段（join_phase=%d），忽略",
				 mi->join_phase);
			break;
		}
		cl_handle_member_probe_done(bgp, gv);
		break;

	case MIDR_TRIGGER_ANCHOR_PROBE_DONE:
		/*
		 * 评估的是次优群，跟正在加入的候选群是两码事，不受 join_phase
		 * 状态机约束（不像 REP/MEMBER_PROBE_DONE 那样要求处在对应加入
		 * 阶段）。仅当 NDS 确实起了一轮锚点探测（至少一个槽位非 0）时
		 * 才处理。
		 */
		if (mi->anchor_group_id[0] == 0 && mi->anchor_group_id[1] == 0) {
			MIDR_LOG("MIDR CL: ANCHOR_PROBE_DONE 但无锚点候选群，忽略");
			break;
		}
		cl_handle_anchor_probe_done(bgp, gv);
		break;

	case MIDR_TRIGGER_ISOLATED:
		/*
		 * All established sessions (group + anchor) have been gone
		 * for a full debounce window (NDS-side check, see
		 * midr_isolation_check()) -- a connectivity failure, not a
		 * link-quality judgement. Always discard the current group
		 * and restart the join flow, regardless of how
		 * local_group_id got set: PERIODIC_SYNC's LEAVE never even
		 * looks at this case (an isolated node's own group has 0
		 * known adjacent members, which its threshold treats as
		 * "stay"), so nothing else will notice.
		 */
		{
			struct midr_cluster_decision d = {};

			d.decision_type = MIDR_DECISION_RECONNECT;
			d.old_group_id = mi->local_group_id;
			d.new_group_id = 0;
			MIDR_FLOW_LOG("MIDR CL: ISOLATED — 群 %u 已无任何已建立会话，RECONNECT",
				      mi->local_group_id);
			midr_nds_on_cluster_decision(bgp, &d);
		}
		break;

	case MIDR_TRIGGER_CAPABILITY_UPDATE:
		/*
		 * 能力更新：节点通过 TLV 1187 变更能力时触发。
		 * 稳态实现：重评估群内能力匹配度（stub）。
		 */
		MIDR_LOG("MIDR CL: CAPABILITY_UPDATE — 稳态能力重评估（stub）");
		break;

	case MIDR_TRIGGER_PERIODIC_SYNC:
		/*
		 * 周期同步（每 MIDR_PERIODIC_SYNC_INTERVAL 秒）：稳态退群判定，
		 * 见 cl_handle_periodic_sync()。换群（选到更优群后主动切换）
		 * 仍是待办，退群判好之后 NDS 会自动重新走一遍加入流程，效果上
		 * 覆盖了"退群+另择新群"的场景。
		 */
		cl_handle_periodic_sync(bgp, gv);
		break;

	case MIDR_TRIGGER_NODE_CHANGE:
		/*
		 * 节点加入/离开/失效时触发。
		 * 稳态实现：若新节点同群，可用好链路++；若失效节点同群，
		 * 检查留群条件（stub）。
		 */
		MIDR_LOG("MIDR CL: NODE_CHANGE — 分群影响评估（stub）");
		break;
	}
}

/* ===========================================================================
 * 模块初始化接口
 * =========================================================================*/

void midr_cl_register_callback(struct bgp *bgp, midr_global_view_cb cb)
{
	if (bgp && bgp->midr_nds_info)
		bgp->midr_nds_info->cl_callback = cb;
}

void midr_cl_init(struct bgp *bgp)
{
	midr_cl_register_callback(bgp, midr_cl_on_global_view);
}
