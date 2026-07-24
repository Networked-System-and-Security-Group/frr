// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR Control Layer
 *
 * Bridges the MIDR node table (bgp_midr.c / NDS) and the BGP peer FSM.
 * Evaluates peering policy and drives peer_remote_as() / peer_delete()
 * based on node table events.
 */

#ifndef _FRR_BGP_MIDR_CTRL_H
#define _FRR_BGP_MIDR_CTRL_H

#include <stdint.h>
#include <stdbool.h>
#include <netinet/in.h>

struct bgp;
struct bgp_midr;
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
#define MIDR_CTRL_MSG_VERSION 2 /* v2: REP_LIST_RESP 条目加 rep_rid 栏 (12B→16B) */

enum midr_ctrl_msg_type {
	MIDR_CTRL_PEER_REQUEST = 1,	  /* "please peer back with me" */
	MIDR_CTRL_REP_LIST_REQ = 2,	  /* new node -> bootstrap: send rep list */
	MIDR_CTRL_REP_LIST_RESP = 3,	  /* bootstrap -> new node: rep directory */
	MIDR_CTRL_MEMBER_LIST_REQ = 4,	  /* new node -> rep: send group members */
	MIDR_CTRL_MEMBER_LIST_RESP = 5,	  /* rep -> new node: member list (table A) */
	MIDR_CTRL_ANNOUNCE = 6,	  /* new node -> each candidate member: "this is
					   * who I am" so it can validate my PM probes.
					   * One-way, no reply expected. */
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

/* MEMBER_LIST_RESP item, 16 bytes, network byte order. */
struct midr_ctrl_member_item {
	struct in_addr rid;
	struct in_addr transport;
	uint32_t asn;
	uint32_t group_id;
};

/* A pending request to retransmit (keyed by destination + type).  Lives in
 * bgp_midr->ctrl_pending; exposed here so `show midr join` can render the
 * not-yet-answered requests.
 * retries_left 语义: PEER_REQUEST 计 UDP 重发次数; 列表类 (REP_LIST_REQ /
 * MEMBER_LIST_REQ) 迁 TCP 后计 "TCP 短连接尝试次数" (retx tick 遇在途连接
 * 跳过不减)。 */
struct midr_ctrl_pending {
	struct in_addr target_transport; /* resend destination */
	uint8_t type;			 /* request type being retransmitted */
	uint32_t target_group;		 /* group field carried in the request */
	int retries_left;
};

/* Human-readable name of an enum midr_ctrl_msg_type value (for logs/show). */
extern const char *midr_ctrl_msg_type_str(uint8_t type);

/* Open / close the control-channel UDP socket (called from bgp_midr_init/finish). */
extern void midr_ctrl_init(struct bgp *bgp);
extern void midr_ctrl_finish(struct bgp *bgp);

/*
 * Called by bgp_midr.c (NDS) to initiate a (multi-hop eBGP + BGP-LS) session to
 * a node, after NDS has decided to peer.  Dedups against an existing peer and
 * sends a reverse PEER_REQUEST so the far end peers back.
 */
extern void midr_ctrl_connect(struct bgp *bgp,
			      const struct midr_node_entry *entry);

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
 * 双条件 = PEER_FLAG_MIDR_OVERLAY 标记（出身）∧ 除 BGP-LS 外无激活地址族（签名）。
 * 用于让 no midr neighbor / midr neighbor / try_disconnect / connect 去重四处
 * 一律"运维会话让路"：真是 MIDR 自己的才动，否则拒绝 + 告警。
 */
extern bool midr_nds_peer_is_overlay(struct peer *peer);

/*
 * Called by bgp_midr.c (NDS) before a node entry is removed (withdraw or
 * expiry). Tears down the dynamically-created BGP session if one exists.
 */
extern void midr_ctrl_on_node_remove(struct bgp *bgp,
				     struct midr_node_entry *entry);

/*
 * Initiate BGP sessions to every non-self node in the given group (used by
 * the new-node join orchestration after a JOIN decision).  Returns the
 * number of members a session was initiated to.
 */
extern int midr_ctrl_connect_group(struct bgp *bgp, uint32_t group_id);

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
extern bool midr_ctrl_tcp_client_inflight(struct bgp_midr *mi,
					  struct in_addr dst, uint8_t type);

#endif /* _FRR_BGP_MIDR_CTRL_H */
