// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR Control Layer —— 语义层
 *
 * Evaluates peering policy from MIDR node table events and drives the BGP
 * peer FSM (peer_remote_as / peer_delete) accordingly.
 *
 * 控制通道双传输 (端口 5859, 语义在此、TCP 传输在 bgp_midr_ctrl_tcp.c):
 *  - UDP: PEER_REQUEST —— 新节点选群后向各群成员 transport 单播 "请反向建连"
 *    通知, 成员反向发起多跳 eBGP, 双向建立 (独立于 PM 探测通道)。
 *  - TCP 短连接: REP_LIST / MEMBER_LIST 列表交换 —— 本文件负责消息构造 / 资格
 *    闸门 / 列表内容组装 (on_tcp_request/on_tcp_response 回调), 连接机制在
 *    bgp_midr_ctrl_tcp.c。载荷随规模增长撞 UDP 尺寸墙故迁 TCP, 决策见
 *    docs/decisions/midr-preexchange-transport-udp-vs-tcp.md。
 */

#include "zebra.h"

#include "memory.h"
#include "frrevent.h"
#include "linklist.h"
#include "network.h" /* set_nonblocking */
#include "sockopt.h" /* sockopt_reuseaddr */
#include "sockunion.h"
#include "stream.h"
#include "prefix.h"
#include "log.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_midr_nds.h"
#include "bgpd/bgp_midr_ctrl.h"
#include "bgpd/bgp_midr_pm.h" /* midr_pm_add_target（connect_group 启探测） */

DEFINE_MTYPE_STATIC(BGPD, MIDR_CTRL_PENDING, "MIDR ctrl pending peer-request");

/*
 * PEER_REQUEST retransmit: UDP is lossy, so resend until the session is up.
 *
 * 默认参数 = 参数化之前那两个硬编码常量（3 秒 × 5 次）。四个入队点当前全用它，
 * 故行为与改前逐跳一致；将来某处要另一套节奏，就地换一份 params 传进去即可，
 * 不必再动队列结构。
 */
static const struct midr_ctrl_retx_params midr_ctrl_retx_default = {
	.interval_ms = 3000,
	.count = 5,
	.stop_on_signal = true,
};

/*
 * ANNOUNCE has no reply to wait for (stop_on_signal=false: sending
 * params.count times without an answer is a normal finish, not a give-up --
 * see the retx-timer comment at its stop_on_signal=false branch). A shorter,
 * more frequent retry than the default budget above, since ANNOUNCE exists
 * to beat the sender's own EWMA warm-up window (MIDR_JOIN_PROBE_WAIT_SECS):
 * one lost UDP packet during a cluster's simultaneous-startup burst used to
 * be permanent (the receiver's pm_is_known_transport() would drop every
 * subsequent probe as "unknown transport" for the rest of the run, since
 * nothing ever resent the self-identification that would have fixed it).
 */
static const struct midr_ctrl_retx_params midr_ctrl_retx_announce = {
	.interval_ms = 2000,
	.count = 3,
	.stop_on_signal = false,
};

/* struct midr_ctrl_pending 定义已移至 bgp_midr_ctrl.h（show midr join 要展示
 * 未应答请求），MTYPE 仍留在本文件。 */

static void midr_ctrl_send_req(struct bgp_midr_nds *mi, struct in_addr dst,
			       uint8_t type, uint32_t target_group);
static void midr_ctrl_enqueue_request(struct bgp *bgp, struct in_addr dst,
				      uint8_t type, uint32_t target_group,
				      const struct midr_ctrl_retx_params *rp);
static void midr_ctrl_send_peer_request(struct bgp *bgp,
					const struct midr_node_entry *entry,
					enum midr_session_reason reason);
static void midr_ctrl_drop_pending(struct bgp_midr_nds *mi, struct in_addr dst,
				   uint8_t type);

static void midr_ctrl_member_probe_done_cb(struct event *t)
{
	struct bgp *bgp = EVENT_ARG(t);
	MIDR_FLOW_LOG("MIDR 加入：MEMBER_PROBE_DONE 定时器触发，通知 CL");
	midr_nds_notify_cl(bgp, MIDR_TRIGGER_MEMBER_PROBE_DONE);
}

/* 两个次优群限时探测完成，交 CL 选锚点。 */
static void midr_ctrl_anchor_probe_done_cb(struct event *t)
{
	struct bgp *bgp = EVENT_ARG(t);
	MIDR_FLOW_LOG("MIDR 锚点：ANCHOR_PROBE_DONE 定时器触发，通知 CL");
	midr_nds_notify_cl(bgp, MIDR_TRIGGER_ANCHOR_PROBE_DONE);
}
static void midr_ctrl_retx_timer(struct event *t);
static void midr_ctrl_udp_recv(struct event *t);

/* Build a union sockunion (AF_INET) from a bare in_addr. */
static void midr_su_from_in_addr(union sockunion *su, struct in_addr a)
{
	struct prefix p = {};

	p.family = AF_INET;
	p.prefixlen = IPV4_MAX_BITLEN;
	p.u.prefix4 = a;
	prefix2sockunion(&p, su);
}

/* 到该 transport 的 BGP 会话是否已 Established（判活统一走会话状态：子稿 §1
 * 前提二"会话 Down 即死"，不引用 legacy last_seen/expire）。 */
static bool midr_ctrl_transport_session_up(struct bgp *bgp,
					   struct in_addr transport)
{
	union sockunion su;
	struct peer *peer;

	midr_su_from_in_addr(&su, transport);
	peer = peer_lookup(bgp, &su);

	return peer && peer->connection->status == Established;
}

const char *midr_ctrl_msg_type_str(uint8_t type)
{
	switch (type) {
	case MIDR_CTRL_PEER_REQUEST:
		return "PEER_REQUEST";
	case MIDR_CTRL_REP_LIST_REQ:
		return "REP_LIST_REQ";
	case MIDR_CTRL_REP_LIST_RESP:
		return "REP_LIST_RESP";
	case MIDR_CTRL_MEMBER_LIST_REQ:
		return "MEMBER_LIST_REQ";
	case MIDR_CTRL_MEMBER_LIST_RESP:
		return "MEMBER_LIST_RESP";
	case MIDR_CTRL_PEER_REJECT:
		return "PEER_REJECT";
	case MIDR_CTRL_BOOTSTRAP_LIST_REQ:
		return "BOOTSTRAP_LIST_REQ";
	case MIDR_CTRL_BOOTSTRAP_LIST_RESP:
		return "BOOTSTRAP_LIST_RESP";
	case MIDR_CTRL_ATTACH_REQUEST:
		return "ATTACH_REQUEST";
	case MIDR_CTRL_GROUP_ALLOC_REQ:
		return "GROUP_ALLOC_REQ";
	case MIDR_CTRL_GROUP_ALLOC_RESP:
		return "GROUP_ALLOC_RESP";
	default:
		return "UNKNOWN";
	}
}

/*
 * "建连类"请求 = PEER_REQUEST 与 ATTACH_REQUEST（批 5 前置①）：同一个 20B 帧、
 * 同走 UDP、同一套重传与死心语义，唯一差别是收方要不要过引导负面守卫。凡按
 * 类型分流传输、判死心、清 pending 的地方一律用本判据，**别再逐处写
 * `== MIDR_CTRL_PEER_REQUEST`** —— 散着写将来必漏一处（本批实测：⓪ 清单点名
 * 5 处，全树复扫实为 12 处）。
 */
static bool midr_ctrl_is_peer_req_like(uint8_t type)
{
	return type == MIDR_CTRL_PEER_REQUEST ||
	       type == MIDR_CTRL_ATTACH_REQUEST;
}

/* ------------------------------------------------------------------ */
/* Peer-request UDP control channel                                     */
/* ------------------------------------------------------------------ */

/* Fill a 20B request frame (PEER_REQUEST / REP_LIST_REQ / MEMBER_LIST_REQ) with
 * our identity + target_group.  Shared by the UDP path (midr_ctrl_send_req) and
 * the TCP path (bgp_midr_ctrl_tcp.c); each field is already in wire order so the
 * caller can copy the struct out verbatim. */
void midr_ctrl_fill_msg(struct bgp *bgp, struct midr_ctrl_msg *msg, uint8_t type,
			uint32_t target_group)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;

	memset(msg, 0, sizeof(*msg));
	msg->version = MIDR_CTRL_MSG_VERSION;
	msg->type = type;
	msg->requester_rid = bgp->router_id;
	msg->requester_transport = mi->local_transport_addr;
	msg->requester_asn = htonl(bgp->as);
	msg->target_group = htonl(target_group);
}

/* Low-level send: build a request frame (PEER_REQUEST / REP_LIST_REQ /
 * MEMBER_LIST_REQ) carrying our identity and unicast it. */
static void midr_ctrl_send_req(struct bgp_midr_nds *mi, struct in_addr dst,
			       uint8_t type, uint32_t target_group)
{
	struct midr_ctrl_msg msg = {};
	struct sockaddr_in sa = {};

	if (mi->ctrl_sock < 0 || !mi->transport_addr_set)
		return;

	midr_ctrl_fill_msg(mi->bgp, &msg, type, target_group);

	sa.sin_family = AF_INET;
	sa.sin_port = htons(MIDR_CTRL_UDP_PORT);
	sa.sin_addr = dst;

	if (sendto(mi->ctrl_sock, &msg, sizeof(msg), 0, (struct sockaddr *)&sa,
		   sizeof(sa)) < 0)
		zlog_warn("MIDR ctrl: request type %u sendto %pI4 failed: %s",
			  type, &dst, safe_strerror(errno));
}

/*
 * 发起一次请求 (首发或重试). 按类型分流传输:
 *   - PEER_REQUEST / ATTACH_REQUEST : UDP (一次性 nudge, 反向会话 Established
 *     为隐式 ack);
 *   - REP/MEMBER/BOOTSTRAP_LIST_REQ : TCP 短连接 (列表交换, 载荷随规模增长必须
 *     可靠)。
 * 重试队列 (ctrl_pending) 对两者一致——只是"重发动作"落到不同传输。
 * ⚠ 这里若漏认 ATTACH_REQUEST，挂靠请求会被当成列表交换发去 TCP，而对端根本
 * 没有对应的 TCP 处理——静默失败，是本批最隐蔽的一处。
 */
static void midr_ctrl_request_attempt(struct bgp *bgp, struct in_addr dst,
				      uint8_t type, uint32_t target_group)
{
	/* ANNOUNCE stays on the UDP one-way frame (never had a TCP counterpart
	 * -- it's not a list exchange), so it doesn't belong under
	 * midr_ctrl_is_peer_req_like()'s "connection-establishing" umbrella;
	 * routed here explicitly instead of widening that predicate's meaning. */
	if (midr_ctrl_is_peer_req_like(type) || type == MIDR_CTRL_ANNOUNCE)
		midr_ctrl_send_req(bgp->midr_nds_info, dst, type, target_group);
	else
		midr_ctrl_tcp_client_start(bgp, dst, type, target_group);
}

/* Send a request once and enqueue (or refresh) a retransmit keyed by (dst,type).
 * rp = 本入队点自带的重传参数；NULL 取默认（3s × 5 次、等完成信号）。 */
static void midr_ctrl_enqueue_request(struct bgp *bgp, struct in_addr dst,
				      uint8_t type, uint32_t target_group,
				      const struct midr_ctrl_retx_params *rp)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct listnode *node;
	struct midr_ctrl_pending *p;

	if (!rp)
		rp = &midr_ctrl_retx_default;

	if (!mi->transport_addr_set) {
		zlog_warn("MIDR ctrl: local transport-address unset; cannot send request type %u",
			  type);
		return;
	}

	midr_ctrl_request_attempt(bgp, dst, type, target_group);

	for (ALL_LIST_ELEMENTS_RO(mi->ctrl_pending, node, p))
		if (p->target_transport.s_addr == dst.s_addr &&
		    p->type == type) {
			p->target_group = target_group;
			p->params = *rp;
			p->retries_left = rp->count;
			p->due_ms = rp->interval_ms;
			return;
		}

	p = XCALLOC(MTYPE_MIDR_CTRL_PENDING, sizeof(*p));
	p->target_transport = dst;
	p->type = type;
	p->target_group = target_group;
	p->params = *rp;
	p->retries_left = rp->count;
	p->due_ms = rp->interval_ms;
	listnode_add(mi->ctrl_pending, p);

	/*
	 * 定时器已在跑就不重新武装——新条目跟着当前节拍走（与参数化之前一致：
	 * 那时也是单节拍、入队不重开表）。只有队列从空变非空时才起表，间隔取
	 * 本条的 interval。
	 */
	if (!mi->t_ctrl_retx) {
		mi->ctrl_retx_tick_ms = rp->interval_ms;
		event_add_timer_msec(bm->master, midr_ctrl_retx_timer, bgp,
				     rp->interval_ms, &mi->t_ctrl_retx);
	}
}

void midr_ctrl_set_retx_budget(struct bgp *bgp, struct in_addr dst,
			       uint8_t type, int retries)
{
	struct bgp_midr_nds *mi;
	struct listnode *node;
	struct midr_ctrl_pending *p;

	if (!bgp || !bgp->midr_nds_info || retries <= 0)
		return;
	mi = bgp->midr_nds_info;

	for (ALL_LIST_ELEMENTS_RO(mi->ctrl_pending, node, p))
		if (p->target_transport.s_addr == dst.s_addr &&
		    p->type == type) {
			/*
			 * 只压不抬：入队时已按默认预算发过第一次，这里改的是"还能再
			 * 试几次"。往大了改没有正当用途（真要更执着，改的该是默认值）。
			 */
			if (p->retries_left > retries) {
				p->retries_left = retries;
				p->params.count = retries;
			}
			MIDR_FLOW_LOG("MIDR ctrl: %s 到 %pI4 的重传预算压到 %d 次（约 %d 秒死心）",
				      midr_ctrl_msg_type_str(type), &dst,
				      p->retries_left,
				      p->retries_left * p->params.interval_ms /
					      1000);
			return;
		}
}

/* New node -> bootstrap: request the representative directory. */
void midr_ctrl_send_rep_request(struct bgp *bgp,
				struct in_addr bootstrap_transport)
{
	if (!bgp || !bgp->midr_nds_info)
		return;
	midr_ctrl_enqueue_request(bgp, bootstrap_transport,
				  MIDR_CTRL_REP_LIST_REQ, 0,
				  &midr_ctrl_retx_default);
	MIDR_FLOW_LOG("MIDR ctrl: sent REP_LIST_REQ to %pI4", &bootstrap_transport);
}

/* New node -> representative: request the group's member list (table A). */
void midr_ctrl_send_member_request(struct bgp *bgp, struct in_addr rep_transport,
				   uint32_t group_id)
{
	if (!bgp || !bgp->midr_nds_info)
		return;
	midr_ctrl_enqueue_request(bgp, rep_transport, MIDR_CTRL_MEMBER_LIST_REQ,
				  group_id, &midr_ctrl_retx_default);
	MIDR_FLOW_LOG("MIDR ctrl: sent MEMBER_LIST_REQ to %pI4 (group %u)",
		  &rep_transport, group_id);
}

/* 群代表 -> 引导：要一份活引导名单（②按需拉取，子稿 §2②）。 */
void midr_ctrl_send_bootstrap_list_request(struct bgp *bgp, struct in_addr dst)
{
	if (!bgp || !bgp->midr_nds_info)
		return;
	midr_ctrl_enqueue_request(bgp, dst, MIDR_CTRL_BOOTSTRAP_LIST_REQ, 0,
				  &midr_ctrl_retx_default);
	MIDR_FLOW_LOG("MIDR ctrl: sent BOOTSTRAP_LIST_REQ to %pI4", &dst);
}

/* New node -> the network's elected allocator bootstrap: ask for an
 * authoritative new group id before settling a CREATE. See the
 * MIDR_CTRL_GROUP_ALLOC_REQ enum comment (bgp_midr_ctrl.h) for why. */
void midr_ctrl_send_group_alloc_request(struct bgp *bgp, struct in_addr dst)
{
	if (!bgp || !bgp->midr_nds_info)
		return;
	midr_ctrl_enqueue_request(bgp, dst, MIDR_CTRL_GROUP_ALLOC_REQ, 0,
				  &midr_ctrl_retx_default);
	MIDR_FLOW_LOG("MIDR ctrl: sent GROUP_ALLOC_REQ to %pI4", &dst);
}

/*
 * New node -> an arbitrary candidate (rep or member): self-announce so the
 * receiver can validate our subsequent PM probes without an explicit
 * request/response round trip. One-way, no reply -- but retried up to
 * midr_ctrl_retx_announce.count times (2s apart) via the same ctrl_pending
 * queue the request/response messages use, because a single lost UDP packet
 * here used to be a permanent failure: the receiver never learns our
 * transport address, so pm_is_known_transport() drops every one of our PM
 * probes to it for the rest of the run, and REP_PROBE_DONE/MEMBER_PROBE_DONE
 * evaluate the candidate as having zero data -- indistinguishable from the
 * candidate not existing. Exposed for callers outside this file (e.g.
 * midr_join_on_rep_list() in bgp_midr_nds.c, which needs to announce to
 * *every* rep in the directory, not only the bootstrap that already received
 * a REP_LIST_REQ from us).
 */
void midr_ctrl_send_announce(struct bgp *bgp, struct in_addr dst)
{
	if (!bgp || !bgp->midr_nds_info)
		return;
	midr_ctrl_enqueue_request(bgp, dst, MIDR_CTRL_ANNOUNCE, 0,
				  &midr_ctrl_retx_announce);
	MIDR_FLOW_LOG("MIDR ctrl: sent ANNOUNCE to %pI4", &dst);
}

/*
 * Drop the pending retransmit for (dst,type) — response arrived from that
 * specific destination.  Scoped by dst (not just type) since several
 * MEMBER_LIST_REQ can be in flight to different reps at once (join candidate
 * plus runner-up groups); filtering by type alone would drop other still-
 * outstanding destinations' retransmit tracking the moment any one responds.
 */
static void midr_ctrl_drop_pending(struct bgp_midr_nds *mi, struct in_addr dst,
				   uint8_t type)
{
	struct listnode *node, *nnode;
	struct midr_ctrl_pending *p;

	for (ALL_LIST_ELEMENTS(mi->ctrl_pending, node, nnode, p))
		if (p->type == type &&
		    p->target_transport.s_addr == dst.s_addr) {
			list_delete_node(mi->ctrl_pending, node);
			XFREE(MTYPE_MIDR_CTRL_PENDING, p);
		}
}

/*
 * 同款判据的只读版：本机此刻正朝 dst 发着的**建连类**请求是哪一种（没有则返回
 * 0）。B2 收 PEER_REJECT 时用它防伪——没在朝人家敲门，就没理由接受人家的拒绝；
 * 顺带告诉调用方该清哪一条 pending。
 * 返回类型而非 bool（批 5 前置①）：挂靠被拒时在途的是 ATTACH_REQUEST，只认
 * PEER_REQUEST 会把真拒绝包当成伪造丢掉，半边会话继续盲等 15s 才死心。
 */
static uint8_t midr_ctrl_pending_peer_req_type(struct bgp_midr_nds *mi,
					       struct in_addr dst)
{
	struct listnode *node;
	struct midr_ctrl_pending *p;

	for (ALL_LIST_ELEMENTS_RO(mi->ctrl_pending, node, p))
		if (midr_ctrl_is_peer_req_like(p->type) &&
		    p->target_transport.s_addr == dst.s_addr)
			return p->type;
	return 0;
}

/*
 * Notify a target node to peer back with us, and queue retransmits so a lost
 * datagram does not leave the reverse session unconfigured.
 */
static void midr_ctrl_send_peer_request(struct bgp *bgp,
					const struct midr_node_entry *entry,
					enum midr_session_reason reason)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct prefix locator;
	uint8_t type;

	midr_node_get_locator(entry, &locator);
	if (locator.family != AF_INET)
		return;

	/*
	 * 挂靠边发专用类型（批 5 前置①），其余照旧 PEER_REQUEST。reason 是调用方
	 * 自报的来意（会话台账那套），这里直接复用、不另加参数——"为什么要这条边"
	 * 与"用哪种请求去要"本就是同一件事的两面。
	 */
	type = (reason == MIDR_SESSION_ATTACH) ? MIDR_CTRL_ATTACH_REQUEST
					       : MIDR_CTRL_PEER_REQUEST;

	midr_ctrl_enqueue_request(bgp, locator.u.prefix4, type,
				  mi->local_group_id, &midr_ctrl_retx_default);
	MIDR_FLOW_LOG("MIDR ctrl: sent %s to %pI4 (group %u)",
		  midr_ctrl_msg_type_str(type), &locator.u.prefix4,
		  mi->local_group_id);
}

/*
 * B2（结论 23 的加速件，答疑 93）：拒绝一条 PEER_REQUEST 时回发一包，告诉
 * 发起方"别等了"。**不进重传队列**——发一次就完（`midr_ctrl_send_req` 直发、
 * 不 enqueue）：丢了就退回发起方那边 3s×5 的超时死心兜底，天然安全，也不必
 * 为它再造一套 pending 状态。
 *
 * 目标地址取请求帧里的 requester_transport（发起方自报的可达地址）——它正是
 * 我们回配时会去连的那个地址，也正是发起方 ctrl_pending 里那条的键，两边对得上。
 */
static void midr_ctrl_send_peer_reject(struct bgp *bgp, struct in_addr dst)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;

	if (dst.s_addr == INADDR_ANY)
		return; /* 请求帧没自报 transport，无处可回 */

	midr_ctrl_send_req(mi, dst, MIDR_CTRL_PEER_REJECT, mi->local_group_id);
	MIDR_FLOW_LOG("MIDR ctrl: sent PEER_REJECT to %pI4", &dst);
}

/*
 * 死心拆除（结论 23）：PEER_REQUEST 重传耗尽后，把本机当初为它预配的那半边
 * peer 拆掉。对端从没回配过，这条会话永远到不了 Established。
 *
 * 拆之前**必过 ⑦ 归属守卫** `midr_nds_peer_is_overlay`（判据 = 带
 * PEER_FLAG_MIDR_OVERLAY 标记）：万一这个 transport 地址恰好撞上运维用 loopback
 * 手配的静态邻居，不认人就会把原生会话拆了——那是事故（07-22 真踩过 locator
 * 误命中）。运维优先，认不出就只销账、不动会话本体。
 *
 * 台账照销：账记的是"MIDR 需要这条边"，需求已经宣告失败，账就该没（家规③的
 * 行为语义）。会话本体的去留由 ⑦ 决定，两件事分开。
 *
 * `why` = 死心原因，由调用方给：两条路进来（重传耗尽 / 收到 PEER_REJECT），
 * 语义同款但**原因不同**，写死在这里会说假话（B2 验收实测到过：明明是被拒，
 * 日志却报"重传 5 次无回应"）。
 */
static void midr_ctrl_drop_half_peer(struct bgp *bgp, struct in_addr transport,
				     const char *why)
{
	union sockunion su;
	struct peer *peer;

	midr_su_from_in_addr(&su, transport);
	peer = peer_lookup(bgp, &su);

	if (peer && midr_nds_peer_is_overlay(peer)) {
		zlog_info("MIDR ctrl: 拆除 %pI4 的半边会话——%s（到此为止，不再重试）",
			  &transport, why);
		peer_delete(peer);
	} else if (peer) {
		MIDR_LOG("MIDR ctrl: %pI4 死心，但该地址上是运维原生会话，只销账不拆会话（⑦ 守卫）",
			 &transport);
	}

	midr_nds_ledger_drop(bgp, transport);

	/* 账销了、会话拆了，节点表那边也得跟着收拾：停探 + 撤链路上报 + 清
	 * link_entry + 清 is_adjacent，否则 CL 还当它是条好边、PM 还对着它探。 */
	midr_nds_cleanup_by_transport(bgp, transport,
				      MIDR_STOP_KEEPALIVE_TIMEOUT);
}

/*
 * Resend pending requests.  PEER_REQUEST terminates when its reverse session
 * reaches Established; the list requests terminate when their response arrives
 * (removed via midr_ctrl_drop_pending in the recv path).  All give up after
 * their own params.count attempts.
 */
static void midr_ctrl_retx_timer(struct event *t)
{
	struct bgp *bgp = EVENT_ARG(t);
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct listnode *node, *nnode;
	struct midr_ctrl_pending *p;
	struct in_addr rep_failed = { 0 }; /* §8.32：本轮死心的 REP_LIST_REQ 目标 */
	bool rep_gave_up = false;
	struct in_addr blist_failed = { 0 }; /* 本轮死心的 BOOTSTRAP_LIST_REQ 目标 */
	bool blist_gave_up = false;
	bool galloc_gave_up = false; /* 本轮死心的 GROUP_ALLOC_REQ——目标固定为
				      * 唯一发号引导，不用像 rep/blist 那样带地址 */
	bool attach_gave_up = false; /* 本轮有挂靠死心 → 循环外重挑（D2） */
	/* 死者所在批次，决定重挑从哪一批继续（批 6 的 A-3）。一拍里多条挂靠同时
	 * 死心时以最后一条为准：重挑一次补齐所有缺口，而 SECOND 只在第一批已试尽
	 * 时才会出现，取谁都不会跳过还没试的第一批候选。 */
	enum midr_attach_batch attach_batch = MIDR_ATTACH_BATCH_FIRST;
	/* 本跳实际经过的时间 = 上次武装这张表用的间隔。全队按它统一记账。 */
	int tick_ms = mi->ctrl_retx_tick_ms > 0
			      ? mi->ctrl_retx_tick_ms
			      : midr_ctrl_retx_default.interval_ms;

	for (ALL_LIST_ELEMENTS(mi->ctrl_pending, node, nnode, p)) {
		/*
		 * 各条按自己的 interval 走：没到点的这一跳一概不动（不查信号、
		 * 不烧次数、不重发）。所有条目 interval 相同时每跳人人到点，
		 * 等价于参数化之前的"单节拍、全队各减一次"。
		 */
		p->due_ms -= tick_ms;
		if (p->due_ms > 0)
			continue;
		p->due_ms = p->params.interval_ms;

		if (p->params.stop_on_signal &&
		    midr_ctrl_is_peer_req_like(p->type)) {
			union sockunion su;
			struct peer *peer;

			midr_su_from_in_addr(&su, p->target_transport);
			peer = peer_lookup(bgp, &su);
			if (peer && peer->connection->status == Established) {
				list_delete_node(mi->ctrl_pending, node);
				XFREE(MTYPE_MIDR_CTRL_PENDING, p);
				continue;
			}
		}
		/* 列表类走 TCP: 若已有在途连接 (一次尝试最长 5s), 本轮跳过、不烧
		 * 重试数——避免 retx 3s 节奏虚耗尝试次数。无在途才减数并再起一次。 */
		if (!midr_ctrl_is_peer_req_like(p->type) &&
		    midr_ctrl_tcp_client_inflight(mi, p->target_transport,
						  p->type))
			continue;

		if (--p->retries_left <= 0) {
			/*
			 * 不等完成信号的消息（stop_on_signal=false）：发满 count 次
			 * 就是干完了，不算失败——不报 warn、不拆半边、不 failover。
			 * 当前四个入队点全是 true，故走不到这里；它是记档第 13 条
			 * （给 ANNOUNCE 加重发）落地时的入口。
			 */
			if (!p->params.stop_on_signal) {
				list_delete_node(mi->ctrl_pending, node);
				XFREE(MTYPE_MIDR_CTRL_PENDING, p);
				continue;
			}
			/* 常开 warn（不进 debug 频道）：反复无响应多半是 underlay
			 * 路由缺失，静默放弃会让故障极难定位。传输中立措辞（UDP 无
			 * 响应 / TCP 连接失败皆适用）。 */
			/* 次数取入队时那份 params.count 而非默认值：挂靠第二批候选的
			 * 预算被压到 2 次，报 5 次就是日志说假话（批 6 实测抓到）。 */
			zlog_warn("MIDR ctrl: %s 尝试 %d 次无响应，放弃（目标 %pI4）——请检查本端到 %pI4 的 underlay 路由（transport 互通前提）",
				  midr_ctrl_msg_type_str(p->type),
				  p->params.count, &p->target_transport,
				  &p->target_transport);
			if (p->type == MIDR_CTRL_REP_LIST_REQ) {
				rep_failed = p->target_transport;
				rep_gave_up = true;
			}
			/* ② 拉名单问不到：换下一个候选（兜底遍历），与上面的
			 * join failover 是两条独立的路——各自的游标、各自的守卫，
			 * 互不影响（子稿 §4-3"不接 join 线"）。 */
			if (p->type == MIDR_CTRL_BOOTSTRAP_LIST_REQ) {
				blist_failed = p->target_transport;
				blist_gave_up = true;
			}
			/* 发号引导问不到：没有候补名单可换（只信任那一台确定性选出
			 * 的分配者），直接回落本地估算，见
			 * midr_nds_group_alloc_failed()。 */
			if (p->type == MIDR_CTRL_GROUP_ALLOC_REQ)
				galloc_gave_up = true;
			/*
			 * 死心拆除（方案定稿结论 23，08-11 批 2）：PEER_REQUEST
			 * 死心 = 对端始终没回配，本机当初为它预配的**半边 peer**
			 * 就此永远停在 Active——不拆的话它跟着进程一辈子，
			 * `show midr neighbors` 里长期挂着一行僵尸、台账也留着一笔
			 * 无主的账（批 0 实测：死心后 5 分钟仍在，零重传零回收）。
			 *
			 * 只做三件事，**到此为止**：过 ⑦ 守卫认人 → 拆半边 → 销账。
			 * 不回调、S5 那两道去重一行不改。
			 *
			 * ⚠ 两种建连请求在"换不换一个试"上不同（D2 之后）：
			 *   - PEER_REQUEST **没有** failover——它的三个场景全是"指名连
			 *     这一个"（同群某成员 / CL 点名的锚点 / 运维手配），没有替补
			 *     池，换谁都不对（结论 23 那句"没有换一个试"说的是它）；
			 *   - ATTACH_REQUEST **有** failover——挂靠是全代码第一个有替补池
			 *     的建连场景（名单里几台引导挂哪两台都行），故死心后给候选盖
			 *     attach_failed 章并改挑下一台。
			 */
			if (midr_ctrl_is_peer_req_like(p->type)) {
				char why[96];

				/* 类型名取自帧本身：挂靠死心时要说 ATTACH_REQUEST，
				 * 写死 "PEER_REQUEST" 就是日志说假话（同型错批 2
				 * 踩过一次，`why` 入参正是那次加的）。 */
				snprintf(why, sizeof(why),
					 "%s 重传 %d 次无回应，对端始终未回配",
					 midr_ctrl_msg_type_str(p->type),
					 p->params.count);
				midr_ctrl_drop_half_peer(bgp,
							 p->target_transport,
							 why);
				/* 盖章是纯状态改动（只碰候选池），循环内安全；
				 * 重挑放循环外——attach_pick 会 connect、往
				 * ctrl_pending 追加条目，边遍历边改必崩。 */
				if (p->type == MIDR_CTRL_ATTACH_REQUEST) {
					/* 记下死者属哪一批：重挑从那一批接着走
					 * （批 6 的 A-3），别空扫已试尽的第一批。 */
					attach_batch = midr_nds_attach_mark_failed(
						bgp, p->target_transport);
					attach_gave_up = true;
				}
			}
			list_delete_node(mi->ctrl_pending, node);
			XFREE(MTYPE_MIDR_CTRL_PENDING, p);
			continue;
		}
		midr_ctrl_request_attempt(bgp, p->target_transport, p->type,
					  p->target_group);
	}

	/* §8.32 failover：入网第一跳（REP_LIST_REQ）死心 → 回调 NDS 换下一候选。
	 * 放循环外：回调会向新候选发请求、往 ctrl_pending 追加条目，循环内调用
	 * = 边遍历边改表。NDS 侧守卫（意图仍在 + 地址==游标候选）保证过期/串话
	 * 的回调被安全忽略；放在重挂判断之前，新请求的重试定时器才能被武装。 */
	if (rep_gave_up)
		midr_join_bootstrap_failed(bgp, rep_failed);
	/* 同理放循环外：回调会向下一个候选发请求、往 ctrl_pending 追加条目。 */
	if (blist_gave_up)
		midr_nds_bootstrap_list_failed(bgp, blist_failed);
	/* 发号请求死心 → 直接回落本地估算完成落定（无候补池，见上）。 */
	if (galloc_gave_up)
		midr_nds_group_alloc_failed(bgp);
	/* 挂靠死心的换台（D2）。同样放循环外，且必须排在下面重挂重试定时器**之前**
	 * ——新发出的 ATTACH_REQUEST 才能被这一轮的定时器武装上重传。
	 * 判据（是代表 ∧ ATTACH 账不足 K ∧ 还有没盖章的候选）全在 pick 内部。 */
	if (attach_gave_up)
		midr_nds_attach_pick_from(bgp, attach_batch);

	/*
	 * 下一跳的间隔 = 全队里最近的一个到期时刻。必须排在上面三个回调**之后**
	 * ——它们会往队里追加新条目（due_ms = 各自的 interval），漏算就会让新
	 * 请求等过头。
	 */
	if (!list_isempty(mi->ctrl_pending)) {
		int next_ms = 0;

		for (ALL_LIST_ELEMENTS_RO(mi->ctrl_pending, node, p))
			if (!next_ms || p->due_ms < next_ms)
				next_ms = p->due_ms;
		if (next_ms < 1)
			next_ms = 1;
		mi->ctrl_retx_tick_ms = next_ms;
		event_add_timer_msec(bm->master, midr_ctrl_retx_timer, bgp,
				     next_ms, &mi->t_ctrl_retx);
	}
}

/*
 * 已写入 REP_LIST 应答的条目里是否存在 (group_id, transport)——判重键与
 * midr_rep_dir_add 一致 (学侧/配侧/合并三处同键)。直接读回 stream 已写区,
 * 免维护第二份集合; memcpy 取条目规避对齐/别名问题, 条数小 O(n²) 无碍。
 */
static bool midr_ctrl_rep_list_contains(const struct stream *s, uint32_t count,
					uint32_t group_id,
					struct in_addr transport)
{
	const uint8_t *base =
		STREAM_DATA(s) + sizeof(struct midr_ctrl_list_hdr);
	struct midr_ctrl_rep_item it;

	for (uint32_t i = 0; i < count; i++) {
		memcpy(&it, base + (size_t)i * sizeof(it), sizeof(it));
		if (ntohl(it.group_id) == group_id &&
		    it.rep_transport.s_addr == transport.s_addr)
			return true;
	}
	return false;
}

/*
 * 组装一个 REP_LIST_RESP payload (hdr + items) 到 stream, 返回之 (调用方/传输层
 * 负责前缀封帧与释放)。TCP 流式无 LIST_MAX 上限, 唯一边界是 u16 count (>65535
 * 告警截断——消灭原 UDP 版的静默截断)。dst 仅用于日志。
 *
 * 条目两来源 (任务甲, 2026-07-10):
 *   1) mi->rep_dir (手配/学来) 全量排前——CL 现为"取首条"占位策略, 应答条目序
 *      = 客户端目录序, 排前即"人工覆盖/应急兜底"的实际生效机制; 真 CL 按链路
 *      质量选优后自动退化为并列候选, 无需再改。
 *   2) midr_rep_candidates() 从 global_view 按 GROUP_REP 位推导的候选, 与已写
 *      条目 (group_id, transport) 重合的跳过。
 * 合并后 0 条则返回 NULL = 不回包 (沉默同闸门): 回空表会让客户端清目录+销重
 * 试项后一次性"放弃加入", 沉默则 3s×5 重试可等 BGP-LS 收敛自愈——自动化使
 * "目录空"成为引导节点重启后的必经收敛窗口, 必须保住重试。
 *
 * 注意: 用 stream_new() 按精确条数预分配, 不用 stream_new_expandable()。因为
 * 底层 stream_put() 会先跑 CHECK_SIZE 把写入长度截到当前缓冲余量、再判断是否
 * 扩容——即 raw stream_put 对可扩 stream 也不会真正扩容 (FRR 已知 wart)。预分
 * 配到位则 CHECK_SIZE 永不触发, 整块写入正确 (上界过分配无害, endp 反映实际)。
 */
static struct stream *midr_ctrl_build_rep_list(struct bgp *bgp,
					       struct in_addr dst)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct list *derived = list_new();
	struct stream *s;
	struct listnode *node;
	struct midr_rep_entry *r;
	struct midr_node_entry *ne;
	uint32_t maxn;
	uint32_t count = 0, n_dir = 0, n_derived = 0, n_dedup = 0;
	size_t count_pos;

	midr_rep_candidates(bgp, derived);
	maxn = listcount(mi->rep_dir) + listcount(derived);
	if (maxn > 65535)
		maxn = 65535;
	s = stream_new(sizeof(struct midr_ctrl_list_hdr) +
		       (size_t)maxn * sizeof(struct midr_ctrl_rep_item));

	stream_putc(s, MIDR_CTRL_MSG_VERSION);
	stream_putc(s, MIDR_CTRL_REP_LIST_RESP);
	count_pos = stream_get_endp(s);
	stream_putw(s, 0); /* count 占位, 末尾回填 */

	/* 来源一: rep_dir 全量, 排前 (覆盖生效机制, 见函数头) */
	for (ALL_LIST_ELEMENTS_RO(mi->rep_dir, node, r)) {
		struct midr_ctrl_rep_item item;

		if (count >= 65535) {
			zlog_warn("MIDR ctrl: rep directory exceeds 65535 — REP_LIST_RESP truncated");
			break;
		}
		item.group_id = htonl(r->group_id);
		item.rep_transport = r->rep_transport;
		item.rep_asn = htonl((uint32_t)r->rep_asn);
		/* v2 真名栏: rep_dir 里的条目一律自带 rid——手配来源已随
		 * `midr rep group` 删除 (2026-08-11 批 1.5), 剩下的唯一来源是
		 * "收 REP_LIST_RESP 全量替换", 那些条目的 rid 由对端按节点表真名
		 * 键填好。原先"手配条目 rid=0 → 应答时刻按 transport 反查回填"的
		 * 那一步随之取消 (反查不中会发 0, 正是影子条目的源头)。 */
		item.rep_rid = r->rep_rid;
		stream_put(s, &item, sizeof(item));
		count++;
		n_dir++;
	}

	/* 来源二: 推导候选 (has_transport_addr 已由 midr_rep_candidates 保证) */
	for (ALL_LIST_ELEMENTS_RO(derived, node, ne)) {
		struct midr_ctrl_rep_item item;

		if (count >= 65535) {
			zlog_warn("MIDR ctrl: rep directory exceeds 65535 — REP_LIST_RESP truncated");
			break;
		}
		if (midr_ctrl_rep_list_contains(s, count, ne->group_id,
						ne->transport_addr)) {
			n_dedup++;
			continue;
		}
		item.group_id = htonl(ne->group_id);
		item.rep_transport = ne->transport_addr;
		item.rep_asn = htonl((uint32_t)ne->asn);
		item.rep_rid = ne->node_id.u.prefix4; /* 节点表按真名为键, rid 现成 */
		stream_put(s, &item, sizeof(item));
		count++;
		n_derived++;
	}
	list_delete(&derived);

	if (count == 0) {
		/* 沉默保重试 (见函数头); 客户端重试等收敛后再答 */
		MIDR_LOG("MIDR ctrl: rep directory empty (view not converged?) — not answering %pI4",
			 &dst);
		stream_free(s);
		return NULL;
	}

	stream_putw_at(s, count_pos, (uint16_t)count);
	MIDR_FLOW_LOG("MIDR ctrl: sent REP_LIST_RESP (%u reps: %u rep_dir + %u derived, %u deduped) to %pI4",
		      count, n_dir, n_derived, n_dedup, &dst);
	return s;
}

/*
 * 组装一个 MEMBER_LIST_RESP payload (table A, 含自己在前, 使加入节点必与 rep
 * 建连), 精确预分配 stream (原因见 build_rep_list)。上限同上 (u16 count)。
 */
static struct stream *midr_ctrl_build_member_list(struct bgp *bgp,
						  struct in_addr dst,
						  uint32_t group_id)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct stream *s;
	struct list *members;
	struct listnode *node;
	struct midr_node_entry *entry;
	uint32_t count = 0;
	uint32_t maxn;
	size_t count_pos;

	/* 先取成员表以便精确预分配 stream (原因见 build_rep_list 注释)。上界 = 自己
	 * + 全部成员 (AF_INET 过滤可能更少, 过分配无害: endp 反映实际写入)。 */
	members = list_new();
	midr_group_members(bgp, group_id, members);
	maxn = 1 + listcount(members);
	if (maxn > 65535)
		maxn = 65535;
	s = stream_new(sizeof(struct midr_ctrl_list_hdr) +
		       (size_t)maxn * sizeof(struct midr_ctrl_member_item));

	stream_putc(s, MIDR_CTRL_MSG_VERSION);
	stream_putc(s, MIDR_CTRL_MEMBER_LIST_RESP);
	count_pos = stream_get_endp(s);
	stream_putw(s, 0);

	/* Ourselves (the representative) first. */
	if (mi->transport_addr_set) {
		struct midr_ctrl_member_item item;

		item.rid = bgp->router_id;
		item.transport = mi->local_transport_addr;
		item.asn = htonl(bgp->as);
		item.group_id = htonl(group_id);
		stream_put(s, &item, sizeof(item));
		count++;
	}

	for (ALL_LIST_ELEMENTS_RO(members, node, entry)) {
		struct prefix locator;
		struct midr_ctrl_member_item item;

		if (count >= 65535) {
			zlog_warn("MIDR ctrl: group %u members exceed 65535 — MEMBER_LIST_RESP truncated",
				  group_id);
			break;
		}
		midr_node_get_locator(entry, &locator);
		if (locator.family != AF_INET)
			continue;
		item.rid = entry->node_id.u.prefix4;
		item.transport = locator.u.prefix4;
		item.asn = htonl(entry->asn);
		item.group_id = htonl(group_id);
		stream_put(s, &item, sizeof(item));
		count++;
	}
	list_delete(&members);

	stream_putw_at(s, count_pos, (uint16_t)count);
	MIDR_FLOW_LOG("MIDR ctrl: sent MEMBER_LIST_RESP (%u members) for group %u to %pI4",
		      count, group_id, &dst);
	return s;
}

/*
 * 引导侧：组装 BOOTSTRAP_LIST_RESP（hdr + 8B 条目），子稿 §2②。
 *
 * 内容 = **手配名单 ∩ 自己骨干会话活性** + **本机自己**：
 *   - 手配名单 = 本机 bootstrap_list（`midr bootstrap` 逐条配的，引导之间互配）。
 *     运维登记是主来源；08-21 起引导也发 Node NLRI，学到带 BOOTSTRAP 位的条目会
 *     以 SEED 身份补进同一个池（midr_maybe_save_bootstrap_seed），本函数一视同仁。
 *   - 活性过滤 = 到该地址的 BGP 会话是否 Established（判活统一走会话状态，子稿
 *     §1 前提二）。运维 `no midr session` 拆掉的骨干边，会话一没就自然掉出名单，
 *     不必另查排除名单（排除只管建连、不管发名单，答疑 69）。
 *   - **含本机自己**：这一条是对子稿字面（"手配名单 ∩ 活性"）的有意扩展。不含
 *     的话每台引导答出的环各缺自己 → 代表拿到的环因"问了谁"而不同，哈希"顺次
 *     取 K"的确定可重放性（答疑 58）就没了，而且答话的这台永远挂不上任何代表。
 *     自己活着是答得出这一包的前提，无需再判活。
 *
 * 0 条则返回 NULL 沉默（同 build_rep_list 的口径）：引导刚起、骨干还没收敛时
 * 回空表会让对方误以为"天下无引导"，沉默则对方 3s×5 重试可等收敛。
 */
static struct stream *midr_ctrl_build_bootstrap_list(struct bgp *bgp,
						     struct in_addr dst)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct stream *s;
	struct listnode *node;
	struct midr_bootstrap_entry *b;
	struct midr_ctrl_bootstrap_item item;
	uint32_t count = 0, n_skip = 0;
	uint32_t maxn;
	size_t count_pos;

	/* 精确预分配（原因见 build_rep_list：raw stream_put 对可扩 stream 也不
	 * 真扩容，FRR 已知 wart）。上界 = 自己 + 全部候选，过分配无害。 */
	maxn = 1 + listcount(mi->bootstrap_list);
	if (maxn > 65535)
		maxn = 65535;
	s = stream_new(sizeof(struct midr_ctrl_list_hdr) +
		       (size_t)maxn * sizeof(struct midr_ctrl_bootstrap_item));

	stream_putc(s, MIDR_CTRL_MSG_VERSION);
	stream_putc(s, MIDR_CTRL_BOOTSTRAP_LIST_RESP);
	count_pos = stream_get_endp(s);
	stream_putw(s, 0); /* count 占位，末尾回填 */

	if (mi->transport_addr_set) {
		item.transport = mi->local_transport_addr;
		item.asn = htonl(bgp->as);
		/* 自己这条的 rid 直接取本机 router-id——最权威的来源，不必绕候选池
		 * （批 5 R 系列，协议 v3 条目三栏）。 */
		item.rid = bgp->router_id;
		stream_put(s, &item, sizeof(item));
		count++;
	}

	for (ALL_LIST_ELEMENTS_RO(mi->bootstrap_list, node, b)) {
		if (count >= 65535) {
			zlog_warn("MIDR ctrl: bootstrap list exceeds 65535 — BOOTSTRAP_LIST_RESP truncated");
			break;
		}
		/* 自己已在首条（手配名单理论上不含自己，防御性去重）。 */
		if (mi->transport_addr_set &&
		    b->transport.s_addr == mi->local_transport_addr.s_addr)
			continue;
		if (!midr_ctrl_transport_session_up(bgp, b->transport)) {
			n_skip++;
			continue;
		}
		item.transport = b->transport;
		item.asn = htonl((uint32_t)b->asn);
		/* rid 来自候选池条目，而池内 rid 恒非 0（入口把关，见
		 * midr_bootstrap_list_add）——运维手配 `midr bootstrap` 时必填。 */
		item.rid = b->rid;
		stream_put(s, &item, sizeof(item));
		count++;
	}

	if (count == 0) {
		MIDR_LOG("MIDR ctrl: 活引导名单为空（本机 transport 未配？骨干会话未起？）——不回答 %pI4",
			 &dst);
		stream_free(s);
		return NULL;
	}

	stream_putw_at(s, count_pos, (uint16_t)count);
	MIDR_FLOW_LOG("MIDR ctrl: sent BOOTSTRAP_LIST_RESP (%u 台活引导，含本机；%u 个候选因会话未起被滤掉) to %pI4",
		      count, n_skip, &dst);
	return s;
}

/*
 * Allocator side: hand out the next group id from a monotonic counter.
 * Seeded lazily (on first request, not at ctrl-init) from a scan of this
 * bootstrap's own node table — the same cl_max_group_id() computation CL
 * does locally, just run here where the view is far more complete than any
 * freshly-joining node's. From then on we only ever increment, never
 * re-scan: a group can still be momentarily invisible in global_view due to
 * NLRI propagation lag, so re-deriving the baseline on every request could
 * hand out a number that collides with one already granted a moment ago.
 *
 * Any bootstrap answers if asked (no local election check here) — refusing
 * would just make the requester time out and fall back anyway, no safety
 * gained. The correctness property this protocol leans on is "only one
 * process is ever actually asked" (every node computes the same elected
 * allocator independently, see midr_nds_pick_group_allocator() in
 * bgp_midr_nds.c), not "only one process is capable of answering".
 */
static struct stream *midr_ctrl_build_group_alloc_resp(struct bgp *bgp,
							struct in_addr dst)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct stream *s;
	struct midr_ctrl_group_alloc_item item;
	uint32_t gid;

	if (!(mi->local_capabilities & MIDR_CAP_BOOTSTRAP)) {
		MIDR_LOG("MIDR ctrl: ignoring GROUP_ALLOC_REQ from %pI4 (caps 0x%x — not a bootstrap)",
			 &dst, mi->local_capabilities);
		return NULL;
	}

	if (!mi->group_alloc_seeded) {
		struct list *nodes = midr_nds_cl_nodes_getter(mi->global_view);
		struct listnode *n;
		struct midr_node_entry *entry;
		uint32_t max_id = 0;

		for (ALL_LIST_ELEMENTS_RO(nodes, n, entry))
			if (entry->group_id > max_id)
				max_id = entry->group_id;
		list_delete(&nodes);

		mi->group_alloc_next = max_id + 1;
		mi->group_alloc_seeded = true;
		MIDR_LOG("MIDR ctrl: group-id 发号器起播，起始值 %u（本机节点表当前最大群号 %u）",
			 mi->group_alloc_next, max_id);
	}

	gid = mi->group_alloc_next++;

	s = stream_new(sizeof(struct midr_ctrl_list_hdr) + sizeof(item));
	stream_putc(s, MIDR_CTRL_MSG_VERSION);
	stream_putc(s, MIDR_CTRL_GROUP_ALLOC_RESP);
	stream_putw(s, 1); /* count，恒为 1 */
	item.group_id = htonl(gid);
	stream_put(s, &item, sizeof(item));

	MIDR_FLOW_LOG("MIDR ctrl: GROUP_ALLOC_REQ from %pI4 — 分配群 %u",
		      &dst, gid);
	return s;
}

/* 代表侧：收下一份活引导名单——逐条吸收（候选池 + 种子库），再交 NDS 汇总。 */
static void midr_ctrl_recv_bootstrap_list(struct bgp *bgp, const uint8_t *buf,
					  ssize_t n, struct in_addr src)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	const struct midr_ctrl_list_hdr *hdr =
		(const struct midr_ctrl_list_hdr *)buf;
	const struct midr_ctrl_bootstrap_item *items;
	uint16_t count, i;

	if (n < (ssize_t)sizeof(*hdr))
		return;
	count = ntohs(hdr->count);
	/* 严格等长（同 REP_LIST_RESP）：不匹配 = 对端 bug / 损坏，丢弃。 */
	if (n != (ssize_t)(sizeof(*hdr) + (size_t)count * sizeof(*items))) {
		MIDR_LOG("MIDR ctrl: BOOTSTRAP_LIST_RESP length %zd != expected for count %u — dropping",
			 (ssize_t)n, count);
		return;
	}
	items = (const struct midr_ctrl_bootstrap_item *)(buf + sizeof(*hdr));

	midr_ctrl_drop_pending(mi, src, MIDR_CTRL_BOOTSTRAP_LIST_REQ);

	/* 开收：先清上一份的批次标记，下面逐条置回（批 6 缺口 A 的"减法"半边——
	 * 少了它，名单只做加法，已下线的引导永远留在候选池里且照样被优先挑中）。 */
	midr_nds_bootstrap_list_begin(bgp);

	/* 用完即弃语义：名单不落任何常驻表（不进 global_view，子稿 §5 已否决），
	 * 只喂候选池与种子库——前者供 failover/挂靠挑选，后者供重启自举。 */
	for (i = 0; i < count; i++)
		midr_nds_bootstrap_learn(bgp, items[i].transport,
					 (as_t)ntohl(items[i].asn),
					 items[i].rid);

	midr_nds_on_bootstrap_list(bgp, src, count);
}

/* Requester side: unpack the allocated id and hand it to NDS to finish the
 * CREATE that's been waiting on it. Malformed/zero replies are treated the
 * same as a give-up — NDS falls back to its own local estimate either way. */
static void midr_ctrl_recv_group_alloc(struct bgp *bgp, const uint8_t *buf,
				       ssize_t n, struct in_addr src)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	const struct midr_ctrl_list_hdr *hdr =
		(const struct midr_ctrl_list_hdr *)buf;
	const struct midr_ctrl_group_alloc_item *item;
	uint16_t count;
	uint32_t gid;

	if (n < (ssize_t)sizeof(*hdr))
		return;
	count = ntohs(hdr->count);
	if (count != 1 ||
	    n != (ssize_t)(sizeof(*hdr) + sizeof(struct midr_ctrl_group_alloc_item))) {
		MIDR_LOG("MIDR ctrl: GROUP_ALLOC_RESP malformed (count=%u len=%zd) from %pI4 — dropping",
			 count, n, &src);
		return;
	}
	item = (const struct midr_ctrl_group_alloc_item *)(buf + sizeof(*hdr));
	gid = ntohl(item->group_id);

	midr_ctrl_drop_pending(mi, src, MIDR_CTRL_GROUP_ALLOC_REQ);

	if (gid == 0) {
		zlog_warn("MIDR ctrl: GROUP_ALLOC_RESP from %pI4 returned 群 0（非法）——回退本地估算",
			  &src);
		midr_nds_group_alloc_failed(bgp);
		return;
	}

	MIDR_FLOW_LOG("MIDR ctrl: GROUP_ALLOC_RESP from %pI4 — 分得群 %u", &src, gid);
	midr_nds_group_alloc_done(bgp, gid);
}

/* New node: store the bootstrap's rep directory, then run join stage 1. */
static void midr_ctrl_recv_rep_list(struct bgp *bgp, const uint8_t *buf,
				    ssize_t n, struct in_addr src)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	const struct midr_ctrl_list_hdr *hdr =
		(const struct midr_ctrl_list_hdr *)buf;
	const struct midr_ctrl_rep_item *items;
	uint16_t count, i;

	if (n < (ssize_t)sizeof(*hdr))
		return;
	count = ntohs(hdr->count);
	/* TCP 帧长由本端定义, 收紧为严格等长 (不匹配 = 对端 bug / 损坏, 丢弃)。
	 * 原 UDP 版的 LIST_MAX clamp 已随迁移删除——不删则 >80 成员仍被砍。 */
	if (n != (ssize_t)(sizeof(*hdr) + (size_t)count * sizeof(*items))) {
		MIDR_LOG("MIDR ctrl: REP_LIST_RESP length %zd != expected for count %u — dropping",
			 (ssize_t)n, count);
		return;
	}
	items = (const struct midr_ctrl_rep_item *)(buf + sizeof(*hdr));

	midr_rep_dir_clear(bgp);
	for (i = 0; i < count; i++)
		midr_rep_dir_add(bgp, ntohl(items[i].group_id),
				 items[i].rep_transport,
				 (as_t)ntohl(items[i].rep_asn),
				 items[i].rep_rid);

	midr_ctrl_drop_pending(mi, src, MIDR_CTRL_REP_LIST_REQ);
	MIDR_FLOW_LOG("MIDR ctrl: REP_LIST_RESP with %u reps — starting join", count);
	midr_join_on_rep_list(bgp);
}

/*
 * New node: learn the rep's member list (table A) into the global view and
 * probe each (I-1).  Connecting is deferred to the JOIN decision (see ⑥):
 * only after CL judges the group worth joining does NDS connect_group.
 *
 * RECOMMEND 阶段现在最多并发发出 3 个 MEMBER_LIST_REQ——主候选群
 * （mi->join_group_id）+ 至多 2 个次优群（mi->anchor_group_id[]）。
 * 三路响应都会落到这同一个函数，靠响应里自带的 group_id（items[0]，代表本身
 * 那条）区分是哪一路：命中 join_group_id 走原有入群评估路径；命中
 * anchor_group_id[] 走新的锚点候选路径（不置 is_adjacent，避免污染本群邻接
 * 统计；见 midr_nds_learn_anchor_candidate 头注释）；两者都不命中视为过期/
 * 串台响应（例如上一轮 join 的迟到响应），只清 pending、不灌表。
 */
static void midr_ctrl_recv_member_list(struct bgp *bgp, const uint8_t *buf,
				       ssize_t n, struct in_addr src)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	const struct midr_ctrl_list_hdr *hdr =
		(const struct midr_ctrl_list_hdr *)buf;
	const struct midr_ctrl_member_item *items;
	uint16_t count, i;
	uint32_t resp_group;
	bool is_join_candidate, is_anchor;

	if (n < (ssize_t)sizeof(*hdr))
		return;
	count = ntohs(hdr->count);
	/* 严格等长 (同 REP_LIST_RESP); LIST_MAX clamp 已删。 */
	if (n != (ssize_t)(sizeof(*hdr) + (size_t)count * sizeof(*items))) {
		MIDR_LOG("MIDR ctrl: MEMBER_LIST_RESP length %zd != expected for count %u — dropping",
			 (ssize_t)n, count);
		return;
	}
	items = (const struct midr_ctrl_member_item *)(buf + sizeof(*hdr));

	/* 收到响应 → 停止对这个目的地重传（不管下面判到哪一路）。 */
	midr_ctrl_drop_pending(mi, src, MIDR_CTRL_MEMBER_LIST_REQ);

	if (count == 0) {
		MIDR_LOG("MIDR ctrl: MEMBER_LIST_RESP from %pI4 为空，无法判断所属群，忽略",
			 &src);
		return;
	}
	/* 代表自己那条（组装时放最前）自带真实群号，用它判定这是哪一路响应。 */
	resp_group = ntohl(items[0].group_id);
	is_join_candidate =
		mi->join_group_id != 0 && resp_group == mi->join_group_id;
	is_anchor = !is_join_candidate && resp_group != 0 &&
		    (resp_group == mi->anchor_group_id[0] ||
		     resp_group == mi->anchor_group_id[1]);

	if (!is_join_candidate && !is_anchor) {
		MIDR_LOG("MIDR ctrl: MEMBER_LIST_RESP 群 %u 与当前候选群/锚点群都不符（过期响应？），忽略",
			 resp_group);
		return;
	}

	/*
	 * ⑥ 先探后判：把成员表灌入 global_view、按所属路径决定是否标邻居
	 * （is_adjacent，仅 join 候选群路径）并 I-1 探测，但【不在此建连】。
	 * 建连推迟到 I-7 决策之后（JOIN 走 connect_group；ANCHOR 走
	 * midr_ctrl_connect，见 midr_nds_on_cluster_decision）。
	 */
	for (i = 0; i < count; i++) {
		uint32_t item_gid = ntohl(items[i].group_id);

		if (IPV4_ADDR_SAME(&items[i].rid, &bgp->router_id))
			continue; /* 跳过描述自己的条目 */

		if (is_join_candidate) {
			midr_nds_learn_member(bgp, items[i].rid,
					      ntohl(items[i].asn),
					      items[i].transport, item_gid);
			MIDR_FLOW_LOG("MIDR 加入：I-1 探测成员 %pI4（群 %u）",
				      &items[i].rid, item_gid);
		} else {
			midr_nds_learn_anchor_candidate(
				bgp, items[i].rid, ntohl(items[i].asn),
				items[i].transport, item_gid);
			MIDR_FLOW_LOG("MIDR 锚点：I-1 探测锚点候选 %pI4（次优群 %u）",
				      &items[i].rid, item_gid);
		}

		/*
		 * 该成员从未跟我们交换过任何报文（我们是从群代表的
		 * MEMBER_LIST_RESP 里间接得知它的），它的 pm_is_known_transport
		 * 校验会把我们刚发起的探测包当未知来源丢弃。发一个单向 ANNOUNCE
		 * 自报身份，让它记住我们——不等回复、不触发建连。两条路径的候选
		 * 都需要，语义相同。走 midr_ctrl_send_announce() 而非直接
		 * midr_ctrl_send_req()，好让这条也吃到 ANNOUNCE 的重传预算
		 * （单包丢失不再是永久性的 unknown-transport）。
		 */
		midr_ctrl_send_announce(bgp, items[i].transport);
	}

	if (is_join_candidate) {
		/*
		 * 探完整批成员后，编排层显式发 MEMBER_PROBE_DONE，交 CL 评估是否
		 * 入群（I-7 JOIN/CREATE）。一整批只发一次。不在此清 join_phase：
		 * 收尾在 midr_nds_on_cluster_decision 的 JOIN/CREATE 分支。
		 */
		MIDR_FLOW_LOG("MIDR 加入：收到群 %u 成员列表，MEMBER_PROBE_DONE 将在 %d 秒后触发（等待 EWMA 热身）",
			      mi->join_group_id, MIDR_JOIN_PROBE_WAIT_SECS);
		event_cancel(&mi->t_member_probe_done);
		event_add_timer(bm->master, midr_ctrl_member_probe_done_cb,
				bgp, MIDR_JOIN_PROBE_WAIT_SECS,
				&mi->t_member_probe_done);
	} else {
		/*
		 * 两个次优群各自异步到达，共用一个定时器——每到一路就重新起
		 * MIDR_JOIN_PROBE_WAIT_SECS 秒倒计时（与 REP/MEMBER 热身同款
		 * 手法），保证较晚到的那一路也能拿到足够的探测热身时间；只有
		 * 一路候选时同样适用（第一路到达即开始倒计时）。
		 */
		MIDR_FLOW_LOG("MIDR 锚点：收到次优群 %u 成员列表，ANCHOR_PROBE_DONE 将在 %d 秒后触发（等待 EWMA 热身）",
			      resp_group, MIDR_JOIN_PROBE_WAIT_SECS);
		event_cancel(&mi->t_anchor_probe_done);
		event_add_timer(bm->master, midr_ctrl_anchor_probe_done_cb,
				bgp, MIDR_JOIN_PROBE_WAIT_SECS,
				&mi->t_anchor_probe_done);
	}
}

/* ------------------------------------------------------------------ */
/* TCP 列表交换 —— 语义层回调 (被 bgp_midr_ctrl_tcp.c 调用)             */
/* ------------------------------------------------------------------ */

/*
 * 传输层收到一个完整请求帧 (20B midr_ctrl_msg) 后回调。返回响应 payload stream
 * (不含长度前缀, 由传输层封帧发出); 返回 NULL = 不回包 (闸门不过, 沉默同 UDP)。
 * 闸门逻辑与日志文案照搬 UDP 版 midr_ctrl_udp_recv 的对应分支。
 */
struct stream *midr_ctrl_on_tcp_request(struct bgp *bgp, const uint8_t *payload,
					size_t len, struct in_addr remote)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct midr_ctrl_msg msg;

	if (!mi)
		return NULL;
	if (len != sizeof(msg)) {
		MIDR_LOG("MIDR ctrl: TCP request bad length %zu (want %zu) from %pI4",
			 len, sizeof(msg), &remote);
		return NULL;
	}
	memcpy(&msg, payload, sizeof(msg));
	if (msg.version != MIDR_CTRL_MSG_VERSION)
		return NULL;

	/* 退网守卫（TCP 请求侧，同 UDP 那道的理由）：退网了就不再以引导/代表身份
	 * 应答任何列表请求。返回 NULL = 沉默不回包，与闸门不过时同款处置。 */
	if (mi->shutdown) {
		MIDR_LOG("MIDR 退网：丢弃控制通道 TCP 请求 type=%u（本机已退网）",
			 msg.type);
		return NULL;
	}

	switch (msg.type) {
	case MIDR_CTRL_REP_LIST_REQ:
		/* Only a bootstrap node answers (闸门照 MEMBER_LIST 的
		 * GROUP_REP 模式, 无组匹配项; BOOTSTRAP 位由此升"闸门+通告+
		 * 目录数据源"三职) */
		if (!(mi->local_capabilities & MIDR_CAP_BOOTSTRAP)) {
			MIDR_LOG("MIDR ctrl: ignoring REP_LIST_REQ from %pI4 (caps 0x%x — not a bootstrap)",
				 &remote, mi->local_capabilities);
			return NULL;
		}
		/*
		 * 这里【不】learn_requester（保底轮 2 批 5 前置②）——**别照
		 * "放行 PM 探测"的旧理由加回来**。
		 *
		 * 那行原是 zhc 07-08 与 ANNOUNCE 同批写的过渡产物：当时 ANNOUNCE 只
		 * 发给成员列表里的群成员，目录里的代表没有自我介绍的路，只能靠"谁收
		 * 到我的请求谁记一条"，所以 REP_LIST_REQ 与 MEMBER_LIST_REQ 两处都
		 * 记。后来补上了"给目录里每个代表都发 ANNOUNCE"的通用机制
		 * （midr_join_on_rep_list 对每个 rep 先 send_announce 再
		 * pm_add_target），这条 TCP 路就退化成**引导独有的冗余**——所有普通
		 * 代表都只靠 ANNOUNCE 一条 UDP 路，唯独引导多一条；留着才是给引导开
		 * 小灶，删掉才是一视同仁。
		 *
		 * 更要紧的是它有害：它建的条目只有 rid/transport/asn，caps 与
		 * group_id 全 0（XCALLOC 清零），15s 内引导若收到该代表的挂靠请求，
		 * 负面守卫查表看到"caps 空"就判成普通成员把它拒掉——设计原话"查不到
		 * 放行"被这条幽灵条目变成了"查到了，是普通成员"。
		 *
		 * 安全性已核：pm_add_target 全树 4 个调用点无一以引导为目标；CL 侧无
		 * 按 BOOTSTRAP 位取指标的逻辑；兼任引导仍被 ANNOUNCE 兜住；终态引导
		 * 专职后压根不进 rep 目录（midr_rep_candidates 双重排除）。
		 */
		MIDR_FLOW_LOG("MIDR ctrl: REP_LIST_REQ from %pI4 — replying with rep directory",
			      &msg.requester_transport);
		return midr_ctrl_build_rep_list(bgp, remote);
	case MIDR_CTRL_MEMBER_LIST_REQ: {
		uint32_t group = ntohl(msg.target_group);

		/* Only a representative of this group answers. */
		if (!(mi->local_capabilities & MIDR_CAP_GROUP_REP) ||
		    group != mi->local_group_id) {
			MIDR_LOG("MIDR ctrl: ignoring MEMBER_LIST_REQ for group %u (caps 0x%x our-group %u)",
				 group, mi->local_capabilities,
				 mi->local_group_id);
			return NULL;
		}
		/* 同 REP_LIST_REQ: 先学请求方身份, 放行其后续 PM 探测。 */
		midr_nds_learn_requester(bgp, msg.requester_rid,
					 (as_t)ntohl(msg.requester_asn),
					 msg.requester_transport);
		MIDR_FLOW_LOG("MIDR ctrl: MEMBER_LIST_REQ for group %u from %pI4 — replying",
			      group, &msg.requester_transport);
		return midr_ctrl_build_member_list(bgp, remote, group);
	}
	case MIDR_CTRL_BOOTSTRAP_LIST_REQ:
		/* 只有引导节点答（闸门同 REP_LIST_REQ：BOOTSTRAP 位既是资格也是
		 * 数据源）。不学请求方身份——这条路径之后没有 PM 探测要放行
		 * （拿名单是为了建挂靠会话，不是为了探测）。 */
		if (!(mi->local_capabilities & MIDR_CAP_BOOTSTRAP)) {
			MIDR_LOG("MIDR ctrl: ignoring BOOTSTRAP_LIST_REQ from %pI4 (caps 0x%x — not a bootstrap)",
				 &remote, mi->local_capabilities);
			return NULL;
		}
		MIDR_FLOW_LOG("MIDR ctrl: BOOTSTRAP_LIST_REQ from %pI4 — replying with live bootstrap list",
			      &msg.requester_transport);
		return midr_ctrl_build_bootstrap_list(bgp, remote);
	case MIDR_CTRL_GROUP_ALLOC_REQ:
		/* 闸门同 BOOTSTRAP_LIST_REQ：只有引导节点答，理由见
		 * midr_ctrl_build_group_alloc_resp() 头注释。不学请求方身份——
		 * 这条路径之后没有 PM 探测要放行。 */
		return midr_ctrl_build_group_alloc_resp(bgp, remote);
	default:
		MIDR_LOG("MIDR ctrl: TCP unexpected request type %u from %pI4",
			 msg.type, &remote);
		return NULL;
	}
}

/*
 * 传输层收到一个完整响应帧后回调。req_type = 本端当初发出的请求类型 (响应类型
 * 配对校验, 防串台); src = 响应来源 (并发多路 MEMBER_LIST_REQ 时, 供
 * drop_pending 精确清对应 (dst,type) 条目, 见 bgp_midr_ctrl.h 声明处注释);
 * 转现有 recv_rep_list / recv_member_list 灌视图 + 推进 join。
 */
void midr_ctrl_on_tcp_response(struct bgp *bgp, uint8_t req_type,
			       const uint8_t *payload, size_t len,
			       struct in_addr src)
{
	const struct midr_ctrl_list_hdr *hdr;

	if (len < sizeof(*hdr)) {
		MIDR_LOG("MIDR ctrl: TCP response too short (%zu)", len);
		return;
	}
	hdr = (const struct midr_ctrl_list_hdr *)payload;
	if (hdr->version != MIDR_CTRL_MSG_VERSION)
		return;

	/* 退网守卫（TCP 响应侧）：退网时 join 状态已清空，迟到的 REP/MEMBER_LIST_RESP
	 * 不该再灌视图、更不该推进一个已经不存在的 join（join_intent 那道守卫只拦
	 * REP_LIST_RESP 那一路，这里一并堵死）。 */
	if (bgp->midr_nds_info && bgp->midr_nds_info->shutdown) {
		MIDR_LOG("MIDR 退网：丢弃控制通道 TCP 响应 type=%u（本机已退网）",
			 hdr->type);
		return;
	}

	switch (hdr->type) {
	case MIDR_CTRL_REP_LIST_RESP:
		if (req_type != MIDR_CTRL_REP_LIST_REQ) {
			MIDR_LOG("MIDR ctrl: REP_LIST_RESP but our request was type %u — dropping",
				 req_type);
			return;
		}
		midr_ctrl_recv_rep_list(bgp, payload, (ssize_t)len, src);
		break;
	case MIDR_CTRL_MEMBER_LIST_RESP:
		if (req_type != MIDR_CTRL_MEMBER_LIST_REQ) {
			MIDR_LOG("MIDR ctrl: MEMBER_LIST_RESP but our request was type %u — dropping",
				 req_type);
			return;
		}
		midr_ctrl_recv_member_list(bgp, payload, (ssize_t)len, src);
		break;
	case MIDR_CTRL_BOOTSTRAP_LIST_RESP:
		if (req_type != MIDR_CTRL_BOOTSTRAP_LIST_REQ) {
			MIDR_LOG("MIDR ctrl: BOOTSTRAP_LIST_RESP but our request was type %u — dropping",
				 req_type);
			return;
		}
		midr_ctrl_recv_bootstrap_list(bgp, payload, (ssize_t)len, src);
		break;
	case MIDR_CTRL_GROUP_ALLOC_RESP:
		if (req_type != MIDR_CTRL_GROUP_ALLOC_REQ) {
			MIDR_LOG("MIDR ctrl: GROUP_ALLOC_RESP but our request was type %u — dropping",
				 req_type);
			return;
		}
		midr_ctrl_recv_group_alloc(bgp, payload, (ssize_t)len, src);
		break;
	default:
		MIDR_LOG("MIDR ctrl: TCP unexpected response type %u", hdr->type);
		break;
	}
}

/* Read one control datagram and dispatch on its type.  UDP now only carries
 * PEER_REQUEST (20B); list exchange (REP/MEMBER_LIST) moved to TCP. */
static void midr_ctrl_udp_recv(struct event *t)
{
	struct bgp *bgp = EVENT_ARG(t);
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	uint8_t buf[sizeof(struct midr_ctrl_msg)];
	struct midr_ctrl_msg msg;
	ssize_t n;

	/* Keep listening regardless of how this datagram is handled. */
	event_add_read(bm->master, midr_ctrl_udp_recv, bgp, mi->ctrl_sock,
		       &mi->t_ctrl_read);

	n = recvfrom(mi->ctrl_sock, buf, sizeof(buf), 0, NULL, NULL);
	if (n < 2) {
		if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
			zlog_warn("MIDR ctrl: recvfrom failed: %s",
				  safe_strerror(errno));
		return;
	}
	if (buf[0] != MIDR_CTRL_MSG_VERSION)
		return;

	/*
	 * 退网守卫之三（控制通道入站）。**必须放在 recvfrom 之后**：提前 return
	 * 数据报还赖在缓冲区里，读事件会立刻再触发，转成忙等。
	 *
	 * 为什么需要这一道（实测挖出，2026-08-21 骨干台子）：退网本体把会话拆净、
	 * 节点表清空之后，对端的 PEER_REQUEST 一到，本机照旧"应邀回配"，当场把会话
	 * 又建了回来——日志实录：退网收尾行的**下一行**就是 `PEER_REQUEST … peering
	 * back` + 台账重新登记。只堵 BGP-LS 收包与 I-3 递交两条路是不够的，5859
	 * 控制通道是第三条独立入站路径。
	 *
	 * 一律丢弃、不分类型：退网 = 不再参与 MIDR。回配建连（PEER_REQUEST /
	 * ATTACH_REQUEST）是参与；以引导/代表身份应答目录（REP/MEMBER/BOOTSTRAP_LIST
	 * _REQ）也是参与；迟到的 *_RESP 更不该推进一个已经不存在的 join。
	 */
	if (mi->shutdown) {
		MIDR_LOG("MIDR 退网：丢弃控制通道 UDP 消息 type=%u（本机已退网）",
			 buf[1]);
		return;
	}

	switch (buf[1]) {
	case MIDR_CTRL_PEER_REQUEST:
	case MIDR_CTRL_ATTACH_REQUEST: {
		uint32_t target_group;
		struct midr_node_entry req = {};
		enum midr_session_reason reason;
		/*
		 * 批 5 前置①：挂靠走专用类型，两类共用本分支——帧结构与回配动作
		 * 完全一致，**唯一差别是挂靠跳过引导负面守卫**（见下面 ③′）。
		 */
		bool is_attach = (buf[1] == MIDR_CTRL_ATTACH_REQUEST);

		if (n < (ssize_t)sizeof(msg))
			return;
		memcpy(&msg, buf, sizeof(msg));
		target_group = ntohl(msg.target_group);

		/*
		 * ===== 接收侧过滤（方案定稿结论 15 终形，08-11 批 2 重写）=====
		 *
		 * 原先这里是**群号白名单**："target_group 不等于本机群号就丢"。它
		 * 是群间零连接的两个死因之一（另一个是发起侧只认同群）——CL 选出的
		 * 跨群锚点、代表挂靠引导、引导之间的骨干边，全部被它挡死。白名单本
		 * 身也防不住什么：群号在报文里是明文，外人照填；"防成员越界"不在威
		 * 胁模型内，真安全要靠 ② 的鉴权。故整条删除，**不复活**（红线）。
		 *
		 * 新顺序（① 包长/版本已在上面做过）：
		 *   ② 鉴权（留位，见下）
		 *   ③ 发起方在排除名单 → 拒
		 *   ③′ 引导角色守卫（负面式，仅本机是引导时生效）
		 *   ④ 其余一律回配 + 台账登记
		 *
		 * 总原则：**结构由发起侧塑造，普通节点接收侧只记账不设卡**。
		 */

		/*
		 * ② 鉴权 —— 留位，本轮不做。
		 * PEER_REQUEST 目前无任何身份校验：谁都能伪造 requester_rid/
		 * transport 让本机去建一条会话。当前威胁模型是实验室与受控组网，
		 * 故后置；**真实部署前必须补上**（时点：骨干网完成 + 合并第一组 +
		 * 合并第二组之后）。届时走协议 v2→v3 加签名头，与本批"帧不动"
		 * 的约束不冲突——那是另一个专题。
		 */

		/*
		 * ③ 排除名单：运维 `no midr session` 拉黑过的节点，不许它反过来
		 * 把会话拽起来。这是 §G 四条里点名"接收侧还缺的那道"——此前
		 * should_peer 与 midr_ctrl_connect 各有一道，唯独接收路径没有，
		 * 于是拉黑的对端只要主动发 PEER_REQUEST 就能绕回来。
		 * 键正好对得上：名单以 router-id 为键（批 2 换键），而报文自带
		 * requester_rid。
		 */
		if (midr_nds_is_session_excluded(bgp, msg.requester_rid)) {
			MIDR_LOG("MIDR ctrl: 拒绝 %s —— 发起方 rid %pI4 在会话排除名单中",
				 midr_ctrl_msg_type_str(buf[1]),
				 &msg.requester_rid);
			midr_ctrl_send_peer_reject(bgp,
						   msg.requester_transport);
			return;
		}

		/*
		 * ③′ 引导角色守卫（负面式，**仅本机带 BOOTSTRAP 位时生效**）。
		 *
		 * 引导节点只跟两种人建会话：别的引导（骨干互连，运维手配）、以及
		 * 来挂靠的群代表。**不存在合法的"普通成员 → 引导"边**，所以在引导
		 * 上把这类请求拒掉不会制造死路（这正是"负面式"的含义：不列白名单说
		 * 谁能进，只点名一类不该来的）。
		 *
		 * 判据 = 查节点表，**明确识别**为普通成员（既非 BOOTSTRAP 又非
		 * GROUP_REP）才拒；**查不到一律放行**——宁放勿拒，为的是兜住挂靠的
		 * 时序：代表刚翻上 GROUP_REP 位就来挂靠，而本机可能还没收到它那条
		 * 带新能力位的 Node NLRI。
		 *
		 * 最坏误拒窗（定稿结论 15 记）：对端能力位变化到本机看见之间。轮 2~4
		 * 期间该窗口 = 节点表 expire 15s；轮 4 挂上第二组回调后，优雅下线是
		 * 秒级、猝死则为 holdtime。窗口内被误拒的一方会重试（3s×5），通常
		 * 下一轮就过。
		 *
		 * **挂靠请求（ATTACH_REQUEST）跳过本守卫**（批 5 前置①）：它自报
		 * 来意 = "我是群代表，来挂靠"，而本守卫针对的正是"非挂靠的普通成员
		 * 建连"。判据从"查表猜你是谁"换成"看你来干嘛"——身份会过时（幽灵
		 * 条目让刚问过路的代表看起来像普通成员），意图不会。
		 */
		if (!is_attach && (mi->local_capabilities & MIDR_CAP_BOOTSTRAP)) {
			struct midr_node_entry rkey = {};
			struct midr_node_entry *rentry;

			rkey.node_id.family = AF_INET;
			rkey.node_id.prefixlen = IPV4_MAX_BITLEN;
			rkey.node_id.u.prefix4 = msg.requester_rid;
			rentry = midr_node_hash_find(&mi->global_view->nodes,
						     &rkey);
			if (rentry &&
			    !(rentry->capabilities &
			      (MIDR_CAP_BOOTSTRAP | MIDR_CAP_GROUP_REP))) {
				MIDR_LOG("MIDR ctrl: 拒绝 PEER_REQUEST —— 本机是引导节点，而发起方 %pI4 在节点表里是普通成员（无 BOOTSTRAP/GROUP_REP 位）",
					 &msg.requester_rid);
				midr_ctrl_send_peer_reject(
					bgp, msg.requester_transport);
				return;
			}
		}

		/* ④ 其余一律回配。
		 * The message is self-describing — build a transient entry and
		 * peer back; no dependency on the BGP-LS node table. */
		req.node_id.family = AF_INET;
		req.node_id.prefixlen = IPV4_MAX_BITLEN;
		req.node_id.u.prefix4 = msg.requester_rid;
		req.asn = ntohl(msg.requester_asn);
		req.group_id = target_group;
		req.transport_addr = msg.requester_transport;
		req.has_transport_addr = true;

		MIDR_FLOW_LOG("MIDR ctrl: %s from rid %pI4 transport %pI4 AS %u group %u — peering back",
			  midr_ctrl_msg_type_str(buf[1]), &msg.requester_rid,
			  &msg.requester_transport, (unsigned int)req.asn,
			  target_group);

		/*
		 * 台账原因按"我这边为什么也需要这条边"记，不按"谁先动手"记。
		 * target_group 是发起方自己的群号（见 midr_ctrl_send_peer_request）：
		 *   == 我的群号 → 同群互联，这条边我方同样有需求，我退群时该拆；
		 *      记成 PEER_REQ_REPLY 会让"只拆 SAME_GROUP"的清理判据漏掉它。
		 *   != 我的群号 → 才是真回声（需求 owner 在对端，我不该主动拆）。
		 * 群号白名单删掉后（08-11 批 2）本判断真正开始分岔，同一个比较就此
		 * 从"拒绝判据"降级为"记账判据"。
		 * 加 `target_group != 0` 一条：0 = 未分群，两个未分群的节点不算
		 * "同群"（本机是引导时 local_group_id 恒 0，不加这条会把每一条挂靠
		 * 边都记成 SAME_GROUP）。
		 */
		reason = (target_group != 0 &&
			  target_group == mi->local_group_id)
				 ? MIDR_SESSION_SAME_GROUP
				 : MIDR_SESSION_PEER_REQ_REPLY;

		/* send_nudge=false：我是被请求方，再发一次 PEER_REQUEST 就是回声。 */
		midr_ctrl_connect(bgp, &req, reason, false);

		/* SAME_GROUP 时 connect 内已把对端纳入本群邻居（置位 + 起探），
		 * 视图变了要告知 CL；本路一次一个节点，就地发一条。 */
		if (reason == MIDR_SESSION_SAME_GROUP)
			midr_nds_notify_cl(bgp, MIDR_TRIGGER_NODE_CHANGE);
		break;
	}
	case MIDR_CTRL_PEER_REJECT: {
		uint8_t req_type;

		/*
		 * B2：对端明确拒绝了我的建连请求（答疑 93）。
		 *
		 * **防伪判据**：5859 通道尚无鉴权，谁都能伪造一包 REJECT 让我拆
		 * 会话。所以只认"我确实正朝这个地址发着建连请求"的——去
		 * ctrl_pending 里按 (建连类, 发送者 transport) 查，查不到
		 * **直接丢**。伪造者得先知道我此刻正朝谁建连，窗口极小；何况伪造
		 * PEER_REQUEST 的危害本来就更大，不因本特性变差。
		 * 判据用帧里自报的 transport 而非 UDP 源地址：overlay 是多跳的，
		 * 源地址取决于 underlay 选路，未必等于对端 transport（也就未必等于
		 * pending 的键）。
		 *
		 * 处理 = **提前死心**，与 3s×5 超时死心完全同款语义（结论 23）：
		 * 停重传 + 拆自己预配的半边 + warn 到此为止。区别只是快——秒级，
		 * 而不是盲等 15s。
		 * 换不换一台试的口径也与超时死心一致（见 retx_timer 里那段）：
		 * PEER_REQUEST 不换（指名连这一个，无替补池）；ATTACH_REQUEST 换
		 * （D2 failover：盖 attach_failed 章 + 立即重挑下一台引导）。
		 */
		if (n < (ssize_t)sizeof(msg))
			return;
		memcpy(&msg, buf, sizeof(msg));

		/* 在途的可能是 PEER_REQUEST 也可能是 ATTACH_REQUEST（挂靠被拒），
		 * 查出是哪一种：既作防伪判据，也用来清对应那条 pending。 */
		req_type = midr_ctrl_pending_peer_req_type(
			mi, msg.requester_transport);
		if (!req_type) {
			MIDR_LOG("MIDR ctrl: 丢弃 PEER_REJECT —— 本机并未朝 %pI4 发建连请求（防伪判据）",
				 &msg.requester_transport);
			return;
		}

		zlog_info("MIDR ctrl: %pI4 拒绝了本机的 %s（rid %pI4），提前死心：停止重传并拆除半边会话（拒绝原因见对端日志）",
			  &msg.requester_transport,
			  midr_ctrl_msg_type_str(req_type), &msg.requester_rid);
		midr_ctrl_drop_pending(mi, msg.requester_transport, req_type);
		midr_ctrl_drop_half_peer(bgp, msg.requester_transport,
					 "对端明确回了 PEER_REJECT（提前死心）");
		/* 挂靠被拒 → 换一台（D2）。此处不在遍历 ctrl_pending，盖章后
		 * 直接重挑是安全的。 */
		if (req_type == MIDR_CTRL_ATTACH_REQUEST) {
			enum midr_attach_batch batch;

			/* 重挑从死者所在批次继续（批 6 的 A-3）。 */
			batch = midr_nds_attach_mark_failed(
				bgp, msg.requester_transport);
			midr_nds_attach_pick_from(bgp, batch);
		}
		break;
	}
	case MIDR_CTRL_REP_LIST_REQ:
	case MIDR_CTRL_REP_LIST_RESP:
	case MIDR_CTRL_MEMBER_LIST_REQ:
	case MIDR_CTRL_MEMBER_LIST_RESP:
		/* 列表交换已迁 TCP; 收到 UDP 列表报文多半来自旧版本节点 (混跑不
		 * 兼容, 既定决策)。留此提示便于将来误用混版本时定位。 */
		MIDR_LOG("MIDR ctrl: 忽略 UDP 列表报文 type=%s —— 列表交换已迁 TCP (旧版本节点?)",
			 midr_ctrl_msg_type_str(buf[1]));
		break;
	case MIDR_CTRL_ANNOUNCE:
		/*
		 * 一个候选群成员在被灌入某加入节点的探测列表前，双方从未交换过
		 * 任何报文——PM 的来源校验（pm_is_known_transport）会把加入节点
		 * 的探测包当作未知来源丢弃。这里只是记住发送者身份，不回复、不
		 * 建连（保持"探成员阶段只探不连"）。
		 */
		if (n < (ssize_t)sizeof(msg))
			return;
		memcpy(&msg, buf, sizeof(msg));
		midr_nds_learn_requester(bgp, msg.requester_rid,
					 (as_t)ntohl(msg.requester_asn),
					 msg.requester_transport);
		MIDR_FLOW_LOG("MIDR ctrl: ANNOUNCE from %pI4 — noted for PM source validation",
			  &msg.requester_transport);
		break;
	default:
		break;
	}
}

/*
 * Bind to local_transport_addr only — NOT INADDR_ANY — same reasoning as the
 * PM probe socket (bgp_midr_pm.c): limits the attack surface to the MIDR
 * loopback interface, and just as importantly for correctness, controls
 * which *source* address our own outgoing sendto()s carry. An INADDR_ANY
 * bind leaves that choice to the kernel's route-to-destination lookup,
 * which for a multi-hop overlay path can pick the sender's point-to-point
 * link address instead of its advertised transport identity — a source the
 * transit hops beyond the first one have no route for, so a reverse-path
 * check (or equivalent) silently drops the packet a hop or two downstream.
 * This is exactly what happened to MIDR_CTRL_ANNOUNCE from a cross-transit
 * node in the backbone-topology test (see CLAUDE.md's "a single lost
 * ANNOUNCE packet" writeup): PM's packets (correctly sourced) arrived, but
 * every ctrl-channel packet from the same sender to the same destination
 * never did.
 *
 * transport_addr_set is false when midr_ctrl_init() runs (it fires before
 * the config file is read, same lifecycle constraint PM has), so the actual
 * open is deferred to midr_ctrl_on_transport_addr_set(), called from the
 * `midr transport-address` VTY handler once an address exists to bind to.
 */
static void midr_ctrl_open_udp_sock(struct bgp *bgp)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct sockaddr_in sa = {};
	int sock;

	if (!mi->transport_addr_set) {
		zlog_warn("MIDR ctrl: local transport-address not configured; "
			  "UDP channel deferred until `midr transport-address` is set");
		return;
	}

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		zlog_err("MIDR ctrl: UDP socket() failed: %s",
			 safe_strerror(errno));
		return;
	}
	sockopt_reuseaddr(sock);

	sa.sin_family = AF_INET;
	sa.sin_addr = mi->local_transport_addr;
	sa.sin_port = htons(MIDR_CTRL_UDP_PORT);
	if (bind(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		zlog_err("MIDR ctrl: UDP bind(%pI4:%u) failed: %s",
			 &mi->local_transport_addr, MIDR_CTRL_UDP_PORT,
			 safe_strerror(errno));
		close(sock);
		return;
	}
	set_nonblocking(sock);
	mi->ctrl_sock = sock;
	event_add_read(bm->master, midr_ctrl_udp_recv, bgp, sock,
		       &mi->t_ctrl_read);

	MIDR_LOG("MIDR ctrl: peer-request UDP channel ready on %pI4:%u",
		  &mi->local_transport_addr, MIDR_CTRL_UDP_PORT);
}

/*
 * Called by the VTY `midr transport-address` handler after the address is
 * set — mirrors midr_pm_on_transport_addr_set() (bgp_midr_pm.c). No rescan
 * needed here the way PM's counterpart does one: nothing enqueues a
 * ctrl_pending request before an address exists (midr_ctrl_enqueue_request()
 * itself checks transport_addr_set and warns+bails), so there's no backlog
 * of silently-dropped sends to retrofit — just the socket to open.
 */
void midr_ctrl_on_transport_addr_set(struct bgp *bgp)
{
	struct bgp_midr_nds *mi;

	if (!bgp || !bgp->midr_nds_info)
		return;
	mi = bgp->midr_nds_info;

	if (mi->ctrl_sock < 0)
		midr_ctrl_open_udp_sock(bgp);
}

void midr_ctrl_init(struct bgp *bgp)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;

	if (!mi)
		return;

	mi->ctrl_sock = -1;
	mi->ctrl_pending = list_new();

	/* 开 TCP 列表交换通道 (监听失败记 err)。先于 UDP 建立, 使二者互不依赖
	 * ——UDP bind 失败的早返回不应连带跳过 TCP。 */
	midr_ctrl_tcp_init(bgp);

	midr_ctrl_open_udp_sock(bgp);
}

void midr_ctrl_finish(struct bgp *bgp)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct listnode *node, *nnode;
	struct midr_ctrl_pending *p;

	if (!mi)
		return;

	/* 关 TCP 列表交换通道 (监听 + 所有活动连接)。 */
	midr_ctrl_tcp_finish(bgp);

	event_cancel(&mi->t_ctrl_read);
	event_cancel(&mi->t_ctrl_retx);
	if (mi->ctrl_sock >= 0) {
		close(mi->ctrl_sock);
		mi->ctrl_sock = -1;
	}
	if (mi->ctrl_pending) {
		for (ALL_LIST_ELEMENTS(mi->ctrl_pending, node, nnode, p))
			XFREE(MTYPE_MIDR_CTRL_PENDING, p);
		list_delete(&mi->ctrl_pending);
	}
}

/* ------------------------------------------------------------------ */
/* Peer lifecycle                                                       */
/* ------------------------------------------------------------------ */

/*
 * 把一个刚由 peer_remote_as() 建出的 peer 整形成 MIDR overlay 会话。
 * 自动建连（midr_ctrl_connect）与手动命令（midr neighbor）共用，保证
 * "MIDR 会话长什么样"只有一个定义：
 *
 *   - multihop：MIDR 对等体多为非直连（transport 对 transport）；
 *   - update-source：TCP 源地址绑本端 transport（loopback）。不绑则内核
 *     用出口网卡地址做源，对端按源地址查邻居表对不上号，多跳会话卡 Active；
 *   - 激活 BGP-LS：拓扑信息在 MIDR 对等体间直接流动，不必绕中继；
 *   - 撤销 IPv4 单播：FRR 建 peer 时按 bgp->default_af 自动附送（bgpd.c
 *     peer_create，在 peer_remote_as() 内部就已完成、无从阻止，只能建完
 *     再撤）。不撤的后果：overlay 多跳会话把远端路由以更短 AS path 注入
 *     underlay——本端向邻居通告"其实必须借道该邻居才走得通"的路径，形成
 *     控制面无环、转发面成环的递归下一跳环路（07-21 十节点实测 g1b/g1c
 *     互指、g2c 探测全丢；见 docs/decisions/midr-overlay-underlay-layering.md）。
 *     MIDR 会话只承载 BGP-LS——谁提供转发可达性，谁才开 IPv4 单播。
 *
 * 此时会话尚未 Established，peer_deactivate 只清配置位、不触发 reset。
 */
void midr_nds_ctrl_setup_overlay_peer(struct bgp *bgp, struct peer *peer)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;

	/*
	 * Stamp ownership FIRST: this is the single point both the auto-connect
	 * path and the `midr neighbor` command pass through, so the flag
	 * uniquely marks peers MIDR created.  The session-ownership guards
	 * (midr_nds_peer_is_overlay) read it to avoid touching operator sessions.
	 */
	SET_FLAG(peer->flags, PEER_FLAG_MIDR_OVERLAY);

	peer_ebgp_multihop_set(peer, MAXTTL);
	if (mi && mi->transport_addr_set) {
		union sockunion local_su;

		midr_su_from_in_addr(&local_su, mi->local_transport_addr);
		peer_update_source_addr_set(peer, &local_su);
	}
	/* MIDR overlay 会话只载 (4,9) MIDR-LS —— 件②（轮 4）退役了我方自有的
	 * (4,8) BGP-LS 那一族，拓扑情报全部由第二组编码传播。 */
	peer_activate(peer, AFI_BGP_LS, SAFI_MIDR_LS);
	peer_deactivate(peer, AFI_IP, SAFI_UNICAST);
}

/*
 * 会话归属判据（⑦，四处守卫共用）：这条 peer 是不是 MIDR 自己建的 overlay 会话？
 *
 * 判据只有一条 = 带 PEER_FLAG_MIDR_OVERLAY 标记（出身）。标记只由上面的整形
 * helper 盖，而运维经 peer_remote_as 建的原生会话从来没有它——挡住运维会话靠的
 * 就是这一条。运维用 `midr session` 手配的 MIDR 会话有标记，归台账 MANUAL 豁免
 * （α）管，也轮不到别的判据。
 *
 * 〔批 5c（08-15）删掉了原来的第二条"长相（签名）：除 BGP-LS 外无激活地址族"。
 * 签名判据的前提是"MIDR 会复用运维的原生会话"——复用了别人的会话，"这条到底是
 * 谁的"才含糊，才需要看长相；合并第二组、LS 迁族后一律自建不复用，前提消失，
 * 那时签名不但没用，还会因地址族变了而认不出自己人。留着它反倒制造真问题：
 * 运维给一条 MIDR 会话开了 IPv4，这条边就再也拆不掉（no midr session 拒删、
 * try_disconnect 拒拆），洞只是被挪了个位置。全树其余归属判据（8212 豁免
 * bgp_route.c、peer 状态钩子）本来就只认标记，删签名是把 2:1 分裂统一掉。
 * 全案见 docs/群间保底连接/轮2/批次记录/5c-三处定案讨论.md 第二处。〕
 */
bool midr_nds_peer_is_overlay(struct peer *peer)
{
	if (!peer)
		return false;

	return CHECK_FLAG(peer->flags, PEER_FLAG_MIDR_OVERLAY);
}

void midr_ctrl_connect(struct bgp *bgp, const struct midr_node_entry *entry,
		       enum midr_session_reason reason, bool send_nudge)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	union sockunion su;
	struct prefix locator;
	struct peer *peer;
	struct in_addr remote_rid = { .s_addr = INADDR_ANY };
	as_t asn = entry->asn;
	int ret;

	/* 轮 4 放宽：不再要求 asn 非 0。建连走 AS_EXTERNAL，只校验"对端 AS ≠ 本机
	 * AS"、随后用对端 OPEN 的真实 AS 覆盖（bgp_packet.c:2037），传什么都不参与
	 * 校验；而第二组的 membership 对象不含 ASN，换源后这道守卫会永远挡住建连。
	 * 真必需的是 transport（建连目标地址）。 */

	/*
	 * 件②（轮 4）收紧：没有 transport 就**不建连**，不再回落 router-id。
	 *
	 * MIDR 节点不配 `midr transport-address` 本身就是运维事故，正确反应是把事故
	 * 亮出来，而不是静默拿 router-id 顶上去建一条大概率连不通的会话（router-id
	 * 只是身份，未必可路由——真分离基线下它通常压根不在转发面里）。
	 * 展示类调用点（show 命令）仍走 midr_node_get_locator 的回落：显示 router-id
	 * 比显示空白有用。
	 */
	if (!entry->has_transport_addr) {
		zlog_warn("MIDR ctrl: %pFX 没有 transport 地址，不建连——该节点多半漏配了 midr transport-address（router-id 只是身份、未必可路由，不作回落）",
			  &entry->node_id);
		return;
	}

	/* Peer with the node's real reachable address (TLV 1188), not its
	 * router-id; router-id is only an identity and may be unroutable. */
	midr_node_get_locator(entry, &locator);

	/*
	 * 自连闸：谁也不许跟本机建会话（判据 = rid 撞本机 router-id 或目标撞本机
	 * transport）。四条路里只有 connect_group 天然挡得住（过 should_peer）；
	 * 回配的 rid 是对端帧自报的，挂靠候选池只拒 rid=0 不拒本机
	 * （`midr bootstrap <本机地址>` 即可造出）。不拦则发给本机 5859 的请求被
	 * 自己收下、再走回配，自己给自己记账、自己探自己。
	 */
	if ((entry->node_id.family == AF_INET &&
	     IPV4_ADDR_SAME(&entry->node_id.u.prefix4, &bgp->router_id)) ||
	    (mi && mi->transport_addr_set && locator.family == AF_INET &&
	     IPV4_ADDR_SAME(&locator.u.prefix4, &mi->local_transport_addr))) {
		zlog_warn("MIDR ctrl: 拒绝与本机自身建会话（%pFX）——请检查 bootstrap / session 配置是否把本机填成了对端",
			  &entry->node_id);
		return;
	}

	/*
	 * 运维 `no midr session` 持久排除的节点，任何自动路径（发现建邻居、
	 * connect_group 换组/退群重收敛、CL 的 ANCHOR、挂靠）都不得在这里悄悄
	 * 把会话建回来。跨群那几条路本就绕开 midr_discovery_should_peer（定稿
	 * 结论 15：它是成员判断不是闸门），所以本处是唯一能覆盖全部调用路径的
	 * 地方，两道一个都不能丢。`midr session`（手工 escape hatch）不走本函数，
	 * 不受影响。键 = router-id（08-11 批 2 由地址换来）。
	 */
	if (entry->node_id.family == AF_INET &&
	    midr_nds_is_session_excluded(bgp, entry->node_id.u.prefix4)) {
		MIDR_LOG("MIDR ctrl: %pFX 在会话排除名单中，跳过自动建连",
			 &entry->node_id);
		return;
	}

	prefix2sockunion(&locator, &su);

	/*
	 * 台账家规①：登记放在下面两道去重检查【之前】。"MIDR 需要这条边"和
	 * "要不要新建会话"是两回事——会话可能早就存在（手工建过、复用了原生
	 * 会话），那时不必再建，但这笔账必须记上，否则将来凭账认边时它无名无姓，
	 * 会被清理流程当无主边误拆。排除名单那道拦在前面是对的：被排除 = 这条边
	 * 根本不该有，记账无意义。
	 * 家规③（复用原生会话的边也记账、⑦ 拒拆时账照销）由本处登记 + 拆口
	 * 无条件销账共同实现，账里不再存 reused 标记（08-10 定案：复用会话根本
	 * 不打 OVERLAY 标、⑦ 认人不靠账，该字段功能性为零）。
	 */
	if (entry->node_id.family == AF_INET)
		remote_rid = entry->node_id.u.prefix4;
	if (locator.family == AF_INET)
		midr_nds_ledger_note(bgp, locator.u.prefix4, reason, remote_rid,
				     entry->group_id);

	/*
	 * SAME_GROUP 状态动作：这条边因同群而建，就在此把对端纳入本群邻居——回配与
	 * connect_group 共用（三件套原先手写在后者）。
	 * ⚠ 必须在两道去重之前：地址上已有会话时同样要置位起探，否则老成员拿不到
	 * link_entry，periodic_sync 会误判 LEAVE 散群。
	 * notify CL 由调用方按"一批边建完"的粒度发。
	 */
	if (reason == MIDR_SESSION_SAME_GROUP) {
		struct in_addr transport = { .s_addr = INADDR_ANY };

		if (entry->has_transport_addr)
			transport = entry->transport_addr;
		midr_nds_adopt_group_peer(bgp, remote_rid, asn, transport,
					  entry->group_id);
		midr_mark_topology(bgp, entry);
	}

	/*
	 * 反向建连 nudge：请对端也建一条回来。由函数末尾提到去重之前——去重 return
	 * 掉的情况（本端已有会话）对端未必也有。原先靠去重顺手吞掉 nudge 来终止
	 * A↔B 回声，现改为显式刹车（回配传 false）。
	 */
	if (send_nudge)
		midr_ctrl_send_peer_request(bgp, entry, reason);

	/*
	 * S5 第一道去重（⑦）：transport 地址上已有会话。地址被占 = 无法另建（BGP
	 * 一地址一会话）——终止 A<->B notify 握手也靠这条 return（收到回响
	 * PEER_REQUEST 的一端在此发现已建的 peer 便停发）。按归属分两类：
	 *   - MIDR 自己的：正常复用（重复 connect 很常见），沉默；
	 *   - 运维的：拓扑无通道可走、这条边建不起来，warn 出来——但绝不整形运维
	 *     会话（那是洞 #3、违反运维优先）。
	 *
	 * 〔件②（轮 4）删去原来夹在中间的"运维会话已激活 LS 就借它当 LS 通道"那
	 * 一支：迁 (4,9) MIDR-LS 后一律自建不复用（真分离约定），该分支机械改族
	 * 也只是恒假死代码。运维会话无论载不载 LS，处置都是同一句 warn。〕
	 */
	{
		struct peer *occupant = peer_lookup(bgp, &su);

		if (occupant) {
			if (midr_nds_peer_is_overlay(occupant)) {
				/* MIDR 自己的会话，正常复用（沉默）。 */
			} else {
				zlog_warn("MIDR ctrl: %pFX 的 transport 地址上有一条运维会话，MIDR 不整形运维配置、这条边建不起来；请检查该静态邻居是否误用了 transport（loopback）地址——静态会话只应配链路地址",
					  &entry->node_id);
			}
			return;
		}
	}

	/*
	 * 〔发现链专题删去原 S5 第二道去重（按 router-id 复用现成 LS 会话）：它的
	 * 前提是静态会话可能载 LS，真分离基线上该判据永不成立。约定见
	 * containerlab/部署手册.md。〕
	 */

	ret = peer_remote_as(bgp, &su, NULL, &asn, AS_EXTERNAL, NULL);
	if (ret != 0) {
		zlog_warn("MIDR ctrl: peer_remote_as(%pFX AS %u) failed: %d",
			  &entry->node_id, asn, ret);
		return;
	}

	MIDR_FLOW_LOG("MIDR ctrl: peering initiated with %pFX AS %u",
		  &entry->node_id, asn);

	/* 整形成 overlay 会话（multihop + update-source + 只载 BGP-LS），
	 * 定义与理由见 midr_nds_ctrl_setup_overlay_peer 头注释。 */
	peer = peer_lookup(bgp, &su);
	if (peer)
		midr_nds_ctrl_setup_overlay_peer(bgp, peer);
}

static void midr_try_disconnect(struct bgp *bgp,
				const struct midr_node_entry *entry,
				bool force)
{
	union sockunion su;
	struct prefix locator;

	midr_node_get_locator(entry, &locator);
	prefix2sockunion(&locator, &su);

	/*
	 * α（08-12 拍板，批 4 提前落地）：**自动清理一律不碰 MANUAL 账下的边**。
	 *
	 * 运维用 `midr session` 手配的边只有运维能拆（`no midr session`）。不加这道
	 * 守卫时的实测事故：对端发过一次 join 请求（TCP 请求处理里 learn_requester
	 * 顺手把它学进节点表）→ 请求停了 15s 条目过期 → 走到这里把手配的骨干边
	 * 一并删掉，而 ⑦ 归属守卫**放行**（手配边同样带 OVERLAY 标记，判据全中），
	 * 台账 MANUAL 粘性只护"原因字段"、护不住会话本体。后果 =
	 * 骨干网无声地缺一条边，且不自愈（本机进程没重启、conf 不会重灌）。
	 *
	 * 位置在**销账之前**：账与会话都不动——账在，`show midr neighbors` 才还能
	 * 显示这条边的来历；边在，运维配的东西才没被自动流程动过。
	 *
	 * 与 B1 断连老化（批 5）的 MANUAL 豁免是**同一条规矩**，两处生效。
	 * 放在 try_disconnect 而不是 expire 定时器里，是因为本函数是拆会话的唯一
	 * 出口：上游挂着 expire / withdraw / 换组多个调用点，轮 4/5 删掉 expire、
	 * 换成第二组的 withdraw 回调后，这道守卫照旧生效。
	 *
	 * warn 级、不带 debug 门控：本案的"静默拆边"正是两处日志都 debug 级害的。
	 *
	 * force 旁路（退网专题，2026-08-21）：α 防的是**自动侧**越权拆运维的账，
	 * 而本端 `midr shutdown` 是运维显式敲的命令、与手配同级——退网要把本机的
	 * MIDR 会话拆净（留一条还在收发 LS 的会话，对上层的影响说不清），故那一条
	 * 路径传 force=true 走旁路。**除退网外一律传 false**：expire / 对端 withdraw /
	 * 换组 / 挂靠卸任 / B1 老化都是自动侧，豁免照旧。对端那半边收到的是
	 * withdraw、走的正是自动路径，所以仍被 α 拦下（只 warn 不拆，判据 1）。
	 */
	if (locator.family == AF_INET && !force) {
		const struct midr_session_ledger_entry *led =
			midr_nds_ledger_lookup(bgp, locator.u.prefix4);

		if (led && led->reason == MIDR_SESSION_MANUAL) {
			zlog_warn("MIDR ctrl: %pFX（%pI4）的会话是运维手配（台账 MANUAL），跳过自动拆除——要拆请用 no midr session",
				  &entry->node_id, &locator.u.prefix4);
			return;
		}
	}

	/*
	 * 拆口销账（结论 20）：走到这里 = MIDR 判定"这条边我不需要了"，需求没了
	 * 账就销——放在最前面，三条退出路径（无 peer / ⑦ 拒拆 / 真拆）都覆盖到。
	 * ⑦ 拒拆那条尤其要销：守卫护的是运维会话本体，台账记的是我方需求，
	 * 两件事（答疑 14）。账留着会让后续凭账认边把运维会话认成 MIDR 的边。
	 */
	if (locator.family == AF_INET)
		midr_nds_ledger_drop(bgp, locator.u.prefix4);

	struct peer *peer = peer_lookup(bgp, &su);
	if (!peer)
		return;

	/*
	 * S4 归属守卫（⑦）：只拆 MIDR 自建的 overlay 会话。键空间隔离（静态邻居按
	 * 链路地址注册、这里按 locator 查）在 lab 下成立，但运维用 loopback（= 本
	 * 节点 locator 键）配静态邻居是 iBGP 标准实践、完全合法，此时 peer_lookup
	 * 会命中它——无守卫则误删运维会话、砸转发面。判据 = OVERLAY 标记。
	 * 旧 PEER_FLAG_CONFIG_NODE 守卫不可用：peer_remote_as 对谁都置它、区分不了。
	 */
	if (!midr_nds_peer_is_overlay(peer)) {
		MIDR_LOG("MIDR ctrl: %pFX locator 命中运维会话，拒拆（保护 underlay）",
			 &entry->node_id);
		return;
	}

	MIDR_LOG("MIDR ctrl: removing peer %pFX (node gone)", &entry->node_id);
	peer_delete(peer);
}

/* ------------------------------------------------------------------ */
/* Public interface (called from bgp_midr_nds.c / NDS)                     */
/* ------------------------------------------------------------------ */

void midr_ctrl_on_node_remove(struct bgp *bgp, struct midr_node_entry *entry)
{
	/* 自动路径（expire / 对端 withdraw / 换组清理）：α 豁免照旧生效。 */
	midr_try_disconnect(bgp, entry, false);
}

/*
 * 按 transport 拆一条 MIDR 边（批 5 挂靠钩子用）。
 *
 * 为什么需要这个入口：拆边的既有出口 midr_try_disconnect() 收的是**节点表条目**，
 * 而挂靠的对端是引导节点——它不发 Node NLRI，节点表里永远没有它，拿不到条目。
 * 这里按 connect 侧同款做法拼一个临时条目（信息自带、不查表），其余一概复用
 * try_disconnect：MANUAL 豁免（α）、销账、⑦ 归属守卫，一个都不绕过。
 *
 * rid 用于日志可读（"拆的是谁"）；给 0 也能工作（locator 由 transport 决定）。
 *
 * force 见 midr_try_disconnect 内的 α 说明：只有本端退网（运维显式命令）传 true，
 * 自动路径一律 false。
 */
void midr_ctrl_detach_transport(struct bgp *bgp, struct in_addr transport,
				struct in_addr rid, bool force,
				enum midr_stop_reason reason)
{
	struct midr_node_entry e = {};

	if (!bgp || !bgp->midr_nds_info || transport.s_addr == INADDR_ANY)
		return;

	e.node_id.family = AF_INET;
	e.node_id.prefixlen = IPV4_MAX_BITLEN;
	e.node_id.u.prefix4 = rid.s_addr != INADDR_ANY ? rid : transport;
	e.transport_addr = transport;
	e.has_transport_addr = true;

	midr_try_disconnect(bgp, &e, force);

	/* 拆完会话还要收拾节点表那半边（停探 + 撤链路上报 + 清 link_entry +
	 * 清 is_adjacent）——临时条目碰不到表里的真条目，不补就留下"标着邻居、
	 * 边却没了"的幻影。对端是引导时反查落空、天然 no-op。 */
	midr_nds_cleanup_by_transport(bgp, transport, reason);
}

void midr_mark_topology(struct bgp *bgp, const struct midr_node_entry *entry)
{
	(void)bgp;
	/* Stub: record a qualifying connection into the topology graph.
	 * Real topology-graph bookkeeping lands in a later phase. */
	MIDR_LOG("MIDR ctrl: mark topology connection to %pFX (group %u) (stub)",
		   &entry->node_id, entry->group_id);
}

int midr_ctrl_connect_group(struct bgp *bgp, uint32_t group_id,
			    enum midr_session_reason reason)
{
	struct midr_node_entry *entry;
	int count = 0;

	if (!bgp || !bgp->midr_nds_info)
		return 0;

	frr_each (midr_node_hash, &bgp->midr_nds_info->global_view->nodes, entry) {
		/*
		 * 逐成员过 midr_discovery_should_peer（定稿结论 15 点名的终态调用
		 * 形状）。它判的正是"同群 ∧ 非 self ∧ 排除 group-0 ∧ 未排除"，与
		 * 这里原先的裸筛（非 self + 同群）相比只多了排除名单一道——效果与
		 * connect 内那道等价、双保险仍在，真正的好处是**统一入口**：将来
		 * should_peer 加任何条件，换组/JOIN 建连这条路不会被漏掉。
		 *
		 * 注意它比的是"本机群号"而非入参 group_id：唯一调用方
		 * midr_group_reconverge（手动换组与 I-7 JOIN 都汇到它）在第 1 步
		 * midr_originate_group_update 里已把 local_group_id 置成 new_gid，
		 * 到第 2 步调本函数时两者恒等，故等价。
		 */
		if (!midr_discovery_should_peer(bgp, entry))
			continue;
		/*
		 * 三件套（is_adjacent + I-1 + mark_topology）已收编进
		 * midr_ctrl_connect() 的 SAME_GROUP 分支——回配路要做同样的事。
		 * ⚠ 故本函数的 reason 必须是 SAME_GROUP，换别的值三件套就不生效。
		 */
		midr_ctrl_connect(bgp, entry, reason, true);
		count++;
	}

	return count;
}
