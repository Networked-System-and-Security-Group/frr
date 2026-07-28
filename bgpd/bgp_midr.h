// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR core (NDS - Neighbor Discovery & Selection)
 *
 * Central MIDR instance state hung off bgp->midr_info.  Owns the global
 * view (node table + link table + group map), drives the internal
 * interfaces (I-3 / I-5 / I-7) and the BGP-LS export (E-1).
 *
 * The node table is populated from BGP-LS Node NLRIs carrying TLV 1185 /
 * TLV 1187; the liveness module owns periodic refresh and confirmation.
 */

#ifndef _FRR_BGP_MIDR_H
#define _FRR_BGP_MIDR_H

#include <time.h>

#include "typesafe.h"
#include "prefix.h"
#include "sockunion.h"
#include "frrevent.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_ls_nlri.h"
#include "bgpd/bgp_debug.h"

/*
 * MIDR debug logging helpers, gated by `debug bgp midr [discovery]`.
 *   MIDR_FLOW_LOG — hierarchical-discovery + peering flow (REP_LIST/MEMBER_LIST
 *                   /select/connect); shown on either the general switch or the
 *                   dedicated discovery channel.
 *   MIDR_LOG      — general MIDR (node table, filtered requests, stubs); shown
 *                   only on the general switch.
 * Both expand zlog_debug, so callers must include "log.h" (all MIDR .c do).
 */
#define MIDR_FLOW_LOG(...)                                                      \
	do {                                                                   \
		if (BGP_DEBUG_MIDR_FLOW)                                        \
			zlog_debug(__VA_ARGS__);                               \
	} while (0)
#define MIDR_LOG(...)                                                           \
	do {                                                                   \
		if (BGP_DEBUG(midr, MIDR))                                      \
			zlog_debug(__VA_ARGS__);                               \
	} while (0)

/* Timer intervals (seconds) */
#define MIDR_KEEPALIVE_INTERVAL	    5  /* default self Node refresh */
#define MIDR_NODE_EXPIRE_TIME	   15  /* default transition to SUSPECT */
#define MIDR_EXPIRE_CHECK_INTERVAL  5  /* default node-table scan period */
#define MIDR_PERIODIC_SYNC_INTERVAL 30 /* CL periodic re-evaluation */
#define MIDR_PM_PROBE_INTERVAL	    10 /* periodic PM probe of connected nodes */

/* Forward declarations */
struct bgp;
struct peer;
struct bgp_ls_nlri;
struct bgp_ls_attr;
struct midr_liveness;

/* ===========================================================================
 * Core data structures (see 接口实现文档 §2)
 * =========================================================================*/

/* §2.1 Per-link metrics (short-term or long-term) */
struct midr_link_metrics {
	uint32_t rtt_us;     /* RTT (microseconds), EMA-smoothed */
	uint32_t rtt_min_us; /* observation-window min RTT */
	uint32_t rtt_max_us; /* observation-window max RTT */
	double loss_rate;    /* loss rate (0.0~1.0) */
	uint32_t bw_score;   /* bandwidth score */
};

enum midr_link_status {
	MIDR_LINK_UP = 0,
	MIDR_LINK_DEGRADED = 1,
	MIDR_LINK_DOWN = 2,
};

/* §2.2 One probed link in the global view */
struct midr_link_entry {
	struct prefix remote_node_id; /* peer node IP */
	enum midr_link_status status;
	uint32_t consecutive_failures;
	time_t last_probe_time;
	struct midr_link_metrics short_term; /* second-level */
	struct midr_link_metrics long_term;  /* day/week-level */
};

PREDECL_HASH(midr_node_hash);

/* A DEAD/LEAVE node has no stable table entry.  REMOVING only guards the
 * short, re-entrant detach/free commit window.
 */
enum midr_node_liveness_state {
	MIDR_NODE_ACTIVE = 0,
	MIDR_NODE_SUSPECT = 1,
	MIDR_NODE_REMOVING = 2, /* transient re-entrancy guard */
};

enum midr_node_suspect_cause {
	MIDR_NODE_SUSPECT_NONE = 0,
	MIDR_NODE_SUSPECT_TIMEOUT = (1U << 0),
	MIDR_NODE_SUSPECT_WITHDRAW = (1U << 1),
};

/*
 * §2.3 One node in the global view.
 * Key: node_id (the node IP — same identity the old code called router_id,
 * here widened to struct prefix per the design doc).
 */
struct midr_node_entry {
	/*
	 * Hash key = node router-id (from the Node NLRI node descriptor).
	 * This MUST be the NLRI identity, because a BGP-LS WITHDRAW (MP_UNREACH)
	 * carries only the NLRI — not attributes — so the transport address
	 * (an attribute, TLV 1188) is unavailable at withdraw time and cannot
	 * serve as the key.
	 */
	struct prefix node_id;
	as_t asn;		/* AS number (needed for auto-peering) */
	uint32_t group_id;	/* TLV 1185 */
	uint32_t capabilities;	/* TLV 1187 */
	uint64_t cap_seqno;	/* capability sequence number */
	/*
	 * Real reachable address (locator, TLV 1188): what we peer with /
	 * probe / display.  Falls back to node_id (router-id) when the node
	 * does not advertise one.  Kept separate from the key so the address
	 * can change without changing identity.
	 */
	struct in_addr transport_addr;
	bool has_transport_addr;
	/*
	 * Only receipt of the node's BGP-LS Node NLRI may update last_seen.
	 * Weak MEMBER_LIST discovery and indirect ALIVE responses instead set
	 * a local, non-transitive lease.  That lease may keep this instance's
	 * entry ACTIVE, but can never be used to vote ALIVE for a third party.
	 */
	time_t last_seen;
	time_t nontransitive_alive_until;
	/*
	 * 接口设计文档 §2.3 规格字段，现实现未接线（全树零读写）——判断群
	 * 代表一律用 capabilities & MIDR_CAP_GROUP_REP（rep 目录推导/闸门
	 * 均如此）。裁撤或接线待与 CL owner 的 cap 位核对对话一并定。
	 */
	bool is_group_rep;
	bool is_self;		/* this entry describes the local node */
	/*
	 * 是否为本节点的"邻居"：仅经 BGP-LS 泛洪收到（view-only）= false；
	 * 与本节点建立了探测/会话关系 = true。CL/PM 的"邻居质量全图"只看
	 * is_adjacent==true 的子集；跨群全量信息仍可遍历整张 nodes 表。
	 * 本字段与 on_node_nlri 的逐字段更新正交，泛洪不会把它清掉。
	 */
	bool is_adjacent;

	/* Liveness confirmation state.  SUSPECT keeps the entry, links and BGP
	 * session intact, but excludes it from new protocol decisions.
	 */
	enum midr_node_liveness_state liveness_state;
	uint8_t liveness_suspect_causes;
	time_t suspect_since;

	struct midr_node_hash_item hash_item;
};

extern unsigned int midr_node_hash_key(const struct midr_node_entry *e);
extern int midr_node_hash_cmp(const struct midr_node_entry *a,
			      const struct midr_node_entry *b);

DECLARE_HASH(midr_node_hash, struct midr_node_entry, hash_item,
	     midr_node_hash_cmp, midr_node_hash_key);

/* §2.4 Complete global quality view (maintained by NDS) */
struct midr_global_view {
	struct midr_node_hash_head nodes; /* node table (keyed by node_id) */
	struct list *links;		  /* list of struct midr_link_entry */
	struct hash *groups;		  /* group_id -> list of prefix * */
};

/*
 * A group-representative directory entry.  Two roles share this structure:
 *   - on a bootstrap node: the statically-configured directory (`midr rep
 *     group ...`) served in REP_LIST_RESP;
 *   - on a joining node: the directory learned from the bootstrap's response.
 */
struct midr_rep_entry {
	uint32_t group_id;
	struct in_addr rep_transport; /* rep's reachable address (UDP + peering) */
	as_t rep_asn;
};

/* §2.5 I-3 trigger event types */
enum midr_trigger_type {
	MIDR_TRIGGER_REP_PROBE_DONE = 1,    /* rep probe finished */
	MIDR_TRIGGER_MEMBER_PROBE_DONE = 2, /* member probe finished */
	MIDR_TRIGGER_CAPABILITY_UPDATE = 3, /* TLV 1187 capability update */
	MIDR_TRIGGER_PERIODIC_SYNC = 4,	    /* periodic sync timer */
	MIDR_TRIGGER_NODE_CHANGE = 5,	    /* node join/leave/expire */
};

/* §2.6 I-7 clustering decision */
enum midr_decision_type {
	MIDR_DECISION_RECOMMEND = 1,
	MIDR_DECISION_JOIN = 2,
	MIDR_DECISION_LEAVE = 3,
	MIDR_DECISION_SPLIT = 4,
	MIDR_DECISION_CREATE = 5,
};

struct midr_node_evidence {
	struct prefix node_id;
	struct midr_link_metrics metrics;
};

struct midr_cluster_decision {
	enum midr_decision_type decision_type;
	uint32_t new_group_id;
	uint32_t old_group_id;
	struct prefix recommended_rep; /* only for RECOMMEND */
	struct list *evidence;	       /* list of struct midr_node_evidence */
};

/* §2.4 I-3 callback registered by the clustering (CL) module */
typedef void (*midr_global_view_cb)(struct bgp *bgp,
				    enum midr_trigger_type trigger,
				    const struct midr_global_view *gv);

/*
 * 新节点加入流程所处的阶段。仅用于 midr_nds_on_cluster_decision 的幂等 guard
 * （RECOMMEND 只在 PROBING_REPS、JOIN/CREATE 只在 PROBING_MEMBERS 处理）与
 * `show midr join` 展示。trigger（REP/MEMBER_PROBE_DONE）由编排层在"探完一批"
 * 后显式 notify_cl，不在 I-5 回灌时按本阶段推导。
 */
enum midr_join_phase {
	MIDR_JOIN_IDLE = 0,	   /* 未在加入流程中（稳态） */
	MIDR_JOIN_PROBING_REPS,	   /* 正在探测群代表 */
	MIDR_JOIN_PROBING_MEMBERS, /* 正在探测目标群成员 */
};

/* §2.7 MIDR instance state, hung off bgp->midr_info */
struct bgp_midr {
	struct bgp *bgp; /* back-pointer */

	/* === NDS === */
	struct midr_global_view *global_view;
	struct midr_liveness *liveness; /* opaque failure-confirmation context */

	/* === PM === */
	struct hash *probe_contexts; /* prefix -> midr_probe_ctx (PM owns) */

	/* === CL === */
	uint32_t local_group_id;     /* local group-id (TLV 1185) */
	uint32_t local_capabilities; /* local capability bitmap (TLV 1187) */
	struct list *rep_dir;	     /* list of struct midr_rep_entry (rep directory) */
	midr_global_view_cb cl_callback;

	/* === Local transport address (TLV 1188) + graceful shutdown === */
	struct in_addr local_transport_addr; /* our reachable locator */
	bool transport_addr_set;	     /* operator configured one */
	bool shutdown;			     /* graceful shutdown: stop advertising self */

	/* === TLV sequence numbers === */
	uint32_t perf_seqno; /* TLV 1186 seqno */
	uint32_t cap_seqno;  /* TLV 1187 seqno */

	/* === Timers === */
	struct event *t_periodic_sync; /* CL periodic re-evaluation */
	struct event *t_probe_timeout; /* PM probe timeout */
	struct event *t_keepalive;     /* re-originate self Node NLRI */
	struct event *t_expire_check;  /* liveness scan/confirmation tick */
	struct event *t_pm_probe;      /* periodic PM probe of connected nodes */

	/* === New-node join (bootstrap, UDP hierarchical discovery) === */
	union sockunion bootstrap_su; /* bootstrap node address */
	as_t bootstrap_asn;	      /* bootstrap node ASN */
	bool bootstrap_set;	      /* a bootstrap node is configured */
	bool join_in_progress;	      /* guard：join 进行中，等价于 join_phase != IDLE，
				       * 保留给 show midr join，由 join_phase 同步维护 */
	enum midr_join_phase join_phase; /* 加入流程阶段，决定 I-5 回灌发哪个 trigger */
	uint32_t join_group_id;	      /* group joined (for show midr join) */
	uint32_t join_members;	      /* members we initiated sessions to */

	/* === Control channel — independent of PM ===
	 * 端口 5859 上并存两条传输 (内核 TCP/UDP 端口空间独立):
	 *  - UDP: PEER_REQUEST + liveness 确认/Gossip;
	 *  - TCP 短连接: REP_LIST / MEMBER_LIST 列表交换 (传输层在
	 *    bgp_midr_ctrl_tcp.c)。
	 * 各自 socket/格式，PM 另有自己的通道。见 bgp_midr_ctrl.{c,h}。
	 */
	int ctrl_sock;		   /* UDP socket fd, -1 when closed */
	struct event *t_ctrl_read; /* read event on ctrl_sock */
	struct list *ctrl_pending; /* struct midr_ctrl_pending (retransmit) */
	struct event *t_ctrl_retx; /* PEER_REQUEST retransmit timer */

	/* TCP 列表交换通道 (bgp_midr_ctrl_tcp.c 私有管理其内部 conn 结构) */
	int ctrl_tcp_lsock;		 /* TCP 监听 fd, -1 when closed */
	struct event *t_ctrl_tcp_accept; /* accept event on ctrl_tcp_lsock */
	struct list *ctrl_tcp_conns;	 /* 活动 TCP 连接 (tcp.c 私有元素类型) */
};

/* ===========================================================================
 * Module lifecycle
 * =========================================================================*/

extern void bgp_midr_init(struct bgp *bgp);
extern void bgp_midr_finish(struct bgp *bgp);

/* ===========================================================================
 * NDS node table (migrated from the old bgp_midr_node.c)
 * =========================================================================*/

/* Receive entry: update/insert from a Node NLRI (called from bgp_ls.c).
 * 传播面单点 (2/3) —— 收包入口 backend 替换边界; 完整说明见 bgp_midr.c 定义处。 */
extern void midr_nds_on_node_nlri(struct bgp *bgp, struct bgp_ls_nlri *nlri,
				  struct bgp_ls_attr *ls_attr);

/* Treat an ordinary Node NLRI WITHDRAW as suspicion evidence. */
extern void midr_nds_on_node_withdraw(struct bgp *bgp,
				      struct bgp_ls_nlri *nlri);

/* Runtime removal is centralized here because NDS owns the node/link tables
 * and their private memory types.  The operation is idempotent.
 */
enum midr_node_remove_cause {
	MIDR_NODE_REMOVE_QUORUM_DEAD,
	MIDR_NODE_REMOVE_GRACEFUL_LEAVE,
};
extern bool midr_nds_commit_node_remove(struct bgp *bgp,
					const struct prefix *node_id,
					enum midr_node_remove_cause cause);

/* 把一个群成员（MEMBER_LIST_RESP）灌入 global_view、标记邻居并 I-1 探测 */
extern void midr_nds_learn_member(struct bgp *bgp, struct in_addr rid, as_t asn,
				  struct in_addr transport, uint32_t group_id);

/* Refresh the local self-entry after originating the local Node NLRI */
extern void midr_nds_local_node_update(struct bgp *bgp);

/* Effective locator (real reachable addr, TLV 1188; falls back to node_id) */
extern void midr_node_get_locator(const struct midr_node_entry *e,
				  struct prefix *out);

/* Update local capability / group-id and re-originate immediately */
extern void midr_nds_set_capability(struct bgp *bgp, uint32_t new_caps);
extern void midr_nds_set_group_id(struct bgp *bgp, uint32_t new_gid);

/*
 * Single entry point for advertising/withdrawing our own Node NLRI to the
 * fabric.  Centralises the shutdown guard and debug logging that were
 * previously scattered across every bgp_ls_originate_bgp_node() call site.
 * The reason only drives logging (and future differentiation); it does not
 * change semantics, except LEAVE -> withdraw and everything else -> originate.
 *
 * 传播面单点 (1/3) —— 自通告出口 backend 替换边界; 完整三点说明见
 * bgp_midr.c 的 midr_propagate_self() 定义处。
 */
enum midr_origin_reason {
	MIDR_ORIGIN_INIT,
	MIDR_ORIGIN_KEEPALIVE,
	MIDR_ORIGIN_GROUP_UPDATE,
	MIDR_ORIGIN_CAP_UPDATE,
	MIDR_ORIGIN_TRANSPORT_UPDATE,
	MIDR_ORIGIN_REJOIN,	/* re-advertise after `no midr shutdown` */
	MIDR_ORIGIN_LEAVE,	/* withdraw (graceful shutdown / leave) */
	/* reserved for the future node-failure-forwarding module */
};
extern void midr_propagate_self(struct bgp *bgp, enum midr_origin_reason reason);

/* ===========================================================================
 * Internal interfaces
 * =========================================================================*/

/* I-5: PM -> NDS, link state (short-term + long-term) update */
extern void midr_nds_on_link_update(struct bgp *bgp,
				    const struct prefix *node_id,
				    enum midr_link_status status,
				    uint32_t consecutive_failures,
				    const struct midr_link_metrics *short_term,
				    const struct midr_link_metrics *long_term);

/* I-3: NDS -> CL, hand the global view to the clustering module */
extern void midr_nds_notify_cl(struct bgp *bgp,
			       enum midr_trigger_type trigger);

/* Find the Established BGP peer whose router-id matches node_id; NULL if none.
 * Shared by E-1 (Link NLRI origination) and the periodic PM probe. */
extern struct peer *midr_node_established_peer(struct bgp *bgp,
					      const struct prefix *node_id);

/* I-7: CL -> NDS, apply a clustering decision */
extern void midr_nds_on_cluster_decision(
	struct bgp *bgp, const struct midr_cluster_decision *decision);

/* Re-originate the local Node NLRI carrying a (new) group-id (TLV 1185) */
extern void midr_originate_group_update(struct bgp *bgp, uint32_t new_group_id,
					uint32_t old_group_id);

/* ===========================================================================
 * New-node join (bootstrap)
 * =========================================================================*/

/*
 * Command O: point this node at a bootstrap node and kick off hierarchical
 * discovery — send a REP_LIST_REQ over the UDP control channel (NO BGP-LS to
 * the bootstrap, so no full-table dump).  The rest is response-driven.
 */
extern void midr_join_via_bootstrap(struct bgp *bgp, const union sockunion *su,
				    as_t asn);

/*
 * Stage 1: the bootstrap's REP_LIST_RESP has been parsed into mi->rep_dir.
 * Probe the reps (I-1), pick a group, then query that group's representative
 * for its member list (MEMBER_LIST_REQ).  Called from the ctrl recv path.
 */
extern void midr_join_on_rep_list(struct bgp *bgp);

/*
 * Collect borrowed pointers to every non-self node in `group_id` into `out`
 * (a caller-owned list).  Single source of truth for "table A"; today derived
 * from the BGP-LS global view, swappable here if propagation scoping changes.
 */
extern void midr_group_members(struct bgp *bgp, uint32_t group_id,
			       struct list *out);

/*
 * Collect borrowed pointers to nodes qualified for the served rep directory
 * (REP_LIST): GROUP_REP bit + alive + usable group/asn/transport.  Includes
 * self.  Single source of truth for the derived directory view (任务甲).
 */
extern void midr_rep_candidates(struct bgp *bgp, struct list *out);

/* Collect borrowed pointers to currently usable entries in the configured or
 * learned representative directory.  The raw directory remains intact so a
 * recovered representative can become usable again.
 */
extern void midr_rep_directory_usable(struct bgp *bgp, struct list *out);

/* Group-representative directory (bootstrap config + learned). */
extern void midr_rep_dir_add(struct bgp *bgp, uint32_t group_id,
			     struct in_addr rep_transport, as_t rep_asn);
extern bool midr_rep_dir_del(struct bgp *bgp, uint32_t group_id,
			     struct in_addr rep_transport);
extern void midr_rep_dir_clear(struct bgp *bgp);
extern struct midr_rep_entry *midr_rep_dir_find_group(struct bgp *bgp,
						      uint32_t group_id);

/* ===========================================================================
 * Read-only getters (for external modules, e.g. ④ flooding control)
 * =========================================================================*/

/* The NDS-maintained global view, or NULL if MIDR is not initialised. */
extern struct midr_global_view *midr_get_global_view(struct bgp *bgp);

/* This node's local group-id (TLV 1185); 0 if MIDR is not initialised. */
extern uint32_t midr_local_group_id(struct bgp *bgp);

/* Look up `node_id` in the node table; on hit, write its group-id to *out and
 * return true, else return false (*out untouched). */
extern bool midr_node_group_id(struct bgp *bgp, const struct prefix *node_id,
			       uint32_t *out);

#endif /* _FRR_BGP_MIDR_H */
