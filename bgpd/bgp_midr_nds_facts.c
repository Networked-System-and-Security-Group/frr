// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR 本地事实表 + 单位换算 —— 对接第二组 7-24 topology 接口（轮 0 备料）
 *
 * 设计说明见 bgp_midr_nds_facts.h。轮 0 只建表与 helper，**无任何调用方**，
 * 故行为零变化；写入方在轮 1（node 上报线）/ 轮 2（link 上报线）接上。
 */

#include <zebra.h>

#include <errno.h>

#include "log.h"
#include "memory.h"
#include "linklist.h"
#include "prefix.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_midr.h"
#include "bgpd/bgp_midr_nds.h"
#include "bgpd/bgp_midr_nds_facts.h"

DEFINE_MTYPE_STATIC(BGPD, MIDR_NDS_FACTS, "MIDR NDS fact table");
DEFINE_MTYPE_STATIC(BGPD, MIDR_NDS_FACT_LINK, "MIDR NDS link fact");
DEFINE_MTYPE_STATIC(BGPD, MIDR_NDS_SNAPSHOT, "MIDR NDS topology snapshot");

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

	/*
	 * 引导节点不上报自己（专职化：只转发、不自产）。
	 *
	 * 与 link 侧那道保险同因：现状引导什么都不发，靠的是"成为引导即自动关
	 * distribute"，而那道判据只把守旧 NLRI 通道；轮 4 换第二组真实现后上报
	 * 不再路过它，没有这道守卫引导就会把自己报进拓扑（它群号恒 0，对方还会
	 * 按"群 0 → withdraw Membership"再撤一次，白跑一趟）。
	 *
	 * 报过之后才成为引导（`midr role bootstrap` 现配）的，先把自己撤干净再
	 * 闭嘴 —— 否则对方视图里留着一条永不刷新的陈旧条目。
	 */
	if (midr_nds_is_bootstrap(bgp)) {
		if (f->node_reported && f->node.node_id) {
			f->node.version++;
			ret = midr_topology_node_withdraw(ctx, f->node.node_id,
							  f->node.version);
			if (ret) {
				zlog_warn("MIDR facts: 引导节点撤销自身上报失败 ret=%d (%s)",
					  ret, why);
				return;
			}
			f->node_reported = false;
			MIDR_LOG("MIDR facts: 本机是引导节点，撤销自身上报后闭嘴 (%s)",
				 why);
			return;
		}
		MIDR_LOG("MIDR facts: 本机是引导节点，node 上报跳过 (%s)", why);
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

	/*
	 * 满格丢包（loss_rate >= 1.0）**照报**，不再返回 false 转 withdraw
	 * （2026-08-19 口径改定，全案见头文件本函数注释）：loss_ppm 已由
	 * midr_nds_metric_loss_ppm() 夹到 999999，过得了他们的值域校验；链路
	 * 生死归会话/节点级，指标层只管说"这条现在有多烂"。
	 */

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

/* ===========================================================================
 * link 上报（轮 2）—— 设计、闸门与调用点见头文件
 * =========================================================================*/

void midr_nds_report_link(struct bgp *bgp, const struct midr_link_entry *link)
{
	struct bgp_midr_nds *mi;
	struct midr_nds_facts *f;
	struct midr_context *ctx;
	struct midr_nds_fact_link *fl;
	struct midr_link_metrics metrics = {};
	uint32_t local_rid, remote_rid;
	uint64_t link_id, seqno;
	int ret;

	f = facts_of(bgp);
	if (!f || !link)
		return;

	mi = bgp->midr_nds_info;

	ctx = midr_nds_group2_ctx(bgp);
	if (!ctx) {
		zlog_warn("MIDR facts: topology context 不可用，link 上报跳过 (%pFX)",
			  &link->remote_node_id);
		return;
	}

	/*
	 * 闸门③ 键合法性。本机 router-id 是他们 midr_validate_link_update() 的
	 * 第一关（为 0 直接 -ENOENT）；对端 rid 为 0 或等于本机同样 -EINVAL。
	 * 与 node 是否已上报无关 —— 他们拿的就是 ctx->bgp->router_id.s_addr，
	 * 不存在"先立户"这道前置。
	 */
	local_rid = bgp->router_id.s_addr;
	remote_rid = link->remote_node_id.u.prefix4.s_addr;
	if (!local_rid || !remote_rid || local_rid == remote_rid)
		return;

	/*
	 * 保险：引导自己不报，连引导/保底边也不报。
	 *
	 * 为什么非要在这里再拦一道 —— 现状引导之所以什么都不发，靠的是"成为引导
	 * 即自动关 distribute"，而那道判据长在 bgp_ls_originate_bgp_node() 里、
	 * 只把守**旧 NLRI 通道**；轮 4 换第二组真实现后，上报走的是他们的 API、
	 * 根本不路过 originate，那个开关就管不着了。所以守卫必须落在**我方上报
	 * 出口**（并且不能写进 bgp_midr_group2_shim.c —— 那个文件轮 4 整份删除）。
	 *
	 * 判据见 midr_nds_link_is_backbone()：台账 BACKBONE/ATTACH ∪ 引导候选池
	 * rid ∪ 节点表 BOOTSTRAP 位。已在《致第二组-引导节点豁免与确认》§2 向
	 * 对方预告过"连引导的链路我方主动不上报"。
	 */
	if (midr_nds_is_bootstrap(bgp)) {
		MIDR_LOG("MIDR facts: 本机是引导节点，link 上报跳过 (%pFX)",
			 &link->remote_node_id);
		return;
	}
	if (midr_nds_link_is_backbone(bgp, &link->remote_node_id)) {
		MIDR_LOG("MIDR facts: 保底边（对端引导），link 上报跳过 (%pFX)",
			 &link->remote_node_id);
		return;
	}

	/*
	 * 闸门① 会话 Established。会话都没建就上报这条链路等于虚报 —— 何况
	 * shim 期转调的 E-1 本来也要靠这个 peer 才发得出 Link NLRI（找不到就
	 * 空转），这道闸门同时保证换壳前后的发送时机一致。
	 */
	if (!midr_node_established_peer(bgp, &link->remote_node_id)) {
		if (BGP_DEBUG(midr, MIDR))
			zlog_debug("MIDR facts: 无 Established 会话，link 上报跳过 (%pFX)",
				   &link->remote_node_id);
		return;
	}

	/*
	 * 满格丢包 = 链路不可用，转**撤销**（2026-08-20 口径改定，推翻 08-19 的
	 * "夹 999999 照报"）。依据是第二组原文档 §5.5 白纸黑字：`loss_ppm ==
	 * 1000000` 表示 Link 不可用，第一组不得继续 upsert ACTIVE、对已存在对象
	 * 应调用 link withdraw —— 旧口径当初的前提"死链怎么表达他们没规定"是我方
	 * 漏读。取舍理由：上层要的是链路的准确消息，"还活着但丢 99.9999%"是我方
	 * 编出来的第三态；链路若其实还活着，下一拍探到就自然重新 upsert。
	 *
	 * 前提 rtt_us 非 0 —— 热身期/不可达时 loss_rate 也可能是 1.0，那属于
	 * "还没探到"，归下面闸门② 管，不能当成"探到了满格"。
	 *
	 * 位置在 link_get 之前：满格且从没报过时不该新建条目（那会凭空造一个墓碑）。
	 */
	if (link->short_term.rtt_us && link->short_term.loss_rate >= 1.0) {
		midr_nds_report_link_withdraw(bgp, &link->remote_node_id);
		return;
	}

	link_id = midr_nds_facts_link_id(bgp, remote_rid);
	fl = midr_nds_facts_link_get(bgp, remote_rid, link_id);
	if (!fl)
		return;

	/*
	 * 闸门② 指标可用。热身期（rtt=0）不报 —— 报了也过不了他们的校验，还会
	 * 污染视图。满格丢包**不再走这里退出**（口径改定，见 to_group2 注释）。
	 *
	 * seqno 先填 0，过了闸门再补真值：递增必须发生在**确定要发**之后，否则
	 * 热身期被挡下的那几次会白白吃掉序号（实测过：seqno 会恒领先 version 1）。
	 */
	if (!midr_nds_metrics_to_group2(&link->short_term, 0, &metrics)) {
		if (BGP_DEBUG(midr, MIDR))
			zlog_debug("MIDR facts: 指标尚不可上报（热身/未探到），link 上报跳过 (%pFX)",
				   &link->remote_node_id);
		return;
	}

	/*
	 * seqno 在这里递增（原先在 E-1 内部）：measurement_seqno 与 TLV 1186 的
	 * seqno 必须是同一个数，否则 shim 期两条路各自计数、日志对不上。shim 的
	 * link_upsert 从 link->metrics.measurement_seqno 取回它传给 E-1。
	 */
	seqno = ++mi->perf_seqno;
	metrics.measurement_seqno = seqno;

	/*
	 * 链路两端地址：他们的校验要求两个都 present 且同族（IPv4）。overlay
	 * 链路没有"接口地址"这一说，用两端的 locator —— 本端取 transport（没配
	 * 则回落 router-id，与 midr_node_get_locator 的口径一致）、对端取
	 * remote_node_id。local_ifindex 恒 0（多跳链路出口由路由表现算，文档
	 * 约定填 0 = 不适用），XCALLOC 已置 0。
	 */
	fl->data.link_local_address.ipa_type = IPADDR_V4;
	fl->data.link_local_address.ipaddr_v4 = mi->transport_addr_set
						       ? mi->local_transport_addr
						       : bgp->router_id;
	fl->data.link_remote_address.ipa_type = IPADDR_V4;
	fl->data.link_remote_address.ipaddr_v4 = link->remote_node_id.u.prefix4;
	fl->data.metrics = metrics;
	fl->data.policy_state = MIDR_POLICY_ALLOWED;

	/* version 是「上报序号」，发出前递增（理由见 midr_nds_report_node）。 */
	fl->data.version++;

	ret = midr_topology_link_upsert(ctx, &fl->data);
	if (ret) {
		/* -EINVAL 我方填错 / -EAGAIN 对方 resync 中 / -ENOSPC 队列满且
		 * 对方进 OUT_OF_SYNC。别让失败静默（node 侧同款）。 */
		zlog_warn("MIDR facts: link_upsert 失败 ret=%d (%pFX)", ret,
			  &link->remote_node_id);
		return;
	}

	fl->reported = true;
}

void midr_nds_report_link_withdraw(struct bgp *bgp,
				   const struct prefix *remote_node_id)
{
	struct midr_nds_facts *f;
	struct midr_context *ctx;
	struct midr_nds_fact_link *fl;
	uint32_t remote_rid;
	uint64_t link_id;
	int ret;

	f = facts_of(bgp);
	if (!f || !remote_node_id)
		return;

	remote_rid = remote_node_id->u.prefix4.s_addr;
	if (!remote_rid)
		return;

	link_id = midr_nds_facts_link_id(bgp, remote_rid);
	fl = midr_nds_facts_link_find(bgp, remote_rid, link_id);
	if (!fl)
		return; /* 没有事实条目 = 从没报过，无事可做 */

	/* 报过才撤：没报过对方那边压根没有这条，撤了是无中生有。 */
	if (!fl->reported)
		return;

	ctx = midr_nds_group2_ctx(bgp);
	if (!ctx) {
		zlog_warn("MIDR facts: topology context 不可用，link 撤销跳过 (%pFX)",
			  remote_node_id);
		return; /* 条目留着，等 context 就绪后还能撤 */
	}

	/* 撤销同样要带更高的 version，否则被对方当旧事件静默丢弃。 */
	fl->data.version++;
	ret = midr_topology_link_withdraw(ctx, &fl->data.key, fl->data.version);
	if (ret) {
		zlog_warn("MIDR facts: link_withdraw 失败 ret=%d (%pFX)", ret,
			  remote_node_id);
		return;
	}

	/*
	 * ⚠ 撤销后**保留事实条目当墓碑**，只把 reported 打回假 —— 不再删条目
	 * （2026-08-20 修，推翻轮 2 的"撤完删条目"）。
	 *
	 * 原因在对方的 apply 阶段：他们 withdraw 也不删条目，而是留一条
	 * active=false 的墓碑并**记住 withdraw 用的 version**（他们
	 * midr_apply_link_withdraw）；此后同键 upsert 只要 `version <= 墓碑版本`
	 * 就被静默丢弃（计 ignored_old，API 照样返回 0）。我方若删掉条目，重建时
	 * version 从 1 重起 —— 凡是"撤过又重报"的链路（rejoin、换组回迁、满格
	 * 丢包恢复）首批 upsert 会全被吞，且没有任何错误可看。
	 *
	 * 所以条目留着、version 就地延续，只在 facts_finish() 释放。
	 * ⚠ shim 没有事实表、镜像不了 apply 阶段，这个坑在 shim 期测不出来。
	 *
	 * 墓碑不会无限增长：link_id 恒 0，每个对端至多一条。snapshot 侧靠
	 * reported 位把墓碑滤掉（见 snapshot_link_eligible）。
	 */
	fl->reported = false;

	MIDR_LOG("MIDR facts: link 撤销上报 (%pFX) version=%" PRIu64,
		 remote_node_id, fl->data.version);
}

/* ===========================================================================
 * snapshot provider（轮 3）—— **反方向**：我方实现、第二组调用
 *
 * 原型在 bgp_midr.h（拷自第二组），故本文件不再声明。它**不在**
 * bgp_midr_group2_shim.c 里 —— 那个文件装的是"他们实现、我方调用"的假实现、
 * 轮 4 整份删除；snapshot 是 provider 方向，是我方长期件。他们树里那两个带
 * __attribute__((weak))（bgp_midr_input.c:111/122，返回 -ENOSYS），我方的强定义
 * 会覆盖它们，合树不会出现重复符号。
 *
 * 语义（他们原文档 §4.3 + 实读消费侧 bgp_midr_input.c）：
 *   - 返回 0 = **完整权威全量**。快照里缺席的旧 active 对象由他们视为已失效并
 *     生成权威撤销 —— 所以"少放一个"不是无害的省略，是一次注销；
 *   - 一票否决：任一对象过不了他们的校验 -> 整份 resync 判失败、保留旧基线、
 *     定时重试（他们 midr_snapshot_to_fact_table）。发出前自检是唯一兜得住的位置；
 *   - 未就绪返 -EAGAIN，他们保留当前状态稍后重试（不得解释成"对象全撤"）；
 *   - 数组由我方分配、对他们只读，他们拷走需要的字段后调 _release 交还我方释放。
 *
 * 一致读取：bgpd 是单线程事件循环，本函数执行期间没有别的代码能改事实表，
 * 天然满足他们"snapshot 期间的变化必须进快照或进后续增量、不得两头落空"的要求。
 * =========================================================================*/

static void snapshot_prefix_from_rid(struct prefix *p, uint32_t rid)
{
	memset(p, 0, sizeof(*p));
	p->family = AF_INET;
	p->prefixlen = IPV4_MAX_BITLEN;
	p->u.prefix4.s_addr = rid;
}

/* ---------------------------------------------------------------------------
 * 第一层：入选（该不该在基线里）—— 判据与上报出口逐条同源
 *
 * 主判据是 reported 位（= 对方账上现在有没有这条），但它只答"报出去过没有"、
 * 不答"现在还该不该在"：会话刚断而清理还没跑到时，条目仍挂着 reported=true，
 * 只信这个位就会在 resync 时把一条平时已经报不出去的链路倒进基线。所以
 * reported 位之外再照抄一遍上报闸门 —— 两边一致，才不会出现"平时报得上去、
 * resync 时整份被拒"或反之。
 * -------------------------------------------------------------------------*/

static bool snapshot_node_eligible(struct bgp *bgp,
				   const struct midr_nds_facts *f)
{
	if (!f->node_reported || !f->node_valid)
		return false;

	/* 引导专职化 / 优雅下线期间本就不报（与 midr_nds_report_node 同判据）。
	 * 正常路径下这两种情况 node_reported 已被 withdraw 打回假，这里是复核。 */
	if (midr_nds_is_bootstrap(bgp) || bgp->midr_nds_info->shutdown)
		return false;

	/* 键合法：他们拿 ctx->bgp->router_id.s_addr 逐位比。 */
	if (!f->node.node_id || f->node.node_id != bgp->router_id.s_addr)
		return false;

	return true;
}

static bool snapshot_link_eligible(struct bgp *bgp,
				   const struct midr_nds_fact_link *fl)
{
	struct prefix remote;

	/* 墓碑（撤过）与从没报过的占位条目都在这一关滤掉。 */
	if (!fl->reported)
		return false;

	/* 闸门③ 键合法（他们 midr_validate_link_update 的第一关）。 */
	if (!fl->data.key.local_node_id ||
	    fl->data.key.local_node_id != bgp->router_id.s_addr ||
	    !fl->data.key.remote_node_id ||
	    fl->data.key.remote_node_id == fl->data.key.local_node_id)
		return false;

	/* 闸门② 指标可用。看的是**事实表里存着的**上报值：热身期从来没报过，
	 * 走不到这儿；这一关兜的是事实被写坏的情况。 */
	if (!fl->data.metrics.rtt_us)
		return false;

	snapshot_prefix_from_rid(&remote, fl->data.key.remote_node_id);

	/* 保底边不报（引导侧同理由，见 midr_nds_report_link 的保险段）。 */
	if (midr_nds_link_is_backbone(bgp, &remote))
		return false;

	/* 闸门① 会话 Established —— 会话没了就不该继续声称这条链路在。 */
	if (!midr_node_established_peer(bgp, &remote))
		return false;

	return true;
}

/* ---------------------------------------------------------------------------
 * 第二层：字段级自检（字段填没填对）—— 镜像他们 midr_validate_*
 * （bgp_midr_input.c:213-283 @c344e40ba1）
 *
 * 与 shim 里那份同源，但**不能复用** —— shim 轮 4 整份删除，这份是长期件。
 * 改动前先比对他们的源文件。
 *
 * 与入选的分工：入选管"该不该报"（设计内的排除，正常且频繁）；本层管"字段
 * 合不合法"。能走到本层的条目都是报过、且此刻仍过得了闸门的，字段还坏就只能
 * 是我方 bug，**按构造不该发生** —— 所以它不是要优雅处理的数据情况，而是要
 * 大声报警的程序错误：调用方据此整份 -EAGAIN，不跳过（跳过等于把 bug 藏起来，
 * 还会按"缺席=已失效"把一条好链路悄悄注销掉）。
 * -------------------------------------------------------------------------*/

static bool snapshot_ipaddr_present(const struct ipaddr *address)
{
	return address->ipa_type == IPADDR_V4 || address->ipa_type == IPADDR_V6;
}

static bool snapshot_policy_valid(enum midr_policy_state state)
{
	return state == MIDR_POLICY_ALLOWED || state == MIDR_POLICY_BLOCKED;
}

static const char *snapshot_node_selfcheck(const struct midr_node_update *node)
{
	if (!snapshot_policy_valid(node->policy_state))
		return "policy_state 非法";
	if (node->has_transport_address) {
		if (!snapshot_ipaddr_present(&node->transport_address))
			return "transport 地址族缺失";
	} else if (node->transport_address.ipa_type != IPADDR_NONE) {
		return "has_transport_address 为假但地址非空";
	}

	return NULL;
}

static const char *snapshot_link_selfcheck(const struct midr_link_update *link)
{
	if (link->local_ifindex < 0)
		return "local_ifindex 为负";
	if (!snapshot_ipaddr_present(&link->link_local_address) ||
	    !snapshot_ipaddr_present(&link->link_remote_address))
		return "链路两端地址缺失";
	if (link->link_local_address.ipa_type !=
	    link->link_remote_address.ipa_type)
		return "链路两端地址族不一致";
	if (!link->metrics.has_rtt_us || !link->metrics.has_loss_ppm ||
	    !link->metrics.has_available_bandwidth_kbps)
		return "指标 has_* 未全置";
	if (!link->metrics.rtt_us)
		return "rtt_us 为 0";
	if (!link->metrics.available_bandwidth_kbps)
		return "available_bandwidth_kbps 为 0";
	/* 他们是 >= 1000000 即拒收（严格小于，比文档写的「≤1e6」更严）。 */
	if (link->metrics.loss_ppm >= 1000000)
		return "loss_ppm 达到或超过 1e6";
	if (!snapshot_policy_valid(link->policy_state))
		return "policy_state 非法";

	return NULL;
}

/* ---------------------------------------------------------------------------
 * 接口本体
 * -------------------------------------------------------------------------*/

static void snapshot_free_arrays(struct midr_topology_snapshot *snapshot)
{
	void *p;

	/* 数组声明成 const 是给消费方看的（他们只读），释放时按本来面目处理。 */
	p = (void *)snapshot->nodes;
	XFREE(MTYPE_MIDR_NDS_SNAPSHOT, p);
	p = (void *)snapshot->links;
	XFREE(MTYPE_MIDR_NDS_SNAPSHOT, p);

	memset(snapshot, 0, sizeof(*snapshot));
}

int midr_topology_snapshot_get(struct midr_context *ctx,
			       struct midr_topology_snapshot *snapshot)
{
	struct bgp *bgp = bgp_get_default();
	struct midr_nds_facts *f;
	struct midr_node_update *nodes = NULL;
	struct midr_link_update *links = NULL;
	struct listnode *node;
	struct midr_nds_fact_link *fl;
	const char *bad;
	size_t link_n = 0, i = 0;
	bool node_ok;

	(void)ctx; /* opaque handle，我方只透传、从不解引用 */

	if (!snapshot)
		return -EINVAL;
	memset(snapshot, 0, sizeof(*snapshot));

	/*
	 * -EAGAIN 的三种"还没准备好"。区别于引导/下线的**有意沉默**（那是
	 * "我确实什么都没有"，返回空快照 + 0）：未就绪时返回空快照会让他们按
	 * "缺席=已失效"把我方整个注销掉，再装一份空基线，语义完全失真。
	 */
	f = facts_of(bgp);
	if (!f)
		return -EAGAIN; /* 事实表未就绪：init 之前 / finish 之后 */
	if (!bgp->router_id.s_addr)
		return -EAGAIN; /* 身份未就绪：键都还没有 */

	/* 一趟数、一趟填 —— 中间不会有人改表（单线程事件循环）。 */
	node_ok = snapshot_node_eligible(bgp, f);
	for (ALL_LIST_ELEMENTS_RO(f->links, node, fl))
		if (snapshot_link_eligible(bgp, fl))
			link_n++;

	/*
	 * 数组只在 count 非 0 时分配 —— 他们对
	 * `(!!nodes) != (node_count != 0)` 直接 -EINVAL，空快照必须是
	 * "NULL + 0"而不是"非空指针 + 0"。
	 */
	if (node_ok) {
		nodes = XCALLOC(MTYPE_MIDR_NDS_SNAPSHOT, sizeof(*nodes));
		*nodes = f->node;

		bad = snapshot_node_selfcheck(nodes);
		if (bad) {
			zlog_warn("MIDR facts: snapshot 自检失败（node_id=%u：%s），整份拒发",
				  nodes->node_id, bad);
			XFREE(MTYPE_MIDR_NDS_SNAPSHOT, nodes);
			return -EAGAIN;
		}

		snapshot->nodes = nodes;
		snapshot->node_count = 1;
	}

	if (link_n) {
		links = XCALLOC(MTYPE_MIDR_NDS_SNAPSHOT,
				link_n * sizeof(*links));

		for (ALL_LIST_ELEMENTS_RO(f->links, node, fl)) {
			if (!snapshot_link_eligible(bgp, fl))
				continue;

			links[i] = fl->data;

			bad = snapshot_link_selfcheck(&links[i]);
			if (bad) {
				zlog_warn("MIDR facts: snapshot 自检失败（link %u->%u：%s），整份拒发",
					  links[i].key.local_node_id,
					  links[i].key.remote_node_id, bad);
				snapshot->links = links;
				snapshot->link_count = link_n;
				snapshot_free_arrays(snapshot);
				return -EAGAIN;
			}
			i++;
		}

		snapshot->links = links;
		snapshot->link_count = link_n;
	}

	/*
	 * version 原样拷出、**不递增** —— 对象 version 的语义是"上报序号"，快照
	 * 不是一次上报；递增会让快照内容和"version N 那次 upsert 说的内容"对不上。
	 * snapshot_version 是另一个东西（整份快照的编号，供他们调试与一致性检查），
	 * 每成功产出一份递增一次。
	 */
	snapshot->snapshot_version = ++f->snapshot_version;

	MIDR_LOG("MIDR facts: snapshot 产出 node=%zu link=%zu（表内 %u 条）snapshot_version=%" PRIu64,
		 snapshot->node_count, snapshot->link_count,
		 f->links ? f->links->count : 0, snapshot->snapshot_version);
	return 0;
}

void midr_topology_snapshot_release(struct midr_context *ctx,
				    struct midr_topology_snapshot *snapshot)
{
	(void)ctx;

	/* 他们的失败路径也会拿全零结构调进来（见他们 midr_input_resync_step），
	 * 所以这里必须容忍 nodes/links 为 NULL。 */
	if (!snapshot)
		return;

	snapshot_free_arrays(snapshot);
}
