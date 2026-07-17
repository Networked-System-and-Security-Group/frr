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
#include "prefix.h"
#include "linklist.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_midr.h"
#include "bgpd/bgp_midr_cl.h"

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

/*
 * 在全局视图链路列表中按 IPv4 地址查找链路条目。
 * 用于 REP_PROBE_DONE：群代表的探测 key 是 transport addr（locator），
 * 不一定有对应的 node 条目。
 */
static struct midr_link_entry *
cl_find_link_by_ipv4(const struct midr_global_view *gv,
		     const struct in_addr *addr)
{
	struct listnode *n;
	struct midr_link_entry *link;

	for (ALL_LIST_ELEMENTS_RO(gv->links, n, link)) {
		if (link->remote_node_id.family == AF_INET &&
		    IPV4_ADDR_SAME(&link->remote_node_id.u.prefix4, addr))
			return link;
	}
	return NULL;
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
static bool cl_metrics_is_better(const struct midr_link_metrics *cand,
				  const struct midr_link_metrics *best)
{
	if (cand->rtt_us != best->rtt_us)
		return cand->rtt_us < best->rtt_us;
	if (cand->loss_rate != best->loss_rate)
		return cand->loss_rate < best->loss_rate;
	return cand->bw_score > best->bw_score;
}

/*
 * 遍历 gv->nodes，找出其中最大的 group_id，用于 CREATE 分配新群编号。
 * 若节点表为空，返回 0（CREATE 新群 ID 将为 1）。
 */
static uint32_t cl_max_group_id(const struct midr_global_view *gv)
{
	struct midr_node_entry *entry;
	uint32_t max_id = 0;

	frr_each (midr_node_hash, (struct midr_node_hash_head *)&gv->nodes,
		  entry) {
		if (entry->group_id > max_id)
			max_id = entry->group_id;
	}
	return max_id;
}

/* ===========================================================================
 * REP_PROBE_DONE 处理
 * =========================================================================*/

/*
 * 从 mi->rep_dir（群代表目录）中，结合 gv->links 的长期探测指标，选出性能最优的
 * 群代表，经 I-7 RECOMMEND 回灌。
 *
 * 候选筛选条件：链路状态 UP 且已有探测数据（rtt_us > 0）。
 * 排序规则：长期 RTT 更低 → 丢包率更低 → 带宽分数更高 → group_id 更小（稳定）。
 *
 * 无可用候选时输出 CREATE（new_group_id = 当前最大 group_id + 1）。
 */
static void cl_handle_rep_probe_done(struct bgp *bgp,
				     const struct midr_global_view *gv)
{
	struct bgp_midr *mi = bgp->midr_info;
	struct listnode *n;
	struct midr_rep_entry *r;
	struct midr_rep_entry *best_rep = NULL;
	struct midr_link_entry *best_link = NULL;
	struct midr_cluster_decision d = {};

	for (ALL_LIST_ELEMENTS_RO(mi->rep_dir, n, r)) {
		struct midr_link_entry *link;

		link = cl_find_link_by_ipv4(gv, &r->rep_transport);
		if (!cl_link_has_data(link))
			continue;

		if (!best_rep ||
		    cl_metrics_is_better(&link->long_term,
					 &best_link->long_term) ||
		    (link->long_term.rtt_us == best_link->long_term.rtt_us &&
		     link->long_term.loss_rate ==
			     best_link->long_term.loss_rate &&
		     link->long_term.bw_score ==
			     best_link->long_term.bw_score &&
		     r->group_id < best_rep->group_id)) {
			best_rep = r;
			best_link = link;
		}
	}

	if (!best_rep) {
		uint32_t new_gid = cl_max_group_id(gv) + 1;

		d.decision_type = MIDR_DECISION_CREATE;
		d.new_group_id = new_gid;
		d.old_group_id = mi->local_group_id;
		MIDR_FLOW_LOG(
			"MIDR CL: REP_PROBE_DONE → 无可用群代表，CREATE 新群 %u",
			new_gid);
	} else {
		d.decision_type = MIDR_DECISION_RECOMMEND;
		d.new_group_id = best_rep->group_id;
		d.old_group_id = mi->local_group_id;
		d.recommended_rep.family = AF_INET;
		d.recommended_rep.prefixlen = IPV4_MAX_BITLEN;
		d.recommended_rep.u.prefix4 = best_rep->rep_transport;
		MIDR_FLOW_LOG(
			"MIDR CL: REP_PROBE_DONE → RECOMMEND 群 %u 代表 %pI4"
			"（rtt=%u us, loss=%.4f, bw=%u）",
			best_rep->group_id, &best_rep->rep_transport,
			best_link->long_term.rtt_us,
			best_link->long_term.loss_rate,
			best_link->long_term.bw_score);
	}

	midr_nds_on_cluster_decision(bgp, &d);
}

/* ===========================================================================
 * MEMBER_PROBE_DONE 处理
 * =========================================================================*/

/*
 * 统计目标群（mi->join_group_id）内满足入群条件的 is_adjacent 邻居数量，
 * 并收集用于日志的最差 RTT 和最差 loss_rate（便于排查未达标原因）。
 */
static size_t cl_count_good_member_links(const struct bgp *bgp,
					 const struct midr_global_view *gv,
					 uint32_t target_group_id,
					 uint32_t *out_worst_rtt_us,
					 double *out_worst_loss)
{
	struct midr_node_entry *entry;
	size_t good = 0;
	uint32_t worst_rtt = 0;
	double worst_loss = 0.0;

	frr_each (midr_node_hash, (struct midr_node_hash_head *)&gv->nodes,
		  entry) {
		struct midr_link_entry *link;

		if (!entry->is_adjacent || entry->is_self)
			continue;
		if (entry->group_id != target_group_id)
			continue;

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

	if (out_worst_rtt_us)
		*out_worst_rtt_us = worst_rtt;
	if (out_worst_loss)
		*out_worst_loss = worst_loss;

	return good;
}

/*
 * 处理 MEMBER_PROBE_DONE：
 *   目标群 = mi->join_group_id（由 RECOMMEND 阶段确定）。
 *   遍历全局视图中该群的 is_adjacent 节点，统计满足长期 RTT/丢包率阈值的数量。
 *   ≥ MIDR_CL_MIN_GOOD_LINKS 则 JOIN；否则 CREATE（分配新群 ID = 最大 + 1）。
 *
 * 注：当前 NDS 不支持次优代表重试流程（RECOMMEND guard 仅在 PROBING_REPS 阶段
 * 生效）。若需要重试，后续可在 NDS 放开 guard 后在此输出 RECOMMEND，CL 保存有
 * 序候选列表即可实现 group-demo 的完整回退逻辑。
 */
static void cl_handle_member_probe_done(struct bgp *bgp,
					const struct midr_global_view *gv)
{
	struct bgp_midr *mi = bgp->midr_info;
	uint32_t target_gid = mi->join_group_id;
	uint32_t worst_rtt = 0;
	double worst_loss = 0.0;
	size_t good_links;
	struct midr_cluster_decision d = {};

	good_links = cl_count_good_member_links(bgp, gv, target_gid,
						&worst_rtt, &worst_loss);

	if (good_links >= MIDR_CL_MIN_GOOD_LINKS) {
		d.decision_type = MIDR_DECISION_JOIN;
		d.new_group_id = target_gid;
		d.old_group_id = mi->local_group_id;
		MIDR_FLOW_LOG(
			"MIDR CL: MEMBER_PROBE_DONE → JOIN 群 %u"
			"（%zu 条好链路，达到 %u 条阈值）",
			target_gid, good_links, MIDR_CL_MIN_GOOD_LINKS);
	} else {
		uint32_t new_gid = cl_max_group_id(gv) + 1;

		d.decision_type = MIDR_DECISION_CREATE;
		d.new_group_id = new_gid;
		d.old_group_id = mi->local_group_id;
		MIDR_FLOW_LOG(
			"MIDR CL: MEMBER_PROBE_DONE → 群 %u 仅 %zu/%u 条好链路"
			"（最差 rtt=%u us, loss=%.4f），CREATE 新群 %u",
			target_gid, good_links, MIDR_CL_MIN_GOOD_LINKS,
			worst_rtt, worst_loss, new_gid);
	}

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
	struct bgp_midr *mi = bgp->midr_info;

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
		if (!mi->rep_dir || list_isempty(mi->rep_dir)) {
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

	case MIDR_TRIGGER_CAPABILITY_UPDATE:
		/*
		 * 能力更新：节点通过 TLV 1187 变更能力时触发。
		 * 稳态实现：重评估群内能力匹配度（stub）。
		 */
		MIDR_LOG("MIDR CL: CAPABILITY_UPDATE — 稳态能力重评估（stub）");
		break;

	case MIDR_TRIGGER_PERIODIC_SYNC:
		/*
		 * 周期同步（每 MIDR_PERIODIC_SYNC_INTERVAL 秒）：
		 * 稳态实现：检查留群条件（长期 RTT < 40ms，≥3 节点），
		 * 不满足时触发换群或退群（stub）。
		 */
		MIDR_LOG("MIDR CL: PERIODIC_SYNC — 稳态分群重评估（stub）");
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
	if (bgp && bgp->midr_info)
		bgp->midr_info->cl_callback = cb;
}

void midr_cl_init(struct bgp *bgp)
{
	midr_cl_register_callback(bgp, midr_cl_on_global_view);
}
