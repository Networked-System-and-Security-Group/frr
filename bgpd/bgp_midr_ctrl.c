// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR Control Layer —— 语义层
 *
 * Evaluates peering policy from MIDR node table events and drives the BGP
 * peer FSM (peer_remote_as / peer_delete) accordingly.
 *
 * 控制通道双传输 (端口 5859, 语义在此、TCP 传输在 bgp_midr_ctrl_tcp.c):
 *  - UDP: PEER_REQUEST + liveness —— 前者驱动反向建连，后者只在独立
 *    bgp_midr_liveness.c 中处理确认与 Gossip 语义 (独立于 PM 探测通道)。
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
#include "bgpd/bgp_midr_liveness.h"

DEFINE_MTYPE_STATIC(BGPD, MIDR_CTRL_PENDING, "MIDR ctrl pending peer-request");

/* PEER_REQUEST retransmit: UDP is lossy, so resend until the session is up. */
#define MIDR_CTRL_RETX_INTERVAL 3 /* seconds */
#define MIDR_CTRL_RETX_MAX	5 /* attempts before giving up */

/* struct midr_ctrl_pending 定义已移至 bgp_midr_ctrl.h（show midr join 要展示
 * 未应答请求），MTYPE 仍留在本文件。 */

static void midr_ctrl_send_req(struct bgp_midr *mi, struct in_addr dst,
			       uint8_t type, uint32_t target_group);
static bool midr_ctrl_enqueue_request(struct bgp *bgp, struct in_addr dst,
				      uint8_t type, uint32_t target_group);
static void midr_ctrl_send_peer_request(struct bgp *bgp,
					const struct midr_node_entry *entry);
static void midr_ctrl_drop_pending(struct bgp_midr *mi, uint8_t type);
static void midr_ctrl_retx_timer(struct event *t);
static void midr_ctrl_udp_recv(struct event *t);

static void midr_ctrl_prefix_from_in_addr(struct prefix *p, struct in_addr a)
{
	memset(p, 0, sizeof(*p));
	p->family = AF_INET;
	p->prefixlen = IPV4_MAX_BITLEN;
	p->u.prefix4 = a;
}

static bool midr_ctrl_endpoint_usable(struct bgp *bgp, struct in_addr address)
{
	struct prefix endpoint;

	midr_ctrl_prefix_from_in_addr(&endpoint, address);
	return midr_liveness_endpoint_usable(bgp, &endpoint);
}

/* Build a union sockunion (AF_INET) from a bare in_addr. */
static void midr_su_from_in_addr(union sockunion *su, struct in_addr a)
{
	struct prefix p;

	midr_ctrl_prefix_from_in_addr(&p, a);
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
	case MIDR_LIVENESS_PROBE_REQ:
		return "PROBE_REQ";
	case MIDR_LIVENESS_PROBE_RESP:
		return "PROBE_RESP";
	case MIDR_LIVENESS_DEAD:
		return "DEAD";
	case MIDR_LIVENESS_GRACEFUL_LEAVE:
		return "GRACEFUL_LEAVE";
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

int midr_ctrl_udp_send(struct bgp *bgp,
		       const struct sockaddr_in *destination,
		       const void *payload, size_t length)
{
	struct bgp_midr *mi;
	ssize_t sent;

	if (!bgp || !bgp->midr_info || !destination || !payload ||
	    destination->sin_family != AF_INET || destination->sin_port == 0 ||
	    length == 0 || length > MIDR_LIVENESS_MAX_WIRE_SIZE)
		return -1;
	mi = bgp->midr_info;
	if (mi->ctrl_sock < 0)
		return -1;

	sent = sendto(mi->ctrl_sock, payload, length, MSG_DONTWAIT,
		      (const struct sockaddr *)destination,
		      sizeof(*destination));
	if (sent < 0) {
		if (!ERRNO_IO_RETRY(errno))
			zlog_warn("midr_ctrl: UDP sendto %pI4 failed: %s",
				  &destination->sin_addr, safe_strerror(errno));
		return -1;
	}
	if ((size_t)sent != length) {
		zlog_warn("midr_ctrl: UDP short send to %pI4 (%zd/%zu)",
			  &destination->sin_addr, sent, length);
		return -1;
	}
	return 0;
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

	midr_ctrl_udp_send(mi->bgp, &sa, &msg, sizeof(msg));
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
static bool midr_ctrl_enqueue_request(struct bgp *bgp, struct in_addr dst,
				      uint8_t type, uint32_t target_group)
{
	struct bgp_midr *mi = bgp->midr_info;
	struct listnode *node;
	struct midr_ctrl_pending *p;

	if (!mi->transport_addr_set) {
		zlog_warn("midr_ctrl: local transport-address unset; cannot send request type %u",
			  type);
		return false;
	}
	if (!midr_ctrl_endpoint_usable(bgp, dst)) {
		MIDR_LOG("midr_ctrl: refusing %s to quarantined transport %pI4",
			 midr_ctrl_msg_type_str(type), &dst);
		return false;
	}

	midr_ctrl_request_attempt(bgp, dst, type, target_group);

	for (ALL_LIST_ELEMENTS_RO(mi->ctrl_pending, node, p))
		if (p->target_transport.s_addr == dst.s_addr &&
		    p->type == type) {
			p->target_group = target_group;
			p->retries_left = MIDR_CTRL_RETX_MAX;
			return true;
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
	return true;
}

/* New node -> bootstrap: request the representative directory. */
void midr_ctrl_send_rep_request(struct bgp *bgp,
				struct in_addr bootstrap_transport)
{
	if (!bgp || !bgp->midr_info)
		return;
	if (midr_ctrl_enqueue_request(bgp, bootstrap_transport,
				      MIDR_CTRL_REP_LIST_REQ, 0))
		MIDR_FLOW_LOG("midr_ctrl: sent REP_LIST_REQ to %pI4",
			      &bootstrap_transport);
}

/* New node -> representative: request the group's member list (table A). */
void midr_ctrl_send_member_request(struct bgp *bgp, struct in_addr rep_transport,
				   uint32_t group_id)
{
	if (!bgp || !bgp->midr_info)
		return;
	if (midr_ctrl_enqueue_request(bgp, rep_transport,
				      MIDR_CTRL_MEMBER_LIST_REQ, group_id))
		MIDR_FLOW_LOG("midr_ctrl: sent MEMBER_LIST_REQ to %pI4 (group %u)",
			      &rep_transport, group_id);
}

/* Drop all pending retransmits of a given request type (response arrived). */
static void midr_ctrl_drop_pending(struct bgp_midr *mi, uint8_t type)
{
	struct listnode *node, *nnode;
	struct midr_ctrl_pending *p;

	for (ALL_LIST_ELEMENTS(mi->ctrl_pending, node, nnode, p))
		if (p->type == type) {
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

	if (midr_ctrl_enqueue_request(bgp, locator.u.prefix4,
				      MIDR_CTRL_PEER_REQUEST,
				      mi->local_group_id))
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

	for (ALL_LIST_ELEMENTS(mi->ctrl_pending, node, nnode, p)) {
		if (!midr_ctrl_endpoint_usable(bgp, p->target_transport)) {
			MIDR_LOG("midr_ctrl: cancel pending %s to SUSPECT transport %pI4",
				 midr_ctrl_msg_type_str(p->type),
				 &p->target_transport);
			list_delete_node(mi->ctrl_pending, node);
			XFREE(MTYPE_MIDR_CTRL_PENDING, p);
			continue;
		}
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
			list_delete_node(mi->ctrl_pending, node);
			XFREE(MTYPE_MIDR_CTRL_PENDING, p);
			continue;
		}
		midr_ctrl_request_attempt(bgp, p->target_transport, p->type,
					  p->target_group);
	}

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
 *   1) mi->rep_dir (手配/学来) 的当前可用视图排前——原始目录不删除，
 *      SUSPECT/近期删除项恢复后可重新进入；应答条目序 = 客户端目录序，
 *      排前即"人工覆盖/应急兜底"的实际生效机制。
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
	struct list *directory = list_new();
	struct list *derived = list_new();
	struct stream *s;
	struct listnode *node;
	struct midr_rep_entry *r;
	struct midr_node_entry *ne;
	uint32_t maxn;
	uint32_t count = 0, n_dir = 0, n_derived = 0, n_dedup = 0;
	size_t count_pos;

	midr_rep_directory_usable(bgp, directory);
	midr_rep_candidates(bgp, derived);
	maxn = listcount(directory) + listcount(derived);
	if (maxn > 65535)
		maxn = 65535;
	s = stream_new(sizeof(struct midr_ctrl_list_hdr) +
		       (size_t)maxn * sizeof(struct midr_ctrl_rep_item));

	stream_putc(s, MIDR_CTRL_MSG_VERSION);
	stream_putc(s, MIDR_CTRL_REP_LIST_RESP);
	count_pos = stream_get_endp(s);
	stream_putw(s, 0); /* count 占位, 末尾回填 */

	/* 来源一: rep_dir 的可用借用视图，原始目录保持不变。 */
	for (ALL_LIST_ELEMENTS_RO(directory, node, r)) {
		struct midr_ctrl_rep_item item;

		if (count >= 65535) {
			zlog_warn("midr_ctrl: rep directory exceeds 65535 — REP_LIST_RESP truncated");
			break;
		}
		item.group_id = htonl(r->group_id);
		item.rep_transport = r->rep_transport;
		item.rep_asn = htonl((uint32_t)r->rep_asn);
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
		if (!midr_ctrl_endpoint_usable(bgp, ne->transport_addr))
			continue;
		if (midr_ctrl_rep_list_contains(s, count, ne->group_id,
						ne->transport_addr)) {
			n_dedup++;
			continue;
		}
		item.group_id = htonl(ne->group_id);
		item.rep_transport = ne->transport_addr;
		item.rep_asn = htonl((uint32_t)ne->asn);
		stream_put(s, &item, sizeof(item));
		count++;
		n_derived++;
	}
	list_delete(&directory);
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
		if (locator.family != AF_INET ||
		    !midr_liveness_endpoint_usable(bgp, &locator))
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
				    ssize_t n)
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
				 (as_t)ntohl(items[i].rep_asn));

	midr_ctrl_drop_pending(mi, MIDR_CTRL_REP_LIST_REQ);
	MIDR_FLOW_LOG("midr_ctrl: REP_LIST_RESP with %u reps — starting join", count);
	midr_join_on_rep_list(bgp);
}

/*
 * New node: learn the rep's member list (table A) into the global view and
 * probe each (I-1).  Connecting is deferred to the JOIN decision (see ⑥):
 * only after CL judges the group worth joining does NDS connect_group.
 */
static void midr_ctrl_recv_member_list(struct bgp *bgp, const uint8_t *buf,
				       ssize_t n)
{
	struct bgp_midr *mi = bgp->midr_info;
	const struct midr_ctrl_list_hdr *hdr =
		(const struct midr_ctrl_list_hdr *)buf;
	const struct midr_ctrl_member_item *items;
	uint16_t count, i;

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

	/*
	 * ⑥ 先探后判：把成员表灌入 global_view、标记邻居（is_adjacent）并 I-1 探测，
	 * 但【不在此建连】。建连推迟到 CL 判定入群（JOIN）之后，由
	 * midr_nds_on_cluster_decision 的 JOIN 分支调 connect_group。
	 */
	for (i = 0; i < count; i++) {
		struct prefix node_id;

		if (IPV4_ADDR_SAME(&items[i].rid, &bgp->router_id))
			continue; /* 跳过描述自己的条目 */
		midr_ctrl_prefix_from_in_addr(&node_id, items[i].rid);
		if (!midr_liveness_indirect_endpoint_usable(
			    bgp, &node_id, &items[i].transport)) {
			MIDR_LOG("MIDR 加入：忽略隔离成员 %pFX",
				 &node_id);
			continue;
		}

		midr_nds_learn_member(bgp, items[i].rid, ntohl(items[i].asn),
				      items[i].transport,
				      ntohl(items[i].group_id));
		MIDR_FLOW_LOG("MIDR 加入：I-1 探测成员 %pI4（群 %u）",
			      &items[i].rid, ntohl(items[i].group_id));
	}

	/* 收到成员列表响应 → 停止重传 MEMBER_LIST_REQ（UDP 重传队列机制）。 */
	midr_ctrl_drop_pending(mi, MIDR_CTRL_MEMBER_LIST_REQ);

	/*
	 * 探完整批成员后，编排层显式发 MEMBER_PROBE_DONE，交 CL 评估是否入群
	 * （I-7 JOIN/CREATE）。一整批只发一次。不在此清 join_phase：收尾在
	 * midr_nds_on_cluster_decision 的 JOIN/CREATE 分支。
	 */
	MIDR_FLOW_LOG("MIDR 加入：收到群 %u 成员列表，探测完成 → MEMBER_PROBE_DONE",
		      mi->join_group_id);
	midr_nds_notify_cl(bgp, MIDR_TRIGGER_MEMBER_PROBE_DONE);
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
	struct midr_node_entry key = {};

	if (!mi)
		return NULL;
	if (mi->shutdown) {
		MIDR_LOG("midr_ctrl: ignoring TCP list request while MIDR is shut down");
		return NULL;
	}
	if (len != sizeof(msg)) {
		MIDR_LOG("midr_ctrl: TCP request bad length %zu (want %zu) from %pI4",
			 len, sizeof(msg), &remote);
		return NULL;
	}
	memcpy(&msg, payload, sizeof(msg));
	if (msg.version != MIDR_CTRL_MSG_VERSION)
		return NULL;
	if (!midr_ctrl_endpoint_usable(bgp, remote)) {
		MIDR_LOG("midr_ctrl: ignoring list request from quarantined transport %pI4",
			 &remote);
		return NULL;
	}
	key.node_id.family = AF_INET;
	key.node_id.prefixlen = IPV4_MAX_BITLEN;
	key.node_id.u.prefix4 = msg.requester_rid;
	if (!midr_liveness_indirect_endpoint_usable(
		    bgp, &key.node_id, &msg.requester_transport)) {
		MIDR_LOG("midr_ctrl: ignoring list request from quarantined node %pFX",
			 &key.node_id);
		return NULL;
	}

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
 * 配对校验, 防串台); 转现有 recv_rep_list / recv_member_list 灌视图 + 推进 join。
 */
void midr_ctrl_on_tcp_response(struct bgp *bgp, uint8_t req_type,
			       const uint8_t *payload, size_t len,
			       struct in_addr remote)
{
	const struct midr_ctrl_list_hdr *hdr;

	if (!midr_ctrl_endpoint_usable(bgp, remote)) {
		MIDR_LOG("midr_ctrl: ignoring TCP response from quarantined transport %pI4",
			 &remote);
		return;
	}
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
		midr_ctrl_recv_rep_list(bgp, payload, (ssize_t)len);
		break;
	case MIDR_CTRL_MEMBER_LIST_RESP:
		if (req_type != MIDR_CTRL_MEMBER_LIST_REQ) {
			MIDR_LOG("midr_ctrl: MEMBER_LIST_RESP but our request was type %u — dropping",
				 req_type);
			return;
		}
		midr_ctrl_recv_member_list(bgp, payload, (ssize_t)len);
		break;
	default:
		MIDR_LOG("midr_ctrl: TCP unexpected response type %u", hdr->type);
		break;
	}
}

/* Read one control datagram and dispatch on its type.  UDP carries the fixed
 * PEER_REQUEST plus liveness confirmation/gossip; list exchange remains TCP.
 */
static void midr_ctrl_udp_recv(struct event *t)
{
	struct bgp *bgp = EVENT_ARG(t);
	struct bgp_midr *mi = bgp->midr_info;
	uint8_t buf[MIDR_LIVENESS_MAX_WIRE_SIZE];
	struct midr_ctrl_msg msg;
	struct sockaddr_in from = {};
	struct iovec iov = {
		.iov_base = buf,
		.iov_len = sizeof(buf),
	};
	struct msghdr msgh = {
		.msg_name = &from,
		.msg_namelen = sizeof(from),
		.msg_iov = &iov,
		.msg_iovlen = 1,
	};
	ssize_t n;

	/* Keep listening regardless of how this datagram is handled. */
	event_add_read(bm->master, midr_ctrl_udp_recv, bgp, mi->ctrl_sock,
		       &mi->t_ctrl_read);

	n = recvmsg(mi->ctrl_sock, &msgh, MSG_DONTWAIT);
	if (n < 2) {
		if (n < 0 && !ERRNO_IO_RETRY(errno))
			zlog_warn("midr_ctrl: recvfrom failed: %s",
				  safe_strerror(errno));
		return;
	}
	if ((msgh.msg_flags & MSG_TRUNC) || from.sin_family != AF_INET)
		return;
	if (buf[0] != MIDR_CTRL_MSG_VERSION)
		return;
	if (midr_liveness_handle_ctrl(bgp, buf, (size_t)n, &from))
		return;

	switch (buf[1]) {
	case MIDR_CTRL_PEER_REQUEST: {
		uint32_t target_group;
		struct midr_node_entry req = {};

		if (n != (ssize_t)sizeof(msg))
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

		if (!midr_ctrl_endpoint_usable(bgp, from.sin_addr) ||
		    !midr_liveness_indirect_endpoint_usable(
			    bgp, &req.node_id, &req.transport_addr)) {
			MIDR_LOG("midr_ctrl: ignoring PEER_REQUEST from quarantined node %pFX",
				 &req.node_id);
			return;
		}

		MIDR_FLOW_LOG("midr_ctrl: PEER_REQUEST rid %pI4 transport %pI4 AS %u group %u",
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
	if (set_cloexec(sock) < 0) {
		zlog_warn("midr_ctrl: UDP set_cloexec() failed: %s",
			  safe_strerror(errno));
		close(sock);
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

void midr_ctrl_connect(struct bgp *bgp, const struct midr_node_entry *entry)
{
	union sockunion su;
	struct prefix locator;
	struct peer *peer;
	as_t asn;
	int ret;

	if (!bgp || !bgp->midr_info || !entry || bgp->midr_info->shutdown)
		return;
	asn = entry->asn;
	midr_node_get_locator(entry, &locator);

	/* A transient PEER_REQUEST entry may bypass the node table.  The
	 * combined identity+transport gate handles both known SUSPECT/REMOVING
	 * nodes and recently removed unknown endpoints.
	 */
	if (locator.family != AF_INET ||
	    !midr_liveness_indirect_endpoint_usable(
		    bgp, &entry->node_id, &locator.u.prefix4)) {
		MIDR_LOG("midr_ctrl: skip quarantined endpoint %pFX",
			 &entry->node_id);
		return;
	}

	if (asn == 0) {
		zlog_warn("midr_ctrl: skipping %pFX — ASN not yet known",
			  &entry->node_id);
		return;
	}

	/* Peer with the node's real reachable address (TLV 1188), not its
	 * router-id; router-id is only an identity and may be unroutable. */
	prefix2sockunion(&locator, &su);

	/*
	 * Dedup: a session toward this locator already exists.  This both
	 * avoids duplicating a peer and terminates the A<->B notify handshake
	 * — the side that receives the echoed PEER_REQUEST finds the peer it
	 * already created here and stops, so it sends no further notify.
	 */
	if (peer_lookup(bgp, &su))
		return;

	/*
	 * 第二道去重：已存在任意一条到该节点（按对端 router-id 匹配）的 Established
	 * 会话——典型是直连节点配置里手写的静态链路会话——就复用它（已激活 BGP-LS），
	 * 不再叠一条多跳 transport 会话。上面的 peer_lookup 按地址查 connectionhash，
	 * 漏掉以链路地址注册的静态会话，故这里按 router-id 补一道。
	 */
	if (midr_node_established_peer(bgp, &entry->node_id)) {
		MIDR_LOG("midr_ctrl: %pFX already reachable via existing session (router-id match) — skip duplicate transport peering",
			 &entry->node_id);
		return;
	}

	/* AS_SPECIFIED naturally creates iBGP for the same ASN and eBGP for a
	 * different ASN; both cases are permitted by MIDR.
	 */
	ret = peer_remote_as(bgp, &su, NULL, &asn, AS_SPECIFIED, NULL);
	if (ret != 0) {
		zlog_warn("midr_ctrl: peer_remote_as(%pFX AS %u) failed: %d",
			  &entry->node_id, asn, ret);
		return;
	}

	MIDR_FLOW_LOG("midr_ctrl: peering initiated with %pFX AS %u",
		  &entry->node_id, asn);

	/* MIDR peers may not be directly connected — allow multi-hop eBGP. */
	peer = peer_lookup(bgp, &su);
	if (peer) {
		struct bgp_midr *mi = bgp->midr_info;

		if (asn != bgp->as)
			peer_ebgp_multihop_set(peer, MAXTTL);
		/* Activate BGP-LS so topology info flows directly between
		 * MIDR peers, not only via relay nodes. */
		peer_activate(peer, AFI_BGP_LS, SAFI_BGP_LS);
		/*
		 * Source the TCP from our own transport-address (loopback) so
		 * the far end sees a connection from the locator it configured
		 * as the neighbor.  Without this the SYN is sourced from the
		 * egress interface, the far end finds no matching neighbor, and
		 * the multi-hop loopback session stays stuck in Active.
		 */
		if (mi->transport_addr_set) {
			union sockunion local_su;

			midr_su_from_in_addr(&local_su, mi->local_transport_addr);
			peer_update_source_addr_set(peer, &local_su);
		}
	}

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
	 * Tear down the session unconditionally.  The old PEER_FLAG_CONFIG_NODE
	 * guard could not distinguish MIDR-created peers from operator-config
	 * ones (peer_remote_as sets that flag on both), so it never deleted
	 * anything.  Statically configured neighbors generally key by a link
	 * address, not the node locator, so peer_lookup() above won't match
	 * them; if you need static neighbors back, redeploy the config.
	 */
	MIDR_LOG("midr_ctrl: removing peer %pFX (node gone)", &entry->node_id);
	peer_delete(peer);
}

/* ------------------------------------------------------------------ */
/* Public interface (called from bgp_midr.c / NDS)                     */
/* ------------------------------------------------------------------ */

void midr_ctrl_on_node_remove(struct bgp *bgp, struct midr_node_entry *entry)
{
	struct bgp_midr *mi = bgp->midr_info;
	struct listnode *node, *next;
	struct midr_ctrl_pending *pending;
	struct prefix locator;

	midr_node_get_locator(entry, &locator);
	if (locator.family == AF_INET && mi->ctrl_pending)
		for (ALL_LIST_ELEMENTS(mi->ctrl_pending, node, next, pending))
			if (IPV4_ADDR_SAME(&pending->target_transport,
					   &locator.u.prefix4)) {
				list_delete_node(mi->ctrl_pending, node);
				XFREE(MTYPE_MIDR_CTRL_PENDING, pending);
			}
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
		struct prefix locator;

		if (entry->is_self)
			continue;
		if (entry->group_id != group_id)
			continue;
		midr_node_get_locator(entry, &locator);
		if (locator.family != AF_INET ||
		    !midr_liveness_indirect_endpoint_usable(
			    bgp, &entry->node_id, &locator.u.prefix4))
			continue;
		midr_ctrl_connect(bgp, entry);
		midr_mark_topology(bgp, entry);
		count++;
	}

	return count;
}
