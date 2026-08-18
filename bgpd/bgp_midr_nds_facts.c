// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR 本地事实表 + 单位换算 —— 对接第二组 7-24 topology 接口（轮 0 备料）
 *
 * 设计说明见 bgp_midr_nds_facts.h。轮 0 只建表与 helper，**无任何调用方**，
 * 故行为零变化；写入方在轮 1（node 上报线）/ 轮 2（link 上报线）接上。
 */

#include <zebra.h>

#include "log.h"
#include "memory.h"
#include "linklist.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_midr.h"
#include "bgpd/bgp_midr_nds.h"
#include "bgpd/bgp_midr_nds_facts.h"

DEFINE_MTYPE_STATIC(BGPD, MIDR_NDS_FACTS, "MIDR NDS fact table");
DEFINE_MTYPE_STATIC(BGPD, MIDR_NDS_FACT_LINK, "MIDR NDS link fact");

/* ===========================================================================
 * 生命周期
 * =========================================================================*/

void midr_nds_facts_init(struct bgp *bgp)
{
	struct bgp_midr_nds *mi;
	struct midr_nds_facts *f;

	if (!bgp || !bgp->midr_nds_info || bgp->midr_nds_info->facts)
		return;

	mi = bgp->midr_nds_info;
	f = XCALLOC(MTYPE_MIDR_NDS_FACTS, sizeof(*f));
	f->links = list_new();
	/* node 事实留空：router-id 就绪后由 midr_nds_facts_node_refresh() 填
	 * （轮 1）。node_valid 为假期间不得上报。 */
	mi->facts = f;

	MIDR_LOG("MIDR facts: 本地事实表就绪 (instance %s)", bgp->name_pretty);
}

void midr_nds_facts_finish(struct bgp *bgp)
{
	struct bgp_midr_nds *mi;
	struct midr_nds_facts *f;
	struct listnode *node, *nnode;
	struct midr_nds_fact_link *fl;

	if (!bgp || !bgp->midr_nds_info || !bgp->midr_nds_info->facts)
		return;

	mi = bgp->midr_nds_info;
	f = mi->facts;

	for (ALL_LIST_ELEMENTS(f->links, node, nnode, fl))
		XFREE(MTYPE_MIDR_NDS_FACT_LINK, fl);
	list_delete(&f->links);

	XFREE(MTYPE_MIDR_NDS_FACTS, f);
	mi->facts = NULL;
}

static struct midr_nds_facts *facts_of(struct bgp *bgp)
{
	if (!bgp || !bgp->midr_nds_info)
		return NULL;

	return bgp->midr_nds_info->facts;
}

/* ===========================================================================
 * node 事实
 * =========================================================================*/

bool midr_nds_facts_node_refresh(struct bgp *bgp)
{
	struct bgp_midr_nds *mi;
	struct midr_nds_facts *f;
	struct midr_node_update next = {};
	uint32_t rid;

	f = facts_of(bgp);
	if (!f)
		return false;

	mi = bgp->midr_nds_info;

	/* node_id = 本机 router-id 的 s_addr 原值。第二组的校验拿
	 * ctx->bgp->router_id.s_addr 逐位比（他们 bgp_midr_input.c:227），
	 * 转主机序会对不上。router-id 为 0 时他们返回 -ENOENT。 */
	rid = bgp->router_id.s_addr;
	if (!rid) {
		if (f->node_valid) {
			/* router-id 撤了：事实作废，但保留 version 单调性 —— 轮 1
			 * 的调用方据 node_valid 决定发 withdraw。 */
			f->node_valid = false;
			MIDR_LOG("MIDR facts: router-id 撤销, node 事实作废");
			return true;
		}
		return false;
	}

	next.node_id = rid;
	next.group_id = mi->local_group_id;
	next.cap_flags = mi->local_capabilities; /* 附录 A：低 32 位放现有能力位 */

	if (mi->transport_addr_set) {
		next.has_transport_address = true;
		next.transport_address.ipa_type = IPADDR_V4;
		next.transport_address.ipaddr_v4 = mi->local_transport_addr;
	} else {
		/* 他们要求 has_transport_address 为假时 ipa_type 必须是
		 * IPADDR_NONE（不能留脏值），XCALLOC 的 0 正好是它。 */
		next.has_transport_address = false;
	}

	/* policy_state：MIDR_POLICY_ALLOWED == 0，即默认值。
	 * policy_tags：文档 §12「无已定义值时填 0」—— 第二组自己还没定义任何
	 * 取值，恒填 0，直到他们在附录里登记含义（计划 Q14）。 */
	next.policy_state = MIDR_POLICY_ALLOWED;
	next.policy_tags = 0;

	/* 逐字段比，只有实质变化才返回 true（幂等，Q8 的频率控制天然满足）。
	 * version 不参与比较，故先拿旧值填上再比。 */
	next.version = f->node.version;
	if (f->node_valid && !memcmp(&f->node, &next, sizeof(next)))
		return false;

	/* version **不在这里递增** —— 它是「上报序号」而非「事实修订号」，归
	 * midr_nds_report_node() 在真正发出事件时递增（理由见头文件的 version
	 * 语义说明）。本函数只更新事实内容与 node_valid。 */
	f->node = next;
	f->node_valid = true;

	MIDR_LOG("MIDR facts: node 事实更新 rid=%pI4 group=%u caps=0x%" PRIx64,
		 &bgp->router_id, next.group_id, next.cap_flags);
	return true;
}

/* ===========================================================================
 * node 上报（轮 1）—— 设计与调用点清单见头文件
 * =========================================================================*/

struct midr_context *midr_nds_group2_ctx(struct bgp *bgp)
{
	struct bgp_midr_nds *mi;

	if (!bgp || !bgp->midr_nds_info)
		return NULL;

	mi = bgp->midr_nds_info;
	if (!mi->g2_ctx)
		mi->g2_ctx = midr_context_get_default();

	return mi->g2_ctx;
}

void midr_nds_report_node(struct bgp *bgp, enum midr_origin_reason reason)
{
	struct bgp_midr_nds *mi;
	struct midr_nds_facts *f;
	struct midr_context *ctx;
	const char *why = midr_origin_reason_str(reason);
	bool changed;
	int ret;

	f = facts_of(bgp);
	if (!f)
		return;

	mi = bgp->midr_nds_info;

	ctx = midr_nds_group2_ctx(bgp);
	if (!ctx) {
		zlog_warn("MIDR facts: topology context 不可用，node 上报跳过 (%s)",
			  why);
		return;
	}

	/* 撤销路径也要先刷：router-id 被撤时靠 refresh 把 node_valid 打掉。 */
	changed = midr_nds_facts_node_refresh(bgp);

	/*
	 * withdraw 分支 = LEAVE（`midr shutdown`）或事实已失效（router-id 撤销）。
	 * 键取 f->node —— 事实作废时它仍留着上次报出去的那把键，正是对方认得的
	 * 那个 node_id/version。
	 *
	 * LEAVE 无论报没报过都撤（与改造前 midr_propagate_self(LEAVE) 无条件
	 * withdraw 等价）；事实失效则只在报过时才有东西可撤。两种情况都要求
	 * node_id 非 0，否则连键都没有。
	 */
	if (reason == MIDR_ORIGIN_LEAVE || !f->node_valid) {
		if (!f->node.node_id ||
		    (reason != MIDR_ORIGIN_LEAVE && !f->node_reported))
			return;

		/* 见下方 upsert 处的 version 说明：撤销同样要带更高的 version，
		 * 否则被对方当旧事件静默丢弃。 */
		f->node.version++;
		ret = midr_topology_node_withdraw(ctx, f->node.node_id,
						  f->node.version);
		if (ret) {
			zlog_warn("MIDR facts: node_withdraw 失败 ret=%d (%s)",
				  ret, why);
			return;
		}

		f->node_reported = false;
		MIDR_LOG("MIDR facts: node 撤销上报 (%s) node_id=%u version=%" PRIu64,
			 why, f->node.node_id, f->node.version);
		return;
	}

	/* 优雅下线期间不上报「我还在」。事实内容照更（上面 refresh 已写入），只是
	 * 不发出去 —— version 不动、node_reported 保持假，于是 `no midr shutdown`
	 * 必然重报，且带的正是下线期间攒下的新身份。 */
	if (mi->shutdown) {
		MIDR_LOG("MIDR facts: 已优雅下线，node 上报抑制 (%s)", why);
		return;
	}

	/* 幂等：无实质变化且已报过就不重报。!node_reported 那一半覆盖
	 * `no midr shutdown` —— 那时身份与下线前完全相同，changed 为假。 */
	if (!changed && f->node_reported)
		return;

	/*
	 * version 在**发出前**递增，而不是在事实变化时 —— 它的语义是「上报序号」。
	 * 硬依据：他们 apply 阶段对 node/link 的 upsert 与 withdraw 一律
	 * `version <= 已存版本` 即丢弃（bgp_midr_input.c:293 / :316 / :344 / :367
	 * @c344e40ba1），而且**不报错**：事件被计进 ignored_old，API 照样返回 0。
	 * 若 version 只跟事实走，两条路会静默失效：
	 *   - `midr shutdown` 的 withdraw 与上次 upsert 同版 -> 撤销被丢；
	 *   - `no midr shutdown` 的重报事实没变、又是同版 -> 重报被丢。
	 * ⚠ 这道闸门在 apply 阶段、不在 validate 阶段，所以 shim 里逐条镜像的
	 * 校验**照不出来**（shim 无事实表）。轮 2 的 link 上报同理。
	 */
	f->node.version++;

	ret = midr_topology_node_upsert(ctx, &f->node);
	if (ret) {
		/* 真实现的返回码语义（轮 0 读码所得）：-EINVAL 校验拒收 /
		 * -EAGAIN 对方状态机 resync 中拒收 / -ENOSPC 队列满**且对方直接
		 * 进 OUT_OF_SYNC**。shim 期只会出 0 或 -EINVAL，但按真实现语义
		 * 至少留一条 warn，别让失败静默。 */
		zlog_warn("MIDR facts: node_upsert 失败 ret=%d (%s)", ret, why);
		return;
	}

	f->node_reported = true;
}

/* ===========================================================================
 * link 事实
 * =========================================================================*/

struct midr_nds_fact_link *midr_nds_facts_link_find(struct bgp *bgp,
						    uint32_t remote_node_id,
						    uint64_t link_id)
{
	struct midr_nds_facts *f = facts_of(bgp);
	struct listnode *node;
	struct midr_nds_fact_link *fl;

	if (!f)
		return NULL;

	for (ALL_LIST_ELEMENTS_RO(f->links, node, fl))
		if (fl->data.key.remote_node_id == remote_node_id &&
		    fl->data.key.link_id == link_id)
			return fl;

	return NULL;
}

struct midr_nds_fact_link *midr_nds_facts_link_get(struct bgp *bgp,
						   uint32_t remote_node_id,
						   uint64_t link_id)
{
	struct midr_nds_facts *f = facts_of(bgp);
	struct midr_nds_fact_link *fl;

	if (!f)
		return NULL;

	fl = midr_nds_facts_link_find(bgp, remote_node_id, link_id);
	if (fl)
		return fl;

	fl = XCALLOC(MTYPE_MIDR_NDS_FACT_LINK, sizeof(*fl));
	fl->data.key.local_node_id = bgp->router_id.s_addr;
	fl->data.key.remote_node_id = remote_node_id;
	fl->data.key.link_id = link_id;
	/* local_ifindex = 0：overlay 多跳链路出口由路由表现算、没有固定出接口，
	 * 文档约定填 0 = 不适用（Q16）。XCALLOC 已置 0。 */
	fl->data.policy_state = MIDR_POLICY_ALLOWED;
	/* 0 = 尚未上报过。首次 upsert 时由上报层递增到 1 —— version 是「上报
	 * 序号」，与 node 侧同一套语义（见 midr_nds_report_node 里的说明）。 */
	fl->data.version = 0;
	listnode_add(f->links, fl);

	return fl;
}

void midr_nds_facts_link_del(struct bgp *bgp, struct midr_nds_fact_link *fl)
{
	struct midr_nds_facts *f = facts_of(bgp);

	if (!f || !fl)
		return;

	listnode_delete(f->links, fl);
	XFREE(MTYPE_MIDR_NDS_FACT_LINK, fl);
}

uint64_t midr_nds_facts_link_id(struct bgp *bgp, uint32_t remote_node_id)
{
	(void)bgp;
	(void)remote_node_id;

	/* 恒 0 —— 理由与将来换分配器的说明见头文件。 */
	return 0;
}

/* ===========================================================================
 * 单位换算
 * =========================================================================*/

uint32_t midr_nds_metric_rtt_us(const struct midr_nds_link_metrics *m)
{
	if (!m)
		return 0;

	return m->rtt_us; /* 内部已是微秒，平移 */
}

uint32_t midr_nds_metric_loss_ppm(const struct midr_nds_link_metrics *m)
{
	double ppm;

	if (!m || m->loss_rate <= 0.0)
		return 0;

	ppm = m->loss_rate * 1000000.0;
	if (ppm >= 999999.0)
		return 999999U; /* 他们是 >= 1e6 即 -EINVAL，夹在门槛内 */

	return (uint32_t)(ppm + 0.5);
}

uint32_t midr_nds_metric_bw_kbps(const struct midr_nds_link_metrics *m)
{
	static bool warned;

	(void)m; /* bw_score 无量纲，无法换成 kbps —— 见头文件 TODO（问题 #7） */

	if (!warned) {
		warned = true;
		zlog_warn("MIDR facts: available_bandwidth_kbps 用占位值 %u kbps 上报（PM 无真实带宽测量，等问题清单 #7 口径落定）",
			  MIDR_NDS_BW_KBPS_PLACEHOLDER);
	}

	return MIDR_NDS_BW_KBPS_PLACEHOLDER;
}

bool midr_nds_metrics_to_group2(const struct midr_nds_link_metrics *m,
				uint64_t seqno,
				struct midr_link_metrics *out)
{
	uint32_t rtt_us;

	if (!m || !out)
		return false;

	/* rtt == 0 = 还没探到（热身期 / 不可达）。「接口不定义 link_state，
	 * 探测中/热身中不提交」—— 正好对上我方「60s 热身、rtt=0 视为未探到」
	 * 的既有判据（Q16）。 */
	rtt_us = midr_nds_metric_rtt_us(m);
	if (!rtt_us)
		return false;

	/* 全丢：第二组要求转 withdraw，不许 upsert。 */
	if (m->loss_rate >= 1.0)
		return false;

	memset(out, 0, sizeof(*out));
	out->has_rtt_us = true;
	out->rtt_us = rtt_us;
	out->has_loss_ppm = true;
	out->loss_ppm = midr_nds_metric_loss_ppm(m);
	out->has_available_bandwidth_kbps = true;
	out->available_bandwidth_kbps = midr_nds_metric_bw_kbps(m);
	out->measurement_seqno = seqno;
	/* measurement_timestamp_ms 留 0：轮 2 接上报时由调用方按需填，避免在
	 * 换算层引进时间源。 */

	return true;
}
