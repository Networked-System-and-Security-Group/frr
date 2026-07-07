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
#include "bgpd/bgp_debug.h"

DEFINE_MTYPE_STATIC(BGPD, BGP_MIDR, "MIDR instance");
DEFINE_MTYPE_STATIC(BGPD, MIDR_GLOBAL_VIEW, "MIDR global view");
DEFINE_MTYPE_STATIC(BGPD, MIDR_NODE_ENTRY, "MIDR node entry");
DEFINE_MTYPE_STATIC(BGPD, MIDR_LINK_ENTRY, "MIDR link entry");
DEFINE_MTYPE_STATIC(BGPD, MIDR_REP_ENTRY, "MIDR rep directory entry");

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
		/* TODO（探测生命周期）：不建邻居时此处应 I-2 停探 + 清残留
		 * link_entry；stub 下 remove_target 是空操作，待 PM 异步化统一做。 */
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
 */
void midr_nds_on_node_nlri(struct bgp *bgp, struct bgp_ls_nlri *nlri,
			   struct bgp_ls_attr *ls_attr)
{
	struct midr_global_view *gv;
	struct midr_node_entry key = {};
	struct midr_node_entry *entry;
	bool is_new = false;
	bool changed = false;

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
		if (entry->group_id != ls_attr->midr_group_id)
			changed = true;
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
		if (is_new)
			midr_nds_on_node_discovered(bgp, entry);
		else if (changed)
			midr_nds_notify_cl(bgp, MIDR_TRIGGER_NODE_CHANGE);
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

	/* I-2: stop probing by node_id (PM's probe_contexts hash key). */
	midr_pm_remove_target(bgp, &entry->node_id, MIDR_STOP_GRACEFUL_SHUTDOWN);

	midr_ctrl_on_node_remove(bgp, entry);
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

void midr_nds_set_group_id(struct bgp *bgp, uint32_t new_gid)
{
	if (!bgp || !bgp->midr_info)
		return;

	bgp->midr_info->local_group_id = new_gid;
	midr_propagate_self(bgp, MIDR_ORIGIN_GROUP_UPDATE);
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
			/* I-2: stop probing by node_id (PM's hash key). */
			midr_pm_remove_target(bgp, &entry->node_id,
					      MIDR_STOP_KEEPALIVE_TIMEOUT);
			midr_ctrl_on_node_remove(bgp, entry);
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

	midr_nds_notify_cl(bgp, MIDR_TRIGGER_PERIODIC_SYNC);

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

	if (!bgp || !bgp->midr_info || !node_id)
		return;

	mi = bgp->midr_info;

	link = midr_global_view_find_link(mi->global_view, node_id);
	if (!link) {
		link = XCALLOC(MTYPE_MIDR_LINK_ENTRY, sizeof(*link));
		prefix_copy(&link->remote_node_id, node_id);
		listnode_add(mi->global_view->links, link);
	}

	link->status = status;
	link->consecutive_failures = consecutive_failures;
	link->last_probe_time = monotime(NULL);
	if (short_term)
		link->short_term = *short_term;
	if (long_term)
		link->long_term = *long_term;

	/* E-1: write short-term metrics into BGP-LS */
	midr_e1_write_to_bgpls(bgp, link);

	/*
	 * 此处【不】notify CL。I-5 只负责更新 link_entry + E-1。把全局视图交给 CL
	 * （I-3）的时机由编排层决定：
	 *   - join 探群代表/成员：由 midr_join_on_rep_list / recv_member_list 在
	 *     "探完一整批"后显式发 REP_PROBE_DONE / MEMBER_PROBE_DONE；
	 *   - 稳态：节点表增删/能力变更发 NODE_CHANGE，周期定时器发 PERIODIC_SYNC。
	 * 这样既不让 I-5 入口掺编排判断，又消除"每条探测各 notify 一次"。
	 * TODO：① 稳态"性能显著变化才 notify(NODE_CHANGE)"的门控（落点 probe_timer，
	 *       现 stub 假数据会乱触发故不实装）；② status==DOWN 的失效确认。
	 */
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
		mi->local_group_id = decision->new_group_id;
		midr_originate_group_update(bgp, decision->new_group_id,
					    decision->old_group_id);
		/*
		 * ⑥ 入群确认后才与群内成员建连（探成员阶段只探不连）。connect_group
		 * 遍历 global_view 中 group==new_group_id 的成员（learn_member 已灌入）
		 * 发起多跳 eBGP。
		 */
		mi->join_members =
			midr_ctrl_connect_group(bgp, decision->new_group_id);
		/* join 完成回稳态：此后探测的 I-5 回灌按 NODE_CHANGE 处理，
		 * 不再是 rep/member 段。 */
		mi->join_phase = MIDR_JOIN_IDLE;
		mi->join_in_progress = false;
		MIDR_FLOW_LOG("MIDR I-7：JOIN 群 %u 完成（建连 %u 个成员），加入流程结束（回稳态）",
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
		/* 所有候选群均不满足 → 自建新群。同属 join 第二段，幂等收尾。 */
		if (mi->join_phase != MIDR_JOIN_PROBING_MEMBERS) {
			MIDR_LOG("MIDR I-7：CREATE 但不在探成员阶段，忽略");
			break;
		}
		mi->local_group_id = decision->new_group_id;
		midr_originate_group_update(bgp, decision->new_group_id, 0);
		/*
		 * TODO（CREATE 自建群待完善）：
		 *   1. 自建群时没有任何现成节点可建连（与 JOIN 不同），后续需额外处理
		 *      （如何引导其它节点加入本新群）。
		 *   2. 需清空 global_view 的 nodes / link_entry —— 之前为评估候选群而
		 *      探测/灌入的那些节点并不属于这个新群，应清掉以免污染稳态视图。
		 */
		mi->join_phase = MIDR_JOIN_IDLE;
		mi->join_in_progress = false;
		MIDR_FLOW_LOG("MIDR I-7：CREATE 自建群 %u，加入流程结束（回稳态）",
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
		      struct in_addr rep_transport, as_t rep_asn)
{
	struct bgp_midr *mi;
	struct listnode *node;
	struct midr_rep_entry *r;

	if (!bgp || !bgp->midr_info || !bgp->midr_info->rep_dir)
		return;
	mi = bgp->midr_info;

	/* Dedup on (group_id, rep_transport); refresh ASN if it already exists. */
	for (ALL_LIST_ELEMENTS_RO(mi->rep_dir, node, r))
		if (r->group_id == group_id &&
		    r->rep_transport.s_addr == rep_transport.s_addr) {
			r->rep_asn = rep_asn;
			return;
		}

	r = XCALLOC(MTYPE_MIDR_REP_ENTRY, sizeof(*r));
	r->group_id = group_id;
	r->rep_transport = rep_transport;
	r->rep_asn = rep_asn;
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

/* ===========================================================================
 * New-node join (bootstrap) — hierarchical UDP discovery
 *
 * Stage 0: `midr bootstrap <IP> ...` -> REP_LIST_REQ to the bootstrap (UDP).
 * Stage 1 (midr_join_on_rep_list): probe reps (I-1), pick a group, then
 *          MEMBER_LIST_REQ to that group's representative.
 * Stage 3 (in bgp_midr_ctrl.c, on MEMBER_LIST_RESP): connect every member
 *          (the representative included).
 * No BGP-LS session is opened to the bootstrap, so there is no full-table dump.
 * =========================================================================*/

void midr_join_via_bootstrap(struct bgp *bgp, const union sockunion *su,
			     as_t asn)
{
	struct bgp_midr *mi;

	if (!bgp || !bgp->midr_info || !su)
		return;

	mi = bgp->midr_info;

	if (su->sa.sa_family != AF_INET) {
		zlog_warn("MIDR JOIN: bootstrap address must be IPv4");
		return;
	}

	mi->bootstrap_su = *su;
	mi->bootstrap_asn = asn;
	mi->bootstrap_set = true;

	/* Ask the bootstrap for its representative directory over UDP. */
	midr_ctrl_send_rep_request(bgp, su->sin.sin_addr);
	MIDR_FLOW_LOG("MIDR JOIN: sent REP_LIST_REQ to bootstrap %pSU (UDP discovery)",
		  su);
}

void midr_join_on_rep_list(struct bgp *bgp)
{
	struct bgp_midr *mi;
	struct listnode *node;
	struct midr_rep_entry *r;

	if (!bgp || !bgp->midr_info)
		return;
	mi = bgp->midr_info;

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

	/* I-1：对每个群代表启动探测（PM stub 同步把指标灌进 link_entry，不再 notify）。 */
	for (ALL_LIST_ELEMENTS_RO(mi->rep_dir, node, r)) {
		struct prefix locator;

		midr_prefix_from_in_addr(&locator, r->rep_transport);
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

	/* Stop PM periodic probe timer */
	midr_pm_finish(bgp);

	/* Close the peer-request UDP control channel */
	midr_ctrl_finish(bgp);

	midr_global_view_free(mi->global_view);
	if (mi->rep_dir) {
		midr_rep_dir_clear(bgp);
		list_delete(&mi->rep_dir);
	}

	XFREE(MTYPE_BGP_MIDR, mi);
	bgp->midr_info = NULL;

	MIDR_LOG("MIDR: module terminated for instance %s",
		  bgp->name_pretty);
}
