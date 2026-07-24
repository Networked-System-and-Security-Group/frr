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
#include "bgpd/bgp_midr.h"
#include "bgpd/bgp_midr_ctrl.h"
#include "bgpd/bgp_midr_pm.h" /* midr_pm_add_target（connect_group 启探测） */

DEFINE_MTYPE_STATIC(BGPD, MIDR_CTRL_PENDING, "MIDR ctrl pending peer-request");

/* PEER_REQUEST retransmit: UDP is lossy, so resend until the session is up. */
#define MIDR_CTRL_RETX_INTERVAL 3 /* seconds */
#define MIDR_CTRL_RETX_MAX	5 /* attempts before giving up */

/* struct midr_ctrl_pending 定义已移至 bgp_midr_ctrl.h（show midr join 要展示
 * 未应答请求），MTYPE 仍留在本文件。 */

static void midr_ctrl_send_req(struct bgp_midr *mi, struct in_addr dst,
			       uint8_t type, uint32_t target_group);
static void midr_ctrl_enqueue_request(struct bgp *bgp, struct in_addr dst,
				      uint8_t type, uint32_t target_group);
static void midr_ctrl_send_peer_request(struct bgp *bgp,
					const struct midr_node_entry *entry);
static void midr_ctrl_drop_pending(struct bgp_midr *mi, struct in_addr dst,
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
	default:
		return "UNKNOWN";
	}
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
	struct bgp_midr *mi = bgp->midr_info;

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
static void midr_ctrl_send_req(struct bgp_midr *mi, struct in_addr dst,
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
		zlog_warn("midr_ctrl: request type %u sendto %pI4 failed: %s",
			  type, &dst, safe_strerror(errno));
}

/*
 * 发起一次请求 (首发或重试). 按类型分流传输:
 *   - PEER_REQUEST      : UDP (一次性 nudge, 反向会话 Established 为隐式 ack);
 *   - REP/MEMBER_LIST_REQ: TCP 短连接 (列表交换, 载荷随规模增长必须可靠)。
 * 重试队列 (ctrl_pending) 对两者一致——只是"重发动作"落到不同传输。
 */
static void midr_ctrl_request_attempt(struct bgp *bgp, struct in_addr dst,
				      uint8_t type, uint32_t target_group)
{
	if (type == MIDR_CTRL_PEER_REQUEST)
		midr_ctrl_send_req(bgp->midr_info, dst, type, target_group);
	else
		midr_ctrl_tcp_client_start(bgp, dst, type, target_group);
}

/* Send a request once and enqueue (or refresh) a retransmit keyed by (dst,type). */
static void midr_ctrl_enqueue_request(struct bgp *bgp, struct in_addr dst,
				      uint8_t type, uint32_t target_group)
{
	struct bgp_midr *mi = bgp->midr_info;
	struct listnode *node;
	struct midr_ctrl_pending *p;

	if (!mi->transport_addr_set) {
		zlog_warn("midr_ctrl: local transport-address unset; cannot send request type %u",
			  type);
		return;
	}

	midr_ctrl_request_attempt(bgp, dst, type, target_group);

	for (ALL_LIST_ELEMENTS_RO(mi->ctrl_pending, node, p))
		if (p->target_transport.s_addr == dst.s_addr &&
		    p->type == type) {
			p->target_group = target_group;
			p->retries_left = MIDR_CTRL_RETX_MAX;
			return;
		}

	p = XCALLOC(MTYPE_MIDR_CTRL_PENDING, sizeof(*p));
	p->target_transport = dst;
	p->type = type;
	p->target_group = target_group;
	p->retries_left = MIDR_CTRL_RETX_MAX;
	listnode_add(mi->ctrl_pending, p);

	if (!mi->t_ctrl_retx)
		event_add_timer(bm->master, midr_ctrl_retx_timer, bgp,
				MIDR_CTRL_RETX_INTERVAL, &mi->t_ctrl_retx);
}

/* New node -> bootstrap: request the representative directory. */
void midr_ctrl_send_rep_request(struct bgp *bgp,
				struct in_addr bootstrap_transport)
{
	if (!bgp || !bgp->midr_info)
		return;
	midr_ctrl_enqueue_request(bgp, bootstrap_transport,
				  MIDR_CTRL_REP_LIST_REQ, 0);
	MIDR_FLOW_LOG("midr_ctrl: sent REP_LIST_REQ to %pI4", &bootstrap_transport);
}

/* New node -> representative: request the group's member list (table A). */
void midr_ctrl_send_member_request(struct bgp *bgp, struct in_addr rep_transport,
				   uint32_t group_id)
{
	if (!bgp || !bgp->midr_info)
		return;
	midr_ctrl_enqueue_request(bgp, rep_transport, MIDR_CTRL_MEMBER_LIST_REQ,
				  group_id);
	MIDR_FLOW_LOG("midr_ctrl: sent MEMBER_LIST_REQ to %pI4 (group %u)",
		  &rep_transport, group_id);
}

/*
 * New node -> an arbitrary candidate (rep or member): self-announce so the
 * receiver can validate our subsequent PM probes without an explicit
 * request/response round trip. One-way, fire-and-forget — same frame as
 * MIDR_CTRL_ANNOUNCE's existing use from midr_ctrl_recv_member_list(), just
 * exposed for callers outside this file (e.g. midr_join_on_rep_list() in
 * bgp_midr.c, which needs to announce to *every* rep in the directory, not
 * only the bootstrap that already received a REP_LIST_REQ from us).
 */
void midr_ctrl_send_announce(struct bgp *bgp, struct in_addr dst)
{
	if (!bgp || !bgp->midr_info)
		return;
	midr_ctrl_send_req(bgp->midr_info, dst, MIDR_CTRL_ANNOUNCE, 0);
	MIDR_FLOW_LOG("midr_ctrl: sent ANNOUNCE to %pI4", &dst);
}

/*
 * Drop the pending retransmit for (dst,type) — response arrived from that
 * specific destination.  Scoped by dst (not just type) since several
 * MEMBER_LIST_REQ can be in flight to different reps at once (join candidate
 * plus runner-up groups); filtering by type alone would drop other still-
 * outstanding destinations' retransmit tracking the moment any one responds.
 */
static void midr_ctrl_drop_pending(struct bgp_midr *mi, struct in_addr dst,
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
 * Notify a target node to peer back with us, and queue retransmits so a lost
 * datagram does not leave the reverse session unconfigured.
 */
static void midr_ctrl_send_peer_request(struct bgp *bgp,
					const struct midr_node_entry *entry)
{
	struct bgp_midr *mi = bgp->midr_info;
	struct prefix locator;

	midr_node_get_locator(entry, &locator);
	if (locator.family != AF_INET)
		return;

	midr_ctrl_enqueue_request(bgp, locator.u.prefix4,
				  MIDR_CTRL_PEER_REQUEST, mi->local_group_id);
	MIDR_FLOW_LOG("midr_ctrl: sent PEER_REQUEST to %pI4 (group %u)",
		  &locator.u.prefix4, mi->local_group_id);
}

/*
 * Resend pending requests.  PEER_REQUEST terminates when its reverse session
 * reaches Established; the list requests terminate when their response arrives
 * (removed via midr_ctrl_drop_pending in the recv path).  All give up after
 * MIDR_CTRL_RETX_MAX attempts.
 */
static void midr_ctrl_retx_timer(struct event *t)
{
	struct bgp *bgp = EVENT_ARG(t);
	struct bgp_midr *mi = bgp->midr_info;
	struct listnode *node, *nnode;
	struct midr_ctrl_pending *p;
	struct in_addr rep_failed = { 0 }; /* §8.32：本轮死心的 REP_LIST_REQ 目标 */
	bool rep_gave_up = false;

	for (ALL_LIST_ELEMENTS(mi->ctrl_pending, node, nnode, p)) {
		if (p->type == MIDR_CTRL_PEER_REQUEST) {
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
		if (p->type != MIDR_CTRL_PEER_REQUEST &&
		    midr_ctrl_tcp_client_inflight(mi, p->target_transport,
						  p->type))
			continue;

		if (--p->retries_left <= 0) {
			/* 常开 warn（不进 debug 频道）：反复无响应多半是 underlay
			 * 路由缺失，静默放弃会让故障极难定位。传输中立措辞（UDP 无
			 * 响应 / TCP 连接失败皆适用）。 */
			zlog_warn("midr_ctrl: %s 尝试 %d 次无响应，放弃（目标 %pI4）——请检查本端到 %pI4 的 underlay 路由（transport 互通前提）",
				  midr_ctrl_msg_type_str(p->type),
				  MIDR_CTRL_RETX_MAX, &p->target_transport,
				  &p->target_transport);
			if (p->type == MIDR_CTRL_REP_LIST_REQ) {
				rep_failed = p->target_transport;
				rep_gave_up = true;
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

	if (!list_isempty(mi->ctrl_pending))
		event_add_timer(bm->master, midr_ctrl_retx_timer, bgp,
				MIDR_CTRL_RETX_INTERVAL, &mi->t_ctrl_retx);
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
	struct bgp_midr *mi = bgp->midr_info;
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
			zlog_warn("midr_ctrl: rep directory exceeds 65535 — REP_LIST_RESP truncated");
			break;
		}
		item.group_id = htonl(r->group_id);
		item.rep_transport = r->rep_transport;
		item.rep_asn = htonl((uint32_t)r->rep_asn);
		/* v2 真名栏: 学来的条目自带 rid; 手配条目 (rid=0) 在应答时刻按
		 * transport 反查节点表回填, 仍查不到就发 0 (收方走旧占位路径,
		 * 行为与 v1 相同)。反查是每次应答现查现填, 视图收敛后自愈。 */
		item.rep_rid = r->rep_rid.s_addr != INADDR_ANY
				       ? r->rep_rid
				       : midr_nds_rid_by_transport(
						 bgp, r->rep_transport);
		stream_put(s, &item, sizeof(item));
		count++;
		n_dir++;
	}

	/* 来源二: 推导候选 (has_transport_addr 已由 midr_rep_candidates 保证) */
	for (ALL_LIST_ELEMENTS_RO(derived, node, ne)) {
		struct midr_ctrl_rep_item item;

		if (count >= 65535) {
			zlog_warn("midr_ctrl: rep directory exceeds 65535 — REP_LIST_RESP truncated");
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
		MIDR_LOG("midr_ctrl: rep directory empty (view not converged?) — not answering %pI4",
			 &dst);
		stream_free(s);
		return NULL;
	}

	stream_putw_at(s, count_pos, (uint16_t)count);
	MIDR_FLOW_LOG("midr_ctrl: sent REP_LIST_RESP (%u reps: %u rep_dir + %u derived, %u deduped) to %pI4",
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
	struct bgp_midr *mi = bgp->midr_info;
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
			zlog_warn("midr_ctrl: group %u members exceed 65535 — MEMBER_LIST_RESP truncated",
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
	MIDR_FLOW_LOG("midr_ctrl: sent MEMBER_LIST_RESP (%u members) for group %u to %pI4",
		      count, group_id, &dst);
	return s;
}

/* New node: store the bootstrap's rep directory, then run join stage 1. */
static void midr_ctrl_recv_rep_list(struct bgp *bgp, const uint8_t *buf,
				    ssize_t n, struct in_addr src)
{
	struct bgp_midr *mi = bgp->midr_info;
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
		MIDR_LOG("midr_ctrl: REP_LIST_RESP length %zd != expected for count %u — dropping",
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
	MIDR_FLOW_LOG("midr_ctrl: REP_LIST_RESP with %u reps — starting join", count);
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
	struct bgp_midr *mi = bgp->midr_info;
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
		MIDR_LOG("midr_ctrl: MEMBER_LIST_RESP length %zd != expected for count %u — dropping",
			 (ssize_t)n, count);
		return;
	}
	items = (const struct midr_ctrl_member_item *)(buf + sizeof(*hdr));

	/* 收到响应 → 停止对这个目的地重传（不管下面判到哪一路）。 */
	midr_ctrl_drop_pending(mi, src, MIDR_CTRL_MEMBER_LIST_REQ);

	if (count == 0) {
		MIDR_LOG("midr_ctrl: MEMBER_LIST_RESP from %pI4 为空，无法判断所属群，忽略",
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
		MIDR_LOG("midr_ctrl: MEMBER_LIST_RESP 群 %u 与当前候选群/锚点群都不符（过期响应？），忽略",
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
		 * 都需要，语义相同。
		 */
		midr_ctrl_send_req(mi, items[i].transport, MIDR_CTRL_ANNOUNCE, 0);
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
	struct bgp_midr *mi = bgp->midr_info;
	struct midr_ctrl_msg msg;

	if (!mi)
		return NULL;
	if (len != sizeof(msg)) {
		MIDR_LOG("midr_ctrl: TCP request bad length %zu (want %zu) from %pI4",
			 len, sizeof(msg), &remote);
		return NULL;
	}
	memcpy(&msg, payload, sizeof(msg));
	if (msg.version != MIDR_CTRL_MSG_VERSION)
		return NULL;

	switch (msg.type) {
	case MIDR_CTRL_REP_LIST_REQ:
		/* Only a bootstrap node answers (闸门照 MEMBER_LIST 的
		 * GROUP_REP 模式, 无组匹配项; BOOTSTRAP 位由此升"闸门+通告+
		 * 目录数据源"三职) */
		if (!(mi->local_capabilities & MIDR_CAP_BOOTSTRAP)) {
			MIDR_LOG("midr_ctrl: ignoring REP_LIST_REQ from %pI4 (caps 0x%x — not a bootstrap)",
				 &remote, mi->local_capabilities);
			return NULL;
		}
		/* 请求方接下来会反过来对我们发 PM 探测, 而此刻它尚无 BGP-LS 会话,
		 * 我们的 global_view 里没有它, pm_is_known_transport() 会把其探测包
		 * 当未知来源丢弃。先学一条最小条目放行探测(队友在 UDP 列表 handler
		 * 的原逻辑, 随列表交换迁 TCP 落位到此)。 */
		midr_nds_learn_requester(bgp, msg.requester_rid,
					 (as_t)ntohl(msg.requester_asn),
					 msg.requester_transport);
		MIDR_FLOW_LOG("midr_ctrl: REP_LIST_REQ from %pI4 — replying with rep directory",
			      &msg.requester_transport);
		return midr_ctrl_build_rep_list(bgp, remote);
	case MIDR_CTRL_MEMBER_LIST_REQ: {
		uint32_t group = ntohl(msg.target_group);

		/* Only a representative of this group answers. */
		if (!(mi->local_capabilities & MIDR_CAP_GROUP_REP) ||
		    group != mi->local_group_id) {
			MIDR_LOG("midr_ctrl: ignoring MEMBER_LIST_REQ for group %u (caps 0x%x our-group %u)",
				 group, mi->local_capabilities,
				 mi->local_group_id);
			return NULL;
		}
		/* 同 REP_LIST_REQ: 先学请求方身份, 放行其后续 PM 探测。 */
		midr_nds_learn_requester(bgp, msg.requester_rid,
					 (as_t)ntohl(msg.requester_asn),
					 msg.requester_transport);
		MIDR_FLOW_LOG("midr_ctrl: MEMBER_LIST_REQ for group %u from %pI4 — replying",
			      group, &msg.requester_transport);
		return midr_ctrl_build_member_list(bgp, remote, group);
	}
	default:
		MIDR_LOG("midr_ctrl: TCP unexpected request type %u from %pI4",
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
		MIDR_LOG("midr_ctrl: TCP response too short (%zu)", len);
		return;
	}
	hdr = (const struct midr_ctrl_list_hdr *)payload;
	if (hdr->version != MIDR_CTRL_MSG_VERSION)
		return;

	switch (hdr->type) {
	case MIDR_CTRL_REP_LIST_RESP:
		if (req_type != MIDR_CTRL_REP_LIST_REQ) {
			MIDR_LOG("midr_ctrl: REP_LIST_RESP but our request was type %u — dropping",
				 req_type);
			return;
		}
		midr_ctrl_recv_rep_list(bgp, payload, (ssize_t)len, src);
		break;
	case MIDR_CTRL_MEMBER_LIST_RESP:
		if (req_type != MIDR_CTRL_MEMBER_LIST_REQ) {
			MIDR_LOG("midr_ctrl: MEMBER_LIST_RESP but our request was type %u — dropping",
				 req_type);
			return;
		}
		midr_ctrl_recv_member_list(bgp, payload, (ssize_t)len, src);
		break;
	default:
		MIDR_LOG("midr_ctrl: TCP unexpected response type %u", hdr->type);
		break;
	}
}

/* Read one control datagram and dispatch on its type.  UDP now only carries
 * PEER_REQUEST (20B); list exchange (REP/MEMBER_LIST) moved to TCP. */
static void midr_ctrl_udp_recv(struct event *t)
{
	struct bgp *bgp = EVENT_ARG(t);
	struct bgp_midr *mi = bgp->midr_info;
	uint8_t buf[sizeof(struct midr_ctrl_msg)];
	struct midr_ctrl_msg msg;
	ssize_t n;

	/* Keep listening regardless of how this datagram is handled. */
	event_add_read(bm->master, midr_ctrl_udp_recv, bgp, mi->ctrl_sock,
		       &mi->t_ctrl_read);

	n = recvfrom(mi->ctrl_sock, buf, sizeof(buf), 0, NULL, NULL);
	if (n < 2) {
		if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
			zlog_warn("midr_ctrl: recvfrom failed: %s",
				  safe_strerror(errno));
		return;
	}
	if (buf[0] != MIDR_CTRL_MSG_VERSION)
		return;

	switch (buf[1]) {
	case MIDR_CTRL_PEER_REQUEST: {
		uint32_t target_group;
		struct midr_node_entry req = {};

		if (n < (ssize_t)sizeof(msg))
			return;
		memcpy(&msg, buf, sizeof(msg));
		target_group = ntohl(msg.target_group);

		/* Receiver-side filter: only act if the request targets our
		 * group; unrelated nodes ignore it (this is what makes the
		 * unavoidable broadcast harmless and the connect targeted). */
		if (target_group == 0 || target_group != mi->local_group_id) {
			MIDR_LOG("midr_ctrl: ignoring PEER_REQUEST from %pI4 (target group %u, ours %u)",
				   &msg.requester_rid, target_group,
				   mi->local_group_id);
			return;
		}

		/* The message is self-describing — build a transient entry and
		 * peer back; no dependency on the BGP-LS node table. */
		req.node_id.family = AF_INET;
		req.node_id.prefixlen = IPV4_MAX_BITLEN;
		req.node_id.u.prefix4 = msg.requester_rid;
		req.asn = ntohl(msg.requester_asn);
		req.group_id = target_group;
		req.transport_addr = msg.requester_transport;
		req.has_transport_addr = true;

		MIDR_FLOW_LOG("midr_ctrl: PEER_REQUEST from rid %pI4 transport %pI4 AS %u group %u — peering back",
			  &msg.requester_rid, &msg.requester_transport,
			  (unsigned int)req.asn, target_group);

		midr_ctrl_connect(bgp, &req);
		break;
	}
	case MIDR_CTRL_REP_LIST_REQ:
	case MIDR_CTRL_REP_LIST_RESP:
	case MIDR_CTRL_MEMBER_LIST_REQ:
	case MIDR_CTRL_MEMBER_LIST_RESP:
		/* 列表交换已迁 TCP; 收到 UDP 列表报文多半来自旧版本节点 (混跑不
		 * 兼容, 既定决策)。留此提示便于将来误用混版本时定位。 */
		MIDR_LOG("midr_ctrl: 忽略 UDP 列表报文 type=%s —— 列表交换已迁 TCP (旧版本节点?)",
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
		MIDR_FLOW_LOG("midr_ctrl: ANNOUNCE from %pI4 — noted for PM source validation",
			  &msg.requester_transport);
		break;
	default:
		break;
	}
}

void midr_ctrl_init(struct bgp *bgp)
{
	struct bgp_midr *mi = bgp->midr_info;
	struct sockaddr_in sa = {};
	int sock;

	if (!mi)
		return;

	mi->ctrl_sock = -1;
	mi->ctrl_pending = list_new();

	/* 开 TCP 列表交换通道 (监听失败仅 warn)。先于 UDP 建立, 使二者互不依赖
	 * ——UDP bind 失败的早返回不应连带跳过 TCP。 */
	midr_ctrl_tcp_init(bgp);

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		zlog_warn("midr_ctrl: UDP socket() failed: %s",
			  safe_strerror(errno));
		return;
	}
	sockopt_reuseaddr(sock);

	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port = htons(MIDR_CTRL_UDP_PORT);
	if (bind(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		zlog_warn("midr_ctrl: UDP bind(:%u) failed: %s",
			  MIDR_CTRL_UDP_PORT, safe_strerror(errno));
		close(sock);
		return;
	}
	set_nonblocking(sock);
	mi->ctrl_sock = sock;
	event_add_read(bm->master, midr_ctrl_udp_recv, bgp, sock,
		       &mi->t_ctrl_read);

	MIDR_LOG("midr_ctrl: peer-request UDP channel on :%u",
		  MIDR_CTRL_UDP_PORT);
}

void midr_ctrl_finish(struct bgp *bgp)
{
	struct bgp_midr *mi = bgp->midr_info;
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
	struct bgp_midr *mi = bgp->midr_info;

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
	peer_activate(peer, AFI_BGP_LS, SAFI_BGP_LS);
	peer_deactivate(peer, AFI_IP, SAFI_UNICAST);
}

/*
 * 会话归属判据（⑦，四处守卫共用）：这条 peer 是不是 MIDR 自己建的 overlay 会话？
 *
 * 双条件——两条都过才算自己人：
 *   1. 出身：带 PEER_FLAG_MIDR_OVERLAY 标记（只有上面的整形 helper 会设，
 *      运维经 peer_remote_as 建的会话没有它）；
 *   2. 长相（签名）：除 BGP-LS 外未激活任何地址族。⑥ 之后 MIDR 会话 = 只载
 *      BGP-LS 是不变量；若运维事后手动给它开了 IPv4 等转发面地址族，签名不过、
 *      保守当外人（宁可拒删也不误碰喂转发面的会话）。
 *
 * 判错方向刻意保守：把自己人误判成运维的 → 至多拒删（有原生 no neighbor 兜底）；
 * 反过来误删运维会话不可接受（会砸转发面）。
 */
bool midr_nds_peer_is_overlay(struct peer *peer)
{
	afi_t afi;
	safi_t safi;

	if (!peer)
		return false;

	/* 出身。 */
	if (!CHECK_FLAG(peer->flags, PEER_FLAG_MIDR_OVERLAY))
		return false;

	/* 长相：任何非 BGP-LS 的已激活地址族都说明它在喂转发面，判外人。 */
	FOREACH_AFI_SAFI (afi, safi) {
		if (afi == AFI_BGP_LS && safi == SAFI_BGP_LS)
			continue;
		if (peer->afc[afi][safi])
			return false;
	}

	return true;
}

void midr_ctrl_connect(struct bgp *bgp, const struct midr_node_entry *entry)
{
	union sockunion su;
	struct prefix locator;
	struct peer *peer;
	as_t asn = entry->asn;
	int ret;

	if (asn == 0) {
		zlog_warn("midr_ctrl: skipping %pFX — ASN not yet known",
			  &entry->node_id);
		return;
	}

	/* Peer with the node's real reachable address (TLV 1188), not its
	 * router-id; router-id is only an identity and may be unroutable. */
	midr_node_get_locator(entry, &locator);

	/*
	 * 运维 `no midr session` 持久排除的地址，任何自动路径（发现建邻居、
	 * connect_group 换组/退群重收敛）都不得在这里悄悄把会话建回来：
	 * connect_group 直接按群号遍历成员调用本函数，完全不经
	 * midr_discovery_should_peer 那道闸门。此处补上是唯一能覆盖所有调用
	 * 路径的地方。`midr session`（手工escape hatch）不走本函数，不受影响。
	 */
	if (locator.family == AF_INET &&
	    midr_nds_is_session_excluded(bgp, locator.u.prefix4)) {
		MIDR_LOG("midr_ctrl: %pFX 在会话排除名单中，跳过自动建连",
			 &entry->node_id);
		return;
	}

	prefix2sockunion(&locator, &su);

	/*
	 * S5 第一道去重（⑦）：transport 地址上已有会话。地址被占 = 无法另建（BGP
	 * 一地址一会话），只能复用或告警——终止 A<->B notify 握手也靠这条 return
	 * （收到回响 PEER_REQUEST 的一端在此发现已建的 peer 便停发）。按归属分类：
	 *   - MIDR 自己的：正常复用（重复 connect 很常见）；
	 *   - 运维的、已激活 LS：借它当 LS 通道，可用，记 log；
	 *   - 运维的、未激活 LS：拓扑无通道可走、邻接实际不可用，warn 出来——但绝不
	 *     整形运维会话（那是洞 #3、违反运维优先）。
	 */
	{
		struct peer *occupant = peer_lookup(bgp, &su);

		if (occupant) {
			if (midr_nds_peer_is_overlay(occupant)) {
				/* MIDR 自己的会话，正常复用（沉默）。 */
			} else if (occupant->afc[AFI_BGP_LS][SAFI_BGP_LS]) {
				MIDR_LOG("midr_ctrl: %pFX transport 地址由已激活 LS 的运维会话占用，借用其为 LS 通道",
					 &entry->node_id);
			} else {
				zlog_warn("midr_ctrl: %pFX 的 transport 地址被一条未激活 link-state 的运维会话占用，LS 邻接不可用；请在原生配置为该邻居激活 link-state",
					  &entry->node_id);
			}
			return;
		}
	}

	/*
	 * S5 第二道去重（⑦）：按对端 router-id 找一条现成 Established 会话——典型是
	 * 直连节点的静态链路会话（键 = 链路地址，第一道按 transport 查不到它）。
	 * 收紧：必须【已激活 BGP-LS】才算"已可达、可复用"；只 Established 不够——
	 * 未激活 LS 的会话传不了拓扑、邻接实为坏（原注释"已激活 BGP-LS"是没查证的
	 * 假设）。此时 transport 地址空闲（第一道已放行），照常另建自己的多跳会话。
	 * 只在调用点加 LS 判定、不动 midr_node_established_peer 本身——PM 探测闸门
	 * 与 E-1 origination 还在用它，对"会话"的语义要求不同（Established 即可）。
	 */
	{
		struct peer *reachable =
			midr_node_established_peer(bgp, &entry->node_id);

		if (reachable && reachable->afc[AFI_BGP_LS][SAFI_BGP_LS]) {
			MIDR_LOG("midr_ctrl: %pFX already reachable via existing LS session (router-id match) — skip duplicate transport peering",
				 &entry->node_id);
			return;
		}
		if (reachable)
			MIDR_LOG("midr_ctrl: %pFX 有现成会话但未激活 LS，另建 transport overlay 会话以承载拓扑",
				 &entry->node_id);
	}

	ret = peer_remote_as(bgp, &su, NULL, &asn, AS_EXTERNAL, NULL);
	if (ret != 0) {
		zlog_warn("midr_ctrl: peer_remote_as(%pFX AS %u) failed: %d",
			  &entry->node_id, asn, ret);
		return;
	}

	MIDR_FLOW_LOG("midr_ctrl: peering initiated with %pFX AS %u",
		  &entry->node_id, asn);

	/* 整形成 overlay 会话（multihop + update-source + 只载 BGP-LS），
	 * 定义与理由见 midr_nds_ctrl_setup_overlay_peer 头注释。 */
	peer = peer_lookup(bgp, &su);
	if (peer)
		midr_nds_ctrl_setup_overlay_peer(bgp, peer);

	/*
	 * Bidirectional build-up: ask the target to peer back with us over the
	 * UDP control channel.  Encapsulated here so every connect attempt
	 * (join orchestration or an incoming PEER_REQUEST) notifies the far
	 * end uniformly; the dedup above keeps the handshake from looping.
	 */
	midr_ctrl_send_peer_request(bgp, entry);
}

static void midr_try_disconnect(struct bgp *bgp,
				const struct midr_node_entry *entry)
{
	union sockunion su;
	struct prefix locator;

	midr_node_get_locator(entry, &locator);
	prefix2sockunion(&locator, &su);

	struct peer *peer = peer_lookup(bgp, &su);
	if (!peer)
		return;

	/*
	 * S4 归属守卫（⑦）：只拆 MIDR 自建的 overlay 会话。键空间隔离（静态邻居按
	 * 链路地址注册、这里按 locator 查）在 lab 下成立，但运维用 loopback（= 本
	 * 节点 locator 键）配静态邻居是 iBGP 标准实践、完全合法，此时 peer_lookup
	 * 会命中它——无守卫则误删运维会话、砸转发面。判据 = 标记 ∧ 只载 BGP-LS。
	 * 旧 PEER_FLAG_CONFIG_NODE 守卫不可用：peer_remote_as 对谁都置它、区分不了。
	 */
	if (!midr_nds_peer_is_overlay(peer)) {
		MIDR_LOG("midr_ctrl: %pFX locator 命中运维会话，拒拆（保护 underlay）",
			 &entry->node_id);
		return;
	}

	MIDR_LOG("midr_ctrl: removing peer %pFX (node gone)", &entry->node_id);
	peer_delete(peer);
}

/* ------------------------------------------------------------------ */
/* Public interface (called from bgp_midr.c / NDS)                     */
/* ------------------------------------------------------------------ */

void midr_ctrl_on_node_remove(struct bgp *bgp, struct midr_node_entry *entry)
{
	midr_try_disconnect(bgp, entry);
}

void midr_mark_topology(struct bgp *bgp, const struct midr_node_entry *entry)
{
	(void)bgp;
	/* Stub: record a qualifying connection into the topology graph.
	 * Real topology-graph bookkeeping lands in a later phase. */
	MIDR_LOG("midr_ctrl: mark topology connection to %pFX (group %u) (stub)",
		   &entry->node_id, entry->group_id);
}

int midr_ctrl_connect_group(struct bgp *bgp, uint32_t group_id)
{
	struct midr_node_entry *entry;
	int count = 0;

	if (!bgp || !bgp->midr_info)
		return 0;

	frr_each (midr_node_hash, &bgp->midr_info->global_view->nodes, entry) {
		if (entry->is_self)
			continue;
		if (entry->group_id != group_id)
			continue;
		midr_ctrl_connect(bgp, entry);
		/*
		 * 标记为邻居——与发现路径 midr_nds_on_node_discovered 一致。少了这句，
		 * connect_group 建的会话不带 is_adjacent，后续换组/离群时按 is_adjacent
		 * 判据的拆连（midr_group_reconverge 第 3 步）就找不到它们、造成会话泄漏。
		 */
		entry->is_adjacent = true;
		/*
		 * I-1 启动探测——同样与发现路径一致。少了这句，经 connect_group
		 * 建连的成员（手动换组、I-7 JOIN）永远没有 probe_ctx：PM 每 10s 的
		 * 兜底扫描只会灌零指标保活，E-1 导出的 TLV 1186 恒为 rtt=0/bw=0，
		 * CL 后续拿这些假零做稳态判断即失真。JOIN 路径因成员表阶段已 add
		 * 过而侥幸不显，手动换组则必现（07-21 十节点实验实证）。
		 */
		midr_pm_add_target(bgp, &entry->node_id, MIDR_SRC_GOSSIP,
				   entry->capabilities);
		midr_mark_topology(bgp, entry);
		count++;
	}

	return count;
}
