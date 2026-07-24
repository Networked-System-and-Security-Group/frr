// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR VTY commands
 *
 * Provides CLI for MIDR configuration and diagnostics:
 *   midr group-id <N>
 *   midr session <IP> remote-as <ASN>   （旧名 midr neighbor，隐藏别名过渡）
 *   no midr session <IP>                （旧名 no midr neighbor，同上）
 *   midr help [plain]
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
#include "bgpd/bgp_midr.h"
#include "bgpd/bgp_midr_ctrl.h"
#include "bgpd/bgp_midr_pm.h"
#include "bgpd/bgp_midr_vty.h"
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

	frr_each (midr_node_hash, &bgp->midr_info->global_view->nodes, entry)
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

	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	uint32_t gid = (uint32_t)atol(argv[2]->arg);
	uint32_t old_gid = bgp->midr_info->local_group_id;

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

	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	if (str2sockunion(argv[2]->arg, &su) < 0) {
		vty_out(vty, "%% Invalid IP address: %s\n", argv[2]->arg);
		return CMD_WARNING_CONFIG_FAILED;
	}

	/*
	 * 手工敲这条命令是运维显式意图，必须能覆盖此前的 `no midr session`
	 * 排除——否则下一次自动重收敛（connect_group）又会被排除名单挡住，
	 * 运维刚建好的会话反而在下一次换组/退群时消失，从"手工建连"退化成
	 * "手工建一次性连接"。
	 */
	midr_nds_session_exclude_del(bgp, su.sin.sin_addr);

	/*
	 * S3 撞车守卫：必须拦在 peer_remote_as 之前——它对已存在 peer 会直接复用，
	 * ASN 不同还静默改 AS（bgpd.c peer_as_change），随后整形把活的 underlay
	 * 会话就地改造成 overlay（撤 IPv4、multihop），砸掉转发面（洞 #3）。
	 */
	peer = peer_lookup(bgp, &su);
	if (peer) {
		if (midr_nds_peer_is_overlay(peer)) {
			/* 已是 MIDR 自建会话：幂等重整形（同值短路、不 reset），不改 AS。 */
			midr_nds_ctrl_setup_overlay_peer(bgp, peer);
			vty_out(vty,
				"MIDR session %s 已存在（MIDR overlay 会话），已确保形态一致\n",
				argv[2]->arg);
			return CMD_SUCCESS;
		}
		/* 运维会话占用该地址：拒绝接管（运维优先，绝不动它）。 */
		if (peer->afc[AFI_BGP_LS][SAFI_BGP_LS])
			vty_out(vty,
				"%% %s 已有运维会话且已激活 link-state，可直接承载 MIDR 拓扑，无需另建\n",
				argv[2]->arg);
		else
			vty_out(vty,
				"%% %s 已被运维会话占用；拒绝接管（避免砸转发面）。请在原生配置为该邻居激活 link-state 复用现有会话，或改用该节点其它地址\n",
				argv[2]->arg);
		return CMD_WARNING_CONFIG_FAILED;
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

	vty_out(vty, "MIDR session %s AS %u created\n", argv[2]->arg, asn);
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
	 * （删了会砸转发面）。判据 = 标记 ∧ 只载 BGP-LS 签名。
	 */
	if (!midr_nds_peer_is_overlay(peer)) {
		vty_out(vty,
			"%% %s 是运维配置的会话（非 MIDR overlay），拒绝删除；如确需删除请用原生命令 no neighbor %s\n",
			argv[3]->arg, argv[3]->arg);
		return CMD_WARNING_CONFIG_FAILED;
	}

	/*
	 * S6 清账：按地址反查节点表条目，查到走 detach 全套（停探 + 删 link +
	 * 清 is_adjacent + 拆会话）；查不到（该地址不对应已知节点）退化为只拆会话。
	 */
	if (!midr_nds_detach_by_locator(bgp, su.sin.sin_addr))
		peer_delete(peer);

	/*
	 * 持久排除而非临时拔线——运维显式敲这条命令表达"不想再跟这个节点做
	 * 邻居"，不该被下一次自动重收敛（换组/退群时的 connect_group）悄悄
	 * 连回来。写名单在 detach 之后，不影响本次清账本身。
	 */
	midr_nds_session_exclude_add(bgp, su.sin.sin_addr);

	vty_out(vty,
		"MIDR session %s removed and persistently excluded (use `midr session %s remote-as <ASN>` to reconnect)\n",
		argv[3]->arg, argv[3]->arg);
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

	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	midr_nds_set_capability(bgp, bgp->midr_info->local_capabilities |
					     MIDR_CAP_BOOTSTRAP);
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

	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	midr_nds_set_capability(bgp, bgp->midr_info->local_capabilities &
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

	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	midr_nds_set_capability(bgp, bgp->midr_info->local_capabilities |
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

	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	midr_nds_set_capability(bgp, bgp->midr_info->local_capabilities &
					     ~MIDR_CAP_GROUP_REP);
	vty_out(vty, "MIDR role group-rep cleared\n");
	return CMD_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* midr rep group <GID> transport <IP> remote-as <ASN>                 */
/* (bootstrap-side static representative directory)                     */
/* ------------------------------------------------------------------ */

DEFUN(midr_rep,
      midr_rep_cmd,
      "midr rep group (1-4294967295) transport A.B.C.D remote-as (1-4294967295)",
      "MIDR configuration\n"
      "Register a group representative (bootstrap directory)\n"
      "Group\n"
      "Group ID value\n"
      "Representative transport address\n"
      "Representative IPv4 address\n"
      "Remote AS\n"
      "AS number\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);
	uint32_t gid = (uint32_t)atol(argv[3]->arg);
	struct in_addr rep_transport;
	as_t asn = (as_t)atol(argv[7]->arg);

	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}
	if (inet_pton(AF_INET, argv[5]->arg, &rep_transport) != 1) {
		vty_out(vty, "%% Invalid transport address: %s\n", argv[5]->arg);
		return CMD_WARNING_CONFIG_FAILED;
	}

	midr_rep_dir_add(bgp, gid, rep_transport, asn,
			 (struct in_addr){ .s_addr = INADDR_ANY });
	vty_out(vty, "MIDR rep for group %u at %s AS %u added\n", gid,
		argv[5]->arg, asn);
	return CMD_SUCCESS;
}

DEFUN(no_midr_rep,
      no_midr_rep_cmd,
      "no midr rep group (1-4294967295) transport A.B.C.D",
      NO_STR
      "MIDR configuration\n"
      "Register a group representative (bootstrap directory)\n"
      "Group\n"
      "Group ID value\n"
      "Representative transport address\n"
      "Representative IPv4 address\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);
	uint32_t gid = (uint32_t)atol(argv[4]->arg);
	struct in_addr rep_transport;

	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}
	if (inet_pton(AF_INET, argv[6]->arg, &rep_transport) != 1) {
		vty_out(vty, "%% Invalid transport address: %s\n", argv[6]->arg);
		return CMD_WARNING_CONFIG_FAILED;
	}

	if (!midr_rep_dir_del(bgp, gid, rep_transport)) {
		vty_out(vty, "%% No such rep (group %u, %s)\n", gid,
			argv[6]->arg);
		return CMD_WARNING;
	}
	vty_out(vty, "MIDR rep for group %u at %s removed\n", gid, argv[6]->arg);
	return CMD_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* midr bootstrap <IP> remote-as <ASN>  (command O, run on a new node) */
/* ------------------------------------------------------------------ */

DEFUN(midr_bootstrap,
      midr_bootstrap_cmd,
      "midr bootstrap A.B.C.D remote-as (1-4294967295)",
      "MIDR configuration\n"
      "Join the network via a bootstrap node\n"
      "Bootstrap node IP address\n"
      "Remote AS\n"
      "AS number\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);
	union sockunion su;
	as_t asn = (as_t)atol(argv[4]->arg);

	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	if (str2sockunion(argv[2]->arg, &su) < 0) {
		vty_out(vty, "%% Invalid IP address: %s\n", argv[2]->arg);
		return CMD_WARNING_CONFIG_FAILED;
	}

	vty_out(vty, "MIDR: joining via bootstrap %s AS %u ...\n", argv[2]->arg,
		asn);
	midr_join_via_bootstrap(bgp, &su, asn);
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

	if (!bgp->midr_info) {
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

	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	if (inet_pton(AF_INET, argv[2]->arg, &addr) != 1) {
		vty_out(vty, "%% Invalid IPv4 address: %s\n", argv[2]->arg);
		return CMD_WARNING_CONFIG_FAILED;
	}

	bgp->midr_info->local_transport_addr = addr;
	bgp->midr_info->transport_addr_set = true;
	midr_propagate_self(bgp, MIDR_ORIGIN_TRANSPORT_UPDATE); /* +TLV 1188 */

	/* Open PM socket now that we have an address to bind to.  midr_pm_init()
	 * runs before the config file is read, so the socket is deferred until
	 * this command is processed. */
	midr_pm_on_transport_addr_set(bgp);

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

	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	bgp->midr_info->transport_addr_set = false;
	bgp->midr_info->local_transport_addr.s_addr = INADDR_ANY;
	midr_propagate_self(bgp, MIDR_ORIGIN_TRANSPORT_UPDATE); /* -TLV 1188 */

	vty_out(vty, "MIDR transport-address cleared\n");
	return CMD_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* midr shutdown   (graceful departure: withdraw self + stop keepalive) */
/* ------------------------------------------------------------------ */

DEFUN(midr_shutdown,
      midr_shutdown_cmd,
      "midr shutdown",
      "MIDR configuration\n"
      "Gracefully leave MIDR: withdraw our Node NLRI and stop advertising\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);

	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	bgp->midr_info->shutdown = true; /* keepalive will stop re-originating */
	midr_propagate_self(bgp, MIDR_ORIGIN_LEAVE); /* withdraw: we are leaving */

	vty_out(vty, "MIDR: gracefully shut down (Node NLRI withdrawn)\n");
	return CMD_SUCCESS;
}

DEFUN(no_midr_shutdown,
      no_midr_shutdown_cmd,
      "no midr shutdown",
      NO_STR
      "MIDR configuration\n"
      "Rejoin MIDR: resume advertising our Node NLRI\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);

	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	bgp->midr_info->shutdown = false;
	midr_propagate_self(bgp, MIDR_ORIGIN_REJOIN); /* re-announce ourselves */

	vty_out(vty, "MIDR: resumed (Node NLRI re-originated)\n");
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
static void midr_show_node_table(struct vty *vty, struct bgp *bgp,
				 uint32_t required_caps)
{
	struct midr_node_entry *entry;
	char caps_buf[64];
	time_t now = monotime(NULL);

	vty_out(vty, "%-18s %-18s %-8s %-10s %-8s %s\n",
		"Router-ID", "Transport-Addr", "ASN", "Group-ID", "Status",
		"Capabilities");
	vty_out(vty, "%-18s %-18s %-8s %-10s %-8s %s\n",
		"------------------", "------------------", "--------",
		"----------", "--------", "------------");

	frr_each (midr_node_hash, &bgp->midr_info->global_view->nodes, entry) {
		bool active = entry->is_self ||
			      (now - entry->last_seen) <= MIDR_NODE_EXPIRE_TIME;
		char taddr[INET_ADDRSTRLEN];

		if ((entry->capabilities & required_caps) != required_caps)
			continue;

		if (entry->has_transport_addr)
			inet_ntop(AF_INET, &entry->transport_addr, taddr,
				  sizeof(taddr));
		else
			snprintf(taddr, sizeof(taddr), "-");

		vty_out(vty, "%-18pI4 %-18s %-8u %-10u %-8s %s\n",
			&entry->node_id.u.prefix4,
			taddr,
			entry->asn,
			entry->group_id,
			active ? "active" : "expired",
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

	if (!bgp->midr_info) {
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
	struct listnode *node;
	struct midr_rep_entry *r;

	if (!bgp) {
		vty_out(vty, "%% No BGP instance found\n");
		return CMD_WARNING;
	}

	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	midr_show_node_table(vty, bgp, MIDR_CAP_GROUP_REP);

	/* 本地 rep_dir (手配/学来, REP_LIST 应答排前的来源) 恒显示——一眼区
	 * 分"全网视图"(上表, 按位推导)与"本地目录"(本段); 手配/学来的区分标
	 * 记是未来项 F (from_config)。 */
	vty_out(vty, "\nLocal rep directory (rep_dir, served first in REP_LIST):\n");
	if (list_isempty(bgp->midr_info->rep_dir))
		vty_out(vty, "  (none)\n");
	else
		for (ALL_LIST_ELEMENTS_RO(bgp->midr_info->rep_dir, node, r)) {
			char rid_buf[INET_ADDRSTRLEN] = "-";

			if (r->rep_rid.s_addr != INADDR_ANY)
				inet_ntop(AF_INET, &r->rep_rid, rid_buf,
					  sizeof(rid_buf));
			vty_out(vty, "  group %u -> %pI4 (AS %u, rid %s)\n",
				r->group_id, &r->rep_transport,
				(unsigned int)r->rep_asn, rid_buf);
		}

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

	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	midr_show_node_table(vty, bgp, MIDR_CAP_BOOTSTRAP);

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

	if (!bgp) {
		vty_out(vty, "%% No BGP instance found\n");
		return CMD_WARNING;
	}

	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	nodes = &bgp->midr_info->global_view->nodes;

	vty_out(vty, "%-18s %-8s %-14s %-10s %s\n",
		"Neighbor", "ASN", "State", "Group-ID", "Capabilities");
	vty_out(vty, "%-18s %-8s %-14s %-10s %s\n",
		"------------------", "--------", "--------------",
		"----------", "------------");

	for (ALL_LIST_ELEMENTS_RO(bgp->peer, node, peer)) {
		struct midr_node_entry *ne = NULL;
		const char *nbr_disp;
		char taddr[INET_ADDRSTRLEN];
		char state_buf[32];

		/* Only peers carrying the BGP-LS AF are MIDR sessions. */
		if (!peer->afc[AFI_BGP_LS][SAFI_BGP_LS])
			continue;

		any = true;

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

		vty_out(vty, "%-18s %-8u %-14s ", nbr_disp, peer->as,
			state_buf);

		if (ne)
			vty_out(vty, "%-10u %s\n", ne->group_id,
				midr_caps_str(ne->capabilities, caps_buf,
					      sizeof(caps_buf)));
		else
			vty_out(vty, "%-10s %s\n", "n/a", "n/a");
	}

	if (!any)
		vty_out(vty, "%% No MIDR (BGP-LS) sessions\n");

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

DEFUN(show_midr_join,
      show_midr_join_cmd,
      "show midr join",
      SHOW_STR
      "MIDR information\n"
      "Show MIDR new-node join state\n")
{
	struct bgp *bgp = bgp_get_default();
	struct bgp_midr *mi;
	struct listnode *node;
	struct midr_ctrl_pending *pend;

	if (!bgp || !bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}
	mi = bgp->midr_info;

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
			vty_out(vty, "  %u. %-15pI4 AS %-10u [%s]%s%s\n", idx,
				&be->transport, be->asn,
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
	 * join_group_id 记的是"正在评估/加入的候选群"（RECOMMEND 阶段选定），
	 * 不是"已加入的群"——真身永远看 Local group-id。原名 "Joined group"
	 * 会误导：CL 走 CREATE 分支自建群时二者不同（候选 2 / 实建 4）。
	 * 流程结束后该字段已清零，此处只在真正在途时才显示。
	 */
	if (mi->join_group_id)
		vty_out(vty, "Target group   : %u (加入中的候选群)\n",
			mi->join_group_id);
	vty_out(vty, "Members linked : %u\n", mi->join_members);

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
	vty_out(vty,
		"    %smidr rep group <GID> transport <IP> remote-as <ASN>%s\n",
		C_CMD, C_RST);
	vty_out(vty,
		"        引导目录:登记某群的群代表(可达地址 + ASN),供新节点发现\n");
	vty_out(vty, "    %sno midr rep group <GID> transport <IP>%s\n", C_CMD,
		C_RST);
	vty_out(vty, "        从引导目录删除指定群代表\n");
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
	vty_out(vty, "    %sshow midr nodes%s\n", C_CMD, C_RST);
	vty_out(vty,
		"        显示 MIDR 节点表(由 BGP-LS Node NLRI 学到的所有节点)\n");
	vty_out(vty, "    %sshow midr reps%s\n", C_CMD, C_RST);
	vty_out(vty,
		"        列全网群代表(按 GROUP_REP 能力位过滤节点表) + 本地 rep 目录\n");
	vty_out(vty, "    %sshow midr bootstraps%s\n", C_CMD, C_RST);
	vty_out(vty, "        列全网引导节点(按 BOOTSTRAP 能力位过滤节点表)\n");
	vty_out(vty, "    %sshow midr neighbors%s\n", C_CMD, C_RST);
	vty_out(vty, "        显示已建立的 MIDR(BGP-LS)直连会话\n");
	vty_out(vty, "    %sshow midr join%s\n", C_CMD, C_RST);
	vty_out(vty, "        显示新节点入网状态\n");
	vty_out(vty, "    %smidr help [plain]%s\n", C_CMD, C_RST);
	vty_out(vty, "        显示本帮助(默认彩色,附加 plain 显示纯文本版)\n");
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

void bgp_midr_vty_init(void)
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
	install_element(BGP_NODE, &midr_rep_cmd);
	install_element(BGP_NODE, &no_midr_rep_cmd);
	install_element(BGP_NODE, &midr_bootstrap_cmd);
	install_element(BGP_NODE, &no_midr_bootstrap_cmd);
	install_element(BGP_NODE, &midr_transport_address_cmd);
	install_element(BGP_NODE, &no_midr_transport_address_cmd);
	install_element(BGP_NODE, &midr_shutdown_cmd);
	install_element(BGP_NODE, &no_midr_shutdown_cmd);
	install_element(BGP_NODE, &midr_help_cmd);
	/* `midr help` 也在 vtysh 顶层可用(enable/view),无需进配置态。
	 * 只装 VIEW_NODE 即可——lib/command.c 的 install_element 对 VIEW_NODE
	 * 会自动连带装进 ENABLE_NODE；再显式装一次会触发启动期
	 * "duplicate install_element call?" 告警。 */
	install_element(VIEW_NODE, &midr_help_cmd);
	install_element(VIEW_NODE, &show_midr_nodes_cmd);
	install_element(VIEW_NODE, &show_midr_reps_cmd);
	install_element(VIEW_NODE, &show_midr_bootstraps_cmd);
	install_element(VIEW_NODE, &show_midr_neighbors_cmd);
	install_element(VIEW_NODE, &show_midr_join_cmd);
}
