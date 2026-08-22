// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR Control Layer
 *
 * Bridges the MIDR node table (bgp_midr_nds.c / NDS) and the BGP peer FSM.
 * Evaluates peering policy and drives peer_remote_as() / peer_delete()
 * based on node table events.
 */

#ifndef _FRR_BGP_MIDR_CTRL_H
#define _FRR_BGP_MIDR_CTRL_H

#include <stdint.h>
#include <stdbool.h>
#include <netinet/in.h>

/* enum midr_session_reason（会话台账原因）按值出现在 midr_ctrl_connect 的
 * 签名里，必须见到定义；顺带让本头自洽（原先靠各 .c 先包含 nds.h 才编得过）。 */
#include "bgpd/bgp_midr_nds.h"

struct bgp;
struct bgp_midr_nds;
struct midr_node_entry;
struct peer;
struct stream;

/* ===========================================================================
 * MIDR control channel —— 双传输
 *
 * 端口 5859 上并存两条传输（内核 TCP/UDP 端口空间独立，同号无冲突）：
 *
 *  - UDP :5859 —— PEER_REQUEST 专用。新节点选定群后向各群成员的 transport
 *    地址单播 PEER_REQUEST，成员反向建连（双向）。载荷固定 20B、永不撞尺寸墙，
 *    以反向会话 Established 为隐式 ack，保留原 3s×5 次重传。此通道独立于 PM
 *    探测通道（各自 socket/端口/格式），PM 设计变更不影响它。
 *
 *  - TCP :5859 短连接 —— REP_LIST / MEMBER_LIST 请求-响应（列表交换）。载荷随
 *    全网群数 / 群规模增长会撞 UDP 尺寸墙（rep 目录 ~122 群、成员表 >91 人即
 *    分片；UDP 5 次重传即弃 + 超 80 条静默截断），故迁 TCP 流式短连接（连上→
 *    请求→响应→立即关）。决策见 docs/decisions/
 *    midr-preexchange-transport-udp-vs-tcp.md；传输层实现在 bgp_midr_ctrl_tcp.c。
 * =========================================================================*/

#define MIDR_CTRL_UDP_PORT    5859 /* control channel only (distinct from PM) */
#define MIDR_CTRL_TCP_PORT    MIDR_CTRL_UDP_PORT /* 列表交换 TCP，与 UDP 同号并存 */
/*
 * v2: REP_LIST_RESP 条目加 rep_rid 栏 (12B→16B)
 * v3: BOOTSTRAP_LIST_RESP 条目加 rid 栏 (8B→12B，保底轮 2 批 5 R 系列)
 *     —— 条目变长必须升版：不升的话旧 v2 收方会按 8B 错位解析出垃圾，升了则
 *     整包直接丢弃、干净失败。新增**消息类型**不必升版（PEER_REJECT /
 *     BOOTSTRAP_LIST / ATTACH_REQUEST 都没升），改**条目长度**才必须升。
 */
#define MIDR_CTRL_MSG_VERSION 3

enum midr_ctrl_msg_type {
	MIDR_CTRL_PEER_REQUEST = 1,	  /* "please peer back with me" */
	MIDR_CTRL_REP_LIST_REQ = 2,	  /* new node -> bootstrap: send rep list */
	MIDR_CTRL_REP_LIST_RESP = 3,	  /* bootstrap -> new node: rep directory */
	MIDR_CTRL_MEMBER_LIST_REQ = 4,	  /* new node -> rep: send group members */
	MIDR_CTRL_MEMBER_LIST_RESP = 5,	  /* rep -> new node: member list (table A) */
	MIDR_CTRL_ANNOUNCE = 6,	  /* new node -> each candidate member: "this is
					   * who I am" so it can validate my PM probes.
					   * One-way, no reply expected. */
	/*
	 * B2（2026-08-11 半边残留评审定案，执行计划答疑 93）：接收侧拒绝
	 * PEER_REQUEST 时回发一包，让发起方**提前死心**——不必盲等 3s×5=15s
	 * 才靠超时拆掉自己预配的半边。
	 *
	 * 新增消息类型不碰"帧不动"红线：红线的准确范围是"PEER_REQUEST 帧不加
	 * 字段、不为闸门升协议版"；新增一种类型是常规操作（批 3 的
	 * BOOTSTRAP_LIST 同样是新类型）。协议版本号不动。
	 *
	 * 字段语义与 PEER_REQUEST **完全一致**：requester_* 三件恒表示"本包
	 * 发送者自己"（这里就是拒绝方），target_group = 拒绝方自己的群号。
	 * 发起方据 requester_transport 去 ctrl_pending 里找"我是不是正朝这个
	 * 地址发着 PEER_REQUEST"——找不到就丢（防伪，见 midr_ctrl_udp_recv）。
	 * 拒绝原因不进帧（reserved 保持保留位）：原因写在拒绝方自己的日志里，
	 * 发起方只需知道"被拒了"。要带原因码时再启用 reserved，届时另议。
	 */
	MIDR_CTRL_PEER_REJECT = 7,
	/*
	 * ② 按需拉取活引导名单（保底轮 2 批 3；子稿《引导节点更新-对接稿》§2②）：
	 * 群代表问一台引导"当下活着的引导有哪些"，引导以"手配名单 ∩ 自己骨干会话
	 * 活性"作答。走 **TCP 短连接**（与 REP_LIST/MEMBER_LIST 同族：列表交换一律
	 * TCP，见决策 midr-preexchange-transport-udp-vs-tcp.md），复用 4B 长度前缀
	 * 分帧与 ctrl_pending 重试。协议版本不动（新增类型不升版，同 PEER_REJECT）。
	 *
	 * REQ 用与 REP_LIST_REQ 同构的 20B 请求帧，target_group 无意义置 0。
	 */
	MIDR_CTRL_BOOTSTRAP_LIST_REQ = 8,
	MIDR_CTRL_BOOTSTRAP_LIST_RESP = 9,
	/*
	 * 挂靠专用建连请求（保底轮 2 批 5 前置①）。语义 = "我是群代表，来挂靠"，
	 * 帧结构与 PEER_REQUEST **完全一致**（同 20B、字段同义），只是 type 值不同。
	 *
	 * 为什么要单独一类：引导的负面守卫靠"查节点表猜发起方是谁"决定拒不拒，而
	 * learn_requester 造出的幽灵条目（只有 rid/transport/asn、caps 与 group_id
	 * 全 0）会让刚问过路的代表看起来像普通成员 —— 挂靠被自己人拒掉，且现象随
	 * 时序与哈希漂。换成专用类型后判据从"猜你是谁"变成"看你来干嘛"：
	 * **身份会过时，意图不会**。引导收到它 = 过排除名单 → 直接回配，跳过负面
	 * 守卫；收到普通 PEER_REQUEST 仍然拒 + PEER_REJECT（定稿结论 15 措辞不动）。
	 *
	 * ⚠ 新旧不混跑：旧节点收到本类型走 default 丢弃 → 挂靠失败。落地时全网同批
	 * 换二进制（先例：协议 v2）。
	 */
	MIDR_CTRL_ATTACH_REQUEST = 10,
};

/*
 * Request frame, fixed 20 bytes, network byte order.  Shared by PEER_REQUEST,
 * REP_LIST_REQ, MEMBER_LIST_REQ and ANNOUNCE: all carry the requester's
 * identity so the responder can reply / peer back / recognise it without a
 * node-table lookup.
 *   - PEER_REQUEST     : target_group = the group the requester joined
 *   - REP_LIST_REQ     : target_group = 0 (ignored)
 *   - MEMBER_LIST_REQ  : target_group = the group whose members are wanted
 *   - ANNOUNCE         : target_group = 0 (ignored)
 */
struct midr_ctrl_msg {
	uint8_t version;
	uint8_t type;
	uint16_t reserved;
	struct in_addr requester_rid;	    /* router-id (identity) */
	struct in_addr requester_transport; /* reachable locator to reply / peer back */
	uint32_t requester_asn;
	uint32_t target_group;
};

/* Variable-length response header, followed by `count` list items. 4 bytes. */
struct midr_ctrl_list_hdr {
	uint8_t version;
	uint8_t type;
	uint16_t count; /* number of items that follow (network order) */
};

/* REP_LIST_RESP item, 16 bytes, network byte order.
 * rep_rid = 代表的 router-id（真名）；0 = 应答方不知道（收方退回
 * "拿 transport 冒充 node_id"的旧占位路径，向后兼容）。 */
struct midr_ctrl_rep_item {
	uint32_t group_id;
	struct in_addr rep_transport;
	uint32_t rep_asn;
	struct in_addr rep_rid;
};

/*
 * BOOTSTRAP_LIST_RESP item, 12 bytes, network byte order.
 *
 * 三栏 = transport + asn + rid（保底轮 2 批 5 R 系列，协议 v3）。
 * 〔旧注释说"没有 rid：引导专职化后不发 Node NLRI，谁都学不到它的 router-id"，
 *  该理由已被推翻——引导的 rid 现在由**运维手配**：`midr bootstrap <IP>
 *  remote-as <ASN> router-id <RID>` 三个参数全必选，rid 因此静态可得、不依赖
 *  引导发不发 NLRI，也不依赖会话是否 Established。〕
 *
 * 为什么名单要带 rid：① 挂靠挑台按 rid 排环（确定可重放）；② 会话排除名单以
 * router-id 为键，没有 rid 就没法把一台引导拉黑；③ 会话台账的"对端 rid"栏。
 * 收侧规矩：rid 为 0 的条目**拒收**（见 midr_nds_bootstrap_learn）。
 */
struct midr_ctrl_bootstrap_item {
	struct in_addr transport;
	uint32_t asn;
	struct in_addr rid;
};

/* MEMBER_LIST_RESP item, 16 bytes, network byte order. */
struct midr_ctrl_member_item {
	struct in_addr rid;
	struct in_addr transport;
	uint32_t asn;
	uint32_t group_id;
};

/*
 * 重传参数——每个入队点自带一份，不再吃全局硬编码（小整理批参数化）。
 *   interval_ms    重传间隔（毫秒）
 *   count          总尝试次数，首发算第 1 次
 *   stop_on_signal 有"完成信号"即停：PEER_REQUEST 认反向会话 Established，
 *                  列表类认响应到达（recv 路径 midr_ctrl_drop_pending 摘队）。
 *                  false = 不等信号，发满 count 次自然收摊，**不算失败**、也不
 *                  走死心处置（拆半边 / failover / 盖章）。
 * ⚠ 当前四个入队点全填 stop_on_signal=true —— 队列本就是"等响应"的设计。
 *   false 是给 ANNOUNCE 那类单程消息将来要重发时预留的口子（记档第 13 条：
 *   我方只提供能力，要不要用由 ANNOUNCE 的 owner 定）。
 */
struct midr_ctrl_retx_params {
	int interval_ms;
	int count;
	bool stop_on_signal;
};

/* A pending request to retransmit (keyed by destination + type).  Lives in
 * bgp_midr_nds->ctrl_pending; exposed here so `show midr join` can render the
 * not-yet-answered requests.
 * retries_left 语义: PEER_REQUEST 计 UDP 重发次数; 列表类 (REP_LIST_REQ /
 * MEMBER_LIST_REQ) 迁 TCP 后计 "TCP 短连接尝试次数" (retx tick 遇在途连接
 * 跳过不减)。 */
struct midr_ctrl_pending {
	struct in_addr target_transport; /* resend destination */
	uint8_t type;			 /* request type being retransmitted */
	uint32_t target_group;		 /* group field carried in the request */
	int retries_left;
	/*
	 * 入队时那份参数（含预算 count）。有了它，死心那句 warn 才能说出**实际**
	 * 试了几次——挂靠第二批候选的预算被 midr_ctrl_set_retx_budget() 压成 2 次，
	 * 若照旧打默认的 5 就是日志说假话（试了 2 次却报 5 次，批 6 实测抓到）。
	 */
	struct midr_ctrl_retx_params params;
	/*
	 * 距下次重传还剩多少毫秒。retx tick 对全队统一递减"本跳实际经过的时间"，
	 * 减到 <=0 的才轮到处理，处理完重置成本条的 interval_ms；下一跳的间隔取
	 * 全队 due_ms 的最小值。所有条目 interval 相同时（当前即如此）等价于
	 * 参数化之前的"单节拍、每跳全队各减一次"。
	 */
	int due_ms;
};

/* Human-readable name of an enum midr_ctrl_msg_type value (for logs/show). */
extern const char *midr_ctrl_msg_type_str(uint8_t type);

/* Open / close the control-channel UDP socket (called from bgp_midr_nds_init/finish). */
extern void midr_ctrl_init(struct bgp *bgp);
extern void midr_ctrl_finish(struct bgp *bgp);

/*
 * Called by bgp_midr_nds.c (NDS) to initiate a (multi-hop eBGP + BGP-LS) session to
 * a node, after NDS has decided to peer.  Dedups against an existing peer and
 * sends a reverse PEER_REQUEST so the far end peers back.
 *
 * reason = 调用方"为什么要这条边"，登进会话台账（结论 20）。每个调用点自报
 * 来意，不在这里猜——拆会话/对账时凭它认出保底、群间、运维点名的合法边。
 * reason == SAME_GROUP 时另外承担"把对端纳入本群邻居"（见定义处该分支）。
 *
 * send_nudge = 要不要朝对端发 PEER_REQUEST。**回配路必须传 false**（我是被请求
 * 方，再发就是回声），发起类三处传 true。不做默认值：默认发会把"回配忘了关
 * nudge"这类错误静默化。
 */
extern void midr_ctrl_connect(struct bgp *bgp,
			      const struct midr_node_entry *entry,
			      enum midr_session_reason reason, bool send_nudge);

/*
 * 把一个刚建出的 peer 整形成 MIDR overlay 会话：multihop + update-source
 * (本端 transport) + 激活 BGP-LS + 撤销 FRR 自动附送的 IPv4 单播（overlay
 * 不得向 underlay 注入转发路由——否则递归下一跳环路，见函数实现头注释与
 * docs/decisions/midr-overlay-underlay-layering.md）。自动建连
 * (midr_ctrl_connect) 与手动命令 (midr neighbor) 共用。
 */
extern void midr_nds_ctrl_setup_overlay_peer(struct bgp *bgp, struct peer *peer);

/*
 * 会话归属判据（⑦，四处守卫共用）：peer 是不是 MIDR 自建的 overlay 会话。
 * 判据 = 带 PEER_FLAG_MIDR_OVERLAY 标记（出身）——运维原生会话永远没有它。
 * 〔批 5c 删掉了原来的第二条"只载 BGP-LS"签名判据，理由见函数定义处注释。〕
 * 用于让 no midr session / midr session / try_disconnect / connect 去重四处
 * 一律"运维会话让路"：真是 MIDR 自己的才动，否则拒绝 + 告警。
 */
extern bool midr_nds_peer_is_overlay(struct peer *peer);

/*
 * Called by bgp_midr_nds.c (NDS) before a node entry is removed (withdraw or
 * expiry). Tears down the dynamically-created BGP session if one exists.
 */
extern void midr_ctrl_on_node_remove(struct bgp *bgp,
				     struct midr_node_entry *entry);

/*
 * 按 transport 拆一条 MIDR 边（批 5 挂靠钩子：卸任清位 / 钩子 (b) 换台先拆旧）。
 * 对端是引导节点时节点表里没有它的条目，故不走 on_node_remove 那条路；内部拼
 * 临时条目后复用同一套拆除逻辑（MANUAL 豁免 + 销账 + ⑦ 归属守卫）。
 * rid 只用于日志可读，可为 0。
 *
 * force：真则跳过 MANUAL 豁免（α）。**只有本端 `midr shutdown` 退网传 true**
 * ——那是运维显式命令、与手配同级；自动路径（挂靠卸任、B1 老化、换台先拆旧）
 * 一律传 false。判据与理由见 midr_try_disconnect 内 α 注释。
 */
extern void midr_ctrl_detach_transport(struct bgp *bgp, struct in_addr transport,
				       struct in_addr rid, bool force);

/*
 * Initiate BGP sessions to every non-self node in the given group (used by
 * the new-node join orchestration after a JOIN decision).  Returns the
 * number of members a session was initiated to.  reason 透传给台账。
 */
extern int midr_ctrl_connect_group(struct bgp *bgp, uint32_t group_id,
				   enum midr_session_reason reason);

/*
 * Hierarchical-discovery requests (new node side), sent over the TCP short
 * connection (list exchange):
 *   - send a REP_LIST_REQ to the bootstrap's transport address, then
 *   - send a MEMBER_LIST_REQ to the chosen group representative.
 * Both enqueue a retransmit (ctrl_pending) until the matching response arrives;
 * each retry re-opens a TCP short connection (see bgp_midr_ctrl_tcp.c).
 */
extern void midr_ctrl_send_rep_request(struct bgp *bgp,
				       struct in_addr bootstrap_transport);
extern void midr_ctrl_send_member_request(struct bgp *bgp,
					  struct in_addr rep_transport,
					  uint32_t group_id);

/*
 * 群代表 -> 引导：要一份"当下活着的引导名单"（BOOTSTRAP_LIST_REQ，同走 TCP 短
 * 连接 + ctrl_pending 重试）。调用方 = NDS 的 midr_nds_bootstrap_list_fetch()。
 * 重试耗尽（死心）时 retx 定时器回调 midr_nds_bootstrap_list_failed() 换下一个
 * 候选——与 REP_LIST_REQ 的 join failover 是**两条独立的路**，各用各的游标。
 */
extern void midr_ctrl_send_bootstrap_list_request(struct bgp *bgp,
						  struct in_addr dst);

/*
 * 改一条**在途请求**的剩余重传次数（保底轮 2 批 6）。
 * 唯一用途 = 挂靠挑台的第二批候选（不在最新活引导名单里）建连后把预算压到
 * MIDR_ATTACH_RETX_SECOND，让"多半已经死了"的候选 6s 就死心、早点轮到下一台；
 * 第一批不调用、照吃入队时那份 params 的默认 count。
 * 按 (目标 transport, 消息类型) 定位，查不到就什么都不做——调用方在
 * midr_ctrl_connect() 之后调，而 connect 可能因去重根本没入队。
 */
extern void midr_ctrl_set_retx_budget(struct bgp *bgp, struct in_addr dst,
				      uint8_t type, int retries);

/*
 * New node -> an arbitrary candidate's transport address: one-way
 * MIDR_CTRL_ANNOUNCE self-identification, no retransmit/response tracking.
 * Lets the receiver recognise our subsequent PM probes as coming from a
 * known source even though it never sent us a REP_LIST_REQ/MEMBER_LIST_REQ
 * itself (e.g. a non-bootstrap rep in the representative directory).
 */
extern void midr_ctrl_send_announce(struct bgp *bgp, struct in_addr dst);

/*
 * Mark a qualifying connection into the topology graph.  Skeleton stub: the
 * real topology-graph bookkeeping lands in a later phase.
 */
extern void midr_mark_topology(struct bgp *bgp,
			       const struct midr_node_entry *entry);

/* ===========================================================================
 * TCP 列表交换 —— 传输层 (bgp_midr_ctrl_tcp.c) 与语义层 (bgp_midr_ctrl.c) 接口
 *
 * 传输层只管 "可靠送一帧 / 收一帧"（连接生命周期、监听、非阻塞连接、长度
 * 前缀分帧、超时、错误汇聚），不理解协议语义；语义层负责消息构造、资格闸门、
 * 列表内容组装。两层通过下列函数解耦。
 * =========================================================================*/

/* 帧格式: [4B 长度前缀 (网络序，只计 payload)][payload = 原 UDP 报文字节]。 */

/* --- 语义层 (ctrl.c) 提供给传输层调用 --- */

/*
 * 填充一个 20B 请求帧 (PEER_REQUEST / REP_LIST_REQ / MEMBER_LIST_REQ) —— 携带
 * 本节点身份 (rid/transport/asn) 与 target_group。UDP 与 TCP 两条路径共用。
 * 各字段已按 wire 序 (htonl) 就绪，调用方直接整块拷贝即可。
 */
extern void midr_ctrl_fill_msg(struct bgp *bgp, struct midr_ctrl_msg *msg,
			       uint8_t type, uint32_t target_group);

/*
 * 传输层收到一个完整请求帧后回调。返回响应 payload (stream，不含长度前缀，
 * 由传输层封帧发出)；返回 NULL = 不回包 (闸门不过，沉默语义同 UDP 版)。
 * remote 为发起方 (accept 得到的对端地址)，仅用于日志。
 */
extern struct stream *midr_ctrl_on_tcp_request(struct bgp *bgp,
					       const uint8_t *payload,
					       size_t len,
					       struct in_addr remote);

/*
 * 传输层收到一个完整响应帧后回调。req_type = 本端当初发出的请求类型，供响应
 * 类型配对校验；src = 响应来源（即当初请求的目的地），供并发多个
 * MEMBER_LIST_REQ（主候选群 + 至多 2 个次优群）时精确清对应的重传 pending
 * 条目（按 (dst,type) 而非只按 type，否则某个目标的响应会误清掉另一个尚未
 * 响应的目标的重传追踪）；内部转现有 recv_rep_list / recv_member_list 灌全
 * 局视图并推进 join 状态机。
 */
extern void midr_ctrl_on_tcp_response(struct bgp *bgp, uint8_t req_type,
				      const uint8_t *payload, size_t len,
				      struct in_addr src);

/* --- 传输层 (tcp.c) 提供给语义层调用 --- */

/* 建连接表 + 开 5859/TCP 监听 (监听失败仅 warn，不影响客户端侧)。 */
extern void midr_ctrl_tcp_init(struct bgp *bgp);
/* 关监听 + 逐条关闭所有活动连接 + 释放连接表。 */
extern void midr_ctrl_tcp_finish(struct bgp *bgp);

/*
 * 发起一次 TCP 短连接请求 (REP_LIST_REQ / MEMBER_LIST_REQ) 到 dst:5859。
 * 在途去重: 同 (dst,type) 已有连接时，group 相同则忽略、不同则关旧起新。
 * 连接失败不阻塞——语义层的 ctrl_pending 重试机制会稍后再来一次。
 */
extern void midr_ctrl_tcp_client_start(struct bgp *bgp, struct in_addr dst,
				       uint8_t type, uint32_t target_group);

/* 重试定时器查询: 到 dst 的 type 请求是否已有在途 TCP 连接 (在途则本轮不减
 * retries_left，避免虚耗重试数)。 */
extern bool midr_ctrl_tcp_client_inflight(struct bgp_midr_nds *mi,
					  struct in_addr dst, uint8_t type);

#endif /* _FRR_BGP_MIDR_CTRL_H */
