// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR Control Layer —— 语义层
 *
 * Evaluates peering policy from MIDR node table events and drives the BGP
 * peer FSM (peer_remote_as / peer_delete) accordingly.
 *
 * 控制通道双传输 (端口 5859, 语义在此、TCP 传输在 bgp_midr_ctrl_tcp.c):
 *  - UDP: 固定长控制消息（PEER_REQUEST / ATTACH_REQUEST / PEER_REJECT /
 *    ANNOUNCE）——负责反向建连、拒绝和请求方身份预告（独立于 PM 通道）。
 *  - TCP 短连接: REP_LIST / MEMBER_LIST / BOOTSTRAP_LIST 列表交换 —— 本文件负责消息构造 / 资格
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
#include "iana_afi.h"

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

/* struct midr_ctrl_pending 定义已移至 bgp_midr_ctrl.h（show midr join 要展示
 * 未应答请求），MTYPE 仍留在本文件。 */

static void midr_ctrl_send_req(struct bgp_midr_nds *mi, struct ipaddr dst,
			       uint8_t type, uint32_t target_group);
static void midr_ctrl_enqueue_request(struct bgp *bgp, struct ipaddr dst,
				      uint8_t type, uint32_t target_group,
				      const struct midr_ctrl_retx_params *rp);
static void midr_ctrl_send_peer_request(struct bgp *bgp,
					const struct midr_node_entry *entry,
					enum midr_session_reason reason);
static void midr_ctrl_drop_pending(struct bgp_midr_nds *mi, struct ipaddr dst,
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

/* 到该 transport 的 BGP 会话是否已 Established（判活统一走会话状态：子稿 §1
 * 前提二"会话 Down 即死"，不引用 legacy last_seen/expire）。 */
static bool midr_ctrl_transport_session_up(struct bgp *bgp,
					   struct ipaddr transport)
{
	union sockunion su;
	struct peer *peer;

	if (!midr_ipaddr_to_sockunion(&transport, &su))
		return false;
	peer = peer_lookup(bgp, &su);

	return peer && peer->connection &&
	       peer->connection->status == Established;
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
	case MIDR_CTRL_ANNOUNCE:
		return "ANNOUNCE";
	case MIDR_CTRL_PEER_REJECT:
		return "PEER_REJECT";
	case MIDR_CTRL_BOOTSTRAP_LIST_REQ:
		return "BOOTSTRAP_LIST_REQ";
	case MIDR_CTRL_BOOTSTRAP_LIST_RESP:
		return "BOOTSTRAP_LIST_RESP";
	case MIDR_CTRL_ATTACH_REQUEST:
		return "ATTACH_REQUEST";
	default:
		return "UNKNOWN";
	}
}

/*
 * "建连类"请求 = PEER_REQUEST 与 ATTACH_REQUEST（批 5 前置①）：同一个 36B 帧、
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

static bool midr_ctrl_is_udp_type(uint8_t type)
{
	return midr_ctrl_is_peer_req_like(type) || type == MIDR_CTRL_ANNOUNCE ||
	       type == MIDR_CTRL_PEER_REJECT;
}

static bool midr_ctrl_is_tcp_request_type(uint8_t type)
{
	return type == MIDR_CTRL_REP_LIST_REQ ||
	       type == MIDR_CTRL_MEMBER_LIST_REQ ||
	       type == MIDR_CTRL_BOOTSTRAP_LIST_REQ;
}

static bool midr_ctrl_is_request_type(uint8_t type)
{
	return midr_ctrl_is_udp_type(type) ||
	       midr_ctrl_is_tcp_request_type(type);
}

static void midr_ctrl_put_u16(uint8_t *p, uint16_t value)
{
	value = htons(value);
	memcpy(p, &value, sizeof(value));
}

static void midr_ctrl_put_u32(uint8_t *p, uint32_t value)
{
	value = htonl(value);
	memcpy(p, &value, sizeof(value));
}

static uint16_t midr_ctrl_get_u16(const uint8_t *p)
{
	uint16_t value;

	memcpy(&value, p, sizeof(value));
	return ntohs(value);
}

static uint32_t midr_ctrl_get_u32(const uint8_t *p)
{
	uint32_t value;

	memcpy(&value, p, sizeof(value));
	return ntohl(value);
}

/* Version-4 locator: IANA AFI (2), reserved (2), fixed 16-byte address area. */
static bool midr_ctrl_encode_locator(uint8_t *wire,
				     const struct ipaddr *locator)
{
	if (!wire || !midr_ipaddr_valid_locator(locator))
		return false;

	memset(wire, 0, MIDR_CTRL_LOCATOR_LEN);
	if (IS_IPADDR_V4(locator)) {
		midr_ctrl_put_u16(wire, IANA_AFI_IPV4);
		memcpy(wire + 4, &locator->ipaddr_v4,
		       sizeof(locator->ipaddr_v4));
		return true;
	}
	if (IS_IPADDR_V6(locator)) {
		midr_ctrl_put_u16(wire, IANA_AFI_IPV6);
		memcpy(wire + 4, &locator->ipaddr_v6,
		       sizeof(locator->ipaddr_v6));
		return true;
	}
	return false;
}

static bool midr_ctrl_decode_locator(const uint8_t *wire,
				     struct ipaddr *locator)
{
	uint16_t afi;
	static const uint8_t zero_tail[12];

	if (!wire || !locator)
		return false;
	SET_IPADDR_NONE(locator);
	if (midr_ctrl_get_u16(wire + 2) != 0)
		return false;

	afi = midr_ctrl_get_u16(wire);
	if (afi == IANA_AFI_IPV4) {
		if (memcmp(wire + 8, zero_tail, sizeof(zero_tail)) != 0)
			return false;
		locator->ipa_type = IPADDR_V4;
		memcpy(&locator->ipaddr_v4, wire + 4,
		       sizeof(locator->ipaddr_v4));
	} else if (afi == IANA_AFI_IPV6) {
		locator->ipa_type = IPADDR_V6;
		memcpy(&locator->ipaddr_v6, wire + 4,
		       sizeof(locator->ipaddr_v6));
	} else {
		return false;
	}

	if (!midr_ipaddr_valid_locator(locator)) {
		SET_IPADDR_NONE(locator);
		return false;
	}
	return true;
}

/* Encode each list item completely before touching the output stream.  This
 * keeps a failed locator conversion from leaving an uncounted partial item in
 * the frame. */
static bool midr_ctrl_stream_put_rep_item(struct stream *s, uint32_t group_id,
					  const struct ipaddr *locator,
					  as_t asn, struct in_addr rid)
{
	uint8_t wire[MIDR_CTRL_LOCATOR_LEN];

	if (!s || !group_id || rid.s_addr == INADDR_ANY ||
	    !midr_ctrl_encode_locator(wire, locator))
		return false;
	stream_putl(s, group_id);
	stream_put(s, wire, sizeof(wire));
	stream_putl(s, (uint32_t)asn);
	stream_put(s, &rid, sizeof(rid));
	return true;
}

static bool midr_ctrl_stream_put_member_item(struct stream *s,
					     struct in_addr rid,
					     const struct ipaddr *locator,
					     as_t asn, uint32_t group_id)
{
	uint8_t wire[MIDR_CTRL_LOCATOR_LEN];

	if (!s || !group_id || rid.s_addr == INADDR_ANY ||
	    !midr_ctrl_encode_locator(wire, locator))
		return false;
	stream_put(s, &rid, sizeof(rid));
	stream_put(s, wire, sizeof(wire));
	stream_putl(s, (uint32_t)asn);
	stream_putl(s, group_id);
	return true;
}

static bool midr_ctrl_stream_put_bootstrap_item(
	struct stream *s, const struct ipaddr *locator, as_t asn,
	struct in_addr rid)
{
	uint8_t wire[MIDR_CTRL_LOCATOR_LEN];

	if (!s || rid.s_addr == INADDR_ANY ||
	    !midr_ctrl_encode_locator(wire, locator))
		return false;
	stream_put(s, wire, sizeof(wire));
	stream_putl(s, (uint32_t)asn);
	stream_put(s, &rid, sizeof(rid));
	return true;
}

static bool midr_ctrl_same_active_family(struct bgp *bgp,
					 const struct ipaddr *locator)
{
	struct ipaddr local;
	struct prefix owner = {};

	if (!midr_nds_local_transport_get(bgp, &local) ||
	    !midr_ipaddr_valid_locator(locator) ||
	    ipaddr_family(&local) != ipaddr_family(locator))
		return false;
	owner.family = AF_INET;
	owner.prefixlen = IPV4_MAX_BITLEN;
	owner.u.prefix4 = midr_nds_rid_by_transport(bgp, *locator);
	return midr_nds_locator_unique(bgp, &owner, locator);
}

bool midr_ctrl_encode_request(struct bgp *bgp, uint8_t *payload, size_t len,
			      uint8_t type, uint32_t target_group)
{
	struct ipaddr local;

	if (!bgp || !payload || len != MIDR_CTRL_REQUEST_LEN ||
	    !midr_ctrl_is_request_type(type) ||
	    bgp->router_id.s_addr == INADDR_ANY ||
	    !midr_nds_local_transport_get(bgp, &local))
		return false;

	memset(payload, 0, len);
	payload[0] = MIDR_CTRL_MSG_VERSION;
	payload[1] = type;
	memcpy(payload + 4, &bgp->router_id, sizeof(bgp->router_id));
	if (!midr_ctrl_encode_locator(payload + 8, &local))
		return false;
	midr_ctrl_put_u32(payload + 28, (uint32_t)bgp->as);
	midr_ctrl_put_u32(payload + 32, target_group);
	return true;
}

static bool midr_ctrl_list_identity_usable(struct bgp *bgp,
					 struct in_addr rid,
					 const struct ipaddr *transport);

static bool midr_ctrl_decode_request(struct bgp *bgp, const uint8_t *payload,
				     size_t len, struct ipaddr source,
				     struct midr_ctrl_request *request)
{
	if (!bgp || !payload || !request || len != MIDR_CTRL_REQUEST_LEN ||
	    payload[0] != MIDR_CTRL_MSG_VERSION ||
	    !midr_ctrl_is_request_type(payload[1]) ||
	    midr_ctrl_get_u16(payload + 2) != 0)
		return false;

	memset(request, 0, sizeof(*request));
	request->type = payload[1];
	memcpy(&request->requester_rid, payload + 4,
	       sizeof(request->requester_rid));
	if (request->requester_rid.s_addr == INADDR_ANY ||
	    !midr_ctrl_decode_locator(payload + 8,
				      &request->requester_transport))
		return false;
	request->requester_asn = (as_t)midr_ctrl_get_u32(payload + 28);
	request->target_group = midr_ctrl_get_u32(payload + 32);

	/* Clients bind their active locator, so the observed source must be
	 * exactly the locator asserted in the payload. */
	if (!midr_ipaddr_valid_locator(&source) ||
	    !midr_ipaddr_same(&source, &request->requester_transport) ||
	    !midr_ctrl_same_active_family(bgp,
					 &request->requester_transport) ||
	    IPV4_ADDR_SAME(&request->requester_rid, &bgp->router_id) ||
	    !midr_ctrl_list_identity_usable(bgp, request->requester_rid,
					    &request->requester_transport))
		return false;
	return true;
}

/* ------------------------------------------------------------------ */
/* Peer-request UDP control channel                                     */
/* ------------------------------------------------------------------ */

/* Low-level UDP send for fixed-size control requests.  List requests are
 * dispatched to the TCP short-connection transport by request_attempt(). */
static void midr_ctrl_send_req(struct bgp_midr_nds *mi, struct ipaddr dst,
			       uint8_t type, uint32_t target_group)
{
	uint8_t payload[MIDR_CTRL_REQUEST_LEN];
	union sockunion su;

	if (!mi || !mi->bgp || mi->ctrl_sock < 0 ||
	    !midr_ctrl_is_udp_type(type) ||
	    !midr_ipaddr_valid_locator(&dst) ||
	    !midr_ctrl_same_active_family(mi->bgp, &dst))
		return;
	if (!midr_ctrl_encode_request(mi->bgp, payload, sizeof(payload), type,
				      target_group))
		return;
	if (!midr_ipaddr_to_sockunion(&dst, &su))
		return;
	if (sockunion_family(&su) == AF_INET)
		su.sin.sin_port = htons(MIDR_CTRL_UDP_PORT);
	else
		su.sin6.sin6_port = htons(MIDR_CTRL_UDP_PORT);

	if (sendto(mi->ctrl_sock, payload, sizeof(payload), 0,
		   (struct sockaddr *)&su, sockunion_sizeof(&su)) < 0)
		zlog_warn("MIDR ctrl: request type %u sendto %pIA failed: %s",
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
static void midr_ctrl_request_attempt(struct bgp *bgp, struct ipaddr dst,
				      uint8_t type, uint32_t target_group)
{
	if (!bgp || !bgp->midr_nds_info)
		return;
	if (midr_ctrl_is_udp_type(type))
		midr_ctrl_send_req(bgp->midr_nds_info, dst, type, target_group);
	else if (midr_ctrl_is_tcp_request_type(type))
		midr_ctrl_tcp_client_start(bgp, dst, type, target_group);
}

/* Send a request once and enqueue (or refresh) a retransmit keyed by (dst,type).
 * rp = 本入队点自带的重传参数；NULL 取默认（3s × 5 次、等完成信号）。 */
static void midr_ctrl_enqueue_request(struct bgp *bgp, struct ipaddr dst,
				      uint8_t type, uint32_t target_group,
				      const struct midr_ctrl_retx_params *rp)
{
	struct bgp_midr_nds *mi;
	struct listnode *node;
	struct midr_ctrl_pending *p;

	if (!bgp || !bgp->midr_nds_info ||
	    !bgp->midr_nds_info->ctrl_pending ||
	    !midr_ctrl_is_request_type(type) ||
	    !midr_ipaddr_valid_locator(&dst))
		return;
	mi = bgp->midr_nds_info;

	if (!rp)
		rp = &midr_ctrl_retx_default;
	if (rp->interval_ms <= 0 || rp->count <= 0)
		return;

	if (!mi->transport_active ||
	    !midr_ctrl_same_active_family(bgp, &dst)) {
		zlog_warn("MIDR ctrl: no active same-family transport; cannot send request type %u to %pIA",
			  type, &dst);
		return;
	}

	midr_ctrl_request_attempt(bgp, dst, type, target_group);

	for (ALL_LIST_ELEMENTS_RO(mi->ctrl_pending, node, p))
		if (midr_ipaddr_same(&p->target_transport, &dst) &&
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

void midr_ctrl_set_retx_budget(struct bgp *bgp, struct ipaddr dst,
			       uint8_t type, int retries)
{
	struct bgp_midr_nds *mi;
	struct listnode *node;
	struct midr_ctrl_pending *p;

	if (!bgp || !bgp->midr_nds_info ||
	    !bgp->midr_nds_info->ctrl_pending || retries <= 0)
		return;
	mi = bgp->midr_nds_info;

	for (ALL_LIST_ELEMENTS_RO(mi->ctrl_pending, node, p))
		if (midr_ipaddr_same(&p->target_transport, &dst) &&
		    p->type == type) {
			/*
			 * 只压不抬：入队时已按默认预算发过第一次，这里改的是"还能再
			 * 试几次"。往大了改没有正当用途（真要更执着，改的该是默认值）。
			 */
			if (p->retries_left > retries) {
				p->retries_left = retries;
				p->params.count = retries;
			}
			MIDR_FLOW_LOG("MIDR ctrl: %s 到 %pIA 的重传预算压到 %d 次（约 %d 秒死心）",
				      midr_ctrl_msg_type_str(type), &dst,
				      p->retries_left,
				      p->retries_left * p->params.interval_ms /
					      1000);
			return;
		}
}

/* New node -> bootstrap: request the representative directory. */
void midr_ctrl_send_rep_request(struct bgp *bgp,
				struct ipaddr bootstrap_transport)
{
	if (!bgp || !bgp->midr_nds_info)
		return;
	midr_ctrl_enqueue_request(bgp, bootstrap_transport,
				  MIDR_CTRL_REP_LIST_REQ, 0,
				  &midr_ctrl_retx_default);
	MIDR_FLOW_LOG("MIDR ctrl: sent REP_LIST_REQ to %pIA", &bootstrap_transport);
}

/* New node -> representative: request the group's member list (table A). */
void midr_ctrl_send_member_request(struct bgp *bgp, struct ipaddr rep_transport,
				   uint32_t group_id)
{
	if (!bgp || !bgp->midr_nds_info)
		return;
	midr_ctrl_enqueue_request(bgp, rep_transport, MIDR_CTRL_MEMBER_LIST_REQ,
				  group_id, &midr_ctrl_retx_default);
	MIDR_FLOW_LOG("MIDR ctrl: sent MEMBER_LIST_REQ to %pIA (group %u)",
		  &rep_transport, group_id);
}

/* 群代表 -> 引导：要一份活引导名单（②按需拉取，子稿 §2②）。 */
void midr_ctrl_send_bootstrap_list_request(struct bgp *bgp, struct ipaddr dst)
{
	if (!bgp || !bgp->midr_nds_info)
		return;
	midr_ctrl_enqueue_request(bgp, dst, MIDR_CTRL_BOOTSTRAP_LIST_REQ, 0,
				  &midr_ctrl_retx_default);
	MIDR_FLOW_LOG("MIDR ctrl: sent BOOTSTRAP_LIST_REQ to %pIA", &dst);
}

/*
 * New node -> an arbitrary candidate (rep or member): self-announce so the
 * receiver can validate our subsequent PM probes without an explicit
 * request/response round trip. One-way, fire-and-forget — same frame as
 * MIDR_CTRL_ANNOUNCE's existing use from midr_ctrl_recv_member_list(), just
 * exposed for callers outside this file (e.g. midr_join_on_rep_list() in
 * bgp_midr_nds.c, which needs to announce to *every* rep in the directory, not
 * only the bootstrap that already received a REP_LIST_REQ from us).
 */
void midr_ctrl_send_announce(struct bgp *bgp, struct ipaddr dst)
{
	if (!bgp || !bgp->midr_nds_info)
		return;
	midr_ctrl_send_req(bgp->midr_nds_info, dst, MIDR_CTRL_ANNOUNCE, 0);
	MIDR_FLOW_LOG("MIDR ctrl: sent ANNOUNCE to %pIA", &dst);
}

/*
 * Drop the pending retransmit for (dst,type) — response arrived from that
 * specific destination.  Scoped by dst (not just type) since several
 * MEMBER_LIST_REQ can be in flight to different reps at once (join candidate
 * plus runner-up groups); filtering by type alone would drop other still-
 * outstanding destinations' retransmit tracking the moment any one responds.
 */
static void midr_ctrl_drop_pending(struct bgp_midr_nds *mi, struct ipaddr dst,
				   uint8_t type)
{
	struct listnode *node, *nnode;
	struct midr_ctrl_pending *p;

	if (!mi || !mi->ctrl_pending)
		return;

	for (ALL_LIST_ELEMENTS(mi->ctrl_pending, node, nnode, p))
		if (p->type == type &&
		    midr_ipaddr_same(&p->target_transport, &dst)) {
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
					       struct ipaddr dst)
{
	struct listnode *node;
	struct midr_ctrl_pending *p;

	if (!mi || !mi->ctrl_pending)
		return 0;

	for (ALL_LIST_ELEMENTS_RO(mi->ctrl_pending, node, p))
		if (midr_ctrl_is_peer_req_like(p->type) &&
		    midr_ipaddr_same(&p->target_transport, &dst))
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
	uint8_t type;

	if (!entry->has_transport_addr ||
	    !midr_ctrl_same_active_family(bgp, &entry->transport_addr))
		return;

	/*
	 * 挂靠边发专用类型（批 5 前置①），其余照旧 PEER_REQUEST。reason 是调用方
	 * 自报的来意（会话台账那套），这里直接复用、不另加参数——"为什么要这条边"
	 * 与"用哪种请求去要"本就是同一件事的两面。
	 */
	type = (reason == MIDR_SESSION_ATTACH) ? MIDR_CTRL_ATTACH_REQUEST
					       : MIDR_CTRL_PEER_REQUEST;

	midr_ctrl_enqueue_request(bgp, entry->transport_addr, type,
				  mi->local_group_id, &midr_ctrl_retx_default);
	MIDR_FLOW_LOG("MIDR ctrl: sent %s to %pIA (group %u)",
		  midr_ctrl_msg_type_str(type), &entry->transport_addr,
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
static void midr_ctrl_send_peer_reject(struct bgp *bgp, struct ipaddr dst)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;

	if (!midr_ipaddr_valid_locator(&dst))
		return; /* 请求帧没自报 transport，无处可回 */

	midr_ctrl_send_req(mi, dst, MIDR_CTRL_PEER_REJECT,
			   mi->local_group_id);
	MIDR_FLOW_LOG("MIDR ctrl: sent PEER_REJECT to %pIA", &dst);
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
static void midr_ctrl_drop_half_peer(struct bgp *bgp, struct ipaddr transport,
				     const char *why)
{
	union sockunion su;
	struct peer *peer;

	if (!midr_ipaddr_to_sockunion(&transport, &su))
		return;
	peer = peer_lookup(bgp, &su);

	if (peer && midr_nds_peer_is_overlay(peer)) {
		zlog_info("MIDR ctrl: 拆除 %pIA 的半边会话——%s（到此为止，不再重试）",
			  &transport, why);
		peer_delete(peer);
	} else if (peer) {
		MIDR_LOG("MIDR ctrl: %pIA 死心，但该地址上是运维原生会话，只销账不拆会话（⑦ 守卫）",
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
	struct ipaddr rep_failed = {}; /* §8.32：本轮死心的 REP_LIST_REQ 目标 */
	bool rep_gave_up = false;
	struct ipaddr blist_failed = {}; /* 本轮死心的 BOOTSTRAP_LIST_REQ 目标 */
	bool blist_gave_up = false;
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

			if (!midr_ipaddr_to_sockunion(&p->target_transport, &su))
				continue;
			peer = peer_lookup(bgp, &su);
			if (peer && peer->connection &&
			    peer->connection->status == Established) {
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
			zlog_warn("MIDR ctrl: %s 尝试 %d 次无响应，放弃（目标 %pIA）——请检查本端到 %pIA 的 underlay 路由（transport 互通前提）",
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
 * Control v4 requires RID and locator to form a duplicate-free one-to-one
 * mapping within each list.  Read back the already encoded items so all three
 * builders can preserve their existing source order while dropping a later
 * conflicting identity.  State tables are already list-backed and small; the
 * receive side still uses the bounded O(n log n) whole-frame validator for
 * untrusted maximum-sized input.
 */
static bool midr_ctrl_list_has_identity(const struct stream *s, uint32_t count,
					size_t item_len,
					size_t locator_offset,
					size_t rid_offset,
					struct in_addr rid,
					const struct ipaddr *transport)
{
	const uint8_t *base = STREAM_DATA(s) + MIDR_CTRL_LIST_HDR_LEN;

	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *item = base + (size_t)i * item_len;
		struct ipaddr decoded;
		struct in_addr decoded_rid;

		memcpy(&decoded_rid, item + rid_offset, sizeof(decoded_rid));
		if (!midr_ctrl_decode_locator(item + locator_offset, &decoded))
			return true; /* encoded prefix is corrupt: fail closed */
		if (IPV4_ADDR_SAME(&decoded_rid, &rid) ||
		    midr_ipaddr_same(&decoded, transport))
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
 *   2) midr_rep_candidates() 从 global_view 按 GROUP_REP 位推导的候选；RID
 *      或 locator 与已写条目冲突的跳过，维持 v4 一对一身份映射。
 * 合并后 0 条则返回 NULL = 不回包 (沉默同闸门): 回空表会让客户端清目录+销重
 * 试项后一次性"放弃加入", 沉默则 3s×5 重试可等远端视图收敛自愈——自动化使
 * "目录空"成为引导节点重启后的必经收敛窗口, 必须保住重试。
 *
 * 注意: 用 stream_new() 按精确条数预分配, 不用 stream_new_expandable()。因为
 * 底层 stream_put() 会先跑 CHECK_SIZE 把写入长度截到当前缓冲余量、再判断是否
 * 扩容——即 raw stream_put 对可扩 stream 也不会真正扩容 (FRR 已知 wart)。预分
 * 配到位则 CHECK_SIZE 永不触发, 整块写入正确 (上界过分配无害, endp 反映实际)。
 */
static struct stream *midr_ctrl_build_rep_list(struct bgp *bgp,
					       struct ipaddr dst)
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
	s = stream_new(MIDR_CTRL_LIST_HDR_LEN +
		       (size_t)maxn * MIDR_CTRL_REP_ITEM_LEN);

	stream_putc(s, MIDR_CTRL_MSG_VERSION);
	stream_putc(s, MIDR_CTRL_REP_LIST_RESP);
	count_pos = stream_get_endp(s);
	stream_putw(s, 0); /* count 占位, 末尾回填 */

	/* 来源一: rep_dir 全量, 排前 (覆盖生效机制, 见函数头) */
	for (ALL_LIST_ELEMENTS_RO(mi->rep_dir, node, r)) {
		if (count >= 65535) {
			zlog_warn("MIDR ctrl: rep directory exceeds 65535 — REP_LIST_RESP truncated");
			break;
		}
		if (!midr_ctrl_same_active_family(bgp, &r->rep_transport) ||
		    r->rep_rid.s_addr == INADDR_ANY)
			continue;
		if (midr_ctrl_list_has_identity(
			    s, count, MIDR_CTRL_REP_ITEM_LEN, 4, 28,
			    r->rep_rid, &r->rep_transport)) {
			n_dedup++;
			continue;
		}
		if (!midr_ctrl_stream_put_rep_item(
			    s, r->group_id, &r->rep_transport, r->rep_asn,
			    r->rep_rid))
			continue;
		/* v4 真名栏: rep_dir 里的条目一律自带 rid——手配来源已随
		 * `midr rep group` 删除 (2026-08-11 批 1.5), 剩下的唯一来源是
		 * "收 REP_LIST_RESP 全量替换", 那些条目的 rid 由对端按节点表真名
		 * 键填好。v4 不编码 rid=0 的兼容占位。 */
		count++;
		n_dir++;
	}

	/* 来源二: 推导候选 (has_transport_addr 已由 midr_rep_candidates 保证) */
	for (ALL_LIST_ELEMENTS_RO(derived, node, ne)) {
		if (count >= 65535) {
			zlog_warn("MIDR ctrl: rep directory exceeds 65535 — REP_LIST_RESP truncated");
			break;
		}
		if (ne->node_id.family != AF_INET ||
		    ne->node_id.u.prefix4.s_addr == INADDR_ANY ||
		    !midr_ctrl_same_active_family(bgp, &ne->transport_addr))
			continue;
		if (midr_ctrl_list_has_identity(
			    s, count, MIDR_CTRL_REP_ITEM_LEN, 4, 28,
			    ne->node_id.u.prefix4, &ne->transport_addr)) {
			n_dedup++;
			continue;
		}
		if (!midr_ctrl_stream_put_rep_item(
			    s, ne->group_id, &ne->transport_addr, ne->asn,
			    ne->node_id.u.prefix4))
			continue;
		count++;
		n_derived++;
	}
	list_delete(&derived);

	if (count == 0) {
		/* 沉默保重试 (见函数头); 客户端重试等收敛后再答 */
		MIDR_LOG("MIDR ctrl: rep directory empty (view not converged?) — not answering %pIA",
			 &dst);
		stream_free(s);
		return NULL;
	}

	stream_putw_at(s, count_pos, (uint16_t)count);
	MIDR_FLOW_LOG("MIDR ctrl: sent REP_LIST_RESP (%u reps: %u rep_dir + %u derived, %u deduped) to %pIA",
		      count, n_dir, n_derived, n_dedup, &dst);
	return s;
}

/*
 * 组装一个 MEMBER_LIST_RESP payload (table A, 含自己在前, 使加入节点必与 rep
 * 建连), 精确预分配 stream (原因见 build_rep_list)。上限同上 (u16 count)。
 */
static struct stream *midr_ctrl_build_member_list(struct bgp *bgp,
						  struct ipaddr dst,
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
	s = stream_new(MIDR_CTRL_LIST_HDR_LEN +
		       (size_t)maxn * MIDR_CTRL_MEMBER_ITEM_LEN);

	stream_putc(s, MIDR_CTRL_MSG_VERSION);
	stream_putc(s, MIDR_CTRL_MEMBER_LIST_RESP);
	count_pos = stream_get_endp(s);
	stream_putw(s, 0);

	/* Ourselves (the representative) first. */
	{
		struct ipaddr local;

		if (bgp->router_id.s_addr != INADDR_ANY &&
		    midr_nds_local_transport_get(bgp, &local) &&
		    midr_ctrl_stream_put_member_item(
			    s, bgp->router_id, &local, bgp->as, group_id))
			count++;
	}

	for (ALL_LIST_ELEMENTS_RO(members, node, entry)) {
		if (count >= 65535) {
			zlog_warn("MIDR ctrl: group %u members exceed 65535 — MEMBER_LIST_RESP truncated",
				  group_id);
			break;
		}
		if (entry->node_id.family != AF_INET ||
		    entry->node_id.u.prefix4.s_addr == INADDR_ANY ||
		    !entry->has_transport_addr ||
		    !midr_ctrl_same_active_family(bgp, &entry->transport_addr))
			continue;
		if (midr_ctrl_list_has_identity(
			    s, count, MIDR_CTRL_MEMBER_ITEM_LEN, 4, 0,
			    entry->node_id.u.prefix4, &entry->transport_addr))
			continue;
		if (!midr_ctrl_stream_put_member_item(
			    s, entry->node_id.u.prefix4, &entry->transport_addr,
			    entry->asn, group_id))
			continue;
		count++;
	}
	list_delete(&members);

	stream_putw_at(s, count_pos, (uint16_t)count);
	MIDR_FLOW_LOG("MIDR ctrl: sent MEMBER_LIST_RESP (%u members) for group %u to %pIA",
		      count, group_id, &dst);
	return s;
}

/*
 * 引导侧：组装 BOOTSTRAP_LIST_RESP（hdr + 28B 条目），子稿 §2②。
 *
 * 内容 = **手配名单 ∩ 自己骨干会话活性** + **本机自己**：
 *   - 手配名单 = 本机 bootstrap_list（`midr bootstrap` 逐条配的，引导之间互配）。
 *     运维登记是主来源；收到 BOOTSTRAP_LIST 后学到的条目以 SEED 身份补进同一
 *     个池，本函数一视同仁。权威 remote-view 若提供引导事实，也可作为兼容来源。
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
						     struct ipaddr dst)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct stream *s;
	struct listnode *node;
	struct midr_bootstrap_entry *b;
	uint32_t count = 0, n_skip = 0;
	uint32_t maxn;
	size_t count_pos;

	/* 精确预分配（原因见 build_rep_list：raw stream_put 对可扩 stream 也不
	 * 真扩容，FRR 已知 wart）。上界 = 自己 + 全部候选，过分配无害。 */
	maxn = 1 + listcount(mi->bootstrap_list);
	if (maxn > 65535)
		maxn = 65535;
	s = stream_new(MIDR_CTRL_LIST_HDR_LEN +
		       (size_t)maxn * MIDR_CTRL_BOOTSTRAP_ITEM_LEN);

	stream_putc(s, MIDR_CTRL_MSG_VERSION);
	stream_putc(s, MIDR_CTRL_BOOTSTRAP_LIST_RESP);
	count_pos = stream_get_endp(s);
	stream_putw(s, 0); /* count 占位，末尾回填 */

	{
		struct ipaddr local;

		if (bgp->router_id.s_addr != INADDR_ANY &&
		    midr_nds_local_transport_get(bgp, &local) &&
		    midr_ctrl_stream_put_bootstrap_item(
			    s, &local, bgp->as, bgp->router_id)) {
			/* 自己这条的 rid 直接取本机 router-id——最权威的来源，不必绕候选池
			 * （批 5 R 系列，协议 v4 条目三栏）。 */
			count++;
		}
	}

	for (ALL_LIST_ELEMENTS_RO(mi->bootstrap_list, node, b)) {
		if (count >= 65535) {
			zlog_warn("MIDR ctrl: bootstrap list exceeds 65535 — BOOTSTRAP_LIST_RESP truncated");
			break;
		}
		/* 自己已在首条（手配名单理论上不含自己，防御性去重）。 */
		if (mi->transport_active && midr_ipaddr_same(
						   &b->transport,
						   &mi->active_transport_addr))
			continue;
		if (!midr_ctrl_transport_session_up(bgp, b->transport)) {
			n_skip++;
			continue;
		}
		if (b->rid.s_addr == INADDR_ANY ||
		    !midr_ctrl_same_active_family(bgp, &b->transport))
			continue;
		if (midr_ctrl_list_has_identity(
			    s, count, MIDR_CTRL_BOOTSTRAP_ITEM_LEN, 0, 24,
			    b->rid, &b->transport))
			continue;
		/* rid 来自候选池条目，而池内 rid 恒非 0（入口把关，见
		 * midr_bootstrap_list_add）——运维手配 `midr bootstrap` 时必填。 */
		if (!midr_ctrl_stream_put_bootstrap_item(
			    s, &b->transport, b->asn, b->rid))
			continue;
		count++;
	}

	if (count == 0) {
		MIDR_LOG("MIDR ctrl: 活引导名单为空（本机 transport 未配？骨干会话未起？）——不回答 %pIA",
			 &dst);
		stream_free(s);
		return NULL;
	}

	stream_putw_at(s, count_pos, (uint16_t)count);
	MIDR_FLOW_LOG("MIDR ctrl: sent BOOTSTRAP_LIST_RESP (%u 台活引导，含本机；%u 个候选因会话未起被滤掉) to %pIA",
		      count, n_skip, &dst);
	return s;
}

struct midr_ctrl_rep_decoded {
	uint32_t group_id;
	struct ipaddr transport;
	as_t asn;
	struct in_addr rid;
};

struct midr_ctrl_member_decoded {
	struct in_addr rid;
	struct ipaddr transport;
	as_t asn;
	uint32_t group_id;
};

struct midr_ctrl_bootstrap_decoded {
	struct ipaddr transport;
	as_t asn;
	struct in_addr rid;
};

/* Read-only semantic validation performed before a decoded list is allowed to
 * mutate NDS state.  Control traffic may refresh metadata, but only the
 * authoritative remote-view transaction may migrate an existing identity to
 * another locator. */
static bool midr_ctrl_list_identity_usable(struct bgp *bgp,
					   struct in_addr rid,
					   const struct ipaddr *transport)
{
	struct bgp_midr_nds *mi;
	struct midr_node_entry key = {};
	struct midr_node_entry *existing;
	struct ipaddr local;

	if (!bgp || !bgp->midr_nds_info ||
	    !bgp->midr_nds_info->global_view || !transport ||
	    rid.s_addr == INADDR_ANY ||
	    !midr_nds_local_transport_get(bgp, &local) ||
	    !midr_ipaddr_valid_locator(transport) ||
	    ipaddr_family(&local) != ipaddr_family(transport))
		return false;

	key.node_id.family = AF_INET;
	key.node_id.prefixlen = IPV4_MAX_BITLEN;
	key.node_id.u.prefix4 = rid;
	mi = bgp->midr_nds_info;

	/* A list may describe us (MEMBER_LIST commonly does), but that identity
	 * must name the active local locator exactly.  A remote identity must
	 * never claim the local locator. */
	if (IPV4_ADDR_SAME(&rid, &bgp->router_id))
		return midr_ipaddr_same(&local, transport) &&
		       midr_nds_locator_unique(bgp, &key.node_id, transport);
	if (midr_ipaddr_same(&local, transport))
		return false;

	existing = midr_node_hash_find(&mi->global_view->nodes, &key);
	if (existing && existing->has_transport_addr &&
	    midr_ipaddr_valid_locator(&existing->transport_addr) &&
	    !midr_ipaddr_same(&existing->transport_addr, transport))
		return false;

	return midr_nds_locator_unique(bgp, &key.node_id, transport);
}

/* A learned bootstrap list may refresh a known candidate, but it must not
 * relabel an operator-configured (or previously learned) locator as another
 * BGP identity.  Check this before list_begin() so a conflicting frame cannot
 * partially age or replace the current candidate set. */
static bool midr_ctrl_bootstrap_locator_binding_usable(
	const struct bgp_midr_nds *mi, struct in_addr rid,
	const struct ipaddr *transport)
{
	struct listnode *node;
	struct midr_bootstrap_entry *candidate;

	if (!mi || !mi->bootstrap_list || !transport)
		return false;

	for (ALL_LIST_ELEMENTS_RO(mi->bootstrap_list, node, candidate)) {
		bool same_rid = candidate->rid.s_addr == rid.s_addr;
		bool same_locator =
			midr_ipaddr_same(&candidate->transport, transport);

		/* An existing candidate may be refreshed only as the very same
		 * identity binding.  Reject both locator relabeling and RID migration;
		 * the latter must arrive through the authoritative Node update path. */
		if (same_rid || same_locator)
			return same_rid && same_locator;
	}
	return true;
}

struct midr_ctrl_identity_key {
	struct in_addr rid;
	uint8_t locator[MIDR_CTRL_LOCATOR_LEN];
};

static int midr_ctrl_identity_cmp_rid(const void *a, const void *b)
{
	const struct midr_ctrl_identity_key *ka = a;
	const struct midr_ctrl_identity_key *kb = b;

	return memcmp(&ka->rid, &kb->rid, sizeof(ka->rid));
}

static int midr_ctrl_identity_cmp_locator(const void *a, const void *b)
{
	const struct midr_ctrl_identity_key *ka = a;
	const struct midr_ctrl_identity_key *kb = b;

	return memcmp(ka->locator, kb->locator, sizeof(ka->locator));
}

/* Within one frame, node identity and locator are a duplicate-free one-to-one
 * mapping.  Sorting a temporary key array keeps this check bounded by
 * O(count log count), including for a legal maximum-sized frame. */
static bool midr_ctrl_list_identities_unique(
	struct midr_ctrl_identity_key *keys, uint16_t count)
{
	uint16_t i;

	if (count < 2)
		return true;

	qsort(keys, count, sizeof(*keys), midr_ctrl_identity_cmp_rid);
	for (i = 1; i < count; i++)
		if (memcmp(&keys[i - 1].rid, &keys[i].rid,
			   sizeof(keys[i].rid)) == 0)
			return false;

	qsort(keys, count, sizeof(*keys), midr_ctrl_identity_cmp_locator);
	for (i = 1; i < count; i++)
		if (memcmp(keys[i - 1].locator, keys[i].locator,
			   sizeof(keys[i].locator)) == 0)
			return false;
	return true;
}

static bool midr_ctrl_decode_list_header(const uint8_t *buf, size_t len,
					 uint8_t type, size_t item_len,
					 uint16_t *count)
{
	size_t expected;

	if (!buf || !count || len < MIDR_CTRL_LIST_HDR_LEN ||
	    buf[0] != MIDR_CTRL_MSG_VERSION || buf[1] != type)
		return false;
	*count = midr_ctrl_get_u16(buf + 2);
	if (*count > (SIZE_MAX - MIDR_CTRL_LIST_HDR_LEN) / item_len)
		return false;
	expected = MIDR_CTRL_LIST_HDR_LEN + (size_t)*count * item_len;
	return len == expected;
}

/* 代表侧：整帧校验后吸收活引导名单，不允许部分列表落状态。 */
static void midr_ctrl_recv_bootstrap_list(struct bgp *bgp, const uint8_t *buf,
					  ssize_t n, struct ipaddr src)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct midr_ctrl_bootstrap_decoded *items = NULL;
	struct midr_ctrl_identity_key *identities = NULL;
	uint16_t count, i;

	if (n < 0 || !midr_ctrl_decode_list_header(
			     buf, (size_t)n, MIDR_CTRL_BOOTSTRAP_LIST_RESP,
			     MIDR_CTRL_BOOTSTRAP_ITEM_LEN, &count) ||
	    !midr_ctrl_same_active_family(bgp, &src)) {
		MIDR_LOG("MIDR ctrl: invalid BOOTSTRAP_LIST_RESP from %pIA — dropping whole frame",
			 &src);
		return;
	}
	if (count) {
		items = XCALLOC(MTYPE_TMP, (size_t)count * sizeof(*items));
		identities = XCALLOC(MTYPE_TMP,
				     (size_t)count * sizeof(*identities));
	}
	for (i = 0; i < count; i++) {
		const uint8_t *item = buf + MIDR_CTRL_LIST_HDR_LEN +
				      (size_t)i * MIDR_CTRL_BOOTSTRAP_ITEM_LEN;

		if (!midr_ctrl_decode_locator(item, &items[i].transport) ||
		    !midr_ctrl_same_active_family(bgp, &items[i].transport))
			goto invalid;
		items[i].asn = (as_t)midr_ctrl_get_u32(item + 20);
		memcpy(&items[i].rid, item + 24, sizeof(items[i].rid));
		if (!midr_ctrl_list_identity_usable(
			    bgp, items[i].rid, &items[i].transport) ||
		    !midr_ctrl_bootstrap_locator_binding_usable(
			    mi, items[i].rid, &items[i].transport))
			goto invalid;
		identities[i].rid = items[i].rid;
		if (!midr_ctrl_encode_locator(identities[i].locator,
					      &items[i].transport))
			goto invalid;
	}
	if (!midr_ctrl_list_identities_unique(identities, count))
		goto invalid;

	midr_ctrl_drop_pending(mi, src, MIDR_CTRL_BOOTSTRAP_LIST_REQ);
	midr_nds_bootstrap_list_begin(bgp);
	for (i = 0; i < count; i++)
		midr_nds_bootstrap_learn(bgp, items[i].transport, items[i].asn,
					 items[i].rid);
	XFREE(MTYPE_TMP, identities);
	XFREE(MTYPE_TMP, items);
	midr_nds_on_bootstrap_list(bgp, src, count);
	return;

invalid:
	XFREE(MTYPE_TMP, identities);
	XFREE(MTYPE_TMP, items);
	MIDR_LOG("MIDR ctrl: invalid BOOTSTRAP_LIST_RESP item from %pIA — dropping whole frame",
		 &src);
}

/* New node: validate the complete directory before replacing the old one. */
static void midr_ctrl_recv_rep_list(struct bgp *bgp, const uint8_t *buf,
				    ssize_t n, struct ipaddr src)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct midr_ctrl_rep_decoded *items = NULL;
	struct midr_ctrl_identity_key *identities = NULL;
	uint16_t count, i;

	if (n < 0 || !midr_ctrl_decode_list_header(
			     buf, (size_t)n, MIDR_CTRL_REP_LIST_RESP,
			     MIDR_CTRL_REP_ITEM_LEN, &count) ||
	    !midr_ctrl_same_active_family(bgp, &src)) {
		MIDR_LOG("MIDR ctrl: invalid REP_LIST_RESP from %pIA — dropping whole frame",
			 &src);
		return;
	}
	if (count) {
		items = XCALLOC(MTYPE_TMP, (size_t)count * sizeof(*items));
		identities = XCALLOC(MTYPE_TMP,
				     (size_t)count * sizeof(*identities));
	}
	for (i = 0; i < count; i++) {
		const uint8_t *item = buf + MIDR_CTRL_LIST_HDR_LEN +
				      (size_t)i * MIDR_CTRL_REP_ITEM_LEN;

		items[i].group_id = midr_ctrl_get_u32(item);
		if (!items[i].group_id ||
		    !midr_ctrl_decode_locator(item + 4, &items[i].transport) ||
		    !midr_ctrl_same_active_family(bgp, &items[i].transport))
			goto invalid;
		items[i].asn = (as_t)midr_ctrl_get_u32(item + 24);
		memcpy(&items[i].rid, item + 28, sizeof(items[i].rid));
		if (!midr_ctrl_list_identity_usable(
			    bgp, items[i].rid, &items[i].transport))
			goto invalid;
		identities[i].rid = items[i].rid;
		if (!midr_ctrl_encode_locator(identities[i].locator,
					      &items[i].transport))
			goto invalid;
	}
	if (!midr_ctrl_list_identities_unique(identities, count))
		goto invalid;

	midr_rep_dir_clear(bgp);
	for (i = 0; i < count; i++)
		midr_rep_dir_add(bgp, items[i].group_id, items[i].transport,
				 items[i].asn, items[i].rid);
	XFREE(MTYPE_TMP, identities);
	XFREE(MTYPE_TMP, items);
	midr_ctrl_drop_pending(mi, src, MIDR_CTRL_REP_LIST_REQ);
	MIDR_FLOW_LOG("MIDR ctrl: REP_LIST_RESP with %u reps — starting join", count);
	midr_join_on_rep_list(bgp);
	return;

invalid:
	XFREE(MTYPE_TMP, identities);
	XFREE(MTYPE_TMP, items);
	MIDR_LOG("MIDR ctrl: invalid REP_LIST_RESP item from %pIA — dropping whole frame",
		 &src);
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
				       ssize_t n, struct ipaddr src)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct midr_ctrl_member_decoded *items = NULL;
	struct midr_ctrl_identity_key *identities = NULL;
	uint16_t count, i;
	uint32_t resp_group;
	bool is_join_candidate, is_anchor;

	if (n < 0 || !midr_ctrl_decode_list_header(
			     buf, (size_t)n, MIDR_CTRL_MEMBER_LIST_RESP,
			     MIDR_CTRL_MEMBER_ITEM_LEN, &count) ||
	    !midr_ctrl_same_active_family(bgp, &src)) {
		MIDR_LOG("MIDR ctrl: invalid MEMBER_LIST_RESP from %pIA — dropping whole frame",
			 &src);
		return;
	}
	if (count) {
		items = XCALLOC(MTYPE_TMP, (size_t)count * sizeof(*items));
		identities = XCALLOC(MTYPE_TMP,
				     (size_t)count * sizeof(*identities));
	}
	for (i = 0; i < count; i++) {
		const uint8_t *item = buf + MIDR_CTRL_LIST_HDR_LEN +
				      (size_t)i * MIDR_CTRL_MEMBER_ITEM_LEN;

		memcpy(&items[i].rid, item, sizeof(items[i].rid));
		if (items[i].rid.s_addr == INADDR_ANY ||
		    !midr_ctrl_decode_locator(item + 4, &items[i].transport) ||
		    !midr_ctrl_same_active_family(bgp, &items[i].transport))
			goto invalid;
		items[i].asn = (as_t)midr_ctrl_get_u32(item + 24);
		items[i].group_id = midr_ctrl_get_u32(item + 28);
		if (!items[i].group_id ||
		    (i && items[i].group_id != items[0].group_id))
			goto invalid;
		if (!midr_ctrl_list_identity_usable(
			    bgp, items[i].rid, &items[i].transport))
			goto invalid;
		identities[i].rid = items[i].rid;
		if (!midr_ctrl_encode_locator(identities[i].locator,
					      &items[i].transport))
			goto invalid;
	}
	if (!midr_ctrl_list_identities_unique(identities, count))
		goto invalid;

	/* 收到响应 → 停止对这个目的地重传（不管下面判到哪一路）。 */
	midr_ctrl_drop_pending(mi, src, MIDR_CTRL_MEMBER_LIST_REQ);

	if (count == 0) {
		MIDR_LOG("MIDR ctrl: MEMBER_LIST_RESP from %pIA 为空，无法判断所属群，忽略",
			 &src);
		XFREE(MTYPE_TMP, identities);
		XFREE(MTYPE_TMP, items);
		return;
	}
	/* 代表自己那条（组装时放最前）自带真实群号，用它判定这是哪一路响应。 */
	resp_group = items[0].group_id;
	is_join_candidate =
		mi->join_group_id != 0 && resp_group == mi->join_group_id;
	is_anchor = !is_join_candidate && resp_group != 0 &&
		    (resp_group == mi->anchor_group_id[0] ||
		     resp_group == mi->anchor_group_id[1]);

	if (!is_join_candidate && !is_anchor) {
		MIDR_LOG("MIDR ctrl: MEMBER_LIST_RESP 群 %u 与当前候选群/锚点群都不符（过期响应？），忽略",
			 resp_group);
		XFREE(MTYPE_TMP, identities);
		XFREE(MTYPE_TMP, items);
		return;
	}

	/*
	 * ⑥ 先探后判：把成员表灌入 global_view、按所属路径决定是否标邻居
	 * （is_adjacent，仅 join 候选群路径）并 I-1 探测，但【不在此建连】。
	 * 建连推迟到 I-7 决策之后（JOIN 走 connect_group；ANCHOR 走
	 * midr_ctrl_connect，见 midr_nds_on_cluster_decision）。
	 */
	for (i = 0; i < count; i++) {
		uint32_t item_gid = items[i].group_id;

		if (IPV4_ADDR_SAME(&items[i].rid, &bgp->router_id))
			continue; /* 跳过描述自己的条目 */

		if (is_join_candidate) {
			midr_nds_learn_member(
				bgp, items[i].rid, items[i].asn,
				items[i].transport, item_gid);
			MIDR_FLOW_LOG("MIDR 加入：I-1 探测成员 %pI4（群 %u）",
				      &items[i].rid, item_gid);
		} else {
			midr_nds_learn_anchor_candidate(
				bgp, items[i].rid, items[i].asn,
				items[i].transport, item_gid);
			MIDR_FLOW_LOG("MIDR 锚点：I-1 探测锚点候选 %pI4（次优群 %u）",
				      &items[i].rid, item_gid);
		}

		/*
		 * 该成员从未跟我们交换过任何报文（我们是从群代表的
		 * MEMBER_LIST_RESP 里间接得知它的），它的 pm_is_known_transport
		 * 校验会把我们刚发起的探测包当未知来源丢弃。发一个单向 ANNOUNCE
		 * 自报身份，让它记住我们——不等回复、不触发建连。两条路径的候选
		 * 都需要，语义相同。
		 */
		midr_ctrl_send_req(mi, items[i].transport, MIDR_CTRL_ANNOUNCE, 0);
	}
	XFREE(MTYPE_TMP, identities);
	XFREE(MTYPE_TMP, items);

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
	return;

invalid:
	XFREE(MTYPE_TMP, identities);
	XFREE(MTYPE_TMP, items);
	MIDR_LOG("MIDR ctrl: invalid MEMBER_LIST_RESP item from %pIA — dropping whole frame",
		 &src);
}

/* ------------------------------------------------------------------ */
/* TCP 列表交换 —— 语义层回调 (被 bgp_midr_ctrl_tcp.c 调用)             */
/* ------------------------------------------------------------------ */

/*
 * 传输层收到一个完整请求帧（36B version-4 request）后回调。返回响应 payload stream
 * (不含长度前缀, 由传输层封帧发出); 返回 NULL = 不回包 (闸门不过, 沉默同 UDP)。
 * 闸门逻辑与日志文案照搬 UDP 版 midr_ctrl_udp_recv 的对应分支。
 */
struct stream *midr_ctrl_on_tcp_request(struct bgp *bgp, const uint8_t *payload,
					size_t len, struct ipaddr remote)
{
	struct bgp_midr_nds *mi;
	struct midr_ctrl_request request;

	if (!bgp || !bgp->midr_nds_info)
		return NULL;
	mi = bgp->midr_nds_info;
	if (!midr_ctrl_decode_request(bgp, payload, len, remote, &request)) {
		MIDR_LOG("MIDR ctrl: invalid TCP request from %pIA — dropping",
			 &remote);
		return NULL;
	}

	/* 退网守卫（TCP 请求侧，同 UDP 那道的理由）：退网了就不再以引导/代表身份
	 * 应答任何列表请求。返回 NULL = 沉默不回包，与闸门不过时同款处置。 */
	if (mi->shutdown) {
		MIDR_LOG("MIDR 退网：丢弃控制通道 TCP 请求 type=%u（本机已退网）",
			 request.type);
		return NULL;
	}

	switch (request.type) {
	case MIDR_CTRL_REP_LIST_REQ:
		if (request.target_group != 0)
			return NULL;
		/* Only a bootstrap node answers (闸门照 MEMBER_LIST 的
		 * GROUP_REP 模式, 无组匹配项; BOOTSTRAP 位由此升"闸门+通告+
		 * 目录数据源"三职) */
		if (!(mi->local_capabilities & MIDR_CAP_BOOTSTRAP)) {
			MIDR_LOG("MIDR ctrl: ignoring REP_LIST_REQ from %pIA (caps 0x%x — not a bootstrap)",
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
		MIDR_FLOW_LOG("MIDR ctrl: REP_LIST_REQ from %pIA — replying with rep directory",
			      &request.requester_transport);
		return midr_ctrl_build_rep_list(bgp, remote);
	case MIDR_CTRL_MEMBER_LIST_REQ: {
		uint32_t group = request.target_group;

		/* Only a representative of this group answers. */
		if (!group || !(mi->local_capabilities & MIDR_CAP_GROUP_REP) ||
		    group != mi->local_group_id) {
			MIDR_LOG("MIDR ctrl: ignoring MEMBER_LIST_REQ for group %u (caps 0x%x our-group %u)",
				 group, mi->local_capabilities,
				 mi->local_group_id);
			return NULL;
		}
		/* 同 REP_LIST_REQ: 先学请求方身份, 放行其后续 PM 探测。 */
		midr_nds_learn_requester(
			bgp, request.requester_rid, request.requester_asn,
			request.requester_transport);
		MIDR_FLOW_LOG("MIDR ctrl: MEMBER_LIST_REQ for group %u from %pIA — replying",
			      group, &request.requester_transport);
		return midr_ctrl_build_member_list(bgp, remote, group);
	}
	case MIDR_CTRL_BOOTSTRAP_LIST_REQ:
		if (request.target_group != 0)
			return NULL;
		/* 只有引导节点答（闸门同 REP_LIST_REQ：BOOTSTRAP 位既是资格也是
		 * 数据源）。不学请求方身份——这条路径之后没有 PM 探测要放行
		 * （拿名单是为了建挂靠会话，不是为了探测）。 */
		if (!(mi->local_capabilities & MIDR_CAP_BOOTSTRAP)) {
			MIDR_LOG("MIDR ctrl: ignoring BOOTSTRAP_LIST_REQ from %pIA (caps 0x%x — not a bootstrap)",
				 &remote, mi->local_capabilities);
			return NULL;
		}
		MIDR_FLOW_LOG("MIDR ctrl: BOOTSTRAP_LIST_REQ from %pIA — replying with live bootstrap list",
			      &request.requester_transport);
		return midr_ctrl_build_bootstrap_list(bgp, remote);
	default:
		MIDR_LOG("MIDR ctrl: TCP unexpected request type %u from %pIA",
			 request.type, &remote);
		return NULL;
	}
}

/*
 * 传输层收到一个完整响应帧后回调。req_type = 本端当初发出的请求类型 (响应类型
 * 配对校验, 防串台); src = 响应来源 (并发多路 MEMBER_LIST_REQ 时, 供
 * drop_pending 精确清对应 (dst,type) 条目, 见 bgp_midr_ctrl.h 声明处注释);
 * 转现有 recv_rep_list / recv_member_list / recv_bootstrap_list 灌视图
 * + 推进 join。
 */
void midr_ctrl_on_tcp_response(struct bgp *bgp, uint8_t req_type,
			       const uint8_t *payload, size_t len,
			       struct ipaddr src)
{
	uint8_t response_type;

	if (!bgp || !bgp->midr_nds_info || !payload)
		return;
	if (len < MIDR_CTRL_LIST_HDR_LEN) {
		MIDR_LOG("MIDR ctrl: TCP response too short (%zu)", len);
		return;
	}
	if (payload[0] != MIDR_CTRL_MSG_VERSION)
		return;
	response_type = payload[1];

	/* 退网守卫（TCP 响应侧）：退网时相关状态已清空，迟到的 REP/MEMBER/
	 * BOOTSTRAP_LIST_RESP
	 * 不该再灌视图、更不该推进一个已经不存在的 join（join_intent 那道守卫只拦
	 * REP_LIST_RESP 那一路，这里一并堵死）。 */
	if (bgp->midr_nds_info && bgp->midr_nds_info->shutdown) {
		MIDR_LOG("MIDR 退网：丢弃控制通道 TCP 响应 type=%u（本机已退网）",
			 response_type);
		return;
	}

	switch (response_type) {
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
	default:
		MIDR_LOG("MIDR ctrl: TCP unexpected response type %u",
			 response_type);
		break;
	}
}

/* Read one fixed-size control datagram and dispatch on its type.  List
 * exchange (REP/MEMBER/BOOTSTRAP_LIST) uses TCP short connections. */
static void midr_ctrl_udp_recv(struct event *t)
{
	struct bgp *bgp = EVENT_ARG(t);
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	uint8_t buf[MIDR_CTRL_REQUEST_LEN + 1];
	struct midr_ctrl_request request;
	union sockunion source_su = {};
	struct ipaddr source = midr_ipaddr_none();
	socklen_t source_len = sizeof(source_su);
	ssize_t n;

	/* Keep listening regardless of how this datagram is handled. */
	event_add_read(bm->master, midr_ctrl_udp_recv, bgp, mi->ctrl_sock,
		       &mi->t_ctrl_read);

	n = recvfrom(mi->ctrl_sock, buf, sizeof(buf), 0,
		     (struct sockaddr *)&source_su, &source_len);
	if (n < 0) {
		if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
			zlog_warn("MIDR ctrl: recvfrom failed: %s",
				  safe_strerror(errno));
		return;
	}
	if (!midr_sockunion_to_ipaddr(&source_su, &source) ||
	    !midr_ctrl_decode_request(bgp, buf, (size_t)n, source, &request)) {
		MIDR_LOG("MIDR ctrl: invalid UDP request from %pIA — dropping",
			 &source);
		return;
	}
	if (!midr_ctrl_is_udp_type(request.type)) {
		MIDR_LOG("MIDR ctrl: request type %s is not permitted over UDP",
			 midr_ctrl_msg_type_str(request.type));
		return;
	}

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
			 request.type);
		return;
	}

	switch (request.type) {
	case MIDR_CTRL_PEER_REQUEST:
	case MIDR_CTRL_ATTACH_REQUEST: {
		uint32_t target_group = request.target_group;
		struct midr_node_entry req = {};
		enum midr_session_reason reason;
		/*
		 * 批 5 前置①：挂靠走专用类型，两类共用本分支——帧结构与回配动作
		 * 完全一致，**唯一差别是挂靠跳过引导负面守卫**（见下面 ③′）。
		 */
		bool is_attach = (request.type == MIDR_CTRL_ATTACH_REQUEST);

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
		 * 合并第二组之后）。届时必须再升协议版本并加签名头；当前 v4 帧不
		 * 预留隐式扩展，避免新旧实现误解同一版本。
		 */

		/*
		 * ③ 排除名单：运维 `no midr session` 拉黑过的节点，不许它反过来
		 * 把会话拽起来。这是 §G 四条里点名"接收侧还缺的那道"——此前
		 * should_peer 与 midr_ctrl_connect 各有一道，唯独接收路径没有，
		 * 于是拉黑的对端只要主动发 PEER_REQUEST 就能绕回来。
		 * 键正好对得上：名单以 router-id 为键（批 2 换键），而报文自带
		 * requester_rid。
		 */
		if (midr_nds_is_session_excluded(bgp,
						 request.requester_rid)) {
			MIDR_LOG("MIDR ctrl: 拒绝 %s —— 发起方 rid %pI4 在会话排除名单中",
				 midr_ctrl_msg_type_str(request.type),
				 &request.requester_rid);
			midr_ctrl_send_peer_reject(bgp,
						   request.requester_transport);
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
		 * 带新能力位的权威 Node fact。
		 *
		 * 最坏误拒窗（定稿结论 15 记）：对端能力位变化到权威 remote-view
		 * 更新到达之间；优雅下线通常秒级，猝死则受 BGP holdtime 约束。
		 * 窗口内被误拒的一方会重试（3s×5），通常
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
			rkey.node_id.u.prefix4 = request.requester_rid;
			rentry = midr_node_hash_find(&mi->global_view->nodes,
						     &rkey);
			if (rentry &&
			    !(rentry->capabilities &
			      (MIDR_CAP_BOOTSTRAP | MIDR_CAP_GROUP_REP))) {
				MIDR_LOG("MIDR ctrl: 拒绝 PEER_REQUEST —— 本机是引导节点，而发起方 %pI4 在节点表里是普通成员（无 BOOTSTRAP/GROUP_REP 位）",
					 &request.requester_rid);
				midr_ctrl_send_peer_reject(
					bgp, request.requester_transport);
				return;
			}
		}

		/* ④ 其余一律回配。
		 * The message is self-describing — build a transient entry and
		 * peer back; no dependency on the BGP-LS node table. */
		req.node_id.family = AF_INET;
		req.node_id.prefixlen = IPV4_MAX_BITLEN;
		req.node_id.u.prefix4 = request.requester_rid;
		req.asn = request.requester_asn;
		req.group_id = target_group;
		req.transport_addr = request.requester_transport;
		req.has_transport_addr = true;

		MIDR_FLOW_LOG("MIDR ctrl: %s from rid %pI4 transport %pIA AS %u group %u — peering back",
			  midr_ctrl_msg_type_str(request.type),
			  &request.requester_rid,
			  &request.requester_transport, (unsigned int)req.asn,
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
		 * v4 解码已强制 UDP 源地址与帧内 transport 完全一致，因此用帧内字段
		 * 查 pending 与按数据报来源查是同一个键；不接受 underlay 自动选出的
		 * 其它源地址。
		 *
		 * 处理 = **提前死心**，与 3s×5 超时死心完全同款语义（结论 23）：
		 * 停重传 + 拆自己预配的半边 + warn 到此为止。区别只是快——秒级，
		 * 而不是盲等 15s。
		 * 换不换一台试的口径也与超时死心一致（见 retx_timer 里那段）：
		 * PEER_REQUEST 不换（指名连这一个，无替补池）；ATTACH_REQUEST 换
		 * （D2 failover：盖 attach_failed 章 + 立即重挑下一台引导）。
		 */
		/* 在途的可能是 PEER_REQUEST 也可能是 ATTACH_REQUEST（挂靠被拒），
		 * 查出是哪一种：既作防伪判据，也用来清对应那条 pending。 */
		req_type = midr_ctrl_pending_peer_req_type(
			mi, request.requester_transport);
		if (!req_type) {
			MIDR_LOG("MIDR ctrl: 丢弃 PEER_REJECT —— 本机并未朝 %pIA 发建连请求（防伪判据）",
				 &request.requester_transport);
			return;
		}

		zlog_info("MIDR ctrl: %pIA 拒绝了本机的 %s（rid %pI4），提前死心：停止重传并拆除半边会话（拒绝原因见对端日志）",
			  &request.requester_transport,
			  midr_ctrl_msg_type_str(req_type),
			  &request.requester_rid);
		midr_ctrl_drop_pending(mi, request.requester_transport, req_type);
		midr_ctrl_drop_half_peer(bgp, request.requester_transport,
					 "对端明确回了 PEER_REJECT（提前死心）");
		/* 挂靠被拒 → 换一台（D2）。此处不在遍历 ctrl_pending，盖章后
		 * 直接重挑是安全的。 */
		if (req_type == MIDR_CTRL_ATTACH_REQUEST) {
			enum midr_attach_batch batch;

			/* 重挑从死者所在批次继续（批 6 的 A-3）。 */
			batch = midr_nds_attach_mark_failed(
				bgp, request.requester_transport);
			midr_nds_attach_pick_from(bgp, batch);
		}
		break;
	}
	case MIDR_CTRL_ANNOUNCE:
		/*
		 * 一个候选群成员在被灌入某加入节点的探测列表前，双方从未交换过
		 * 任何报文——PM 的来源校验（pm_is_known_transport）会把加入节点
		 * 的探测包当作未知来源丢弃。这里只是记住发送者身份，不回复、不
		 * 建连（保持"探成员阶段只探不连"）。
		 */
		midr_nds_learn_requester(
			bgp, request.requester_rid, request.requester_asn,
			request.requester_transport);
		MIDR_FLOW_LOG("MIDR ctrl: ANNOUNCE from %pIA — noted for PM source validation",
			  &request.requester_transport);
		break;
	default:
		break;
	}
}

void midr_ctrl_init(struct bgp *bgp)
{
	struct bgp_midr_nds *mi;

	if (!bgp || !bgp->midr_nds_info)
		return;
	mi = bgp->midr_nds_info;

	mi->ctrl_sock = -1;
	mi->ctrl_pending = list_new();
	midr_ctrl_tcp_init(bgp);
}

void midr_ctrl_close(struct bgp *bgp)
{
	struct bgp_midr_nds *mi;
	struct listnode *node, *nnode;
	struct midr_ctrl_pending *p;

	if (!bgp || !bgp->midr_nds_info)
		return;
	mi = bgp->midr_nds_info;

	event_cancel(&mi->t_ctrl_read);
	event_cancel(&mi->t_ctrl_retx);
	mi->ctrl_retx_tick_ms = 0;
	if (mi->ctrl_sock >= 0) {
		close(mi->ctrl_sock);
		mi->ctrl_sock = -1;
	}
	midr_ctrl_tcp_close(bgp);

	if (mi->ctrl_pending)
		for (ALL_LIST_ELEMENTS(mi->ctrl_pending, node, nnode, p)) {
			list_delete_node(mi->ctrl_pending, node);
			XFREE(MTYPE_MIDR_CTRL_PENDING, p);
		}
}

void midr_ctrl_forget_target(struct bgp *bgp, struct ipaddr transport)
{
	struct bgp_midr_nds *mi;
	struct listnode *node, *nnode;
	struct midr_ctrl_pending *pending;
	union sockunion su;
	struct peer *peer;

	if (!bgp || !bgp->midr_nds_info ||
	    !midr_ipaddr_valid_locator(&transport))
		return;
	mi = bgp->midr_nds_info;

	if (mi->ctrl_pending)
		for (ALL_LIST_ELEMENTS(mi->ctrl_pending, node, nnode, pending)) {
			if (!midr_ipaddr_same(&pending->target_transport,
					      &transport))
				continue;
			list_delete_node(mi->ctrl_pending, node);
			XFREE(MTYPE_MIDR_CTRL_PENDING, pending);
		}
	if (!mi->ctrl_pending || list_isempty(mi->ctrl_pending)) {
		event_cancel(&mi->t_ctrl_retx);
		mi->ctrl_retx_tick_ms = 0;
	}

	midr_ctrl_tcp_cancel_target(bgp, transport);
	if (!midr_ipaddr_to_sockunion(&transport, &su))
		return;
	peer = peer_lookup(bgp, &su);
	if (peer && midr_nds_peer_is_overlay(peer))
		peer_delete(peer);
}

bool midr_ctrl_open(struct bgp *bgp, const struct ipaddr *local)
{
	struct bgp_midr_nds *mi;
	union sockunion su;
	int sock;

	if (!bgp || !bgp->midr_nds_info ||
	    !midr_ipaddr_valid_locator(local))
		return false;
	mi = bgp->midr_nds_info;

	/* The two transports form one active control endpoint.  A partial open
	 * is closed and reported as failure to the NDS reconcile transaction. */
	midr_ctrl_close(bgp);
	if (!midr_ctrl_tcp_open(bgp, local))
		return false;

	sock = socket(ipaddr_family(local), SOCK_DGRAM, 0);
	if (sock < 0) {
		zlog_err("MIDR ctrl: UDP socket(%pIA) failed: %s", local,
			 safe_strerror(errno));
		goto fail;
	}
	sockopt_reuseaddr(sock);
	if (IS_IPADDR_V6(local)) {
		int one = 1;

		if (setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &one,
			       sizeof(one)) < 0) {
			zlog_err("MIDR ctrl: UDP IPV6_V6ONLY(%pIA) failed: %s",
				 local, safe_strerror(errno));
			close(sock);
			goto fail;
		}
	}
	if (!midr_ipaddr_to_sockunion(local, &su) ||
	    sockunion_bind(sock, &su, MIDR_CTRL_UDP_PORT, &su) < 0) {
		zlog_err("MIDR ctrl: UDP bind(%pIA:%u) failed: %s", local,
			 MIDR_CTRL_UDP_PORT, safe_strerror(errno));
		close(sock);
		goto fail;
	}
	set_nonblocking(sock);
	mi->ctrl_sock = sock;
	event_add_read(bm->master, midr_ctrl_udp_recv, bgp, sock,
		       &mi->t_ctrl_read);
	MIDR_LOG("MIDR ctrl: UDP channel ready on %pIA:%u", local,
		 MIDR_CTRL_UDP_PORT);
	return true;

fail:
	midr_ctrl_tcp_close(bgp);
	return false;
}

void midr_ctrl_finish(struct bgp *bgp)
{
	struct bgp_midr_nds *mi;

	if (!bgp || !bgp->midr_nds_info)
		return;
	mi = bgp->midr_nds_info;

	midr_ctrl_close(bgp);
	midr_ctrl_tcp_finish(bgp);
	if (mi->ctrl_pending)
		list_delete(&mi->ctrl_pending);
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
 *   - 激活 (AFI_BGP_LS, SAFI_MIDR_LS)：拓扑信息在 MIDR 对等体间直接流动；
 *   - 撤销 IPv4/IPv6 单播：FRR 建 peer 时按 bgp->default_af 自动附送（bgpd.c
 *     peer_create，在 peer_remote_as() 内部就已完成、无从阻止，只能建完
 *     再撤）。不撤的后果：overlay 多跳会话把远端路由以更短 AS path 注入
 *     underlay——本端向邻居通告"其实必须借道该邻居才走得通"的路径，形成
 *     控制面无环、转发面成环的递归下一跳环路（07-21 十节点实测 g1b/g1c
 *     互指、g2c 探测全丢；见 docs/decisions/midr-overlay-underlay-layering.md）。
 *     MIDR 会话只承载 MIDR-LS——谁提供转发可达性，谁才开单播地址族。
 *
 * 此时会话尚未 Established，peer_deactivate 只清配置位、不触发 reset。
 */
void midr_nds_ctrl_setup_overlay_peer(struct bgp *bgp, struct peer *peer)
{
	struct ipaddr local;

	/*
	 * Stamp ownership FIRST: this is the single point both the auto-connect
	 * path and the `midr neighbor` command pass through, so the flag
	 * uniquely marks peers MIDR created.  The session-ownership guards
	 * (midr_nds_peer_is_overlay) read it to avoid touching operator sessions.
	 */
	SET_FLAG(peer->flags, PEER_FLAG_MIDR_OVERLAY);

	peer_ebgp_multihop_set(peer, MAXTTL);
	/* 死亡感知 15s（复刻旧 expire 灵敏度）。定位与取舍见宏定义处注释。 */
	peer_timers_set(peer, MIDR_OVERLAY_KEEPALIVE, MIDR_OVERLAY_HOLDTIME);
	if (midr_nds_local_transport_get(bgp, &local)) {
		union sockunion local_su;

		if (midr_ipaddr_to_sockunion(&local, &local_su))
			peer_update_source_addr_set(peer, &local_su);
	}
	/* MIDR overlay 会话只载 (4,9) MIDR-LS —— 件②（轮 4）退役了我方自有的
	 * (4,8) BGP-LS 那一族，拓扑情报全部由第二组编码传播。 */
	peer_activate(peer, AFI_BGP_LS, SAFI_MIDR_LS);
	peer_deactivate(peer, AFI_IP, SAFI_UNICAST);
	peer_deactivate(peer, AFI_IP6, SAFI_UNICAST);
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
	struct bgp_midr_nds *mi;
	struct midr_node_entry key = {};
	struct midr_node_entry *known;
	union sockunion su;
	struct ipaddr local;
	struct peer *peer;
	struct in_addr remote_rid = { .s_addr = INADDR_ANY };
	bool has_remote_rid;
	as_t asn;
	int ret;

	if (!bgp || !bgp->midr_nds_info || !entry)
		return;
	mi = bgp->midr_nds_info;
	asn = entry->asn;
	if (entry->node_id.family != AF_INET ||
	    entry->node_id.prefixlen != IPV4_MAX_BITLEN) {
		zlog_warn("MIDR ctrl: invalid remote router-id %pFX; refusing session",
			  &entry->node_id);
		return;
	}
	remote_rid = entry->node_id.u.prefix4;
	has_remote_rid = remote_rid.s_addr != INADDR_ANY;
	/* `midr session` is keyed by the operator-supplied locator and has no RID
	 * argument.  On its first start (and after a cold config restore), the
	 * remote BGP Identifier is therefore legitimately unresolved until OPEN.
	 * Every automatic/control-message path carries an identity and must remain
	 * fail-closed; only MANUAL may temporarily record RID 0. */
	if (!has_remote_rid && reason != MIDR_SESSION_MANUAL) {
		zlog_warn("MIDR ctrl: missing remote router-id for automatic session to %pIA; refusing session",
			  &entry->transport_addr);
		return;
	}

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
	 * 展示同样只显示显式 locator；缺失即显示未配置，避免身份与端点混淆。
	 */
	if (!entry->has_transport_addr ||
	    !midr_ipaddr_valid_locator(&entry->transport_addr)) {
		zlog_warn("MIDR ctrl: %pFX 没有 transport 地址，不建连——该节点多半漏配了 midr transport-address（router-id 只是身份、未必可路由，不作回落）",
			  &entry->node_id);
		return;
	}

	if (!mi || mi->transport_reconfiguring || mi->shutdown ||
	    !midr_nds_local_transport_get(bgp, &local))
		return;
	if (ipaddr_family(&local) != ipaddr_family(&entry->transport_addr)) {
		zlog_warn("MIDR ctrl: local %pIA and remote %pIA transport families differ; refusing session",
			  &local, &entry->transport_addr);
		return;
	}
	known = NULL;
	if (has_remote_rid) {
		key.node_id = entry->node_id;
		known = midr_node_hash_find(&mi->global_view->nodes, &key);
	}
	if (known && known != entry && known->has_transport_addr &&
	    midr_ipaddr_valid_locator(&known->transport_addr) &&
	    !midr_ipaddr_same(&known->transport_addr,
			       &entry->transport_addr)) {
		zlog_warn("MIDR ctrl: router-id %pI4 is already bound to locator %pIA; refusing transient rewrite to %pIA",
			  &remote_rid, &known->transport_addr,
			  &entry->transport_addr);
		return;
	}
	if (!midr_nds_locator_unique(bgp, &entry->node_id,
				      &entry->transport_addr)) {
		zlog_warn("MIDR ctrl: locator %pIA is claimed by multiple node identities; refusing session",
			  &entry->transport_addr);
		return;
	}

	/*
	 * 自连闸：谁也不许跟本机建会话（判据 = rid 撞本机 router-id 或目标撞本机
	 * transport）。四条路里只有 connect_group 天然挡得住（过 should_peer）；
	 * 回配的 rid 是对端帧自报的，挂靠候选池只拒 rid=0 不拒本机
	 * （`midr bootstrap <本机地址>` 即可造出）。不拦则发给本机 5859 的请求被
	 * 自己收下、再走回配，自己给自己记账、自己探自己。
	 */
	if ((has_remote_rid &&
	     IPV4_ADDR_SAME(&entry->node_id.u.prefix4, &bgp->router_id)) ||
	    (mi->transport_active &&
	     midr_ipaddr_same(&entry->transport_addr,
			       &mi->active_transport_addr))) {
		zlog_warn("MIDR ctrl: 拒绝与本机自身建会话（%pFX）——请检查 bootstrap / session 配置是否把本机填成了对端",
			  &entry->node_id);
		return;
	}

	/*
	 * 运维 `no midr session` 持久排除的节点，任何自动路径（发现建邻居、
	 * connect_group 换组/退群重收敛、CL 的 ANCHOR、挂靠）都不得在这里悄悄
	 * 把会话建回来。跨群那几条路本就绕开 midr_discovery_should_peer（定稿
	 * 结论 15：它是成员判断不是闸门），所以本处是唯一能覆盖全部调用路径的
	 * 地方，两道一个都不能丢。`midr session` 同样走本函数，但 MANUAL 原因显式
	 * 绕过本守卫。键 = router-id（08-11 批 2 由地址换来）。
	 */
	if (reason != MIDR_SESSION_MANUAL && has_remote_rid &&
	    midr_nds_is_session_excluded(bgp, entry->node_id.u.prefix4)) {
		MIDR_LOG("MIDR ctrl: %pFX 在会话排除名单中，跳过自动建连",
			 &entry->node_id);
		return;
	}

	if (!midr_ipaddr_to_sockunion(&entry->transport_addr, &su))
		return;

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
	midr_nds_ledger_note(bgp, entry->transport_addr, reason, remote_rid,
			     asn, entry->group_id);

	/*
	 * SAME_GROUP 状态动作：这条边因同群而建，就在此把对端纳入本群邻居——回配与
	 * connect_group 共用（三件套原先手写在后者）。
	 * ⚠ 必须在两道去重之前：地址上已有会话时同样要置位起探，否则老成员拿不到
	 * link_entry，periodic_sync 会误判 LEAVE 散群。
	 * notify CL 由调用方按"一批边建完"的粒度发。
	 */
	if (reason == MIDR_SESSION_SAME_GROUP) {
		struct ipaddr transport = midr_ipaddr_none();

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

	/* 整形成 overlay 会话（multihop + update-source + 只载 MIDR-LS），
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
	struct ipaddr transport;

	if (!entry->has_transport_addr ||
	    !midr_ipaddr_valid_locator(&entry->transport_addr))
		return;
	transport = entry->transport_addr;
	if (!midr_ipaddr_to_sockunion(&transport, &su))
		return;

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
	 * 路径传 force=true 走旁路。显式 `no midr session` 在先删除持久 MANUAL
	 * 意图后也传 true；其余自动路径（对端 withdraw / 换组 / 挂靠卸任 /
	 * B1 老化）一律传 false，豁免照旧。对端那半边收到的是
	 * withdraw、走的正是自动路径，所以仍被 α 拦下（只 warn 不拆，判据 1）。
	 */
	if (!force) {
		const struct midr_session_ledger_entry *led =
			midr_nds_ledger_lookup(bgp, transport);

		if (led && led->reason == MIDR_SESSION_MANUAL) {
			zlog_warn("MIDR ctrl: %pFX（%pIA）的会话是运维手配（台账 MANUAL），跳过自动拆除——要拆请用 no midr session",
				  &entry->node_id, &transport);
			return;
		}
	}

	/*
	 * 拆口销账（结论 20）：走到这里 = MIDR 判定"这条边我不需要了"，需求没了
	 * 账就销——放在最前面，三条退出路径（无 peer / ⑦ 拒拆 / 真拆）都覆盖到。
	 * ⑦ 拒拆那条尤其要销：守卫护的是运维会话本体，台账记的是我方需求，
	 * 两件事（答疑 14）。账留着会让后续凭账认边把运维会话认成 MIDR 的边。
	 */
	midr_nds_ledger_drop(bgp, transport);

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
	/* 自动路径（对端 withdraw / locator 换代 / 换组清理）：α 豁免照旧生效。 */
	midr_try_disconnect(bgp, entry, false);
}

/*
 * 按 transport 拆一条 MIDR 边（批 5 挂靠钩子用）。
 *
 * 为什么需要这个入口：拆边的既有出口 midr_try_disconnect() 收的是**节点表条目**，
 * 而挂靠的对端是专职引导——它通常没有 remote-view Node 条目，拿不到现成条目。
 * 这里按 connect 侧同款做法拼一个临时条目（信息自带、不查表），其余一概复用
 * try_disconnect：MANUAL 豁免（α）、销账、⑦ 归属守卫，一个都不绕过。
 *
 * rid 用于日志可读（"拆的是谁"）；给 0 也能工作（locator 由 transport 决定）。
 *
 * force 见 midr_try_disconnect 内的 α 说明：本端退网和显式
 * `no midr session` 传 true，自动路径一律 false。
 */
void midr_ctrl_detach_transport(struct bgp *bgp, struct ipaddr transport,
				struct in_addr rid, bool force,
				enum midr_stop_reason reason)
{
	struct midr_node_entry e = {};
	struct in_addr display_rid = rid;

	if (!bgp || !bgp->midr_nds_info ||
	    !midr_ipaddr_valid_locator(&transport))
		return;

	e.node_id.family = AF_INET;
	e.node_id.prefixlen = IPV4_MAX_BITLEN;
	e.node_id.u.prefix4 = display_rid;
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
