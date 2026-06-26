// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR Control Layer
 *
 * Evaluates peering policy from MIDR node table events and drives the BGP
 * peer FSM (peer_remote_as / peer_delete) accordingly.
 *
 * Also owns the MIDR peer-request UDP control channel (independent of PM):
 * after a new node picks its group it unicasts a PEER_REQUEST to each group
 * member's transport address; the member peers back, so both ends configure
 * each other and the (multi-hop eBGP) session establishes bidirectionally.
 */

#include "zebra.h"

#include "memory.h"
#include "frrevent.h"
#include "linklist.h"
#include "network.h" /* set_nonblocking */
#include "sockopt.h" /* sockopt_reuseaddr */
#include "sockunion.h"
#include "prefix.h"
#include "log.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_midr.h"
#include "bgpd/bgp_midr_ctrl.h"

DEFINE_MTYPE_STATIC(BGPD, MIDR_CTRL_PENDING, "MIDR ctrl pending peer-request");

/* PEER_REQUEST retransmit: UDP is lossy, so resend until the session is up. */
#define MIDR_CTRL_RETX_INTERVAL 3 /* seconds */
#define MIDR_CTRL_RETX_MAX	5 /* attempts before giving up */

/* A pending request to retransmit (keyed by destination + type). */
struct midr_ctrl_pending {
	struct in_addr target_transport; /* resend destination */
	uint8_t type;			 /* request type being retransmitted */
	uint32_t target_group;		 /* group field carried in the request */
	int retries_left;
};

static void midr_ctrl_send_req(struct bgp_midr *mi, struct in_addr dst,
			       uint8_t type, uint32_t target_group);
static void midr_ctrl_enqueue_request(struct bgp *bgp, struct in_addr dst,
				      uint8_t type, uint32_t target_group);
static void midr_ctrl_send_peer_request(struct bgp *bgp,
					const struct midr_node_entry *entry);
static void midr_ctrl_drop_pending(struct bgp_midr *mi, uint8_t type);
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

/* ------------------------------------------------------------------ */
/* Peer-request UDP control channel                                     */
/* ------------------------------------------------------------------ */

/* Low-level send: build a request frame (PEER_REQUEST / REP_LIST_REQ /
 * MEMBER_LIST_REQ) carrying our identity and unicast it. */
static void midr_ctrl_send_req(struct bgp_midr *mi, struct in_addr dst,
			       uint8_t type, uint32_t target_group)
{
	struct bgp *bgp = mi->bgp;
	struct midr_ctrl_msg msg = {};
	struct sockaddr_in sa = {};

	if (mi->ctrl_sock < 0 || !mi->transport_addr_set)
		return;

	msg.version = MIDR_CTRL_MSG_VERSION;
	msg.type = type;
	msg.requester_rid = bgp->router_id;
	msg.requester_transport = mi->local_transport_addr;
	msg.requester_asn = htonl(bgp->as);
	msg.target_group = htonl(target_group);

	sa.sin_family = AF_INET;
	sa.sin_port = htons(MIDR_CTRL_UDP_PORT);
	sa.sin_addr = dst;

	if (sendto(mi->ctrl_sock, &msg, sizeof(msg), 0, (struct sockaddr *)&sa,
		   sizeof(sa)) < 0)
		zlog_warn("midr_ctrl: request type %u sendto %pI4 failed: %s",
			  type, &dst, safe_strerror(errno));
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

	midr_ctrl_send_req(mi, dst, type, target_group);

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
		if (--p->retries_left <= 0) {
			MIDR_FLOW_LOG("midr_ctrl: giving up request type %u retransmit to %pI4",
				  p->type, &p->target_transport);
			list_delete_node(mi->ctrl_pending, node);
			XFREE(MTYPE_MIDR_CTRL_PENDING, p);
			continue;
		}
		midr_ctrl_send_req(mi, p->target_transport, p->type,
				   p->target_group);
	}

	if (!list_isempty(mi->ctrl_pending))
		event_add_timer(bm->master, midr_ctrl_retx_timer, bgp,
				MIDR_CTRL_RETX_INTERVAL, &mi->t_ctrl_retx);
}

/* Bootstrap: build and unicast a REP_LIST_RESP from mi->rep_dir. */
static void midr_ctrl_send_rep_list(struct bgp_midr *mi, struct in_addr dst)
{
	uint8_t buf[sizeof(struct midr_ctrl_list_hdr) +
		    MIDR_CTRL_LIST_MAX * sizeof(struct midr_ctrl_rep_item)];
	struct midr_ctrl_list_hdr *hdr = (struct midr_ctrl_list_hdr *)buf;
	struct midr_ctrl_rep_item *items =
		(struct midr_ctrl_rep_item *)(buf + sizeof(*hdr));
	struct sockaddr_in sa = {};
	struct listnode *node;
	struct midr_rep_entry *r;
	uint16_t count = 0;
	size_t len;

	if (mi->ctrl_sock < 0)
		return;

	for (ALL_LIST_ELEMENTS_RO(mi->rep_dir, node, r)) {
		if (count >= MIDR_CTRL_LIST_MAX)
			break;
		items[count].group_id = htonl(r->group_id);
		items[count].rep_transport = r->rep_transport;
		items[count].rep_asn = htonl((uint32_t)r->rep_asn);
		count++;
	}

	hdr->version = MIDR_CTRL_MSG_VERSION;
	hdr->type = MIDR_CTRL_REP_LIST_RESP;
	hdr->count = htons(count);
	len = sizeof(*hdr) + (size_t)count * sizeof(struct midr_ctrl_rep_item);

	sa.sin_family = AF_INET;
	sa.sin_port = htons(MIDR_CTRL_UDP_PORT);
	sa.sin_addr = dst;

	if (sendto(mi->ctrl_sock, buf, len, 0, (struct sockaddr *)&sa,
		   sizeof(sa)) < 0)
		zlog_warn("midr_ctrl: REP_LIST_RESP sendto %pI4 failed: %s",
			  &dst, safe_strerror(errno));
	else
		MIDR_FLOW_LOG("midr_ctrl: sent REP_LIST_RESP (%u reps) to %pI4",
			  count, &dst);
}

/* Representative: build and unicast a MEMBER_LIST_RESP for `group_id` (table A,
 * with ourselves included so the joining node always peers with the rep). */
static void midr_ctrl_send_member_list(struct bgp *bgp, struct in_addr dst,
				       uint32_t group_id)
{
	struct bgp_midr *mi = bgp->midr_info;
	uint8_t buf[sizeof(struct midr_ctrl_list_hdr) +
		    MIDR_CTRL_LIST_MAX * sizeof(struct midr_ctrl_member_item)];
	struct midr_ctrl_list_hdr *hdr = (struct midr_ctrl_list_hdr *)buf;
	struct midr_ctrl_member_item *items =
		(struct midr_ctrl_member_item *)(buf + sizeof(*hdr));
	struct sockaddr_in sa = {};
	struct list *members;
	struct listnode *node;
	struct midr_node_entry *entry;
	uint16_t count = 0;
	size_t len;

	if (mi->ctrl_sock < 0)
		return;

	/* Ourselves (the representative) first. */
	if (mi->transport_addr_set && count < MIDR_CTRL_LIST_MAX) {
		items[count].rid = bgp->router_id;
		items[count].transport = mi->local_transport_addr;
		items[count].asn = htonl(bgp->as);
		items[count].group_id = htonl(group_id);
		count++;
	}

	members = list_new();
	midr_group_members(bgp, group_id, members);
	for (ALL_LIST_ELEMENTS_RO(members, node, entry)) {
		struct prefix locator;

		if (count >= MIDR_CTRL_LIST_MAX)
			break;
		midr_node_get_locator(entry, &locator);
		if (locator.family != AF_INET)
			continue;
		items[count].rid = entry->node_id.u.prefix4;
		items[count].transport = locator.u.prefix4;
		items[count].asn = htonl(entry->asn);
		items[count].group_id = htonl(group_id);
		count++;
	}
	list_delete(&members);

	hdr->version = MIDR_CTRL_MSG_VERSION;
	hdr->type = MIDR_CTRL_MEMBER_LIST_RESP;
	hdr->count = htons(count);
	len = sizeof(*hdr) + (size_t)count * sizeof(struct midr_ctrl_member_item);

	sa.sin_family = AF_INET;
	sa.sin_port = htons(MIDR_CTRL_UDP_PORT);
	sa.sin_addr = dst;

	if (sendto(mi->ctrl_sock, buf, len, 0, (struct sockaddr *)&sa,
		   sizeof(sa)) < 0)
		zlog_warn("midr_ctrl: MEMBER_LIST_RESP sendto %pI4 failed: %s",
			  &dst, safe_strerror(errno));
	else
		MIDR_FLOW_LOG("midr_ctrl: sent MEMBER_LIST_RESP (%u members) for group %u to %pI4",
			  count, group_id, &dst);
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
	if (count > MIDR_CTRL_LIST_MAX)
		count = MIDR_CTRL_LIST_MAX;
	if (n < (ssize_t)(sizeof(*hdr) + (size_t)count * sizeof(*items)))
		return;
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
	if (count > MIDR_CTRL_LIST_MAX)
		count = MIDR_CTRL_LIST_MAX;
	if (n < (ssize_t)(sizeof(*hdr) + (size_t)count * sizeof(*items)))
		return;
	items = (const struct midr_ctrl_member_item *)(buf + sizeof(*hdr));

	/*
	 * ⑥ 先探后判：把成员表灌入 global_view、标记邻居（is_adjacent）并 I-1 探测，
	 * 但【不在此建连】。建连推迟到 CL 判定入群（JOIN）之后，由
	 * midr_nds_on_cluster_decision 的 JOIN 分支调 connect_group。
	 */
	for (i = 0; i < count; i++) {
		if (IPV4_ADDR_SAME(&items[i].rid, &bgp->router_id))
			continue; /* 跳过描述自己的条目 */

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

/* Read one control datagram and dispatch on its type. */
static void midr_ctrl_udp_recv(struct event *t)
{
	struct bgp *bgp = EVENT_ARG(t);
	struct bgp_midr *mi = bgp->midr_info;
	uint8_t buf[sizeof(struct midr_ctrl_list_hdr) +
		    MIDR_CTRL_LIST_MAX * sizeof(struct midr_ctrl_member_item)];
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
		if (n < (ssize_t)sizeof(msg))
			return;
		memcpy(&msg, buf, sizeof(msg));
		MIDR_FLOW_LOG("midr_ctrl: REP_LIST_REQ from %pI4 — replying with rep directory",
			  &msg.requester_transport);
		midr_ctrl_send_rep_list(mi, msg.requester_transport);
		break;
	case MIDR_CTRL_MEMBER_LIST_REQ: {
		uint32_t group;

		if (n < (ssize_t)sizeof(msg))
			return;
		memcpy(&msg, buf, sizeof(msg));
		group = ntohl(msg.target_group);

		/* Only a representative of this group answers. */
		if (!(mi->local_capabilities & MIDR_CAP_GROUP_REP) ||
		    group != mi->local_group_id) {
			MIDR_LOG("midr_ctrl: ignoring MEMBER_LIST_REQ for group %u (caps 0x%x our-group %u)",
				   group, mi->local_capabilities,
				   mi->local_group_id);
			return;
		}
		MIDR_FLOW_LOG("midr_ctrl: MEMBER_LIST_REQ for group %u from %pI4 — replying",
			  group, &msg.requester_transport);
		midr_ctrl_send_member_list(bgp, msg.requester_transport, group);
		break;
	}
	case MIDR_CTRL_REP_LIST_RESP:
		midr_ctrl_recv_rep_list(bgp, buf, n);
		break;
	case MIDR_CTRL_MEMBER_LIST_RESP:
		midr_ctrl_recv_member_list(bgp, buf, n);
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
	prefix2sockunion(&locator, &su);

	/*
	 * Dedup: a session toward this locator already exists.  This both
	 * avoids duplicating a peer and terminates the A<->B notify handshake
	 * — the side that receives the echoed PEER_REQUEST finds the peer it
	 * already created here and stops, so it sends no further notify.
	 */
	if (peer_lookup(bgp, &su))
		return;

	ret = peer_remote_as(bgp, &su, NULL, &asn, AS_EXTERNAL, NULL);
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
		midr_mark_topology(bgp, entry);
		count++;
	}

	return count;
}
