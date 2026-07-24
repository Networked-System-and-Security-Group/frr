// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR core (NDS - Neighbor Discovery & Selection)
 *
 * Central MIDR instance state hung off bgp->midr_info.  Owns the global
 * view (node table + link table + group map), drives the internal
 * interfaces (I-3 / I-5 / I-7) and the BGP-LS export (E-1).
 *
 * The node table is populated from BGP-LS Node NLRIs carrying TLV 1185 /
 * TLV 1187; two timers (keepalive / expire-check) keep it fresh.
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
#define MIDR_KEEPALIVE_INTERVAL	    5  /* re-originate self Node NLRI */
#define MIDR_NODE_EXPIRE_TIME	   15  /* mark gone after this long */
#define MIDR_EXPIRE_CHECK_INTERVAL  5  /* scan period for expired nodes */
#define MIDR_PERIODIC_SYNC_INTERVAL 30 /* CL periodic re-evaluation */
#define MIDR_PM_PROBE_INTERVAL	    10 /* periodic PM probe of connected nodes */
/* Seconds to wait after I-1 before firing REP/MEMBER_PROBE_DONE.  Lets the
 * long-term EWMA (α=0.05) warm up enough for CL to see a clear difference
 * between good links (RTT ≪ 20 ms) and bad links (RTT ≫ 20 ms). At α=0.05,
 * 60 samples (60s at 1 probe/s) gives 1-0.95^60 ≈ 0.954 convergence, vs.
 * 1-0.95^20 ≈ 0.641 at the old 20s — a wider safety margin between the good-
 * and bad-link RTTs before CL evaluates. */
#define MIDR_JOIN_PROBE_WAIT_SECS   60

/* ---------------------------------------------------------------------------
 * §8.21 指标变化门控（去抖）阈值 —— ⚠ 全部为粗定值，待真实网络跑出数据后校准。
 *
 * 用途：PM 每秒回灌一次指标（I-5），若无门控则每秒每邻居重编码+泛洪一条 Link
 * NLRI，绝大多数是原样重发。门控 = "显著变化才对外发/才惊动 CL"。
 * 比较基准一律是 link->sent_metrics（上次发出的快照），公式：
 *     相对变化 = |本次 − 快照| / 快照        （分母是快照，不是本次）
 *
 * RTT 用"相对 + 绝对"双条件，因为它跨数量级（内网百微秒 vs 跨群几十毫秒），
 * 单一绝对值套不住两头：214us→260us 相对超 20% 但绝对才 46us（微值抖动，
 * 不该发）；45ms→46ms 绝对差 1ms 但相对仅 2%（同样不该发）。两条都过才算变化。
 * LOSS 只用绝对差：值域固定 0-1、不随链路基线缩放，且消费侧全是绝对阈值
 * （CL 入群判 loss<5%），1 个百分点的分辨率正好匹配决策粒度，无需按链路分档。
 * ------------------------------------------------------------------------- */
#define MIDR_DEBOUNCE_RTT_REL_PCT   20	  /* RTT 相对变化门槛（%） */
#define MIDR_DEBOUNCE_RTT_ABS_US    1000  /* RTT 绝对变化门槛（微秒，1ms） */
#define MIDR_DEBOUNCE_LOSS_ABS	    0.01  /* 丢包率绝对变化门槛（1 个百分点） */
#define MIDR_DEBOUNCE_BW_REL_PCT    25	  /* 带宽分数相对变化门槛（%）。⚠ 当前
					   * 未启用：bw_score 是 rtt/loss 的派生量
					   * （bgp_midr_pm.c 公式），独立判据会给
					   * RTT 的绝对差免疫开旁路（07-23 实测假
					   * 触发）。bw 改独立测量后再启用，理由
					   * 见 bgp_midr.c 判断函数内注释。 */
#define MIDR_DEBOUNCE_MAX_SILENCE   30	  /* 兜底：距上次发送满此秒数必发一次 */

/* Forward declarations */
struct bgp;
struct peer;
struct bgp_ls_nlri;
struct bgp_ls_attr;

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

	/*
	 * §8.21 去抖状态（只此三项，无历史序列/滑窗）。基准是"上次**发出**的
	 * 那组值"而非上一次测量值——逐次比会漏慢漂移（每秒 +1ms 永远不超阈值，
	 * 累计漂 50ms 对外还是老值），与快照比则漂移积累到阈值必然触发。
	 * 平滑不在这一层：进来的 short_term 已是 PM 侧 EWMA 的产物。
	 */
	struct midr_link_metrics sent_metrics; /* 上次发出的指标快照 */
	time_t last_sent_time;		       /* 上次发出的时刻（兜底周期用） */
	bool sent_once;			       /* 是否发过（首条必发） */
};

PREDECL_HASH(midr_node_hash);

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
	time_t last_seen;	/* last keepalive timestamp (local clock) */
	/*
	 * 接口设计文档 §2.3 原有一个 is_group_rep 布尔字段，2026-07-23 裁撤：
	 * 它与 capabilities 的 GROUP_REP 位是同一事实的两个真值源，而全树只写
	 * 不读（恒为 false），留着迟早被误用。判断群代表统一走下方的
	 * midr_node_is_group_rep()——语义以访问函数形式保留。
	 */
	bool is_self;		/* this entry describes the local node */
	/*
	 * 是否为本节点的"邻居"：仅经 BGP-LS 泛洪收到（view-only）= false；
	 * 与本节点建立了探测/会话关系 = true。CL/PM 的"邻居质量全图"只看
	 * is_adjacent==true 的子集；跨群全量信息仍可遍历整张 nodes 表。
	 * 本字段与 on_node_nlri 的逐字段更新正交，泛洪不会把它清掉。
	 */
	bool is_adjacent;

	struct midr_node_hash_item hash_item;
};

/*
 * 角色判定的唯一入口（裁撤 is_group_rep 字段后的替代，见上方注释）。
 * 真值源 = 节点自己经 TLV 1187 通告的能力位；rep 目录推导、MEMBER_LIST
 * 应答闸门、show midr reps 过滤本就都按这个位判，这里只是给它一个名字。
 */
static inline bool midr_node_is_group_rep(const struct midr_node_entry *e)
{
	return e && (e->capabilities & MIDR_CAP_GROUP_REP);
}

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
	struct in_addr rep_rid; /* rep's router-id (真名)；0 = 未知（探测退回
				 * 占位路径，wire 上对应 v2 rep_rid 栏） */
};

/*
 * §8.32 bootstrap 韧性：候选引导节点（多候选 + failover）。
 * bootstrap_list 的元素；手配（MANUAL）排前、种子（SEED，§8.31 重启读回）排后，
 * 次序即尝试优先级。去重键 = transport（同地址重复添加只更新，手配覆盖种子）。
 */
enum midr_bootstrap_source {
	MIDR_BOOTSTRAP_MANUAL = 0, /* midr bootstrap 命令手配 */
	MIDR_BOOTSTRAP_SEED,	   /* 持久化种子（重启读回） */
};

struct midr_bootstrap_entry {
	struct in_addr transport; /* 引导节点可达地址 */
	as_t asn;
	enum midr_bootstrap_source source;
	bool failed; /* 本轮第一跳已尝试失败（show 展示用；开新一轮时清零） */
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
	/*
	 * doc/change.md A1：本节点当选/卸任群代表。判定算法（谁来判、何时判）
	 * 是 CL 稳态优化待办，这两个类型先落地供其接入；old_group_id/
	 * new_group_id 均取 local_group_id（角色变更不改群归属）。
	 */
	MIDR_DECISION_REP_ELECT = 6,
	MIDR_DECISION_REP_RESIGN = 7,
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

	/* === PM === */
	struct hash *probe_contexts; /* prefix -> midr_probe_ctx (PM owns) */
	int pm_sock;		     /* UDP fd for PM probing, -1 when closed */
	struct event *t_pm_read;     /* read event on pm_sock */

	/* === CL === */
	uint32_t local_group_id;     /* local group-id (TLV 1185) */
	uint32_t local_capabilities; /* local capability bitmap (TLV 1187) */
	struct list *rep_dir;	     /* list of struct midr_rep_entry (rep directory) */
	midr_global_view_cb cl_callback;
	/*
	 * B1（doc/change.md）：local_group_id 最近一次实际变化（含首次落定）
	 * 的本地时钟时间戳，唯一写手是 midr_originate_group_update()。CL 的
	 * PERIODIC_SYNC 退群判定拿它做热身闸门——群号刚变化不足
	 * MIDR_JOIN_PROBE_WAIT_SECS 秒时跳过评估，否则会在长期 EWMA 还没收
	 * 敛的链路上误判"好链路不够"，导致刚入群就抖动着又退群。
	 */
	time_t group_settled_at;

	/* === Local transport address (TLV 1188) + graceful shutdown === */
	struct in_addr local_transport_addr; /* our reachable locator */
	bool transport_addr_set;	     /* operator configured one */
	bool shutdown;			     /* graceful shutdown: stop advertising self */

	/* === TLV sequence numbers === */
	uint32_t perf_seqno; /* TLV 1186 seqno */
	uint32_t cap_seqno;  /* TLV 1187 seqno */

	/* === Timers === */
	struct event *t_periodic_sync;	  /* CL periodic re-evaluation */
	struct event *t_probe_timeout;	  /* PM probe timeout */
	struct event *t_keepalive;	  /* re-originate self Node NLRI */
	struct event *t_expire_check;	  /* scan for expired nodes */
	struct event *t_pm_probe;	  /* periodic PM probe of connected nodes */
	struct event *t_rep_probe_done;	  /* deferred REP_PROBE_DONE after EWMA warm-up */
	struct event *t_member_probe_done; /* deferred MEMBER_PROBE_DONE after EWMA warm-up */
	struct event *t_bootstrap_boot;	  /* §8.31 一次性种子自举定时器 */

	/* === New-node join (bootstrap, UDP hierarchical discovery) === */
	struct list *bootstrap_list;	/* 候选引导节点（struct midr_bootstrap_entry），
					 * 手配在前、种子在后，次序即尝试优先级（§8.32） */
	struct listnode *bootstrap_cur; /* 游标：当前正在尝试第一跳的候选；
					 * NULL = 第一跳未在尝试（未开始或已完成） */
	bool join_in_progress;	      /* guard：join 进行中，等价于 join_phase != IDLE，
				       * 保留给 show midr join，由 join_phase 同步维护 */
	bool join_intent;	      /* 存在一个尚未落定的加入意图：配 bootstrap 置起，
				       * join 落定（JOIN/CREATE 回稳态）或被手动换组作废时清。
				       * midr_join_on_rep_list 据此丢弃"意图已作废后才迟到的
				       * REP_LIST_RESP"，防止运维强制换组被 join 静默覆盖。 */
	enum midr_join_phase join_phase; /* 加入流程阶段，决定 I-5 回灌发哪个 trigger */
	uint32_t join_group_id;	      /* group joined (for show midr join) */
	uint32_t join_members;	      /* members we initiated sessions to */

	/* === Control channel — independent of PM ===
	 * 端口 5859 上并存两条传输 (内核 TCP/UDP 端口空间独立):
	 *  - UDP: PEER_REQUEST (反向建连 nudge) + ctrl_pending 重传队列;
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

/* Remove entry on a Node NLRI WITHDRAW */
extern void midr_nds_on_node_withdraw(struct bgp *bgp,
				      struct bgp_ls_nlri *nlri);

/* 把一个群成员（MEMBER_LIST_RESP）灌入 global_view、标记邻居并 I-1 探测 */
extern void midr_nds_learn_member(struct bgp *bgp, struct in_addr rid, as_t asn,
				  struct in_addr transport, uint32_t group_id);

/*
 * 收到 REP_LIST_REQ / MEMBER_LIST_REQ 时，为请求方灌入一条最小 global_view
 * 条目（仅 transport_addr，用于 PM 的 pm_is_known_transport 来源校验），不置
 * is_adjacent、不触发 I-1——请求方是否真正入群由 CL 决定，这里只是让它作为
 * "自证身份的探测来源"被接受，不代表已建立邻居关系。
 */
extern void midr_nds_learn_requester(struct bgp *bgp, struct in_addr rid,
				     as_t asn, struct in_addr transport);

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
 * §8.32 failover：ctrl 层 REP_LIST_REQ 重试耗尽（"死心"）时回调（唯一调用点
 * midr_ctrl_retx_timer 的放弃分支）。守卫通过则游标后移、向下一候选重发第一跳
 * 请求；候选耗尽则放弃本轮（join_intent 保留，迟到 RESP 仍可自愈——与单候选旧
 * 行为一致）。
 */
extern void midr_join_bootstrap_failed(struct bgp *bgp, struct in_addr failed);

/* `no midr bootstrap A.B.C.D`：删指定候选（true=找到并删除）；正在尝试的
 * 被删则顺移到下一候选。 */
extern bool midr_bootstrap_list_del(struct bgp *bgp, struct in_addr addr);

/* `no midr bootstrap`（无参）：清空候选清单并作废在途加入意图（与手动换组
 * 同款中止语义，批注点 C 已拍板）。 */
extern void midr_bootstrap_clear(struct bgp *bgp);

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

/* Group-representative directory (bootstrap config + learned).
 * rep_rid: 代表的 router-id；手配来源无从得知时传 0（wire 亦以 0 表未知）。 */
extern void midr_rep_dir_add(struct bgp *bgp, uint32_t group_id,
			     struct in_addr rep_transport, as_t rep_asn,
			     struct in_addr rep_rid);

/* 按 transport 地址在节点表反查 router-id（真名）；查不到返回 0。
 * build_rep_list 应答时刻给手配条目回填 rid 用（护栏②，批注 55）。 */
extern struct in_addr midr_nds_rid_by_transport(struct bgp *bgp,
						struct in_addr transport);

/* ⑦ `no midr neighbor` 清账：按地址反查节点表条目并 detach 全套（停探+删 link+
 * 清 is_adjacent+拆会话）。查到返回 true，查不到 false（调用方退化为只拆会话）。 */
extern bool midr_nds_detach_by_locator(struct bgp *bgp, struct in_addr addr);
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
