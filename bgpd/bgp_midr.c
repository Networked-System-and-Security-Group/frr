// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR core (NDS - Neighbor Discovery & Selection).
 *
 * Owns bgp->midr_info: the global view (node/link/group tables), the
 * node-table keepalive/expire timers (migrated from the old
 * bgp_midr_node.c), and the internal interfaces I-3 / I-5 / I-7 plus the
 * BGP-LS export E-1.
 *
 * Skeleton stage: node-table + timers are fully wired; PM/CL/E-1 bodies
 * are stubs that maintain just enough state to keep the call chain alive.
 */

#include <zebra.h>

#include <math.h> /* fabs()：§8.21 去抖的丢包率绝对差比较 */

#include "memory.h"
#include "frrevent.h"
#include "monotime.h"
#include "prefix.h"
#include "linklist.h"
#include "log.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_ls.h"
#include "bgpd/bgp_ls_nlri.h"
#include "bgpd/bgp_midr.h"
#include "bgpd/bgp_midr_ctrl.h"
#include "bgpd/bgp_midr_cl.h"
#include "bgpd/bgp_midr_pm.h"
#include "bgpd/bgp_midr_tlv.h"
#include "bgpd/bgp_midr_store.h"
#include "bgpd/bgp_debug.h"

/* §8.31 bootstrap 种子持久化参数 */
#define MIDR_STORE_SEED_KEEP	  32 /* 种子库最多留几条（防膨胀，prune 上限） */
#define MIDR_BOOTSTRAP_SELF_BOOT_SECS 10 /* 启动后延迟自举的等待秒数（等网络就绪） */

DEFINE_MTYPE_STATIC(BGPD, BGP_MIDR, "MIDR instance");
DEFINE_MTYPE_STATIC(BGPD, MIDR_GLOBAL_VIEW, "MIDR global view");
DEFINE_MTYPE_STATIC(BGPD, MIDR_NODE_ENTRY, "MIDR node entry");
DEFINE_MTYPE_STATIC(BGPD, MIDR_LINK_ENTRY, "MIDR link entry");
DEFINE_MTYPE_STATIC(BGPD, MIDR_REP_ENTRY, "MIDR rep directory entry");
DEFINE_MTYPE_STATIC(BGPD, MIDR_BOOTSTRAP_ENTRY, "MIDR bootstrap candidate");

/* ===========================================================================
 * Hash helpers (node table is keyed by node_id prefix)
 * =========================================================================*/

unsigned int midr_node_hash_key(const struct midr_node_entry *e)
{
	return prefix_hash_key(&e->node_id);
}

int midr_node_hash_cmp(const struct midr_node_entry *a,
		       const struct midr_node_entry *b)
{
	return prefix_cmp(&a->node_id, &b->node_id);
}

/* ===========================================================================
 * Small helpers
 * =========================================================================*/

/* Build a host prefix (AF_INET/32) from a bare IPv4 address. */
static void midr_prefix_from_in_addr(struct prefix *p, struct in_addr addr)
{
	memset(p, 0, sizeof(*p));
	p->family = AF_INET;
	p->prefixlen = IPV4_MAX_BITLEN;
	p->u.prefix4 = addr;
}

static bool midr_prefix_is_self(struct bgp *bgp, const struct prefix *p)
{
	return p->family == AF_INET &&
	       IPV4_ADDR_SAME(&p->u.prefix4, &bgp->router_id);
}

/*
 * Effective locator for an entry: the real reachable address used to peer
 * with / probe / display the node.  Uses the advertised transport address
 * (TLV 1188) when present, otherwise falls back to node_id (router-id).
 */
void midr_node_get_locator(const struct midr_node_entry *e, struct prefix *out)
{
	if (e->has_transport_addr)
		midr_prefix_from_in_addr(out, e->transport_addr);
	else
		prefix_copy(out, &e->node_id);
}

/*
 * §8.31：若该节点是"可作种子的引导节点"（BOOTSTRAP 位 + transport + asn 齐备），
 * 把它的 transport 地址写入种子库（有则刷 last_seen、无则插入）。非引导节点、
 * self、字段不全者一律跳过。调用点：on_node_nlri 的变化分支（学到/变了才写，
 * 不是每 5s keepalive）、periodic_sync 定时器（每 30s 顺路刷活性）。
 */
static void midr_maybe_save_bootstrap_seed(const struct midr_node_entry *entry)
{
	char buf[INET_ADDRSTRLEN];

	if (entry->is_self)
		return;
	if (!(entry->capabilities & MIDR_CAP_BOOTSTRAP))
		return;
	if (!entry->has_transport_addr || entry->asn == 0)
		return;

	snprintfrr(buf, sizeof(buf), "%pI4", &entry->transport_addr);
	midr_store_seed_save(buf, (uint32_t)entry->asn, monotime(NULL));
}

/* ===========================================================================
 * Global view lifecycle
 * =========================================================================*/

static struct midr_global_view *midr_global_view_new(void)
{
	struct midr_global_view *gv;

	gv = XCALLOC(MTYPE_MIDR_GLOBAL_VIEW, sizeof(*gv));
	midr_node_hash_init(&gv->nodes);
	gv->links = list_new();
	gv->groups = NULL; /* skeleton: CL not wired yet */

	return gv;
}

static void midr_global_view_free(struct midr_global_view *gv)
{
	struct midr_node_entry *entry;
	struct listnode *node, *nnode;
	struct midr_link_entry *link;

	if (!gv)
		return;

	frr_each_safe (midr_node_hash, &gv->nodes, entry) {
		midr_node_hash_del(&gv->nodes, entry);
		XFREE(MTYPE_MIDR_NODE_ENTRY, entry);
	}
	midr_node_hash_fini(&gv->nodes);

	for (ALL_LIST_ELEMENTS(gv->links, node, nnode, link))
		XFREE(MTYPE_MIDR_LINK_ENTRY, link);
	list_delete(&gv->links);

	XFREE(MTYPE_MIDR_GLOBAL_VIEW, gv);
}

static struct midr_link_entry *
midr_global_view_find_link(struct midr_global_view *gv,
			   const struct prefix *node_id)
{
	struct listnode *node;
	struct midr_link_entry *link;

	for (ALL_LIST_ELEMENTS_RO(gv->links, node, link))
		if (prefix_same(&link->remote_node_id, node_id))
			return link;

	return NULL;
}

/*
 * 按键删除一条 link_entry（命中返回 true）。links 表此前只增不减：node 下线
 * 只删 node 表却漏删对应 link，仅探未连的节点探测后也残留——孤儿长期堆积。
 * MTYPE_MIDR_LINK_ENTRY 是本文件 DEFINE_MTYPE_STATIC，释放只能在这里做。
 */
static bool midr_global_view_del_link(struct midr_global_view *gv,
				      const struct prefix *key)
{
	struct listnode *node, *nnode;
	struct midr_link_entry *link;

	for (ALL_LIST_ELEMENTS(gv->links, node, nnode, link))
		if (prefix_same(&link->remote_node_id, key)) {
			list_delete_node(gv->links, node);
			XFREE(MTYPE_MIDR_LINK_ENTRY, link);
			return true;
		}
	return false;
}

/* ===========================================================================
 * NDS node table
 * =========================================================================*/

/*
 * Coarse pre-filter for a freshly discovered remote node: decide whether it is
 * even worth probing.  Skeleton accepts every non-self node; real policy (path
 * crosses a tier-1 node, capability match, cross-group dedup, ...) lands later.
 * Note: this only gates whether we PROBE — the connect decision is separate.
 */
static bool midr_discovery_filter(struct bgp *bgp, struct midr_node_entry *entry)
{
	(void)bgp;
	if (entry->is_self)
		return false;
	/* TODO：真实粗筛规则（暂为 stub）。 */
	return true;
}

/*
 * 决定是否与一个【发现阶段】学到的节点建立邻居关系（探测 + 建会话）。
 * 保守 stub：暂用"本群匹配"占位——只对与本节点同群的节点建邻居，作为天然
 * 节流（新节点不会被每个跨群节点拨号）。真实的指标阈值 / 跨群规则待 PM/CL
 * 落地后接管（跨群建连应是 CL 的分群决策）。决策机制待定。
 */
static bool midr_discovery_should_peer(struct bgp *bgp,
				       const struct midr_node_entry *entry)
{
	struct bgp_midr *mi = bgp->midr_info;

	return entry->group_id != 0 && entry->group_id == mi->local_group_id;
}

/*
 * 解除与一个节点的探测/邻居关系：I-2 停探 + 清残留 link_entry + 清 is_adjacent，
 * 可选拆掉动态会话。节点条目本身的去留由调用方决定（本函数不动 node 表）。
 *
 * 双键清理：发现阶段的探测入口按 locator(transport /32) 建 link，而 PM 周期
 * loop 按 node_id(router-id) 回灌又建一条——同一节点最多两条 link，两个键都删。
 * 有 transport 时二者不同、各删一次；无 transport 时 locator 回落到 node_id，
 * prefix_same 为真只删一次。del_link 未命中返回 false，故本函数天然幂等。
 */
static void midr_nds_detach_node(struct bgp *bgp, struct midr_node_entry *entry,
				 enum midr_stop_reason reason,
				 bool teardown_session)
{
	struct midr_global_view *gv = bgp->midr_info->global_view;
	struct prefix locator;

	midr_node_get_locator(entry, &locator);
	/* I-2 停探。键必须与 I-1 配对：add 侧按 node_id 建探测 ctx（join 占位
	 * 条目 node_id==locator，仍一致），故删也统一按 node_id——按 locator 删
	 * 会在 transport≠router-id 的节点上找不到 ctx → 探测泄漏。 */
	midr_pm_remove_target(bgp, &entry->node_id, reason);
	midr_global_view_del_link(gv, &locator);
	if (!prefix_same(&locator, &entry->node_id))
		midr_global_view_del_link(gv, &entry->node_id);
	entry->is_adjacent = false;
	if (teardown_session)
		midr_ctrl_on_node_remove(bgp, entry);
}

/*
 * 收到一条经 BGP-LS 泛洪学到的节点后的接收侧反应（相当于 bootstrap 加入编排
 * 的单节点镜像版）。次序：粗筛 → 测性能 → 判断是否 peer → 直连：
 *
 *   1. 粗筛       -> 是否值得探测？（midr_discovery_filter，stub）
 *   2. 测性能     -> I-1 启动探测（同步把指标灌进 link_entry，供下一步判断）
 *   3. 是否 peer  -> midr_discovery_should_peer（保守 stub：本群匹配；未来读指标）
 *   4. 成为邻居   -> 标记 is_adjacent + 直连 midr_ctrl_connect + 显式 notify
 *
 * is_adjacent 只在【确定 peer 后】才置 true（语义="已决定纳入为邻居"，而非
 * "已探测过"）；不通过则保持 false。探测的 I-5 回灌只更新 link_entry、不 notify，
 * 故这里在确定建邻居后显式 notify(NODE_CHANGE) 让 CL 重评估。
 */
static void midr_nds_on_node_discovered(struct bgp *bgp,
					struct midr_node_entry *entry)
{
	/* 1. 探测前粗筛（用户的 stub，现仅排除 self）。 */
	if (!midr_discovery_filter(bgp, entry))
		return;

	/* 2. 测性能：I-1 以 router-id（node_id）为探测上下文的 key；PM 从
	 * global_view 条目取 transport_addr 作为实际探测目标。 */
	midr_pm_add_target(bgp, &entry->node_id, MIDR_SRC_GOSSIP,
			   entry->capabilities);

	/* 3. 判断是否建邻居（保守 stub：本群匹配；未来基于 link_entry 指标）。 */
	if (!midr_discovery_should_peer(bgp, entry)) {
		MIDR_LOG("MIDR 发现：节点 %pFX 群 %u -> 仅探测，不建邻居",
			 &entry->node_id, entry->group_id);
		/* 不建邻居：I-2 停探 + 清掉刚探测建的残留 link_entry（不拆会话，
		 * 本就没建）。否则跨群/非邻居节点探测后 link 永久堆积。 */
		midr_nds_detach_node(bgp, entry, MIDR_STOP_CLUSTER_CHANGE, false);
		return;
	}

	/* 4. 成为邻居：确定 peer 后才标 is_adjacent，直连，并显式通知 CL。 */
	entry->is_adjacent = true;
	midr_ctrl_connect(bgp, entry);
	midr_nds_notify_cl(bgp, MIDR_TRIGGER_NODE_CHANGE);

	MIDR_FLOW_LOG("MIDR 发现：节点 %pFX 群 %u -> 建邻居（探测+建连）",
		      &entry->node_id, entry->group_id);
}

/*
 * Update or insert a node entry from a received Node NLRI.
 * Called from bgp_nlri_parse_ls() for UPDATE messages (receive entry point).
 这是AI自己写的函数,收到NODE NLRI后如何存储里面的节点信息，对单个entry进行处理，调用点有while循环
 *
 * ===== 传播面单点 (2/3) = backend 替换边界 =====
 * MIDR 的"收包入口"单点: 远端 Node NLRI 经此进节点表。它本身【不】产生
 * origination (无 bgp_ls_originate_* 调用), 是三个 backend 替换边界之一
 * ——将来切到第二组 remote-view 回调 / gossip (C 计划) 时, 收侧数据源在此
 * 换掉, 边界外的节点表消费者不感知。见 midr_propagate_self 头注释的完整
 * 三点说明。
 */
void midr_nds_on_node_nlri(struct bgp *bgp, struct bgp_ls_nlri *nlri,
			   struct bgp_ls_attr *ls_attr)
{
	struct midr_global_view *gv;
	struct midr_node_entry key = {};
	struct midr_node_entry *entry;
	bool is_new = false;
	bool changed = false;
	bool group_changed = false; /* 群号本次实际变了（对端改组对称反应用） */
	uint32_t prev_gid = 0;	    /* 变更前的群号（仅 group_changed 时有效） */

	if (!bgp || !bgp->midr_info || !nlri || !ls_attr)
		return;

	gv = bgp->midr_info->global_view;
	midr_prefix_from_in_addr(&key.node_id,
				 nlri->nlri_data.node.local_node.bgp_router_id);

	entry = midr_node_hash_find(&gv->nodes, &key);
	if (!entry) {
		entry = XCALLOC(MTYPE_MIDR_NODE_ENTRY, sizeof(*entry));
		entry->node_id = key.node_id;
		midr_node_hash_add(&gv->nodes, entry);
		is_new = true;
	}

	/* ASN from Node Descriptor (TLV 512) if present */
	if (CHECK_FLAG(nlri->nlri_data.node.local_node.present_tlvs,
		       BGP_LS_NODE_DESC_AS_BIT))
		entry->asn = nlri->nlri_data.node.local_node.asn;

	/* Group ID (TLV 1185) */
	if (CHECK_FLAG(ls_attr->present_tlvs, BGP_LS_ATTR_MIDR_GROUP_ID_BIT)) {
		if (entry->group_id != ls_attr->midr_group_id) {
			changed = true;
			group_changed = true;
			prev_gid = entry->group_id;
		}
		entry->group_id = ls_attr->midr_group_id;
	}

	/* Node Capability (TLV 1187) — only accept newer seqno */
	if (CHECK_FLAG(ls_attr->present_tlvs,
		       BGP_LS_ATTR_MIDR_NODE_CAPABILITY_BIT)) {
		if (ls_attr->midr_cap_seqno > entry->cap_seqno) {
			if (entry->capabilities != ls_attr->midr_node_caps)
				changed = true;
			entry->capabilities = ls_attr->midr_node_caps;
			entry->cap_seqno = ls_attr->midr_cap_seqno;
		}
	}

	/* Transport Address (TLV 1188) — locator for peering/probe/display */
	if (CHECK_FLAG(ls_attr->present_tlvs,
		       BGP_LS_ATTR_MIDR_TRANSPORT_ADDR_BIT)) {
		entry->transport_addr = ls_attr->midr_transport_addr;
		entry->has_transport_addr = true;
	}

	entry->last_seen = monotime(NULL);
	entry->is_self = midr_prefix_is_self(bgp, &entry->node_id);

	/*
	 * Receive-side reaction.  A keepalive refresh of an already-known node
	 * (neither new nor changed) only bumps last_seen above and is ignored
	 * here, so the reaction chain is not re-run every 5s.  Our own NLRI
	 * (looped back) is skipped too.
	 */
	if (!entry->is_self) {
		/* §8.31：学到/变更了引导节点就记一笔种子（keepalive 刷新不触发，
		 * 因为下面两分支只在 is_new/changed 进入）。 */
		if (is_new || changed)
			midr_maybe_save_bootstrap_seed(entry);

		if (is_new) {
			midr_nds_on_node_discovered(bgp, entry);
		} else if (changed) {
			bool handled_by_discovery = false;

			/*
			 * 对端改组对称反应：远端把自己的群号改了，本地动态会话要
			 * 跟着重收敛，不必等 CL。判据复用 should_peer（本群且群号非 0）。
			 */
			if (group_changed) {
				struct bgp_midr *mi = bgp->midr_info;
				bool was_mine = (prev_gid != 0 &&
						 prev_gid == mi->local_group_id);
				bool now_mine =
					midr_discovery_should_peer(bgp, entry);

				if (was_mine && !now_mine && entry->is_adjacent) {
					/* 原本同群、现已离本群：拆掉动态会话。 */
					midr_nds_detach_node(
						bgp, entry,
						MIDR_STOP_CLUSTER_CHANGE, true);
					MIDR_FLOW_LOG("MIDR 对端改组：%pFX 群 %u -> %u（离本群），拆会话",
						      &entry->node_id, prev_gid,
						      entry->group_id);
				} else if (now_mine && !entry->is_adjacent) {
					/* 现进本群、尚未邻接：走发现全链建连
					 * （其内部建邻居后自会 notify CL）。 */
					midr_nds_on_node_discovered(bgp, entry);
					handled_by_discovery = true;
					MIDR_FLOW_LOG("MIDR 对端改组：%pFX 群 %u -> %u（进本群），建连",
						      &entry->node_id, prev_gid,
						      entry->group_id);
				}
			}

			/* 发现路径已自带 notify；其余变更在此统一 notify 一次。 */
			if (!handled_by_discovery)
				midr_nds_notify_cl(bgp,
						   MIDR_TRIGGER_NODE_CHANGE);
		}
	}
}

/* Remove a node entry on a Node NLRI WITHDRAW. */
void midr_nds_on_node_withdraw(struct bgp *bgp, struct bgp_ls_nlri *nlri)
{
	struct midr_global_view *gv;
	struct midr_node_entry key = {};
	struct midr_node_entry *entry;

	if (!bgp || !bgp->midr_info || !nlri)
		return;

	gv = bgp->midr_info->global_view;
	midr_prefix_from_in_addr(&key.node_id,
				 nlri->nlri_data.node.local_node.bgp_router_id);

	entry = midr_node_hash_find(&gv->nodes, &key);
	if (!entry)
		return;

	/* I-2 停探 + 清 link_entry + 拆动态会话（必须在删 node 之前，detach
	 * 内部要读 entry 的 locator）。 */
	midr_nds_detach_node(bgp, entry, MIDR_STOP_GRACEFUL_SHUTDOWN, true);
	midr_node_hash_del(&gv->nodes, entry);
	XFREE(MTYPE_MIDR_NODE_ENTRY, entry);

	/* I-3: node membership changed */
	midr_nds_notify_cl(bgp, MIDR_TRIGGER_NODE_CHANGE);
}

/*
 * 把一个群成员（来自 MEMBER_LIST_RESP）灌入 global_view 并标记为邻居，再 I-1
 * 启动探测。NDS 拥有 global_view，故灌入统一由 NDS 负责（ctrl 侧只把 wire 解出
 * 的字段交进来）。已存在则刷新字段。注意：这里显式置 is_adjacent=true；后续
 * 该节点的 Node NLRI 泛洪到达时，on_node_nlri 只逐字段刷新、不碰 is_adjacent。
 */
void midr_nds_learn_member(struct bgp *bgp, struct in_addr rid, as_t asn,
			   struct in_addr transport, uint32_t group_id)
{
	struct midr_global_view *gv;
	struct midr_node_entry key = {};
	struct midr_node_entry *entry;

	if (!bgp || !bgp->midr_info)
		return;

	gv = bgp->midr_info->global_view;
	midr_prefix_from_in_addr(&key.node_id, rid);

	entry = midr_node_hash_find(&gv->nodes, &key);
	if (!entry) {
		entry = XCALLOC(MTYPE_MIDR_NODE_ENTRY, sizeof(*entry));
		entry->node_id = key.node_id;
		midr_node_hash_add(&gv->nodes, entry);
	}
	entry->asn = asn;
	entry->group_id = group_id;
	entry->transport_addr = transport;
	entry->has_transport_addr = true;
	entry->last_seen = monotime(NULL);
	entry->is_self = midr_prefix_is_self(bgp, &entry->node_id);
	entry->is_adjacent = true; /* 成员表里的成员就是要建邻居的对象 */

	/* I-1: probe by node_id; PM resolves transport_addr from global_view. */
	midr_pm_add_target(bgp, &entry->node_id, MIDR_SRC_BOOTSTRAP,
			   entry->capabilities);
}

/*
 * 收到 REP_LIST_REQ / MEMBER_LIST_REQ 时调用：请求方在其自身的 join 流程里会
 * 反过来对我们发 PM 探测包，而 midr_pm_recv() 的 pm_is_known_transport() 只
 * 接受 global_view 里已知的来源地址——请求方此时还没有 BGP-LS 会话，我们的
 * global_view 里没有它，探测包会被当成未知来源静默丢弃。
 * 这里用请求帧自带的身份（router-id/transport/asn）灌一条最小条目，仅用于
 * 通过来源校验；不置 is_adjacent、不触发 I-1——是否真正建邻居仍由 CL 决定。
 */
void midr_nds_learn_requester(struct bgp *bgp, struct in_addr rid, as_t asn,
			      struct in_addr transport)
{
	struct midr_global_view *gv;
	struct midr_node_entry key = {};
	struct midr_node_entry *entry;

	if (!bgp || !bgp->midr_info)
		return;

	gv = bgp->midr_info->global_view;
	midr_prefix_from_in_addr(&key.node_id, rid);

	entry = midr_node_hash_find(&gv->nodes, &key);
	if (!entry) {
		entry = XCALLOC(MTYPE_MIDR_NODE_ENTRY, sizeof(*entry));
		entry->node_id = key.node_id;
		midr_node_hash_add(&gv->nodes, entry);
	}
	entry->asn = asn;
	entry->transport_addr = transport;
	entry->has_transport_addr = true;
	entry->last_seen = monotime(NULL);
	entry->is_self = midr_prefix_is_self(bgp, &entry->node_id);
}

/*
 * Refresh the local self-entry from local configuration.
 * Called after bgp_ls_originate_bgp_node() succeeds.
 */
void midr_nds_local_node_update(struct bgp *bgp)
{
	struct midr_global_view *gv;
	struct midr_node_entry key = {};
	struct midr_node_entry *entry;

	if (!bgp || !bgp->midr_info)
		return;

	gv = bgp->midr_info->global_view;
	midr_prefix_from_in_addr(&key.node_id, bgp->router_id);

	entry = midr_node_hash_find(&gv->nodes, &key);
	if (!entry) {
		entry = XCALLOC(MTYPE_MIDR_NODE_ENTRY, sizeof(*entry));
		entry->node_id = key.node_id;
		entry->is_self = true;
		midr_node_hash_add(&gv->nodes, entry);
	}

	entry->asn = bgp->as;
	entry->group_id = bgp->midr_info->local_group_id;
	entry->capabilities = bgp->midr_info->local_capabilities;
	if (bgp->midr_info->transport_addr_set) {
		entry->transport_addr = bgp->midr_info->local_transport_addr;
		entry->has_transport_addr = true;
	}
	entry->last_seen = monotime(NULL);
	entry->is_self = true;
}

void midr_nds_set_capability(struct bgp *bgp, uint32_t new_caps)
{
	if (!bgp || !bgp->midr_info)
		return;

	bgp->midr_info->local_capabilities = new_caps;
	midr_propagate_self(bgp, MIDR_ORIGIN_CAP_UPDATE);
}

/*
 * 换组重收敛编排核心：把本地群号切到 new_gid，并让动态会话跟随重收敛。
 * 手动通道（midr_nds_set_group_id）与 I-7 JOIN 决策共用这一段，返回与新群
 * 建连的成员数。三步次序有讲究：
 *
 *   1. 先改群号 + 重通告——必须早于建连：PEER_REQUEST 资格闸门按 local_group_id
 *      过滤，若先建连、群号还是旧的，对端回来的反向建连会被自己的闸门挡掉。
 *      重通告走 midr_originate_group_update（薄封装）→ midr_propagate_self 传播面
 *      单点，不绕过 backend 替换边界。
 *   2. 与新群成员建连（connect_group 遍历 global_view 中群号==new_gid 的成员）。
 *      new_gid==0（手动离群）时跳过——0 号非有效群，无成员可连。
 *   3. 拆旧群：凡"仍标 is_adjacent 却已不属于本群"的节点一律 detach，判据用
 *      !should_peer 而非 ==old_gid，顺带清掉任何历史残留的错群邻接。detach 复用
 *      links 泄漏修复轮的统一清理（I-2 停探 + 双键删 link_entry + 复位 is_adjacent
 *      + 拆动态会话），不删 node 条目。PM 真实化后第 2 步的停探语义自动完整。
 *   4. 停非本群群代表的评估期探测：join 第一段对 rep_dir 里【每个】群代表都起了
 *      探测（midr_join_on_rep_list），落定后除本群代表外都不再需要。它们不带
 *      is_adjacent（只探不纳入邻居），第 3 步扫不到，故按 rep_dir 单独收口——
 *      否则每 join 一次就永久多养一批跨群探测。
 */
static uint32_t midr_group_reconverge(struct bgp *bgp, uint32_t new_gid)
{
	struct bgp_midr *mi = bgp->midr_info;
	uint32_t old_gid = mi->local_group_id;
	struct midr_node_entry *entry;
	struct listnode *rn;
	struct midr_rep_entry *r;
	uint32_t connected = 0, detached = 0, reps_stopped = 0;

	/* 1. 改群号 + 重通告（先于建连）。 */
	midr_originate_group_update(bgp, new_gid, old_gid);

	/* 2. 与新群成员建连（离群 new_gid==0 时无成员可连，跳过）。 */
	if (new_gid != 0)
		connected = midr_ctrl_connect_group(bgp, new_gid);

	/* 3. 拆旧群：邻接但已不同群的节点全部 detach。 */
	frr_each (midr_node_hash, &mi->global_view->nodes, entry) {
		if (entry->is_self || !entry->is_adjacent)
			continue;
		if (midr_discovery_should_peer(bgp, entry))
			continue; /* 仍属本群，留着 */
		midr_nds_detach_node(bgp, entry, MIDR_STOP_CLUSTER_CHANGE, true);
		detached++;
	}

	/*
	 * 4. 停非本群群代表的评估期探测。键的取法必须与 midr_join_on_rep_list
	 *    起探时一致（有真名用真名、否则用 transport 建的占位条目），否则找不到
	 *    条目、停不掉。两道守卫：
	 *      - is_self：本节点自兼群代表时不能把自己停了；
	 *      - is_adjacent：该代表若已是本群邻居（如它换群进了本群、而 rep_dir 里
	 *        还是旧群号），它归第 3 步管，这里不碰——避免误伤正经邻接。
	 *    不拆会话（teardown=false）：只探不连的代表本就没有会话；万一它另有会话，
	 *    那也不是本步该管的账。
	 */
	for (ALL_LIST_ELEMENTS_RO(mi->rep_dir, rn, r)) {
		struct midr_node_entry key = {};
		struct midr_node_entry *rep_entry;

		if (r->group_id == new_gid)
			continue; /* 本群代表，留着继续探 */

		if (r->rep_rid.s_addr != INADDR_ANY)
			midr_prefix_from_in_addr(&key.node_id, r->rep_rid);
		else
			midr_prefix_from_in_addr(&key.node_id, r->rep_transport);

		rep_entry = midr_node_hash_find(&mi->global_view->nodes, &key);
		if (!rep_entry || rep_entry->is_self || rep_entry->is_adjacent)
			continue;

		midr_nds_detach_node(bgp, rep_entry, MIDR_STOP_CLUSTER_CHANGE,
				     false);
		reps_stopped++;
	}

	MIDR_FLOW_LOG("MIDR 换组重收敛：群 %u -> %u（建连 %u 个新群成员，拆除 %u 个旧群邻接，停探 %u 个非本群代表）",
		      old_gid, new_gid, connected, detached, reps_stopped);
	return connected;
}

void midr_nds_set_group_id(struct bgp *bgp, uint32_t new_gid)
{
	struct bgp_midr *mi;

	if (!bgp || !bgp->midr_info)
		return;
	mi = bgp->midr_info;

	/* old==new 短路：无变化不折腾会话。 */
	if (mi->local_group_id == new_gid) {
		MIDR_LOG("MIDR 换组(手动)：群号未变（%u），无动作", new_gid);
		return;
	}

	/*
	 * 中止在途/待发的 join：运维手动换组 = 强制切换，放弃经 bootstrap 加入的
	 * 意图。作废 join_intent 是关键——REP_LIST_REQ 可能已发出、响应尚未到达
	 * （此刻 join_in_progress 还是 false），若不作废意图，迟到的 REP_LIST_RESP
	 * 会把 join 重新拉起、CL 决策再把运维刚设的群号改掉（静默覆盖）。
	 */
	if (mi->join_intent || mi->join_in_progress) {
		mi->join_intent = false;
		mi->join_in_progress = false;
		mi->join_phase = MIDR_JOIN_IDLE;
		mi->join_group_id = 0;
		mi->bootstrap_cur = NULL; /* §8.32：收起 failover 游标，残留 pending
					   * 死掉时守卫（intent+地址匹配）自然忽略 */
		MIDR_LOG("MIDR 换组(手动)：作废在途加入意图");
	}

	MIDR_FLOW_LOG("MIDR 换组(手动)：群 %u -> %u", mi->local_group_id, new_gid);
	midr_group_reconverge(bgp, new_gid);
}

static const char *midr_origin_reason_str(enum midr_origin_reason reason)
{
	switch (reason) {
	case MIDR_ORIGIN_INIT:
		return "init";
	case MIDR_ORIGIN_KEEPALIVE:
		return "keepalive";
	case MIDR_ORIGIN_GROUP_UPDATE:
		return "group-update";
	case MIDR_ORIGIN_CAP_UPDATE:
		return "cap-update";
	case MIDR_ORIGIN_TRANSPORT_UPDATE:
		return "transport-update";
	case MIDR_ORIGIN_REJOIN:
		return "rejoin";
	case MIDR_ORIGIN_LEAVE:
		return "leave";
	}
	return "unknown";
}

/*
 * Advertise (or, for LEAVE, withdraw) our own Node NLRI.  The single MIDR
 * egress point: every self-origination flows through here so the shutdown
 * guard and logging live in one place.  A withdraw is always honoured; a
 * re-origination is suppressed while gracefully shut down (otherwise a later
 * cap/group/transport change would silently undo a `midr shutdown`).
 *
 * ===== 传播面单点 (1/3) = backend 替换边界 =====
 * 这是 MIDR "自通告出口"，也是三个允许触碰 BGP-LS 数据库写入
 * (bgp_ls_originate_* / bgp_ls_withdraw_*) 的单点之一。三点为:
 *   (1) midr_propagate_self      —— 自通告出口 (本函数)
 *   (2) midr_nds_on_node_nlri    —— 收包入口 (不产生 origination)
 *   (3) midr_e1_write_to_bgpls   —— E-1 链路指标出口
 * 三者构成 backend 替换边界: 自有 BGP-LS origination <-> 第二组 upsert+回调
 * <-> gossip (C 计划) 可整体替换其内部实现, 边界外调用方 (9 处调用本函数的
 * keepalive/set_capability/set_group_id/originate_group_update/vty 命令等)
 * 不感知实现切换。除这三点外, MIDR 层不得直调 origination/withdraw 族
 * (2026-07-10 全树盘点确认无旁路; 详见 docs/decisions/
 * midr-propagation-plane-and-gossip.md)。
 */
void midr_propagate_self(struct bgp *bgp, enum midr_origin_reason reason)
{
	struct bgp_midr *mi;

	if (!bgp || !bgp->midr_info)
		return;
	mi = bgp->midr_info;

	if (reason == MIDR_ORIGIN_LEAVE) {
		bgp_ls_withdraw_bgp_node(bgp);
	} else {
		if (mi->shutdown) {
			if (BGP_DEBUG(midr, MIDR))
				zlog_debug("MIDR: propagate-self (%s) suppressed while shut down",
					   midr_origin_reason_str(reason));
			return;
		}
		bgp_ls_originate_bgp_node(bgp);
	}

	if (BGP_DEBUG(midr, MIDR))
		zlog_debug("MIDR: propagate-self reason=%s",
			   midr_origin_reason_str(reason));
}

/* ===========================================================================
 * Timers
 * =========================================================================*/

/*
 * Expire-check timer: drop remote nodes not refreshed within the expiry
 * window.  Staleness is measured purely on the local clock (last_seen is
 * stamped at receive time), so cross-node clock skew is irrelevant.
 */
static void midr_expire_check_timer(struct event *t)
{
	struct bgp *bgp = EVENT_ARG(t);
	struct bgp_midr *mi = bgp->midr_info;
	struct midr_node_entry *entry;
	time_t now = monotime(NULL);

	frr_each_safe (midr_node_hash, &mi->global_view->nodes, entry) {
		if (entry->is_self)
			continue;
		if (now - entry->last_seen > MIDR_NODE_EXPIRE_TIME) {
			if (BGP_DEBUG(midr, MIDR))
				zlog_debug("MIDR: node %pFX (group %u) expired",
					   &entry->node_id, entry->group_id);
			/* I-2 停探 + 清 link_entry + 拆动态会话（删 node 之前）。 */
			midr_nds_detach_node(bgp, entry,
					     MIDR_STOP_KEEPALIVE_TIMEOUT, true);
			midr_node_hash_del(&mi->global_view->nodes, entry);
			XFREE(MTYPE_MIDR_NODE_ENTRY, entry);
			/* I-3: node membership changed */
			midr_nds_notify_cl(bgp, MIDR_TRIGGER_NODE_CHANGE);
		}
	}

	event_add_timer(bm->master, midr_expire_check_timer, bgp,
			MIDR_EXPIRE_CHECK_INTERVAL, &mi->t_expire_check);
}

/*
 * Keepalive timer: re-originate the local Node NLRI (TLV 1185/1187) so
 * remote nodes keep our entry fresh.
 */
static void midr_keepalive_timer(struct event *t)
{
	struct bgp *bgp = EVENT_ARG(t);
	struct bgp_midr *mi = bgp->midr_info;

	/* The shutdown guard lives in midr_propagate_self() now, so a graceful
	 * `midr shutdown` is not undone by this 5s refresh. */
	midr_propagate_self(bgp, MIDR_ORIGIN_KEEPALIVE);

	event_add_timer(bm->master, midr_keepalive_timer, bgp,
			MIDR_KEEPALIVE_INTERVAL, &mi->t_keepalive);
}

/* Deferred join-phase triggers: fire after MIDR_JOIN_PROBE_WAIT_SECS to give
 * the PM long-term EWMA time to warm up before CL evaluates link quality. */
static void midr_join_rep_probe_done_cb(struct event *t)
{
	struct bgp *bgp = EVENT_ARG(t);
	MIDR_FLOW_LOG("MIDR 加入：REP_PROBE_DONE 定时器触发，通知 CL");
	midr_nds_notify_cl(bgp, MIDR_TRIGGER_REP_PROBE_DONE);
}

/*
 * Periodic-sync timer: hand the global view to CL for a full re-evaluation
 * on a fixed cadence (independent of probe/membership events).
 */
static void midr_periodic_sync_timer(struct event *t)
{
	struct bgp *bgp = EVENT_ARG(t);
	struct bgp_midr *mi = bgp->midr_info;
	struct midr_node_entry *entry;

	midr_nds_notify_cl(bgp, MIDR_TRIGGER_PERIODIC_SYNC);

	/* §8.31：顺路把当前视图里的引导节点刷一遍种子库 last_seen（"我最近还
	 * 见过它"），再 prune 到上限防膨胀。下线/过期不删库——种子跨活性留底。 */
	frr_each (midr_node_hash, &mi->global_view->nodes, entry)
		midr_maybe_save_bootstrap_seed(entry);
	midr_store_seed_prune(MIDR_STORE_SEED_KEEP);

	event_add_timer(bm->master, midr_periodic_sync_timer, bgp,
			MIDR_PERIODIC_SYNC_INTERVAL, &mi->t_periodic_sync);
}

/* ===========================================================================
 * I-5: PM -> NDS, link state update
 * =========================================================================*/

/*
 * E-1: export short-term link metrics as TLV 1186 on the corresponding
 * Link NLRI.  Finds the BGP peer whose remote_id matches link->remote_node_id,
 * builds a bgp_ls_attr carrying TLV 1186, and calls
 * bgp_ls_originate_bgp_link() to upsert the Link NLRI in the BGP-LS RIB.
 *
 * loss_rate (double 0.0-1.0) is scaled to micro-units (×10^6) for the
 * uint32_t wire field, matching RFC 7471 millionths-of-loss convention.
 *
 * ===== 传播面单点 (3/3) = backend 替换边界 =====
 * MIDR 的 "E-1 链路指标出口" 单点 (定义见下方 midr_e1_write_to_bgpls):
 * 唯一从 PM 侧把链路性能写入 BGP-LS Link NLRI 的地方, 是三个 backend 替换
 * 边界之一——将来 origination 移交第二组 (直接带 rtt/loss/bw+seqno 三元指标
 * 由对方编码) 时在此换 backend。见 midr_propagate_self 头注释的完整三点说明。
 */
struct peer *midr_node_established_peer(struct bgp *bgp,
				       const struct prefix *node_id)
{
	struct peer *peer;
	struct listnode *node;

	/* Locate the BGP peer by its advertised router-id (TLV 516). */
	for (ALL_LIST_ELEMENTS_RO(bgp->peer, node, peer)) {
		if (CHECK_FLAG(peer->sflags, PEER_STATUS_GROUP))
			continue;
		if (peer->connection->status != Established)
			continue;
		if (peer->remote_id.s_addr == node_id->u.prefix4.s_addr)
			return peer;
	}

	return NULL;
}

/*
 * §8.21 去抖判断（"一处判断、两处消费"的那一处）：本次指标相对**上次发出的
 * 快照**该不该发，以及——同样重要——是"真变化"还是"只是该刷一次了"。
 *
 * 返回原因而非布尔，是因为两个下游的口径不同：
 *   - 下游②（对外发 Link NLRI）：任何非 NONE 都要发；
 *   - 下游①（对内 notify CL）：**只有真变化才发**。周期兜底是为保传播/防静默
 *     错误而重发，指标一动没动，此时通知 CL 等于喂假事件，会害它做无谓的重
 *     评估（07-23 实测发现：不区分的话 notify 数完全跟随兜底节拍）。
 *
 * 四条必发条件：
 *   1. FIRST     首次（还没发过）——邻居的第一条 1186 不能等；
 *   2. STATUS    status 跳变——UP/DEGRADED/DOWN 变化永远是大事，DOWN 必须立刻广播；
 *   3. THRESHOLD 任一指标超阈值（阈值与取值理由见 bgp_midr.h 的 MIDR_DEBOUNCE_*）；
 *   4. PERIODIC  距上次发送 ≥ MIDR_DEBOUNCE_MAX_SILENCE 秒的兜底重发。不新增
 *                定时器：I-5 每秒都进来，顺路查一下时间即可。
 *
 * 只读不写：快照的更新在调用方发出之后做（发出去了才算数）。
 * 乒乓后手（暂不实装）：若实测值在阈值边界反复横跳导致发送偏多，可在此加
 * "两次发送最小间隔"——last_sent_time 已经在手边，多一个 && 即可。
 */
enum midr_debounce_reason {
	MIDR_DEBOUNCE_NONE = 0,	 /* 无显著变化且未到兜底期：两个下游都跳过 */
	MIDR_DEBOUNCE_FIRST,	 /* 首条 */
	MIDR_DEBOUNCE_STATUS,	 /* 状态跳变 */
	MIDR_DEBOUNCE_THRESHOLD, /* 指标超阈值 */
	MIDR_DEBOUNCE_PERIODIC,	 /* 周期兜底（发但不通知 CL） */
};

/* 是否属于"真变化"——决定要不要惊动 CL（兜底不算）。 */
static bool midr_debounce_is_real_change(enum midr_debounce_reason r)
{
	return r == MIDR_DEBOUNCE_FIRST || r == MIDR_DEBOUNCE_STATUS ||
	       r == MIDR_DEBOUNCE_THRESHOLD;
}

static enum midr_debounce_reason
midr_nds_e1_should_send(const struct midr_link_entry *link,
			enum midr_link_status new_status,
			const struct midr_link_metrics *cur, time_t now)
{
	const struct midr_link_metrics *snap = &link->sent_metrics;
	uint32_t rtt_delta;

	/* 1. 首次必发。 */
	if (!link->sent_once)
		return MIDR_DEBOUNCE_FIRST;

	/* 2. 状态跳变必发。 */
	if (new_status != link->status)
		return MIDR_DEBOUNCE_STATUS;

	/*
	 * 3. 逐指标超阈值判断。次序在兜底之前：同一秒里既超阈值又到兜底期时，
	 *    要按"真变化"记（否则会被兜底吃掉、漏掉给 CL 的通知）。
	 */

	/* RTT：相对 + 绝对双条件同时满足才算变化（理由见头文件注释）。 */
	rtt_delta = cur->rtt_us > snap->rtt_us ? cur->rtt_us - snap->rtt_us
					       : snap->rtt_us - cur->rtt_us;
	if (rtt_delta >= MIDR_DEBOUNCE_RTT_ABS_US) {
		/* 快照为 0（还没测到过有效值）时任何非零值都算显著变化，
		 * 同时避开除零。 */
		if (snap->rtt_us == 0)
			return MIDR_DEBOUNCE_THRESHOLD;
		if ((uint64_t)rtt_delta * 100 >=
		    (uint64_t)snap->rtt_us * MIDR_DEBOUNCE_RTT_REL_PCT)
			return MIDR_DEBOUNCE_THRESHOLD;
	}

	/* 丢包率：绝对差。 */
	if (fabs(cur->loss_rate - snap->loss_rate) >= MIDR_DEBOUNCE_LOSS_ABS)
		return MIDR_DEBOUNCE_THRESHOLD;

	/*
	 * 带宽分数【刻意不作独立判据】——07-23 实测教训，非疏漏：
	 * 现实现里 bw_score = sqrt(1.5)/(rtt × sqrt(loss))（bgp_midr_pm.c），
	 * 是 rtt 与 loss 的确定性派生量、不含独立信息。若让它独立触发，就等于给
	 * RTT 开了条绕过"绝对差 ≥1ms"免疫的旁路：内网 rtt 225→289us（+28%，绝对
	 * 才 64us 本该被挡）经 1/x 放大成 bw −22%，稳态下累计漂移即穿透 25% 阈值
	 * ——实测每几秒就假触发一次 NODE_CHANGE。而 bw 量纲跨 200 倍（跨群 271
	 * vs 内网 55000），任何绝对门槛都套不住，靠调参解决不了。
	 * 结论：rtt/loss 判无显著变化时，bw 的变化必是二者亚阈值抖动被放大的结果，
	 * 不是新信息。⚠ 若将来 bw_score 改为独立测量（真实带宽探测而非 rtt 派生），
	 * 必须回来恢复它的独立判据。
	 */

	/* 4. 指标无显著变化：距上次发送够久则兜底重发一次（只发、不通知 CL）。 */
	if (now - link->last_sent_time >= MIDR_DEBOUNCE_MAX_SILENCE)
		return MIDR_DEBOUNCE_PERIODIC;

	return MIDR_DEBOUNCE_NONE;
}

static void midr_e1_write_to_bgpls(struct bgp *bgp, struct midr_link_entry *link)
{
	struct peer *peer;
	struct bgp_ls_attr *ls_attr;

	peer = midr_node_established_peer(bgp, &link->remote_node_id);
	if (!peer) {
		if (BGP_DEBUG(midr, MIDR))
			zlog_debug(
				"MIDR E-1: no established peer for node %pFX, skipping",
				&link->remote_node_id);
		return;
	}

	ls_attr = bgp_ls_attr_alloc();
	bgp->midr_info->perf_seqno++;
	midr_tlv_set_link_perf(ls_attr, link->short_term.rtt_us,
			       (uint32_t)(link->short_term.loss_rate * 1e6),
			       link->short_term.bw_score,
			       (uint64_t)bgp->midr_info->perf_seqno);

	bgp_ls_originate_bgp_link(bgp, peer, ls_attr);
	bgp_ls_attr_free(ls_attr);

	if (BGP_DEBUG(midr, MIDR))
		zlog_debug(
			"MIDR E-1: exported TLV 1186 for link to %pFX (rtt=%uus loss=%.4f bw=%u seqno=%u)",
			&link->remote_node_id, link->short_term.rtt_us,
			link->short_term.loss_rate, link->short_term.bw_score,
			bgp->midr_info->perf_seqno);
}

void midr_nds_on_link_update(struct bgp *bgp, const struct prefix *node_id,
			     enum midr_link_status status,
			     uint32_t consecutive_failures,
			     const struct midr_link_metrics *short_term,
			     const struct midr_link_metrics *long_term)
{
	struct bgp_midr *mi;
	struct midr_link_entry *link;
	time_t now = monotime(NULL);
	enum midr_debounce_reason reason;

	if (!bgp || !bgp->midr_info || !node_id)
		return;

	mi = bgp->midr_info;

	link = midr_global_view_find_link(mi->global_view, node_id);
	if (!link) {
		link = XCALLOC(MTYPE_MIDR_LINK_ENTRY, sizeof(*link));
		prefix_copy(&link->remote_node_id, node_id);
		listnode_add(mi->global_view->links, link);
	}

	/*
	 * §8.21 去抖判断必须在覆盖 link->status / short_term **之前**做——判据要
	 * 拿"本次值"跟"上次发出的快照"比，且 status 跳变判断要看新旧两个值。
	 * 本地 link_entry 照常每秒更新（CL 的 60s 热身窗、pm-test 的指标判定都读
	 * 它），去抖只门控两件对外的事：重发 Link NLRI、惊动 CL。
	 */
	reason = midr_nds_e1_should_send(link, status,
					 short_term ? short_term
						    : &link->short_term,
					 now);

	link->status = status;
	link->consecutive_failures = consecutive_failures;
	link->last_probe_time = now;
	if (short_term)
		link->short_term = *short_term;
	if (long_term)
		link->long_term = *long_term;

	/*
	 * 一次成功的探测回复本身就是活性信号。稳态邻居的 last_seen 靠 BGP-LS
	 * NLRI 泛洪刷新，但"只探不连"阶段的候选成员（midr_nds_learn_member 灌入，
	 * is_adjacent=true 但尚无 BGP-LS 会话）没有任何 NLRI 泛洪可刷新它——若不
	 * 在这里补上，MIDR_NODE_EXPIRE_TIME（15s）会在 MIDR_JOIN_PROBE_WAIT_SECS
	 * （60s）的 CL 评估窗口结束前就把这些条目过期删除，MEMBER_PROBE_DONE 到
	 * 时发现候选全部消失，误判为 0 条好链路。
	 */
	if (status == MIDR_LINK_UP) {
		struct midr_node_entry key = {};
		struct midr_node_entry *node;

		key.node_id = *node_id;
		node = midr_node_hash_find(&mi->global_view->nodes, &key);
		if (node)
			node->last_seen = now;
	}

	/*
	 * 无显著变化：本地视图已更新完毕，对外两件事都跳过（既不重发 Link NLRI、
	 * 也不惊动 CL）。绝大多数秒走这条路。
	 */
	if (reason == MIDR_DEBOUNCE_NONE)
		return;

	/* 下游②（对外）：E-1 把短期指标写进 BGP-LS Link NLRI（TLV 1186）。
	 * seqno 在 E-1 内部自增，条件化之后它自然变成"真发出去才递增"的语义。 */
	midr_e1_write_to_bgpls(bgp, link);

	/* 发出即刻记快照：下次比较的基准，以及兜底周期的计时起点。 */
	link->sent_metrics = link->short_term;
	link->last_sent_time = now;
	link->sent_once = true;

	/*
	 * 下游①（对内）：链路质量真变化才值得让 CL 重新评估分群，发一条
	 * NODE_CHANGE。两道闸：
	 *   - **只在真变化时**（首条/状态跳变/超阈值）——周期兜底重发时指标一动
	 *     没动，通知了就是喂假事件，CL 会被骗去做无谓重评估（07-23 实测：不加
	 *     这道闸，notify 数完全跟随兜底节拍，60s 内 15 条全是假的）；
	 *   - **只在稳态**——join 期（PROBING_REPS/PROBING_MEMBERS）指标正从 0 爬向
	 *     真值，每一步都算"显著变化"，会连发一串无意义通知；而那段时间编排层
	 *     本就有专门的 REP/MEMBER_PROBE_DONE 通知，CL 不缺消息。
	 *
	 * 注意：join 与稳态的分工没变——"探完一批"仍由编排层显式发 DONE，I-5 只在
	 * 稳态补上原先完全缺失的"某条链路变天了"这一路事件（CL 侧 NODE_CHANGE 分支
	 * 现为 stub，接上零风险；其稳态算法到货即可消费）。
	 */
	if (midr_debounce_is_real_change(reason) &&
	    mi->join_phase == MIDR_JOIN_IDLE)
		midr_nds_notify_cl(bgp, MIDR_TRIGGER_NODE_CHANGE);

	/* TODO（未做）：status==DOWN 的失效确认（被动下线判定，见 §8）。 */
}

/* ===========================================================================
 * I-3: NDS -> CL
 * =========================================================================*/

void midr_nds_notify_cl(struct bgp *bgp, enum midr_trigger_type trigger)
{
	struct bgp_midr *mi = bgp->midr_info;

	if (mi && mi->cl_callback)
		mi->cl_callback(bgp, trigger, mi->global_view);
}

/* ===========================================================================
 * I-7: CL -> NDS, apply a clustering decision
 * =========================================================================*/

void midr_originate_group_update(struct bgp *bgp, uint32_t new_group_id,
				 uint32_t old_group_id)
{
	if (!bgp || !bgp->midr_info)
		return;

	bgp->midr_info->local_group_id = new_group_id;

	/* Re-originate the local Node NLRI carrying the new TLV 1185 */
	midr_propagate_self(bgp, MIDR_ORIGIN_GROUP_UPDATE);

	if (BGP_DEBUG(midr, MIDR))
		zlog_debug("MIDR: group-id %u -> %u re-originated", old_group_id,
			   new_group_id);
}

void midr_nds_on_cluster_decision(struct bgp *bgp,
				  const struct midr_cluster_decision *decision)
{
	struct bgp_midr *mi;

	if (!bgp || !bgp->midr_info || !decision)
		return;

	mi = bgp->midr_info;

	switch (decision->decision_type) {
	case MIDR_DECISION_RECOMMEND:
		/*
		 * §1.1 第一段产物：CL 选定群代表。幂等 guard——只在"探群代表"阶段
		 * 处理第一个 RECOMMEND（探测同步循环会触发多次 REP_PROBE_DONE，第一次
		 * 在此把阶段推进到 PROBING_MEMBERS，后续因阶段不符被忽略）。
		 * 待办 1：若某群代表中途下线，REP_PROBE_DONE 可能因"探不齐"而发不出
		 *         （现阶段靠"探到即推进"跑通正常流程，超时兜底后续再做）。
		 */
		if (mi->join_phase != MIDR_JOIN_PROBING_REPS) {
			MIDR_LOG("MIDR I-7：RECOMMEND 但不在探群代表阶段，忽略");
			break;
		}
		mi->join_phase = MIDR_JOIN_PROBING_MEMBERS;
		mi->join_group_id = decision->new_group_id;
		/*
		 * 此处【不】通告群号——RECOMMEND 只是选定要评估的候选群，尚未真正
		 * 入群。群号通告（TLV 1185）推迟到 JOIN（CL 判定值得入群后），避免
		 * "提前开香槟"。join_group_id 仅记录候选群，供后续探成员 / 建连用。
		 */
		/* 向推荐的群代表请求成员列表（MEMBER_LIST_REQ）。 */
		midr_ctrl_send_member_request(bgp,
					      decision->recommended_rep.u.prefix4,
					      decision->new_group_id);
		MIDR_FLOW_LOG("MIDR I-7：RECOMMEND 群代表 %pFX → 进入探成员阶段，请求群 %u 成员",
			      &decision->recommended_rep, decision->new_group_id);
		break;
	case MIDR_DECISION_JOIN:
		/*
		 * §1.1 第二段产物：入群。幂等 guard——只在"探成员"阶段处理第一个
		 * JOIN，处理后回到稳态（IDLE），后续重复 JOIN 被忽略。
		 * 待办 1：同上，"探不齐"异常后续做超时兜底。
		 */
		if (mi->join_phase != MIDR_JOIN_PROBING_MEMBERS) {
			MIDR_LOG("MIDR I-7：JOIN 但不在探成员阶段，忽略");
			break;
		}
		/*
		 * ⑥ 入群：走与手动换组共用的重收敛编排——改群号+重通告、与新群成员
		 * 建连、并拆掉旧群残留邻接。首次分群 old_group_id==0 时无旧群可拆
		 * （reconverge 内按"邻接却已不同群"判据自然拆 0 个）；非首次（换群）
		 * 则顺带断开原群会话，补上旧版 JOIN 只建不拆的缺口。
		 */
		mi->join_members =
			midr_group_reconverge(bgp, decision->new_group_id);
		/* join 落定回稳态：清意图（此后迟到的 REP_LIST_RESP 不再重启 join），
		 * 此后探测的 I-5 回灌按 NODE_CHANGE 处理，不再是 rep/member 段。 */
		mi->join_phase = MIDR_JOIN_IDLE;
		mi->join_in_progress = false;
		mi->join_intent = false;
		mi->join_group_id = 0; /* 候选群字段回零：流程已结束，真身看 local_group_id */
		MIDR_FLOW_LOG("MIDR I-7：JOIN 群 %u 完成（经重收敛编排建连 %u 个成员），加入流程结束（回稳态）",
			      decision->new_group_id, mi->join_members);
		break;
	case MIDR_DECISION_LEAVE:
		/* 待办 2：撤销 group-id 通告 + 经 I-2 停止相关探测（未实现）。 */
		MIDR_LOG("MIDR I-7：LEAVE 群 %u（stub）", decision->old_group_id);
		break;
	case MIDR_DECISION_SPLIT:
		midr_originate_group_update(bgp, decision->new_group_id,
					    decision->old_group_id);
		break;
	case MIDR_DECISION_CREATE:
		/*
		 * 自建新群。CREATE 有两个可能来源，二者都应被接受：
		 *   - PROBING_REPS：REP_PROBE_DONE 时没有任何群代表探到数据
		 *     （cl_handle_rep_probe_done 的"无可用候选"分支），根本没
		 *     进入过 PROBING_MEMBERS；
		 *   - PROBING_MEMBERS：所选群的成员链路数达不到入群阈值
		 *     （cl_handle_member_probe_done 的兜底分支）。
		 * 只在真正不在加入流程中（IDLE）时才视为过期/重复通知而忽略。
		 */
		if (mi->join_phase != MIDR_JOIN_PROBING_REPS &&
		    mi->join_phase != MIDR_JOIN_PROBING_MEMBERS) {
			MIDR_LOG("MIDR I-7：CREATE 但不在加入流程中，忽略");
			break;
		}
		/*
		 * 自建群同样走重收敛编排（与 JOIN 共用），承担两件事：
		 *   1. 改群号 + 重通告（原先手写的两行即此）；
		 *   2. **清评估期残留**——为评估候选群而灌入并探测的那批节点
		 *      （midr_nds_learn_member 置 is_adjacent=true + I-1 起探）并不属于
		 *      这个新群，reconverge 第 3 步按"邻接却已不同群"判据把它们统一
		 *      detach（I-2 停探 + 双键删 link_entry + 复位 is_adjacent），
		 *      否则 CL 稳态会把别人群的节点当成本群邻接、PM 也继续白探。
		 *      **detach 而非删除 node 条目**：p-1 全网互知下条目删了也会经泛洪
		 *      重学（白删），且跨群视图本就该在；要断的只是"邻居化"关系。
		 *      （原 TODO 措辞"清空 nodes"过度，2026-07-10 批注轮已修正为本语义。）
		 * 第 2 步 connect_group 在新群里找不到成员，自然建连 0 个；评估期未建过
		 * 会话（⑥ 先探后判），故 detach 的拆会话半边基本空转。
		 */
		midr_group_reconverge(bgp, decision->new_group_id);
		/*
		 * 创群者即首任群代表：置 GROUP_REP 位并重通告（set_capability 内部发
		 * Node NLRI）。不置位则新群是**死群**——答 MEMBER_LIST 的闸门要这个位
		 * （bgp_midr_ctrl.c）、进引导节点 rep 目录的推导也按这个位过滤
		 * （midr_rep_candidates），后来者既问不到成员表也发现不了这个群。
		 * ownership 无碍：能力位只能自己改，这里改的正是本节点自己的位。
		 * 次序在 reconverge 之后——先落定新群号再宣称代表身份，避免中间态
		 * 通告出"旧群号 + 我是代表"的错误组合。
		 * TODO（仍缺）：如何主动引导其它节点加入本新群（现依赖它们各自 join 时
		 * 经引导节点目录发现本群）。
		 */
		midr_nds_set_capability(bgp, mi->local_capabilities |
					      MIDR_CAP_GROUP_REP);
		mi->join_phase = MIDR_JOIN_IDLE;
		mi->join_in_progress = false;
		mi->join_intent = false; /* join 落定，清意图与 JOIN 一致，堵迟到响应重启 */
		/*
		 * 候选群字段回零。CREATE 是二者唯一会不同的分支：候选群（RECOMMEND
		 * 选中、探完发现链路不达标）与实建群号不是一个值，不清就会在
		 * `show midr join` 里留下"Local group-id 4 / 候选群 2"的错位残留。
		 */
		mi->join_group_id = 0;
		MIDR_FLOW_LOG("MIDR I-7：CREATE 自建群 %u（已置 GROUP_REP 位自任首任代表），加入流程结束（回稳态）",
			      decision->new_group_id);
		break;
	}
}

/* ===========================================================================
 * Group-representative directory
 *
 * Two roles share mi->rep_dir:
 *   - bootstrap node: statically configured (`midr rep group ...`), served to
 *     joining nodes in REP_LIST_RESP;
 *   - joining node: rebuilt from the bootstrap's REP_LIST_RESP.
 * =========================================================================*/

void midr_rep_dir_add(struct bgp *bgp, uint32_t group_id,
		      struct in_addr rep_transport, as_t rep_asn,
		      struct in_addr rep_rid)
{
	struct bgp_midr *mi;
	struct listnode *node;
	struct midr_rep_entry *r;

	if (!bgp || !bgp->midr_info || !bgp->midr_info->rep_dir)
		return;
	mi = bgp->midr_info;

	/* Dedup on (group_id, rep_transport); refresh ASN if it already exists.
	 * rid 只在非 0 时刷新——后到的无 rid 条目不得抹掉已知真名。 */
	for (ALL_LIST_ELEMENTS_RO(mi->rep_dir, node, r))
		if (r->group_id == group_id &&
		    r->rep_transport.s_addr == rep_transport.s_addr) {
			r->rep_asn = rep_asn;
			if (rep_rid.s_addr != INADDR_ANY)
				r->rep_rid = rep_rid;
			return;
		}

	r = XCALLOC(MTYPE_MIDR_REP_ENTRY, sizeof(*r));
	r->group_id = group_id;
	r->rep_transport = rep_transport;
	r->rep_asn = rep_asn;
	r->rep_rid = rep_rid;
	listnode_add(mi->rep_dir, r);
}

bool midr_rep_dir_del(struct bgp *bgp, uint32_t group_id,
		      struct in_addr rep_transport)
{
	struct bgp_midr *mi;
	struct listnode *node, *nnode;
	struct midr_rep_entry *r;

	if (!bgp || !bgp->midr_info || !bgp->midr_info->rep_dir)
		return false;
	mi = bgp->midr_info;

	for (ALL_LIST_ELEMENTS(mi->rep_dir, node, nnode, r))
		if (r->group_id == group_id &&
		    r->rep_transport.s_addr == rep_transport.s_addr) {
			list_delete_node(mi->rep_dir, node);
			XFREE(MTYPE_MIDR_REP_ENTRY, r);
			return true;
		}
	return false;
}

void midr_rep_dir_clear(struct bgp *bgp)
{
	struct bgp_midr *mi;
	struct listnode *node, *nnode;
	struct midr_rep_entry *r;

	if (!bgp || !bgp->midr_info || !bgp->midr_info->rep_dir)
		return;
	mi = bgp->midr_info;

	for (ALL_LIST_ELEMENTS(mi->rep_dir, node, nnode, r)) {
		list_delete_node(mi->rep_dir, node);
		XFREE(MTYPE_MIDR_REP_ENTRY, r);
	}
}

struct midr_rep_entry *midr_rep_dir_find_group(struct bgp *bgp,
					       uint32_t group_id)
{
	struct bgp_midr *mi;
	struct listnode *node;
	struct midr_rep_entry *r;

	if (!bgp || !bgp->midr_info || !bgp->midr_info->rep_dir)
		return NULL;
	mi = bgp->midr_info;

	for (ALL_LIST_ELEMENTS_RO(mi->rep_dir, node, r))
		if (r->group_id == group_id)
			return r;
	return NULL;
}

/*
 * "Table A": every non-self node in `group_id`, derived from the BGP-LS global
 * view.  Single source of truth — when BGP-LS propagation scoping changes,
 * only this function changes.  `out` collects borrowed entry pointers (caller
 * owns the list; entries stay owned by the hash).
 */
void midr_group_members(struct bgp *bgp, uint32_t group_id, struct list *out)
{
	struct midr_node_entry *entry;

	if (!bgp || !bgp->midr_info || !out)
		return;

	frr_each (midr_node_hash, &bgp->midr_info->global_view->nodes, entry) {
		if (entry->is_self)
			continue;
		if (entry->group_id != group_id)
			continue;
		listnode_add(out, entry);
	}
}

/*
 * rep 目录推导（任务甲）：收集有资格进 REP_LIST 应答的节点——GROUP_REP 位
 * + 活性 + 群号/ASN/transport 三字段可用。含 self（引导节点自兼群代表）。
 * 不走 midr_node_get_locator 回落：router-id 可能不可路由，rep 目录是新
 * 节点入网第一跳，宁缺毋黑洞（比 MEMBER_LIST 组装的回落处理更严）。
 * `out` 收 borrowed 指针（list 归调用方，条目归 hash）。
 * 目前唯一消费者 = midr_ctrl_build_rep_list；未来 bootstrap failover /
 * 目录持久化复用。数据源随传播面 backend 迁移（现 = 自有收包路径灌的
 * global_view，迁移后换第二组 remote view）。
 */
void midr_rep_candidates(struct bgp *bgp, struct list *out)
{
	struct midr_node_entry *entry;
	time_t now = monotime(NULL);

	if (!bgp || !bgp->midr_info || !out)
		return;

	frr_each (midr_node_hash, &bgp->midr_info->global_view->nodes, entry) {
		if (!midr_node_is_group_rep(entry))
			continue;
		/* 活性兜底；主防线是 expire 定时器删表（≤5s 扫描周期） */
		if (!entry->is_self &&
		    (now - entry->last_seen) > MIDR_NODE_EXPIRE_TIME)
			continue;
		if (entry->group_id == 0 || entry->asn == 0 ||
		    !entry->has_transport_addr) {
			MIDR_LOG("midr: rep 候选 %pI4 跳过 (group=%u asn=%u has_transport=%d)",
				 &entry->node_id.u.prefix4, entry->group_id,
				 entry->asn, entry->has_transport_addr);
			continue;
		}
		listnode_add(out, entry);
	}
}

/* ===========================================================================
 * Read-only getters (for external modules, e.g. ④ flooding control)
 * =========================================================================*/

struct midr_global_view *midr_get_global_view(struct bgp *bgp)
{
	if (!bgp || !bgp->midr_info)
		return NULL;
	return bgp->midr_info->global_view;
}

uint32_t midr_local_group_id(struct bgp *bgp)
{
	if (!bgp || !bgp->midr_info)
		return 0;
	return bgp->midr_info->local_group_id;
}

bool midr_node_group_id(struct bgp *bgp, const struct prefix *node_id,
			uint32_t *out)
{
	struct midr_node_entry key = {};
	struct midr_node_entry *entry;

	if (!bgp || !bgp->midr_info || !node_id || !out)
		return false;

	prefix_copy(&key.node_id, node_id);
	entry = midr_node_hash_find(&bgp->midr_info->global_view->nodes, &key);
	if (!entry)
		return false;

	*out = entry->group_id;
	return true;
}

/*
 * 按 transport 地址反查节点真名（router-id）。给 build_rep_list 应答时刻回填
 * 手配条目的 rid 用（护栏②）：手配 `midr rep group ...` 命令里没有 rid 栏，
 * 借节点表补——含 self（引导节点手配自己当代表是标准姿势，self 条目启动即有，
 * 反查必中）。查不到返回 0，wire 上 0 = 未知，收方走旧占位路径。
 */
struct in_addr midr_nds_rid_by_transport(struct bgp *bgp,
					 struct in_addr transport)
{
	struct in_addr zero = { .s_addr = INADDR_ANY };
	struct midr_node_entry *entry;

	if (!bgp || !bgp->midr_info)
		return zero;

	frr_each (midr_node_hash, &bgp->midr_info->global_view->nodes, entry) {
		if (!entry->has_transport_addr)
			continue;
		if (entry->node_id.family != AF_INET)
			continue;
		if (entry->transport_addr.s_addr == transport.s_addr)
			return entry->node_id.u.prefix4;
	}
	return zero;
}

/*
 * ⑦ `no midr neighbor` 清账入口：按地址（会话对端 = 某节点 locator）反查节点表
 * 条目，查到则走 midr_nds_detach_node 全套（I-2 停探 + 双键删 link + 清
 * is_adjacent + 拆会话），返回 true；查不到返回 false（调用方退化为只拆会话）。
 *
 * 按 locator 比对而非仅 transport：节点无 transport 时 locator 回落 router-id，
 * 两种键都能命中。detach 内部保持 static——只经此 wrapper 对 VTY 暴露一个动作，
 * 不把停探/清账的原语散出去。
 */
bool midr_nds_detach_by_locator(struct bgp *bgp, struct in_addr addr)
{
	struct midr_node_entry *entry;

	if (!bgp || !bgp->midr_info)
		return false;

	frr_each (midr_node_hash, &bgp->midr_info->global_view->nodes, entry) {
		struct prefix loc;

		midr_node_get_locator(entry, &loc);
		if (loc.family == AF_INET &&
		    loc.u.prefix4.s_addr == addr.s_addr) {
			midr_nds_detach_node(bgp, entry,
					     MIDR_STOP_GRACEFUL_SHUTDOWN, true);
			return true;
		}
	}
	return false;
}

/* ===========================================================================
 * New-node join (bootstrap) — hierarchical discovery
 *
 * Stage 0: `midr bootstrap <IP> ...` -> REP_LIST_REQ to the bootstrap (TCP
 *          list exchange).
 * Stage 1 (midr_join_on_rep_list): probe reps (I-1), pick a group, then
 *          MEMBER_LIST_REQ to that group's representative (TCP).
 * Stage 3 (in bgp_midr_ctrl.c, on MEMBER_LIST_RESP): connect every member
 *          (the representative included).
 * No BGP-LS session is opened to the bootstrap, so there is no full-table dump.
 * =========================================================================*/

/* ---------------------------------------------------------------------------
 * §8.32 bootstrap 候选清单（多候选 + failover）
 * 元素为 struct midr_bootstrap_entry；次序即尝试优先级：手配（MANUAL，按敲入
 * 顺序）在前、种子（SEED，§8.31 重启读回）在后。游标 bootstrap_cur 指向本轮
 * 正在尝试第一跳的候选；REP_LIST_REQ 重试耗尽时 ctrl 层回调
 * midr_join_bootstrap_failed()，游标后移换下一候选，全部耗尽才放弃。
 * ------------------------------------------------------------------------- */

static struct midr_bootstrap_entry *
midr_bootstrap_find(struct bgp_midr *mi, struct in_addr addr,
		    struct listnode **node_out)
{
	struct listnode *node;
	struct midr_bootstrap_entry *b;

	for (ALL_LIST_ELEMENTS_RO(mi->bootstrap_list, node, b))
		if (b->transport.s_addr == addr.s_addr) {
			if (node_out)
				*node_out = node;
			return b;
		}
	return NULL;
}

/* 候选在清单中的 1 起序号（日志/展示用）。 */
static unsigned int midr_bootstrap_index(struct bgp_midr *mi,
					 const struct listnode *target)
{
	struct listnode *node;
	unsigned int idx = 0;

	for (node = listhead(mi->bootstrap_list); node;
	     node = listnextnode(node)) {
		idx++;
		if (node == target)
			return idx;
	}
	return 0;
}

/*
 * 追加/刷新候选（去重键 = transport）。已存在只刷新 ASN/来源（MANUAL 覆盖
 * SEED，位置不动——移动会使游标悬空，不值得）；新增时 MANUAL 插在首个 SEED
 * 之前、SEED 追加到尾，维持"手配在前、种子在后"。
 */
static void midr_bootstrap_list_add(struct bgp_midr *mi, struct in_addr addr,
				    as_t asn, enum midr_bootstrap_source source)
{
	struct midr_bootstrap_entry *b;
	struct listnode *node;

	b = midr_bootstrap_find(mi, addr, NULL);
	if (b) {
		b->asn = asn;
		if (source == MIDR_BOOTSTRAP_MANUAL)
			b->source = MIDR_BOOTSTRAP_MANUAL;
		return;
	}

	b = XCALLOC(MTYPE_MIDR_BOOTSTRAP_ENTRY, sizeof(*b));
	b->transport = addr;
	b->asn = asn;
	b->source = source;

	if (source == MIDR_BOOTSTRAP_MANUAL) {
		struct midr_bootstrap_entry *cur;

		for (ALL_LIST_ELEMENTS_RO(mi->bootstrap_list, node, cur))
			if (cur->source == MIDR_BOOTSTRAP_SEED) {
				listnode_add_before(mi->bootstrap_list, node, b);
				return;
			}
	}
	listnode_add(mi->bootstrap_list, b);
}

/* 向游标当前候选发第一跳请求（REP_LIST_REQ，经 TCP 列表交换 + pending 重试）。 */
static void midr_bootstrap_start_attempt(struct bgp *bgp)
{
	struct bgp_midr *mi = bgp->midr_info;
	struct midr_bootstrap_entry *b;

	if (!mi->bootstrap_cur)
		return;
	b = listgetdata(mi->bootstrap_cur);
	midr_ctrl_send_rep_request(bgp, b->transport);
	MIDR_FLOW_LOG("MIDR JOIN: sent REP_LIST_REQ to bootstrap %pI4 [%s]（第 %u/%u 个候选）",
		      &b->transport,
		      b->source == MIDR_BOOTSTRAP_SEED ? "seed" : "manual",
		      midr_bootstrap_index(mi, mi->bootstrap_cur),
		      listcount(mi->bootstrap_list));
}

/* §8.31 种子读回回调：把库里一条种子（transport 字符串 + asn）追加进候选清单，
 * 标 SEED 来源（排手配之后）。midr_store_seed_load 逐条调本函数。 */
static void midr_bootstrap_seed_load_cb(const char *transport, uint32_t asn,
					void *arg)
{
	struct bgp *bgp = arg;
	union sockunion su;

	if (str2sockunion(transport, &su) < 0 || su.sa.sa_family != AF_INET)
		return;
	midr_bootstrap_list_add(bgp->midr_info, su.sin.sin_addr, (as_t)asn,
				MIDR_BOOTSTRAP_SEED);
}

/*
 * §8.31 种子自举定时器（启动后延迟 MIDR_BOOTSTRAP_SELF_BOOT_SECS 触发一次）：
 * 断电重启后 `midr bootstrap` 一次性命令已随进程消失、无人再敲，节点会永远闲
 * 着。这里在启动初期用读回的种子自动发起一轮加入——三关全过才动（批注 B）：
 *   ① 候选清单非空（全新机器无种子 → 不动，与现状一致）；
 *   ② 本地无群号（local_group_id==0）——已手配 group-id 的节点是既有群成员，
 *      不该自动跑去加入别的群；
 *   ③ 无人已手动敲过 `midr bootstrap`（join_intent 未起）——人工优先。
 * 三关全过后走与手动入网一样的流程（开新一轮、游标指表头、发第一跳），失败
 * 同样按 §8.32 failover 换下一颗种子。
 */
static void midr_bootstrap_self_boot_cb(struct event *t)
{
	struct bgp *bgp = EVENT_ARG(t);
	struct bgp_midr *mi = bgp->midr_info;
	struct listnode *node;
	struct midr_bootstrap_entry *b;

	if (!mi->bootstrap_list || list_isempty(mi->bootstrap_list))
		return;
	if (mi->local_group_id != 0)
		return;
	if (mi->join_intent)
		return;

	mi->join_intent = true;
	for (ALL_LIST_ELEMENTS_RO(mi->bootstrap_list, node, b))
		b->failed = false;
	mi->bootstrap_cur = listhead(mi->bootstrap_list);
	MIDR_FLOW_LOG("MIDR bootstrap：种子自举——重启后无群号且无人工命令，用 %u 个种子候选发起加入",
		      listcount(mi->bootstrap_list));
	midr_bootstrap_start_attempt(bgp);
}

void midr_join_via_bootstrap(struct bgp *bgp, const union sockunion *su,
			     as_t asn)
{
	struct bgp_midr *mi;
	struct listnode *node;
	struct midr_bootstrap_entry *b;

	if (!bgp || !bgp->midr_info || !su)
		return;

	mi = bgp->midr_info;

	if (su->sa.sa_family != AF_INET) {
		zlog_warn("MIDR JOIN: bootstrap address must be IPv4");
		return;
	}

	/* §8.32：命令语义 = 追加候选（不再是覆盖单值）。 */
	midr_bootstrap_list_add(mi, su->sin.sin_addr, asn,
				MIDR_BOOTSTRAP_MANUAL);

	/* 已有在途 join：只入列不打断——新候选排进清单，failover 轮得到它。 */
	if (mi->join_intent) {
		MIDR_FLOW_LOG("MIDR JOIN: 已有在途加入，候选 %pSU 仅入列（现共 %u 个候选）",
			      su, listcount(mi->bootstrap_list));
		return;
	}

	/* 表达一个尚未落定的加入意图：REP_LIST_RESP 到达时凭此放行进入 join。
	 * 手动换组会作废它，届时迟到的响应被丢弃（见 midr_nds_set_group_id）。 */
	mi->join_intent = true;

	/* 开新一轮：清 failed 标记，游标指向表头，向首个候选发第一跳。 */
	for (ALL_LIST_ELEMENTS_RO(mi->bootstrap_list, node, b))
		b->failed = false;
	mi->bootstrap_cur = listhead(mi->bootstrap_list);
	midr_bootstrap_start_attempt(bgp);
}

/*
 * §8.32 failover：REP_LIST_REQ 重试耗尽（"死心"）时由 ctrl 层回调（唯一调用点
 * = midr_ctrl_retx_timer 的放弃分支）。守卫：意图仍在 + 失败地址就是游标当前
 * 候选——被删候选/上一轮的残留 pending 死掉时地址对不上，静默忽略。
 * 候选耗尽时 join_intent 保留：某慢候选的迟到 REP_LIST_RESP 仍可自愈进 join
 * （与单候选旧行为一致）；手动换组照旧作废一切。
 */
void midr_join_bootstrap_failed(struct bgp *bgp, struct in_addr failed)
{
	struct bgp_midr *mi;
	struct midr_bootstrap_entry *b;

	if (!bgp || !bgp->midr_info)
		return;
	mi = bgp->midr_info;

	if (!mi->join_intent || !mi->bootstrap_cur)
		return;
	b = listgetdata(mi->bootstrap_cur);
	if (b->transport.s_addr != failed.s_addr)
		return;

	b->failed = true;
	mi->bootstrap_cur = listnextnode(mi->bootstrap_cur);
	if (mi->bootstrap_cur) {
		struct midr_bootstrap_entry *next =
			listgetdata(mi->bootstrap_cur);

		zlog_warn("MIDR bootstrap failover：%pI4 无响应，改试 %pI4（第 %u/%u 个候选）",
			  &failed, &next->transport,
			  midr_bootstrap_index(mi, mi->bootstrap_cur),
			  listcount(mi->bootstrap_list));
		midr_bootstrap_start_attempt(bgp);
	} else {
		zlog_warn("MIDR bootstrap failover：全部 %u 个候选耗尽，放弃本轮加入（意图保留，迟到响应仍可自愈；可补候选后重敲 midr bootstrap）",
			  listcount(mi->bootstrap_list));
	}
}

bool midr_bootstrap_list_del(struct bgp *bgp, struct in_addr addr)
{
	struct bgp_midr *mi;
	struct listnode *node = NULL;
	struct midr_bootstrap_entry *b;
	bool was_current = false;

	if (!bgp || !bgp->midr_info)
		return false;
	mi = bgp->midr_info;

	b = midr_bootstrap_find(mi, addr, &node);
	if (!b)
		return false;

	/* 正在尝试的被删：游标先顺移（该候选的残留 pending 死掉时，failover
	 * 守卫因地址对不上自然忽略，无需显式清 pending）。 */
	if (mi->bootstrap_cur == node) {
		mi->bootstrap_cur = listnextnode(node);
		was_current = true;
	}
	list_delete_node(mi->bootstrap_list, node);
	XFREE(MTYPE_MIDR_BOOTSTRAP_ENTRY, b);

	if (was_current && mi->join_intent) {
		if (mi->bootstrap_cur)
			midr_bootstrap_start_attempt(bgp);
		else
			zlog_warn("MIDR bootstrap：正在尝试的候选被删且已无其它候选，放弃本轮加入（意图保留）");
	}
	return true;
}

void midr_bootstrap_clear(struct bgp *bgp)
{
	struct bgp_midr *mi;
	struct listnode *node, *nnode;
	struct midr_bootstrap_entry *b;

	if (!bgp || !bgp->midr_info)
		return;
	mi = bgp->midr_info;

	mi->bootstrap_cur = NULL;
	for (ALL_LIST_ELEMENTS(mi->bootstrap_list, node, nnode, b)) {
		list_delete_node(mi->bootstrap_list, node);
		XFREE(MTYPE_MIDR_BOOTSTRAP_ENTRY, b);
	}

	/* 连带作废在途加入意图（与手动换组同款中止语义，批注点 C 已拍板）：
	 * 清空候选 = 运维明确表态"别入网了"，迟到的 REP_LIST_RESP 也被丢弃。 */
	if (mi->join_intent || mi->join_in_progress) {
		mi->join_intent = false;
		mi->join_in_progress = false;
		mi->join_phase = MIDR_JOIN_IDLE;
		mi->join_group_id = 0;
		MIDR_LOG("MIDR bootstrap：清空候选清单并作废在途加入意图");
	}
}

void midr_join_on_rep_list(struct bgp *bgp)
{
	struct bgp_midr *mi;
	struct listnode *node;
	struct midr_rep_entry *r;

	if (!bgp || !bgp->midr_info)
		return;
	mi = bgp->midr_info;

	/*
	 * 加入意图守卫：只有意图仍有效才进入 join。运维手动换组会作废意图，此后
	 * 迟到的 REP_LIST_RESP（REP_LIST_REQ 3s×5 重传可能在换组后才回来）到这里
	 * 被静默丢弃，避免 join 把运维强制切换的群号覆盖掉。
	 */
	if (!mi->join_intent) {
		MIDR_LOG("MIDR 加入：加入意图已作废（多半被手动换组中止），丢弃迟到的 REP_LIST_RESP");
		return;
	}

	if (!mi->rep_dir || list_isempty(mi->rep_dir)) {
		MIDR_FLOW_LOG("MIDR 加入：群代表目录为空，放弃加入");
		return;
	}

	/*
	 * 进入"探测群代表"阶段：此后 PM 经 I-5 回灌时按 join_phase 发
	 * REP_PROBE_DONE，交给 CL 选最优代表（I-7 RECOMMEND），再由
	 * midr_nds_on_cluster_decision 发 MEMBER_LIST_REQ 取成员列表。
	 * NDS 不再自己"取首条"选群——选群职责归 CL。
	 */
	mi->join_in_progress = true;
	mi->join_phase = MIDR_JOIN_PROBING_REPS;
	/* §8.32：第一跳完成，收起 failover 游标；候选清单保留（供展示与
	 * §8.31 种子写库）。此后目录已到手，无第一跳可 failover。 */
	mi->bootstrap_cur = NULL;

	/* I-1：对每个群代表启动探测（PM stub 同步把指标灌进 link_entry，不再 notify）。 */
	for (ALL_LIST_ELEMENTS_RO(mi->rep_dir, node, r)) {
		struct prefix locator;
		struct midr_node_entry key = {};
		struct midr_node_entry *entry;

		/*
		 * 群代表此时尚未有 BGP-LS 会话（bootstrap 只是 UDP 通道），
		 * global_view 里还没有它的条目，而 midr_pm_add_target() 要求
		 * 探测目标已存在于 global_view 才会启动探测，故先灌一条条目。
		 * 键的取法（dual-ctx 根治，协议 v2 起目录条目带真名）：
		 *   - rep_rid 非 0 → 以真名（router-id）为键。稍后该代表的
		 *     Node NLRI / MEMBER_LIST 正式条目同键命中同一条，一台机器
		 *     不再占两条表项、两个探测 ctx 抢同一地址的回复；
		 *   - rep_rid 为 0（旧引导节点 / 应答方反查不到）→ 沿用旧法：拿
		 *     transport addr 冒充 node_id 建占位条目（CL 侧兼容双查会按
		 *     transport 回落命中，行为与 v1 相同）。
		 * 守卫：rid 是自己时不按真名建（会撞 self 条目），走旧路。
		 */
		if (r->rep_rid.s_addr != INADDR_ANY &&
		    !IPV4_ADDR_SAME(&r->rep_rid, &bgp->router_id))
			midr_prefix_from_in_addr(&locator, r->rep_rid);
		else
			midr_prefix_from_in_addr(&locator, r->rep_transport);

		key.node_id = locator;
		entry = midr_node_hash_find(&mi->global_view->nodes, &key);
		if (!entry) {
			entry = XCALLOC(MTYPE_MIDR_NODE_ENTRY, sizeof(*entry));
			entry->node_id = locator;
			midr_node_hash_add(&mi->global_view->nodes, entry);
		}
		entry->asn = r->rep_asn;
		entry->group_id = r->group_id;
		entry->transport_addr = r->rep_transport;
		entry->has_transport_addr = true;
		entry->last_seen = monotime(NULL);

		/*
		 * Only the bootstrap rep (the one that received our
		 * REP_LIST_REQ) already knows who we are — bootstrap != a
		 * given rep in general (a rep_dir can list reps other than
		 * the bootstrap itself, e.g. group 2's rep here). Any other
		 * rep has never exchanged a single control message with us,
		 * so its own global_view has no entry for us at all and
		 * pm_is_known_transport() will reject every PM probe we send
		 * it. Self-announce to each rep before I-1 starts probing so
		 * midr_nds_learn_requester() seeds that entry up front
		 * (harmless — and idempotent — if the rep already knows us).
		 */
		midr_ctrl_send_announce(bgp, r->rep_transport);

		midr_pm_add_target(bgp, &locator, MIDR_SRC_BOOTSTRAP, 0);
		MIDR_FLOW_LOG("MIDR 加入：I-1 探测群代表 %pI4（群 %u）",
			      &r->rep_transport, r->group_id);
	}

	/* 延迟 MIDR_JOIN_PROBE_WAIT_SECS 秒再发 REP_PROBE_DONE，让 PM 的
	 * 长期 EWMA（α=0.05）先积累足够样本，使 CL 能区分好/坏链路。 */
	event_cancel(&mi->t_rep_probe_done);
	event_add_timer(bm->master, midr_join_rep_probe_done_cb, bgp,
			MIDR_JOIN_PROBE_WAIT_SECS, &mi->t_rep_probe_done);
	MIDR_FLOW_LOG("MIDR 加入：REP_PROBE_DONE 将在 %d 秒后触发（等待 EWMA 热身）",
		      MIDR_JOIN_PROBE_WAIT_SECS);
}

/* ===========================================================================
 * Module lifecycle
 * =========================================================================*/

void bgp_midr_init(struct bgp *bgp)
{
	struct bgp_midr *mi;

	if (bgp->inst_type != BGP_INSTANCE_TYPE_DEFAULT)
		return;

	mi = XCALLOC(MTYPE_BGP_MIDR, sizeof(*mi));
	mi->bgp = bgp;
	mi->global_view = midr_global_view_new();
	mi->probe_contexts = NULL; /* PM: created on demand (skeleton) */
	mi->local_group_id = 0;
	mi->local_capabilities = 0;
	mi->rep_dir = list_new();
	mi->bootstrap_list = list_new(); /* §8.32 候选引导节点清单 */
	mi->perf_seqno = 0;
	mi->cap_seqno = 0;

	bgp->midr_info = mi;

	/* CL registers its global-view callback */
	midr_cl_init(bgp);

	/* Open the peer-request UDP control channel (bidirectional build-up) */
	midr_ctrl_init(bgp);

	/* PM arms its periodic probe-of-connected-nodes timer (I-1 loop) */
	midr_pm_init(bgp);

	/* Arm node-table keepalive / expire-check + CL periodic-sync timers */
	event_add_timer(bm->master, midr_keepalive_timer, bgp,
			MIDR_KEEPALIVE_INTERVAL, &mi->t_keepalive);
	event_add_timer(bm->master, midr_expire_check_timer, bgp,
			MIDR_EXPIRE_CHECK_INTERVAL, &mi->t_expire_check);
	event_add_timer(bm->master, midr_periodic_sync_timer, bgp,
			MIDR_PERIODIC_SYNC_INTERVAL, &mi->t_periodic_sync);

	/* §8.31：建种子表（失败仅 warn、种子功能静默降级，主链路照跑）；读回
	 * 种子进候选清单，并挂一次性自举定时器（守卫见 self_boot_cb）。 */
	if (midr_store_init() == 0) {
		midr_store_seed_load(midr_bootstrap_seed_load_cb, bgp);
		event_add_timer(bm->master, midr_bootstrap_self_boot_cb, bgp,
				MIDR_BOOTSTRAP_SELF_BOOT_SECS,
				&mi->t_bootstrap_boot);
	}

	MIDR_LOG("MIDR: module initialized for instance %s",
		  bgp->name_pretty);
}

void bgp_midr_finish(struct bgp *bgp)
{
	struct bgp_midr *mi;

	if (!bgp || !bgp->midr_info)
		return;

	mi = bgp->midr_info;

	event_cancel(&mi->t_keepalive);
	event_cancel(&mi->t_expire_check);
	event_cancel(&mi->t_periodic_sync);
	event_cancel(&mi->t_probe_timeout);
	event_cancel(&mi->t_rep_probe_done);
	event_cancel(&mi->t_member_probe_done);
	event_cancel(&mi->t_bootstrap_boot);

	/* Stop PM periodic probe timer */
	midr_pm_finish(bgp);

	/* Close the peer-request UDP control channel */
	midr_ctrl_finish(bgp);

	midr_global_view_free(mi->global_view);
	if (mi->rep_dir) {
		midr_rep_dir_clear(bgp);
		list_delete(&mi->rep_dir);
	}
	if (mi->bootstrap_list) { /* §8.32 候选清单（条目归本文件 MTYPE） */
		struct listnode *node, *nnode;
		struct midr_bootstrap_entry *b;

		for (ALL_LIST_ELEMENTS(mi->bootstrap_list, node, nnode, b))
			XFREE(MTYPE_MIDR_BOOTSTRAP_ENTRY, b);
		list_delete(&mi->bootstrap_list);
		mi->bootstrap_cur = NULL;
	}

	XFREE(MTYPE_BGP_MIDR, mi);
	bgp->midr_info = NULL;

	MIDR_LOG("MIDR: module terminated for instance %s",
		  bgp->name_pretty);
}
