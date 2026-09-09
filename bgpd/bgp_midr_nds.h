// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR core (NDS - Neighbor Discovery & Selection)
 *
 * Central MIDR instance state hung off bgp->midr_nds_info.  Owns the global
 * view (node table + link table + group map), drives the internal
 * interfaces (I-1 / I-2 / I-3 / I-5 / I-7) and publishes local topology
 * facts through the second group's provider API.
 *
 * Authoritative remote nodes arrive through the second group's remote-view
 * callbacks.  Control list/request learning may add transient entries, while
 * remote-view withdraw callbacks own authoritative retirement.
 */

#ifndef _FRR_BGP_MIDR_NDS_H
#define _FRR_BGP_MIDR_NDS_H

#include <time.h>

#include "typesafe.h"
#include "prefix.h"
#include "sockunion.h"
#include "frrevent.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_ls_nlri.h"
#include "bgpd/bgp_debug.h"
#include "bgpd/bgp_midr.h" /* 第二组接口结构（remote view 回调与逆换算用） */
#include "bgpd/bgp_midr_addr.h"
#include "bgpd/bgp_midr_pm.h" /* enum midr_stop_reason（I-2 停探原因） */

/*
 * MIDR 日志两件事，别混：**级别**决定"够不够格写出去"，**频道**决定"这类啰嗦
 * 话现在想不想听"。频道（`debug bgp midr [discovery]`）**只管 debug 那一档**
 * ——zlog_warn/info/err 是裸调，开不开频道都照出。
 *
 * 两个宏都是 debug 级，区别只在受哪个开关控制：
 *   MIDR_FLOW_LOG — 分层发现流程：REP/MEMBER/BOOTSTRAP 列表交换、join 编排、
 *                   挑台建连。一次 join 几十条，故单开一路，排查 join 卡住时
 *                   不被节点表刷屏。总开关或 discovery 频道任一打开即输出。
 *   MIDR_LOG      — MIDR 通用：节点表增删、被过滤掉的请求、stub 提示。
 *                   只有总开关打开才输出。
 * 两者都展开成 zlog_debug，故调用方需 include "log.h"（各 MIDR .c 均已含）。
 *
 * 写新日志先判档，判据 = **运维视角**（先问运维要不要知道，再用频率兜底）：
 *   err   进程级失败，某功能从此不可用（建表失败、监听 socket 起不来）
 *   warn  运维**得动手**才能恢复（underlay 不通、候选耗尽脱骨干、配置冲突）
 *   info  运维不用动手，但该知道"本机现在是什么状态"（入群/建群、当选/卸任
 *         代表、挂靠建立与切换、死心与被拒）。**低频是硬条件**——按上表该进
 *         info 但每分钟出好几条的，一律压回 debug，否则把日志淹了。
 *   debug 代码内部怎么流转（收了几条名单、登记了什么账、第几次重传、走了
 *         哪个分支）
 * 一句话：info 描述"设备处于什么状态"，debug 描述"代码在做什么"。
 *
 * ⚠ 改日志**文案**前先查 frr/midr-test/ 下的判据脚本——它们直接 grep 日志串，
 *   改一个字判据就静默失效（永远匹配不上，最难查的那种）。只调级别不动文案
 *   是安全的。全案见 docs/decisions/midr-log-levels-and-channels.md。
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
#define MIDR_PERIODIC_SYNC_INTERVAL 30 /* CL periodic re-evaluation */

/*
 * MIDR overlay 会话的 BGP timers（轮 5，成员死亡共振实验后加）。
 *
 * ⚠ 临时缓解、非长久之计（用户 08-24 拍板）。这是帕累托取舍：holdtime 长 =
 * 抗物理链路震荡但死亡通知慢（默认 180s），短 = 秒级感知但链路抖动时会误拆。
 * 15s 复刻的是旧 expire 的灵敏度——件④ 把活性判定交给 BGP 之后，若沿用出厂
 * 默认 180s，"死成员留在 CL 分母里"的窗口会从 15s 拉到 180s，periodic_sync
 * （30s 一拍）必撞 → 全群成员各自独立判 3/4 < 阈值 → 集体 LEAVE → 碎群
 * （08-24 拔线实验实测，群 1 碎成 2+1+1）。根治在 CL 判据加固（滞回/
 * 阻尼/分母摘除死链），归 CL owner，知会档第 20 件。
 * 只作用于 MIDR 自建的 overlay 会话；静态 underlay 会话不动。
 */
#define MIDR_OVERLAY_KEEPALIVE 5
#define MIDR_OVERLAY_HOLDTIME  15

/*
 * 节点能力位（node->capabilities）。轮 5 从 bgp_ls_nlri.h 搬来 —— 件② 删掉
 * MIDR 自有的 BGP-LS TLV 之后，它们与 BGP-LS 再无关系，只是 MIDR 的身份标记。
 */
#define MIDR_CAP_SRV6	   (1U << 0)
#define MIDR_CAP_ROUTING   (1U << 1)
#define MIDR_CAP_BOOTSTRAP (1U << 2) /* 引导节点 */
#define MIDR_CAP_GROUP_REP (1U << 3) /* 群代表 */
/* Consecutive MIDR_PERIODIC_SYNC_INTERVAL ticks of zero established sessions
 * (group members + anchors alike) required before a node is declared
 * isolated and restarts the join flow. At 2 ticks / 30s cadence this is a 60s
 * debounce window, keeping a momentary reconnect blip from tearing down a
 * working group membership. */
#define MIDR_ISOLATION_DEBOUNCE_TICKS 2
#define MIDR_PM_PROBE_INTERVAL	    10 /* periodic PM probe of connected nodes */
/* Seconds to wait after I-1 before firing REP/MEMBER_PROBE_DONE.  Lets the
 * long-term EWMA (α=0.05) warm up enough for CL to see a clear difference
 * between good links (RTT ≪ 20 ms) and bad links (RTT ≫ 20 ms). At α=0.05,
 * 60 samples (60s at 1 probe/s) gives 1-0.95^60 ≈ 0.954 convergence, vs.
 * 1-0.95^20 ≈ 0.641 at the old 20s — a wider safety margin between the good-
 * and bad-link RTTs before CL evaluates. */
#define MIDR_JOIN_PROBE_WAIT_SECS   60

/*
 * 退网延时拆会话（秒）：`midr shutdown` 先发撤销、隔这么久再拆会话——拆了会话
 * 就没通道把撤销发出去了（记档 39 的病根）。身份回落/清表/收尾日志仍**当场**做，
 * 只有拆会话这一步延后。⚠ 判据脚本 backbone-shutdown 敲完命令 sleep 6 再查，
 * 改大于 6 会让"会话拆净"判据假失败。
 */
#define MIDR_SHUTDOWN_TEARDOWN_DELAY 5

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
					   * 见 bgp_midr_nds.c 判断函数内注释。 */

/* -------------------------------------------------------------------------
 * 挂靠（群代表 → 引导）调参，保底轮 2 批 5。
 * K 的语义 = **上限**：实挂 = min(K, 活引导数)。只剩 1 台就挂 1 台降级运行
 * （功能在、冗余没了，打 warn），不拒绝——一台总比零台强。
 * ⚠ 建议 K < 引导总数：K >= 总数时退化为"全挂"，没有备台可切，"留备份"的本意
 * 就没了（不出错，只是白设一个上限）。
 * ------------------------------------------------------------------------- */
#define MIDR_ATTACH_K 2

/*
 * 第二批候选（不在最新活引导名单里的）的建连重传次数，保底轮 2 批 6。
 * 第一批照旧吃默认重传参数（3s×5=15s）；第二批是低可信候选——名单没报
 * 它活着，多半真死了——给 3s×2=6s 快速试错，早点轮到下一台。
 * ⚠ 不能设成 1：UDP 单程丢一包就误判死心，重传的意义就没了。
 */
#define MIDR_ATTACH_RETX_SECOND 2

/* -------------------------------------------------------------------------
 * B1 断连老化调参，保底轮 2 批 5c。
 * 收的是"节点还在、这条边却断了"那一族残留（对端单侧排除我之后我这边的半边、
 * 群代表卸任后引导侧的回配半边、引导死后代表侧的半边）——节点表 expire 判的是
 * "节点还在不在"，这些一条都够不着。
 * ⚠ 计时起点 = BGP 判死那一刻，不是物理断开：明确死（进程退出 / no neighbor，
 * TCP 有 FIN·RST）秒级判死，静默死（断电、黑洞）要等 holdtime（默认 180s），
 * 故最坏清理延迟 ≈ holdtime + 本值。
 * ------------------------------------------------------------------------- */
#define MIDR_SESSION_DOWN_AGE 300 /* 断连满这么多秒即拆边销账（5min） */
#define MIDR_AGE_SCAN_MAX     16  /* 一拍最多拆几条，其余留下一拍 */

/* Forward declarations */
struct bgp;
struct peer;
struct bgp_ls_nlri;
struct bgp_ls_attr;

/* ===========================================================================
 * Core data structures (see 接口实现文档 §2)
 * =========================================================================*/

/* §2.1 Per-link metrics (short-term or long-term) */
struct midr_nds_link_metrics {
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
	struct prefix remote_node_id; /* peer's 32-bit BGP Identifier */
	enum midr_link_status status;
	uint32_t consecutive_failures;
	time_t last_probe_time;
	struct midr_nds_link_metrics short_term; /* second-level */
	struct midr_nds_link_metrics long_term;  /* day/week-level */

	/*
	 * §8.21 去抖状态（只此三项，无历史序列/滑窗）。基准是"上次**发出**的
	 * 那组值"而非上一次测量值——逐次比会漏慢漂移（每秒 +1ms 永远不超阈值，
	 * 累计漂 50ms 对外还是老值），与快照比则漂移积累到阈值必然触发。
	 * 平滑不在这一层：进来的 short_term 已是 PM 侧 EWMA 的产物。
	 */
	struct midr_nds_link_metrics sent_metrics; /* 上次发出的指标快照 */
	bool sent_once;			       /* 是否发过（首条必发） */
};

PREDECL_HASH(midr_node_hash);

/*
 * §2.3 One node in the global view.
 * Key: node_id, a 32-bit BGP Identifier stored in an AF_INET host prefix for
 * compatibility with existing hash/callback interfaces.  It is identity, not
 * a reachable IPv4 locator.
 */
struct midr_node_entry {
	/*
	 * Hash key = remote-view node_id (the peer's BGP Identifier).  The
	 * authoritative withdraw callback carries this stable identity even when
	 * the node's independently versioned locator has changed or disappeared;
	 * the locator therefore must not be used as the hash key.
	 */
	struct prefix node_id;
	as_t asn;		/* AS number (needed for auto-peering) */
	uint32_t group_id;	/* group carried by the remote Node fact */
	uint32_t capabilities;	/* capability bitmap in the remote Node fact */
	uint64_t cap_seqno;	/* capability sequence number */
	/*
	 * Real reachable address (explicit Node-fact locator): what we peer with /
	 * probe / display.  Missing transport is represented only by
	 * has_transport_addr=false; operational paths never fall back to node_id.
	 * Kept separate from the key so the address can change without changing
	 * identity.
	 */
	struct ipaddr transport_addr;
	bool has_transport_addr;
	/*
	 * 上次收到关于该节点的消息（第二组回调 / 5859 名单 / PM 回包），monotime。
	 * ⚠ **纯观测量**：件④ 起条目生死归第二组 withdraw 回调，任何判死或过滤
	 * 都不许读它（`now - last_update > X` 这种判据一律不许再写回来）。
	 */
	time_t last_update;
	/*
	 * 接口设计文档 §2.3 原有一个 is_group_rep 布尔字段，2026-07-23 裁撤：
	 * 它与 capabilities 的 GROUP_REP 位是同一事实的两个真值源，而全树只写
	 * 不读（恒为 false），留着迟早被误用。判断群代表统一走下方的
	 * midr_node_is_group_rep()——语义以访问函数形式保留。
	 */
	bool is_self;		/* this entry describes the local node */
	/*
	 * 是否为本节点的"邻居"：仅存在于远端视图（view-only）= false；
	 * 与本节点建立了探测/会话关系 = true。CL/PM 的"邻居质量全图"只看
	 * is_adjacent==true 的子集；跨群全量信息仍可遍历整张 nodes 表。
	 * 本字段与 remote-view 的逐字段更新正交，普通事实刷新不会把它清掉。
	 */
	bool is_adjacent;

	struct midr_node_hash_item hash_item;
};

/*
 * 角色判定的唯一入口（裁撤 is_group_rep 字段后的替代，见上方注释）。
 * 真值源 = 节点自己的远端 Node fact 能力位；rep 目录推导、MEMBER_LIST
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
 * One group-representative directory entry learned at runtime from a
 * bootstrap response.  A bootstrap may also merge these cached entries with
 * representatives derived from its current remote view when answering.
 */
struct midr_rep_entry {
	uint32_t group_id;
	struct ipaddr rep_transport; /* rep's reachable IPv4/IPv6 locator */
	as_t rep_asn;
	struct in_addr rep_rid; /* rep's IPv4 router-id identity；Control v4
				 * decoder rejects zero */
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
	struct ipaddr transport; /* 引导节点可达地址 */
	as_t asn;
	/*
	 * 引导节点的 router-id（保底轮 2 批 5 R 系列）。
	 * **不变量：池内 rid 恒非 0** —— 四条进池的路各自在入口把关（手配 =
	 * 命令必选参数；名单/种子 = rid 为 0 则告警跳过；NLRI 学入 = 节点表条目
	 * 天然有）。有了它才谈得上：挂靠挑台按 rid 排环（确定可重放）、按 rid
	 * 把某台引导拉黑（排除名单以 router-id 为键）、台账登记对端 rid。
	 */
	struct in_addr rid;
	enum midr_bootstrap_source source;
	bool failed; /* 本轮第一跳已尝试失败（show 展示用；开新一轮时清零） */
	/*
	 * 挂靠专用的失败标记（D2）。与上面那个 join 线的 failed **严格分开**：
	 * 两条线各有各的轮次节奏，共用一个字段会串轮（join 开新一轮会把挂靠的
	 * 记忆抹掉，反之亦然）。
	 * 置：ATTACH 建连死心（重传耗尽）或收到 PEER_REJECT；
	 * 清：下一次"开新一轮"——钩子 (a)/(b) 调 bootstrap_list_fetch 时统一清。
	 * 挑台跳过带此标记的候选，于是"绕环一圈"能真的绕过去。
	 */
	bool attach_failed;
	/*
	 * 本条目是否出现在**最新一份**活引导名单里（保底轮 2 批 6，缺口 A）。
	 * 置/清：收到一份 BOOTSTRAP_LIST_RESP 时先全清（midr_nds_bootstrap_list_begin）
	 * 再逐条置（midr_nds_bootstrap_learn）——这就是名单缺的那半"减法"。
	 * 用途：挑台分两批，在名单里的先挑。名单是**优先级不是准入资格**——引导侧
	 * 名单漏配一台只会让它排后面，不会让它永远选不上（判据⑤ 验的就是这条）。
	 * ⚠ mi->bootstrap_list_seen 为假（一份名单都没收到过）时本字段无意义，
	 *   那种情况下全部候选按第一批处理，见 midr_nds_attach_pick_from()。
	 */
	bool in_last_list;
};

/*
 * 挑台从哪一批开始（批 6 缺口 A 的 A-3）。死心重挑要**从死者所在的那一批继续**：
 * 死的是第一批的 → 从第一批接着挑（第一批试尽才轮到第二批）；死的是第二批的 →
 * 说明第一批那时已试尽，直接从第二批继续、不空扫第一批。
 */
enum midr_attach_batch {
	MIDR_ATTACH_BATCH_FIRST = 0,  /* 在最新名单里，死心阈值 3s×5 */
	MIDR_ATTACH_BATCH_SECOND = 1, /* 不在名单里，死心阈值 3s×2 */
};

/* §2.5 I-3 trigger event types */
enum midr_trigger_type {
	MIDR_TRIGGER_REP_PROBE_DONE = 1,    /* rep probe finished */
	MIDR_TRIGGER_MEMBER_PROBE_DONE = 2, /* member probe finished */
	MIDR_TRIGGER_CAPABILITY_UPDATE = 3, /* Node-fact capability update */
	MIDR_TRIGGER_PERIODIC_SYNC = 4,	    /* periodic sync timer */
	MIDR_TRIGGER_NODE_CHANGE = 5,	    /* node update/withdraw */
	/*
	 * 两个次优群的候选成员限时探测完成。NDS 在 RECOMMEND 阶段向两个次优
	 * 代表要来成员列表、探测 MIDR_JOIN_PROBE_WAIT_SECS 秒后发出，交 CL
	 * 选锚点（cl_handle_anchor_probe_done → I-7 ANCHOR）。
	 */
	MIDR_TRIGGER_ANCHOR_PROBE_DONE = 6,
	/*
	 * Established sessions (group members and anchors alike, both use
	 * the BGP-LS AF, counted together) have been at zero for
	 * MIDR_ISOLATION_DEBOUNCE_TICKS consecutive periodic checks in a
	 * row. PERIODIC_SYNC's own LEAVE judgement looks at "how good are
	 * the known adjacent members of my group" -- for an isolated node
	 * that count is 0 already, which its threshold reads as "stay", so
	 * it never catches this case. Raised from the debounce counter in
	 * midr_periodic_sync_timer(); CL responds with
	 * MIDR_DECISION_RECONNECT.
	 */
	MIDR_TRIGGER_ISOLATED = 7,
};

/* §2.6 I-7 clustering decision */
enum midr_decision_type {
	MIDR_DECISION_RECOMMEND = 1,
	MIDR_DECISION_JOIN = 2,
	MIDR_DECISION_LEAVE = 3,
	MIDR_DECISION_SPLIT = 4,
	MIDR_DECISION_CREATE = 5,
	/*
	 * 本节点当选/卸任群代表。判定算法（谁来判、何时判）是 CL 稳态优化
	 * 待办，这两个类型先落地供其接入；old_group_id/new_group_id 均取
	 * local_group_id（角色变更不改群归属）。
	 */
	MIDR_DECISION_REP_ELECT = 6,
	MIDR_DECISION_REP_RESIGN = 7,
	/*
	 * 群间锚点连接。不改变本节点的 group_id（old/new_group_id 均取
	 * local_group_id，纯记录用）——真正的内容在 evidence：至多 4 个跨群
	 * 锚点候选（node_id + 探测指标），NDS 收到后对每条直接
	 * midr_ctrl_connect()。
	 */
	MIDR_DECISION_ANCHOR = 8,
	/*
	 * Restart the join flow after total connectivity loss
	 * (MIDR_TRIGGER_ISOLATED). old_group_id takes local_group_id,
	 * new_group_id takes 0, same field usage as LEAVE. Deliberately not
	 * reusing MIDR_DECISION_LEAVE: LEAVE means "the algorithm judged
	 * link quality too low and wants to switch groups"; RECONNECT means
	 * "every session died", which isn't a quality judgement. Keeping
	 * them separate lets logs distinguish the two triggers, and avoids
	 * the LEAVE guard ("a configured group-id wins over the algorithm's
	 * opinion", bgp_midr_nds.c) accidentally blocking isolation
	 * recovery too — a manually pinned node must still be allowed to
	 * dig itself out of total isolation.
	 */
	MIDR_DECISION_RECONNECT = 9,
};

struct midr_node_evidence {
	struct prefix node_id;
	struct midr_nds_link_metrics metrics;
};

struct midr_rep_identity {
	uint32_t group_id;
	struct prefix node_id;
};

struct midr_cluster_decision {
	enum midr_decision_type decision_type;
	uint32_t new_group_id;
	uint32_t old_group_id;
	struct prefix recommended_rep_id; /* only for RECOMMEND */
	struct list *evidence;	       /* list of struct midr_node_evidence */
	/*
	 * Valid only for RECOMMEND. Contains up to two borrowed
	 * struct midr_rep_identity pointers for the second- and third-ranked
	 * representatives. The identities are valid only during the synchronous
	 * I-7 callback and contain no locator.
	 */
	struct list *anchor_reps;
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

/* 对接第二组 topology 接口的本地事实表；定义在 bgp_midr_nds_facts.h */
struct midr_nds_facts;

/* 第二组接口的 opaque 句柄（真定义在他们的 bgp_midr_private.h）；我方只透传，
 * 绝不解引用。见 bgp_midr.h（原名拷入的接口头文件）。 */
struct midr_context;

/*
 * 会话台账（方案定稿结论 20）——给每条 MIDR 会话记一笔"它为什么存在"。
 *
 * 为什么要账：轮 2 起会话来源变多（同群自动 / 骨干 / 挂靠 / CL 群间 / 手配 /
 * 应邀回配），而自动清理原本的判据是拿 midr_discovery_should_peer 现推"这条边
 * 该不该有"——同群的边推得出来，骨干/挂靠/群间的边（对端不同群）推不出来，会被
 * 当成无主边误拆。改成建的时候记原因、拆的时候查账。
 *
 * 键 = transport：账记的是"边"，随会话生灭、寿命等于会话寿命，登记那一刻
 * transport 必知而对端 router-id 未必知（手配 `midr session` 只给地址）。
 * "身份要稳"的红利归长命的"人"单 = 排除名单（键 = router-id）。两表两义两键。
 */
enum midr_session_reason {
	MIDR_SESSION_SAME_GROUP = 0, /* 同群自动互联 */
	MIDR_SESSION_ATTACH,	     /* 群代表挂靠引导 */
	MIDR_SESSION_CL_ANCHOR,	     /* CL 群间锚点（I-7 ANCHOR）决策 */
	MIDR_SESSION_MANUAL,	     /* 运维 `midr session` 手配 */
	MIDR_SESSION_PEER_REQ_REPLY, /* 应邀回配（收到 PEER_REQUEST） */
};

struct midr_session_ledger_entry {
	struct ipaddr transport; /* 键 */
	enum midr_session_reason reason;
	struct in_addr remote_rid; /* 值：认人（show / 对账"这条边对面是谁"） */
	as_t remote_asn;            /* 值：重配恢复 / MANUAL 配置写回 */
	uint32_t remote_group;	   /* 值：对端群号 */
	/*
	 * B1 断连老化（批 5c）：会话掉出 Established 的时刻（monotime），
	 * 0 = 连着 / 从未连上。字段寄生在本条目上、不是独立定时器——条目被销
	 * （拆边、实例销毁）字段随之消失，计时自然终止，无需写取消逻辑。
	 * MANUAL 账永不计时（掉线只 warn，拆边归运维）。
	 */
	time_t down_since;
};

/* Persistent operator intent from `midr session`.  Unlike the runtime
 * ledger, this list survives peer teardown, transport reconfiguration and
 * `midr shutdown`, and is the source used by the config writer. */
struct midr_manual_session {
	struct ipaddr transport;
	as_t remote_asn;
	/* May be 0 until the first successful BGP OPEN; identity is still IPv4. */
	struct in_addr remote_rid;
};

/* §2.7 MIDR instance state, hung off bgp->midr_nds_info */
struct bgp_midr_nds {
	struct bgp *bgp; /* back-pointer */

	/* === NDS === */
	struct midr_global_view *global_view;

	/* === PM === */
	struct hash *probe_contexts; /* prefix -> midr_probe_ctx (PM owns) */
	int pm_sock;		     /* UDP fd for PM probing, -1 when closed */
	struct event *t_pm_read;     /* read event on pm_sock */

	/* === CL === */
	uint32_t local_group_id;     /* local group-id in the Node fact：运行值，
				      * I-7 JOIN/CREATE 会改它 */
	uint32_t config_group_id;    /* `midr group-id N` 的配置值，0 = 未配置。
				      * 与运行值分离（决策 midr-shutdown-semantics §5.1）：
				      * FRR 不中途重读 frr.conf，被 CL 覆盖后进程里再无处
				      * 可查。两个用途：① 走 join 但群号听配置（I-7 的
				      * RECOMMEND/CREATE 分支不采纳 CL 群号）；② 退网时
				      * local_group_id 回落到它。 */
	uint32_t local_capabilities; /* local Node-fact capability bitmap */
	struct list *rep_dir;	     /* list of struct midr_rep_entry (rep directory) */
	midr_global_view_cb cl_callback;
	/*
	 * local_group_id 最近一次实际变化（含首次落定）的本地时钟时间戳，
	 * 唯一写手是 midr_originate_group_update()。CL 的 PERIODIC_SYNC 退群
	 * 判定拿它做热身闸门——群号刚变化不足 MIDR_JOIN_PROBE_WAIT_SECS 秒时
	 * 跳过评估，否则会在长期 EWMA 还没收敛的链路上误判"好链路不够"，导致
	 * 刚入群就抖动着又退群。
	 */
	time_t group_settled_at;

	/*
	 * `no midr session` 持久排除名单——list of `struct in_addr *`
	 * （router-id identity）。只影响"自动重连"
	 * （midr_ctrl_connect / midr_discovery_should_peer），不影响运维用
	 * `midr session` 手工显式重连（那条路径先移出名单）。
	 */
	struct list *session_blacklist;

	/*
	 * 会话台账——list of `struct midr_session_ledger_entry *`，键 =
	 * transport。每建一条 MIDR 会话登记一笔"建它的原因"，拆会话的两个出口
	 * （midr_try_disconnect / bgp_midr_nds_finish）顺手销账，保证账与会话不
	 * 脱节。定义与三条家规见 struct midr_session_ledger_entry 头注释与
	 * midr_nds_ledger_note()。
	 */
	struct list *session_ledger;
	/* Persistent `midr session` configuration, separate from runtime ledger. */
	struct list *manual_sessions;

	/* === Local explicit transport locator + graceful shutdown === */
	/* Configured intent survives a bind failure and is written to config. */
	struct ipaddr local_transport_addr;
	bool transport_addr_set;
	/* Runtime users may consume only a locator whose complete Control endpoint
	 * was bound successfully. */
	struct ipaddr active_transport_addr;
	bool transport_active;
	bool transport_reconfiguring; /* suppress peer-hook teardown side effects */
	bool shutdown;			     /* graceful shutdown: stop advertising self */
	/* === Fact sequence numbers === */
	uint32_t perf_seqno; /* performance sequence number */
	uint32_t cap_seqno;  /* capability sequence number */

	/* === Timers === */
	/* ⚠ 件④ 删了 keepalive/expire，**这个不能跟着删**：它还扛着递交 CL、
	 * 刷种子库、B1 超龄扫描、孤岛自救、补边兜底扫描五件事。 */
	struct event *t_periodic_sync;	  /* CL periodic re-evaluation */
	struct event *t_probe_timeout;	  /* PM probe timeout */
	struct event *t_pm_probe;	  /* periodic PM probe of connected nodes */
	struct event *t_rep_probe_done;	  /* deferred REP_PROBE_DONE after EWMA warm-up */
	struct event *t_member_probe_done; /* deferred MEMBER_PROBE_DONE after EWMA warm-up */
	struct event *t_anchor_probe_done; /* deferred ANCHOR_PROBE_DONE，见 anchor_group_id */
	struct event *t_bootstrap_boot;	  /* §8.31 一次性种子自举定时器 */
	struct event *t_attach_reap;	  /* 钩子 (b) 掉线的"下一拍"处理（D4） */
	struct event *t_session_reap;	  /* 掉沿清账的"下一拍"处理（件④） */
	struct event *t_shutdown_teardown; /* 退网延时拆会话（见 MIDR_SHUTDOWN_TEARDOWN_DELAY） */
	/*
	 * 钩子 (b) 记下的待处理掉线 transport（struct ipaddr *；同时掉线最多 K 条）。
	 * 为什么不在钩子里当场拆：FSM 喊完 peer_status_changed 之后还要回来摸这条
	 * 会话（收尾、打日志），当场 peer_delete 是重入雷区。官方三个消费者
	 * （bmp/dump/snmp）无一动 peer 生命周期，我们照同款姿势——钩子只记名，
	 * 下一拍事件里再动手（微秒级延迟，行为不变）。
	 */
	struct list *attach_down_pending;

	/* 件④：掉沿清账的待处理会话（struct midr_session_down *）。不在钩子里
	 * 当场拆的理由同上。 */
	struct list *session_down_pending;

	/*
	 * === 件③ 第二组 authoritative remote-view（轮 4）===
	 * remote_withdrawn: 最近被回调撤销的对象（struct midr_remote_withdrawn *），
	 *   用于"虚报观察"——撤销后短窗内同对象又 update 就计一笔。它只是探针：
	 *   我方按 07-30 定案走纯 BGP 判活、不为虚报预建缓冲，先看频率高不高。
	 */
	struct list *remote_withdrawn;
	bool remote_view_registered;   /* 回调已注册（幂等闸，见 register 函数） */
	bool remote_view_reg_failed;   /* 注册失败过（只为日志降噪：首次 warn，
					* 之后 periodic_sync 每拍重试压成 debug） */
	uint64_t remote_node_events;   /* node 回调到达数（update + withdraw） */
	uint64_t remote_link_events;   /* link 回调到达数（本轮只观察不消费） */
	uint64_t remote_suspect_count; /* 疑似虚报撤销（撤后短窗内又 update） */

	/* === New-node join (bootstrap, TCP list discovery) === */
	struct list *bootstrap_list;	/* 候选引导节点（struct midr_bootstrap_entry），
					 * 手配在前、种子在后，次序即尝试优先级（§8.32） */
	struct listnode *bootstrap_cur; /* 游标：当前正在尝试第一跳的候选；
					 * NULL = 第一跳未在尝试（未开始或已完成） */
	/*
	 * ② 按需拉取名单（BOOTSTRAP_LIST）兜底遍历用的**独立游标**，与上面那个
	 * join 第一跳游标严格分开：两者语义不同（那个绑 join_intent 状态机、失败
	 * 走 join failover；这个只为"找一台引导问名单"，不接 join 线，子稿 §4-3），
	 * 共用一个会互相踩掉对方的进度。NULL = 兜底遍历未在进行。
	 * ⚠ 候选被删/清空时必须一并作废（否则悬空 listnode）。
	 */
	struct listnode *bootstrap_probe_cur;
	/*
	 * 本实例**收到过**至少一份活引导名单（批 6 缺口 A）。
	 * 为假时挑台不分批（全部候选按第一批走，行为与批 6 之前完全一致）——刚开机
	 * 还没挂上任何引导时无人可问名单，此刻"谁都不在名单里"是常态而非信号，
	 * 若照分批规则办会把 conf 里配的候选全贬进第二批走 6s 低可信路。
	 */
	bool bootstrap_list_seen;
	bool join_in_progress;	      /* guard：join 进行中，等价于 join_phase != IDLE，
				       * 保留给 show midr join，由 join_phase 同步维护 */
	bool join_intent;	      /* 存在一个尚未落定的加入意图：配 bootstrap 置起，
				       * join 落定（JOIN/CREATE 回稳态）或被手动换组作废时清。
				       * midr_join_on_rep_list 据此丢弃"意图已作废后才迟到的
				       * REP_LIST_RESP"，防止运维强制换组被 join 静默覆盖。 */
	/*
	 * Debounce counter for MIDR_TRIGGER_ISOLATED: midr_periodic_sync_timer()
	 * increments it each time it finds zero established sessions, resets
	 * it to 0 the moment it finds even one, and fires (then resets) once
	 * it reaches MIDR_ISOLATION_DEBOUNCE_TICKS.
	 */
	uint32_t isolated_ticks;
	enum midr_join_phase join_phase; /* 加入流程阶段，决定 I-5 回灌发哪个 trigger */
	uint32_t join_group_id;	      /* group joined (for show midr join) */
	uint32_t join_members;	      /* members we initiated sessions to */

	/*
	 * 正在评估的两个次优群号（RECOMMEND 时从 CL 回灌的 anchor_reps 记
	 * 下），0 = 该槽位无候选。NDS 向这两个群的代表发 MEMBER_LIST_REQ、
	 * 限时探测后发 ANCHOR_PROBE_DONE，CL 据此重新从 gv->nodes 里按群号
	 * 筛候选（见 cl_handle_anchor_probe_done）。
	 */
	uint32_t anchor_group_id[2];

	/* === Control channel — independent of PM ===
	 * 端口 5859 上并存两条传输 (内核 TCP/UDP 端口空间独立):
	 *  - UDP: 固定长 PEER/ATTACH/REJECT/ANNOUNCE 消息 + ctrl_pending 重传;
	 *  - TCP 短连接: REP_LIST / MEMBER_LIST / BOOTSTRAP_LIST 列表交换 (传输层在
	 *    bgp_midr_ctrl_tcp.c)。
	 * 各自 socket/格式，PM 另有自己的通道。见 bgp_midr_ctrl.{c,h}。
	 */
	int ctrl_sock;		   /* UDP socket fd, -1 when closed */
	struct event *t_ctrl_read; /* read event on ctrl_sock */
	struct list *ctrl_pending; /* struct midr_ctrl_pending (retransmit) */
	struct event *t_ctrl_retx; /* PEER_REQUEST retransmit timer */
	/* 上次武装 t_ctrl_retx 用的间隔（毫秒）= 本跳实际经过的时间。回调靠它给
	 * 全队 due_ms 统一记账；间隔本身每跳取全队 due_ms 的最小值。 */
	int ctrl_retx_tick_ms;

	/* TCP 列表交换通道 (bgp_midr_ctrl_tcp.c 私有管理其内部 conn 结构) */
	int ctrl_tcp_lsock;		 /* TCP 监听 fd, -1 when closed */
	struct event *t_ctrl_tcp_accept; /* accept event on ctrl_tcp_lsock */
	struct list *ctrl_tcp_conns;	 /* 活动 TCP 连接 (tcp.c 私有元素类型) */

	/* === 对接第二组 topology 接口：本地事实表 === */
	struct midr_nds_facts *facts; /* node×1 + 出向 link 表 + 每对象 version；
				       * 定义与说明在 bgp_midr_nds_facts.{c,h} */

	/* 第二组接口句柄：bgp_midr_nds_init() 经 midr_context_get_default() 取一次
	 * 存下，之后每次调 midr_topology_* 原样透传（计划 Q7），**绝不解引用**。
	 * 取不到时由 midr_nds_group2_ctx() 兜底重取（真实现可能晚于我方 init
	 * 才就绪；shim 的哨兵恒非 NULL）。 */
	struct midr_context *g2_ctx;
};

/* ===========================================================================
 * Module lifecycle
 * =========================================================================*/

extern void bgp_midr_nds_init(struct bgp *bgp);
extern void bgp_midr_nds_finish(struct bgp *bgp);

/* ===========================================================================
 * NDS node table (migrated from the old bgp_midr_node.c)
 * =========================================================================*/

/* 〔件②（轮 4）删除 midr_nds_on_node_nlri() / midr_nds_on_node_withdraw()：
 * 节点表不再从我方自有的 BGP-LS 收包路径进料，唯一数据源是第二组的 remote-view
 * 回调（bgp_midr_nds.c 文件末尾）。〕 */

/*
 * 下行逆换算单一出口（第二组 remote 结构 → 我方条目字段）：增量回调与
 * `show midr group2-remote` 对账共用，口径只此一份。
 */
extern void midr_nds_remote_node_decode(const struct midr_remote_node_info *node,
					struct in_addr *rid, uint32_t *caps,
					struct ipaddr *transport,
					bool *has_transport);

/* 收包侧反应链（数据源无关）：第二组 remote-view 回调进来后走这条。 */
extern void midr_nds_node_react(struct bgp *bgp, struct midr_node_entry *entry,
				bool is_new, bool changed, bool group_changed,
				uint32_t prev_gid);

/* 虚报观察：被 remote-view 回调撤销过的对象，撤销时刻记一笔（墙钟无关，用
 * monotime）。撤后 MIDR_REMOTE_SUSPECT_WINDOW 内同对象又 update 即计一次疑似虚报。 */
struct midr_remote_withdrawn {
	struct in_addr rid;
	time_t at;
};
#define MIDR_REMOTE_SUSPECT_WINDOW 30

/* 把一个群成员（MEMBER_LIST_RESP）灌入 global_view、标记邻居并 I-1 探测 */
extern void midr_nds_learn_member(struct bgp *bgp, struct in_addr rid, as_t asn,
				  struct ipaddr transport,
				  uint32_t group_id);

/* 同上，但不标邻居——锚点候选是跨群评估节点，不是本群成员。 */
extern void midr_nds_learn_anchor_candidate(struct bgp *bgp,
					    struct in_addr rid, as_t asn,
					    struct ipaddr transport,
					    uint32_t group_id);

/* 把一个同群对端纳入本群邻居：查建条目 + 标 is_adjacent + I-1 起探。删探测 A 后
 * "老成员认识新成员"的唯一入口；详见定义处。 */
extern void midr_nds_adopt_group_peer(struct bgp *bgp, struct in_addr rid,
				      as_t asn,
				      struct ipaddr transport,
				      uint32_t group_id);

/* 反面：一条边没了之后按 transport 反查节点表做完整清理（停探 + 撤链路上报 +
 * 清 link_entry + 清 is_adjacent，不拆会话）。死心与拆边两处共用。
 * reason 只透传给 I-2 停探进日志。 */
extern void midr_nds_cleanup_by_transport(struct bgp *bgp,
					  struct ipaddr transport,
					  enum midr_stop_reason reason);

/*
 * 收到 REP_LIST_REQ / MEMBER_LIST_REQ 时，为请求方灌入一条最小 global_view
 * 条目（仅 transport_addr，用于 PM 的 pm_is_known_transport 来源校验），不置
 * is_adjacent、不触发 I-1——请求方是否真正入群由 CL 决定，这里只是让它作为
 * "自证身份的探测来源"被接受，不代表已建立邻居关系。
 */
extern void midr_nds_learn_requester(struct bgp *bgp, struct in_addr rid,
				     as_t asn,
				     struct ipaddr transport);

/* Refresh the local self-entry before publishing the local Node fact. */
extern void midr_nds_local_node_update(struct bgp *bgp);

/* Explicit reachable locator from the Node fact; never falls back to node_id. */
extern void midr_node_get_locator(const struct midr_node_entry *e,
				  struct prefix *out);

/* Update local capability / group-id and refresh the topology fact immediately. */
extern void midr_nds_set_capability(struct bgp *bgp, uint32_t new_caps);
extern void midr_nds_set_group_id(struct bgp *bgp, uint32_t new_gid);

/*
 * 清锚点评估上下文（备选群号 + 60s 热身定时器）。挂在**异常路径**收尾：退网、
 * 手动改组、重开 join round、中止 join。
 * ⚠ 绝不能挂 midr_group_reconverge——I-7 JOIN 正常入网也走它，那时锚点评估
 * 合法在途，清了等于群间锚点边永远建不起来。详见函数定义处注释。
 */
extern void midr_nds_anchor_ctx_clear(struct bgp *bgp);

/*
 * 退网 / 重入（决策 midr-shutdown-semantics）。`[no] midr shutdown` 的全部动作，
 * vty 层只负责回显。
 *
 * enter：置 shutdown 位 → 撤自身通告 → 凭台账拆光自建会话（含 MANUAL，运维意志）
 *        → 全量停探 → 清节点表（除 self）→ 群号回落配置值 + 清群代表位 / join /
 *        锚点残留。bgpd 与 underlay 一动不动。
 *        返回**被拆掉的 MANUAL 会话条数**，供 vty 说明暂时停用。
 * exit ：清位 → 重新通告 → 恢复持久 MANUAL 会话 → 走与新节点相同的 join。
 *        返回是否真的发起了加入（候选清单为空时只 warn 并返回 false）。
 */
extern unsigned int midr_nds_shutdown_enter(struct bgp *bgp);
extern bool midr_nds_shutdown_exit(struct bgp *bgp);

/*
 * 上报原因：只驱动日志（"这笔身份变化因何而起"），不改变语义。
 *
 * 唯一消费者是 midr_nds_report_node()（bgp_midr_nds_facts.h）——件②（轮 4）删掉
 * 自有 BGP-LS 自通告出口 midr_propagate_self() 之后，身份上报只剩事实表 +
 * 第二组 node_upsert/_withdraw 这一条路。
 */
enum midr_origin_reason {
	MIDR_ORIGIN_INIT,
	MIDR_ORIGIN_GROUP_UPDATE,
	MIDR_ORIGIN_CAP_UPDATE,
	MIDR_ORIGIN_TRANSPORT_UPDATE,
	MIDR_ORIGIN_REJOIN,	/* re-advertise after `no midr shutdown` */
	MIDR_ORIGIN_LEAVE,	/* withdraw (graceful shutdown / leave) */
	/* reserved for the future node-failure-forwarding module */
};
extern const char *midr_origin_reason_str(enum midr_origin_reason reason);

/* ===========================================================================
 * Internal interfaces
 * =========================================================================*/

/* I-5: PM -> NDS, link state (short-term + long-term) update */
extern void midr_nds_on_link_update(struct bgp *bgp,
				    const struct prefix *node_id,
				    enum midr_link_status status,
				    uint32_t consecutive_failures,
				    const struct midr_nds_link_metrics *short_term,
				    const struct midr_nds_link_metrics *long_term);

/* I-3: NDS -> CL, hand the global view to the clustering module */
extern void midr_nds_notify_cl(struct bgp *bgp,
			       enum midr_trigger_type trigger);

/* CL 读节点表的唯一入口：返回滤掉群号 0（引导 / 尚未入群）后的节点清单。
 * 恒非 NULL；元素是节点表条目的借用指针，用完 list_delete() 只释放链表本身。 */
extern struct list *
midr_nds_cl_nodes_getter(const struct midr_global_view *gv);

/* Find the Established BGP peer whose router-id matches node_id; NULL if none.
 * Shared by E-1 (Link NLRI origination) and the periodic PM probe. */
extern struct peer *midr_node_established_peer(struct bgp *bgp,
					      const struct prefix *node_id);

/* I-7: CL -> NDS, apply a clustering decision */
extern void midr_nds_on_cluster_decision(
	struct bgp *bgp, const struct midr_cluster_decision *decision);

/* Refresh the local Node fact after a group-id change. */
extern void midr_originate_group_update(struct bgp *bgp, uint32_t new_group_id,
					uint32_t old_group_id);

/* ===========================================================================
 * New-node join (bootstrap)
 * =========================================================================*/

/*
 * Command O: point this node at a bootstrap node and kick off hierarchical
 * discovery — send a REP_LIST_REQ over a TCP control connection (NO MIDR-LS to
 * the bootstrap, so no full-table dump).  The rest is response-driven.
 */
extern void midr_join_via_bootstrap(struct bgp *bgp, const union sockunion *su,
				    as_t asn, struct in_addr rid);

/*
 * §8.32 failover：ctrl 层 REP_LIST_REQ 重试耗尽（"死心"）时回调（唯一调用点
 * midr_ctrl_retx_timer 的放弃分支）。守卫通过则游标后移、向下一候选重发第一跳
 * 请求；候选耗尽则放弃本轮（join_intent 保留，迟到 RESP 仍可自愈——与单候选旧
 * 行为一致）。
 */
extern void midr_join_bootstrap_failed(struct bgp *bgp,
				       struct ipaddr failed);

/* `no midr bootstrap <locator>`：删指定候选（true=找到并删除）；正在尝试的
 * 被删则顺移到下一候选。 */
extern bool midr_bootstrap_list_del(struct bgp *bgp,
				    struct ipaddr addr);

/* `no midr bootstrap`（无参）：清空候选清单并作废在途加入意图（与手动换组
 * 同款中止语义，批注点 C 已拍板）。 */
extern void midr_bootstrap_clear(struct bgp *bgp);

/* ---------------------------------------------------------------------------
 * ② 按需拉取活引导名单（BOOTSTRAP_LIST；子稿《引导节点更新-对接稿》§2②/§4-3）
 *
 * 三层机制的第②层：群代表**在需要时**向一台引导问"当下活着的引导有哪些"，
 * 拿到就用、用完即弃（不进 global_view，不常驻定时器）。两个触发钩子：
 *   (a) 本机 GROUP_REP 位翻真（刚当上代表）→ 拉名单 → 哈希挑 K 台挂靠；
 *   (b) 已挂靠的引导会话 Down → 拉新名单 → 重挑一台补上。
 * ⚠ 两个钩子的**调用点在保底轮 2 批 5**（挂在 midr_nds_set_capability setter 与
 *   会话状态钩子里）——本轮（批 3）只落地机制本体：消息、应答、吸收、兜底遍历。
 *   在此之前 fetch 无人触发，属预期；批 5 验收清单里有这两条。
 * -------------------------------------------------------------------------*/

/*
 * 钩子 (a)(b) 共用的"去要一份名单"入口。选谁问，两级：
 *   ① 优先问**已挂靠且会话活着**的引导（台账 ATTACH 且 peer Established）——
 *      钩子 (b) 的常态：死了一台，问还活着的那台要新名单；
 *   ② 一个都没有 → 落**兜底遍历**（子稿 §4-3 点名必做的"防代表孤岛最后防线"）：
 *      从候选池头部起挨个试问，失败换下一个。刚当上代表（钩子 a，还没有任何挂靠）
 *      走的也是这条。
 * 不接 join 线：不碰 join_intent/join_phase/bootstrap_cur（节点已在群里）。
 */
extern void midr_nds_bootstrap_list_fetch(struct bgp *bgp);

/*
 * ctrl 层收到 BOOTSTRAP_LIST_RESP 的一条条目时回调：进候选池（SEED 来源）+ 落
 * 种子库。这是当前种子库填充的主路；专职引导不发布自身 Node fact，旧的
 * periodic_sync 节点表遍历只在权威视图未来明确提供引导条目时作为兼容补充。
 */
extern void midr_nds_bootstrap_learn(struct bgp *bgp,
				     struct ipaddr transport, as_t asn,
				     struct in_addr rid);

/*
 * 一份名单**开始**收之前调（批 6 缺口 A）：把候选池里所有 in_last_list 清掉，
 * 随后的 learn 逐条置回——名单从此有加有减，"这台还活着吗"才问得出答案。
 * ⚠ 必须在第一条 learn 之前调（不能放收完的 on_bootstrap_list 里，那时清就把
 *   刚学到的一起清了）。
 */
extern void midr_nds_bootstrap_list_begin(struct bgp *bgp);

/* 一份名单收完后的汇总回调（src = 应答方，count = 条目数）。批 5 在此接
 * "哈希顺次取 K 台挂靠"。 */
extern void midr_nds_on_bootstrap_list(struct bgp *bgp,
				       struct ipaddr src,
				       unsigned int count);

/*
 * 挂靠挑台（批 5）：候选池按 rid 排环 → hash(本机 rid) % N 定起点 → 顺次取，
 * 补到 min(K, 活引导数) 为止。名单到手时调用；本机非群代表则直接返回。
 * **分两批挑**（批 6 缺口 A）：先挑在最新名单里的，不够再挑名单外的。
 * 本函数 = 从第一批开始（钩子 a/b、收名单、config_end 补拉都走它）。
 */
extern void midr_nds_attach_pick(struct bgp *bgp);

/* 同上，但指定从哪一批开始——死心重挑专用（A-3），见 enum midr_attach_batch。 */
extern void midr_nds_attach_pick_from(struct bgp *bgp,
				      enum midr_attach_batch from);

/*
 * 一条挂靠建连失败（ATTACH_REQUEST 重传耗尽，或对端回了 PEER_REJECT）：给该候选
 * 盖 attach_failed 章，挑台从此绕开它，直到下一轮拉名单时清章（D2 failover）。
 * **只置状态、不动作**——重挑由调用方在安全点自行调 midr_nds_attach_pick_from()：
 * pick 会 connect、往 ctrl_pending 追加条目，在遍历那张表的循环里调必崩。
 * 返回死者所在的批次，调用方原样传给 pick_from（A-3：从死者那批继续）；候选池里
 * 查不到该地址时返回 FIRST（保守：宁可多扫一遍第一批，也不跳过还没试的候选）。
 */
extern enum midr_attach_batch
midr_nds_attach_mark_failed(struct bgp *bgp,
			    struct ipaddr transport);

/*
 * 本机是否为 MIDR 引导节点（带 BOOTSTRAP 能力位）。
 * 给 MIDR 之外的模块用（如 bgp_vty.c 的 distribute 命令提示），免得它们去摸
 * 实例结构体内部；MIDR 内部自己判位即可，不必绕这个函数。
 */
extern bool midr_nds_is_bootstrap(struct bgp *bgp);

/*
 * 这条链路是不是「保底边」——即对端是引导节点（骨干互连 / 挂靠 / 手配指向引导）。
 *
 * 用途：轮 2 起 link 上报出口据此跳过保底边（执行总纲"对接第二组携带项"的硬
 * 要求，已向第二组预告）。他们发送端无闸，我方若把保底链路 upsert 出去，会全网
 * 泛洪一批各节点 lsdb 最终判 unusable 的无效 NLRI。
 *
 * 判据三选一命中即真，**顺序即可靠性**：
 *   ① 引导候选池按 rid 命中 —— 不依赖 NLRI 传播时机（引导刚起、链路没通时节点表
 *      里还没有它），也正好补上台账认不出的 MANUAL 盲区（台账只记"运维手配"、
 *      不记对端身份）；
 *   ② 会话台账 reason == ATTACH —— 自动建的保底边一律有账，按条目
 *      里的 remote_rid 认人（不用 transport 反查，省一次节点表查询）；
 *   ③ 节点表条目带 BOOTSTRAP 位 —— ⚠ **件②（轮 4）起恒假**：换第二组数据源后
 *      引导（群号 0）在他们侧是 pending、不回灌，节点表里没有引导条目。①② 足以
 *      兜住，本条留着当将来引导进视图时的自动兜底。
 */
extern bool midr_nds_link_is_backbone(struct bgp *bgp,
				      const struct prefix *remote_node_id);

/*
 * 把"引导节点该有的样子"坐实（幂等；非引导直接返回）：清群代表位、清群号、
 * 撤销本机 Node/Link 拓扑事实。冲突配置一律**引导优先**，被清的项各打一条 warn。
 * 调用点两个：`bgp_config_end` 钩子（配置期收尾，治 conf 行序）与 vty 的
 * `midr role bootstrap`（运行期当场生效）。
 */
extern void midr_nds_bootstrap_enforce(struct bgp *bgp);

/* 拆掉全部挂靠边（卸任清位时调用）。只认台账 reason == ATTACH 的条目，
 * 同群/骨干/锚点/手配边一概不碰。 */
extern void midr_nds_attach_detach_all(struct bgp *bgp);

/*
 * 钩子 (b)（批 5）：一条挂靠会话掉出 Established 时调用——**先拆死掉的旧边**
 * （不拆的话代表身上会堆积死引导的半边），再拉一份新名单重挑一台补上。
 * 注册在 FRR 的 peer_status_changed 钩子上，全局只注册一次。
 */
extern void midr_nds_attach_on_session_down(struct bgp *bgp,
					    struct ipaddr transport);

/*
 * 兜底遍历的"死心"回调（唯一调用点 = midr_ctrl_retx_timer 放弃分支里
 * BOOTSTRAP_LIST_REQ 那支）：当前候选问不到 → 换下一个；全部耗尽 → 响亮 warn、
 * 停在"脱骨干"态，**不加低频重试定时器**（定稿结论 9 纯事件驱动，"实测发现漏
 * 再加"；批 6 实验专门造一次全灭场景验证人工命令能拉回来）。
 */
extern void midr_nds_bootstrap_list_failed(struct bgp *bgp,
					   struct ipaddr failed);

/*
 * Stage 1: the bootstrap's REP_LIST_RESP has been parsed into mi->rep_dir.
 * Probe the reps (I-1), pick a group, then query that group's representative
 * for its member list (MEMBER_LIST_REQ).  Called from the ctrl recv path.
 */
extern void midr_join_on_rep_list(struct bgp *bgp);

/*
 * Collect borrowed pointers to every non-self node in `group_id` into `out`
 * (a caller-owned list).  Single source of truth for "table A"; today derived
 * from the MIDR global view, swappable here if propagation scoping changes.
 */
extern void midr_group_members(struct bgp *bgp, uint32_t group_id,
			       struct list *out);

/*
 * Collect borrowed pointers to nodes qualified for the served rep directory
 * (REP_LIST): GROUP_REP bit + alive + usable group/transport.  Includes
 * self.  Single source of truth for the derived directory view (任务甲).
 */
extern void midr_rep_candidates(struct bgp *bgp, struct list *out);

/* Learned group-representative directory.  Control v4 requires nonzero
 * rep_rid and a valid IPv4/IPv6 locator for every entry. */
extern void midr_rep_dir_add(struct bgp *bgp, uint32_t group_id,
			     struct ipaddr rep_transport, as_t rep_asn,
			     struct in_addr rep_rid);

/* 按 transport 在 bootstrap 候选池反查 router-id；查不到返回 0（批 5 R 系列）。
 * `no midr session <IP>` 反查链的第三级——引导节点前两级都翻不到（不发 NLRI、
 * 半边会话无 remote_id），全靠这一级才拉得黑。 */
extern struct in_addr midr_nds_bootstrap_rid_by_transport(struct bgp *bgp,
							  struct ipaddr transport);

/* 按 transport 地址在节点表反查 router-id（真名）；查不到返回 0。供 VTY 在
 * peer 尚无 remote_id 时将 locator 解析回排除名单使用的稳定身份。 */
extern struct in_addr midr_nds_rid_by_transport(struct bgp *bgp,
						struct ipaddr transport);

/* ⑦ `no midr neighbor` 清账：按地址反查节点表条目并 detach 全套（停探+删 link+
 * 清 is_adjacent+拆会话）。查到返回 true，查不到 false（调用方退化为只拆会话）。 */
extern bool midr_nds_detach_by_locator(struct bgp *bgp,
				       struct ipaddr addr);

/*
 * `no midr session` 持久排除名单存取。**键 = router-id（真名）**（2026-08-11
 * 批 2 由地址换来：排除记的是"这个人别再连"，改 transport 不该让拉黑失效）。
 * add/del 由 VTY 的 `no midr session` / `midr session` 调用（那两条命令的入参
 * 是地址，需先解析成 rid）；is_excluded 有三个读点：midr_discovery_should_peer
 * （本文件，发起侧）、midr_ctrl_connect（bgp_midr_ctrl.c，建连前最后一道）、
 * PEER_REQUEST 接收侧闸门（bgp_midr_ctrl.c，08-11 批 2 新增，报文自带
 * requester_rid 正好对上键）。三处一道都不能丢。
 */
extern bool midr_nds_is_session_excluded(struct bgp *bgp, struct in_addr rid);
extern void midr_nds_session_exclude_add(struct bgp *bgp, struct in_addr rid);
extern void midr_nds_session_exclude_del(struct bgp *bgp, struct in_addr rid);
/* 名单非空？只给 VTY 判断"要不要为解析不出 rid 而告警"用。 */
extern bool midr_nds_session_blacklist_nonempty(struct bgp *bgp);

/*
 * 发起侧成员判断：这个节点是不是"我该自动互联的同群成员"（同群 ∧ 非 self ∧
 * 排除 group-0 ∧ 不在排除名单）。
 *
 * **语义定性 = 成员判断，不是闸门**（方案定稿结论 15）：跨群的路——CL 的
 * ANCHOR、骨干互连、代表挂靠——一律**不经过它**，直接走 midr_ctrl_connect
 * （只过排除名单 + 卫生兜底）。想放跨群就把这里改宽，等于把它变回总闸门，
 * 是定稿明确否掉的方向；改窄则同群自动互联漏边。
 *
 * 调用方：发现链 midr_nds_on_node_discovered、编排两处判断、以及
 * midr_ctrl_connect_group 逐成员过它（结论 15 点名的终态调用形状，故本函数
 * 对外可见；名字维持不改）。
 */
extern bool midr_discovery_should_peer(struct bgp *bgp,
				       const struct midr_node_entry *entry);

/*
 * 会话台账存取（结论 20）。三条家规体现在这里：
 *   ① 登记在建连函数的去重检查【之前】——会话可能早已存在（手工建过、复用了
 *      原生会话），这时不必再建，但"这条边承担着某用途"这笔账必须记上，否则
 *      将来凭账认边时它无名无姓、被清理流程当无主边误拆；
 *   ② MANUAL 粘性——运维点名的原因不被后续自动流程覆盖（事实字段照更新）；
 *   ③ 复用原生会话的边照记账，⑦ 守卫拒拆会话本体时账照销——由拆口
 *      （midr_try_disconnect 开头）无条件销账实现，不需要账里存标记
 *      〔08-10 定案：原 `reused` 字段已删。复用会话根本不打 OVERLAY 标
 *      （两道去重直接 return，整形 helper 只在新建路径上调），⑦ 认人靠
 *      peer flag、不靠账（08-15 批 5c 起判据只剩这一条，签名那半已删）；
 *      该字段功能性为零，且分家删复用分支后必死〕。
 * note() 为幂等 upsert；drop() 对不存在的键是空操作。
 */
extern void midr_nds_ledger_note(struct bgp *bgp,
				 struct ipaddr transport,
				 enum midr_session_reason reason,
				 struct in_addr remote_rid,
				 as_t remote_asn,
				 uint32_t remote_group);
extern void midr_nds_ledger_drop(struct bgp *bgp,
				 struct ipaddr transport);
extern const struct midr_session_ledger_entry *
midr_nds_ledger_lookup(struct bgp *bgp, struct ipaddr transport);
extern const char *midr_session_reason_str(enum midr_session_reason reason);
extern void midr_nds_manual_session_set(struct bgp *bgp,
					struct ipaddr transport, as_t remote_asn,
					struct in_addr remote_rid);
extern bool midr_nds_manual_session_del(struct bgp *bgp,
					struct ipaddr transport);
extern void midr_nds_manual_sessions_restore(struct bgp *bgp);

extern bool midr_rep_dir_del(struct bgp *bgp, uint32_t group_id,
			     struct ipaddr rep_transport);
extern void midr_rep_dir_clear(struct bgp *bgp);
extern struct midr_rep_entry *midr_rep_dir_find_group(struct bgp *bgp,
						      uint32_t group_id);

/* ===========================================================================
 * Read-only getters (for external modules, e.g. ④ flooding control)
 * =========================================================================*/

/* The NDS-maintained global view, or NULL if MIDR is not initialised. */
extern struct midr_global_view *midr_get_global_view(struct bgp *bgp);

/* This node's runtime group-id; 0 if MIDR is not initialised. */
extern uint32_t midr_local_group_id(struct bgp *bgp);

/* Look up `node_id` in the node table; on hit, write its group-id to *out and
 * return true, else return false (*out untouched). */
extern bool midr_node_group_id(struct bgp *bgp, const struct prefix *node_id,
			       uint32_t *out);

/* Address-family-neutral transport accessors for PM and other consumers. */
extern bool midr_nds_local_transport_get(const struct bgp *bgp,
					 struct ipaddr *transport);
/* Set/clear configured intent and apply it through one lifecycle path. */
extern int midr_nds_transport_configure(struct bgp *bgp,
					const struct ipaddr *transport);
extern int midr_nds_transport_reconcile(struct bgp *bgp);
extern bool midr_nds_node_transport_get(const struct bgp *bgp,
					const struct prefix *node_id,
					struct ipaddr *transport);
/* True when no other node identity advertises the same full locator. */
extern bool midr_nds_locator_unique(const struct bgp *bgp,
				    const struct prefix *owner,
				    const struct ipaddr *transport);

#endif /* _FRR_BGP_MIDR_NDS_H */
