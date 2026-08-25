// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR VTY commands
 *
 * Provides CLI for MIDR configuration and diagnostics:
 *   midr group-id <N>
 *   midr session <IP> remote-as <ASN>   （旧名 midr neighbor，隐藏别名过渡）
 *   no midr session <IP>                （旧名 no midr neighbor，同上）
 *   midr help [plain]
 *   show midr self    （本机自身状态，不经节点表）
 *   show midr nodes
 */

#include "zebra.h"

#include "command.h"
#include "vty.h"
#include "log.h"
#include "sockunion.h"
#include "prefix.h"
#include "linklist.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_ls.h"
#include "bgpd/bgp_vty.h" /* bgp_config_inprocess()：引导角色收尾的时机判断 */
#include "bgpd/bgp_midr_nds.h"
#include "bgpd/bgp_midr_nds_facts.h" /* midr_nds_report_node（轮 1 node 上报线） */
#include "bgpd/bgp_midr_ctrl.h"
#include "bgpd/bgp_midr_pm.h"
#include "bgpd/bgp_midr_nds_vty.h"
#include "bgpd/bgp_midr_store.h" /* show midr bootstrap-seeds：现查现出种子表 */
#include "bgpd/bgp_debug.h"
#include "bgpd/bgp_nexthop.h"
#include "bgpd/bgp_table.h"
#include "monotime.h"

/*
 * 节点表里是否已学到任何远端节点。用于抑制"启动加载配置文件"阶段的误报：
 * 那时 BGP 会话尚未建立、节点表必然为空，于是"目标群无已知成员"恒成立，
 * 每个节点启动时都会无差别打出"将成为该群首个成员"（07-21 十节点实验现象）。
 * 视图为空时我们其实无从判断该群有没有成员，沉默比给一个必然为真的断言好。
 */
static bool midr_view_has_remote_nodes(struct bgp *bgp)
{
	struct midr_node_entry *entry;

	frr_each (midr_node_hash, &bgp->midr_nds_info->global_view->nodes, entry)
		if (!entry->is_self)
			return true;

	return false;
}

/* ------------------------------------------------------------------ */
/* midr group-id <N>                                                   */
/* ------------------------------------------------------------------ */

DEFUN(midr_group_id,
      midr_group_id_cmd,
      "midr group-id (0-4294967295)",
      "MIDR configuration\n"
      "Set local MIDR group ID\n"
      "Group ID value\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);

	if (!bgp->midr_nds_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	uint32_t gid = (uint32_t)atol(argv[2]->arg);
	uint32_t old_gid = bgp->midr_nds_info->local_group_id;

	/*
	 * 硬互斥（08-13）：引导节点群号恒 0、不参与任何群，本命令一律拒绝。
	 * 一律拒（含 `midr group-id 0`）而不是只拒非 0：引导上出现这条命令本身
	 * 就是配置错误，静默接受一个"恰好无害"的值只会让人以为它生效了。
	 * 反向顺序（本命令在 `midr role bootstrap` 之前）由 config_end 兜底清理。
	 */
	if (bgp->midr_nds_info->local_capabilities & MIDR_CAP_BOOTSTRAP) {
		vty_out(vty,
			"%% 本节点是 MIDR 引导节点，群号恒为 0、不得配置群号——如需改角色，请下线后修改配置并重启\n");
		return CMD_WARNING_CONFIG_FAILED;
	}

	/* 群 0 = 「未入网」这一个语义（上报资格守卫拿它当入网判据），不接受当配置值
	 * 写进来。不做 no form —— "清配置"不留命令语义；内部 config_group_id = 0 的
	 * 路径不经本命令，不受影响。 */
	if (gid == 0) {
		vty_out(vty,
			"%% 群号 0 保留给「未入网」状态，不得配置——如需退网请用 `midr shutdown`\n");
		return CMD_WARNING_CONFIG_FAILED;
	}

	midr_nds_set_group_id(bgp, gid);

	vty_out(vty, "MIDR group-id set to %u\n", gid);

	/*
	 * 运维反馈：真正切了群且目标群当前无已知成员时，提示本节点将成为该群
	 * 首个成员（强制切换语义下不阻断——群号是标签非注册制实体，允许开新群）。
	 */
	if (gid != old_gid && gid != 0 && midr_view_has_remote_nodes(bgp)) {
		struct list *members = list_new();

		midr_group_members(bgp, gid, members);
		if (list_isempty(members))
			vty_out(vty,
				"%% 群 %u 当前无已知成员，本节点将成为该群首个成员\n",
				gid);
		list_delete(&members);
	}

	return CMD_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* midr session <IP> remote-as <ASN>（Q5 改名，原 midr neighbor：       */
/* "neighbor"一词让人误当配邻接/加群命令，实际它只造会话不碰账）        */
/* ------------------------------------------------------------------ */

/*
 * 运维规范的命令层提醒（2026-08-11 半边残留评审定案，执行计划答疑 93）：
 * `midr session` / `no midr session` **一律双端对称执行**。
 *
 * 为什么要在回显里喊这一嗓子：这两条命令都只动本机那半边。单边配 → 对端不知
 * 情、不回配，本机半边永远停在 Active；单边拆 → 对端那半边成了没人管的僵尸
 * （它既不会重连也不会自己消失）。这类"单侧残留"没有便宜的自动解法，评审定案
 * 是**先用运维规范治源头**，代码侧只做兜底（B2 提前死心治"对方活着且拒我"、
 * B1 断连老化治"对方活着但单侧退出"，后者归批 5）。
 */
#define MIDR_SESSION_SYMMETRY_HINT                                             \
	"%% 提醒：本命令只改本机这一侧。请在对端对称执行，否则会留下单侧半边会话\n"

/*
 * 把 `[no] midr session <IP>` 的入参地址解析成 router-id。
 *
 * 排除名单以 router-id 为键（08-11 批 2，Q2：排除记的是"这个人别再连"，改
 * transport 不该让拉黑失效），而这三条命令的入参是地址，故需要这层翻译。两个
 * 来源，按可靠性排序：
 *   ① `peer->remote_id` —— 对端 OPEN 报文里报的真名，会话起来过就一定准；
 *   ② 节点表按 transport 反查 —— 会话没起来时的回落（对端 NLRI 仍可能经别的
 *      路径泛洪到本机）。
 * 都拿不到返回 0，调用方须告警并跳过排除动作（宁可不落表，也不拿地址当 rid
 * 混进以 router-id 为键的表里——那正是换键要根治的病）。
 */
static struct in_addr midr_vty_rid_for_session_addr(struct bgp *bgp,
						    struct in_addr addr,
						    struct peer *peer)
{
	struct in_addr rid;

	if (peer && peer->remote_id.s_addr != INADDR_ANY)
		return peer->remote_id;
	rid = midr_nds_rid_by_transport(bgp, addr);
	if (rid.s_addr != INADDR_ANY)
		return rid;
	/*
	 * 第三本字典：bootstrap 候选池（批 5 R 系列）。
	 * 前两本对**引导节点**不保证翻得到——节点表要等它的 Node NLRI 传过来（刚起
	 * 或链路不通时就没有），而挂靠没建成的半边（对方死了/拒了、会话停在 Active）
	 * 也没有 remote_id。于是"拆掉这条挂靠边顺便把这台引导拉黑"就落空（报"未能
	 * 持久排除"）。候选池里每条都带运维手配的 rid，正好补上这一级。
	 */
	return midr_nds_bootstrap_rid_by_transport(bgp, addr);
}

DEFUN(midr_session,
      midr_session_cmd,
      "midr session A.B.C.D remote-as (1-4294967295)",
      "MIDR configuration\n"
      "Manually build a MIDR overlay session (escape hatch/debug; normal join uses midr group-id / midr bootstrap)\n"
      "Session peer IP address\n"
      "Remote AS\n"
      "AS number\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);
	union sockunion su;
	as_t asn = (as_t)atol(argv[4]->arg);
	struct peer *peer;
	int ret;

	if (!bgp->midr_nds_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	if (str2sockunion(argv[2]->arg, &su) < 0) {
		vty_out(vty, "%% Invalid IP address: %s\n", argv[2]->arg);
		return CMD_WARNING_CONFIG_FAILED;
	}

	/*
	 * S3 撞车守卫：必须拦在 peer_remote_as 之前——它对已存在 peer 会直接复用，
	 * ASN 不同还静默改 AS（bgpd.c peer_as_change），随后整形把活的 underlay
	 * 会话就地改造成 overlay（撤 IPv4、multihop），砸掉转发面（洞 #3）。
	 */
	peer = peer_lookup(bgp, &su);
	if (peer) {
		if (midr_nds_peer_is_overlay(peer)) {
			struct in_addr rid = midr_vty_rid_for_session_addr(
				bgp, su.sin.sin_addr, peer);

			/* 已是 MIDR 自建会话：幂等重整形（同值短路、不 reset），不改 AS。 */
			midr_nds_ctrl_setup_overlay_peer(bgp, peer);
			/* 本路径命令【成功】，同样是运维显式意图 → 一并解除排除
			 * （见下方 peer_remote_as 前那段注释）。排除名单以 rid 为键。 */
			if (rid.s_addr != INADDR_ANY)
				midr_nds_session_exclude_del(bgp, rid);
			/* 台账家规②：运维点名 → 记 MANUAL（粘性，之后自动流程
			 * 不改写它的原因）。会话已存在也照记——记账不看去重。 */
			midr_nds_ledger_note(bgp, su.sin.sin_addr,
					     MIDR_SESSION_MANUAL, rid, 0);
			vty_out(vty,
				"MIDR session %s 已存在（MIDR overlay 会话），已确保形态一致\n",
				argv[2]->arg);
			vty_out(vty, "%s", MIDR_SESSION_SYMMETRY_HINT);
			return CMD_SUCCESS;
		}
		/* 运维会话占用该地址：拒绝接管（运维优先，绝不动它）。
		 * ⚠ 件②（轮 4）迁 MIDR-LS 后**不再复用运维会话**（真分离约定），
		 * 所以两种情形都只能改地址，区别仅在提示里点明现状。 */
		if (peer->afc[AFI_BGP_LS][SAFI_MIDR_LS])
			vty_out(vty,
				"%% %s 已被运维会话占用（该会话已激活 MIDR-LS）；MIDR 不复用运维会话，请改用该节点其它地址\n",
				argv[2]->arg);
		else
			vty_out(vty,
				"%% %s 已被运维会话占用；拒绝接管（避免砸转发面）。请改用该节点其它地址\n",
				argv[2]->arg);
		return CMD_WARNING_CONFIG_FAILED;
	}

	/*
	 * 手工敲这条命令是运维显式意图，必须能覆盖此前的 `no midr session`
	 * 排除——否则下一次自动重收敛（connect_group）又会被排除名单挡住，
	 * 运维刚建好的会话反而在下一次换组/退群时消失，从"手工建连"退化成
	 * "手工建一次性连接"。
	 *
	 * 【我方改动，需知会 zhc】此句原在 S3 撞车守卫【之前】：守卫一拒（命令
	 * 失败），排除却已经解除了——运维以为自己那条 `no midr session` 还生效，
	 * 实际该节点已能被自动流程连回来（副作用先于校验）。挪到守卫之后，只在
	 * 命令真要往下走时才解除；守卫里"已是 overlay 会话"那条【成功】路径另
	 * 补了一次。
	 *
	 * 排除名单以 router-id 为键（08-11 批 2）：这条路径上 peer 尚未建立
	 * （下面才 peer_remote_as），只能靠节点表反查。查不到就解除不了——告警
	 * 说清楚，别让运维以为拉黑已经撤了。
	 */
	{
		struct in_addr rid = midr_vty_rid_for_session_addr(
			bgp, su.sin.sin_addr, NULL);

		if (rid.s_addr != INADDR_ANY)
			midr_nds_session_exclude_del(bgp, rid);
		else if (midr_nds_session_blacklist_nonempty(bgp))
			vty_out(vty,
				"%% 注意：查不到 %s 的 router-id（节点表里没有它），无法解除可能存在的排除记录；会话照建，但自动重收敛仍可能被排除名单挡住\n",
				argv[2]->arg);
	}

	ret = peer_remote_as(bgp, &su, NULL, &asn, AS_EXTERNAL, NULL);
	if (ret != 0) {
		vty_out(vty, "%% Failed to add neighbor %s (err %d)\n",
			argv[2]->arg, ret);
		return CMD_WARNING_CONFIG_FAILED;
	}

	/* 与自动建连（midr_ctrl_connect）共用同一整形：multihop + update-source
	 * + 只载 BGP-LS（撤 FRR 自动附送的 IPv4 单播——overlay 不得向 underlay
	 * 注入转发路由）。定义与理由见 midr_nds_ctrl_setup_overlay_peer。 */
	peer = peer_lookup(bgp, &su);
	if (peer)
		midr_nds_ctrl_setup_overlay_peer(bgp, peer);

	/* 台账家规②：运维点名建的边记 MANUAL（粘性）。本轮骨干互连也走这条
	 * 命令（Q7 定：骨干纯手工），故引导之间的边同样落在 MANUAL 名下。 */
	midr_nds_ledger_note(bgp, su.sin.sin_addr, MIDR_SESSION_MANUAL,
			     midr_nds_rid_by_transport(bgp, su.sin.sin_addr), 0);

	vty_out(vty, "MIDR session %s AS %u created\n", argv[2]->arg, asn);
	vty_out(vty, "%s", MIDR_SESSION_SYMMETRY_HINT);
	return CMD_SUCCESS;
}

/* 旧名弃用别名（Q5 过渡期保留：不出现在 ? 补全，但仍可执行；过渡期后删）。 */
ALIAS_DEPRECATED(midr_session, midr_neighbor_cmd,
      "midr neighbor A.B.C.D remote-as (1-4294967295)",
      "MIDR configuration\n"
      "Deprecated alias of `midr session` (kept for transition)\n"
      "Session peer IP address\n"
      "Remote AS\n"
      "AS number\n")

/* ------------------------------------------------------------------ */
/* no midr session <IP>（Q5 改名，原 no midr neighbor）                 */
/* ------------------------------------------------------------------ */

DEFUN(no_midr_session,
      no_midr_session_cmd,
      "no midr session A.B.C.D",
      NO_STR
      "MIDR configuration\n"
      "Tear down a MIDR overlay session (guarded: operator sessions refused)\n"
      "Session peer IP address\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);
	union sockunion su;
	struct peer *peer;
	struct in_addr excl_rid;

	if (str2sockunion(argv[3]->arg, &su) < 0) {
		vty_out(vty, "%% Invalid IP address: %s\n", argv[3]->arg);
		return CMD_WARNING_CONFIG_FAILED;
	}

	peer = peer_lookup(bgp, &su);
	if (!peer) {
		vty_out(vty, "%% Neighbor %s not found\n", argv[3]->arg);
		return CMD_WARNING;
	}

	/*
	 * S2 归属守卫：只拆 MIDR 自建的 overlay 会话，绝不误删运维 underlay 会话
	 * （删了会砸转发面）。判据 = OVERLAY 标记（运维原生会话永远没有它）。
	 */
	if (!midr_nds_peer_is_overlay(peer)) {
		vty_out(vty,
			"%% %s 是运维配置的会话（非 MIDR overlay），拒绝删除；如确需删除请用原生命令 no neighbor %s\n",
			argv[3]->arg, argv[3]->arg);
		return CMD_WARNING_CONFIG_FAILED;
	}

	/*
	 * 排除名单以 router-id 为键（08-11 批 2），而本命令的入参是地址——**必须
	 * 在拆会话之前**把 rid 解析出来：peer 一删，peer->remote_id 这个最可靠的
	 * 来源就没了。
	 */
	excl_rid = midr_vty_rid_for_session_addr(bgp, su.sin.sin_addr, peer);

	/*
	 * S6 清账：按地址反查节点表条目，查到走 detach 全套（停探 + 删 link +
	 * 清 is_adjacent + 拆会话）；查不到（该地址不对应已知节点）退化为只拆会话。
	 */
	if (!midr_nds_detach_by_locator(bgp, su.sin.sin_addr))
		peer_delete(peer);

	/* 拆口销账：detach 那条路已经过 midr_try_disconnect 销过账，这里补的是
	 * 上面 peer_delete 退化分支（该地址不对应已知节点，detach 走不到）。
	 * drop 幂等，重复调用无副作用。 */
	midr_nds_ledger_drop(bgp, su.sin.sin_addr);

	/*
	 * 持久排除而非临时拔线——运维显式敲这条命令表达"不想再跟这个节点做
	 * 邻居"，不该被下一次自动重收敛（换组/退群时的 connect_group）悄悄
	 * 连回来。写名单在 detach 之后，不影响本次清账本身。
	 * 键 = 上面预先解析好的 router-id（08-11 批 2）。
	 */
	if (excl_rid.s_addr != INADDR_ANY) {
		midr_nds_session_exclude_add(bgp, excl_rid);
		vty_out(vty,
			"MIDR session %s removed and persistently excluded (rid %pI4; use `midr session %s remote-as <ASN>` to reconnect)\n",
			argv[3]->arg, &excl_rid, argv[3]->arg);
	} else {
		/* 解析不出真名 = 没法记进以 router-id 为键的名单。会话照拆，但
		 * 必须说清楚"这次没拉黑"——否则运维以为拉黑了，自动重收敛却把它
		 * 连回来。宁可不落表，也不拿地址冒充 rid 混进表里。 */
		vty_out(vty,
			"MIDR session %s removed\n", argv[3]->arg);
		vty_out(vty,
			"%% 未能持久排除：查不到该地址对应的 router-id（会话未曾 Established 且节点表中无此节点）。自动重收敛可能把它连回来；待学到该节点后重敲本命令即可拉黑\n");
	}
	vty_out(vty, "%s", MIDR_SESSION_SYMMETRY_HINT);
	return CMD_SUCCESS;
}

/* 旧名弃用别名（Q5 过渡期保留；过渡期后删）。 */
ALIAS_DEPRECATED(no_midr_session, no_midr_neighbor_cmd,
      "no midr neighbor A.B.C.D",
      NO_STR
      "MIDR configuration\n"
      "Deprecated alias of `no midr session` (kept for transition)\n"
      "Session peer IP address\n")

/* ------------------------------------------------------------------ */
/* midr role bootstrap / no midr role bootstrap                        */
/* ------------------------------------------------------------------ */

DEFUN(midr_role_bootstrap,
      midr_role_bootstrap_cmd,
      "midr role bootstrap",
      "MIDR configuration\n"
      "Set the local MIDR role\n"
      "Act as a bootstrap node\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);
	struct bgp_midr_nds *mi;

	if (!bgp->midr_nds_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}
	mi = bgp->midr_nds_info;

	/*
	 * 冲突配置的裁定口径 = **引导优先**：本命令**永不拒绝**。
	 *
	 * 为什么不像 group-rep/group-id 那样反向拒绝（08-13 实测踩过）：那样一来
	 * conf 里三行的最终结果就由**行序**决定——`group-id`+`group-rep` 写在前面
	 * 时，本命令被拒，节点反而成了群代表，与运维写 `midr role bootstrap` 的本意
	 * 完全相反。运维写了这一行就是要它当引导，那就让它当，冲突项由收尾清掉。
	 *
	 * 清理时机分两种：配置期（frr.conf 加载中）交 bgp_config_end 统一收尾——
	 * 此刻后面还可能有 `midr group-id` 等着执行，当场清了也会被重新配上；
	 * 运行期则当场 enforce。两条路都汇到 midr_nds_bootstrap_enforce()。
	 */
	if (mi->local_capabilities & MIDR_CAP_GROUP_REP)
		vty_out(vty,
			"%% 本节点原有的群代表角色将被清除（引导专职化：二者不可兼任）\n");
	if (mi->config_group_id != 0 || mi->local_group_id != 0)
		vty_out(vty,
			"%% 本节点原有的群号 %u 将被清除（引导节点群号恒为 0、不参与任何群）\n",
			mi->config_group_id ? mi->config_group_id
					    : mi->local_group_id);

	midr_nds_set_capability(bgp, mi->local_capabilities |
					     MIDR_CAP_BOOTSTRAP);
	if (!bgp_config_inprocess())
		midr_nds_bootstrap_enforce(bgp);

	vty_out(vty, "MIDR role bootstrap set\n");
	return CMD_SUCCESS;
}

DEFUN(no_midr_role_bootstrap,
      no_midr_role_bootstrap_cmd,
      "no midr role bootstrap",
      NO_STR
      "MIDR configuration\n"
      "Set the local MIDR role\n"
      "Act as a bootstrap node\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);

	if (!bgp->midr_nds_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	midr_nds_set_capability(bgp, bgp->midr_nds_info->local_capabilities &
					     ~MIDR_CAP_BOOTSTRAP);
	vty_out(vty, "MIDR role bootstrap cleared\n");

	return CMD_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* midr role group-rep / no midr role group-rep                        */
/* ------------------------------------------------------------------ */

DEFUN(midr_role_group_rep,
      midr_role_group_rep_cmd,
      "midr role group-rep",
      "MIDR configuration\n"
      "Set the local MIDR role\n"
      "Act as a group representative\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);
	struct bgp_midr_nds *mi;
	struct midr_node_entry *ne;

	if (!bgp->midr_nds_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}
	mi = bgp->midr_nds_info;

	/*
	 * C-8：两道守卫。
	 *
	 * 【这条命令为什么是鸡肋】换届机制没实现、CL 选举是 stub（CL 侧一处
	 * 都不发 REP_ELECT），所以它现在唯一的正当用途是"僵尸群救场"——群里
	 * 代表没了、CL 又选不出新的，运维手动顶一个上去。它不是常规工具，
	 * 常规路径是任务甲的角色自动推导（按能力位）。
	 *
	 * 守卫①：群号为 0 拒绝。与轮 A 给 I-7 REP_ELECT 分支加的那道一致
	 * （bgp_midr_nds.c 内 REP_ELECT）——引导节点群号就是 0，"群 0 的代表"
	 * 会污染 rep 目录与名录。
	 *
	 * 守卫②：群里已有代表则拒绝。换届没落地时再指一个，除了造出双代表
	 * 没有别的用处。跳过自己，所以重复敲仍然幂等。
	 * 【何时该放开第②道】等 CL 真会发 REP_ELECT / REP_RESIGN（选举、换届
	 * 落地）之后，"手动指定第二个代表"才有正当场景（例如运维强制换届），
	 * 届时应改成"告警但允许"，而不是继续拒绝。
	 */
	/*
	 * 守卫⓪（08-13，硬互斥，优先级高于下面两道）：本机是引导节点 → 一律拒绝，
	 * **与是不是运维执行无关**。
	 * 引导专职化之后二者不可兼任：引导不通告自身，一旦兼任群代表，它就没法把
	 * 自己算进群代表名录（名录从节点表推导，而它不在表里），本群从此在全网目录
	 * 里消失——12 lab 的 g1a 实测正是这样让新节点找不到群 1 的。
	 * 要改角色：下线 → 改 conf → 重启，不走命令热切。
	 * 反向（引导写在本命令之后）由 bgp_config_end 兜底清理，见
	 * midr_nds_bootstrap_after_cfg()——两头夹住，结果与 conf 行序无关。
	 */
	if (mi->local_capabilities & MIDR_CAP_BOOTSTRAP) {
		vty_out(vty,
			"%% 本节点是 MIDR 引导节点，不得兼任群代表（引导专职化）——如需改角色，请下线后修改配置并重启\n");
		return CMD_WARNING_CONFIG_FAILED;
	}

	if (mi->local_group_id == 0) {
		vty_out(vty,
			"%% 本节点当前无群号（群 0 不得设代表）——先配 `midr group-id <N>`\n");
		return CMD_WARNING_CONFIG_FAILED;
	}

	frr_each (midr_node_hash, &mi->global_view->nodes, ne) {
		if (ne->is_self || ne->group_id != mi->local_group_id)
			continue;
		if (!midr_node_is_group_rep(ne))
			continue;
		vty_out(vty,
			"%% 群 %u 已有代表 %pI4——当前不支持双代表（CL 换届落地前，本命令只留给僵尸群救场）\n",
			mi->local_group_id, &ne->node_id.u.prefix4);
		return CMD_WARNING_CONFIG_FAILED;
	}

	midr_nds_set_capability(bgp, mi->local_capabilities |
					     MIDR_CAP_GROUP_REP);
	vty_out(vty, "MIDR role group-rep set\n");
	return CMD_SUCCESS;
}

DEFUN(no_midr_role_group_rep,
      no_midr_role_group_rep_cmd,
      "no midr role group-rep",
      NO_STR
      "MIDR configuration\n"
      "Set the local MIDR role\n"
      "Act as a group representative\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);

	if (!bgp->midr_nds_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	midr_nds_set_capability(bgp, bgp->midr_nds_info->local_capabilities &
					     ~MIDR_CAP_GROUP_REP);
	vty_out(vty, "MIDR role group-rep cleared\n");
	return CMD_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* `midr rep group` 已于 2026-08-11（保底轮 2 批 1.5）删除。            */
/*                                                                      */
/* 删它是为了断掉影子条目的源头：手配目录条目没有 router-id，引导应答   */
/* REP_LIST 时只能按 transport 反查节点表回填真名，反查不中就发 rid=0， */
/* 收方于是拿 transport 冒充 node_id 建占位条目——同一台机器在节点表里  */
/* 占两条键，无人合并，污染 CL 的群规模/好链路计数。目录改为只剩"按     */
/* GROUP_REP 能力位从节点表推导"一个来源后，条目的 rid 取自节点表的真名 */
/* 键、恒有真名，rid=0 这个中间态从产生源上消失。                       */
/*                                                                      */
/* 引导冷启动时目录为空不需要手配兜底：应答侧空目录本就沉默不回包，请求 */
/* 侧 3s×5 死心后 failover 换下一台引导，等 BGP-LS 收敛后自愈。         */
/* 救僵尸群改用 `midr role group-rep`（带"群里已有代表则拒"守卫）。     */
/* 决策见 docs/群间保底连接/轮2/轮2执行计划.md 答疑 92 与批 1.5 段。    */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* midr bootstrap <IP> remote-as <ASN> router-id <RID>  (command O)     */
/* ------------------------------------------------------------------ */

DEFUN(midr_bootstrap,
      midr_bootstrap_cmd,
      "midr bootstrap A.B.C.D remote-as (1-4294967295) router-id A.B.C.D",
      "MIDR configuration\n"
      "Join the network via a bootstrap node\n"
      "Bootstrap node IP address\n"
      "Remote AS\n"
      "AS number\n"
      "Bootstrap node router-id (identity; required)\n"
      "Router-id in dotted-quad form\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);
	union sockunion su;
	as_t asn = (as_t)atol(argv[4]->arg);
	struct in_addr rid;

	if (!bgp->midr_nds_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	if (str2sockunion(argv[2]->arg, &su) < 0) {
		vty_out(vty, "%% Invalid IP address: %s\n", argv[2]->arg);
		return CMD_WARNING_CONFIG_FAILED;
	}

	/*
	 * router-id 必选（批 5 R 系列）：引导节点不发 Node NLRI，谁也学不到它的
	 * 真名，只能由运维在这里给。三处指着它——挂靠挑台按 rid 排环（确定可
	 * 重放）、`no midr session <引导IP>` 按 rid 落排除名单、会话台账登记对端
	 * 身份。做成必选而非可选，是为了保住"候选池内 rid 恒非 0"这条不变量：
	 * 有一条无名条目，上面三处就都要加兜底。
	 */
	if (inet_pton(AF_INET, argv[6]->arg, &rid) != 1) {
		vty_out(vty, "%% Invalid router-id: %s\n", argv[6]->arg);
		return CMD_WARNING_CONFIG_FAILED;
	}
	if (rid.s_addr == INADDR_ANY) {
		vty_out(vty, "%% router-id 不能为 0.0.0.0\n");
		return CMD_WARNING_CONFIG_FAILED;
	}

	/*
	 * 回显只说"开始加入"，不提具体引导节点——这条命令的语义是【追加候选】
	 * （§8.32），敲下去未必就是去连你刚填的这个：已有在途 join 时它只是入
	 * 列排队，无在途时发第一跳的也是候选清单的【首条】（手配按加入顺序排、
	 * 种子排后），未必是刚敲的这条。具体走了哪条分支、实际在连谁，由
	 * midr_join_via_bootstrap() 里的两条 FLOW_LOG 记录。
	 *
	 * 引导节点上另说一句（5b）：第四守卫会把加入挡在 midr_join_round_start
	 * 入口，此时照旧回"开始加入"就是**回显说假话**——运维看着像发起了加入，
	 * 实际只入了列。这里按 BOOTSTRAP 位分岔，让回显与实际行为一致。
	 */
	if (bgp->midr_nds_info->local_capabilities & MIDR_CAP_BOOTSTRAP)
		vty_out(vty, "MIDR: 候选已入列（本机是引导节点，不发起加入）\n");
	else
		vty_out(vty, "MIDR: 开始加入\n");
	midr_join_via_bootstrap(bgp, &su, asn, rid);
	return CMD_SUCCESS;
}

/* no midr bootstrap [A.B.C.D]  (§8.32：删一条候选 / 清空候选并中止在途加入) */
DEFUN(no_midr_bootstrap,
      no_midr_bootstrap_cmd,
      "no midr bootstrap [A.B.C.D]",
      NO_STR
      "MIDR configuration\n"
      "Join the network via a bootstrap node\n"
      "Bootstrap node IP address (omit to clear all candidates)\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);
	union sockunion su;

	if (!bgp->midr_nds_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	/* 无地址参数：清空候选清单并作废在途加入意图（批注点 C）。 */
	if (argc < 4) {
		midr_bootstrap_clear(bgp);
		vty_out(vty, "MIDR: cleared all bootstrap candidates\n");
		return CMD_SUCCESS;
	}

	if (str2sockunion(argv[3]->arg, &su) < 0 || su.sa.sa_family != AF_INET) {
		vty_out(vty, "%% Invalid IPv4 address: %s\n", argv[3]->arg);
		return CMD_WARNING_CONFIG_FAILED;
	}
	if (!midr_bootstrap_list_del(bgp, su.sin.sin_addr)) {
		vty_out(vty, "%% No such bootstrap candidate: %s\n", argv[3]->arg);
		return CMD_WARNING;
	}
	vty_out(vty, "MIDR: removed bootstrap candidate %s\n", argv[3]->arg);
	return CMD_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* midr transport-address A.B.C.D   (node's real reachable locator)    */
/* ------------------------------------------------------------------ */

DEFUN(midr_transport_address,
      midr_transport_address_cmd,
      "midr transport-address A.B.C.D",
      "MIDR configuration\n"
      "Set the local reachable address others use to peer with / probe us\n"
      "IPv4 address\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);
	struct in_addr addr;

	if (!bgp->midr_nds_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	if (inet_pton(AF_INET, argv[2]->arg, &addr) != 1) {
		vty_out(vty, "%% Invalid IPv4 address: %s\n", argv[2]->arg);
		return CMD_WARNING_CONFIG_FAILED;
	}

	bgp->midr_nds_info->local_transport_addr = addr;
	bgp->midr_nds_info->transport_addr_set = true;
	midr_nds_report_node(bgp, MIDR_ORIGIN_TRANSPORT_UPDATE); /* +TLV 1188 */

	/* Open PM socket now that we have an address to bind to.  midr_pm_init()
	 * runs before the config file is read, so the socket is deferred until
	 * this command is processed. */
	midr_pm_on_transport_addr_set(bgp);

	/* Same deferred-open lifecycle for the ctrl UDP channel (bind needs an
	 * address to bind to, and midr_ctrl_init() also runs before this). */
	midr_ctrl_on_transport_addr_set(bgp);

	vty_out(vty, "MIDR transport-address set to %s\n", argv[2]->arg);
	return CMD_SUCCESS;
}

DEFUN(no_midr_transport_address,
      no_midr_transport_address_cmd,
      "no midr transport-address [A.B.C.D]",
      NO_STR
      "MIDR configuration\n"
      "Clear the local reachable address\n"
      "IPv4 address\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);

	if (!bgp->midr_nds_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	bgp->midr_nds_info->transport_addr_set = false;
	bgp->midr_nds_info->local_transport_addr.s_addr = INADDR_ANY;
	midr_nds_report_node(bgp, MIDR_ORIGIN_TRANSPORT_UPDATE); /* -TLV 1188 */

	vty_out(vty, "MIDR transport-address cleared\n");
	return CMD_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* midr shutdown   (退网：撤通告 + 拆会话 + 停探 + 清表 + 群号回落)      */
/*                                                                      */
/* 语义是**退网**不是暂停：本机退化成"只有静态配置、尚未入网"的新节点，  */
/* bgpd 与 underlay 一动不动。动作全在 NDS 层，本命令只置状态并回显      */
/* （"midr 命令只置状态"的规矩：动作交事件 / 收敛机器 / config_end）。   */
/* 决策 docs/decisions/midr-shutdown-semantics.md                        */
/* ------------------------------------------------------------------ */

DEFUN(midr_shutdown,
      midr_shutdown_cmd,
      "midr shutdown",
      "MIDR configuration\n"
      "Leave the MIDR fabric: withdraw, tear down MIDR sessions, stop probing\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);
	bool was_rep;
	unsigned int manual;

	if (!bgp->midr_nds_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	if (bgp->midr_nds_info->shutdown) {
		vty_out(vty, "MIDR: 已处于退网状态，无动作\n");
		return CMD_SUCCESS;
	}

	/* 角色位在 enter 里会被清掉，提醒要用的信息先取。 */
	was_rep = !!(bgp->midr_nds_info->local_capabilities &
		     MIDR_CAP_GROUP_REP);

	manual = midr_nds_shutdown_enter(bgp);

	vty_out(vty,
		"MIDR: 已退网（撤销自身通告、拆除 MIDR 会话、停止探测、清空节点表；群号回落 %u）\n",
		bgp->midr_nds_info->local_group_id);
	/* 运维手配过的两样：退网清运行态，提醒重入后自己重敲（不替运维记账）。 */
	if (manual)
		vty_out(vty,
			"%% 其中 %u 条是运维手配会话——重入后如仍需要，请重敲 `midr session <IP> remote-as <ASN>`\n",
			manual);
	if (was_rep)
		vty_out(vty,
			"%% 已卸掉群代表角色——重入后如仍需要，请重敲 `midr role group-rep`\n");
	return CMD_SUCCESS;
}

DEFUN(no_midr_shutdown,
      no_midr_shutdown_cmd,
      "no midr shutdown",
      NO_STR
      "MIDR configuration\n"
      "Rejoin the MIDR fabric: re-advertise and join via a bootstrap node\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);

	if (!bgp->midr_nds_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	if (!bgp->midr_nds_info->shutdown) {
		vty_out(vty, "MIDR: 当前未退网，无动作\n");
		return CMD_SUCCESS;
	}

	if (midr_nds_shutdown_exit(bgp))
		vty_out(vty,
			"MIDR: 已重入（恢复通告，按新节点流程重新经引导加入）\n");
	else
		vty_out(vty,
			"%% MIDR: 已恢复通告，但引导候选清单为空、未发起加入——请先配 `midr bootstrap <IP> remote-as <ASN>`\n");
	return CMD_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* show midr nodes                                                     */
/* ------------------------------------------------------------------ */

static const char *midr_caps_str(uint32_t caps, char *buf, size_t len)
{
	buf[0] = '\0';
	if (!caps) {
		strlcpy(buf, "none", len);
		return buf;
	}
	if (caps & MIDR_CAP_SRV6)
		strlcat(buf, "SRv6 ", len);
	if (caps & MIDR_CAP_ROUTING)
		strlcat(buf, "Routing ", len);
	if (caps & MIDR_CAP_BOOTSTRAP)
		strlcat(buf, "Bootstrap ", len);
	if (caps & MIDR_CAP_GROUP_REP)
		strlcat(buf, "GroupRep", len);
	return buf;
}

/*
 * 三个节点表视图命令 (nodes/reps/bootstraps) 共用的表格体: required_caps=0
 * 列全部, 非 0 只列 capabilities 含全部所要位的节点。reps/bootstraps 是同一
 * 张表的过滤视图, 含 expired 条目 (诊断命令要能看见异常; 协议侧 REP_LIST 组
 * 装另用严过滤, 见 midr_rep_candidates)。
 */
/* 年龄格式化：show midr nodes 的 Age 列与 bootstrap-seeds 共用一把尺。 */
static void midr_seed_age_str(time_t age, char *buf, size_t len)
{
	if (age < 0)
		snprintf(buf, len, "(未来?)"); /* 钟被调过；只提示不猜 */
	else if (age < 60)
		snprintf(buf, len, "%llds", (long long)age);
	else if (age < 3600)
		snprintf(buf, len, "%lldm%llds", (long long)(age / 60),
			 (long long)(age % 60));
	else if (age < 86400)
		snprintf(buf, len, "%lldh%lldm", (long long)(age / 3600),
			 (long long)((age % 3600) / 60));
	else
		snprintf(buf, len, "%lldd%lldh", (long long)(age / 86400),
			 (long long)((age % 86400) / 3600));
}

static void midr_show_node_table(struct vty *vty, struct bgp *bgp,
				 uint32_t required_caps)
{
	struct midr_node_entry *entry;
	char caps_buf[64];
	time_t now = monotime(NULL);

	vty_out(vty, "%-18s %-18s %-8s %-10s %-8s %s\n",
		"Router-ID", "Transport-Addr", "ASN", "Group-ID", "Age",
		"Capabilities");
	vty_out(vty, "%-18s %-18s %-8s %-10s %-8s %s\n",
		"------------------", "------------------", "--------",
		"----------", "--------", "------------");

	frr_each (midr_node_hash, &bgp->midr_nds_info->global_view->nodes, entry) {
		char taddr[INET_ADDRSTRLEN];
		char age[32];

		if ((entry->capabilities & required_caps) != required_caps)
			continue;

		if (entry->has_transport_addr)
			inet_ntop(AF_INET, &entry->transport_addr, taddr,
				  sizeof(taddr));
		else
			snprintf(taddr, sizeof(taddr), "-");

		/* 件④：Age 取代 Status —— 老化判死已删，这里只报"上次收到关于
		 * 它的消息是多久前"，不含活性含义。 */
		if (entry->is_self)
			snprintf(age, sizeof(age), "-");
		else
			midr_seed_age_str(now - entry->last_update, age,
					  sizeof(age));

		vty_out(vty, "%-18pI4 %-18s %-8u %-10u %-8s %s\n",
			&entry->node_id.u.prefix4,
			taddr,
			entry->asn,
			entry->group_id,
			age,
			midr_caps_str(entry->capabilities, caps_buf,
				      sizeof(caps_buf)));
	}
}

DEFUN(show_midr_nodes,
      show_midr_nodes_cmd,
      "show midr nodes",
      SHOW_STR
      "MIDR information\n"
      "Show MIDR node table\n")
{
	struct bgp *bgp = bgp_get_default();

	if (!bgp) {
		vty_out(vty, "%% No BGP instance found\n");
		return CMD_WARNING;
	}

	if (!bgp->midr_nds_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	midr_show_node_table(vty, bgp, 0);

	return CMD_SUCCESS;
}

DEFUN(show_midr_reps,
      show_midr_reps_cmd,
      "show midr reps",
      SHOW_STR
      "MIDR information\n"
      "Show group representatives (nodes with the GROUP_REP capability)\n")
{
	struct bgp *bgp = bgp_get_default();

	if (!bgp) {
		vty_out(vty, "%% No BGP instance found\n");
		return CMD_WARNING;
	}

	if (!bgp->midr_nds_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	/*
	 * 只剩这一段"全网视图"（按 GROUP_REP 能力位从节点表过滤，随 NLRI 更新、
	 * 会 expire、带 router-id）。原下半段 `Local rep directory` 已随
	 * `midr rep group` 一并删除（2026-08-11 批 1.5）——手配来源没了之后，
	 * 引导侧 rep_dir 恒空，加入方那份则是 join 那一刻收来的死快照（无刷新
	 * 无老化），展示出来只会被读成"这是我配的"。看代表一律看本表。
	 */
	midr_show_node_table(vty, bgp, MIDR_CAP_GROUP_REP);

	return CMD_SUCCESS;
}

DEFUN(show_midr_bootstraps,
      show_midr_bootstraps_cmd,
      "show midr bootstraps",
      SHOW_STR
      "MIDR information\n"
      "Show bootstrap nodes (nodes with the BOOTSTRAP capability)\n")
{
	struct bgp *bgp = bgp_get_default();

	if (!bgp) {
		vty_out(vty, "%% No BGP instance found\n");
		return CMD_WARNING;
	}

	if (!bgp->midr_nds_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	midr_show_node_table(vty, bgp, MIDR_CAP_BOOTSTRAP);

	return CMD_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* show midr bootstrap-seeds （子稿 §4-2）                             */
/*                                                                     */
/* 与上面 `show midr bootstraps` 是**两回事**，别混：                  */
/*   - bootstraps  = 节点表按 BOOTSTRAP 能力位过滤的**运行时视图**     */
/*     （靠对方发 Node NLRI 才看得见；批 5 引导不再发 NLRI 后它会空掉，*/
/*      删除已登记在对接轮清理批）；                                   */
/*   - bootstrap-seeds = **持久化种子表**（bgpd.db）现查现出，是"我连过/ */
/*     学到过哪些引导"的跨重启记忆，重启自举的第一跳清单就从它读回。   */
/* ------------------------------------------------------------------ */

struct midr_seed_show_ctx {
	struct vty *vty;
	unsigned int count;
	time_t now;
};

/* 把"距今多久"印成人话（秒/分/时/天），比裸的 epoch 好读得多。 */
static void midr_seed_show_cb(const char *transport, uint32_t asn,
			      const char *rid, time_t last_seen, void *arg)
{
	struct midr_seed_show_ctx *ctx = arg;
	char age[32];

	midr_seed_age_str(ctx->now - last_seen, age, sizeof(age));
	vty_out(ctx->vty, "%-18s %-10u %-16s %s\n", transport, asn,
		rid ? rid : "-", age);
	ctx->count++;
}

DEFUN(show_midr_bootstrap_seeds,
      show_midr_bootstrap_seeds_cmd,
      "show midr bootstrap-seeds",
      SHOW_STR
      "MIDR information\n"
      "Show persisted bootstrap seeds (bgpd.db, used for self-boot after restart)\n")
{
	struct midr_seed_show_ctx ctx;

	ctx.vty = vty;
	ctx.count = 0;
	/* 墙钟：种子表的 last_seen 就是本机墙钟（子稿 §4-5），两者必须同一把尺。
	 * 不能用 monotime——那是节点表那个同名字段的时钟，混用会算出荒谬的距今。 */
	ctx.now = time(NULL);

	vty_out(vty, "%-18s %-10s %-16s %s\n", "Transport", "ASN", "Router-ID",
		"Last-seen");
	vty_out(vty, "%-18s %-10s %-16s %s\n", "------------------",
		"----------", "----------------", "------------");

	midr_store_seed_load(midr_seed_show_cb, &ctx);

	/* 无 sqlite 编译时 midr_store_seed_load 是空宏，输出与"库里没有种子"
	 * 无法区分——两种情况都印这一行，措辞覆盖两者，不谎称"没有种子"。 */
	if (!ctx.count)
		vty_out(vty, "(种子表为空，或本 bgpd 未编译 SQLite 支持)\n");
	else
		vty_out(vty, "共 %u 条种子（按 last_seen 新→旧）\n", ctx.count);

	return CMD_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* show midr neighbors                                                 */
/*                                                                     */
/* Unlike `show midr nodes` (which lists every node learned from a     */
/* BGP-LS Node NLRI — possibly relayed, not directly connected), this  */
/* lists the BGP peers we have an actual MIDR session with, i.e. peers */
/* with the BGP-LS address-family activated.  Group-id / capabilities  */
/* are cross-referenced from the node table when available.            */
/* ------------------------------------------------------------------ */

/*
 * BGP RIB（ipv4 unicast）里有没有覆盖该前缀的路由。注意这只是"BGP 视角
 * 有无覆盖路由"，不是严格的转发可达性（zebra/内核 FIB 才是权威）；用途是
 * 给 show 命令里"卡住"的目标一个可行动的提示（underlay 路由缺失时 UDP 黑洞
 * / 会话卡 Active 都是静默的，见 transport 互通部署契约）。
 */
static bool midr_bgp_rib_covers(struct bgp *bgp, const struct prefix *p)
{
	struct bgp_dest *dest;

	if (!bgp->rib[AFI_IP][SAFI_UNICAST])
		return true; /* 判不了当可达，不误报 */

	dest = bgp_node_match(bgp->rib[AFI_IP][SAFI_UNICAST], p);
	if (!dest)
		return false;
	bgp_dest_unlock_node(dest); /* bgp_node_match 返回已加锁节点 */
	return true;
}

/*
 * 对端 transport 地址在本端是否可达：优先查 BGP nexthop tracking 缓存
 * （multihop peer 建连时经 FSM 注册，键 = connection->su 的 /32 host 前缀，
 * 见 bgp_find_or_add_nexthop 的 peer 分支）；无缓存条目（peer 未注册 NHT）
 * 再退化查 BGP RIB 覆盖路由。只读，不注册、不改状态。
 */
static bool midr_underlay_reachable(struct bgp *bgp, union sockunion *su)
{
	struct prefix p;
	struct bgp_nexthop_cache *bnc;

	if (sockunion_family(su) != AF_INET || !sockunion2hostprefix(su, &p))
		return true; /* 判不了当可达，不误报 */

	bnc = bnc_find(&bgp->nexthop_cache_table[AFI_IP], &p, 0, 0);
	if (bnc)
		return CHECK_FLAG(bnc->flags, BGP_NEXTHOP_VALID);

	return midr_bgp_rib_covers(bgp, &p);
}

DEFUN(show_midr_neighbors,
      show_midr_neighbors_cmd,
      "show midr neighbors",
      SHOW_STR
      "MIDR information\n"
      "Show established MIDR (BGP-LS) sessions\n")
{
	struct bgp *bgp = bgp_get_default();
	struct midr_node_hash_head *nodes;
	struct peer *peer;
	struct listnode *node;
	char caps_buf[64];
	bool any = false;
	unsigned int established = 0;
	unsigned int no_nlri = 0;

	if (!bgp) {
		vty_out(vty, "%% No BGP instance found\n");
		return CMD_WARNING;
	}

	if (!bgp->midr_nds_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	nodes = &bgp->midr_nds_info->global_view->nodes;

	vty_out(vty, "%-18s %-8s %-14s %-6s %-10s %-15s %s\n",
		"Neighbor", "ASN", "State", "LS", "Group-ID", "Origin",
		"Capabilities");
	vty_out(vty, "%-18s %-8s %-14s %-6s %-10s %-15s %s\n",
		"------------------", "--------", "--------------", "------",
		"----------", "---------------", "------------");

	for (ALL_LIST_ELEMENTS_RO(bgp->peer, node, peer)) {
		struct midr_node_entry *ne = NULL;
		const struct midr_session_ledger_entry *le = NULL;
		const char *nbr_disp;
		char taddr[INET_ADDRSTRLEN];
		char state_buf[32];
		char origin[20];

		/*
		 * 件②（轮 4）改按**归属**过滤：列的是"MIDR 自己的边"。
		 *
		 * 改前按承载判（afc[4][8]），把"载不载 LS"写进了过滤条件——等于把
		 * 坏会话藏起来：一条 MIDR 会话没激活 LS 恰恰是异常、最该被看见。现在
		 * 承载降为下面的 LS 列（yes / no），异常一眼可辨。
		 * 迁族后不再复用运维原生会话（真分离约定），故归属过滤不会漏掉谁。
		 */
		if (!midr_nds_peer_is_overlay(peer))
			continue;

		any = true;
		if (peer->connection->status == Established)
			established++;

		/*
		 * 按对端 router-id 反查节点表：节点表以 node_id(router-id) 为键，
		 * peer->remote_id 是对端 OPEN 报文里的 router-id，与会话用哪个地址
		 * 建立无关——故直连静态链路会话也能命中。旧版按 peer 连接地址匹配
		 * 节点 locator，静态会话的链路地址对不上 transport，会错误显示 n/a。
		 */
		if (peer->remote_id.s_addr != INADDR_ANY) {
			struct midr_node_entry key = {};

			key.node_id.family = AF_INET;
			key.node_id.prefixlen = IPV4_MAX_BITLEN;
			key.node_id.u.prefix4 = peer->remote_id;
			ne = midr_node_hash_find(nodes, &key);
		}

		/*
		 * 命中节点则统一显示其 transport（稳定 loopback，比链路地址直观）；
		 * 否则回退显示 peer 的连接地址。
		 */
		nbr_disp = peer->host;
		if (ne && ne->has_transport_addr) {
			inet_ntop(AF_INET, &ne->transport_addr, taddr,
				  sizeof(taddr));
			nbr_disp = taddr;
		}

		/*
		 * 卡在非 Established 时区分两种情形：对端尚未拨入（正常等待）
		 * vs 到对端 transport 无路由（underlay 断，坏事）——后者追加
		 * " (no route)" 标注。
		 */
		snprintf(state_buf, sizeof(state_buf), "%s",
			 lookup_msg(bgp_status_msg, peer->connection->status,
				    NULL));
		if (peer->connection->status != Established &&
		    !midr_underlay_reachable(bgp, &peer->connection->su))
			strlcat(state_buf, " (no route)", sizeof(state_buf));

		/*
		 * Origin = 会话台账里"这条边为什么存在"（轮 1 溢出条 1）。
		 * 查账键是 transport：MIDR 自建会话的连接地址就是 transport；
		 * 复用原生会话时连接地址是链路地址、对不上，故优先用节点表里的
		 * transport 反查、再回落到连接地址。查不到账印 `-`（运维原生
		 * 会话、或 MIDR 没记过需求的边，两者都属"不是 MIDR 要的边"）。
		 */
		if (ne && ne->has_transport_addr)
			le = midr_nds_ledger_lookup(bgp, ne->transport_addr);
		if (!le && sockunion_family(&peer->connection->su) == AF_INET)
			le = midr_nds_ledger_lookup(
				bgp, peer->connection->su.sin.sin_addr);
		if (le)
			snprintf(origin, sizeof(origin), "%s",
				 midr_session_reason_str(le->reason));
		else
			snprintf(origin, sizeof(origin), "-");

		/* LS 列：这条 MIDR 会话到底载没载 MIDR-LS。no = 异常（会话是
		 * MIDR 自建的、却没激活拓扑族，拓扑情报根本传不了）。 */
		vty_out(vty, "%-18s %-8u %-14s %-6s ", nbr_disp, peer->as,
			state_buf,
			peer->afc[AFI_BGP_LS][SAFI_MIDR_LS] ? "yes" : "no");

		if (ne) {
			vty_out(vty, "%-10u %-15s %s\n", ne->group_id, origin,
				midr_caps_str(ne->capabilities, caps_buf,
					      sizeof(caps_buf)));
		} else {
			/*
			 * 这两列的数据源都是节点表（对端的 MIDR 身份）。查不到时
			 * 分两档印，别混成一个 n/a——两者要采取的动作完全不同：
			 *
			 *   -        会话还没建起来，压根没交换过身份（没收过 OPEN、
			 *            remote_id 是 0，节点表无从查起）。正常等待，
			 *            与 Origin 列的 `-` 同义：不适用。
			 *   no-nlri  会话在，但没收到过它的 Node NLRI。
			 *
			 * no-nlri **不等于故障**：专职引导节点按设计就不发 Node NLRI
			 * （保底轮 2 批 5 起），指向引导的边一律长这样；对端不是 MIDR
			 * 节点、或刚建连尚未收敛，也都会落到这一档。要判它是不是问题，
			 * 看同一行的 Origin（ATTACH = 指向引导，属预期）。
			 */
			const char *why = peer->connection->status == Established
						  ? "no-nlri"
						  : "-";

			if (peer->connection->status == Established)
				no_nlri++;
			vty_out(vty, "%-10s %-15s %s\n", why, origin, why);
		}
	}

	if (!any) {
		vty_out(vty, "%% No MIDR (BGP-LS) sessions\n");
		return CMD_SUCCESS;
	}

	/*
	 * C-11：口径说明必须落在输出里——排查的人看终端不翻文档。
	 * 本表列的是"激活了 BGP-LS 的 peer"，不区分建没建起来：拆过又被对端
	 * 重连的、跨群 ANCHOR 探测留下的，都会以非 Established 状态长期挂在
	 * 表内。数"有几条会话可用"必须看下面这个数，不能数行数。
	 *
	 * ⚠ 这句话里**刻意不出现 `Established` 字样**（改措辞时别把它加回来）：
	 * 它一出现，本行就会被 `grep -c Established` 连带数进去——批 4 实测 4 条
	 * 会话数成 5，且错在偏多的方向，"骨干应有 N 条"这类判据会在实际少一条时
	 * 假 PASS。表体的 State 列用的是 FRR 标准状态名，是人习惯去 grep 的词。
	 */
	vty_out(vty,
		"共 %u 条会话处于已建立状态（State 列为其他状态的行也在表内，别按行数计）\n",
		established);

	/* 同 C-11 的道理：no-nlri 这一档不解释，看表的人会当成故障去查。 */
	if (no_nlri)
		vty_out(vty,
			"其中 %u 条会话已建立、但节点表里没有对端条目（两列印 no-nlri）：对端没发 Node NLRI（专职引导节点即如此）、不是 MIDR 节点、或尚未收敛——不一定是故障，对照 Origin 列判断\n",
			no_nlri);

	return CMD_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* show midr join                                                      */
/* ------------------------------------------------------------------ */

static const char *midr_join_phase_str(enum midr_join_phase phase)
{
	switch (phase) {
	case MIDR_JOIN_IDLE:
		return "idle";
	case MIDR_JOIN_PROBING_REPS:
		return "probing-reps";
	case MIDR_JOIN_PROBING_MEMBERS:
		return "probing-members";
	}
	return "unknown";
}

/*
 * 本机自身的 MIDR 状态。
 *
 * 为什么单开一条命令、而不是从 `show midr nodes` 里看自己（08-13 立）：节点表里
 * 那条"自身条目"当初是 origination 的**副产品**，通告发不出去时就不再更新，
 * "我是谁"这件最基本的事反而在表里查不到。本命令直接读运行态实例
 * （bgp->midr_nds_info），与"发不发得出去"彻底解耦。
 * 〔件②（轮 4）后自身条目改由 midr_nds_report_node() 无条件刷新，那个副作用已
 * 消失；但本命令"直接读运行态"的定位不变，仍是查身份最可靠的一条。〕
 */
/*
 * 对第二组接口的状态汇总（轮 4 联调用）：上行（facts 上报）+ 下行（remote-view
 * 回调）两个方向各自的账。判据脚本可直接 grep 这些行。
 */
DEFUN(show_midr_group2,
      show_midr_group2_cmd,
      "show midr group2",
      SHOW_STR
      "MIDR information\n"
      "第二组接口对接状态（上报线 + 远端视图回调）\n")
{
	struct bgp *bgp = bgp_get_default();
	struct bgp_midr_nds *mi;
	struct midr_nds_facts *f;
	struct listnode *node;
	struct midr_nds_fact_link *fl;
	uint32_t link_reported = 0, link_pending = 0, link_total = 0;

	if (!bgp || !bgp->midr_nds_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}
	mi = bgp->midr_nds_info;
	f = mi->facts;

	vty_out(vty, "MIDR <-> 第二组接口状态\n");
	vty_out(vty, "  context           : %s\n",
		mi->g2_ctx ? "已取得" : "未就绪");

	vty_out(vty, "  [上行] 本地事实上报\n");
	if (!f) {
		vty_out(vty, "    事实表          : 未就绪\n");
	} else {
		for (ALL_LIST_ELEMENTS_RO(f->links, node, fl)) {
			link_total++;
			if (fl->reported)
				link_reported++;
			if (fl->pending)
				link_pending++;
		}
		vty_out(vty, "    node            : valid=%d reported=%d pending=%d version=%llu\n",
			f->node_valid, f->node_reported, f->node_pending,
			(unsigned long long)f->node.version);
		vty_out(vty, "    link            : 条目 %u（reported %u / pending %u）\n",
			link_total, link_reported, link_pending);
		vty_out(vty, "    snapshot_version: %llu\n",
			(unsigned long long)f->snapshot_version);
	}

	vty_out(vty, "  [下行] 远端视图回调\n");
	vty_out(vty, "    回调注册        : %s\n",
		mi->remote_view_registered ? "已注册" : "未注册");
	vty_out(vty, "    node 事件       : %llu（update + withdraw）\n",
		(unsigned long long)mi->remote_node_events);
	vty_out(vty, "    link 事件       : %llu（本轮只观察不消费）\n",
		(unsigned long long)mi->remote_link_events);
	vty_out(vty, "    疑似虚报撤销    : %llu\n",
		(unsigned long long)mi->remote_suspect_count);

	return CMD_SUCCESS;
}

DEFUN(show_midr_self,
      show_midr_self_cmd,
      "show midr self",
      SHOW_STR
      "MIDR information\n"
      "Show this node's own MIDR state\n")
{
	struct bgp *bgp = bgp_get_default();
	struct bgp_midr_nds *mi;
	char caps_buf[64];

	if (!bgp || !bgp->midr_nds_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}
	mi = bgp->midr_nds_info;

	vty_out(vty, "Router-ID         : %pI4\n", &bgp->router_id);
	if (mi->transport_addr_set)
		vty_out(vty, "Transport-Addr    : %pI4\n",
			&mi->local_transport_addr);
	else
		vty_out(vty, "Transport-Addr    : - （未配 midr transport-address）\n");
	vty_out(vty, "ASN               : %u\n", bgp->as);
	vty_out(vty, "Group-ID          : %u\n", mi->local_group_id);
	/* 配置值与运行值分离，口径同 `show midr join`：未配印 "-"，免得与群 0
	 * （引导的合法群号）混淆。 */
	if (mi->config_group_id)
		vty_out(vty, "Configured gid    : %u\n", mi->config_group_id);
	else
		vty_out(vty, "Configured gid    : -\n");
	vty_out(vty, "Capabilities      : %s\n",
		midr_caps_str(mi->local_capabilities, caps_buf,
			      sizeof(caps_buf)));
	vty_out(vty, "Shutdown          : %s\n", mi->shutdown ? "yes" : "no");

	/* 〔件②（轮 4）删去 BGP-LS distribute 那两行：本机身份不再经自有 BGP-LS
	 * 通告，该开关与 MIDR 无关了。要看身份报没报出去，查 `show midr group2`
	 * 的上行段（node valid/reported/version）。〕 */

	return CMD_SUCCESS;
}

/*
 * 「我们视角看到的第二组」—— 下行对账，与下面那条上行自检成对。
 *
 * 拉一份他们的 remote view 全量快照，与我方节点表逐条 diff：只在两边**不一致**时
 * 出行（缺、多、字段不同），一致就只报一句总数。用途 = 运行中怀疑漏了增量回调时
 * 人肉核一次（回调是差分通知，漏一次就永久偏差，光看计数器看不出来）。
 *
 * get/release 必须配对：数组是他们分配的，看完还回去。
 */
DEFUN(show_midr_group2_remote,
      show_midr_group2_remote_cmd,
      "show midr group2-remote",
      SHOW_STR
      "MIDR information\n"
      "Diff the second group's remote view against our node table\n")
{
	struct bgp *bgp = bgp_get_default();
	struct midr_remote_view_snapshot snap = {};
	struct midr_context *ctx;
	struct bgp_midr_nds *mi;
	size_t i;
	uint32_t diff = 0, matched = 0;
	int ret;

	if (!bgp || !bgp->midr_nds_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}
	mi = bgp->midr_nds_info;

	ctx = midr_nds_group2_ctx(bgp);
	if (!ctx) {
		vty_out(vty, "%% 第二组 context 不可用\n");
		return CMD_WARNING;
	}

	ret = midr_remote_view_snapshot_get(ctx, &snap);
	if (ret) {
		vty_out(vty, "%% remote view 快照不可用: %d%s\n", ret,
			ret == -EAGAIN ? "（LSDB 尚未就绪，稍后再试）" : "");
		return CMD_WARNING;
	}

	vty_out(vty, "MIDR 远端视图对账（第二组 %zu node / %zu link，快照版本 %" PRIu64 "）\n",
		snap.node_count, snap.link_count, snap.snapshot_version);

	for (i = 0; i < snap.node_count; i++) {
		const struct midr_remote_node_info *rn = &snap.nodes[i];
		struct midr_node_entry key = {};
		struct midr_node_entry *e;
		struct in_addr rid, transport;
		uint32_t caps;
		bool has_transport;

		midr_nds_remote_node_decode(rn, &rid, &caps, &transport,
					    &has_transport);
		key.node_id.family = AF_INET;
		key.node_id.prefixlen = IPV4_MAX_BITLEN;
		key.node_id.u.prefix4 = rid;
		if (rid.s_addr == bgp->router_id.s_addr)
			continue; /* 自己那条不参与对账（回调侧本就丢弃） */

		e = midr_node_hash_find(&mi->global_view->nodes, &key);
		if (!e) {
			vty_out(vty, "  ✗ %pI4 第二组有、我方节点表**缺**（群 %u）\n",
				&rid, rn->group_id);
			diff++;
			continue;
		}
		if (e->group_id != rn->group_id) {
			vty_out(vty, "  ✗ %pI4 群号不一致（我方 %u / 第二组 %u）\n",
				&rid, e->group_id, rn->group_id);
			diff++;
		}
		if (e->capabilities != caps) {
			vty_out(vty, "  ✗ %pI4 caps 不一致（我方 0x%x / 第二组 0x%x）\n",
				&rid, e->capabilities, caps);
			diff++;
		}
		if (has_transport && e->has_transport_addr &&
		    e->transport_addr.s_addr != transport.s_addr) {
			vty_out(vty, "  ✗ %pI4 transport 不一致（我方 %pI4 / 第二组 %pI4）\n",
				&rid, &e->transport_addr, &transport);
			diff++;
		}
		matched++;
	}

	/* 反向：我方表里有、第二组快照里没有的（回调漏了 withdraw，或引导群 0 在
	 * 他们侧 pending——后者是预期，日志里会看到群号 0）。 */
	{
		struct midr_node_entry *e;
		uint32_t expected = 0;

		frr_each (midr_node_hash, &mi->global_view->nodes, e) {
			bool found = false;

			if (e->is_self)
				continue;
			for (i = 0; i < snap.node_count && !found; i++)
				found = (snap.nodes[i].node_id ==
					 e->node_id.u.prefix4.s_addr);
			if (found)
				continue;
			/*
			 * 群 0 缺席是**预期**（交接给轮4 §5c-1）：引导群号恒 0，
			 * 在第二组侧是 pending 不回灌；我方表里有它只因旧 NLRI 线
			 * 还在跑，件② 删旧线后两边就一致了。单独计数、不算不一致，
			 * 免得每次刷 5 条 ✗ 把真问题淹掉。
			 */
			if (e->group_id == 0) {
				expected++;
				continue;
			}
			vty_out(vty, "  ✗ %pFX 我方节点表有、第二组**缺**（群 %u）\n",
				&e->node_id, e->group_id);
			diff++;
		}
		if (expected)
			vty_out(vty, "  · 群 0 节点 %u 个只在我方表里（引导，第二组侧 pending，预期）\n",
				expected);
	}

	midr_remote_view_snapshot_release(ctx, &snap);

	if (!diff)
		vty_out(vty, "  ✓ 两侧一致（对上 %u 个节点）\n", matched);
	else
		vty_out(vty, "  共 %u 处不一致\n", diff);

	return CMD_SUCCESS;
}

/*
 * 「第二组视角看到的我们」—— 手动触发一次 snapshot_get 并把返回的数组打出来。
 *
 * 为什么要有它：snapshot_get 平时只有第二组会调（resync 时），我方看不见返回
 * 值；本命令给那个方向开个窗口，与 `show midr nodes` 肉眼对账。打印的是**单
 * 节点**信息 —— 本机 1 个 Node + 本机全部出向 Link，不是组内也不是全网（全网
 * 视图走反方向的 remote view 接口，第二组实现）。
 *
 * 长期诊断命令（轮 5 复核定案，原登记为"临时调试件"）：它是**唯一**能看见
 * provider 返回码的地方 —— 自检失败整份 -EAGAIN、router-id 为 0 返 -EAGAIN、
 * 满格丢包转 withdraw，这三个取舍都只有这里观察得到。
 * ⚠ 与第二组的 `show midr owned` 不重叠：那条答"对方 LSDB 里现在有我什么"
 * （事实到达之后），本条答"我方现在会给出什么"（事实发出之前）。排查
 * "我方以为报了、对方却没有"时两条一起看，才分得清是发送侧还是接收侧。
 */
DEFUN(show_midr_group2_snapshot,
      show_midr_group2_snapshot_cmd,
      "show midr group2-snapshot",
      SHOW_STR
      "MIDR information\n"
      "Dump the topology snapshot as the second group would receive it\n")
{
	struct bgp *bgp = bgp_get_default();
	struct midr_topology_snapshot snapshot = {};
	struct midr_context *ctx;
	struct bgp_midr_nds *mi;
	char caps_buf[64];
	unsigned int table_links = 0;
	size_t i;
	int ret;

	if (!bgp || !bgp->midr_nds_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}
	mi = bgp->midr_nds_info;
	if (mi->facts && mi->facts->links)
		table_links = mi->facts->links->count;

	ctx = midr_nds_group2_ctx(bgp);
	ret = midr_topology_snapshot_get(ctx, &snapshot);
	if (ret) {
		vty_out(vty, "snapshot_get() = %d", ret);
		if (ret == -EAGAIN)
			vty_out(vty,
				" (-EAGAIN：事实表或本机身份尚未就绪，或对象自检失败——查 warn 日志。第二组会保留旧基线并稍后重试)\n");
		else if (ret == -ENOSYS)
			vty_out(vty, " (-ENOSYS：provider 不可用)\n");
		else
			vty_out(vty, "\n");
		return CMD_WARNING;
	}

	vty_out(vty, "snapshot_get() = 0（完整权威全量：未列出的对象会被对方视为已失效）\n");
	vty_out(vty, "snapshot_version : %" PRIu64 "\n",
		snapshot.snapshot_version);
	vty_out(vty, "node_count       : %zu\n", snapshot.node_count);
	/* 表内条目数 vs 入选数：差值就是被闸门排除的（未报过 / 墓碑 / 热身未完 /
	 * 会话已断 / 保底边），对账时一眼看出"为什么少了"。 */
	vty_out(vty, "link_count       : %zu（事实表内 %u 条，差值 = 被上报闸门排除）\n",
		snapshot.link_count, table_links);

	if (!snapshot.node_count && !snapshot.link_count)
		vty_out(vty, "\n(空快照%s)\n",
			midr_nds_is_bootstrap(bgp)
				? "：本机是引导节点，只转发不自产"
				: mi->shutdown ? "：本机已优雅下线" : "");

	for (i = 0; i < snapshot.node_count; i++) {
		const struct midr_node_update *n = &snapshot.nodes[i];

		vty_out(vty, "\nNode:\n");
		vty_out(vty, "  node_id      : %pI4\n",
			(struct in_addr *)&n->node_id);
		vty_out(vty, "  group_id     : %u\n", n->group_id);
		vty_out(vty, "  cap_flags    : 0x%" PRIx64 " (%s)\n", n->cap_flags,
			midr_caps_str((uint32_t)n->cap_flags, caps_buf,
				      sizeof(caps_buf)));
		if (n->has_transport_address)
			vty_out(vty, "  transport    : %pI4\n",
				&n->transport_address.ipaddr_v4);
		else
			vty_out(vty, "  transport    : -\n");
		vty_out(vty, "  version      : %" PRIu64 "\n", n->version);
	}

	if (snapshot.link_count)
		vty_out(vty, "\nLinks (本机出向):\n");
	for (i = 0; i < snapshot.link_count; i++) {
		const struct midr_link_update *l = &snapshot.links[i];

		vty_out(vty, "  -> %pI4  link_id=%" PRIu64 "\n",
			(struct in_addr *)&l->key.remote_node_id, l->key.link_id);
		vty_out(vty, "     rtt=%uus loss=%uppm bw=%ukbps seqno=%" PRIu64
			     " version=%" PRIu64 "\n",
			l->metrics.rtt_us, l->metrics.loss_ppm,
			l->metrics.available_bandwidth_kbps,
			l->metrics.measurement_seqno, l->version);
		vty_out(vty, "     addr %pI4 -> %pI4\n",
			&l->link_local_address.ipaddr_v4,
			&l->link_remote_address.ipaddr_v4);
	}

	midr_topology_snapshot_release(ctx, &snapshot);
	return CMD_SUCCESS;
}

DEFUN(show_midr_join,
      show_midr_join_cmd,
      "show midr join",
      SHOW_STR
      "MIDR information\n"
      "Show MIDR new-node join state\n")
{
	struct bgp *bgp = bgp_get_default();
	struct bgp_midr_nds *mi;
	struct listnode *node;
	struct midr_ctrl_pending *pend;
	char anchor_buf[32];

	if (!bgp || !bgp->midr_nds_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}
	mi = bgp->midr_nds_info;

	/* §8.32：bootstrap 候选清单（手配在前、种子在后，当前尝试标 <- trying、
	 * 已失败标 failed）。未配也继续输出：稳态的 PEER_REQUEST pending 与
	 * bootstrap 无关，卡住的请求同样要在这里可见。 */
	if (!mi->bootstrap_list || list_isempty(mi->bootstrap_list)) {
		vty_out(vty, "Bootstrap candidates : (none configured)\n");
	} else {
		struct listnode *bn;
		struct midr_bootstrap_entry *be;
		unsigned int idx = 0;

		vty_out(vty, "Bootstrap candidates :\n");
		for (ALL_LIST_ELEMENTS_RO(mi->bootstrap_list, bn, be)) {
			idx++;
			/* rid 列（批 5 R 系列）：挂靠挑台按它排环、拉黑按它认人，
			 * 排查时得看得见。池内恒非 0，故不设"-"分支。 */
			vty_out(vty, "  %u. %-15pI4 AS %-10u rid %-15pI4 [%s]%s%s\n",
				idx, &be->transport, be->asn, &be->rid,
				be->source == MIDR_BOOTSTRAP_SEED ? "seed"
								  : "manual",
				(mi->bootstrap_cur == bn) ? "  <- trying" : "",
				be->failed ? "  failed" : "");
		}
	}
	vty_out(vty, "Join state     : %s\n",
		mi->join_in_progress ? "in-progress" : "idle/done");
	vty_out(vty, "Join phase     : %s\n",
		midr_join_phase_str(mi->join_phase));
	vty_out(vty, "Local group-id : %u\n", mi->local_group_id);
	/*
	 * C-6：配置群号（`midr group-id N` 的值），与上面的运行值分离——CL 的
	 * JOIN/CREATE 只改运行值（local_group_id），配置值不动。两值不同时才
	 * 看得出"手配群号不听 CL 退群"那道守卫是否生效，故必须能单独看到。
	 * 未配时字段值为 0，直接印 0 会跟"群 0"（引导节点的合法群号）混淆，
	 * 故印 "-"。
	 */
	if (mi->config_group_id)
		vty_out(vty, "Configured gid : %u\n", mi->config_group_id);
	else
		vty_out(vty, "Configured gid : -\n");
	/*
	 * join_group_id 记的是"正在评估/加入的候选群"（RECOMMEND 阶段选定），
	 * 不是"已加入的群"——真身永远看 Local group-id。原名 "Joined group"
	 * 会误导：CL 走 CREATE 分支自建群时二者不同（候选 2 / 实建 4）。
	 * 流程结束后该字段已清零，此处只在真正在途时才显示。
	 */
	if (mi->join_group_id)
		vty_out(vty, "Target group   : %u (加入中的候选群)\n",
			mi->join_group_id);
	/*
	 * C-7：正在评估的两个备选群（ANCHOR 候选）。join 的 RECOMMEND 阶段由
	 * CL 回灌进 anchor_group_id[2]，NDS 据此向这两个群的代表发
	 * MEMBER_LIST_REQ 限时探测，探完发 ANCHOR_PROBE_DONE 交 CL 重选。
	 * ⚠ 与上面的 Target group 不是一回事：那个是已选定、正在加入的候选群；
	 * 这两个是仍在评估、尚未选中的次优群。
	 * 槽位值 0 = 该槽无候选；两槽皆空印 "(none)"——0 本身是合法群号，
	 * 不能拿它当"没有"。
	 */
	if (mi->anchor_group_id[0] && mi->anchor_group_id[1])
		snprintf(anchor_buf, sizeof(anchor_buf), "%u, %u",
			 mi->anchor_group_id[0], mi->anchor_group_id[1]);
	else if (mi->anchor_group_id[0])
		snprintf(anchor_buf, sizeof(anchor_buf), "%u",
			 mi->anchor_group_id[0]);
	else if (mi->anchor_group_id[1])
		snprintf(anchor_buf, sizeof(anchor_buf), "%u",
			 mi->anchor_group_id[1]);
	else
		snprintf(anchor_buf, sizeof(anchor_buf), "(none)");
	vty_out(vty, "Anchor cands   : %s\n", anchor_buf);

	/*
	 * C-10：改实时统计（原先直接印 mi->join_members）。那是个死数——唯一
	 * 写入点在 I-7 的 JOIN 分支，只在 JOIN 成功那一刻写一次，之后 LEAVE /
	 * 换组 / CREATE 都不回写（实测：拆掉两台同群会话后实际只剩 3 条，它
	 * 仍显示 5）。join_members 字段本身保留，只是不再拿它当显示源。
	 *
	 * 三级判定：
	 *  ① join 进行中 → 不给数字。learn_member 会在 join 那 60s 给候选群
	 *     成员提前置 is_adjacent（为让 CL 统计得到，bgp_midr_ctrl.c 中有意
	 *     设计），此刻数出来的既不是"已入群成员"也不是"已建会话"，任何
	 *     数字都似是而非。
	 *  ② 运行群号为 0（未入群）→ 无"同群"可言，印 "-"。
	 *  ③ 稳态 → 实时遍历节点表：同群 ∧ 已邻接 ∧ 非自己。
	 * 不复用 midr_group_members()：它不看 is_adjacent，复用还得再过滤一遍。
	 */
	if (mi->join_phase != MIDR_JOIN_IDLE) {
		vty_out(vty, "Members linked : (加入中，请稍候)\n");
	} else if (!mi->local_group_id) {
		vty_out(vty, "Members linked : -\n");
	} else {
		struct midr_node_entry *ne;
		unsigned int linked = 0;

		frr_each (midr_node_hash, &mi->global_view->nodes, ne) {
			if (ne->is_self || !ne->is_adjacent)
				continue;
			if (ne->group_id == mi->local_group_id)
				linked++;
		}
		vty_out(vty, "Members linked : %u (同群且已邻接)\n", linked);
	}

	vty_out(vty, "Pending ctrl requests:\n");
	if (!mi->ctrl_pending || list_isempty(mi->ctrl_pending)) {
		vty_out(vty, "  (none)\n");
		return CMD_SUCCESS;
	}
	for (ALL_LIST_ELEMENTS_RO(mi->ctrl_pending, node, pend)) {
		struct prefix p = {};

		p.family = AF_INET;
		p.prefixlen = IPV4_MAX_BITLEN;
		p.u.prefix4 = pend->target_transport;
		vty_out(vty, "  %-16s -> %-15pI4  retries_left=%d%s\n",
			midr_ctrl_msg_type_str(pend->type),
			&pend->target_transport, pend->retries_left,
			midr_bgp_rib_covers(bgp, &p) ? ""
						     : "  [no BGP route]");
	}
	return CMD_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* midr help [plain]                                                   */
/*                                                                     */
/* 集中列出所有 MIDR 命令及其功能,仿 SYNLS -h 的分组样式。            */
/* 默认 ANSI 彩色;带 plain -> 纯文本(开发期两套并存)。             */
/* ------------------------------------------------------------------ */

static void bgp_midr_print_help(struct vty *vty, bool color)
{
	/* ANSI 转义码:彩色时生效,纯文本时全部为空串。 */
	const char *C_TITLE = color ? "\033[1;36m" : ""; /* 标题:青色加粗   */
	const char *C_SEC = color ? "\033[1;33m" : "";	 /* 分组:黄色加粗   */
	const char *C_CMD = color ? "\033[32m" : "";	 /* 命令名:绿色     */
	const char *C_RST = color ? "\033[0m" : "";	 /* 复位            */

	/* 标题 */
	vty_out(vty, "%s📖 MIDR 命令一览:%s\n\n", C_TITLE, C_RST);

	/*
	 * 配置命令组:都需先进入配置态——`configure terminal` → `router bgp <ASN>`
	 * (命令缩进 4 空格,说明另起一行缩进 8 空格)
	 */
	vty_out(vty, "%s配置命令(需先进入 `router bgp <ASN>` 配置态):%s\n", C_SEC,
		C_RST);
	vty_out(vty, "    %smidr group-id <0-4294967295>%s\n", C_CMD, C_RST);
	vty_out(vty, "        设置本节点 MIDR 群组 ID\n");
	vty_out(vty, "    %smidr transport-address <IP>%s\n", C_CMD, C_RST);
	vty_out(vty, "        设置本节点可达地址(TLV 1188,实际填 loopback)\n");
	vty_out(vty, "    %sno midr transport-address [<IP>]%s\n", C_CMD, C_RST);
	vty_out(vty, "        清除本节点的 transport-address\n");
	vty_out(vty, "    %smidr session <IP> remote-as <ASN>%s\n", C_CMD, C_RST);
	vty_out(vty,
		"        手动建一条 MIDR overlay 会话(逃生舱/调试用;正常加群走 midr group-id / midr bootstrap)\n");
	vty_out(vty,
		"        旧名 midr neighbor 过渡期仍可用(不再出现在补全里)\n");
	vty_out(vty, "    %sno midr session <IP>%s\n", C_CMD, C_RST);
	vty_out(vty,
		"        拆一条 MIDR 会话并清账，且持久排除该地址(不会被下次自动重收敛连回来；"
		"带守卫:运维会话拒删；重敲 midr session <IP> remote-as <ASN> 可解除排除；旧名 no midr neighbor 同上)\n");
	vty_out(vty, "    %smidr role bootstrap%s\n", C_CMD, C_RST);
	vty_out(vty, "        将本节点设为 bootstrap(引导)节点\n");
	vty_out(vty, "    %sno midr role bootstrap%s\n", C_CMD, C_RST);
	vty_out(vty, "        取消本节点的 bootstrap 角色\n");
	vty_out(vty, "    %smidr role group-rep%s\n", C_CMD, C_RST);
	vty_out(vty, "        将本节点设为群代表(group-rep)\n");
	vty_out(vty, "    %sno midr role group-rep%s\n", C_CMD, C_RST);
	vty_out(vty, "        取消本节点的群代表角色\n");
	vty_out(vty, "    %smidr bootstrap <IP> remote-as <ASN>%s\n", C_CMD,
		C_RST);
	vty_out(vty,
		"        作为新节点,经 bootstrap 节点入网(命令 O;可多次配置多个候选,失败自动 failover)\n");
	vty_out(vty, "    %sno midr bootstrap <IP>%s\n", C_CMD, C_RST);
	vty_out(vty,
		"        删除一个 bootstrap 候选(正在尝试的被删则顺移到下一候选)\n");
	vty_out(vty, "    %sno midr bootstrap%s\n", C_CMD, C_RST);
	vty_out(vty,
		"        清空所有 bootstrap 候选并中止在途入网\n");
	vty_out(vty, "    %smidr shutdown%s\n", C_CMD, C_RST);
	vty_out(vty, "        优雅下线本节点(撤销自通告,抑制 keepalive)\n");
	vty_out(vty, "    %sno midr shutdown%s\n", C_CMD, C_RST);
	vty_out(vty, "        重新上线(恢复自通告)\n");

	/* 查看 / 帮助命令组:vtysh 顶层(enable/view)直接可用 */
	vty_out(vty, "\n%s查看 / 帮助命令(vtysh 顶层直接可用,无需进配置态):%s\n",
		C_SEC, C_RST);
	vty_out(vty, "    %sshow midr self%s\n", C_CMD, C_RST);
	vty_out(vty,
		"        显示本机自身的 MIDR 状态(身份/群号/能力位/导出开关;直接读运行态,不经节点表)\n");
	vty_out(vty, "    %sshow midr nodes%s\n", C_CMD, C_RST);
	vty_out(vty,
		"        显示 MIDR 节点表(由 BGP-LS Node NLRI 学到的**其它**节点;本机自己看上一条)\n");
	vty_out(vty, "    %sshow midr reps%s\n", C_CMD, C_RST);
	vty_out(vty,
		"        列全网群代表(按 GROUP_REP 能力位过滤节点表) + 本地 rep 目录\n");
	vty_out(vty, "    %sshow midr bootstraps%s\n", C_CMD, C_RST);
	vty_out(vty, "        列全网引导节点(按 BOOTSTRAP 能力位过滤节点表)\n");
	vty_out(vty, "    %sshow midr bootstrap-seeds%s\n", C_CMD, C_RST);
	vty_out(vty,
		"        列持久化种子表(bgpd.db,重启自举的第一跳清单;与上一条不同:那个是运行时视图,这个是跨重启记忆)\n");
	vty_out(vty, "    %sshow midr neighbors%s\n", C_CMD, C_RST);
	vty_out(vty, "        显示已建立的 MIDR(BGP-LS)直连会话\n");
	vty_out(vty, "    %sshow midr join%s\n", C_CMD, C_RST);
	vty_out(vty, "        显示新节点入网状态\n");
	vty_out(vty, "    %smidr help [plain]%s\n", C_CMD, C_RST);
	vty_out(vty, "        显示本帮助(默认彩色,附加 plain 显示纯文本版)\n");

	/*
	 * ASN 相关:队友 wlk 交付的 IP→ASN / Tier-1 诊断工具箱,命名空间 midr_*
	 * (非 bgp_midr_*),独立于 NDS/PM/CL 主链。配置命令在全局 config 态,
	 * 不在 `router bgp` 下。
	 */
	vty_out(vty,
		"\n%sASN 相关(IP→ASN 与 Tier-1 路径判定,独立诊断工具箱):%s\n",
		C_SEC, C_RST);
	vty_out(vty,
		"    %smidr ip2asn file WORD [discard-runtime-updates]%s  (全局 config 态)\n",
		C_CMD, C_RST);
	vty_out(vty,
		"        加载 IP→ASN 快照文件(CAIDA prefix2as 风格;库有增量脏数据时须带 discard)\n");
	vty_out(vty,
		"    %sno midr ip2asn file [discard-runtime-updates]%s  (全局 config 态)\n",
		C_CMD, C_RST);
	vty_out(vty, "        清空已加载的 IP→ASN 快照\n");
	vty_out(vty,
		"    %smidr ip2asn update file WORD [validate-only]%s  (enable 态)\n",
		C_CMD, C_RST);
	vty_out(vty,
		"        事务化增量更新快照(ADD/REPLACE/DELETE,按 generation CAS 校验,可干跑)\n");
	vty_out(vty, "    %sshow midr ip2asn%s\n", C_CMD, C_RST);
	vty_out(vty,
		"        显示 IP→ASN 快照库状态(条数 / generation / dirty / 更新审计)\n");
	vty_out(vty, "    %sshow midr ip2asn <IP>%s\n", C_CMD, C_RST);
	vty_out(vty, "        查单个 IP 属于哪个 ASN(最长前缀匹配)\n");
	vty_out(vty, "    %sshow midr traceroute <IP> [refresh]%s\n", C_CMD,
		C_RST);
	vty_out(vty,
		"        异步 traceroute + 逐跳 ASN:首问回 job id,稍后再问取结果;refresh 强制重测\n");
	vty_out(vty, "    %sshow midr traceroute job WORD%s\n", C_CMD, C_RST);
	vty_out(vty, "        按 job id 轮询在途/近期 traceroute 结果\n");
	vty_out(vty, "    %sshow midr traceroute scheduler%s\n", C_CMD, C_RST);
	vty_out(vty,
		"        调度器状态与统计(本机不支持时 Ready 为 no,需 glibc>=2.34)\n");
	vty_out(vty, "    %sshow midr tier1 <IP> [refresh]%s\n", C_CMD, C_RST);
	vty_out(vty,
		"        判到目标的路径是否过 Tier-1(异步 traceroute,轮询语义同上)\n");
	vty_out(vty, "    %sshow midr tier1 job WORD%s\n", C_CMD, C_RST);
	vty_out(vty, "        按 job id 轮询 Tier-1 判定结果\n");
	vty_out(vty,
		"    %sshow midr tier1 <IP> observed-as-path ASN...%s\n", C_CMD,
		C_RST);
	vty_out(vty,
		"        手动喂 ASN 序列判是否过 Tier-1(不触发 traceroute)\n");
	vty_out(vty, "    %sclear midr traceroute cache [<IP>]%s  (enable 态)\n",
		C_CMD, C_RST);
	vty_out(vty, "        清 traceroute 结果缓存(全部或单目标)\n");
	vty_out(vty,
		"    %smidr traceroute concurrency|queue-limit|queue-timeout|timeout|cache ...%s  (全局 config 态)\n",
		C_CMD, C_RST);
	vty_out(vty,
		"        调度器 7 项调参(并发/队列/超时/缓存 TTL 与容量,写 running-config)\n");
}

DEFUN(midr_help,
      midr_help_cmd,
      "midr help [plain]",
      "MIDR configuration\n"
      "Show all MIDR commands and their functions\n"
      "Use plain (no ANSI color) output\n")
{
	/* 默认彩色;附加 plain token 时输出纯文本。 */
	bool color = !(argc > 2 && strmatch(argv[argc - 1]->text, "plain"));

	bgp_midr_print_help(vty, color);
	return CMD_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

void bgp_midr_nds_vty_init(void)
{
	install_element(BGP_NODE, &midr_group_id_cmd);
	install_element(BGP_NODE, &midr_session_cmd);
	install_element(BGP_NODE, &no_midr_session_cmd);
	/* 旧名隐藏别名（Q5 过渡） */
	install_element(BGP_NODE, &midr_neighbor_cmd);
	install_element(BGP_NODE, &no_midr_neighbor_cmd);
	install_element(BGP_NODE, &midr_role_bootstrap_cmd);
	install_element(BGP_NODE, &no_midr_role_bootstrap_cmd);
	install_element(BGP_NODE, &midr_role_group_rep_cmd);
	install_element(BGP_NODE, &no_midr_role_group_rep_cmd);
	install_element(BGP_NODE, &midr_bootstrap_cmd);
	install_element(BGP_NODE, &no_midr_bootstrap_cmd);
	install_element(BGP_NODE, &midr_transport_address_cmd);
	install_element(BGP_NODE, &no_midr_transport_address_cmd);
	install_element(BGP_NODE, &midr_shutdown_cmd);
	install_element(BGP_NODE, &no_midr_shutdown_cmd);
	/* 隐藏，批 6 ④ 保活验证专用；随对接轮 4/5 删定时器时一并删。 */
	install_element(BGP_NODE, &midr_help_cmd);
	/* `midr help` 也在 vtysh 顶层可用(enable/view),无需进配置态。
	 * 只装 VIEW_NODE 即可——lib/command.c 的 install_element 对 VIEW_NODE
	 * 会自动连带装进 ENABLE_NODE；再显式装一次会触发启动期
	 * "duplicate install_element call?" 告警。 */
	install_element(VIEW_NODE, &midr_help_cmd);
	install_element(VIEW_NODE, &show_midr_self_cmd);
	install_element(VIEW_NODE, &show_midr_group2_cmd);
	install_element(VIEW_NODE, &show_midr_nodes_cmd);
	install_element(VIEW_NODE, &show_midr_reps_cmd);
	install_element(VIEW_NODE, &show_midr_bootstraps_cmd);
	install_element(VIEW_NODE, &show_midr_bootstrap_seeds_cmd);
	install_element(VIEW_NODE, &show_midr_neighbors_cmd);
	install_element(VIEW_NODE, &show_midr_join_cmd);
	/* 第二组视角的 snapshot 窗口（长期诊断命令，见函数头注释）。 */
	install_element(VIEW_NODE, &show_midr_group2_snapshot_cmd);
	install_element(VIEW_NODE, &show_midr_group2_remote_cmd);
}
