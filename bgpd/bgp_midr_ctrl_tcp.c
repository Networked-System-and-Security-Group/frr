// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR Control Layer —— TCP 传输层（列表交换）
 *
 * 承载 REP_LIST / MEMBER_LIST 请求-响应的 TCP 短连接（连上→请求→响应→立即
 * 关）。只管 "可靠地送出一帧、收回一帧"：连接生命周期、监听/接受、非阻塞连接、
 * 长度前缀分帧、超时、错误汇聚。连接对象（struct midr_ctrl_tcp_conn）是本文件
 * 私有，语义层（bgp_midr_ctrl.c）完全不可见——两层经 bgp_midr_ctrl.h 的
 * midr_ctrl_on_tcp_request / on_tcp_response 回调与 tcp_client_start / init /
 * finish 接口解耦。
 *
 * 为什么单独一个文件：TCP 连接管理与协议语义是两个关注点；决策
 * (docs/decisions/midr-preexchange-transport-udp-vs-tcp.md) 要求 "局限在控制层、
 * 不外溢到 BGP 建连"，拆紧邻的 _tcp.c 属控制层内部组织，不违此意图。
 *
 * PEER_REQUEST 仍走 UDP（bgp_midr_ctrl.c），不在本文件。
 *
 * 帧格式: [4B 长度前缀 (网络序，只计 payload)][payload = 原 UDP 报文字节]。
 *   请求 payload = 20B struct midr_ctrl_msg (严格等长)。
 *   响应 payload = struct midr_ctrl_list_hdr + count × item。
 *
 * 事件生命周期 / 防 UAF: 每条连接持 t_read/t_write/t_timeout 三事件, 同一轮
 * poll 可能双双就绪。统一关闭 conn_close() 先 event_cancel 三事件 (可摘除已就绪
 * 未执行的兄弟事件) 再 close+free; 每个回调走到 conn_close 后立即 return。
 * connect-check 回调入口先撤双 io 事件。这两道防线保证释放后的 conn 不被访问。
 */

#include "zebra.h"

#include "memory.h"
#include "frrevent.h"
#include "linklist.h"
#include "network.h" /* set_nonblocking, ERRNO_IO_RETRY */
#include "sockopt.h" /* sockopt_reuseaddr */
#include "sockunion.h"
#include "stream.h"
#include "prefix.h"
#include "log.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_midr_nds.h"
#include "bgpd/bgp_midr_ctrl.h"

DEFINE_MTYPE_STATIC(BGPD, MIDR_CTRL_TCP_CONN, "MIDR ctrl TCP connection");

#define MIDR_CTRL_TCP_TIMEOUT 5 /* 秒: 整条连接寿命上限 (故障保底, 正常触发不到) */
/* 收侧帧长上限: 由 u16 count 极值推导 (≈1MB), 防假长度大分配。用推导式不用魔数
 * —— 将来能力字段进 member 条目时上限自动跟着结构体走。 */
#define MIDR_CTRL_TCP_MAX_FRAME                                                 \
	(sizeof(struct midr_ctrl_list_hdr) +                                   \
	 65535UL * sizeof(struct midr_ctrl_member_item))
#define MIDR_CTRL_TCP_MAX_CONNS 128 /* 服务端并发上限, 防 fd 耗尽 */
#define MIDR_CTRL_TCP_IBUF_INIT 512 /* 客户端收缓冲初值 (响应到手后按帧长扩) */
#define MIDR_CTRL_TCP_PREFIX_LEN 4  /* 长度前缀字节数 */

enum midr_ctrl_conn_role {
	MIDR_CTRL_CONN_CLIENT, /* 主动发起: 发请求 → 收响应 */
	MIDR_CTRL_CONN_SERVER, /* 被 accept: 收请求 → 发响应 */
};

enum midr_ctrl_conn_state {
	MIDR_CTRL_CONN_CONNECTING, /* client: 非阻塞 connect 在途 */
	MIDR_CTRL_CONN_SENDING,	   /* 排空 obuf (client=请求 / server=响应) */
	MIDR_CTRL_CONN_READING,	   /* 收整帧 (client=响应 / server=请求) */
};

struct midr_ctrl_tcp_conn {
	struct bgp *bgp;
	int fd;
	enum midr_ctrl_conn_role role;
	enum midr_ctrl_conn_state state;
	struct ipaddr remote;	 /* client=请求目标 / server=accept 对端 (日志/去重键) */
	uint8_t type;		 /* client: 请求类型 (响应配对/在途去重) */
	uint32_t target_group;	 /* client: 请求携带的 group */
	struct stream *ibuf;	 /* server 固定 24B; client 512B 起, 按帧长 resize */
	struct stream *obuf;	 /* 可扩; 承载待发的整帧 (前缀+payload) */
	uint32_t frame_len;	 /* 已解析的 payload 长度; 0 = 前缀未解析 */
	struct event *t_read;
	struct event *t_write;
	struct event *t_timeout;
};

/* static 回调前置声明 */
static void midr_ctrl_tcp_read(struct event *t);
static void midr_ctrl_tcp_write(struct event *t);
static void midr_ctrl_tcp_timeout(struct event *t);
static void midr_ctrl_tcp_connect_check(struct event *t);
static void midr_ctrl_tcp_accept(struct event *t);
static void midr_ctrl_tcp_send_request(struct midr_ctrl_tcp_conn *conn);

/* ------------------------------------------------------------------ */
/* 连接生命周期                                                          */
/* ------------------------------------------------------------------ */

static struct midr_ctrl_tcp_conn *
midr_ctrl_tcp_conn_new(struct bgp *bgp, int fd, enum midr_ctrl_conn_role role)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct midr_ctrl_tcp_conn *conn;
	size_t ibuf_init;

	conn = XCALLOC(MTYPE_MIDR_CTRL_TCP_CONN, sizeof(*conn));
	conn->bgp = bgp;
	conn->fd = fd;
	conn->role = role;
	conn->frame_len = 0;

	/* server 只收一个 20B 请求帧, 收缓冲够装前缀+请求即可; client 响应变长, 起
	 * 步 512B, 解析出帧长后再 resize。 */
	ibuf_init = (role == MIDR_CTRL_CONN_SERVER)
			    ? (MIDR_CTRL_TCP_PREFIX_LEN +
			       sizeof(struct midr_ctrl_msg))
			    : MIDR_CTRL_TCP_IBUF_INIT;
	conn->ibuf = stream_new(ibuf_init);
	conn->obuf = stream_new_expandable(256);

	listnode_add(mi->ctrl_tcp_conns, conn);

	event_add_timer(bm->master, midr_ctrl_tcp_timeout, conn,
			MIDR_CTRL_TCP_TIMEOUT, &conn->t_timeout);
	return conn;
}

/* 唯一释放点: 撤三事件 → close(fd) → 出连接表 → 释放缓冲 → 释放对象。
 * 调用方在此之后必须立即 return (回调内不得再访问 conn)。 */
static void midr_ctrl_tcp_conn_close(struct midr_ctrl_tcp_conn *conn)
{
	struct bgp_midr_nds *mi = conn->bgp->midr_nds_info;

	event_cancel(&conn->t_read);
	event_cancel(&conn->t_write);
	event_cancel(&conn->t_timeout);

	if (conn->fd >= 0)
		close(conn->fd);

	if (mi && mi->ctrl_tcp_conns)
		listnode_delete(mi->ctrl_tcp_conns, conn);

	stream_free(conn->ibuf);
	stream_free(conn->obuf);
	XFREE(MTYPE_MIDR_CTRL_TCP_CONN, conn);
}

static void midr_ctrl_tcp_timeout(struct event *t)
{
	struct midr_ctrl_tcp_conn *conn = EVENT_ARG(t);

	MIDR_LOG("MIDR ctrl: TCP %s conn to %pIA timed out (%ds) — closing",
		 conn->role == MIDR_CTRL_CONN_CLIENT ? "client" : "server",
		 &conn->remote, MIDR_CTRL_TCP_TIMEOUT);
	midr_ctrl_tcp_conn_close(conn);
}

/* 转入发送态并挂写事件 (obuf 已装好整帧, getp=0)。 */
static void midr_ctrl_tcp_start_send(struct midr_ctrl_tcp_conn *conn)
{
	conn->state = MIDR_CTRL_CONN_SENDING;
	event_add_write(bm->master, midr_ctrl_tcp_write, conn, conn->fd,
			&conn->t_write);
}

static void midr_ctrl_tcp_want_read(struct midr_ctrl_tcp_conn *conn)
{
	event_add_read(bm->master, midr_ctrl_tcp_read, conn, conn->fd,
		       &conn->t_read);
}

/* ------------------------------------------------------------------ */
/* 分帧写: 排空 obuf, 处理部分写                                         */
/* ------------------------------------------------------------------ */

static void midr_ctrl_tcp_write(struct event *t)
{
	struct midr_ctrl_tcp_conn *conn = EVENT_ARG(t);
	int n;

	n = stream_flush(conn->obuf, conn->fd); /* write(fd, data+getp, endp-getp) */
	if (n < 0) {
		if (ERRNO_IO_RETRY(errno)) {
			event_add_write(bm->master, midr_ctrl_tcp_write, conn,
					conn->fd, &conn->t_write);
			return;
		}
		MIDR_LOG("MIDR ctrl: TCP write to %pIA failed: %s — closing",
			 &conn->remote, safe_strerror(errno));
		midr_ctrl_tcp_conn_close(conn);
		return;
	}
	if (n == 0) {
		/* 有数据待发却写出 0: 不应发生; 防死循环当致命。 */
		MIDR_LOG("MIDR ctrl: TCP write to %pIA returned 0 — closing",
			 &conn->remote);
		midr_ctrl_tcp_conn_close(conn);
		return;
	}

	stream_forward_getp(conn->obuf, n);
	if (STREAM_READABLE(conn->obuf) > 0) {
		/* 部分写, 续挂写事件。 */
		event_add_write(bm->master, midr_ctrl_tcp_write, conn, conn->fd,
				&conn->t_write);
		return;
	}

	/* 整帧发完。 */
	if (conn->role == MIDR_CTRL_CONN_CLIENT) {
		/* 请求已发 → 等响应。 */
		conn->state = MIDR_CTRL_CONN_READING;
		stream_reset(conn->ibuf);
		conn->frame_len = 0;
		midr_ctrl_tcp_want_read(conn);
	} else {
		/* server 响应发完 → 短连接使命完成, 关闭。 */
		midr_ctrl_tcp_conn_close(conn);
	}
}

/* ------------------------------------------------------------------ */
/* 分帧读: 先凑 4B 前缀, 校验帧长, (client) 扩缓冲, 再凑整帧               */
/* ------------------------------------------------------------------ */

static void midr_ctrl_tcp_read(struct event *t)
{
	struct midr_ctrl_tcp_conn *conn = EVENT_ARG(t);
	struct bgp *bgp = conn->bgp;
	const uint8_t *payload;
	size_t need, have, plen;
	ssize_t r;

	/* --- 阶段 1: 长度前缀 --- */
	if (conn->frame_len == 0) {
		have = stream_get_endp(conn->ibuf);
		if (have < MIDR_CTRL_TCP_PREFIX_LEN) {
			r = stream_read_try(conn->ibuf, conn->fd,
					    MIDR_CTRL_TCP_PREFIX_LEN - have);
			if (r == -2) {
				midr_ctrl_tcp_want_read(conn);
				return;
			}
			if (r <= 0) { /* 0=EOF, -1=fatal */
				midr_ctrl_tcp_conn_close(conn);
				return;
			}
			if (stream_get_endp(conn->ibuf) <
			    MIDR_CTRL_TCP_PREFIX_LEN) {
				midr_ctrl_tcp_want_read(conn);
				return;
			}
		}
		conn->frame_len = stream_getl(conn->ibuf); /* 消费 4B, getp→4 */

		/* 校验帧长 (在扩缓冲之前拦下非法值)。 */
		if (conn->role == MIDR_CTRL_CONN_SERVER) {
			if (conn->frame_len != sizeof(struct midr_ctrl_msg)) {
				MIDR_LOG("MIDR ctrl: TCP request from %pIA bad frame_len %u (want %zu) — closing",
					 &conn->remote, conn->frame_len,
					 sizeof(struct midr_ctrl_msg));
				midr_ctrl_tcp_conn_close(conn);
				return;
			}
		} else {
			if (conn->frame_len == 0 ||
			    conn->frame_len > MIDR_CTRL_TCP_MAX_FRAME) {
				MIDR_LOG("MIDR ctrl: TCP response from %pIA bad frame_len %u (max %zu) — closing",
					 &conn->remote, conn->frame_len,
					 (size_t)MIDR_CTRL_TCP_MAX_FRAME);
				midr_ctrl_tcp_conn_close(conn);
				return;
			}
		}

		need = MIDR_CTRL_TCP_PREFIX_LEN + conn->frame_len;
		if (stream_get_size(conn->ibuf) < need)
			stream_resize_inplace(&conn->ibuf, need);
	}

	/* --- 阶段 2: payload --- */
	need = MIDR_CTRL_TCP_PREFIX_LEN + conn->frame_len;
	have = stream_get_endp(conn->ibuf);
	if (have < need) {
		r = stream_read_try(conn->ibuf, conn->fd, need - have);
		if (r == -2) {
			midr_ctrl_tcp_want_read(conn);
			return;
		}
		if (r <= 0) {
			midr_ctrl_tcp_conn_close(conn);
			return;
		}
		if (stream_get_endp(conn->ibuf) < need) {
			midr_ctrl_tcp_want_read(conn);
			return;
		}
	}

	/* --- 整帧就绪, 交语义层 --- */
	payload = stream_pnt(conn->ibuf); /* data + getp(=4) */
	plen = conn->frame_len;

	if (conn->role == MIDR_CTRL_CONN_SERVER) {
		struct stream *resp;
		size_t resp_len;

		resp = midr_ctrl_on_tcp_request(bgp, payload, plen,
						conn->remote);
		if (!resp) {
			/* 闸门不过 / 协议错: 沉默关闭 (语义同 UDP 版)。 */
			midr_ctrl_tcp_conn_close(conn);
			return;
		}
		/* 封帧: 4B 前缀 + 响应 payload。obuf 初始 256B, 大响应须先扩够——
		 * raw stream_put 的 CHECK_SIZE 会先把写入截到当前余量再判断扩容,
		 * 即对可扩 stream 也不会真正增长 (FRR wart), 故显式 resize。 */
		resp_len = stream_get_endp(resp);
		stream_reset(conn->obuf);
		if (stream_get_size(conn->obuf) < MIDR_CTRL_TCP_PREFIX_LEN + resp_len)
			stream_resize_inplace(&conn->obuf,
					      MIDR_CTRL_TCP_PREFIX_LEN + resp_len);
		stream_putl(conn->obuf, resp_len);
		stream_put(conn->obuf, resp->data, resp_len);
		stream_free(resp);
		midr_ctrl_tcp_start_send(conn);
	} else {
		midr_ctrl_on_tcp_response(bgp, conn->type, payload, plen,
					  conn->remote);
		midr_ctrl_tcp_conn_close(conn);
	}
}

/* ------------------------------------------------------------------ */
/* 客户端: 非阻塞 connect + 发请求                                       */
/* ------------------------------------------------------------------ */

static void midr_ctrl_tcp_send_request(struct midr_ctrl_tcp_conn *conn)
{
	struct midr_ctrl_msg msg = {};

	midr_ctrl_fill_msg(conn->bgp, &msg, conn->type, conn->target_group);

	/* 帧总长 24B 恒小于 obuf 初始 256B, raw stream_put 的 CHECK_SIZE 截断
	 * (见 tcp_read 服务端封帧处注释) 在此永不触发。 */
	stream_reset(conn->obuf);
	stream_putl(conn->obuf, sizeof(msg));	   /* 4B 长度前缀 */
	stream_put(conn->obuf, &msg, sizeof(msg)); /* 20B payload */
	midr_ctrl_tcp_start_send(conn);
}

static void midr_ctrl_tcp_connect_check(struct event *t)
{
	struct midr_ctrl_tcp_conn *conn = EVENT_ARG(t);
	int status;
	socklen_t slen = sizeof(status);

	/* 入口先撤双 io 事件 (本回调其一已被 lib 置 NULL, 另一个在此摘除): 防 connect
	 * 检查期 read+write 同轮就绪造成的兄弟事件 UAF。 */
	event_cancel(&conn->t_read);
	event_cancel(&conn->t_write);

	if (getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &status, &slen) < 0) {
		MIDR_LOG("MIDR ctrl: TCP connect to %pIA getsockopt failed: %s — closing",
			 &conn->remote, safe_strerror(errno));
		midr_ctrl_tcp_conn_close(conn);
		return;
	}
	if (status != 0) {
		MIDR_LOG("MIDR ctrl: TCP connect to %pIA failed: %s — closing (等重试)",
			 &conn->remote, safe_strerror(status));
		midr_ctrl_tcp_conn_close(conn);
		return;
	}

	midr_ctrl_tcp_send_request(conn);
}

void midr_ctrl_tcp_client_start(struct bgp *bgp, struct ipaddr dst,
				uint8_t type, uint32_t target_group)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct listnode *node;
	struct midr_ctrl_tcp_conn *c;
	struct midr_ctrl_tcp_conn *conn;
	union sockunion su_dst;
	enum connect_result res;
	int fd;

	if (!mi || !mi->ctrl_tcp_conns)
		return;
	if (!IS_IPADDR_V4(&dst)) {
		MIDR_LOG("MIDR ctrl: protocol v3 cannot connect to %pIA", &dst);
		return;
	}

	/* 在途去重: 同 (dst,type) 已有 client 连接 —— group 相同则忽略, 不同 (join
	 * 中途换群) 则关旧起新。 */
	for (ALL_LIST_ELEMENTS_RO(mi->ctrl_tcp_conns, node, c)) {
		if (c->role != MIDR_CTRL_CONN_CLIENT)
			continue;
		if (!midr_ipaddr_same(&c->remote, &dst) || c->type != type)
			continue;
		if (c->target_group == target_group)
			return; /* 同请求在途, 去重 */
		midr_ctrl_tcp_conn_close(c); /* 换群: 关旧 */
		break;			     /* 列表已变, 退出遍历 */
	}

	fd = socket(ipaddr_family(&dst), SOCK_STREAM, 0);
	if (fd < 0) {
		zlog_warn("MIDR ctrl: TCP socket() failed: %s",
			  safe_strerror(errno));
		return;
	}
	set_nonblocking(fd);

	/* 绑源地址到 local_transport_addr (端口 0): TCP 响应沿连接原路回, 对端回包
	 * 目标 = 我们的源地址; 不绑则内核自动选接口地址, 多跳场景对端可能无回程路由
	 * (与 BGP 会话设 update-source loopback 同理)。绑定失败仅 warn, 软降级。 */
	if (mi->transport_addr_set) {
		union sockunion su_local;

		if (!midr_ipaddr_to_sockunion(&mi->local_transport_addr,
					     &su_local)) {
			close(fd);
			return;
		}
		if (sockunion_bind(fd, &su_local, 0, &su_local) < 0)
			MIDR_LOG("MIDR ctrl: TCP bind source %pIA failed: %s (软降级, 直连仍可用)",
				 &mi->local_transport_addr,
				 safe_strerror(errno));
	}

	if (!midr_ipaddr_to_sockunion(&dst, &su_dst)) {
		close(fd);
		return;
	}
	res = sockunion_connect(fd, &su_dst, htons(MIDR_CTRL_TCP_PORT));
	switch (res) {
	case connect_error:
		MIDR_LOG("MIDR ctrl: TCP connect to %pIA refused: %s (等重试)",
			 &dst, safe_strerror(errno));
		close(fd); /* conn 未建, pending 队列会稍后重试 */
		return;
	case connect_success:
		conn = midr_ctrl_tcp_conn_new(bgp, fd, MIDR_CTRL_CONN_CLIENT);
		conn->remote = dst;
		conn->type = type;
		conn->target_group = target_group;
		midr_ctrl_tcp_send_request(conn);
		return;
	case connect_in_progress:
		conn = midr_ctrl_tcp_conn_new(bgp, fd, MIDR_CTRL_CONN_CLIENT);
		conn->remote = dst;
		conn->type = type;
		conn->target_group = target_group;
		conn->state = MIDR_CTRL_CONN_CONNECTING;
		/* read+write 双挂同一 connect-check 回调 (poll 就绪即成/败)。 */
		event_add_read(bm->master, midr_ctrl_tcp_connect_check, conn,
			       fd, &conn->t_read);
		event_add_write(bm->master, midr_ctrl_tcp_connect_check, conn,
				fd, &conn->t_write);
		return;
	}
}

bool midr_ctrl_tcp_client_inflight(struct bgp_midr_nds *mi, struct ipaddr dst,
				   uint8_t type)
{
	struct listnode *node;
	struct midr_ctrl_tcp_conn *c;

	if (!mi || !mi->ctrl_tcp_conns)
		return false;

	for (ALL_LIST_ELEMENTS_RO(mi->ctrl_tcp_conns, node, c))
		if (c->role == MIDR_CTRL_CONN_CLIENT &&
		    midr_ipaddr_same(&c->remote, &dst) && c->type == type)
			return true;

	return false;
}

/* ------------------------------------------------------------------ */
/* 服务端: 监听 + 接受                                                   */
/* ------------------------------------------------------------------ */

static void midr_ctrl_tcp_accept(struct event *t)
{
	struct bgp *bgp = EVENT_ARG(t);
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct midr_ctrl_tcp_conn *conn;
	union sockunion su;
	int client_fd;

	/* 先重挂 accept 事件 (无论本次如何处理都继续监听)。 */
	event_add_read(bm->master, midr_ctrl_tcp_accept, bgp, mi->ctrl_tcp_lsock,
		       &mi->t_ctrl_tcp_accept);

	client_fd = sockunion_accept(mi->ctrl_tcp_lsock, &su);
	if (client_fd < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
			zlog_warn("MIDR ctrl: TCP accept failed: %s",
				  safe_strerror(errno));
		return;
	}
	set_nonblocking(client_fd);

	/* 并发上限: 引导节点被大量节点同时连时防 fd 耗尽。 */
	if (listcount(mi->ctrl_tcp_conns) >= MIDR_CTRL_TCP_MAX_CONNS) {
		MIDR_LOG("MIDR ctrl: TCP conns at cap %d — dropping accept",
			 MIDR_CTRL_TCP_MAX_CONNS);
		close(client_fd);
		return;
	}

	conn = midr_ctrl_tcp_conn_new(bgp, client_fd, MIDR_CTRL_CONN_SERVER);
	(void)midr_sockunion_to_ipaddr(&su, &conn->remote);
	conn->state = MIDR_CTRL_CONN_READING;
	midr_ctrl_tcp_want_read(conn);
}

static void midr_ctrl_tcp_listen(struct bgp *bgp)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct sockaddr_in sa = {};
	int sock;

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		zlog_err("MIDR ctrl: TCP socket() failed: %s (列表交换监听未开, 客户端侧不受影响)",
			 safe_strerror(errno));
		return;
	}
	sockopt_reuseaddr(sock);

	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port = htons(MIDR_CTRL_TCP_PORT);
	if (bind(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		zlog_err("MIDR ctrl: TCP bind(:%u) failed: %s",
			 MIDR_CTRL_TCP_PORT, safe_strerror(errno));
		close(sock);
		return;
	}
	if (listen(sock, SOMAXCONN) < 0) {
		zlog_err("MIDR ctrl: TCP listen(:%u) failed: %s",
			 MIDR_CTRL_TCP_PORT, safe_strerror(errno));
		close(sock);
		return;
	}
	set_nonblocking(sock);
	mi->ctrl_tcp_lsock = sock;
	event_add_read(bm->master, midr_ctrl_tcp_accept, bgp, sock,
		       &mi->t_ctrl_tcp_accept);

	MIDR_LOG("MIDR ctrl: TCP list-exchange channel on :%u",
		 MIDR_CTRL_TCP_PORT);
}

/* ------------------------------------------------------------------ */
/* init / finish                                                        */
/* ------------------------------------------------------------------ */

void midr_ctrl_tcp_init(struct bgp *bgp)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;

	if (!mi)
		return;

	/* 连接表先于监听无条件创建: 即便监听失败, 客户端侧仍可发起短连接。 */
	mi->ctrl_tcp_lsock = -1;
	mi->ctrl_tcp_conns = list_new();

	midr_ctrl_tcp_listen(bgp); /* 失败仅 warn */
}

void midr_ctrl_tcp_finish(struct bgp *bgp)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct listnode *node;

	if (!mi)
		return;

	event_cancel(&mi->t_ctrl_tcp_accept);
	if (mi->ctrl_tcp_lsock >= 0) {
		close(mi->ctrl_tcp_lsock);
		mi->ctrl_tcp_lsock = -1;
	}

	if (mi->ctrl_tcp_conns) {
		/* conn_close 会 listnode_delete 摘除本节点, 故不能普通迭代:
		 * 每次取表头关一个。 */
		while ((node = listhead(mi->ctrl_tcp_conns)) != NULL)
			midr_ctrl_tcp_conn_close(listgetdata(node));
		list_delete(&mi->ctrl_tcp_conns);
	}
}
