// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR 本地事实表 —— 对接第二组 7-24 topology 接口（轮 0 备料）
 *
 * 第二组 7-24 版接口把第一组定为「事实提供方」：本机的 Node / Link 事实经
 * midr_topology_node_upsert() / midr_topology_link_upsert() 上报，NLRI 的
 * originate / 解析 / 传播 / 版本管理全归第二组。本文件就是那份「事实」在我方
 * 的权威副本：
 *
 *   - node 恒 1 个 —— 我们只拥有自己（计划 Q18；第二组 snapshot 消费侧硬编码
 *     node_count > 1 即 -EINVAL，见他们 bgp_midr_input.c:459）；
 *   - link 表只装 local_node_id == 自己 的**出向**链路（ownership 规则 Q2：
 *     有向链路 R1→R2 归 R1 所有，只有 R1 上报 / 撤销它）；
 *   - 每对象一个进程内单调递增 uint64 version（Q13），存在对象自己的
 *     .version 字段里，各自独立递增。
 *
 * ⚠ **version 的语义是「上报序号」，不是「事实修订号」**：它在真正发出 upsert /
 * withdraw 的那一刻递增（由 midr_nds_report_node 等上报层做），而不是在事实内容
 * 变化时。硬依据是第二组的 apply 阶段——node/link 的 upsert 与 withdraw 一律
 * `version <= 已存版本` 即丢弃（他们 bgp_midr_input.c:293 / :316 / :344 / :367
 * @c344e40ba1），且**不报错**（计进 ignored_old，API 照样返回 0）。按事实修订号
 * 走会让两条路静默失效：`midr shutdown` 的撤销与上次 upsert 同版；
 * `no midr shutdown` 的重报事实没变、又是同版。
 * 这道闸门在 apply 阶段、不在 validate 阶段，**shim 里镜像的校验照不出来**。
 *
 * 它同时是 (a) 之后一切 upsert 的源头，(b) 轮 3 midr_topology_snapshot_get()
 * 的底料。轮 0 只建表 + 换算 helper；写入方在轮 1（node）/ 轮 2（link）接上。
 *
 * 表项直接用第二组的 struct midr_node_update / midr_link_update 存，省掉一层
 * 转换：upsert 时整体递交，snapshot 时整体拷出。
 *
 * 本文件是我方长期件（轮 1–3 那个假实现 bgp_midr_group2_shim.c 已于轮 5 删除，
 * 两者无关）。
 */

#ifndef _FRR_BGP_MIDR_NDS_FACTS_H
#define _FRR_BGP_MIDR_NDS_FACTS_H

#include <zebra.h>

#include "bgpd/bgpd.h"
#include "bgpd/bgp_midr.h" /* 第二组接口原型（原名拷入，以他们为准） */
#include "bgpd/bgp_midr_nds.h"

/*
 * 出向链路事实表的一个表项。data.key.local_node_id 恒为本机 router-id
 * （s_addr 原值，网络字节序 —— 第二组校验拿 ctx->bgp->router_id.s_addr 逐位比，
 * 不可转主机序）。
 */
struct midr_nds_fact_link {
	struct midr_link_update data;

	/* 已经成功 upsert 过一次。轮 2 用它区分「首次上报」与「更新」，并在链路
	 * 死掉时决定要不要发 withdraw（没报过就没什么可撤）。 */
	bool reported;

	/*
	 * 「想报但被第二组拒了」（upsert 返回非 0）。两个用途：① 快照入选判据
	 * = reported || pending；② 失败日志降噪的"上次也失败过"记号。成功即清。
	 *
	 * ⚠ 必须与 reported 分开：reported 兼着墓碑语义（撤过还没恢复的 link——
	 * 会话仍 Established、旧 rtt 仍在，三道闸门全过，只有它认得出）和幂等守卫，
	 * 失败时硬置 reported 会让幂等守卫把该重报的那次吃掉。
	 * ⚠ **withdraw 失败不置 pending**：那时要的正是"别出现在快照里"，快照缺席
	 * 恰好等于撤销；置了反而把它拉回快照、抵消撤销。
	 */
	bool pending;
};

/* 本地事实表，挂 bgp->midr_nds_info->facts。 */
struct midr_nds_facts {
	/* 恒 1 个：我们自己。node_valid 为假时（router-id 尚未就绪）不得上报 ——
	 * 第二组对 local_node_id == 0 返回 -ENOENT。 */
	struct midr_node_update node;
	bool node_valid;

	/* 已成功 upsert 过一次；node_withdraw 成功后复位。与
	 * midr_nds_fact_link.reported 同构，兜的是「事实没变但必须重报」——
	 * `no midr shutdown` 时身份与下线前一模一样，只看 node_refresh 的返回值
	 * 会漏报。 */
	bool node_reported;

	/* node 侧的「想报但被拒」，语义与 midr_nds_fact_link.pending 完全一致
	 * （node 恒 1 个，故只需一个位）。 */
	bool node_pending;

	/* struct midr_nds_fact_link *，出向链路 */
	struct list *links;

	/* 轮 3：每次 snapshot_get 递增，填 struct midr_topology_snapshot
	 * 的 snapshot_version。与各对象自己的 version 是两个东西。 */
	uint64_t snapshot_version;
};

/* ===========================================================================
 * 生命周期（由 bgp_midr_nds_init / _finish 调用）
 * =========================================================================*/

extern void midr_nds_facts_init(struct bgp *bgp);
extern void midr_nds_facts_finish(struct bgp *bgp);

/* ===========================================================================
 * node 事实（轮 1 接上报）
 * =========================================================================*/

/*
 * 按当前本地身份（router-id / group-id / capabilities / transport 地址）重建
 * node 事实。有实质变化返回 true、无变化返回 false（幂等，可随便多调）。
 *
 * **不碰 version** —— 递增归上报层（见文件头「version 的语义是上报序号」）。
 * 一般不单独调它：走 midr_nds_report_node()，那里 refresh 与上报是一体的。
 */
extern bool midr_nds_facts_node_refresh(struct bgp *bgp);

/* ===========================================================================
 * node 上报（轮 1）
 * =========================================================================*/

/*
 * 第二组接口的 context 句柄。bgp_midr_nds_init() 已取过一次存进
 * mi->g2_ctx，本函数只是兜底重取（真实现里 context 可能晚于我方 init 才就绪；
 * shim 的哨兵恒非 NULL）。返回 NULL = 接口尚不可用，调用方跳过本次上报。
 */
extern struct midr_context *midr_nds_group2_ctx(struct bgp *bgp);

/*
 * 身份变化点的统一出口：刷新 node 事实 → midr_topology_node_upsert() /
 * _withdraw()。件②（轮 4）删掉自有 BGP-LS 自通告出口后，这是本机身份对外的
 * **唯一**一条路。
 *
 * 调用点（轮 1 已收编全部 5 处）：midr_nds_set_capability /
 * midr_originate_group_update（即 midr_group_reconverge 第 1 步，手动换组与 I-7
 * JOIN 共用）/ `midr transport-address` 设置与清除 / `midr shutdown` 与
 * `no midr shutdown`。
 *
 * reason 只驱动日志：除 LEAVE -> withdraw 外一律 upsert。四条不显然的规则：
 *   - 幂等：事实无实质变化且已报过 -> 不重报（Q8 频率控制天然满足）；
 *   - `no midr shutdown`：事实与下线前相同、refresh 返回 false，靠
 *     node_reported 已被 withdraw 复位才得以重报；
 *   - 优雅下线期间的身份变化：事实照更（version 不动），但不上报——守卫在本
 *     函数里，是出站方向仅剩的两道之一（另一道在 midr_nds_report_link()）；
 *   - **刷本机 self 条目（midr_nds_local_node_update）挂在本函数最前面**，在所有
 *     抑制 return 之前：它刷的是本地视图，与"这笔要不要报给第二组"无关。
 */
extern void midr_nds_report_node(struct bgp *bgp,
				 enum midr_origin_reason reason);

/* ===========================================================================
 * link 事实（轮 2 接上报）
 * =========================================================================*/

extern struct midr_nds_fact_link *
midr_nds_facts_link_find(struct bgp *bgp, uint32_t remote_node_id,
			 uint64_t link_id);

/* 找不到就建一条（version 初值为 0，首次实际上报前递增为 1）。 */
extern struct midr_nds_fact_link *
midr_nds_facts_link_get(struct bgp *bgp, uint32_t remote_node_id,
			uint64_t link_id);

/*
 * ⚠ 当前**无调用者**：撤销路径改为留墓碑（只置 reported=false，保住 version），
 * 条目只在 midr_nds_facts_finish() 统一释放 —— 理由见 midr_nds_report_link_withdraw()
 * 里的注释。保留本函数供将来真需要淘汰单条目时用。
 */
extern void midr_nds_facts_link_del(struct bgp *bgp,
				    struct midr_nds_fact_link *fl);

/*
 * Q15 的单一出口：link_id 分配。
 *
 * 目前**恒返回 0** —— overlay 链路按对端 router-id 建（PM 目标表一对一），每对
 * 节点恒一条逻辑链路，第二组文档明确允许固定填 0，且 0 是常量、自动满足「重启
 * 前后不变」的稳定性要求（他们按 (local, remote, link_id) 三元组识别链路，ID
 * 漂了会被当成两条：旧的成僵尸、新的从零攒历史）。
 *
 * 已核实他们的 midr_validate_link_update() 对 link_id 无任何约束
 * （bgp_midr_input.c:251），0 合法。
 *
 * 将来真出现平行链路（同一对节点两条链路）才需要真正的分配器 —— 那时只改这一处。
 */
extern uint64_t midr_nds_facts_link_id(struct bgp *bgp,
				       uint32_t remote_node_id);

/* ===========================================================================
 * 单位换算（计划 Q16+Q17：「本段唯一的真工作量」）
 * =========================================================================*/

/*
 * bw 占位值（kbps）。
 *
 * ⚠ 这不是测量值。我方 PM 只有 bw_score —— 由 rtt/loss 派生的无量纲合成分数
 * （bgp_midr_pm.h：loss 开方做分母），**没有任何 kbps 来源**。而第二组的
 * midr_validate_link_update() 要求 has_available_bandwidth_kbps 为真
 * **且** available_bandwidth_kbps != 0，否则 -EINVAL（bgp_midr_input.c:262-267）
 * —— 也就是说 bw 不是「可以先不填」，不填 link_upsert 直接失败。
 *
 * 所以过渡期填这个占位常量让上报链路能跑通，并在首次使用时 zlog_warn 一次，
 * 防止它静默混进联调。口径落定后删掉本常量、改 midr_nds_metric_bw_kbps()。
 *
 * TODO（问题清单 #7）：等第二组答复 —— ① cost 公式对带宽精度要求多高，
 * 「标称带宽 × (1−loss)」这类粗估行不行；② 过渡期允不允许先填配置的标称值。
 * 若答案是「必须真实测量」，则改 PM 补真带宽估计（归 PM owner 队友）。
 */
#define MIDR_NDS_BW_KBPS_PLACEHOLDER 1000000U /* 1 Gbps，纯占位 */

/* rtt：我方内部就是微秒（st_rtt_us / lt_rtt_us），直接平移。 */
extern uint32_t midr_nds_metric_rtt_us(const struct midr_nds_link_metrics *m);

/*
 * loss：我方是 0.0~1.0 的比例，×1e6 即 ppm。
 * 注意上限 —— 第二组是 loss_ppm >= 1000000 即 -EINVAL（**严格小于**，不是文档
 * 里写的「≤1e6」），故本函数把结果夹到 [0, 999999]。
 *
 * 满格丢包由 midr_nds_report_link() 在调用本换算函数前转为 withdraw；本 helper
 * 仍把任意输入夹进第二组接受的数值范围，不能单独承担链路生死语义。
 */
extern uint32_t midr_nds_metric_loss_ppm(const struct midr_nds_link_metrics *m);

/* bw：见 MIDR_NDS_BW_KBPS_PLACEHOLDER。首次调用会 warn 一次。 */
extern uint32_t midr_nds_metric_bw_kbps(const struct midr_nds_link_metrics *m);

/*
 * 把我方一组链路指标换成第二组的 struct midr_link_metrics。
 *
 * 返回 false = **这条链路此刻没有可上报的指标**，调用方跳过本次（不是撤销）：
 *   - rtt_us == 0：还没探到（60s 热身期 / 目标不可达）。对上「接口不定义
 *     link_state，探测中/热身中不提交」的约定。
 * 返回 true 时 *out 已填满，可直接塞进 struct midr_link_update.metrics。
 *
 * 该换算函数本身对 loss_rate >= 1.0 仍返回 true 并夹到 999999；生产出口
 * midr_nds_report_link() 会先依照第二组的 link-unavailable 语义执行 withdraw，
 * 因而调用者不得绕过该出口直接用本 helper 决定链路生死。
 */
extern bool midr_nds_metrics_to_group2(const struct midr_nds_link_metrics *m,
				       uint64_t seqno,
				       struct midr_link_metrics *out);

/* ===========================================================================
 * link 上报（轮 2）
 * =========================================================================*/

/*
 * link 上报出口：更新事实表 → midr_topology_link_upsert()。node 侧
 * midr_nds_report_node() 的同层同构物，**取代 I-5 去抖放行后原先直调的 E-1**
 * （件② 已删掉那条 E-1 出口，本函数是链路指标对外的唯一一条路）。
 *
 * 唯一调用点 = midr_nds_on_link_update() 里去抖放行处。上层（探测 / I-5 /
 * 去抖）对本函数一无所知：该不该报的判断全在这里。
 *
 * 三道闸门（放行才报，判据与理由见函数体）：
 *   ① 会话 Established —— 会话没建就报链路等于虚报；
 *   ② 指标可用 —— 热身期 rtt=0 不报（midr_nds_metrics_to_group2 判）；
 *   ③ 键合法 —— 本机 router-id 非 0、对端 rid 非 0 且与本机不同
 *      （他们 midr_validate_link_update 的第一关，不满足必 -ENOENT/-EINVAL）。
 * 外加一道**保险**：引导 / 保底边不上报（midr_nds_link_is_backbone），详见
 * 该函数头注释。
 */
extern void midr_nds_report_link(struct bgp *bgp,
				 const struct midr_link_entry *link);

/*
 * link 撤销出口：报过才撤，撤完保留事实表墓碑以延续 version。调用点 = 统一收口原语
 * midr_nds_detach_node()（换群拆边 / 节点下线 / 运维拆边都经它）。
 *
 * 这是「链路生死归会话/节点级」的清理路径那一半（另一半是轮 4/5 要挂的
 * peer_status_changed 钩子）。没报过（reported 为假）就没什么可撤。
 *
 * ⚠ 历史注记：轮 2/3 的 shim 期它只打日志不发真撤销（旧 E-1 路径本来就没有
 * 「撤 Link NLRI」这个动作，接上去反而改变行为）。件② 起 shim 不参与编译，
 * 撤销直接走第二组的 link_withdraw。
 */
extern void midr_nds_report_link_withdraw(struct bgp *bgp,
					  const struct prefix *remote_node_id);
/* Withdraw every previously reported link while retaining version tombstones. */
extern void midr_nds_facts_withdraw_all_links(struct bgp *bgp);

/* ===========================================================================
 * 钩子回调（轮 4：上报资格守卫的配套，注册在 bgp_midr_nds_init）
 * =========================================================================*/

/*
 * 配置读完补一笔 node 上报（挂 bgp_config_end），与 report_node 里那道「配置期
 * 抑制」守卫成对：conf 逐行执行时身份还在拼装（`midr group-id` 可能排在
 * `midr transport-address` 之前），报出去是半成品。同款先例 = 挂靠推迟。
 *
 * ⚠ 注册顺序必须排在 midr_nds_bootstrap_after_cfg **之后**：引导专职化收尾会清
 * 群代表位/群号，先清完再报，才不会报一份马上作废的身份。
 */
extern int midr_nds_facts_report_after_cfg(struct bgp *bgp);

/*
 * router-id 就绪后重放一笔（挂 bgp_routerid_update）——守卫要求 rid 非 0，之前的
 * 都被挡下了。withdraw 方向不特殊处理：report_node 走 !node_valid 分支自然发撤销。
 *
 * 与第二组同名钩子并存无碍：他们注册更早、回调只排一个 event 就返回，我方这笔
 * upsert 落进他们的 resync 队列不丢，回放时计一次 ignored_old（内容一致）。
 */
extern int midr_nds_facts_report_after_rid(struct bgp *bgp, bool withdraw);

/* ===========================================================================
 * version 上限守卫（轮 4）
 *
 * 协议 §4.5：version 接近 UINT64_MAX 时必须停止递增、触发完整 resync，在新基线上
 * 恢复。这是协议**唯一**点名要我方调 resync_begin 的场景——另一类"第一组重启"在
 * 合栈后不成立（同进程同生共死，重启时对方 fact table 也是空的）；队列满/内存不足
 * 归第二组自己管，我方按 §4.4 只重试。
 *
 * ⚠ **只有我方发起的 resync 才重置 version**。对方自己发起的（他们不通知我方）绝不
 * 能跟着归零：他们按快照里带出的 version 原值建新表，我方继续递增恒大于它；归零反而
 * 小于表内值、被当旧事件吞掉。`midr shutdown` → `no midr shutdown` 同理（进程没重启、
 * 对方墓碑还记着撤销时的号）。分界线：**看对方的记忆死没死**。
 * =========================================================================*/
#define MIDR_NDS_VERSION_HIGH_WATER (UINT64_MAX - 1024)

#endif /* _FRR_BGP_MIDR_NDS_FACTS_H */
