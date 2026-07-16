// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR 节点分群（CL）模块。
 *
 * 骨架阶段：与 NDS 的解耦边界——
 *   - 下行 I-3：NDS 调 midr_cl_on_global_view(bgp, trigger, gv)，把全局视图 +
 *     触发事件交给 CL；CL 只读 gv（评估时只看 is_adjacent 邻居子集）。
 *   - 上行 I-7：CL 调 midr_nds_on_cluster_decision(bgp, &decision) 回灌决策；
 *     CL 不碰 BGP-LS / 会话 / 定时器。
 * 队友只需在本文件各 trigger 分支填真实算法 + 填 decision，不动 NDS。
 *
 * 两个主填充点：
 *   REP_PROBE_DONE    -> 选最优群代表（RECOMMEND）
 *   MEMBER_PROBE_DONE -> 入群判定（JOIN / CREATE）
 * 真实选群/入群算法、能力与周期重评估仍为 stub。
 */

#include <zebra.h>

#include "log.h"
#include "prefix.h"
#include "network.h" /* frr_weak_random（随机选群备选策略用） */

#include "bgpd/bgpd.h"
#include "bgpd/bgp_midr.h"
#include "bgpd/bgp_midr_cl.h"
#include "bgpd/bgp_midr_liveness.h"

/*
 * I-3 处理：NDS 把全局视图连同触发事件交给 CL。CL 据 trigger 分支决策，
 * 经 I-7（midr_nds_on_cluster_decision）回灌。
 */
static void midr_cl_on_global_view(struct bgp *bgp,
				   enum midr_trigger_type trigger,
				   const struct midr_global_view *gv)
{
	struct bgp_midr *mi = bgp->midr_info;

	(void)gv; /* 骨架未用；真实算法会遍历 gv 的 is_adjacent 子集 */

	switch (trigger) {
	case MIDR_TRIGGER_REP_PROBE_DONE: {
		struct midr_cluster_decision d = {};
		struct midr_rep_entry *chosen = NULL;
		struct listnode *node;

		/*
		 * CL 对 REP_PROBE_DONE 的响应：从候选群代表里选一个（= 选群），
		 * 经 I-7 RECOMMEND 回灌；NDS 据此向该代表要成员列表。
		 * 候选代表在 join 期间的 mi->rep_dir 中。
		 */
		if (!mi || !mi->rep_dir || list_isempty(mi->rep_dir)) {
			MIDR_LOG("MIDR CL：REP_PROBE_DONE 但群代表目录为空（stub）");
			break;
		}

		/* ===== 队友 CL 填充点：选最优群代表 =====
		 * 默认策略：取群代表目录首条（确定性、可复现）。最终应基于 PM 经
		 * I-5 喂入的链路指标（各代表 is_adjacent 链路的 delay/loss/bw）选
		 * 最优，此处即其落点。 */

		/* --- 策略一：取首条 ACTIVE/未知手工项（默认启用）--- */
		for (ALL_LIST_ELEMENTS_RO(mi->rep_dir, node, chosen))
			if (midr_liveness_transport_usable(
				    bgp, chosen->rep_transport))
				break;
		if (!chosen ||
		    !midr_liveness_transport_usable(bgp,
						   chosen->rep_transport)) {
			MIDR_LOG("MIDR CL：REP_PROBE_DONE 无可用群代表（均为 SUSPECT）");
			break;
		}

		/* --- 策略二：随机选（并列备选，默认注释；联调可解开）---
		{
			struct listnode *n;
			struct midr_rep_entry *r;
			unsigned int pick = frr_weak_random() % mi->rep_dir->count;
			unsigned int i = 0;

			for (ALL_LIST_ELEMENTS_RO(mi->rep_dir, n, r))
				if (i++ == pick) {
					chosen = r;
					break;
				}
		}
		--- 策略二结束 --- */

		d.decision_type = MIDR_DECISION_RECOMMEND;
		d.new_group_id = chosen->group_id;
		d.recommended_rep.family = AF_INET;
		d.recommended_rep.prefixlen = IPV4_MAX_BITLEN;
		d.recommended_rep.u.prefix4 = chosen->rep_transport;
		MIDR_FLOW_LOG("MIDR CL：REP_PROBE_DONE → 推荐群代表 %pI4（群 %u）",
			      &chosen->rep_transport, chosen->group_id);
		midr_nds_on_cluster_decision(bgp, &d);
		break;
	}
	case MIDR_TRIGGER_MEMBER_PROBE_DONE: {
		struct midr_cluster_decision d = {};

		/*
		 * CL 对 MEMBER_PROBE_DONE 的响应：成员探测完成，评估是否入群。
		 * 守卫：只在加入流程的"探成员"阶段处理。
		 */
		if (!mi || mi->join_phase != MIDR_JOIN_PROBING_MEMBERS) {
			MIDR_LOG("MIDR CL：MEMBER_PROBE_DONE 但不在探成员阶段（stub）");
			break;
		}

		/* ===== 队友 CL 填充点：入群判定 =====
		 * 骨架默认"满足入群"→ JOIN 当前 join_group_id。真实规则（与群内
		 * ≥5 节点满足长期 RTT<20ms 则 JOIN，否则尝试次优代表 / CREATE 自建）
		 * 待落地，基于 is_adjacent 成员的链路指标评估。 */
		d.decision_type = MIDR_DECISION_JOIN;
		d.old_group_id = mi->local_group_id;
		d.new_group_id = mi->join_group_id;
		MIDR_FLOW_LOG("MIDR CL：MEMBER_PROBE_DONE → 加入群 %u",
			      mi->join_group_id);
		midr_nds_on_cluster_decision(bgp, &d);
		break;
	}
	case MIDR_TRIGGER_CAPABILITY_UPDATE:
		MIDR_LOG("MIDR CL：CAPABILITY_UPDATE — 重评估受影响节点（stub）");
		break;
	case MIDR_TRIGGER_PERIODIC_SYNC:
		MIDR_LOG("MIDR CL：PERIODIC_SYNC — 周期重评估（stub）");
		break;
	case MIDR_TRIGGER_NODE_CHANGE:
		/*
		 * 全局视图成员变化（节点被发现/撤销/过期/群或能力变更）时触发。
		 * 真实分群在此判断本节点是否需要换群，并经 I-7 回灌决策（最终由
		 * midr_propagate_self 重新通告自己）。暂为 stub。
		 */
		MIDR_LOG("MIDR CL：NODE_CHANGE — 更新分群评估（stub）");
		break;
	}
}

void midr_cl_register_callback(struct bgp *bgp, midr_global_view_cb cb)
{
	if (bgp && bgp->midr_info)
		bgp->midr_info->cl_callback = cb;
}

void midr_cl_init(struct bgp *bgp)
{
	midr_cl_register_callback(bgp, midr_cl_on_global_view);
}
