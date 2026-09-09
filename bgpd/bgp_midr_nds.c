// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR core (NDS - Neighbor Discovery & Selection).
 *
 * Owns bgp->midr_nds_info: the NDS global view and local lifecycle, consumes
 * authoritative remote-view callbacks, coordinates Control/PM/CL through the
 * internal interfaces, and publishes local topology facts through E-1.
 * Remote liveness follows BGP/session and explicit withdraw events; the old
 * NDS keepalive/expiry source is no longer authoritative.
 */

#include <zebra.h>

#include <math.h> /* fabs()：§8.21 去抖的丢包率绝对差比较 */

#include "memory.h"
#include "frrevent.h"
#include "monotime.h"
#include "prefix.h"
#include "linklist.h"
#include "log.h"
#include "jhash.h" /* 挂靠挑台的起点哈希（裸取模会让等间隔 rid 撞车） */

#include "bgpd/bgpd.h"
#include "bgpd/bgp_vty.h" /* bgp_config_inprocess()：挂靠钩子的配置期抑制（D1） */
#include "bgpd/bgp_ls.h"
#include "bgpd/bgp_ls_nlri.h"
#include "bgpd/bgp_midr_nds.h"
#include "bgpd/bgp_midr_nds_facts.h"
#include "bgpd/bgp_midr_ctrl.h"
#include "bgpd/bgp_midr_cl.h"
#include "bgpd/bgp_midr_pm.h"
#include "bgpd/bgp_midr_store.h"
#include "bgpd/bgp_debug.h"

/* §8.31 bootstrap 种子持久化参数 */
#define MIDR_STORE_SEED_KEEP	  32 /* 种子库最多留几条（防膨胀，prune 上限） */
#define MIDR_BOOTSTRAP_SELF_BOOT_SECS 10 /* 启动后延迟自举的等待秒数（等网络就绪） */

DEFINE_MTYPE_STATIC(BGPD, BGP_MIDR, "MIDR instance");
DEFINE_MTYPE_STATIC(BGPD, MIDR_GLOBAL_VIEW, "MIDR global view");
DEFINE_MTYPE_STATIC(BGPD, MIDR_NODE_ENTRY, "MIDR node entry");
DEFINE_MTYPE_STATIC(BGPD, MIDR_LINK_ENTRY, "MIDR link entry");
DEFINE_MTYPE_STATIC(BGPD, MIDR_REP_ENTRY, "MIDR rep directory entry");
DEFINE_MTYPE_STATIC(BGPD, MIDR_BOOTSTRAP_ENTRY, "MIDR bootstrap candidate");
DEFINE_MTYPE_STATIC(BGPD, MIDR_SESSION_EXCLUDE, "MIDR excluded session");
DEFINE_MTYPE_STATIC(BGPD, MIDR_SESSION_LEDGER, "MIDR session ledger entry");
DEFINE_MTYPE_STATIC(BGPD, MIDR_MANUAL_SESSION, "MIDR manual session config");
DEFINE_MTYPE_STATIC(BGPD, MIDR_ATTACH_DOWN, "MIDR attach down-pending");
DEFINE_MTYPE_STATIC(BGPD, MIDR_SESSION_DOWN, "MIDR session down-pending");

/* 前向声明：候选池插入（定义在本文件"§8.32 bootstrap 候选"一节，靠近
 * failover 机械件）。种子落库时顺手补插候选池（子稿 §4-1）需要在此之前调它。 */
static void midr_bootstrap_list_add(struct bgp_midr_nds *mi,
				    struct ipaddr addr,
				    as_t asn, struct in_addr rid,
				    enum midr_bootstrap_source source);
/* 同上：候选池按 transport 查条目（rid 反查第三级要用，定义在同一节）。 */
static struct midr_bootstrap_entry *
midr_bootstrap_find(struct bgp_midr_nds *mi, struct ipaddr addr,
		    struct listnode **node_out);
/* 同上：确保挂靠达标（定义在挂靠一节，紧挨 attach_count——它得先有定义）。
 * 能力位 setter 与 config_end 回调都要在此之前调它。 */
static void midr_nds_attach_ensure(struct bgp *bgp, const char *why);

/* ===========================================================================
 * Hash helpers (node table is keyed by node_id prefix)
 * =========================================================================*/

unsigned int midr_node_hash_key(const struct midr_node_entry *e)
{
	return prefix_hash_key(&e->node_id);
}

int midr_node_hash_cmp(const struct midr_node_entry *a,
		       const struct midr_node_entry *b)
{
	return prefix_cmp(&a->node_id, &b->node_id);
}

/* ===========================================================================
 * Small helpers
 * =========================================================================*/

/* Build a host prefix (AF_INET/32) from a bare IPv4 address. */
static void midr_prefix_from_in_addr(struct prefix *p, struct in_addr addr)
{
	memset(p, 0, sizeof(*p));
	p->family = AF_INET;
	p->prefixlen = IPV4_MAX_BITLEN;
	p->u.prefix4 = addr;
}

static bool midr_prefix_is_self(struct bgp *bgp, const struct prefix *p)
{
	return p->family == AF_INET &&
	       IPV4_ADDR_SAME(&p->u.prefix4, &bgp->router_id);
}

/* Resolve only the advertised reachable locator.  Router ID is identity and
 * must never silently become a socket destination or link endpoint. */
void midr_node_get_locator(const struct midr_node_entry *e, struct prefix *out)
{
	memset(out, 0, sizeof(*out));
	if (e && e->has_transport_addr &&
	    midr_ipaddr_valid_locator(&e->transport_addr))
		(void)midr_ipaddr_to_host_prefix(&e->transport_addr, out);
}

/*
 * §8.31：若该节点是"可作种子的引导节点"（BOOTSTRAP 位 + transport + asn 齐备），
 * 把它的 transport 地址写入种子库（有则刷 last_seen、无则插入）。非引导节点、
 * self、字段不全者一律跳过。调用点：收包侧的变化分支（学到/变了才写）、
 * periodic_sync 定时器（每 30s 顺路刷种子库的墙钟）。
 *
 * 当前专职引导不发布自身 Node fact，第二组远端视图也不保证回灌群 0 pending，
 * 因而种子表的正常活水来自 BOOTSTRAP_LIST 收包侧
 *（midr_nds_bootstrap_learn）。本辅助路径仅在将来的权威远端视图确实提供一条
 * BOOTSTRAP 能力事实时作为兼容补充。
 */
static void midr_maybe_save_bootstrap_seed(struct bgp *bgp,
					   const struct midr_node_entry *entry)
{
	char buf[IPADDR_STRING_SIZE];
	char ridbuf[INET_ADDRSTRLEN];
	struct in_addr rid;

	if (entry->is_self)
		return;
	if (!(entry->capabilities & MIDR_CAP_BOOTSTRAP))
		return;
	/* transport 必需（种子就是"重启后往这个地址建连"）；asn 不必需，存 0 即可。 */
	if (!entry->has_transport_addr)
		return;
	/* 真名取节点表条目的键（node_id）。非 IPv4 键理论上不该出现在本路（Node
	 * NLRI 恒以 IPv4 router-id 为键），保险起见跳过而不是塞个 0 进池。 */
	if (entry->node_id.family != AF_INET)
		return;
	rid = entry->node_id.u.prefix4;

	snprintfrr(buf, sizeof(buf), "%pIA", &entry->transport_addr);
	snprintfrr(ridbuf, sizeof(ridbuf), "%pI4", &rid);
	/*
	 * ⚠ 两个时间戳时钟不同、绝不可互相赋值（本函数两个都摸得到）：节点表
	 * `entry->last_update` 是 monotime（内存态、纯观测）；种子表这一列是
	 * **墙钟** time(NULL)（持久化，跨重启要可比）。原先这里误用 monotime——开机秒数跨宿主重启归零，与持久
	 * 化表矛盾：LRU 反向淘汰（旧库的大数值反成"最新"）、加载序颠倒、"距今"
	 * 可为负。墙钟靠本机 RTC 即可，纯本地语义、从不跨节点比较（子稿 §4-5）。
	 */
	midr_store_seed_save(buf, (uint32_t)entry->asn, ridbuf, time(NULL));

	/* 子稿 §4-1：落库的同时补进内存候选池，修"运行期新认识的引导进不了
	 * bootstrap_list"的缺口——否则它只有等下次重启读种子才成为可试候选。
	 * 去重键 = transport，已在池中则只刷 ASN（MANUAL 条目不会被降级）。
	 * rid 取节点表条目的 node_id；只有权威远端视图明确提供该 BOOTSTRAP
	 * 身份时本兼容路径才会触发。 */
	midr_bootstrap_list_add(bgp->midr_nds_info, entry->transport_addr,
				entry->asn, rid, MIDR_BOOTSTRAP_SEED);
}

/* ===========================================================================
 * Global view lifecycle
 * =========================================================================*/

static struct midr_global_view *midr_global_view_new(void)
{
	struct midr_global_view *gv;

	gv = XCALLOC(MTYPE_MIDR_GLOBAL_VIEW, sizeof(*gv));
	midr_node_hash_init(&gv->nodes);
	gv->links = list_new();
	gv->groups = NULL; /* reserved; CL currently derives groups from nodes */

	return gv;
}

static void midr_global_view_free(struct midr_global_view *gv)
{
	struct midr_node_entry *entry;
	struct listnode *node, *nnode;
	struct midr_link_entry *link;

	if (!gv)
		return;

	frr_each_safe (midr_node_hash, &gv->nodes, entry) {
		midr_node_hash_del(&gv->nodes, entry);
		XFREE(MTYPE_MIDR_NODE_ENTRY, entry);
	}
	midr_node_hash_fini(&gv->nodes);

	for (ALL_LIST_ELEMENTS(gv->links, node, nnode, link))
		XFREE(MTYPE_MIDR_LINK_ENTRY, link);
	list_delete(&gv->links);

	XFREE(MTYPE_MIDR_GLOBAL_VIEW, gv);
}

/*
 * CL 特供 getter：滤掉群号 0 的节点（引导 + 尚未入群的），CL 读节点表只走这里。
 *
 * 判据取群号而非 BOOTSTRAP 能力位：能力位在 TLV 1187、带 seqno 后到，条目可能
 * 先进表后补位；群号随 NLRI 一起到，没有这个空窗。
 * 返回值恒非 NULL（无条目就是空表），调用方遍历完 list_delete()——元素是节点表
 * 条目的**借用指针**，只释放链表本身。
 */
struct list *midr_nds_cl_nodes_getter(const struct midr_global_view *gv)
{
	struct list *out = list_new();
	struct midr_node_entry *entry;

	if (!gv)
		return out;

	frr_each (midr_node_hash, (struct midr_node_hash_head *)&gv->nodes,
		  entry) {
		if (entry->group_id == 0)
			continue;
		listnode_add(out, entry);
	}

	return out;
}

static struct midr_link_entry *
midr_global_view_find_link(struct midr_global_view *gv,
			   const struct prefix *node_id)
{
	struct listnode *node;
	struct midr_link_entry *link;

	for (ALL_LIST_ELEMENTS_RO(gv->links, node, link))
		if (prefix_same(&link->remote_node_id, node_id))
			return link;

	return NULL;
}

/*
 * 按键删除一条 link_entry（命中返回 true）。links 表此前只增不减：node 下线
 * 只删 node 表却漏删对应 link，仅探未连的节点探测后也残留——孤儿长期堆积。
 * MTYPE_MIDR_LINK_ENTRY 是本文件 DEFINE_MTYPE_STATIC，释放只能在这里做。
 */
static bool midr_global_view_del_link(struct midr_global_view *gv,
				      const struct prefix *key)
{
	struct listnode *node, *nnode;
	struct midr_link_entry *link;

	for (ALL_LIST_ELEMENTS(gv->links, node, nnode, link))
		if (prefix_same(&link->remote_node_id, key)) {
			list_delete_node(gv->links, node);
			XFREE(MTYPE_MIDR_LINK_ENTRY, link);
			return true;
		}
	return false;
}

/* ===========================================================================
 * `no midr session` 持久排除名单
 *
 * 键 = **router-id（真名）**，2026-08-11（保底轮 2 批 2，Q2）由 locator/地址换来。
 * 排除的语义是"这个**人**别再连"，而节点身份是 router-id；原先按地址为键，运维
 * 改了对端 transport 之后拉黑就静默失效（旧记档 F5）。台账记"边"用 transport、
 * 排除名单记"人"用 router-id——两张表两义两键，各取所长。
 * =========================================================================*/

bool midr_nds_is_session_excluded(struct bgp *bgp, struct in_addr rid)
{
	struct bgp_midr_nds *mi;
	struct listnode *node;
	struct in_addr *a;

	if (!bgp || !bgp->midr_nds_info || !bgp->midr_nds_info->session_blacklist)
		return false;
	mi = bgp->midr_nds_info;

	for (ALL_LIST_ELEMENTS_RO(mi->session_blacklist, node, a))
		if (a->s_addr == rid.s_addr)
			return true;
	return false;
}

/* 名单里有没有东西。只给 VTY 用：`midr session` 解析不出 rid、解除不了排除时，
 * 靠它决定要不要提醒运维——名单本来就是空的就别拿告警吓人。 */
bool midr_nds_session_blacklist_nonempty(struct bgp *bgp)
{
	if (!bgp || !bgp->midr_nds_info || !bgp->midr_nds_info->session_blacklist)
		return false;
	return !list_isempty(bgp->midr_nds_info->session_blacklist);
}

void midr_nds_session_exclude_add(struct bgp *bgp, struct in_addr rid)
{
	struct bgp_midr_nds *mi;
	struct in_addr *a;

	if (!bgp || !bgp->midr_nds_info || !bgp->midr_nds_info->session_blacklist)
		return;
	mi = bgp->midr_nds_info;

	if (midr_nds_is_session_excluded(bgp, rid))
		return; /* 已在名单，幂等 */

	a = XMALLOC(MTYPE_MIDR_SESSION_EXCLUDE, sizeof(*a));
	*a = rid;
	listnode_add(mi->session_blacklist, a);
	MIDR_FLOW_LOG("MIDR 会话排除：router-id %pI4 加入排除名单（持久排除，不再自动重连）",
		      &rid);
}

void midr_nds_session_exclude_del(struct bgp *bgp, struct in_addr rid)
{
	struct bgp_midr_nds *mi;
	struct listnode *node;
	struct in_addr *a;

	if (!bgp || !bgp->midr_nds_info || !bgp->midr_nds_info->session_blacklist)
		return;
	mi = bgp->midr_nds_info;

	for (ALL_LIST_ELEMENTS_RO(mi->session_blacklist, node, a))
		if (a->s_addr == rid.s_addr) {
			listnode_delete(mi->session_blacklist, a);
			XFREE(MTYPE_MIDR_SESSION_EXCLUDE, a);
			MIDR_FLOW_LOG("MIDR 会话排除：router-id %pI4 移出排除名单（运维手工 midr session 显式覆盖）",
				      &rid);
			return;
		}
}

/* ===========================================================================
 * 会话台账（结论 20）——每条 MIDR 会话记一笔"它为什么存在"
 *
 * 与上面排除名单是两张表、两个语义、两种键：账记"边"（键 transport，随会话
 * 生灭），单记"人"（键 router-id，跨会话存续）。设计理由见 bgp_midr_nds.h 里
 * struct midr_session_ledger_entry 的头注释。
 * =========================================================================*/

const char *midr_session_reason_str(enum midr_session_reason reason)
{
	switch (reason) {
	case MIDR_SESSION_SAME_GROUP:
		return "SAME_GROUP";
	case MIDR_SESSION_ATTACH:
		return "ATTACH";
	case MIDR_SESSION_CL_ANCHOR:
		return "CL_ANCHOR";
	case MIDR_SESSION_MANUAL:
		return "MANUAL";
	case MIDR_SESSION_PEER_REQ_REPLY:
		return "PEER_REQ_REPLY";
	}
	return "UNKNOWN";
}

static struct midr_session_ledger_entry *
midr_ledger_find(struct bgp_midr_nds *mi, struct ipaddr transport)
{
	struct listnode *node;
	struct midr_session_ledger_entry *e;

	for (ALL_LIST_ELEMENTS_RO(mi->session_ledger, node, e))
		if (midr_ipaddr_same(&e->transport, &transport))
			return e;
	return NULL;
}

const struct midr_session_ledger_entry *
midr_nds_ledger_lookup(struct bgp *bgp, struct ipaddr transport)
{
	if (!bgp || !bgp->midr_nds_info || !bgp->midr_nds_info->session_ledger)
		return NULL;
	return midr_ledger_find(bgp->midr_nds_info, transport);
}

void midr_nds_ledger_note(struct bgp *bgp, struct ipaddr transport,
			  enum midr_session_reason reason,
			  struct in_addr remote_rid, as_t remote_asn,
			  uint32_t remote_group)
{
	struct bgp_midr_nds *mi;
	struct midr_session_ledger_entry *e;

	if (!bgp || !bgp->midr_nds_info || !bgp->midr_nds_info->session_ledger)
		return;
	if (!midr_ipaddr_valid_locator(&transport))
		return; /* 无键可记（不该发生，防御） */
	mi = bgp->midr_nds_info;

	e = midr_ledger_find(mi, transport);
	if (e) {
		/*
		 * 原因改写规则（事实字段 rid/群号照更新，粘的只是原因）：MANUAL
		 * 覆盖一切且不被覆盖——运维的边被改写，自动拆除判据就把它当自动边；
		 * 其余一律不改写既有账——账记的是这条边最初为什么要。
		 * 原先只挡 PEER_REQ_REPLY，于是锚点边一进本群就被改成 SAME_GROUP，
		 * midr_nds_edge_is_anchor() 再也查不到、锚点豁免整个失效（实测轨迹
		 * CL_ANCHOR -> SAME_GROUP -> PEER_REQ_REPLY）。认错方向安全：顶多
		 * 多留一条边，不会误拆。
		 */
		if (reason == MIDR_SESSION_MANUAL)
			e->reason = reason;
		if (remote_rid.s_addr != INADDR_ANY)
			e->remote_rid = remote_rid;
		if (remote_asn)
			e->remote_asn = remote_asn;
		if (remote_group)
			e->remote_group = remote_group;
		return;
	}

	e = XCALLOC(MTYPE_MIDR_SESSION_LEDGER, sizeof(*e));
	e->transport = transport;
	e->reason = reason;
	e->remote_rid = remote_rid;
	e->remote_asn = remote_asn;
	e->remote_group = remote_group;
	listnode_add(mi->session_ledger, e);

	MIDR_FLOW_LOG("MIDR 台账：登记 %pIA 原因=%s rid=%pI4 群=%u", &transport,
		      midr_session_reason_str(reason), &remote_rid,
		      remote_group);
}

void midr_nds_ledger_drop(struct bgp *bgp, struct ipaddr transport)
{
	struct bgp_midr_nds *mi;
	struct listnode *node, *nnode;
	struct midr_session_ledger_entry *e;

	if (!bgp || !bgp->midr_nds_info || !bgp->midr_nds_info->session_ledger)
		return;
	mi = bgp->midr_nds_info;

	for (ALL_LIST_ELEMENTS(mi->session_ledger, node, nnode, e)) {
		if (!midr_ipaddr_same(&e->transport, &transport))
			continue;
		MIDR_FLOW_LOG("MIDR 台账：销账 %pIA（原因=%s）", &transport,
			      midr_session_reason_str(e->reason));
		list_delete_node(mi->session_ledger, node);
		XFREE(MTYPE_MIDR_SESSION_LEDGER, e);
		return;
	}
}

/*
 * B1 断连老化（批 5c）——起表 / 停表 / 超龄扫描三件。
 *
 * 补的是节点表 expire 够不着的那一族："节点还在、只是这条边断了"（对端单侧排
 * 除我之后我这边的残留半边、群代表卸任后引导侧的回配半边），以及"对端压根不在
 * 节点表里"的残留。expire 按"节点"判，这里按"边"判，两条路互补。
 *
 * MANUAL 豁免（批 4 的 α 同一条规矩、两个生效点）：手配的边不计时、不进扫描，
 * 只在掉线那一刻 warn 一句——手配边没有任何自动机制会来拆，不吭这一声就是全网
 * 静默，运维只能自己去翻 show midr neighbors 才发现。
 */
static void midr_ledger_note_down(struct bgp *bgp, struct ipaddr transport)
{
	struct midr_session_ledger_entry *e;

	if (!bgp->midr_nds_info || !bgp->midr_nds_info->session_ledger)
		return;
	e = midr_ledger_find(bgp->midr_nds_info, transport);
	if (!e)
		return; /* 无账的会话不归 MIDR 管 */

	if (e->reason == MIDR_SESSION_MANUAL) {
		zlog_warn("MIDR 台账：手配会话 %pIA 断开，不做自动老化；要拆请用 no midr session %pIA",
			  &transport, &transport);
		return;
	}

	if (e->down_since)
		return; /* 已在计时，重复的掉线沿不刷新起点 */

	e->down_since = monotime(NULL);
	MIDR_LOG("MIDR 台账：%pIA（原因=%s）掉出 Established，起断连计时",
		 &transport, midr_session_reason_str(e->reason));
}

static void midr_ledger_note_up(struct bgp *bgp, struct ipaddr transport)
{
	struct midr_session_ledger_entry *e;

	if (!bgp->midr_nds_info || !bgp->midr_nds_info->session_ledger)
		return;
	e = midr_ledger_find(bgp->midr_nds_info, transport);
	if (!e || !e->down_since)
		return;

	e->down_since = 0;
	MIDR_LOG("MIDR 台账：%pIA 恢复 Established，断连计时清零", &transport);
}

/*
 * 超龄扫描，挂在 periodic_sync（30s 一拍）。
 *
 * ⚠ 不能边遍历边拆：midr_ctrl_detach_transport() 内部要销账，销的正是脚下这张
 * 链表的当前节点——节点内存已还给系统，遍历指针再读它的 next 就是读已释放内存
 * （轻则乱跳、重则 bgpd 崩）。故遍历只把地址记进栈上定长小数组，遍历完再逐条
 * 拆；一拍最多 MIDR_AGE_SCAN_MAX 条，多的下一拍（要撞上限得有 17 条会话在同一
 * 个 5 分钟窗口里全断且都没恢复）。
 */
static void midr_nds_ledger_age_scan(struct bgp *bgp)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct listnode *node;
	struct midr_session_ledger_entry *e;
	struct {
		struct ipaddr transport;
		struct in_addr rid;
		enum midr_session_reason reason;
	} aged[MIDR_AGE_SCAN_MAX];
	time_t now = monotime(NULL);
	int n = 0, i;

	if (!mi || !mi->session_ledger)
		return;

	for (ALL_LIST_ELEMENTS_RO(mi->session_ledger, node, e)) {
		if (!e->down_since)
			continue;
		if (now - e->down_since < MIDR_SESSION_DOWN_AGE)
			continue;

		aged[n].transport = e->transport;
		aged[n].rid = e->remote_rid;
		aged[n].reason = e->reason;
		if (++n == MIDR_AGE_SCAN_MAX)
			break;
	}

	for (i = 0; i < n; i++) {
		zlog_info("MIDR 台账：%pIA（原因=%s）断连已超 %d 秒，拆除残留半边并销账",
			  &aged[i].transport,
			  midr_session_reason_str(aged[i].reason),
			  MIDR_SESSION_DOWN_AGE);
		midr_ctrl_detach_transport(bgp, aged[i].transport,
					   aged[i].rid, false,
					   MIDR_STOP_SESSION_DOWN);
	}

	if (n == MIDR_AGE_SCAN_MAX)
		MIDR_LOG("MIDR 台账：本拍超龄条目已达上限 %d，其余留下一拍",
			 MIDR_AGE_SCAN_MAX);
}

/* ===========================================================================
 * NDS node table
 * =========================================================================*/

/*
 * 判定一个【发现阶段】学到的节点是否属于"本节点应与之建会话"的集合。
 *
 * 这是派生函数、不是决策函数（责任划分见 docs/decisions/
 * midr-connect-responsibility.md）：会话图的用途是 MIDR-LS 传播连通、不是质量
 * 优选，故此处不读也不该读性能指标——带"选"的判定（要不要跨群持续联络、
 * 联络哪些群、按什么指标取舍）归 CL，经 I-7 决策词下达集合级意图，本函数
 * 只把落定的集合翻译成"连不连"。
 *
 * 目标形态 = 本群成员 ∪ 保底对象 ∪ CL 下达的联络集合，随外部输入分三步落地：
 *   ① 本群成员    —— 即下面这一行（membership 由 CL 经 I-7 定，此处纯派生）
 *   ② 保底对象    —— 群间防孤岛的结构规则，形态待定（主文档 §8「群间保底连接」）
 *   ③ CL 联络集合 —— 待 I-7 加决策词、CL 认领后接
 * ②③ 未落地前不预置空分支（判据没出来，写了也是猜）。
 */
bool midr_discovery_should_peer(struct bgp *bgp,
				const struct midr_node_entry *entry)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct ipaddr local;

	if (!mi || !entry->has_transport_addr ||
	    !midr_ipaddr_valid_locator(&entry->transport_addr) ||
	    !midr_nds_local_transport_get(bgp, &local) ||
	    ipaddr_family(&local) != ipaddr_family(&entry->transport_addr) ||
	    !midr_nds_locator_unique(bgp, &entry->node_id,
				      &entry->transport_addr))
		return false;

	/*
	 * 自己不是自己的邻居。三个老调用点都在外面先挡了 is_self（收包侧
	 * on_node_nlri 在 `if (!entry->is_self)` 块内、reconverge 拆旧群的循环头
	 * 跳过 self、粗筛 midr_discovery_filter 已随探测 A 删除），故这道判据原先
	 * "看起来"在函数体
	 * 里——08-11 批 2 让 connect_group 改为逐成员过本函数时露了馅：那条路
	 * 没有外部 is_self 前置，本机于是给自己建了条会话、还朝自己发 PEER_REQUEST
	 * （实测：`建连 6 个成员`——群里明明只有 5 台）。判据收进函数体，"统一入口"
	 * 这句才真成立，将来新调用点也不必各自记得再挡一次。
	 */
	if (entry->is_self)
		return false;

	if (entry->group_id == 0 || entry->group_id != mi->local_group_id)
		return false;

	/* 运维 `no midr session` 持久排除的节点，哪怕仍同群，也不再自动纳入
	 * 邻居——否则下一次发现/重收敛会把它悄悄连回来，运维命令形同虚设。
	 * 键 = router-id（08-11 批 2 由 locator 换来，见排除名单节头注释）。 */
	if (entry->node_id.family == AF_INET &&
	    midr_nds_is_session_excluded(bgp, entry->node_id.u.prefix4)) {
		/*
		 * 留痕（08-18 新增）：排除是运维显式下的指令，它在**群内这条路**
		 * 生效时原先一点痕迹都不留——`midr_ctrl_connect()` 里那条"跳过自动
		 * 建连"只覆盖不过本函数的路径（群间 ANCHOR / 骨干 / 挂靠），而群内
		 * 重收敛在本函数就被静默滤掉了，运维与排查都只能靠"会话没建成"反推。
		 *
		 * 级别 info（运维不必动手，但该看得见自己的拉黑在起作用）。
		 * ⚠ 频率：件②（轮 4）删掉自有收包路径后，本函数由第二组的
		 * remote_node_update 回调驱动，其语义本就是事件驱动的——原先"被排除的
		 * 节点只要还活着就每 5s 一条"（MIDR keepalive 每 5s 重发 Node NLRI 撑
		 * 出来的频率）已随之消失。正常运行时被排除节点数为 0，本条不打。
		 */
		zlog_info("MIDR 会话排除：router-id %pI4 在排除名单中，群内自动建连跳过它（运维 no midr session 生效中）",
			  &entry->node_id.u.prefix4);
		return false;
	}

	return true;
}

/*
 * 解除与一个节点的探测/邻居关系：I-2 停探 + 清残留 link_entry + 清 is_adjacent，
 * 可选拆掉动态会话。节点条目本身的去留由调用方决定（本函数不动 node 表）。
 *
 * 双键清理：历史路径可能按 locator 建 link，而 PM 周期 loop 按
 * node_id(router-id) 回灌又建一条——同一节点最多两条 link，两个键都删。
 * locator 缺失时仍必须删 node_id 键；del_link 未命中返回 false，故天然幂等。
 */
/*
 * 这条边是不是 CL 下达的群间锚点边（台账 reason == CL_ANCHOR）。
 *
 * 用途：换群/离群的自动清理路径据此**降级处理**——只注销"本群邻居"身份
 * （is_adjacent 照清），保住会话与探测。判据用**台账**而非 CL 的 evidence
 * 清单：evidence 是评估期的临时数据、决策执行完就清零，台账才是常驻真相。
 *
 * ⚠⚠ **2026-08-19 实测：当前架构下本判据不可能命中，下面两处调用是死代码**
 * （对接轮 2 验证，backbone 台子 m2a 群 2→1→2 实验）。两条腿都断：
 *   ① **纯锚点边不带 is_adjacent** —— I-7 的 ANCHOR 分支只调 midr_ctrl_connect，
 *      全程不置该标记；而清理路径（对端改组、换群拆旧群）的判据前提正是
 *      is_adjacent，所以它们**本来就扫不到锚点边**，"误清 anchor"这件事在
 *      当前代码里不成立；
 *   ② **一旦带上 is_adjacent**（对端曾进过本群、被 connect_group 建连），
 *      台账 reason 已被覆盖成 SAME_GROUP —— 家规②只给 MANUAL 粘性，
 *      CL_ANCHOR 会被后续自动流程覆盖，于是这里查不到。
 *      实测轨迹：Origin 一路 CL_ANCHOR -> SAME_GROUP -> PEER_REQ_REPLY。
 *
 * 之所以**留着不删**：由此暴露的"锚点边变成同群边之后算什么"是个语义空洞
 * （运维改群号会拆掉 CL 建的锚点、且不会自动恢复），归 CL owner，待与 zhc
 * 讨论后再定这两处的去留。全案见 docs/记档不做清单.md 第 33 条。
 *
 * ⚠ 只给"换群/离群"这类**身份变化**路径用。节点真死的路（收到 withdraw、
 * expire 判死）不豁免——锚点死了就该拆。
 */
static bool midr_nds_edge_is_anchor(struct bgp *bgp, uint32_t remote_rid)
{
	struct bgp_midr_nds *mi;
	struct midr_session_ledger_entry *led;
	struct listnode *node;

	if (!bgp || !bgp->midr_nds_info || !remote_rid)
		return false;

	mi = bgp->midr_nds_info;
	if (!mi->session_ledger)
		return false;

	for (ALL_LIST_ELEMENTS_RO(mi->session_ledger, node, led))
		if (led->remote_rid.s_addr == remote_rid &&
		    led->reason == MIDR_SESSION_CL_ANCHOR)
			return true;

	return false;
}

static void midr_nds_detach_node(struct bgp *bgp, struct midr_node_entry *entry,
				 enum midr_stop_reason reason,
				 bool teardown_session)
{
	struct midr_global_view *gv = bgp->midr_nds_info->global_view;
	struct prefix locator;

	midr_node_get_locator(entry, &locator);
	/* I-2 停探。键必须与 I-1 配对：add 侧按 node_id 建探测 ctx，故删也统一
	 * 按 node_id——按 locator 删
	 * 会在 transport≠router-id 的节点上找不到 ctx → 探测泄漏。 */
	midr_pm_remove_target(bgp, &entry->node_id, reason);
	/*
	 * 轮 2：链路事实随之作废 —— 报过就发 link_withdraw；事实表保留 version
	 * 墓碑，防止同一稳定 identity 恢复后从低版本重报被第二组丢弃。
	 * 这是「链路生死归会话/节点级」的**清理路径**那一半（另一半是轮 4/5 要挂
	 * 的 peer_status_changed 钩子）；指标层已不再有任何 withdraw 触发点。
	 * 不做的话事实表只增不减，轮 3 的 snapshot 会把早就拆掉的链路重报一遍。
	 */
	midr_nds_report_link_withdraw(bgp, &entry->node_id);
	/* node_id is the canonical PM/fact key and must always be retired, even
	 * when the node has no usable locator.  Remove a distinct historical
	 * locator key as well. */
	midr_global_view_del_link(gv, &entry->node_id);
	if ((locator.family == AF_INET || locator.family == AF_INET6) &&
	    !prefix_same(&locator, &entry->node_id))
		midr_global_view_del_link(gv, &locator);
	entry->is_adjacent = false;
	if (teardown_session)
		midr_ctrl_on_node_remove(bgp, entry);
}

/*
 * 收包侧反应：同群且还没纳入邻居就建边。发现链专题（08-21）在此只删了**无差别
 * 起探**那一半（探测改由 connect 的 SAME_GROUP 分支只对同群起），建连保留——
 * connect_group 只连落定那刻表里已有的成员，两台成员 join 窗口一重叠就互相错过、
 * 落定后无人补（m1a↔m1b 双向零会话实测，见 轮3/plan-同群补边.md）。
 */
static void midr_nds_on_node_discovered(struct bgp *bgp,
					struct midr_node_entry *entry)
{
	if (entry->is_adjacent || !midr_discovery_should_peer(bgp, entry))
		return;

	MIDR_FLOW_LOG("MIDR 发现：节点 %pFX 群 %u -> 同群未邻接，建边",
		      &entry->node_id, entry->group_id);
	midr_ctrl_connect(bgp, entry, MIDR_SESSION_SAME_GROUP, true);
}

/*
 * 〔件②（轮 4）删除收包入口 midr_nds_on_node_nlri()：远端 Node NLRI 不再进
 * 节点表。同一张表现在只有一个数据源 —— 第二组的 remote_node_update 回调
 * （见本文件末尾的回调壳），字段解包在那边做，之后同样汇进下面的
 * midr_nds_node_react()。原先 on_node_nlri 开头那道退网守卫也已随之搬进回调壳。〕
 */


/*
 * 节点表反应链。权威数据源是第二组 remote_node_update 回调；Control
 * list/request 的临时学习入口也复用其中的邻接动作。本函数只管"表变了之后做什么"。
 *
 * 已知节点的 keepalive 刷新（既非 new 也非 changed）只更新 last_seen，不进这里，
 * 所以反应链不会每 5s 重跑一遍。自己的回声（is_self）同样跳过。
 */
void midr_nds_node_react(struct bgp *bgp, struct midr_node_entry *entry,
			 bool is_new, bool changed, bool group_changed,
			 uint32_t prev_gid)
{
	if (!entry->is_self) {
		/* §8.31：学到/变更了引导节点就记一笔种子（keepalive 刷新不触发，
		 * 因为下面两分支只在 is_new/changed 进入）。 */
		if (is_new || changed)
			midr_maybe_save_bootstrap_seed(bgp, entry);
		/* A node that has become a bootstrap/backbone endpoint is no longer
		 * eligible for topology Link facts.  Retire any fact from its former
		 * role immediately instead of waiting for a metric tick or resync. */
		if ((is_new || changed) &&
		    midr_nds_link_is_backbone(bgp, &entry->node_id))
			midr_nds_report_link_withdraw(bgp, &entry->node_id);

		if (is_new) {
			/* 同群就补边（见函数头）；随后统一告知 CL 视图变了。 */
			midr_nds_on_node_discovered(bgp, entry);
			midr_nds_notify_cl(bgp, MIDR_TRIGGER_NODE_CHANGE);
		} else if (changed) {
			/*
			 * 对端改组对称反应：远端把自己的群号改了，本地动态会话要
			 * 跟着重收敛，不必等 CL。判据复用 should_peer（本群且群号非 0）。
			 */
			if (group_changed) {
				struct bgp_midr_nds *mi = bgp->midr_nds_info;
				bool was_mine = (prev_gid != 0 &&
						 prev_gid == mi->local_group_id);
				bool now_mine =
					midr_discovery_should_peer(bgp, entry);

				if (was_mine && !now_mine && entry->is_adjacent) {
					/*
					 * 原本同群、现已离本群：拆掉动态会话。
					 *
					 * **锚点边豁免（降级不跳过）**：对端若同时是 CL 选中的
					 * 群间锚点，只把"本群邻居"这个身份注销（is_adjacent
					 * 照实清 —— 它确实不在本群了，不清的话往后每次换群都
					 * 要再撞一遍），但**保住会话与探测**——那条边是 CL 按
					 * 跨群链路质量建的，本就不该随群号变化而消失。
					 */
					if (midr_nds_edge_is_anchor(
						    bgp,
						    entry->node_id.u.prefix4
							    .s_addr)) {
						entry->is_adjacent = false;
						zlog_info("MIDR 对端改组：%pFX 群 %u -> %u（离本群），锚点边保留会话与探测",
							  &entry->node_id,
							  prev_gid,
							  entry->group_id);
					} else {
						midr_nds_detach_node(
							bgp, entry,
							MIDR_STOP_CLUSTER_CHANGE,
							true);
						zlog_info("MIDR 对端改组：%pFX 群 %u -> %u（离本群），拆会话",
							  &entry->node_id,
							  prev_gid,
							  entry->group_id);
					}
				} else if (now_mine && !entry->is_adjacent) {
					/* 现进本群、尚未邻接：建边。 */
					midr_nds_on_node_discovered(bgp, entry);
					zlog_info("MIDR 对端改组：%pFX 群 %u -> %u（进本群），建连",
						  &entry->node_id, prev_gid,
						  entry->group_id);
				}
			}

			/* locator/能力变化也可能让一个此前不可连接的同群节点变得
			 * 可连接。统一再过一次发现闸门；已有邻接或非本群会短路。 */
			midr_nds_on_node_discovered(bgp, entry);

			/* 收包侧只更新事实，动作交给对端的 PEER_REQUEST；此处统一
			 * 通知 CL 一次（成员视图变了，与建没建邻居无关）。 */
			midr_nds_notify_cl(bgp, MIDR_TRIGGER_NODE_CHANGE);
		}
	}
}

/*
 * 〔件②（轮 4）删除 midr_nds_on_node_withdraw()：撤销事件的唯一来源改为第二组的
 * remote_node_withdraw 回调。清理链本身（detach + 删条目 + 通知 CL）一行没动，
 * 只是叫醒者换了 —— 见文件末尾回调壳里那份。〕
 */

/* Control-list/request data is transient and must not silently migrate the
 * authoritative node identity to another locator.  Such a migration requires
 * the teardown/re-key transaction in midr_nds_remote_node_update_cb(). */
static bool
midr_nds_control_locator_usable(struct bgp *bgp, const struct prefix *owner,
				const struct midr_node_entry *existing,
				const struct ipaddr *transport,
				const char *source)
{
	struct ipaddr local;

	if (!bgp || !owner || !transport)
		return false;

	if (owner->family != AF_INET ||
	    midr_prefix_is_self(bgp, owner) ||
	    !midr_ipaddr_valid_locator(transport) ||
	    !midr_nds_local_transport_get(bgp, &local) ||
	    ipaddr_family(&local) != ipaddr_family(transport) ||
	    midr_ipaddr_same(&local, transport) ||
	    !midr_nds_locator_unique(bgp, owner, transport)) {
		zlog_warn("MIDR %s：拒绝 node %pFX 的 locator %pIA（无效、异族或不唯一）",
			  source, owner, transport);
		return false;
	}

	if (existing && existing->has_transport_addr &&
	    midr_ipaddr_valid_locator(&existing->transport_addr) &&
	    !midr_ipaddr_same(&existing->transport_addr, transport)) {
		zlog_warn("MIDR %s：node %pFX 已绑定 locator %pIA，拒绝由控制消息改写为 %pIA；等待远端视图执行迁移",
			  source, owner, &existing->transport_addr, transport);
		return false;
	}

	return true;
}

/*
 * 把一个群成员（来自 MEMBER_LIST_RESP）灌入 global_view 并标记为邻居，再 I-1
 * 启动探测。NDS 拥有 global_view，故灌入统一由 NDS 负责（ctrl 侧只把 wire 解出
 * 的字段交进来）。已存在则刷新字段。注意：这里显式置 is_adjacent=true；后续
 * 远端视图更新只逐字段刷新、不碰 is_adjacent。
 */
void midr_nds_learn_member(struct bgp *bgp, struct in_addr rid, as_t asn,
			   struct ipaddr transport, uint32_t group_id)
{
	struct midr_global_view *gv;
	struct midr_node_entry key = {};
	struct midr_node_entry *entry;

	if (!bgp || !bgp->midr_nds_info || rid.s_addr == INADDR_ANY)
		return;

	gv = bgp->midr_nds_info->global_view;
	midr_prefix_from_in_addr(&key.node_id, rid);

	entry = midr_node_hash_find(&gv->nodes, &key);
	if (!midr_nds_control_locator_usable(bgp, &key.node_id, entry,
					     &transport, "成员名单"))
		return;
	if (!entry) {
		entry = XCALLOC(MTYPE_MIDR_NODE_ENTRY, sizeof(*entry));
		entry->node_id = key.node_id;
		midr_node_hash_add(&gv->nodes, entry);
	}
	entry->asn = asn;
	entry->group_id = group_id;
	entry->transport_addr = transport;
	entry->has_transport_addr = true;
	entry->last_update = monotime(NULL);
	entry->is_self = midr_prefix_is_self(bgp, &entry->node_id);
	entry->is_adjacent = true; /* 成员表里的成员就是要建邻居的对象 */

	/* I-1: probe by node_id; PM resolves transport_addr from global_view. */
	midr_pm_add_target(bgp, &entry->node_id, MIDR_SRC_BOOTSTRAP,
			   entry->capabilities);
}

/*
 * 把一个锚点候选群成员（来自 MEMBER_LIST_RESP，群号命中
 * mi->anchor_group_id[]，而非 mi->join_group_id）灌入 global_view 并 I-1
 * 启动探测。与 midr_nds_learn_member() 几乎相同，唯一差别是【不】置
 * is_adjacent——锚点候选是跨群评估节点，不是本群邻居，一旦被算进
 * cl_count_good_member_links() 的统计口径就会污染留群/退群判定。不复用同一
 * 函数：调用点意图（"这是要入的群"还是"这是拿来探探看的次优群"）完全不同，
 * 硬把 bool 参数塞进 learn_member() 会让这个关键区别散落在各调用点，不如
 * 各自独立、名字各自说明白自己在干什么。
 */
void midr_nds_learn_anchor_candidate(struct bgp *bgp, struct in_addr rid,
				     as_t asn, struct ipaddr transport,
				     uint32_t group_id)
{
	struct midr_global_view *gv;
	struct midr_node_entry key = {};
	struct midr_node_entry *entry;

	if (!bgp || !bgp->midr_nds_info || rid.s_addr == INADDR_ANY)
		return;

	gv = bgp->midr_nds_info->global_view;
	midr_prefix_from_in_addr(&key.node_id, rid);

	entry = midr_node_hash_find(&gv->nodes, &key);
	if (!midr_nds_control_locator_usable(bgp, &key.node_id, entry,
					     &transport, "锚点候选名单"))
		return;
	if (!entry) {
		entry = XCALLOC(MTYPE_MIDR_NODE_ENTRY, sizeof(*entry));
		entry->node_id = key.node_id;
		midr_node_hash_add(&gv->nodes, entry);
	}
	entry->asn = asn;
	entry->group_id = group_id;
	entry->transport_addr = transport;
	entry->has_transport_addr = true;
	entry->last_update = monotime(NULL);
	entry->is_self = midr_prefix_is_self(bgp, &entry->node_id);
	/* 有意不碰 is_adjacent：若条目已存在且已是邻居（理论上不该发生——本群
	 * 与次优群不同），保守起见也不去清它，只负责"不主动置真"。 */

	/* I-1: probe by node_id; PM resolves transport_addr from global_view. */
	midr_pm_add_target(bgp, &entry->node_id, MIDR_SRC_BOOTSTRAP,
			   entry->capabilities);
}

/*
 * 把一个同群对端纳入本群邻居：查建条目 + 标 is_adjacent + I-1 起探。由
 * midr_ctrl_connect() 的 SAME_GROUP 分支单点调用，回配与 connect_group 共用。
 *
 * 删探测 A 后这是老成员对新成员唯一的起探来源；缺了它老成员拿不到 link_entry，
 * CL 下一拍 periodic_sync 会集体误判 LEAVE 散群。
 * ⚠ 先建条目再起探：midr_pm_add_target() 查不到条目直接返回 -1。
 */
void midr_nds_adopt_group_peer(struct bgp *bgp, struct in_addr rid, as_t asn,
			       struct ipaddr transport, uint32_t group_id)
{
	struct midr_global_view *gv;
	struct midr_node_entry key = {};
	struct midr_node_entry *entry;

	if (!bgp || !bgp->midr_nds_info || rid.s_addr == INADDR_ANY)
		return;

	gv = bgp->midr_nds_info->global_view;
	midr_prefix_from_in_addr(&key.node_id, rid);

	entry = midr_node_hash_find(&gv->nodes, &key);
	if (!midr_nds_control_locator_usable(bgp, &key.node_id, entry,
					     &transport, "同群建连"))
		return;
	if (!entry) {
		entry = XCALLOC(MTYPE_MIDR_NODE_ENTRY, sizeof(*entry));
		entry->node_id = key.node_id;
		midr_node_hash_add(&gv->nodes, entry);
	}
	/* 给了才覆盖：回配帧偶有缺项，零值会抹掉已知事实。 */
	if (asn)
		entry->asn = asn;
	if (group_id)
		entry->group_id = group_id;
	if (midr_ipaddr_valid_locator(&transport)) {
		entry->transport_addr = transport;
		entry->has_transport_addr = true;
	}
	entry->last_update = monotime(NULL);
	entry->is_self = midr_prefix_is_self(bgp, &entry->node_id);

	/* 自己不是自己的邻居（纯防御：两个调用点各自已挡过 self）。 */
	if (entry->is_self)
		return;

	entry->is_adjacent = true;

	/* I-1：键用 node_id（与 I-2 停探配对），PM 自己从条目取 transport。 */
	midr_pm_add_target(bgp, &entry->node_id, MIDR_SRC_GOSSIP,
			   entry->capabilities);
}

/*
 * 一条边没了之后按 transport 反查节点表做完整清理（会话由调用方拆，各有各的
 * force 语义）。死心与拆边两处共用。不清的话 CL 还把它当好边数、PM 还对着它探
 * 到 loss 100%、link 事实还在上报。对端是引导（不在表里）时反查落空、天然 no-op。
 */
void midr_nds_cleanup_by_transport(struct bgp *bgp, struct ipaddr transport,
				   enum midr_stop_reason reason)
{
	struct midr_node_entry *entry;

	if (!bgp || !bgp->midr_nds_info ||
	    !midr_ipaddr_valid_locator(&transport))
		return;

	frr_each (midr_node_hash, &bgp->midr_nds_info->global_view->nodes,
		  entry) {
		if (!entry->has_transport_addr ||
		    !midr_ipaddr_same(&entry->transport_addr, &transport))
			continue;
		if (!entry->is_adjacent)
			return; /* 已清过，幂等 */

		zlog_info("MIDR 边注销：%pFX 的本群邻居身份已撤（会话没了，停探并不再计入 CL 统计）",
			  &entry->node_id);
		midr_nds_detach_node(bgp, entry, reason, false);
		midr_nds_notify_cl(bgp, MIDR_TRIGGER_NODE_CHANGE);
		return;
	}
}

/*
 * 收到 REP_LIST_REQ / MEMBER_LIST_REQ 时调用：请求方在其自身的 join 流程里会
 * 反过来对我们发 PM 探测包，而 midr_pm_recv() 的 pm_is_known_transport() 只
 * 接受 global_view 里已知的来源地址——请求方此时还没有权威 remote-view
 * 条目，我们的 global_view 里没有它，探测包会被当成未知来源静默丢弃。
 * 这里用请求帧自带的身份（router-id/transport/asn）灌一条最小条目，仅用于
 * 通过来源校验；不置 is_adjacent、不触发 I-1——是否真正建邻居仍由 CL 决定。
 */
void midr_nds_learn_requester(struct bgp *bgp, struct in_addr rid, as_t asn,
			      struct ipaddr transport)
{
	struct midr_global_view *gv;
	struct midr_node_entry key = {};
	struct midr_node_entry *entry;

	if (!bgp || !bgp->midr_nds_info || rid.s_addr == INADDR_ANY)
		return;

	gv = bgp->midr_nds_info->global_view;
	midr_prefix_from_in_addr(&key.node_id, rid);

	entry = midr_node_hash_find(&gv->nodes, &key);
	if (!midr_nds_control_locator_usable(bgp, &key.node_id, entry,
					     &transport, "请求方学习"))
		return;
	if (!entry) {
		entry = XCALLOC(MTYPE_MIDR_NODE_ENTRY, sizeof(*entry));
		entry->node_id = key.node_id;
		midr_node_hash_add(&gv->nodes, entry);
	}
	entry->asn = asn;
	entry->transport_addr = transport;
	entry->has_transport_addr = true;
	entry->last_update = monotime(NULL);
	entry->is_self = midr_prefix_is_self(bgp, &entry->node_id);
}

/*
 * Refresh the local self-entry from local configuration.  The topology-fact
 * publisher calls this before applying any publication suppression gate.
 */
void midr_nds_local_node_update(struct bgp *bgp)
{
	struct midr_global_view *gv;
	struct midr_node_entry key = {};
	struct midr_node_entry *entry;

	if (!bgp || !bgp->midr_nds_info)
		return;

	gv = bgp->midr_nds_info->global_view;
	midr_prefix_from_in_addr(&key.node_id, bgp->router_id);

	/*
	 * 先清掉**旧 router-id 留下的 self 孤儿**（08-23 实测必需）。
	 *
	 * bgpd 启动早期 router-id 会先取到某个接口地址（实测 z1 一度是自己的
	 * transport 10.99.0.191），配置读完才落到 loopback；本函数每次按当时的
	 * router_id 建条目，于是旧 rid 那条留在表里成孤儿——而它建的时候写死了
	 * is_self=true，退网清表的 `if (entry->is_self) continue` 会永远跳过它，
	 * 表就再也清不空（backbone-shutdown 判据 4「节点表清空」实测失败）。
	 *
	 * 本函数挂在 midr_nds_report_node 早期（早于全部抑制守卫），配置期就会跑，
	 * 所以这道清理是它的配套、不是可选优化。
	 */
	frr_each_safe (midr_node_hash, &gv->nodes, entry) {
		if (!entry->is_self || prefix_same(&entry->node_id, &key.node_id))
			continue;
		MIDR_LOG("MIDR: 清除旧 router-id 的 self 条目 %pFX（现 router-id %pI4）",
			 &entry->node_id, &bgp->router_id);
		midr_node_hash_del(&gv->nodes, entry);
		XFREE(MTYPE_MIDR_NODE_ENTRY, entry);
	}

	entry = midr_node_hash_find(&gv->nodes, &key);
	if (!entry) {
		entry = XCALLOC(MTYPE_MIDR_NODE_ENTRY, sizeof(*entry));
		entry->node_id = key.node_id;
		entry->is_self = true;
		midr_node_hash_add(&gv->nodes, entry);
	}

	entry->asn = bgp->as;
	entry->group_id = bgp->midr_nds_info->local_group_id;
	entry->capabilities = bgp->midr_nds_info->local_capabilities;
	if (bgp->midr_nds_info->transport_active) {
		entry->transport_addr =
			bgp->midr_nds_info->active_transport_addr;
		entry->has_transport_addr = true;
	} else {
		SET_IPADDR_NONE(&entry->transport_addr);
		entry->has_transport_addr = false;
	}
	entry->last_update = monotime(NULL);
	entry->is_self = true;
}

bool midr_nds_is_bootstrap(struct bgp *bgp)
{
	return bgp && bgp->midr_nds_info &&
	       !!(bgp->midr_nds_info->local_capabilities & MIDR_CAP_BOOTSTRAP);
}

/* 判据与用途见头文件。三条判据任一命中即真。 */
bool midr_nds_link_is_backbone(struct bgp *bgp,
			       const struct prefix *remote_node_id)
{
	struct bgp_midr_nds *mi;
	struct midr_node_entry key = {};
	struct midr_node_entry *entry;
	struct midr_session_ledger_entry *led;
	struct midr_bootstrap_entry *be;
	struct listnode *node;
	uint32_t rid;

	if (!bgp || !bgp->midr_nds_info || !remote_node_id ||
	    remote_node_id->family != AF_INET ||
	    remote_node_id->prefixlen != IPV4_MAX_BITLEN)
		return false;

	mi = bgp->midr_nds_info;
	rid = remote_node_id->u.prefix4.s_addr;
	if (!rid)
		return false;

	/* ① 引导候选池（手配 + 名单 + 种子，池内 rid 恒非 0）。 */
	if (mi->bootstrap_list)
		for (ALL_LIST_ELEMENTS_RO(mi->bootstrap_list, node, be))
			if (be->rid.s_addr == rid)
				return true;

	/* ② 会话台账：按条目里的 remote_rid 认人。 */
	if (mi->session_ledger)
		for (ALL_LIST_ELEMENTS_RO(mi->session_ledger, node, led))
			if (led->remote_rid.s_addr == rid &&
			    led->reason == MIDR_SESSION_ATTACH)
				return true;

	/* ③ 节点表条目带 BOOTSTRAP 位（兜底，正常查不到）。 */
	key.node_id = *remote_node_id;
	entry = midr_node_hash_find(&mi->global_view->nodes, &key);
	if (entry && (entry->capabilities & MIDR_CAP_BOOTSTRAP))
		return true;

	return false;
}


/*
 * 把引导节点该有的样子**一次性坐实**（幂等；非引导直接返回）。
 *
 * 治的是 conf 行序——frr.conf 逐行执行，命令层的守卫只挡得住"`midr role
 * bootstrap` 写在前面"那半边（后面的 `midr group-id` / `midr role group-rep`
 * 会被各自的守卫拒掉）；它写在**后面**时，前面那几行早已生效，没人拦得住。
 * 所以配置读完再统一收一次尾，两头夹住，**不管几行怎么排，最终一定是纯引导**。
 *
 * 冲突配置的裁定口径 = **引导优先**（运维写了 `midr role bootstrap` 就是要它当
 * 引导），被清掉的那几项各打一条 warn——静默清理会让运维以为自己配的群号生效了。
 *
 * 两个调用点：`bgp_config_end` 钩子（配置期收尾）与 vty 的 `midr role bootstrap`
 * （运行期敲命令时当场生效，那时不在配置态、没有后续行会把它改回去）。
 */
void midr_nds_bootstrap_enforce(struct bgp *bgp)
{
	struct bgp_midr_nds *mi;

	if (!bgp || !bgp->midr_nds_info)
		return;
	mi = bgp->midr_nds_info;
	if (!(mi->local_capabilities & MIDR_CAP_BOOTSTRAP))
		return;

	/* ① 群代表位：引导不参群，更不能当代表（理由见 vty 守卫⓪）。 */
	if (mi->local_capabilities & MIDR_CAP_GROUP_REP) {
		zlog_warn("MIDR：本机配置为引导节点，同时配的群代表角色已被忽略并清除（引导专职化：二者不可兼任）");
		midr_nds_set_capability(bgp, mi->local_capabilities &
						     ~MIDR_CAP_GROUP_REP);
	}

	/* ② 群号：引导恒 0。config_group_id 一并清，否则重启后又被它带回来。 */
	if (mi->config_group_id != 0 || mi->local_group_id != 0) {
		zlog_warn("MIDR：本机配置为引导节点，同时配的群号 %u 已被忽略并清除（引导群号恒为 0、不参与任何群）",
			  mi->config_group_id ? mi->config_group_id
					      : mi->local_group_id);
		mi->config_group_id = 0;
		midr_nds_set_group_id(bgp, 0);
	}
}

/* bgp_config_end 钩子的适配壳（hook 要求 int 返回值）。 */
static int midr_nds_bootstrap_after_cfg(struct bgp *bgp)
{
	midr_nds_bootstrap_enforce(bgp);
	return 0;
}

void midr_nds_set_capability(struct bgp *bgp, uint32_t new_caps)
{
	struct bgp_midr_nds *mi;
	bool was_rep, is_rep;
	bool was_boot, is_boot;

	if (!bgp || !bgp->midr_nds_info)
		return;

	mi = bgp->midr_nds_info;
	was_rep = !!(mi->local_capabilities & MIDR_CAP_GROUP_REP);
	is_rep = !!(new_caps & MIDR_CAP_GROUP_REP);
	was_boot = !!(mi->local_capabilities & MIDR_CAP_BOOTSTRAP);
	is_boot = !!(new_caps & MIDR_CAP_BOOTSTRAP);

	/*
	 * 留证（08-13，只报不拦）：引导与群代表硬互斥，人工路径由三条 vty 命令的
	 * 守卫堵死、自动路径（I-7 REP_ELECT / CREATE 自任）在引导上被第四守卫短路，
	 * 配置期的冲突写法由 bgp_config_end 兜底清理。真出现两位并存，说明还有一条
	 * 没数到的路径——报出来，别让它静默存在（本函数是所有置位路径的必经之地）。
	 */
	if (is_boot && (new_caps & MIDR_CAP_GROUP_REP))
		zlog_err("MIDR：能力位出现 Bootstrap + GroupRep 并存（caps 0x%x）——二者互斥，请检查是哪条路径置的位",
			 new_caps);

	mi->local_capabilities = new_caps;
	/* Bootstrap nodes do not originate topology links.  Withdraw the old
	 * role's links at the same transition that withdraws the local Node fact. */
	if (is_boot && !was_boot)
		midr_nds_facts_withdraw_all_links(bgp);
	midr_nds_report_node(bgp, MIDR_ORIGIN_CAP_UPDATE);

	/*
	 * 挂靠钩子（保底轮 2 批 5）——挂在 setter **内部**而不是各个调用点上：
	 * 手动命令 `[no] midr role group-rep`、I-7 的 REP_ELECT/REP_RESIGN、
	 * CREATE 自建群后自任代表，三条路全过这一个入口，一次覆盖、不会漏。
	 *
	 *   是代表 → 确保挂靠达标（不足 K 就去要一份活引导名单）；名单是异步回来
	 *            的，挑 K 台挂靠落在 midr_nds_on_bootstrap_list() 里。
	 *   翻假（卸任）→ 拆掉 ATTACH 账下的边，**只拆 ATTACH**：同群边、骨干边、
	 *                 CL 锚点边、运维手配边一概不碰（凭台账认边，⑦ 核对第二条）。
	 *
	 * ⚠ 判据是"**是**代表"而不是"**刚变成**代表"（批 6 改，原为 is_rep &&
	 * !was_rep）。病根：挂靠全灭之后，本来就是代表的节点位是 true→true、不翻转，
	 * 而另两个触发源（挂靠会话 Down / 收到名单）在全灭态下都不存在——于是脱骨干
	 * warn 里承诺的"敲一条 midr bootstrap 即可拉回"落空，实测只有重启能救。
	 * 改成"是代表就确保达标"之后，凡经过本 setter 的角色动作都会补一次。
	 * 不怕重复触发：ensure 自带"账 < K"判据，挂满了就是空转（见该函数）。
	 */
	if (is_rep) {
		/*
		 * 配置期只置状态、不动作（D1）：frr.conf 里 `midr role group-rep`
		 * 完全可能排在 `midr bootstrap` **之前**（生成的 r1 conf 正是如此），
		 * 那一刻候选池还空、拉名单必然落空，而此后再无触发源——代表就永远
		 * 挂不上（08-13 实测 P1）。改由 bgp_config_end 钩子在配置读完后统一
		 * 补拉（见 midr_nds_attach_after_cfg），与 BGP 原生治顺序的两件套
		 * 同款：peer_create 时置 shut_during_cfg 压住不连，config_end 时由
		 * peer_unshut_after_cfg 统一放行。
		 * 能力位照置、Node fact 照常刷新，推迟的只有"去拉名单"这个动作。
		 */
		if (bgp_config_inprocess()) {
			zlog_info("MIDR 挂靠：本机%s群代表（群 %u），但配置仍在加载中——挂靠推迟到配置读完",
				  was_rep ? "仍是" : "成为", mi->local_group_id);
		} else {
			midr_nds_attach_ensure(bgp, was_rep ? "仍是群代表"
							    : "成为群代表");
		}
	} else if (was_rep) {
		zlog_info("MIDR 挂靠：本机卸任群代表，拆除挂靠会话");
		midr_nds_attach_detach_all(bgp);
	}

}

/*
 * 拆掉全部挂靠边（卸任时调用）。**只认台账里 reason == ATTACH 的条目**——这正是
 * "对账认边改凭台账"（⑦ 核对第二条）的用处：挂靠的对端是引导，用旧判据
 * （should_peer 现推"这条边该不该有"）根本推不出来，因为引导不同群、也不在节点表。
 *
 * 先收集再拆：midr_ctrl_detach_transport() 会销账（改台账链表），边遍历边改必崩。
 */
void midr_nds_attach_detach_all(struct bgp *bgp)
{
	struct bgp_midr_nds *mi;
	struct listnode *node;
	struct midr_session_ledger_entry *e;
	struct ipaddr *doomed;
	struct in_addr *rids;
	unsigned int n = 0, i;

	if (!bgp || !bgp->midr_nds_info)
		return;
	mi = bgp->midr_nds_info;
	if (!mi->session_ledger || list_isempty(mi->session_ledger))
		return;

	doomed = XCALLOC(MTYPE_TMP,
			 listcount(mi->session_ledger) * sizeof(*doomed));
	rids = XCALLOC(MTYPE_TMP,
		       listcount(mi->session_ledger) * sizeof(*rids));
	for (ALL_LIST_ELEMENTS_RO(mi->session_ledger, node, e))
		if (e->reason == MIDR_SESSION_ATTACH) {
			doomed[n] = e->transport;
			rids[n] = e->remote_rid;
			n++;
		}

	for (i = 0; i < n; i++)
		midr_ctrl_detach_transport(bgp, doomed[i], rids[i], false,
					   MIDR_STOP_CLUSTER_CHANGE);

	if (n)
		MIDR_LOG("MIDR 挂靠：已拆除 %u 条挂靠会话（非 ATTACH 的边一律未动）",
			 n);

	XFREE(MTYPE_TMP, doomed);
	XFREE(MTYPE_TMP, rids);
}

/*
 * 换组重收敛编排核心：把本地群号切到 new_gid，并让动态会话跟随重收敛。
 * 手动通道（midr_nds_set_group_id）与 I-7 JOIN 决策共用这一段，返回与新群
 * 建连的成员数。三步次序有讲究：
 *
 *   1. 先改群号 + 重通告——必须早于建连：PEER_REQUEST 资格闸门按 local_group_id
 *      过滤，若先建连、群号还是旧的，对端回来的反向建连会被自己的闸门挡掉。
 *      重通告走 midr_originate_group_update（薄封装）→ midr_nds_report_node()
 *      → 第二组 node_upsert，不绕过上报出口。
 *   2. 与新群成员建连（connect_group 遍历 global_view 中群号==new_gid 的成员）。
 *      new_gid==0（手动离群）时跳过——0 号非有效群，无成员可连。
 *   3. 拆旧群：凡"仍标 is_adjacent 却已不属于本群"的节点一律 detach，判据用
 *      !should_peer 而非 ==old_gid，顺带清掉任何历史残留的错群邻接。detach 复用
 *      links 泄漏修复轮的统一清理（I-2 停探 + 双键删 link_entry + 复位 is_adjacent
 *      + 拆动态会话），不删 node 条目。PM 真实化后第 2 步的停探语义自动完整。
 *   4. 停非本群群代表的评估期探测：join 第一段对 rep_dir 里【每个】群代表都起了
 *      探测（midr_join_on_rep_list），落定后除本群代表外都不再需要。它们不带
 *      is_adjacent（只探不纳入邻居），第 3 步扫不到，故按 rep_dir 单独收口——
 *      否则每 join 一次就永久多养一批跨群探测。**例外**：mi->anchor_group_id[]
 *      这两个次优群的代表暂不收口——它们此刻可能还没被 ANCHOR_PROBE_DONE 读取
 *      （该定时器与触发 JOIN 的定时器是各自独立调度的，落地时间只是相近，谁先
 *      谁后不确定），这里提前删了 link 数据会让锚点评估白算。真正的收口挪到
 *      ANCHOR 决策执行完之后（见 MIDR_DECISION_ANCHOR 分支尾部）。
 */

/*
 * 停止对 rep_dir 单条代表条目的评估期探测（I-2 + 删 link，不拆会话——只探不
 * 连的代表本就没有会话）。midr_group_reconverge 第 4 步与 ANCHOR 决策执行后
 * 的收尾共用，抽出来避免重复。键的取法必须与 midr_join_on_rep_list 起探时一
 * 致——占位路删除后（2026-08-11 批 1.5）双方恒走真名：rid=0 的条目起探时就被
 * 跳过、压根没起探，这里直接返回即可。
 */
static void midr_stop_rep_probe_entry(struct bgp *bgp,
				      const struct midr_rep_entry *r)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct midr_node_entry key = {};
	struct midr_node_entry *rep_entry;

	if (r->rep_rid.s_addr == INADDR_ANY)
		return; /* 起探时已跳过（无真名不建条目），无探可停 */

	midr_prefix_from_in_addr(&key.node_id, r->rep_rid);

	rep_entry = midr_node_hash_find(&mi->global_view->nodes, &key);
	/* is_self：本节点自兼群代表时不能把自己停了。is_adjacent：该代表若已是
	 * 本群邻居（如它换群进了本群、而 rep_dir 里还是旧群号），归 reconverge
	 * 第 3 步管，这里不碰——避免误伤正经邻接。 */
	if (!rep_entry || rep_entry->is_self || rep_entry->is_adjacent)
		return;

	midr_nds_detach_node(bgp, rep_entry, MIDR_STOP_CLUSTER_CHANGE, false);
}

static uint32_t midr_group_reconverge(struct bgp *bgp, uint32_t new_gid)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	uint32_t old_gid = mi->local_group_id;
	struct midr_node_entry *entry;
	struct listnode *rn;
	struct midr_rep_entry *r;
	uint32_t connected = 0, detached = 0, reps_stopped = 0;

	/*
	 * 0. 落到无群（new_gid==0，即 I-7 的 LEAVE / RECONNECT）时先卸任群代表。
	 *
	 * 群代表是**这个群的**代表，群都退了角色不能跟着节点走。放在改群号之前，
	 * 卸任才随下面那笔通告一起发出去。
	 *
	 * 不卸的后果 08-24 拔线实验实测过一整条链：隔离期自任代表的成员恢复后
	 * LEAVE，GROUP_REP 残留 → 它以"群 1 代表"身份进了引导的 rep 目录 → 新节点
	 * 被推荐到这个假代表 → 向它要成员表要不到（它自己群号已是 0，应答闸门不
	 * 放行）→ **join 死锁**，卡在无群状态出不来。
	 *
	 * ⚠ 口径与 midr shutdown 的卸任一致（同样不可逆、同样 warn 提示重敲）——
	 *   两条路径对同一件事只能有一种做法。
	 *
	 * ⚠ BOOTSTRAP 位**只报不清**：引导群号恒 0（bootstrap_enforce 强制），根本
	 *   不该走到这里；而它是**配置角色**（`midr role bootstrap`），清了不可逆
	 *   且静默——全网少一个引导候选却无人知晓，比留着更糟。故照
	 *   midr_nds_set_capability() 里那条留证的同款做法只报错、不动位。
	 */
	if (new_gid == 0 && old_gid != 0) {
		if (mi->local_capabilities & MIDR_CAP_GROUP_REP) {
			zlog_warn("MIDR：退出群 %u 同时卸任群代表（角色属于该群，不随节点走）——如仍需要请重敲 `midr role group-rep`",
				  old_gid);
			midr_nds_set_capability(bgp, mi->local_capabilities &
							     ~MIDR_CAP_GROUP_REP);
		}
		if (mi->local_capabilities & MIDR_CAP_BOOTSTRAP)
			zlog_err("MIDR：引导节点竟走到退群路径（群 %u，caps 0x%x）——引导群号本应恒 0，请查是哪条路径给它置了群号；BOOTSTRAP 位保留不动",
				 old_gid, mi->local_capabilities);
	}

	/* 1. 改群号 + 重通告（先于建连）。 */
	midr_originate_group_update(bgp, new_gid, old_gid);

	/* 2. 与新群成员建连（离群 new_gid==0 时无成员可连，跳过）。 */
	if (new_gid != 0)
		connected = midr_ctrl_connect_group(bgp, new_gid,
						    MIDR_SESSION_SAME_GROUP);

	/* 3. 拆旧群：邻接但已不同群的节点全部 detach。 */
	frr_each (midr_node_hash, &mi->global_view->nodes, entry) {
		if (entry->is_self || !entry->is_adjacent)
			continue;
		if (midr_discovery_should_peer(bgp, entry))
			continue; /* 仍属本群，留着 */
		/*
		 * 锚点边豁免（降级不跳过，与对端改组那处同款）：本机换群后，
		 * CL 建的群间锚点边不该跟着拆——只注销"本群邻居"身份，会话与
		 * 探测保住。判据用台账，理由见 midr_nds_edge_is_anchor()。
		 */
		if (midr_nds_edge_is_anchor(bgp,
					    entry->node_id.u.prefix4.s_addr)) {
			entry->is_adjacent = false;
			zlog_info("MIDR 换群：%pFX 已不属本群，锚点边保留会话与探测",
				  &entry->node_id);
			continue;
		}
		midr_nds_detach_node(bgp, entry, MIDR_STOP_CLUSTER_CHANGE, true);
		detached++;
	}

	/*
	 * 4. 停非本群群代表的评估期探测（例外：还留给锚点评估用的两个次优群，
	 *    见本函数头注释）。
	 */
	for (ALL_LIST_ELEMENTS_RO(mi->rep_dir, rn, r)) {
		if (r->group_id == new_gid)
			continue; /* 本群代表，留着继续探 */
		if (r->group_id == mi->anchor_group_id[0] ||
		    r->group_id == mi->anchor_group_id[1])
			continue; /* 锚点候选还没评估完，留给 ANCHOR 决策执行完后收口 */

		midr_stop_rep_probe_entry(bgp, r);
		reps_stopped++;
	}

	zlog_info("MIDR 换组重收敛：群 %u -> %u（建连 %u 个新群成员，拆除 %u 个旧群邻接，停探 %u 个非本群代表）",
		  old_gid, new_gid, connected, detached, reps_stopped);

	/* 第 2 步的 connect 已逐条把新群成员纳入邻居（置位 + 起探），视图变了要
	 * 告知 CL。按"一批建完"发一次，不在循环里逐成员发（发现链专题）。 */
	if (connected)
		midr_nds_notify_cl(bgp, MIDR_TRIGGER_NODE_CHANGE);

	return connected;
}

/*
 * 清锚点评估的两样上下文：备选群号 anchor_group_id[] 与 60s 热身定时器
 * t_anchor_probe_done（决策 midr-shutdown-semantics §5.4）。
 *
 * ⚠ 锚点（搭桥）全套是 zhc 的交付（合并提交 502007a6），本函数**不碰他的判定
 * 逻辑**，只补异常路径上的清理时机。为什么要补：这两样目前只在正常路径清零
 * （RECOMMEND 开头、ANCHOR 执行完），定时器更是只在实例销毁时才 cancel——于是
 * 退网 / 改组 / 中止 join 之后，迟到的定时器会拿着**过期的备选群**触发
 * ANCHOR_PROBE_DONE，CL 按陈旧上下文去选锚建连。
 *
 * ⚠ 挂点只挂"异常路径"，**绝不挂 midr_group_reconverge**：那个函数正常入网
 * （I-7 JOIN）也要走，而那一刻锚点评估正合法在途——zhc 自己在本文件
 * midr_group_reconverge 头注释第 4 步里点明过这个坑（"这两个次优群的代表暂不
 * 收口……提前删了 link 数据会让锚点评估白算"）。挂那儿 = 群间锚点边永远建不起来。
 * 四个挂点：退网 / 手动改组 / 重开 join round / 中止 join。
 */
void midr_nds_anchor_ctx_clear(struct bgp *bgp)
{
	struct bgp_midr_nds *mi;

	if (!bgp || !bgp->midr_nds_info)
		return;
	mi = bgp->midr_nds_info;

	if (!mi->anchor_group_id[0] && !mi->anchor_group_id[1] &&
	    !mi->t_anchor_probe_done)
		return; /* 没有残留，静默 */

	MIDR_LOG("MIDR 锚点：清理评估上下文（备选群 %u/%u，热身定时器 %s）",
		 mi->anchor_group_id[0], mi->anchor_group_id[1],
		 mi->t_anchor_probe_done ? "在武装" : "无");

	mi->anchor_group_id[0] = 0;
	mi->anchor_group_id[1] = 0;
	event_cancel(&mi->t_anchor_probe_done);
}

void midr_nds_set_group_id(struct bgp *bgp, uint32_t new_gid)
{
	struct bgp_midr_nds *mi;

	if (!bgp || !bgp->midr_nds_info)
		return;
	mi = bgp->midr_nds_info;

	/*
	 * 配置值先落账，且必须早于下面的 old==new 短路：运维配的群号恰好等于当前
	 * 运行值（如 CL 先把我们 JOIN 进了同一个群）时也得记下，否则 I-7 的
	 * RECOMMEND/CREATE 分支与退网回落读到 0、等同于"没配过"。
	 */
	mi->config_group_id = new_gid;

	/*
	 * 退网期只改值、不重收敛（决策 midr-shutdown-semantics §5.1 / 退网专题步 4）。
	 * 退网态下没有会话、没有节点表，reconverge 三步（重通告 / 与新群成员建连 /
	 * 拆旧群邻接）全是空转，重通告还会被 propagate_self 的 shutdown 守卫吞掉。
	 * 运行值跟着配置值走，重入（`no midr shutdown`）时 join 编排按新值起步。
	 */
	if (mi->shutdown) {
		mi->local_group_id = new_gid;
		midr_nds_local_node_update(bgp);
		MIDR_LOG("MIDR 换组(手动)：本机处于退网态，只记配置群号 %u（重入时生效，不重收敛）",
			 new_gid);
		return;
	}

	/* old==new 短路：无变化不折腾会话。 */
	if (mi->local_group_id == new_gid) {
		MIDR_LOG("MIDR 换组(手动)：群号未变（%u），无动作", new_gid);
		return;
	}

	/*
	 * 中止在途/待发的 join：运维手动换组 = 强制切换，放弃经 bootstrap 加入的
	 * 意图。作废 join_intent 是关键——REP_LIST_REQ 可能已发出、响应尚未到达
	 * （此刻 join_in_progress 还是 false），若不作废意图，迟到的 REP_LIST_RESP
	 * 会把 join 重新拉起、CL 决策再把运维刚设的群号改掉（静默覆盖）。
	 */
	if (mi->join_intent || mi->join_in_progress) {
		mi->join_intent = false;
		mi->join_in_progress = false;
		mi->join_phase = MIDR_JOIN_IDLE;
		mi->join_group_id = 0;
		mi->bootstrap_cur = NULL; /* §8.32：收起 failover 游标，残留 pending
					   * 死掉时守卫（intent+地址匹配）自然忽略 */
		MIDR_LOG("MIDR 换组(手动)：作废在途加入意图");
	}

	/*
	 * 挂点②（配套一）：手动改组 = 运维强制切换，上一轮 RECOMMEND 存的备选锚点
	 * 群是按旧群号算出来的，已经过期——清掉，免得迟到的定时器按它建连。
	 * 清在 reconverge **之前**、且只清在这条手动路径上：reconverge 还被 I-7 的
	 * JOIN/LEAVE 共用，那两条路上锚点评估是合法在途的（见 anchor_ctx_clear 注释）。
	 */
	midr_nds_anchor_ctx_clear(bgp);

	MIDR_FLOW_LOG("MIDR 换组(手动)：群 %u -> %u", mi->local_group_id, new_gid);
	midr_group_reconverge(bgp, new_gid);
}

const char *midr_origin_reason_str(enum midr_origin_reason reason)
{
	switch (reason) {
	case MIDR_ORIGIN_INIT:
		return "init";
	case MIDR_ORIGIN_GROUP_UPDATE:
		return "group-update";
	case MIDR_ORIGIN_CAP_UPDATE:
		return "cap-update";
	case MIDR_ORIGIN_TRANSPORT_UPDATE:
		return "transport-update";
	case MIDR_ORIGIN_REJOIN:
		return "rejoin";
	case MIDR_ORIGIN_LEAVE:
		return "leave";
	}
	return "unknown";
}

/*
 * 〔件②（轮 4）删除 midr_propagate_self —— 自有 BGP-LS 自通告出口整条退役。〕
 *
 * 本机身份（群号 / 能力位 / transport）现在只有一条出路：midr_nds_report_node()
 * → 第二组的 midr_topology_node_upsert/_withdraw，由他们编码上线。MIDR 层不再
 * 直调 bgp_ls_originate_* / bgp_ls_withdraw_* 族，原先"传播面三单点"的说法随之
 * 作废（收包入口与 E-1 出口同批删除）。
 *
 * 退网守卫也跟着搬家：原先本函数内层那道（守 keepalive 直调路）没有了，出站方向
 * 只剩上报出口 midr_nds_report_node() / midr_nds_report_link() 各一道。
 */

/* ===========================================================================
 * Timers
 * =========================================================================*/

/*
 * 〔件④（轮 4）删除 keepalive 与 expire-check 两个定时器 —— 活性语义切换。〕
 *
 * 节点条目的生死不再由本机老化判定，改由第二组的对象生命周期决定：
 * remote_node_withdraw 回调删条目、remote_node_update 纠偏字段。条目在表
 * = 对方 usable 视图里还活着。`entry->last_update` 降级为纯观测量（见其
 * 字段注释），任何判死/过滤都不许再读它。
 *
 * 连带退役：`midr keepalive-suppress` 实验开关（它本就只包这两个定时器，
 * 且实测只停得掉"删条目"、停不掉判死判据——记档 57）。
 */

/* Deferred join-phase triggers: fire after MIDR_JOIN_PROBE_WAIT_SECS to give
 * the PM long-term EWMA time to warm up before CL evaluates link quality. */
static void midr_join_rep_probe_done_cb(struct event *t)
{
	struct bgp *bgp = EVENT_ARG(t);
	MIDR_FLOW_LOG("MIDR 加入：REP_PROBE_DONE 定时器触发，通知 CL");
	midr_nds_notify_cl(bgp, MIDR_TRIGGER_REP_PROBE_DONE);
}

/*
 * Periodic-sync timer: hand the global view to CL for a full re-evaluation
 * on a fixed cadence (independent of probe/membership events).
 */
/* Count established sessions carrying the MIDR-LS AF -- group members and
 * anchor connections alike, both use it, so this one count covers "any
 * working MIDR session" regardless of which kind.
 *
 * ⚠ 判据绑在地址族上，件②（轮 4）已随退役 (4,8) 换成 (4,9) MIDR-LS。（别改成
 * 认 PEER_FLAG_MIDR_OVERLAY：运维静态配的会话不带这个标记，会被漏数。） */
static unsigned int midr_established_session_count(struct bgp *bgp)
{
	struct peer *peer;
	struct listnode *node;
	unsigned int count = 0;

	for (ALL_LIST_ELEMENTS_RO(bgp->peer, node, peer)) {
		if (!peer->afc[AFI_BGP_LS][SAFI_MIDR_LS])
			continue;
		if (peer->connection &&
		    peer->connection->status == Established)
			count++;
	}
	return count;
}

/*
 * Debounced isolation detection (MIDR_TRIGGER_ISOLATED). PERIODIC_SYNC's own
 * LEAVE judgement only looks at known adjacent members of the local group,
 * which is trivially empty -- and therefore always "stay" -- for a node that
 * has lost every session; it never notices total isolation. This is a
 * separate check for exactly that gap.
 *
 * Skipped outside steady state (no group yet, or a join still in progress)
 * since a transient zero-session count there is expected, not a failure.
 * 顺带效果：引导节点群号恒 0，第一道守卫即跳过——引导不入群，自然也谈不上
 * 失联自救（与 ⑧=A 第四守卫同向，不必另设防）。
 */
static void midr_isolation_check(struct bgp *bgp)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;

	if (mi->local_group_id == 0 || mi->join_phase != MIDR_JOIN_IDLE) {
		mi->isolated_ticks = 0;
		return;
	}

	if (midr_established_session_count(bgp) > 0) {
		mi->isolated_ticks = 0;
		return;
	}

	mi->isolated_ticks++;
	if (mi->isolated_ticks < MIDR_ISOLATION_DEBOUNCE_TICKS) {
		MIDR_LOG("MIDR: 0 个已建立会话（第 %u/%u 次探测，群 %u），未达去抖阈值",
			 mi->isolated_ticks, MIDR_ISOLATION_DEBOUNCE_TICKS,
			 mi->local_group_id);
		return;
	}

	/* 级别按我方四档判据抬到 warn（会话全断=运维要查网络）；⚠ 文案一字不动——
	 * isolation-test 的判据直接 grep 这串，改一个字判据就静默失效。 */
	zlog_warn("MIDR: 连续 %u 次探测 0 个已建立会话（群 %u），判定孤岛，通知 CL",
		  mi->isolated_ticks, mi->local_group_id);
	mi->isolated_ticks = 0;
	midr_nds_notify_cl(bgp, MIDR_TRIGGER_ISOLATED);
}

/*
 * 同群补边兜底：收包侧只在状态跳变（is_new / 改群号）时建边，一旦那次建连失败
 * 就再没有下一次机会——实测两端各自僵住、边永久缺失（一侧死心清位后不再触发，
 * 另一侧 is_adjacent 还挂着也不触发）。这里按**边的实际状态**兜底重连。
 *
 * 判据用"transport 上有没有通着的 overlay 会话"，不用 is_adjacent（它可能停在
 * 任一侧的旧值），也不用 midr_node_established_peer()（那个按 router-id 找，
 * 分不清运维静态会话与 overlay，直连场景会误判成"已有会话"而不建 LS 通道）。
 *
 * 照 ledger_age_scan 的规矩先收集后动作：connect 会往台账链表追加，边遍历边改必崩。
 */
static void midr_nds_reconnect_missing_peers(struct bgp *bgp)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct midr_node_entry *entry;
	struct midr_node_entry *missing[MIDR_AGE_SCAN_MAX];
	int n = 0, i;

	/* 退网中不建边；join 在途时编排层自己会连，别插一脚。 */
	if (mi->shutdown || mi->join_phase != MIDR_JOIN_IDLE)
		return;

	frr_each (midr_node_hash, &mi->global_view->nodes, entry) {
		union sockunion su;
		struct peer *peer;

		if (n >= MIDR_AGE_SCAN_MAX)
			break;
		if (!midr_discovery_should_peer(bgp, entry))
			continue;

		if (!midr_ipaddr_to_sockunion(&entry->transport_addr, &su))
			continue;
		peer = peer_lookup(bgp, &su);
		if (peer && peer->connection &&
		    peer->connection->status == Established &&
		    midr_nds_peer_is_overlay(peer))
			continue; /* overlay 通着 */

		missing[n++] = entry;
	}

	for (i = 0; i < n; i++) {
		zlog_info("MIDR 补边兜底：同群 %pFX 的 overlay 会话未通，重连",
			  &missing[i]->node_id);
		midr_ctrl_connect(bgp, missing[i], MIDR_SESSION_SAME_GROUP,
				  true);
	}
}

/* 注册函数定义在文件后半（回调实现旁边），periodic_sync 的兜底重试要用。 */
static void midr_nds_remote_view_register(struct bgp *bgp);

static void midr_periodic_sync_timer(struct event *t)
{
	struct bgp *bgp = EVENT_ARG(t);
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct midr_node_entry *entry;

	/*
	 * 件③ 回调注册的兜底重试（轮 5 补）。
	 *
	 * 注册原本只有两次机会：init 那次**必然失败**（我方 init 跑在 bgp_create()
	 * 执行期间，默认实例还没挂上去，取不到 ctx）、bgp_config_end 那次通常成功。
	 * 万一 config_end 也失败（ctx 未就绪、或对方 register 返回非 0），此前**再
	 * 没有人试第二次** —— 件③ 整条下行链哑掉：节点表永远空、谁都学不到，而且
	 * 是**静默的**（只有一条 warn + `show midr group2` 里的"未注册"）。
	 *
	 * 函数内有幂等闸（remote_view_registered），注册成功后这里就是一次空调用；
	 * 放在最前面是因为它是其余一切下行数据的前提。
	 */
	midr_nds_remote_view_register(bgp);

	/* 配置的 locator 可能在命令执行时尚未出现在本机接口上。失败态不向任何
	 * 运行模块暴露该地址；定时重试让接口随后就绪时可以自动激活。 */
	if (mi->transport_addr_set && !mi->transport_active &&
	    !mi->transport_reconfiguring && !mi->shutdown)
		(void)midr_nds_transport_reconcile(bgp);

	midr_nds_notify_cl(bgp, MIDR_TRIGGER_PERIODIC_SYNC);

	/* §8.31：顺路把当前视图里的引导节点刷一遍种子库 last_seen（"我最近还
	 * 见过它"），再 prune 到上限防膨胀。下线/过期不删库——种子跨活性留底。 */
	frr_each (midr_node_hash, &mi->global_view->nodes, entry)
		midr_maybe_save_bootstrap_seed(bgp, entry);
	midr_store_seed_prune(MIDR_STORE_SEED_KEEP);

	/* B1：顺带收拾"断了很久还挂着"的半边（批 5c）。 */
	midr_nds_ledger_age_scan(bgp);

	/* 失联自救：会话全断时没有任何别的机制会反应（见函数头注释）。 */
	midr_isolation_check(bgp);

	/* 同群补边兜底：收包侧那条路只在状态跳变时触发，失败就没有下一次。 */
	midr_nds_reconnect_missing_peers(bgp);

	event_add_timer(bm->master, midr_periodic_sync_timer, bgp,
			MIDR_PERIODIC_SYNC_INTERVAL, &mi->t_periodic_sync);
}

/* ===========================================================================
 * I-5: PM -> NDS, link state update
 * =========================================================================*/

/*
 * 按对端 router-id 找一条**能承载 MIDR 拓扑情报**的已建立会话。
 *
 * 四个调用点（PM 起探两处、link 上报闸门、快照入选筛子）问的都是同一句话：
 * "我跟这个节点之间，那条 MIDR 的边通不通"。所以判据除了 Established，还必须
 * 认 overlay 出身——真分离基线下运维静态邻居不载 MIDR-LS，拿它当"这条边可用"
 * 的证据是错的（会把根本不是 MIDR 边的链路上报给第二组）。
 *
 * `midr session` 手配的 MIDR 会话同样带 OVERLAY 标记（整形 helper 统一盖章），
 * 不受影响；此判据与建连侧"迁族后一律自建不复用"、show 侧过滤是同一口径。
 */
struct peer *midr_node_established_peer(struct bgp *bgp,
				       const struct prefix *node_id)
{
	struct peer *peer;
	struct listnode *node;

	if (!bgp || !node_id || node_id->family != AF_INET ||
	    node_id->prefixlen != IPV4_MAX_BITLEN)
		return NULL;

	/* Locate the BGP peer by its advertised router-id (TLV 516). */
	for (ALL_LIST_ELEMENTS_RO(bgp->peer, node, peer)) {
		if (CHECK_FLAG(peer->sflags, PEER_STATUS_GROUP))
			continue;
		if (!peer->connection ||
		    peer->connection->status != Established)
			continue;
		if (!midr_nds_peer_is_overlay(peer))
			continue;
		if (peer->remote_id.s_addr == node_id->u.prefix4.s_addr)
			return peer;
	}

	return NULL;
}

/*
 * §8.21 去抖判断（"一处判断、两处消费"的那一处）：本次指标相对**上次发出的
 * 快照**该不该发，以及——同样重要——是"真变化"还是"只是该刷一次了"。
 *
 * 三条必发条件：
 *   1. FIRST     首次（还没发过）——邻居的第一条不能等；
 *   2. STATUS    status 跳变——UP/DEGRADED/DOWN 变化永远是大事，DOWN 必须立刻广播；
 *   3. THRESHOLD 任一指标超阈值（阈值与取值理由见 bgp_midr_nds.h 的 MIDR_DEBOUNCE_*）。
 *
 * 〔件④（轮 4）删掉第 4 条 PERIODIC（30s 兜底重发）〕：upsert 的语义是"这个
 * 对象变了"，指标没动也报会让对方误以为变了，且我方 version 是上报序号、每发
 * 必增，等于凭空造版本；而合栈后上报是同进程函数调用，返回值即成败、失败有
 * pending 位重试，不存在"漏收"要靠周期重发去兜。随之"发但不通知 CL"这个特例
 * 也没了 —— 现在非 NONE 即真变化，两个下游口径合一。
 *
 * 只读不写：快照的更新在调用方发出之后做（发出去了才算数）。
 */
enum midr_debounce_reason {
	MIDR_DEBOUNCE_NONE = 0,	 /* 无显著变化：两个下游都跳过 */
	MIDR_DEBOUNCE_FIRST,	 /* 首条 */
	MIDR_DEBOUNCE_STATUS,	 /* 状态跳变 */
	MIDR_DEBOUNCE_THRESHOLD, /* 指标超阈值 */
};

static enum midr_debounce_reason
midr_nds_e1_should_send(const struct midr_link_entry *link,
			enum midr_link_status new_status,
			const struct midr_nds_link_metrics *cur, time_t now)
{
	const struct midr_nds_link_metrics *snap = &link->sent_metrics;
	uint32_t rtt_delta;

	/* 1. 首次必发。 */
	if (!link->sent_once)
		return MIDR_DEBOUNCE_FIRST;

	/* 2. 状态跳变必发。 */
	if (new_status != link->status)
		return MIDR_DEBOUNCE_STATUS;

	/*
	 * 3. 逐指标超阈值判断。次序在兜底之前：同一秒里既超阈值又到兜底期时，
	 *    要按"真变化"记（否则会被兜底吃掉、漏掉给 CL 的通知）。
	 */

	/* RTT：相对 + 绝对双条件同时满足才算变化（理由见头文件注释）。 */
	rtt_delta = cur->rtt_us > snap->rtt_us ? cur->rtt_us - snap->rtt_us
					       : snap->rtt_us - cur->rtt_us;
	if (rtt_delta >= MIDR_DEBOUNCE_RTT_ABS_US) {
		/* 快照为 0（还没测到过有效值）时任何非零值都算显著变化，
		 * 同时避开除零。 */
		if (snap->rtt_us == 0)
			return MIDR_DEBOUNCE_THRESHOLD;
		if ((uint64_t)rtt_delta * 100 >=
		    (uint64_t)snap->rtt_us * MIDR_DEBOUNCE_RTT_REL_PCT)
			return MIDR_DEBOUNCE_THRESHOLD;
	}

	/* 丢包率：绝对差。 */
	if (fabs(cur->loss_rate - snap->loss_rate) >= MIDR_DEBOUNCE_LOSS_ABS)
		return MIDR_DEBOUNCE_THRESHOLD;

	/*
	 * 带宽分数【刻意不作独立判据】——07-23 实测教训，非疏漏：
	 * 现实现里 bw_score = sqrt(1.5)/(rtt × sqrt(loss))（bgp_midr_pm.c），
	 * 是 rtt 与 loss 的确定性派生量、不含独立信息。若让它独立触发，就等于给
	 * RTT 开了条绕过"绝对差 ≥1ms"免疫的旁路：内网 rtt 225→289us（+28%，绝对
	 * 才 64us 本该被挡）经 1/x 放大成 bw −22%，稳态下累计漂移即穿透 25% 阈值
	 * ——实测每几秒就假触发一次 NODE_CHANGE。而 bw 量纲跨 200 倍（跨群 271
	 * vs 内网 55000），任何绝对门槛都套不住，靠调参解决不了。
	 * 结论：rtt/loss 判无显著变化时，bw 的变化必是二者亚阈值抖动被放大的结果，
	 * 不是新信息。⚠ 若将来 bw_score 改为独立测量（真实带宽探测而非 rtt 派生），
	 * 必须回来恢复它的独立判据。
	 */

	return MIDR_DEBOUNCE_NONE;
}

/*
 * 〔件②（轮 4）删除 E-1 出口 midr_e1_write_to_bgpls() 与 shim 专用薄封装
 * midr_nds_e1_write_by_rid()：链路指标不再由我方编成 TLV 1186 挂 Link NLRI，
 * 改由 midr_nds_report_link() 交第二组的 midr_topology_link_upsert() 编码上线。〕
 *
 * 保留的是上游那两层：去抖判断（midr_nds_e1_should_send，就在上面）与上报出口
 * 本身——移交只换出口，判断留在我方（对方不希望高频小变化通知，线上共识）。
 */

void midr_nds_on_link_update(struct bgp *bgp, const struct prefix *node_id,
			     enum midr_link_status status,
			     uint32_t consecutive_failures,
			     const struct midr_nds_link_metrics *short_term,
			     const struct midr_nds_link_metrics *long_term)
{
	struct bgp_midr_nds *mi;
	struct midr_link_entry *link;
	time_t now = monotime(NULL);
	enum midr_debounce_reason reason;

	if (!bgp || !bgp->midr_nds_info || !node_id)
		return;

	mi = bgp->midr_nds_info;

	link = midr_global_view_find_link(mi->global_view, node_id);
	if (!link) {
		link = XCALLOC(MTYPE_MIDR_LINK_ENTRY, sizeof(*link));
		prefix_copy(&link->remote_node_id, node_id);
		listnode_add(mi->global_view->links, link);
	}

	/*
	 * §8.21 去抖判断必须在覆盖 link->status / short_term **之前**做——判据要
	 * 拿"本次值"跟"上次发出的快照"比，且 status 跳变判断要看新旧两个值。
	 * 本地 link_entry 照常每秒更新（CL 的 60s 热身窗、pm-test 的指标判定都读
	 * 它），去抖只门控两件对外的事：重发 Link NLRI、惊动 CL。
	 */
	reason = midr_nds_e1_should_send(link, status,
					 short_term ? short_term
						    : &link->short_term,
					 now);

	link->status = status;
	link->consecutive_failures = consecutive_failures;
	link->last_probe_time = now;
	if (short_term)
		link->short_term = *short_term;
	if (long_term)
		link->long_term = *long_term;

	/* 探测回复也是"关于它的消息"，顺手刷观测时间戳（件④ 后仅供展示与排查，
	 * 不再有老化判据读它）。 */
	if (status == MIDR_LINK_UP) {
		struct midr_node_entry key = {};
		struct midr_node_entry *node;

		key.node_id = *node_id;
		node = midr_node_hash_find(&mi->global_view->nodes, &key);
		if (node)
			node->last_update = now;
	}

	/*
	 * 无显著变化：本地视图已更新完毕，对外两件事都跳过（既不重发 Link NLRI、
	 * 也不惊动 CL）。绝大多数秒走这条路。
	 */
	if (reason == MIDR_DEBOUNCE_NONE)
		return;

	/*
	 * 下游②（对外）：把短期指标交给上报出口。
	 *
	 * 出口 = 事实表 + 第二组的 midr_topology_link_upsert()（轮 2 起如此；件②
	 * 删掉我方自有的 E-1 出口后，这已是唯一一条路）。
	 *
	 * 该不该报的判断（Established / 热身 / 键合法 / 保底边）全在出口里，
	 * 探测、I-5、去抖这三层对它一无所知。
	 */
	midr_nds_report_link(bgp, link);

	/* 发出即刻记快照：下次比较的基准。 */
	link->sent_metrics = link->short_term;
	link->sent_once = true;

	/*
	 * 下游①（对内）：链路质量真变化才值得让 CL 重新评估分群，发一条
	 * NODE_CHANGE。走到这里必是真变化（件④ 删掉周期兜底后 reason 非 NONE
	 * 即真变化），只余"稳态"一道闸——join 期（PROBING_REPS/PROBING_MEMBERS）
	 * 指标正从 0 爬向真值，每一步都算"显著变化"，会连发一串无意义通知；而
	 * 那段时间编排层本就有专门的 REP/MEMBER_PROBE_DONE 通知，CL 不缺消息。
	 *
	 * 注意：join 与稳态的分工没变——"探完一批"仍由编排层显式发 DONE，I-5 只在
	 * 稳态补上原先完全缺失的"某条链路变天了"这一路事件（CL 侧 NODE_CHANGE 分支
	 * 现为 stub，接上零风险；其稳态算法到货即可消费）。
	 */
	if (mi->join_phase == MIDR_JOIN_IDLE)
		midr_nds_notify_cl(bgp, MIDR_TRIGGER_NODE_CHANGE);

	/* TODO（未做）：status==DOWN 的失效确认（被动下线判定，见 §8）。 */
}

/* ===========================================================================
 * I-3: NDS -> CL
 * =========================================================================*/

void midr_nds_notify_cl(struct bgp *bgp, enum midr_trigger_type trigger)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;

	/*
	 * 退网守卫（退网专题步 2 之二）：退网期对 CL 完全静默。一处拦三害——
	 *   ① 退网清表时每删一个条目的 NODE_CHANGE 噪声（判据 6）；
	 *   ② periodic_sync 每 30s 的定期递交（退网后 CL 不该再评估留群/退群）；
	 *   ③ **ISOLATED 自救**：退网后会话全拆，孤岛判定必然命中，CL 会回
	 *      RECONNECT 清群重走 bootstrap join——等于把刚退的网自动拉回去。
	 * 放在 I-3 唯一出口上，上游三个触发源一道拦住，不必各自设防。
	 */
	if (mi && mi->shutdown) {
		MIDR_LOG("MIDR 退网：抑制 I-3 递交（trigger=%d，本机已退网）",
			 trigger);
		return;
	}

	if (mi && mi->cl_callback)
		mi->cl_callback(bgp, trigger, mi->global_view);
}

/* ===========================================================================
 * I-7: CL -> NDS, apply a clustering decision
 * =========================================================================*/

void midr_originate_group_update(struct bgp *bgp, uint32_t new_group_id,
				 uint32_t old_group_id)
{
	if (!bgp || !bgp->midr_nds_info)
		return;

	/* A group-0 node has no publishable topology membership.  Retire all
	 * previously active Link facts before refreshing/withdrawing its Node fact. */
	if (!new_group_id)
		midr_nds_facts_withdraw_all_links(bgp);

	bgp->midr_nds_info->local_group_id = new_group_id;
	/* B1：唯一写手，见 struct bgp_midr_nds.group_settled_at 处注释。 */
	bgp->midr_nds_info->group_settled_at = monotime(NULL);

	/* Refresh the local Node fact carrying the new group id. */
	midr_nds_report_node(bgp, MIDR_ORIGIN_GROUP_UPDATE);

	if (BGP_DEBUG(midr, MIDR))
		zlog_debug("MIDR: group-id %u -> %u; local fact refreshed", old_group_id,
			   new_group_id);
}

/*
 * join 落定：以 gid 重收敛（改群号+重通告、与新群成员建连、拆旧群残留邻接、停
 * 非本群代表探测），需要时自任首任群代表，然后回稳态清 join 状态。两个落定点
 * 共用：I-7 CREATE 分支，以及 RECOMMEND 分支里"配置群不在目录中"的自建群兜底。
 *
 * GROUP_REP 只在"落定的群里目前只有我一个"时才置。不置位则是死群——答
 * MEMBER_LIST 的闸门（bgp_midr_ctrl.c）与引导节点 rep 目录推导
 * （midr_rep_candidates）都按这个位过滤，后来者既问不到成员也发现不了这个群。
 * 反之群里已有别人（配置群号覆盖 CL 的 CREATE 时会出现）就不能贸然自任，否则
 * 一群两代表。ownership 无碍：能力位只能自己改，这里改的正是本节点自己的位。
 *
 * 判据用"群里有没有别的成员"而非"rep_dir 里有没有该群代表"：rep_dir 是某一个
 * 引导节点的二手目录、可能不全（暂存区 B-5），据它判"群不存在"会在目录漏收时
	 * 误自任；global_view 的成员来自第二组 remote view 与 MEMBER_LIST。
 * 反向漏判不存在——探代表阶段每个代表都已灌进 global_view 并带自己的群号，
 * 有代表必有成员。已知残留：群里有人却一个代表都没有（真·死群）时这里不补位，
 * 那属换届/选举语义（任务丙 I-7 选举通道），不该由一个新成员单方面自任。
 *
 * 次序：成员快照在 reconverge 前后取都一样（reconverge 只改本地群号、不改他人
 * 群号），取在前、置位在后——先落定新群号再宣称代表身份，避免中间态通告出
 * "旧群号 + 我是代表"的错误组合。
 */
static void midr_join_settle_group(struct bgp *bgp, uint32_t gid)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct list *members;
	bool alone;
	bool was_rep = !!(mi->local_capabilities & MIDR_CAP_GROUP_REP);

	/*
	 * 自身守卫（批 5c）：群号 0 = 未分群。落定 0 等于对外宣称"我是 0 号群的
	 * 代表"，会污染引导目录、把后来者引进一个不存在的群。
	 * 三个上游今天都保证 gid ≥ 1（配置群号块 / CL 的 create_gid / 批 5c 新增的
	 * 耗尽→CREATE），这道守卫是给将来第四条路留的——链条重核只保证今天对。
	 */
	if (gid == 0) {
		zlog_warn("MIDR 加入落定：群号为 0，拒绝落定（未分群，不该走到这里）");
		return;
	}

	members = list_new();
	midr_group_members(bgp, gid, members);
	alone = list_isempty(members);
	list_delete(&members);

	midr_group_reconverge(bgp, gid);

	/*
	 * `|| was_rep` 是批 6 加的（原来只有 alone）。原判据只考虑了**新节点**
	 * JOIN——"群里已经有人了，你别抢代表位"，于是**本来就是代表**的老节点重走
	 * 一遍 join 时（挂靠全灭后人工敲 midr bootstrap 就是这条路），因为群里有
	 * 成员而走 else 分支，setter 一次都不被调用，挂靠补拉也就无从触发。
	 * 放行它没有副作用：caps 值原样传进 setter，事实没变，
	 * midr_nds_report_node() 的幂等守卫（!changed && node_reported）挡住重报，
		 * 不会多发一条 Node fact；要的只是让 setter 里那道"是代表就确保达标"过一遍。
	 */
	if (alone || was_rep) {
		midr_nds_set_capability(bgp, mi->local_capabilities |
					      MIDR_CAP_GROUP_REP);
		if (was_rep)
			MIDR_FLOW_LOG("MIDR 加入落定：群 %u 本机原本就是代表，身份不变（顺带确认挂靠达标）",
				      gid);
		else
			zlog_info("MIDR 加入落定：群 %u 目前只有本节点，自任首任代表（置 GROUP_REP）",
				  gid);
	} else {
		MIDR_FLOW_LOG("MIDR 加入落定：群 %u 已有其它成员，不自任代表（避免一群两代表）",
			      gid);
	}

	/* 回稳态：清意图（此后迟到的 REP_LIST_RESP 不再重启 join）、候选群字段
	 * 回零（真身看 local_group_id，否则 `show midr join` 留错位残留）。 */
	mi->join_phase = MIDR_JOIN_IDLE;
	mi->join_in_progress = false;
	mi->join_intent = false;
	mi->join_group_id = 0;
}

/*
 * LEAVE 分支重开一轮加入；定义在本文件后段（§8.32 bootstrap 韧性一节）。
 * 【我方改动】zhc 原补丁在此前置声明的是 midr_bootstrap_start_attempt——他手写
 * 了"置意图/清 failed/游标归位/发第一跳"四步；我方 08-06 已把同一段抽成
 * midr_join_round_start（手动 bootstrap、种子自举、退网重入三处共用），改调它。
 */
static void midr_join_round_start(struct bgp *bgp); /* forward */

void midr_nds_on_cluster_decision(struct bgp *bgp,
				  const struct midr_cluster_decision *decision)
{
	struct bgp_midr_nds *mi;

	if (!bgp || !bgp->midr_nds_info || !decision)
		return;

	mi = bgp->midr_nds_info;

	/*
	 * ⑧=A 第四守卫的另一刀（5b）：CL 决策到引导上**一律忽略**。
	 * 引导不参群，本不该收到任何决策（它群号恒 0，CL 的稳态分支自带"无群跳过"）；
	 * 这道是兜底——防 CL 将来某条路径指挥引导去连普通节点，把骨干节点拖成群成员。
	 * JOIN / CREATE / RECOMMEND / REP_ELECT 一概不接。
	 */
	if (mi->local_capabilities & MIDR_CAP_BOOTSTRAP) {
		zlog_info("MIDR I-7：本机是引导节点，忽略 CL 决策 type=%d（引导不参群）",
			  decision->decision_type);
		return;
	}

	switch (decision->decision_type) {
	case MIDR_DECISION_RECOMMEND: {
		uint32_t target_gid = decision->new_group_id;
		struct ipaddr rep_transport;

		if (!midr_ipaddr_from_prefix(&decision->recommended_rep,
					       &rep_transport)) {
			zlog_warn("MIDR I-7: RECOMMEND has no valid representative locator");
			break;
		}

		/*
		 * §1.1 第一段产物：CL 选定群代表。幂等 guard——只在"探群代表"阶段
		 * 处理第一个 RECOMMEND（探测同步循环会触发多次 REP_PROBE_DONE，第一次
		 * 在此把阶段推进到 PROBING_MEMBERS，后续因阶段不符被忽略）。
		 * 待办 1：若某群代表中途下线，REP_PROBE_DONE 可能因"探不齐"而发不出
		 *         （现阶段靠"探到即推进"跑通正常流程，超时兜底后续再做）。
		 */
		if (mi->join_phase != MIDR_JOIN_PROBING_REPS) {
			MIDR_LOG("MIDR I-7：RECOMMEND 但不在探群代表阶段，忽略");
			break;
		}

		/*
		 * 手配群号优先：走 join ≠ 接受 CL 的群号安排——join 只用来打听"我的
		 * 群友在哪"，群号归属由配置决定（决策 midr-shutdown-semantics §5.2）。
		 * 判据用 config_group_id 而非 local_group_id：后者是运行值、会被 I-7
		 * 改，分不清"配的"还是"JOIN 来的"。
		 */
		if (mi->config_group_id != 0) {
			struct midr_rep_entry *r = midr_rep_dir_find_group(
				bgp, mi->config_group_id);

			if (!r) {
				/*
				 * 目录只是"某个引导节点知道的代表"、不是全网真相，
				 * 所以这只是兜底不是解法（暂存区 B-5，归 CL owner
				 * 的群生命周期语义）：以配置群号落定、结束 join，
				 * 不把流程卡死在探代表阶段。
				 */
				zlog_info("MIDR I-7：不采纳 RECOMMEND 群 %u（本地配置群 %u）；目录中无群 %u 的代表，跳过探成员直接按配置群号落定",
					  decision->new_group_id,
					  mi->config_group_id,
					  mi->config_group_id);
				midr_join_settle_group(bgp, mi->config_group_id);
				break;
			}
			target_gid = mi->config_group_id;
			rep_transport = r->rep_transport;
			MIDR_FLOW_LOG("MIDR I-7：不采纳 RECOMMEND 群 %u——按配置群 %u 走，向其代表 %pIA 要成员表",
				      decision->new_group_id, target_gid,
				      &rep_transport);
		}

		mi->join_phase = MIDR_JOIN_PROBING_MEMBERS;
		mi->join_group_id = target_gid;
		/*
		 * 此处【不】通告群号——RECOMMEND 只是选定要评估的候选群，尚未真正
		 * 入群。群号通告（TLV 1185）推迟到 JOIN（CL 判定值得入群后），避免
		 * "提前开香槟"。join_group_id 仅记录候选群，供后续探成员 / 建连用。
		 */
		/* 向群代表请求成员列表（MEMBER_LIST_REQ）。 */
		midr_ctrl_send_member_request(bgp, rep_transport, target_gid);
		MIDR_FLOW_LOG("MIDR I-7：RECOMMEND 群代表 %pIA → 进入探成员阶段，请求群 %u 成员",
			      &rep_transport, target_gid);

		/*
		 * 顺带向至多 2 个"别的群"的代表也发 MEMBER_LIST_REQ，为群间锚点
		 * 连接收集候选。先重置两个槽位——上一轮
		 * join（若有）留下的群号不能带进这一轮，否则新到达的 MEMBER_LIST_RESP
		 * 可能被误判成命中旧槽位。候选不足 2 个时未用到的槽位保持 0
		 * （cl_handle_anchor_probe_done 按 0 跳过）。
		 *
		 * 【我方改动，需知会 zhc】原实现固定取 CL 的第 2/3 名（decision->
		 * anchor_reps）当锚点候选。那是"第 1 名必定是我要进的群"这一前提下的
		 * 写法——而配了群号的节点【不采纳】CL 的群号安排（见上方手配分支），
		 * 第 1 名对它而言只是个外群，照原写法这类节点永远拿不到它，白丢一个
		 * 最优候选；更早一步，若配置群号恰好等于第 2/3 名，那个槽位还会哑火
		 * （MEMBER_LIST_RESP 分流先判"是不是我要进的群"，锚点这一路轮不到）。
		 * 改成按【目标群】剔除：候选池 = CL 前三名（第 1 名在 new_group_id +
		 * recommended_rep，第 2/3 名在 anchor_reps），剔掉 gid == target_gid
		 * 的那一个，取剩下的前 2 个。没配群号时 target_gid 就是第 1 名，剔完
		 * 剩第 2/3 名 —— 与原行为完全一致。
		 */
		mi->anchor_group_id[0] = 0;
		mi->anchor_group_id[1] = 0;
		{
			uint32_t pool_gid[3];
			struct ipaddr pool_transport[3];
			int npool = 0, slot = 0, i;

			/* 第 1 名：CL 推荐的群及其代表。 */
			pool_gid[npool] = decision->new_group_id;
			pool_transport[npool] = rep_transport;
			npool++;

			/* 第 2/3 名：CL 回灌的次优代表（借用指针，仅本次调用有效）。 */
			if (decision->anchor_reps) {
				struct listnode *an;
				struct midr_rep_entry *ar;

				for (ALL_LIST_ELEMENTS_RO(decision->anchor_reps,
							  an, ar)) {
					if (npool >= 3)
						break;
					pool_gid[npool] = ar->group_id;
					pool_transport[npool] = ar->rep_transport;
					npool++;
				}
			}

			for (i = 0; i < npool && slot < 2; i++) {
				if (pool_gid[i] == target_gid)
					continue; /* 这是我要进的群，不是搭桥对象 */
				mi->anchor_group_id[slot] = pool_gid[i];
				midr_ctrl_send_member_request(bgp,
							      pool_transport[i],
							      pool_gid[i]);
				MIDR_FLOW_LOG("MIDR I-7：RECOMMEND 附带请求次优群 %u 成员（锚点候选）",
					      pool_gid[i]);
				slot++;
			}
		}
		break;
	}
	case MIDR_DECISION_JOIN:
		/*
		 * §1.1 第二段产物：入群。幂等 guard——只在"探成员"阶段处理第一个
		 * JOIN，处理后回到稳态（IDLE），后续重复 JOIN 被忽略。
		 * 待办 1：同上，"探不齐"异常后续做超时兜底。
		 */
		if (mi->join_phase != MIDR_JOIN_PROBING_MEMBERS) {
			MIDR_LOG("MIDR I-7：JOIN 但不在探成员阶段，忽略");
			break;
		}
		/*
		 * ⑥ 入群：走与手动换组共用的重收敛编排——改群号+重通告、与新群成员
		 * 建连、并拆掉旧群残留邻接。首次分群 old_group_id==0 时无旧群可拆
		 * （reconverge 内按"邻接却已不同群"判据自然拆 0 个）；非首次（换群）
		 * 则顺带断开原群会话，补上旧版 JOIN 只建不拆的缺口。
		 */
		mi->join_members =
			midr_group_reconverge(bgp, decision->new_group_id);
		/* join 落定回稳态：清意图（此后迟到的 REP_LIST_RESP 不再重启 join），
		 * 此后探测的 I-5 回灌按 NODE_CHANGE 处理，不再是 rep/member 段。 */
		mi->join_phase = MIDR_JOIN_IDLE;
		mi->join_in_progress = false;
		mi->join_intent = false;
		mi->join_group_id = 0; /* 候选群字段回零：流程已结束，真身看 local_group_id */
		zlog_info("MIDR I-7：JOIN 群 %u 完成（经重收敛编排建连 %u 个成员），加入流程结束（回稳态）",
			  decision->new_group_id, mi->join_members);
		break;
	case MIDR_DECISION_LEAVE:
		/*
		 * 退群后清群号、自动重新走一遍加入流程——而不是停在"群号=0"
		 * 等外部决策，也不是整体下线（那是 midr shutdown 的语义）。
		 * 守卫：只在真正稳态（join_phase==IDLE）时处理，避免跟其它在
		 * 途加入/建群冲突；本节点已无群号时视为过期通知，忽略。
		 */
		if (mi->join_phase != MIDR_JOIN_IDLE) {
			MIDR_LOG("MIDR I-7：LEAVE 但不在稳态（join_phase=%d），忽略",
				 mi->join_phase);
			break;
		}
		/*
		 * 【我方补，语义变更——需知会 zhc】手配群号的节点不听 CL 的退群：
		 * 群号归属由配置决定（决策 midr-shutdown-semantics §5.2）。真退了也
		 * 白退——重开的那一轮 join 里 RECOMMEND/CREATE 两个分支都按配置群号
		 * 落定，转一圈还是回同一个群，只是白拆一遍会话。
		 * 判据用 config_group_id 而非 local_group_id：后者是运行值、会被 I-7
		 * 改，分不清"配的"还是"JOIN 来的"。
		 */
		if (mi->config_group_id != 0) {
			MIDR_LOG("MIDR I-7：忽略 LEAVE——本节点手配群号 %u，归属听配置不听 CL",
				 mi->config_group_id);
			break;
		}
		if (mi->local_group_id == 0) {
			MIDR_LOG("MIDR I-7：LEAVE 但本节点当前无群，忽略");
			break;
		}
		/*
		 * 退群只有在"退完还能重新找群"时才划算——没有引导候选就没有回
		 * 头路，reconverge(0) 会先拆掉当前（哪怕不完美但至少连通）的群
		 * 内会话，然后卡在无群状态出不来，比留在原群更差。这里提前判
		 * 断，没候选就直接放弃整个 LEAVE（不拆会话、不清群号），等运
		 * 维补 `midr bootstrap` 候选或以后有别的触发路径再说。
		 */
		if (!mi->bootstrap_list || list_isempty(mi->bootstrap_list)) {
			zlog_info("MIDR I-7：LEAVE 群 %u 但无引导候选可回退，放弃退群（留在原群好过无群）",
				  mi->local_group_id);
			break;
		}
		{
			uint32_t left_gid = mi->local_group_id;

			/* 清群号、拆本群邻接、停非本群代表探测、重通告（群号 0）。 */
			midr_group_reconverge(bgp, 0);

			/*
			 * 自动重开一轮加入，复用 §8.32 候选引导清单。
			 * 【我方改动】原补丁在此手写了"置意图/清 failed/游标归位/
			 * 发第一跳"四步；我方已把同一段抽成 midr_join_round_start
			 * （手动 bootstrap、种子自举、退网重入三处共用），改调它，
			 * 免得同一段逻辑两份。
			 */
			zlog_info("MIDR I-7：LEAVE 群 %u，自动重新加入（%u 个引导候选）",
				  left_gid, listcount(mi->bootstrap_list));
			midr_join_round_start(bgp);
		}
		break;
	case MIDR_DECISION_RECONNECT:
		/*
		 * Triggered by total connectivity loss (MIDR_TRIGGER_ISOLATED).
		 * Shares the restart-join-round logic with LEAVE, but the
		 * meaning differs: LEAVE is "the algorithm judged link
		 * quality too low and wants to switch"; this is "every
		 * session died", which isn't a quality judgement -- so it
		 * deliberately does **not** check config_group_id the way
		 * LEAVE does; a manually pinned node must still be allowed to
		 * dig itself out of total isolation (it will land back on its
		 * configured group at the end of the join round anyway).
		 * Same guards as LEAVE otherwise: only act in true steady
		 * state (avoid colliding with an in-flight join); ignore as a
		 * stale notification if the node already has no group; and
		 * give up entirely when there's no bootstrap candidate to
		 * fall back on — reconverge(0) would tear the group down and
		 * then have nowhere to go.
		 *
		 * 【我方改动】原补丁把"重开一轮加入"抽成 helper
		 * midr_restart_join_from_bootstrap()（内含 reconverge(0)，返回
		 * false 表示无候选）。我方已有等价的两个零件——midr_group_reconverge()
		 * 与 midr_join_round_start()，且 LEAVE 分支就是这么拼的——故照 LEAVE
		 * 的写法展开，不引入第二个 helper。
		 */
		if (mi->join_phase != MIDR_JOIN_IDLE) {
			MIDR_LOG("MIDR I-7：RECONNECT 但不在稳态（join_phase=%d），忽略",
				 mi->join_phase);
			break;
		}
		if (mi->local_group_id == 0) {
			MIDR_LOG("MIDR I-7：RECONNECT 但本节点当前无群，忽略");
			break;
		}
		if (!mi->bootstrap_list || list_isempty(mi->bootstrap_list)) {
			zlog_info("MIDR I-7：RECONNECT 群 %u 但无引导候选可回退，放弃（留在原群好过无群）",
				  mi->local_group_id);
			break;
		}
		{
			uint32_t left_gid = mi->local_group_id;

			midr_group_reconverge(bgp, 0);
			zlog_info("MIDR I-7：RECONNECT 群 %u 因失联触发，自动重新加入（%u 个引导候选）",
				  left_gid, listcount(mi->bootstrap_list));
			midr_join_round_start(bgp);
		}
		break;
	case MIDR_DECISION_SPLIT:
		midr_originate_group_update(bgp, decision->new_group_id,
					    decision->old_group_id);
		break;
	case MIDR_DECISION_CREATE: {
		uint32_t create_gid = decision->new_group_id;

		/*
		 * 自建新群。CREATE 有两个可能来源，二者都应被接受：
		 *   - PROBING_REPS：REP_PROBE_DONE 时没有任何群代表探到数据
		 *     （cl_handle_rep_probe_done 的"无可用候选"分支），根本没
		 *     进入过 PROBING_MEMBERS；
		 *   - PROBING_MEMBERS：所选群的成员链路数达不到入群阈值
		 *     （cl_handle_member_probe_done 的兜底分支）。
		 * 只在真正不在加入流程中（IDLE）时才视为过期/重复通知而忽略。
		 */
		if (mi->join_phase != MIDR_JOIN_PROBING_REPS &&
		    mi->join_phase != MIDR_JOIN_PROBING_MEMBERS) {
			MIDR_LOG("MIDR I-7：CREATE 但不在加入流程中，忽略");
			break;
		}

		/*
		 * 手配群号优先（同 RECOMMEND）：CL 在好链路不足时会取
		 * cl_max_group_id+1 另立新群，配了群号的节点不采纳——归属听配置，
		 * 一律以配置群号落定。
		 */
		if (mi->config_group_id != 0 &&
		    mi->config_group_id != create_gid) {
			MIDR_FLOW_LOG("MIDR I-7：不采纳 CREATE 群 %u——按配置群 %u 落定",
				      create_gid, mi->config_group_id);
			create_gid = mi->config_group_id;
		}

		/*
		 * 自建群同样走重收敛编排（与 JOIN 共用），承担两件事：
		 *   1. 改群号 + 重通告（原先手写的两行即此）；
		 *   2. **清评估期残留**——为评估候选群而灌入并探测的那批节点
		 *      （midr_nds_learn_member 置 is_adjacent=true + I-1 起探）并不属于
		 *      这个新群，reconverge 第 3 步按"邻接却已不同群"判据把它们统一
		 *      detach（I-2 停探 + 双键删 link_entry + 复位 is_adjacent），
		 *      否则 CL 稳态会把别人群的节点当成本群邻接、PM 也继续白探。
		 *      **detach 而非删除 node 条目**：p-1 全网互知下条目删了也会经泛洪
		 *      重学（白删），且跨群视图本就该在；要断的只是"邻居化"关系。
		 *      （原 TODO 措辞"清空 nodes"过度，2026-07-10 批注轮已修正为本语义。）
		 * 第 2 步 connect_group 在新群里找不到成员，自然建连 0 个；评估期未建过
		 * 会话（⑥ 先探后判），故 detach 的拆会话半边基本空转。
		 *
		 * 落定（含"群里只有我一个才自任首任代表"与回稳态清 join 状态）
		 * 统一交 midr_join_settle_group——RECOMMEND 的"配置群不在目录中"
		 * 兜底也走它，两处保持一致。
		 * TODO（仍缺）：如何主动引导其它节点加入本新群（现依赖它们各自 join 时
		 * 经引导节点目录发现本群）。
		 */
		midr_join_settle_group(bgp, create_gid);
		zlog_info("MIDR I-7：CREATE 落定群 %u，加入流程结束（回稳态）",
			  create_gid);
		break;
	}
	case MIDR_DECISION_REP_ELECT:
		/*
		 * 本节点当选群代表。CL 判定算法尚未实现（稳态优化待办），这里
		 * 先接好 NDS 侧执行：幂等检查（已是代表则短路，避免重复置位/
		 * 重复通告）→ 置 GROUP_REP 位 → 刷新拓扑事实（set_capability 内部
		 * 上报 Node fact）。能力位只改自己的，ownership 无碍。
		 */
		if (mi->local_capabilities & MIDR_CAP_GROUP_REP) {
			MIDR_LOG("MIDR I-7：REP_ELECT 但本节点已是群代表，忽略");
			break;
		}
		/*
		 * 【我方补】群号 0 不得当代表：引导节点按约定就挂群号 0，一个
		 * "群 0 的代表"会替并不存在的群应答 MEMBER_LIST 请求，还会被
		 * midr_rep_candidates 推导进代表名录、污染每个新节点收到的目录。
		 * 告警而非静默忽略：走到这里多半是 CL 侧判据漏了群号有效性。
		 */
		if (mi->local_group_id == 0) {
			zlog_warn("MIDR I-7：拒绝 REP_ELECT——本节点当前无群号（群 0 不得设代表）");
			break;
		}
		midr_nds_set_capability(bgp, mi->local_capabilities |
					      MIDR_CAP_GROUP_REP);
		zlog_info("MIDR I-7：REP_ELECT 群 %u，本节点当选代表",
			  mi->local_group_id);
		break;
	case MIDR_DECISION_REP_RESIGN:
		/*
		 * 本节点卸任群代表。幂等检查（已不是代表则短路）→ 清
		 * GROUP_REP 位 → 通告全网。
		 */
		if (!(mi->local_capabilities & MIDR_CAP_GROUP_REP)) {
			MIDR_LOG("MIDR I-7：REP_RESIGN 但本节点当前不是群代表，忽略");
			break;
		}
		midr_nds_set_capability(bgp, mi->local_capabilities &
					      ~MIDR_CAP_GROUP_REP);
		/*
		 * 【我方补】清位只改能力位，而组装 REP_LIST_RESP 的来源一
		 * （mi->rep_dir 全量排前）【不看能力位】——名录里若还留着自己
		 * 那条，卸任后仍会对外宣称自己是代表。按 (本群, 本机 transport)
		 * 删掉自己那条。只做这一刀：来源一/来源二的整体结构留到对接第
		 * 二组、来源二改成从他们视图取数时一起审视（核对档 A-3 乙）。
		 */
		if (mi->transport_active &&
		    midr_ipaddr_valid_locator(&mi->active_transport_addr))
			midr_rep_dir_del(bgp, mi->local_group_id,
					 mi->active_transport_addr);
		zlog_info("MIDR I-7：REP_RESIGN 群 %u，本节点卸任代表",
			  mi->local_group_id);
		break;
	case MIDR_DECISION_ANCHOR: {
		/*
		 * CL 选出的至多 4 个锚点候选（decision->evidence，仅
		 * node_id+metrics）。逐条按 node_id 反查
		 * global_view 拿完整 entry（锚点候选此前已经
		 * midr_nds_learn_anchor_candidate 灌过表，一定能查到），直接
		 * midr_ctrl_connect()——与 connect_group 建群内会话同一原语，
		 * 天然带 S3/S5 那几道去重/归属守卫，不需要另外处理。
		 */
		struct listnode *en;
		struct midr_node_evidence *ev;
		uint32_t connected = 0;

		if (decision->evidence)
			for (ALL_LIST_ELEMENTS_RO(decision->evidence, en, ev)) {
				struct midr_node_entry key = {};
				struct midr_node_entry *entry;

				key.node_id = ev->node_id;
				entry = midr_node_hash_find(
					&mi->global_view->nodes, &key);
				if (!entry) {
					MIDR_LOG("MIDR I-7：ANCHOR 候选 %pFX 不在 global_view，跳过",
						 &ev->node_id);
					continue;
				}
				/* 台账：CL 群间锚点边——这条边对端不同群，
				 * should_peer 推不出来，清理时全靠账认。
				 * 【我方改动，需知会 zhc】本行原为两参调用，
				 * 现为四参（末参 = 发 nudge，发起类传 true）。 */
				midr_ctrl_connect(bgp, entry,
						  MIDR_SESSION_CL_ANCHOR,
						  true);
				connected++;
			}
		zlog_info("MIDR I-7：ANCHOR 处理 %u 个锚点候选，尝试建连 %u 个",
			  decision->evidence
				  ? (uint32_t)listcount(decision->evidence)
				  : 0,
			  connected);

		/*
		 * 锚点数据已经消费完，收口 reconverge 第 4 步为它俩延后的停探
		 * （见 midr_group_reconverge 头注释"例外"那段），并把槽位清零——
		 * 不清的话下一轮 RECOMMEND 判断"是否还留着"会被这轮的旧群号
		 * 误伤（虽然 RECOMMEND 分支本身也会先清零，这里是双保险，两条
		 * 路径各自独立维护自己负责的状态更清楚）。
		 */
		{
			struct listnode *rn;
			struct midr_rep_entry *r;

			for (ALL_LIST_ELEMENTS_RO(mi->rep_dir, rn, r))
				if (r->group_id == mi->anchor_group_id[0] ||
				    r->group_id == mi->anchor_group_id[1])
					midr_stop_rep_probe_entry(bgp, r);
		}

		/*
		 * 【我方补，需知会 zhc】上面那圈只收了次优群的【代表】；
		 * MEMBER_LIST_RESP 灌进来的【普通成员】同样起了探测
		 * （midr_nds_learn_anchor_candidate → I-1），而它们既不在 rep_dir
		 * （上面那圈扫不到）、又不带 is_adjacent（换群清理扫不到），PM 探测
		 * 不收口的话，每评估一次跨群就永久多养一批探测，且这些带着陈旧
		 * 群号的条目会污染 midr_group_members()（它只匹配群号、不看活性
		 * 与邻接），让"落定的群里是不是只有我"判错、该自任代表时不自任
		 * → 死群。
		 * 条目本身归第二组回收（件④ 后 withdraw 删条目、update 纠偏群号），
		 * 此处不必手动删；⚠ 对方撤销传不到时条目会滞留，属新语义的已知形态。
		 *
		 * 【例外】本轮选中的锚点（decision->evidence）不能停：它们刚建了会话，
		 * 是真正的跨群链路，指标要持续测给 CL 用。判据【不能只写】!is_adjacent
		 * —— midr_ctrl_connect 不置该标记（zhc 有意为之，免得污染 CL 的本群
		 * 链路统计），只靠它会把刚选中的对象一起停掉。
		 * 已知残留（留给 zhc 的 ANCHOR 生命周期设计）：被选中者将来若搭桥关系
		 * 解除，仍没有任何路径停它们的探测。
		 */
		{
			struct midr_node_entry *e;

			frr_each_safe (midr_node_hash, &mi->global_view->nodes,
				       e) {
				struct listnode *cn;
				struct midr_node_evidence *cev;
				bool chosen = false;

				if (e->is_self || e->is_adjacent)
					continue;
				if (e->group_id != mi->anchor_group_id[0] &&
				    e->group_id != mi->anchor_group_id[1])
					continue;

				if (decision->evidence)
					for (ALL_LIST_ELEMENTS_RO(decision->evidence,
								  cn, cev))
						if (prefix_same(&cev->node_id,
								&e->node_id)) {
							chosen = true;
							break;
						}
				if (chosen)
					continue;

				midr_nds_detach_node(bgp, e,
						     MIDR_STOP_CLUSTER_CHANGE,
						     false);
			}
		}
		mi->anchor_group_id[0] = 0;
		mi->anchor_group_id[1] = 0;
		break;
	}
	}
}

/* ===========================================================================
 * Group-representative directory
 *
 * mi->rep_dir is runtime state rebuilt from REP_LIST_RESP.  When a bootstrap
 * answers, the Control layer may merge this cache with representatives derived
 * from the current remote view; there is no persisted `midr rep group` source.
 * =========================================================================*/

void midr_rep_dir_add(struct bgp *bgp, uint32_t group_id,
		      struct ipaddr rep_transport, as_t rep_asn,
		      struct in_addr rep_rid)
{
	struct bgp_midr_nds *mi;
	struct listnode *node;
	struct midr_rep_entry *r;

	if (!bgp || !bgp->midr_nds_info || !bgp->midr_nds_info->rep_dir ||
	    !group_id || rep_rid.s_addr == INADDR_ANY ||
	    !midr_ipaddr_valid_locator(&rep_transport))
		return;
	mi = bgp->midr_nds_info;

	/* Dedup on (group_id, rep_transport); refresh the v4-mandatory identity
	 * and ASN if it already exists. */
	for (ALL_LIST_ELEMENTS_RO(mi->rep_dir, node, r))
		if (r->group_id == group_id &&
		    midr_ipaddr_same(&r->rep_transport, &rep_transport)) {
			r->rep_asn = rep_asn;
			r->rep_rid = rep_rid;
			return;
		}

	r = XCALLOC(MTYPE_MIDR_REP_ENTRY, sizeof(*r));
	r->group_id = group_id;
	r->rep_transport = rep_transport;
	r->rep_asn = rep_asn;
	r->rep_rid = rep_rid;
	listnode_add(mi->rep_dir, r);
}

bool midr_rep_dir_del(struct bgp *bgp, uint32_t group_id,
		      struct ipaddr rep_transport)
{
	struct bgp_midr_nds *mi;
	struct listnode *node, *nnode;
	struct midr_rep_entry *r;

	if (!bgp || !bgp->midr_nds_info || !bgp->midr_nds_info->rep_dir)
		return false;
	mi = bgp->midr_nds_info;

	for (ALL_LIST_ELEMENTS(mi->rep_dir, node, nnode, r))
		if (r->group_id == group_id &&
		    midr_ipaddr_same(&r->rep_transport, &rep_transport)) {
			list_delete_node(mi->rep_dir, node);
			XFREE(MTYPE_MIDR_REP_ENTRY, r);
			return true;
		}
	return false;
}

void midr_rep_dir_clear(struct bgp *bgp)
{
	struct bgp_midr_nds *mi;
	struct listnode *node, *nnode;
	struct midr_rep_entry *r;

	if (!bgp || !bgp->midr_nds_info || !bgp->midr_nds_info->rep_dir)
		return;
	mi = bgp->midr_nds_info;

	for (ALL_LIST_ELEMENTS(mi->rep_dir, node, nnode, r)) {
		list_delete_node(mi->rep_dir, node);
		XFREE(MTYPE_MIDR_REP_ENTRY, r);
	}
}

struct midr_rep_entry *midr_rep_dir_find_group(struct bgp *bgp,
					       uint32_t group_id)
{
	struct bgp_midr_nds *mi;
	struct listnode *node;
	struct midr_rep_entry *r;

	if (!bgp || !bgp->midr_nds_info || !bgp->midr_nds_info->rep_dir)
		return NULL;
	mi = bgp->midr_nds_info;

	for (ALL_LIST_ELEMENTS_RO(mi->rep_dir, node, r))
		if (r->group_id == group_id)
			return r;
	return NULL;
}

/*
 * "Table A": every non-self node in `group_id`, derived from the MIDR global
 * view.  Single source of truth — when MIDR-LS propagation scoping changes,
 * only this function changes.  `out` collects borrowed entry pointers (caller
 * owns the list; entries stay owned by the hash).
 */
void midr_group_members(struct bgp *bgp, uint32_t group_id, struct list *out)
{
	struct midr_node_entry *entry;

	if (!bgp || !bgp->midr_nds_info || !out)
		return;

	frr_each (midr_node_hash, &bgp->midr_nds_info->global_view->nodes, entry) {
		if (entry->is_self)
			continue;
		if (entry->group_id != group_id)
			continue;
		listnode_add(out, entry);
	}
}

/*
 * rep 目录推导（任务甲）：收集有资格进 REP_LIST 应答的节点——GROUP_REP 位
 * + 活性 + 非零群号 + 显式 transport。含 self（本机作为群代表时）。
 * 不走 midr_node_get_locator 回落：router-id 可能不可路由，rep 目录是新
 * 节点入网第一跳，宁缺毋黑洞；MEMBER_LIST 同样禁止回落。
 * `out` 收 borrowed 指针（list 归调用方，条目归 hash）。
 * 目前唯一消费者 = midr_ctrl_build_rep_list；未来 bootstrap failover /
 * 目录持久化复用。当前候选来自第二组 remote view 驱动的 global_view。
 */
void midr_rep_candidates(struct bgp *bgp, struct list *out)
{
	struct midr_node_entry *entry;

	if (!bgp || !bgp->midr_nds_info || !out)
		return;

	frr_each (midr_node_hash, &bgp->midr_nds_info->global_view->nodes, entry) {
		if (!midr_node_is_group_rep(entry))
			continue;
		/*
		 * 件④：删掉了这里的活性兜底（`now - last_seen > 15s` 即跳过）。
		 * 条目在表就算活着，生死归第二组的 withdraw 回调 —— 原先那道
		 * 兜底在换源后会把稳态代表全滤掉、rep 目录恒空，新节点 join
		 * 拿不到目录（记档 57）。⚠ 别拿 last_update 把它加回来。
		 */
		/* asn 不再列为必需（轮 4 放宽，见 midr_ctrl_connect）。 */
		if (entry->group_id == 0 || !entry->has_transport_addr) {
			MIDR_LOG("MIDR: rep 候选 %pI4 跳过 (group=%u asn=%u has_transport=%d)",
				 &entry->node_id.u.prefix4, entry->group_id,
				 entry->asn, entry->has_transport_addr);
			continue;
		}
		listnode_add(out, entry);
	}
}

/* ===========================================================================
 * Read-only getters (for external modules, e.g. ④ flooding control)
 * =========================================================================*/

struct midr_global_view *midr_get_global_view(struct bgp *bgp)
{
	if (!bgp || !bgp->midr_nds_info)
		return NULL;
	return bgp->midr_nds_info->global_view;
}

uint32_t midr_local_group_id(struct bgp *bgp)
{
	if (!bgp || !bgp->midr_nds_info)
		return 0;
	return bgp->midr_nds_info->local_group_id;
}

bool midr_node_group_id(struct bgp *bgp, const struct prefix *node_id,
			uint32_t *out)
{
	struct midr_node_entry key = {};
	struct midr_node_entry *entry;

	if (!bgp || !bgp->midr_nds_info || !node_id || !out)
		return false;

	prefix_copy(&key.node_id, node_id);
	entry = midr_node_hash_find(&bgp->midr_nds_info->global_view->nodes, &key);
	if (!entry)
		return false;

	*out = entry->group_id;
	return true;
}

bool midr_nds_local_transport_get(const struct bgp *bgp,
				  struct ipaddr *transport)
{
	const struct bgp_midr_nds *mi;
	struct prefix self_id;

	if (!transport)
		return false;
	SET_IPADDR_NONE(transport);
	if (!bgp || !bgp->midr_nds_info)
		return false;

	mi = bgp->midr_nds_info;
	if (!mi->transport_active ||
	    !midr_ipaddr_valid_locator(&mi->active_transport_addr))
		return false;
	midr_prefix_from_in_addr(&self_id, bgp->router_id);
	if (!midr_nds_locator_unique(bgp, &self_id, &mi->active_transport_addr))
		return false;

	*transport = mi->active_transport_addr;
	return true;
}

bool midr_nds_node_transport_get(const struct bgp *bgp,
				 const struct prefix *node_id,
				 struct ipaddr *transport)
{
	struct midr_node_entry key = {};
	struct midr_node_entry *entry;
	struct ipaddr local;

	if (!transport)
		return false;
	SET_IPADDR_NONE(transport);
	if (!bgp || !node_id || !bgp->midr_nds_info ||
	    !bgp->midr_nds_info->global_view)
		return false;

	prefix_copy(&key.node_id, node_id);
	entry = midr_node_hash_find(&bgp->midr_nds_info->global_view->nodes,
				    &key);
	if (!entry || !entry->has_transport_addr ||
	    !midr_ipaddr_valid_locator(&entry->transport_addr) ||
	    !midr_nds_local_transport_get(bgp, &local) ||
	    ipaddr_family(&local) != ipaddr_family(&entry->transport_addr) ||
	    !midr_nds_locator_unique(bgp, node_id, &entry->transport_addr))
		return false;

	*transport = entry->transport_addr;
	return true;
}

static struct midr_manual_session *
midr_manual_session_find(struct bgp_midr_nds *mi, struct ipaddr transport)
{
	struct listnode *node;
	struct midr_manual_session *session;

	if (!mi || !mi->manual_sessions)
		return NULL;
	for (ALL_LIST_ELEMENTS_RO(mi->manual_sessions, node, session))
		if (midr_ipaddr_same(&session->transport, &transport))
			return session;
	return NULL;
}

/* A manual session is configured by locator + ASN, so its RID can be unknown
 * until the first successful BGP OPEN.  Bind that learned identity only after
 * applying the same single-locator constraints as control-plane discoveries;
 * never let an ambiguous OPEN silently rewrite the authoritative node view. */
static bool midr_manual_session_note_identity(struct bgp *bgp,
					      struct ipaddr transport,
					      struct in_addr remote_rid)
{
	struct bgp_midr_nds *mi;
	struct midr_manual_session *session;
	struct midr_session_ledger_entry *ledger;
	struct midr_node_entry key = {};
	struct midr_node_entry *existing;

	if (!bgp || !bgp->midr_nds_info)
		return false;
	mi = bgp->midr_nds_info;
	session = midr_manual_session_find(mi, transport);
	if (!session)
		return true;
	if (remote_rid.s_addr == INADDR_ANY ||
	    midr_nds_is_session_excluded(bgp, remote_rid))
		return false;

	midr_prefix_from_in_addr(&key.node_id, remote_rid);
	existing = midr_node_hash_find(&mi->global_view->nodes, &key);
	if (!midr_nds_control_locator_usable(bgp, &key.node_id, existing,
					     &transport, "手工会话身份回填"))
		return false;

	session->remote_rid = remote_rid;
	ledger = midr_ledger_find(mi, transport);
	if (ledger && ledger->reason == MIDR_SESSION_MANUAL)
		ledger->remote_rid = remote_rid;
	return true;
}

void midr_nds_manual_session_set(struct bgp *bgp, struct ipaddr transport,
				 as_t remote_asn,
				 struct in_addr remote_rid)
{
	struct bgp_midr_nds *mi;
	struct midr_manual_session *session;

	if (!bgp || !bgp->midr_nds_info ||
	    !midr_ipaddr_valid_locator(&transport) || !remote_asn)
		return;
	mi = bgp->midr_nds_info;
	session = midr_manual_session_find(mi, transport);
	if (!session) {
		session = XCALLOC(MTYPE_MIDR_MANUAL_SESSION, sizeof(*session));
		session->transport = transport;
		listnode_add(mi->manual_sessions, session);
	}
	session->remote_asn = remote_asn;
	/* A repeated command may now describe a different peer behind the same
	 * locator.  Do not retain an identity learned by an older generation when
	 * the current peer/node table cannot resolve it. */
	session->remote_rid = remote_rid;
}

bool midr_nds_manual_session_del(struct bgp *bgp, struct ipaddr transport)
{
	struct bgp_midr_nds *mi;
	struct listnode *node, *nnode;
	struct midr_manual_session *session;

	if (!bgp || !bgp->midr_nds_info)
		return false;
	mi = bgp->midr_nds_info;
	if (!mi->manual_sessions)
		return false;

	for (ALL_LIST_ELEMENTS(mi->manual_sessions, node, nnode, session)) {
		if (!midr_ipaddr_same(&session->transport, &transport))
			continue;
		list_delete_node(mi->manual_sessions, node);
		XFREE(MTYPE_MIDR_MANUAL_SESSION, session);
		return true;
	}
	return false;
}

void midr_nds_manual_sessions_restore(struct bgp *bgp)
{
	struct bgp_midr_nds *mi;
	struct listnode *node;
	struct midr_manual_session *session;
	struct ipaddr local;

	if (!bgp || !bgp->midr_nds_info)
		return;
	mi = bgp->midr_nds_info;
	if (mi->shutdown || mi->transport_reconfiguring ||
	    !midr_nds_local_transport_get(bgp, &local))
		return;

	for (ALL_LIST_ELEMENTS_RO(mi->manual_sessions, node, session)) {
		struct midr_node_entry target = {};
		struct in_addr resolved_rid;

		if (!midr_ipaddr_valid_locator(&session->transport) ||
		    ipaddr_family(&session->transport) != ipaddr_family(&local))
			continue;
		/* frr.conf stores a manual session as locator + ASN, so its cached
		 * RID is normally zero after restart.  If the authoritative node
		 * directory is already populated, resolve it before the uniqueness
		 * gate; otherwise RID 0 remains the deliberate pre-OPEN MANUAL case. */
		resolved_rid = midr_nds_rid_by_transport(bgp,
						    session->transport);
		if (resolved_rid.s_addr != INADDR_ANY)
			session->remote_rid = resolved_rid;
		target.node_id.family = AF_INET;
		target.node_id.prefixlen = IPV4_MAX_BITLEN;
		target.node_id.u.prefix4 = session->remote_rid;
		target.transport_addr = session->transport;
		target.has_transport_addr = true;
		target.asn = session->remote_asn;
		midr_ctrl_connect(bgp, &target, MIDR_SESSION_MANUAL, false);
	}
}

bool midr_nds_locator_unique(const struct bgp *bgp,
			     const struct prefix *owner,
			     const struct ipaddr *transport)
{
	struct midr_node_entry *entry;

	if (!bgp || !bgp->midr_nds_info ||
	    !bgp->midr_nds_info->global_view || !owner ||
	    owner->family != AF_INET ||
	    owner->prefixlen != IPV4_MAX_BITLEN ||
	    !midr_ipaddr_valid_locator(transport))
		return false;

	frr_each (midr_node_hash, &bgp->midr_nds_info->global_view->nodes,
		  entry) {
		if (prefix_same(&entry->node_id, owner) ||
		    !entry->has_transport_addr ||
		    !midr_ipaddr_valid_locator(&entry->transport_addr))
			continue;
		if (midr_ipaddr_same(&entry->transport_addr, transport))
			return false;
	}
	return true;
}

/*
 * 按 transport 地址反查节点真名（router-id）。供 `no midr session` 在 BGP
 * OPEN 尚未提供 remote_id 时，把运维输入的 locator 解析回稳定身份；查不到
 * 返回 0。Control v4 列表不使用本回落，线上 RID 必须非 0。
 */
struct in_addr midr_nds_rid_by_transport(struct bgp *bgp,
					 struct ipaddr transport)
{
	struct in_addr zero = { .s_addr = INADDR_ANY };
	struct in_addr found = { .s_addr = INADDR_ANY };
	struct midr_node_entry *entry;

	if (!bgp || !bgp->midr_nds_info ||
	    !bgp->midr_nds_info->global_view ||
	    !midr_ipaddr_valid_locator(&transport))
		return zero;

	frr_each (midr_node_hash, &bgp->midr_nds_info->global_view->nodes, entry) {
		if (!entry->has_transport_addr ||
		    !midr_ipaddr_valid_locator(&entry->transport_addr))
			continue;
		if (entry->node_id.family != AF_INET ||
		    entry->node_id.prefixlen != IPV4_MAX_BITLEN ||
		    entry->node_id.u.prefix4.s_addr == INADDR_ANY)
			continue;
		if (!midr_ipaddr_same(&entry->transport_addr, &transport))
			continue;
		/* A reverse lookup is usable only when the locator identifies one
		 * stable RID.  Treat an inconsistent directory as unresolved rather
		 * than selecting whichever hash entry happens to be visited first. */
		if (found.s_addr != INADDR_ANY &&
		    found.s_addr != entry->node_id.u.prefix4.s_addr)
			return zero;
		found = entry->node_id.u.prefix4;
	}
	return found;
}

/*
 * 同款反查的候选池版（批 5 R 系列）：按 transport 在 bootstrap 候选池里找 rid。
 * 它是 `no midr session <IP>` 反查链的**第三级**——前两级（会话 remote_id、
 * 节点表）对引导节点不保证翻得到：专职引导通常没有 remote-view Node 条目，挂靠没建成
 * 的半边也没有 remote_id。池内每条都带 rid（"rid 恒非 0"不变量），正好补上。
 * 查不到返回 0。
 */
struct in_addr midr_nds_bootstrap_rid_by_transport(struct bgp *bgp,
						   struct ipaddr transport)
{
	struct in_addr zero = { .s_addr = INADDR_ANY };
	struct midr_bootstrap_entry *b;

	if (!bgp || !bgp->midr_nds_info)
		return zero;

	b = midr_bootstrap_find(bgp->midr_nds_info, transport, NULL);
	return b ? b->rid : zero;
}

/*
 * ⑦ `no midr neighbor` 清账入口：按地址（会话对端 = 某节点 locator）反查节点表
 * 条目，查到则走 midr_nds_detach_node 全套（I-2 停探 + 双键删 link + 清
 * is_adjacent + 拆会话），返回 true；查不到返回 false（调用方退化为只拆会话）。
 *
 * 只按显式 transport 比对；router-id 是身份，绝不能回落成套接字目标。detach
 * 内部保持 static——只经此 wrapper 对 VTY 暴露一个动作，不把停探/清账的原语
 * 散出去。
 */
bool midr_nds_detach_by_locator(struct bgp *bgp, struct ipaddr addr)
{
	struct midr_node_entry *entry;

	if (!bgp || !bgp->midr_nds_info)
		return false;

	frr_each (midr_node_hash, &bgp->midr_nds_info->global_view->nodes, entry) {
		if (!entry->has_transport_addr ||
		    !midr_ipaddr_valid_locator(&entry->transport_addr))
			continue;
		if (midr_ipaddr_same(&entry->transport_addr, &addr)) {
			midr_nds_detach_node(bgp, entry,
					     MIDR_STOP_GRACEFUL_SHUTDOWN, true);
			return true;
		}
	}
	return false;
}

/* ===========================================================================
 * New-node join (bootstrap) — hierarchical discovery
 *
 * Stage 0: `midr bootstrap <locator> ...` -> REP_LIST_REQ to the bootstrap (TCP
 *          list exchange).
 * Stage 1 (midr_join_on_rep_list): probe reps (I-1), pick a group, then
 *          MEMBER_LIST_REQ to that group's representative (TCP).
 * Stage 3 (in bgp_midr_ctrl.c, on MEMBER_LIST_RESP): connect every member
 *          (the representative included).
 * No MIDR-LS session is opened to the bootstrap, so there is no full-table dump.
 * =========================================================================*/

/* ---------------------------------------------------------------------------
 * §8.32 bootstrap 候选清单（多候选 + failover）
 * 元素为 struct midr_bootstrap_entry；次序即尝试优先级：手配（MANUAL，按敲入
 * 顺序）在前、种子（SEED，§8.31 重启读回）在后。游标 bootstrap_cur 指向本轮
 * 正在尝试第一跳的候选；REP_LIST_REQ 重试耗尽时 ctrl 层回调
 * midr_join_bootstrap_failed()，游标后移换下一候选，全部耗尽才放弃。
 * ------------------------------------------------------------------------- */

static struct midr_bootstrap_entry *
midr_bootstrap_find(struct bgp_midr_nds *mi, struct ipaddr addr,
		    struct listnode **node_out)
{
	struct listnode *node;
	struct midr_bootstrap_entry *b;

	for (ALL_LIST_ELEMENTS_RO(mi->bootstrap_list, node, b))
		if (midr_ipaddr_same(&b->transport, &addr)) {
			if (node_out)
				*node_out = node;
			return b;
		}
	return NULL;
}

/* 候选在清单中的 1 起序号（日志/展示用）。 */
static unsigned int midr_bootstrap_index(struct bgp_midr_nds *mi,
					 const struct listnode *target)
{
	struct listnode *node;
	unsigned int idx = 0;

	for (node = listhead(mi->bootstrap_list); node;
	     node = listnextnode(node)) {
		idx++;
		if (node == target)
			return idx;
	}
	return 0;
}

/*
 * 追加/刷新候选（去重键 = transport）。已存在时 MANUAL 可覆盖 SEED；SEED
 * 不得改写 MANUAL 的 ASN/RID，否则一份远端名单会悄悄改变并持久化运维意图。
 * 条目位置不动（移动会使游标悬空）；新增时 MANUAL 插在首个 SEED 之前、SEED
 * 追加到尾，维持"手配在前、种子在后"。
 */
static void midr_bootstrap_list_add(struct bgp_midr_nds *mi, struct ipaddr addr,
				    as_t asn, struct in_addr rid,
				    enum midr_bootstrap_source source)
{
	struct midr_bootstrap_entry *b;
	struct listnode *node;

	/*
	 * 入口把关（批 5 R 系列）：无名条目一律不进池。池内"rid 恒非 0"是环排序、
	 * 拉黑、台账三处的共同前提，破一处就得处处加兜底。
	 * ⚠ 这不是在处理某个现实场景——全网同批换二进制 + 部署删库之后没有 rid=0
	 * 的来源；它是**协议收口的规矩**（收侧永不吸收无名条目），防将来实现失误
	 * 或坏包，出事时有日志可查。别把它当死代码删掉，也别据此以为 rid 可以为 0。
	 */
	if (!mi || !midr_ipaddr_valid_locator(&addr)) {
		zlog_warn("MIDR bootstrap：拒收无效 locator %pIA", &addr);
		return;
	}
	if (rid.s_addr == INADDR_ANY) {
		zlog_warn("MIDR bootstrap：拒收无 router-id 的候选 %pIA（AS %u）——引导候选必须带 rid（对端版本过旧？）",
			  &addr, asn);
		return;
	}

	b = midr_bootstrap_find(mi, addr, NULL);
	if (b) {
		if (b->source == MIDR_BOOTSTRAP_MANUAL &&
		    source == MIDR_BOOTSTRAP_SEED)
			return;
		b->asn = asn;
		b->rid = rid;
		if (source == MIDR_BOOTSTRAP_MANUAL)
			b->source = MIDR_BOOTSTRAP_MANUAL;
		return;
	}

	b = XCALLOC(MTYPE_MIDR_BOOTSTRAP_ENTRY, sizeof(*b));
	b->transport = addr;
	b->asn = asn;
	b->rid = rid;
	b->source = source;

	if (source == MIDR_BOOTSTRAP_MANUAL) {
		struct midr_bootstrap_entry *cur;

		for (ALL_LIST_ELEMENTS_RO(mi->bootstrap_list, node, cur))
			if (cur->source == MIDR_BOOTSTRAP_SEED) {
				listnode_add_before(mi->bootstrap_list, node, b);
				return;
			}
	}
	listnode_add(mi->bootstrap_list, b);
}

/* 向游标当前候选发第一跳请求（REP_LIST_REQ，经 TCP 列表交换 + pending 重试）。 */
static void midr_bootstrap_start_attempt(struct bgp *bgp)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct midr_bootstrap_entry *b;
	struct ipaddr local;

	/* Keep the cursor parked while no local endpoint is active.  Transport
	 * reconcile will restart the round after a successful bind. */
	if (!midr_nds_local_transport_get(bgp, &local))
		return;
	while (mi->bootstrap_cur) {
		b = listgetdata(mi->bootstrap_cur);
		if (midr_ipaddr_valid_locator(&b->transport) &&
		    ipaddr_family(&b->transport) == ipaddr_family(&local))
			break;
		b->failed = true;
		mi->bootstrap_cur = listnextnode(mi->bootstrap_cur);
	}
	if (!mi->bootstrap_cur) {
		zlog_warn("MIDR JOIN: no bootstrap candidate matches active locator family %u",
			  ipaddr_family(&local));
		return;
	}
	b = listgetdata(mi->bootstrap_cur);
	midr_ctrl_send_rep_request(bgp, b->transport);
	MIDR_FLOW_LOG("MIDR JOIN: sent REP_LIST_REQ to bootstrap %pIA [%s]（第 %u/%u 个候选）",
		      &b->transport,
		      b->source == MIDR_BOOTSTRAP_SEED ? "seed" : "manual",
		      midr_bootstrap_index(mi, mi->bootstrap_cur),
		      listcount(mi->bootstrap_list));
}

/*
 * 开新一轮加入：表意图 → 清各候选 failed 标记 → 游标指表头 → 向首个候选发第
 * 一跳。三个入口共用：手动 `midr bootstrap`、§8.31 种子自举，以及退网专题落地
 * 后的 `no midr shutdown`（重新参与 = 走与新节点完全相同的 join，见决策
 * midr-shutdown-semantics §3 C-2）。
 */
static void midr_join_round_start(struct bgp *bgp)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct listnode *node;
	struct midr_bootstrap_entry *b;

	/*
	 * ⑧=A 第四守卫（5b）：引导节点不入群、不 join。
	 * 落在本函数入口，是因为它是**唯一的 round 起点**——手动 `midr bootstrap`、
	 * §8.31 种子自举、退网专题落地后的 `no midr shutdown` 三条路都到这儿汇合，
	 * 而 §8.32 failover 的换台重试全在一个 round 之内；round 开不了就没有下游，
	 * 一刀全堵，不必去各入口分别设防。
	 * 效果 = 引导上敲 `midr bootstrap` 只把候选入列（骨干名单要用），不触发加入。
	 */
	if (mi->local_capabilities & MIDR_CAP_BOOTSTRAP) {
		zlog_info("MIDR JOIN：本机是引导节点，不发起加入（引导专职化：只当引导、不入群）");
		return;
	}

	/*
	 * 挂点③（配套一）：开新一轮 join = 从头再来，上一段人生留下的锚点评估上下文
	 * 一律作废。放在这里一处，四个 round 入口（手动 bootstrap / 种子自举 /
	 * 退网重入 / I-7 LEAVE 后自动重入）全覆盖。
	 */
	midr_nds_anchor_ctx_clear(bgp);

	/* 表达一个尚未落定的加入意图：REP_LIST_RESP 到达时凭此放行进入 join。
	 * 手动换组会作废它，届时迟到的响应被丢弃（见 midr_nds_set_group_id）。 */
	mi->join_intent = true;

	for (ALL_LIST_ELEMENTS_RO(mi->bootstrap_list, node, b))
		b->failed = false;
	mi->bootstrap_cur = listhead(mi->bootstrap_list);
	midr_bootstrap_start_attempt(bgp);
}

/* §8.31 种子读回回调：把库里一条种子（transport 字符串 + asn）追加进候选清单，
 * 标 SEED 来源（排手配之后）。midr_store_seed_load 逐条调本函数。
 * last_seen 本路用不上（候选次序由 MANUAL/SEED 分组决定，不按时间排）——库里
 * 的行本就按 last_seen 新→旧吐出，同组内自然新的在前。 */
static void midr_bootstrap_seed_load_cb(const char *transport, uint32_t asn,
					const char *rid, time_t last_seen,
					void *arg)
{
	struct bgp *bgp = arg;
	struct ipaddr locator;
	struct in_addr rid_addr;

	(void)last_seen;

	if (str2ipaddr(transport, &locator) != 0)
		return;
	/* rid 列解析不出来（旧库遗留行/脏数据）→ 跳过：宁少一个候选，也不把无名
	 * 条目塞进池（"池内 rid 恒非 0"不变量，批 5 R 系列）。 */
	if (!rid || inet_pton(AF_INET, rid, &rid_addr) != 1)
		return;
	midr_bootstrap_list_add(bgp->midr_nds_info, locator, (as_t)asn,
				rid_addr, MIDR_BOOTSTRAP_SEED);
}

/*
 * §8.31 种子自举定时器（启动后延迟 MIDR_BOOTSTRAP_SELF_BOOT_SECS 触发一次）：
 * 断电重启后 `midr bootstrap` 一次性命令已随进程消失、无人再敲，节点会永远闲
 * 着。这里在启动初期用读回的种子自动发起一轮加入——两关全过才动：
 *   ① 候选清单非空（全新机器无种子 → 不动，与现状一致）；
 *   ② 无人已手动敲过 `midr bootstrap`（join_intent 未起）——人工优先。
 * 过关后走与手动入网一样的流程，失败同样按 §8.32 failover 换下一颗种子。
 *
 * 原第②关"本地无群号（local_group_id==0）即不自举"已删（2026-08-06）：走 join
 * ≠ 接受 CL 的群号安排——手配群号的节点同样需要 join 来打听"我的群友在哪"，
 * 群号归属改由 config_group_id 在 I-7 分支上把关（midr_nds_on_cluster_decision
 * 的 RECOMMEND/CREATE 两处不采纳 CL 群号）。留着它的害处是：配了群号又无静态
 * 邻居的节点重启后连一条 overlay 会话都建不起来，直接孤岛。
 */
static void midr_bootstrap_self_boot_cb(struct event *t)
{
	struct bgp *bgp = EVENT_ARG(t);
	struct bgp_midr_nds *mi = bgp->midr_nds_info;

	if (!mi->bootstrap_list || list_isempty(mi->bootstrap_list))
		return;
	if (mi->join_intent)
		return;
	/* 引导不自举。真正的闸在 midr_join_round_start（第四守卫主刀），这条提前
	 * return 只为让日志说清"被挡的是自举这一路"——否则运维只看到 round_start
	 * 那句，分不出是谁触发的。 */
	if (mi->local_capabilities & MIDR_CAP_BOOTSTRAP) {
		MIDR_FLOW_LOG("MIDR bootstrap：本机是引导节点，跳过种子自举");
		return;
	}

	zlog_info("MIDR bootstrap：种子自举——重启后无人工命令，用 %u 个种子候选发起加入",
		  listcount(mi->bootstrap_list));
	midr_join_round_start(bgp);
}

void midr_join_via_bootstrap(struct bgp *bgp, const union sockunion *su,
			     as_t asn, struct in_addr rid)
{
	struct bgp_midr_nds *mi;
	struct ipaddr locator;

	if (!bgp || !bgp->midr_nds_info || !su)
		return;

	mi = bgp->midr_nds_info;

	if (!midr_sockunion_to_ipaddr(su, &locator) ||
	    !midr_ipaddr_valid_locator(&locator)) {
		zlog_warn("MIDR JOIN: bootstrap address is not a valid unicast locator");
		return;
	}

	/* §8.32：命令语义 = 追加候选（不再是覆盖单值）。
	 * rid 由命令必选参数带入（批 5 R 系列）：手配这一刻还没跟对方通上，学不到
	 * 它的 router-id，而挂靠挑台排环、按 rid 拉黑、台账认人三处都指着它。 */
	midr_bootstrap_list_add(mi, locator, asn, rid,
				MIDR_BOOTSTRAP_MANUAL);

	/* 已有在途 join：只入列不打断——新候选排进清单，failover 轮得到它。 */
	if (mi->join_intent) {
		struct midr_bootstrap_entry *cur =
			mi->bootstrap_cur ? listgetdata(mi->bootstrap_cur)
					  : NULL;

		/* 把"实际在连谁"一并记下：命令回显只说"开始加入"，运维要区分
		 * "我这条生效了没有"就得看这里。 */
		if (cur)
			MIDR_FLOW_LOG("MIDR JOIN: 已有在途加入，候选 %pSU 仅入列（现共 %u 条）；当前仍在尝试 %pIA",
				      su, listcount(mi->bootstrap_list),
				      &cur->transport);
		else
			MIDR_FLOW_LOG("MIDR JOIN: 已有在途加入，候选 %pSU 仅入列（现共 %u 条）",
				      su, listcount(mi->bootstrap_list));
		return;
	}

	/* 开新一轮：表意图、清 failed 标记、游标指表头、向首个候选发第一跳。 */
	midr_join_round_start(bgp);

	/* 发第一跳的是清单【首条】，未必是刚敲进来的那条（手配按加入顺序排、
	 * 种子排后），所以把真正的目标记进日志。 */
	if (mi->bootstrap_cur) {
		struct midr_bootstrap_entry *first =
			listgetdata(mi->bootstrap_cur);

		MIDR_FLOW_LOG("MIDR JOIN: joining via %pIA AS %u（候选清单首条，现共 %u 条）",
			      &first->transport, first->asn,
			      listcount(mi->bootstrap_list));
	}
}

/*
 * §8.32 failover：REP_LIST_REQ 重试耗尽（"死心"）时由 ctrl 层回调（唯一调用点
 * = midr_ctrl_retx_timer 的放弃分支）。守卫：意图仍在 + 失败地址就是游标当前
 * 候选——被删候选/上一轮的残留 pending 死掉时地址对不上，静默忽略。
 * 候选耗尽时 join_intent 保留：某慢候选的迟到 REP_LIST_RESP 仍可自愈进 join
 * （与单候选旧行为一致）；手动换组照旧作废一切。
 */
void midr_join_bootstrap_failed(struct bgp *bgp, struct ipaddr failed)
{
	struct bgp_midr_nds *mi;
	struct midr_bootstrap_entry *b;

	if (!bgp || !bgp->midr_nds_info)
		return;
	mi = bgp->midr_nds_info;

	if (!mi->join_intent || !mi->bootstrap_cur)
		return;
	b = listgetdata(mi->bootstrap_cur);
	if (!midr_ipaddr_same(&b->transport, &failed))
		return;

	b->failed = true;
	mi->bootstrap_cur = listnextnode(mi->bootstrap_cur);
	if (mi->bootstrap_cur) {
		struct midr_bootstrap_entry *next =
			listgetdata(mi->bootstrap_cur);

		zlog_info("MIDR bootstrap failover：%pIA 无响应，改试 %pIA（第 %u/%u 个候选）",
			  &failed, &next->transport,
			  midr_bootstrap_index(mi, mi->bootstrap_cur),
			  listcount(mi->bootstrap_list));
		midr_bootstrap_start_attempt(bgp);
	} else {
		zlog_warn("MIDR bootstrap failover：全部 %u 个候选耗尽，放弃本轮加入（意图保留，迟到响应仍可自愈；可补候选后重敲 midr bootstrap）",
			  listcount(mi->bootstrap_list));

		/*
		 * 挂点④之二（配套一）：本轮一个 REP_LIST_RESP 都没拿到，手里的锚点
		 * 备选群只可能是上一轮的残留——清掉再往下走（下面会以空目录触发
		 * REP_PROBE_DONE 交 CL 判自建群，那之后的 RECOMMEND 会重新存新的）。
		 */
		midr_nds_anchor_ctx_clear(bgp);

		/*
		 * 机制Ⅰ（场景 B，批 5c）：候选耗尽 = 挨个问遍也没问出任何群代表。
		 * 全网还没有群时目录必空、每台引导都按"空目录沉默"不应答，照样会
		 * 走到这里——这正是冷启动死锁（第一个节点永远入不了网）。
		 *
		 * 既然一个代表都没探到，就没有热身可等：直接以空目录触发
		 * REP_PROBE_DONE，复用 CL 现成的"无可用代表 → CREATE 自建群"分支
		 * （CL 侧那道 rep_dir 空即返回的守卫由本批一并删掉）。
		 * join_intent 不在此清——settle 落定时自然清。
		 *
		 * 引导自己走不到这里：第四守卫（5b）让带 BOOTSTRAP 位的节点根本开
		 * 不了 join round，所以"引导耗尽后自建群"不会发生。
		 */
		mi->join_in_progress = true;
		mi->join_phase = MIDR_JOIN_PROBING_REPS;
		zlog_info("MIDR 加入：候选耗尽且未探到任何群代表，直接以空目录触发 REP_PROBE_DONE，交 CL 判自建群");
		midr_nds_notify_cl(bgp, MIDR_TRIGGER_REP_PROBE_DONE);
	}
}

bool midr_bootstrap_list_del(struct bgp *bgp, struct ipaddr addr)
{
	struct bgp_midr_nds *mi;
	struct listnode *node = NULL;
	struct midr_bootstrap_entry *b;
	bool was_current = false;

	if (!bgp || !bgp->midr_nds_info)
		return false;
	mi = bgp->midr_nds_info;

	b = midr_bootstrap_find(mi, addr, &node);
	if (!b)
		return false;

	/* 正在尝试的被删：游标先顺移（该候选的残留 pending 死掉时，failover
	 * 守卫因地址对不上自然忽略，无需显式清 pending）。 */
	if (mi->bootstrap_cur == node) {
		mi->bootstrap_cur = listnextnode(node);
		was_current = true;
	}
	/* ② 兜底遍历的独立游标同样不能悬空（下面就要 free 这个 listnode）。
	 * 顺移即可：下一次死心回调会从新位置继续，不必在此重发。 */
	if (mi->bootstrap_probe_cur == node)
		mi->bootstrap_probe_cur = listnextnode(node);
	list_delete_node(mi->bootstrap_list, node);
	XFREE(MTYPE_MIDR_BOOTSTRAP_ENTRY, b);

	if (was_current && mi->join_intent) {
		if (mi->bootstrap_cur)
			midr_bootstrap_start_attempt(bgp);
		else
			zlog_warn("MIDR bootstrap：正在尝试的候选被删且已无其它候选，放弃本轮加入（意图保留）");
	}
	return true;
}

void midr_bootstrap_clear(struct bgp *bgp)
{
	struct bgp_midr_nds *mi;
	struct listnode *node, *nnode;
	struct midr_bootstrap_entry *b;

	if (!bgp || !bgp->midr_nds_info)
		return;
	mi = bgp->midr_nds_info;

	mi->bootstrap_cur = NULL;
	mi->bootstrap_probe_cur = NULL; /* ② 兜底遍历游标，同样不能留悬空 */
	for (ALL_LIST_ELEMENTS(mi->bootstrap_list, node, nnode, b)) {
		list_delete_node(mi->bootstrap_list, node);
		XFREE(MTYPE_MIDR_BOOTSTRAP_ENTRY, b);
	}

	/*
	 * 挂点④（配套一）：清空候选 = 中止 join，锚点评估上下文一并作废——否则
	 * 迟到的热身定时器还会按过期备选群触发 ANCHOR_PROBE_DONE。
	 */
	midr_nds_anchor_ctx_clear(bgp);

	/* 连带作废在途加入意图（与手动换组同款中止语义，批注点 C 已拍板）：
	 * 清空候选 = 运维明确表态"别入网了"，迟到的 REP_LIST_RESP 也被丢弃。 */
	if (mi->join_intent || mi->join_in_progress) {
		mi->join_intent = false;
		mi->join_in_progress = false;
		mi->join_phase = MIDR_JOIN_IDLE;
		mi->join_group_id = 0;
		MIDR_LOG("MIDR bootstrap：清空候选清单并作废在途加入意图");
	}
}

/* ===========================================================================
 * 退网 / 重入（决策 docs/decisions/midr-shutdown-semantics.md）
 *
 * `midr shutdown` 的语义是**退网**、不是行政性暂停：本机退化成"只有静态配置、
 * 尚未入网"的新节点——撤自身通告 + 拆掉自己建的全部 MIDR 会话 + 停掉全部探测 +
 * 清空节点表 + 群号回落配置值 + 对 CL 静默。bgpd 进程与 underlay BGP **一动不动**
 * （"只退 MIDR、不退 BGP"是红线）。
 * `no midr shutdown` 则完全按新节点入网走：清位 + 重新通告 + 经引导 join，零特判。
 * =========================================================================*/

/*
 * 退网第③步的真身：凭台账拆掉本机建的全部 MIDR 会话。由延时定时器调用；
 * `no midr shutdown` 若赶在定时器之前到达，也会同步调它一次（见 shutdown_exit）。
 *
 * 先快照再拆：拆边内部要销账（改的正是脚下这张链表），边遍历边改必崩——与
 * midr_nds_attach_detach_all 同款写法。复用静态会话的直连邻接、frr.conf 手写的
 * 静态邻居都不在台账，天然不碰（判据 3 守的就是这条）。
 *
 * MANUAL 也拆（2026-08-21 拍板）：退网是运维显式敲的命令、与手配同级，留一条还在
 * 收发 MIDR-LS 的会话对上层影响说不清。对端那半边收到的是 withdraw、走自动路径，
 * 仍被 α 豁免拦下（只 warn 不拆，判据 1）。
 */
static unsigned int midr_shutdown_teardown_sessions(struct bgp *bgp,
						    unsigned int *manual_out)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct listnode *node;
	struct midr_session_ledger_entry *e;
	struct ipaddr *doomed_t;
	struct in_addr *doomed_rid;
	bool *doomed_manual;
	unsigned int cnt, n = 0, i, manual = 0;

	if (manual_out)
		*manual_out = 0;
	if (!mi->session_ledger || list_isempty(mi->session_ledger))
		return 0;

	cnt = listcount(mi->session_ledger);
	doomed_t = XCALLOC(MTYPE_TMP, cnt * sizeof(*doomed_t));
	doomed_rid = XCALLOC(MTYPE_TMP, cnt * sizeof(*doomed_rid));
	doomed_manual = XCALLOC(MTYPE_TMP, cnt * sizeof(*doomed_manual));

	for (ALL_LIST_ELEMENTS_RO(mi->session_ledger, node, e)) {
		doomed_t[n] = e->transport;
		doomed_rid[n] = e->remote_rid;
		doomed_manual[n] = (e->reason == MIDR_SESSION_MANUAL);
		n++;
	}

	for (i = 0; i < n; i++) {
		if (doomed_manual[i]) {
			manual++;
			zlog_warn("MIDR 退网：拆除运维手配会话 %pIA（台账 MANUAL）——配置意图保留，重入后自动恢复",
				  &doomed_t[i]);
		}
		midr_ctrl_detach_transport(bgp, doomed_t[i], doomed_rid[i],
					   true, MIDR_STOP_GRACEFUL_SHUTDOWN);
	}

	XFREE(MTYPE_TMP, doomed_t);
	XFREE(MTYPE_TMP, doomed_rid);
	XFREE(MTYPE_TMP, doomed_manual);

	if (manual_out)
		*manual_out = manual;
	return n;
}

/*
 * 延时拆会话的定时器回调。⚠ **当前无人武装它**——延时已回退（见 shutdown_enter
 * 里 ③ 的说明：会踩第二组 bgp_midr_rib.c:512 的断言导致 bgpd abort）。
 * 留着不删，等对方修好断言后把 enter 里那行换回 event_add_timer 即可复用。
 */
static void midr_shutdown_teardown_cb(struct event *t) __attribute__((unused));
static void midr_shutdown_teardown_cb(struct event *t)
{
	struct bgp *bgp = EVENT_ARG(t);
	unsigned int n, manual;

	if (!bgp || !bgp->midr_nds_info)
		return;

	n = midr_shutdown_teardown_sessions(bgp, &manual);
	zlog_info("MIDR 退网：延时 %d 秒到，拆除会话 %u 条（撤销已先行发出）",
		  MIDR_SHUTDOWN_TEARDOWN_DELAY, n);
}

unsigned int midr_nds_shutdown_enter(struct bgp *bgp)
{
	struct bgp_midr_nds *mi;
	struct midr_node_entry *entry;
	unsigned int n = 0, manual = 0, cleared = 0, reps;
	int stopped;

	if (!bgp || !bgp->midr_nds_info)
		return 0;
	mi = bgp->midr_nds_info;

	/*
	 * ① 置位必须是第一动作：三道入站守卫（第二组 node/link 回调、5859 控制通道）
	 * 与上报出口的抑制全靠它，下面每一步的连锁反应都被它压住——否则清表会喷
	 * NODE_CHANGE、拆会话会引来重建。
	 */
	mi->shutdown = true;

	/*
	 * ② 撤自身通告。LEAVE 分支在 midr_nds_report_node() 里排在 shutdown 抑制
	 * **之前**，所以先置位不会把自己这发 withdraw 吞掉（读码核实，退网专题答 2）。
	 */
	midr_nds_report_node(bgp, MIDR_ORIGIN_LEAVE);

	/*
	 * ③ 拆会话：**延后 MIDR_SHUTDOWN_TEARDOWN_DELAY 秒**（本函数只排定时器，
	 * 真拆在 midr_shutdown_teardown_sessions()）——撤销刚在 ② 发出，此刻拆会话
	 * 等于把它自己的传播通道掐了（记档 39）。下面 ④～⑦ 仍当场做：判据要求敲完
	 * 命令立刻看到群号回落与收尾日志。
	 *
	 * 凭台账认边：**不遍历节点表**——纯锚点边不置 is_adjacent，按节点表遍历会
	 * 漏拆（结论 1"拆边凭台账认边"）。
	 *
	 * MANUAL 也拆（2026-08-21 拍板，推翻"本端也豁免"的原案）：退网是运维显式
 * 敲的命令、与手配同级，留一条还在收发 MIDR-LS 的会话，对上层（CL / 第二组
	 * 拓扑视图）的影响说不清，拆干净是保守方向；显式配置意图保留，重入后由
	 * manual_sessions 自动恢复。
	 * 对端那半边不受影响——它收到的是 withdraw、走自动路径，仍被 α 豁免拦下
	 * （只 warn 不拆，判据 1）。
	 *
	 * 先快照再拆：拆边内部要销账（改的正是脚下这张链表），边遍历边改必崩
	 * ——与 midr_nds_attach_detach_all 同款写法。
	 * 复用静态会话的直连邻接、frr.conf 手写的静态邻居都不在台账，天然不碰
	 * （判据 3 守的就是这条）。
	 */
	/*
	 * ⚠ **延时已回退（08-23 实测）**：本处原改为排 5s 定时器延后拆会话（让撤销
	 * 先发出去，治记档 39），实测**必崩 bgpd**——
	 *   00:25:27 node 撤销上报 → 00:25:32 延时到、拆会话 → 同秒
	 *   `bgp_midr_rib.c:512 assertion (store->active_identity_count) failed` → abort
	 * 根因在第二组：撤销让他们 RIB 的活跃身份归 0，而会话还留着 5s，拆会话时
	 * 触发 bgp_midr_rib_process_main()，那里断言"处理主 RIB 时必有活跃身份"——
	 * **"身份已撤、会话仍在"这个中间态打破了他们的假设**。无延时时两件事在同一个
	 * 事件循环里连续发生，中间态不存在，所以一直没暴露。已转问题清单 #17。
	 *
	 * 恢复方式（等他们把 assert 改成容错分支后）：把下面这行换回
	 *   `if (n) event_add_timer(bm->master, midr_shutdown_teardown_cb, bgp,
	 *                           MIDR_SHUTDOWN_TEARDOWN_DELAY, &mi->t_shutdown_teardown);`
	 * 定时器回调、exit 的补拆、finish 的取消都留着没删，改一行即可。
	 */
	n = midr_shutdown_teardown_sessions(bgp, &manual);

	/*
	 * ④ 全量停探（I-2）。不逐边配对：join 期对群代表/成员起的探测、锚点评估对
	 * 次优群代表起的探测都不在节点表里，逐节点停会漏，漏掉的就是停不下来的探测流。
	 */
	stopped = midr_pm_remove_all_targets(bgp, MIDR_STOP_GRACEFUL_SHUTDOWN);

	/*
	 * ⑤ 清节点表，**self 条目除外**（它由本机运行态刷新、不是学来的）。
	 * 逐个先走 detach 全套再删条目：停探（与 ④ 重复但幂等）、链路事实 withdraw、
	 * 删本地链路、清邻接位。必须走 detach 的关键是 withdraw 那件——轮 3 定的
	 * **墓碑口径**（撤销留墓碑、reported 置假、version 就地延续）在它里面，绕开
	 * 自己写会让重入后重报的 version 从头起、被对端按"不比墓碑新"静默吞掉
	 * （判据 7 验的正是这个）。
	 * teardown_session 传 false：会话已在 ③ 按台账处理完，这里再拆会绕开台账。
	 */
	frr_each_safe (midr_node_hash, &mi->global_view->nodes, entry) {
		if (entry->is_self)
			continue;
		midr_nds_detach_node(bgp, entry, MIDR_STOP_GRACEFUL_SHUTDOWN,
				     false);
		midr_node_hash_del(&mi->global_view->nodes, entry);
		XFREE(MTYPE_MIDR_NODE_ENTRY, entry);
		cleared++;
	}

	/*
	 * ⑥ 身份回落成"尚未入网的新节点"。
	 * 群号：回落到配置值——JOIN 来的群号清掉、手配的保留（config_group_id 就是
	 * 为这件事而与运行值分离的，见其字段注释）。
	 * 群代表位：一律清。它没有"出处存底"字段可回落，手配过的按"退网清运行态、
	 * 手配的提醒重敲"处置（与 MANUAL 会话同口径，结论 3）；重入后要不要再当代表，
	 * 由 join 落定规则重判。经 setter 清是为了走它那条统一出口（卸任钩子会顺带
	 * 拆挂靠边，此刻台账已空、空转无害；它内部的重通告被 shutdown 守卫吞掉）。
	 * join / 锚点残留：一并清干净，免得重入时踩到上一段人生的状态。
	 */
	mi->local_group_id = mi->config_group_id;
	if (mi->local_capabilities & MIDR_CAP_GROUP_REP)
		midr_nds_set_capability(bgp, mi->local_capabilities &
						     ~MIDR_CAP_GROUP_REP);

	mi->join_intent = false;
	mi->join_in_progress = false;
	mi->join_phase = MIDR_JOIN_IDLE;
	mi->join_group_id = 0;
	mi->bootstrap_cur = NULL;
	midr_nds_anchor_ctx_clear(bgp); /* 挂点①（配套一） */

	/*
	 * 群代表目录也清（2026-08-21 补）：它是上一段人生学来的网络状态——目录条目
	 * 由 REP_LIST_RESP 灌入 + 按能力位推导，全都描述"网里现在有哪些群、代表是谁"。
	 * 退网 = 退化成尚未入网的新节点，而新节点的目录本来就是空的；留着它，重入时
	 * CL 可能读到早已不存在的群/代表（`midr_rep_dir_add` 按群更新，那些群若已解散
	 * 就永远没人来覆盖），`show midr reps` 也会显示过期条目。
	 * 放在步④全量停探**之后**：目录里的代表在 join 期起过探测，先停探再清目录，
	 * 不会留下没人认领的探测上下文。
	 * ⚠ 这份目录自 2026-08-11（批 1.5 删 `midr rep group`）起**不再有任何 show 命令
	 * 展示**（`show midr reps` 只剩从节点表推导的那段），所以清没清全靠这条日志看。
	 */
	reps = mi->rep_dir ? listcount(mi->rep_dir) : 0;
	midr_rep_dir_clear(bgp);
	if (reps)
		MIDR_LOG("MIDR 退网：清空群代表目录 %u 条（join 时收来的死快照，重入会重新问引导要）",
			 reps);

	/* self 条目跟着运行态刷一次，否则 `show midr nodes` 里自己那行还挂着旧群号
	 * 和旧能力位（它不在上面的清表范围内）。 */
	midr_nds_local_node_update(bgp);

	zlog_info("MIDR 退网：已撤销自身通告，拆除会话 %u 条（其中运维手配 %u 条）、停探 %d 个目标、清空节点表 %u 个条目，群号回落 %u",
		  n, manual, stopped, cleared, mi->local_group_id);

	/* ⑦ bgpd 与 underlay 一动不动——本函数不碰任何非 MIDR 的 peer/路由。 */

	return manual;
}

bool midr_nds_shutdown_exit(struct bgp *bgp)
{
	struct bgp_midr_nds *mi;

	if (!bgp || !bgp->midr_nds_info)
		return false;
	mi = bgp->midr_nds_info;

	/*
	 * ⓪ 赶在延时拆会话之前重上线：取消定时器并**当场补拆**。不补拆则会话残留、
	 * 而节点表/群号/目录早在 enter 时就清空了，下面的 join 会踩到上一段人生的边。
	 * 语义与延时前完全一致（退网 = 会话拆净），只是拆的时机被 exit 提前了。
	 */
	if (mi->t_shutdown_teardown) {
		unsigned int torn, manual;

		event_cancel(&mi->t_shutdown_teardown);
		torn = midr_shutdown_teardown_sessions(bgp, &manual);
		zlog_info("MIDR 退网：延时未到即重上线，当场补拆会话 %u 条", torn);
	}

	/* ① 清位：两道守卫随之解除，keepalive 下一拍自动恢复重发。 */
	mi->shutdown = false;

	/* ② 重新通告自己（群号 = 退网时回落的配置值）。 */
	midr_nds_report_node(bgp, MIDR_ORIGIN_REJOIN);
	/* 手工会话是配置意图，不随退网时的运行态 ledger 一起消失。 */
	midr_nds_manual_sessions_restore(bgp);

	/*
	 * ③ 重新入网。退网把节点表和群号都清了，光恢复通告等于一个谁也不认识的孤
	 * 节点——必须像新节点一样从引导候选问起（决策 §3 C-2：走与新节点完全相同的
	 * join、零特判）。编排复用 midr_join_round_start（手动 bootstrap / 种子自举
	 * 共用的那条路），"群号听配置还是听 CL"的分岔 08-06 已在 join 里落地。
	 *
	 * 候选清单为空 = 没有引导可问，入不了网：打 warn 等运维补候选，**不保留**
	 * "退回自己直接通告等人来捞"——那正是退网语义要去掉的东西（移交后通告归第二
	 * 组、我方无权自发，孤岛状态发了也传不出去）。
	 */
	if (!mi->bootstrap_list || list_isempty(mi->bootstrap_list)) {
		zlog_warn("MIDR 重入：已恢复通告，但引导候选清单为空、无法发起加入——请先配 midr bootstrap <IP> remote-as <ASN>");
		return false;
	}

	zlog_info("MIDR 重入：恢复通告，按新节点流程重新经引导加入（%u 个候选）",
		  listcount(mi->bootstrap_list));
	midr_join_round_start(bgp);
	return true;
}

/* ===========================================================================
 * ② 按需拉取活引导名单（BOOTSTRAP_LIST）——子稿 §2②/§4-3
 *
 * 代表侧（要名单的一方）。引导侧（应答组装 = 手配名单 ∩ 骨干会话活性）在
 * bgp_midr_ctrl.c 的 midr_ctrl_build_bootstrap_list()。
 * 接口语义、两个钩子的落点、"本轮只写不接调用点"见 bgp_midr_nds.h 声明处。
 * =========================================================================*/

/* 到该 transport 的 BGP 会话是否已 Established（判活统一走会话状态，子稿 §1
 * 前提二：会话 Down 即死，不引用 legacy last_seen/expire）。 */
static bool midr_nds_transport_session_up(struct bgp *bgp,
					  struct ipaddr transport)
{
	struct prefix p;
	union sockunion su;
	struct peer *peer;

	if (!midr_ipaddr_to_host_prefix(&transport, &p))
		return false;
	prefix2sockunion(&p, &su);
	peer = peer_lookup(bgp, &su);

	return peer && peer->connection &&
	       peer->connection->status == Established;
}

void midr_nds_bootstrap_learn(struct bgp *bgp, struct ipaddr transport,
			      as_t asn, struct in_addr rid)
{
	struct bgp_midr_nds *mi;
	struct midr_bootstrap_entry *b;
	char buf[IPADDR_STRING_SIZE];
	char ridbuf[INET_ADDRSTRLEN];

	if (!bgp || !bgp->midr_nds_info)
		return;
	/* asn 不再列为必需（轮 4 放宽，见 midr_ctrl_connect）：换源后对端表里的
	 * asn 也会是 0，名单条目照收。 */
	if (!midr_ipaddr_valid_locator(&transport))
		return;
	/* 无名条目不吸收（"池内 rid 恒非 0"不变量，批 5 R 系列）：v4 wire 条目
	 * 必带 rid，收到 0 说明对端实现有误或包被改坏——告警留证后跳过该条，其余
	 * 条目照收。 */
	if (rid.s_addr == INADDR_ANY) {
		zlog_warn("MIDR 引导名单：条目 %pIA（AS %u）无 router-id，跳过（协议 v4 必带 rid）",
			  &transport, asn);
		return;
	}
	mi = bgp->midr_nds_info;

	/* 别把自己学成候选（应答里含应答方自己，见 build_bootstrap_list；本机
	 * 也可能带 BOOTSTRAP 位、出现在别人的名单里）。 */
	if (mi->transport_active &&
	    midr_ipaddr_same(&transport, &mi->active_transport_addr))
		return;

	midr_bootstrap_list_add(mi, transport, asn, rid, MIDR_BOOTSTRAP_SEED);

	/*
	 * 置"这台在最新名单里"（批 6 缺口 A 的加法半边；减法在 list_begin）。
	 * add 不返回条目指针，按 transport 查回来即可——候选数是个位数量级。
	 */
	b = midr_bootstrap_find(mi, transport, NULL);
	if (b)
		b->in_last_list = true;

	/* 种子库新路：名单来的引导逐条落库，墙钟记 last_seen（口径同
	 * midr_maybe_save_bootstrap_seed，两个同名 last_seen 的坑见那里）。 */
	snprintfrr(buf, sizeof(buf), "%pIA", &transport);
	snprintfrr(ridbuf, sizeof(ridbuf), "%pI4", &rid);
	midr_store_seed_save(buf, (uint32_t)asn, ridbuf, time(NULL));
}

/*
 * 一份名单开收之前：清掉上一份留下的批次标记（批 6 缺口 A 的"减法"）。
 * 清在收之前、置在 learn 里，两半合起来 = 候选池里"谁在最新名单里"随每份名单
 * 刷新。少了这一半，名单就只做加法：下线的引导永远留在池里且照样被优先挑中，
 * 计划性下线与直接 kill 毫无差别（E14 实测的病根）。
 */
void midr_nds_bootstrap_list_begin(struct bgp *bgp)
{
	struct bgp_midr_nds *mi;
	struct listnode *node;
	struct midr_bootstrap_entry *b;

	if (!bgp || !bgp->midr_nds_info)
		return;
	mi = bgp->midr_nds_info;

	for (ALL_LIST_ELEMENTS_RO(mi->bootstrap_list, node, b))
		b->in_last_list = false;
}

void midr_nds_on_bootstrap_list(struct bgp *bgp, struct ipaddr src,
				unsigned int count)
{
	struct bgp_midr_nds *mi;

	if (!bgp || !bgp->midr_nds_info)
		return;
	mi = bgp->midr_nds_info;

	/* 名单到手 = 兜底遍历的使命完成（不论它是从哪一级问到的）。 */
	mi->bootstrap_probe_cur = NULL;

	/*
	 * 从此刻起挑台才分两批（批 6 缺口 A）。空名单不算数——引导侧 count==0 时
	 * 压根不回包（见 build_bootstrap_list），真收到 0 条说明对端异常，此时若
	 * 认它作数就会把全部候选贬进第二批。
	 */
	if (count)
		mi->bootstrap_list_seen = true;

	/* 条目已在 midr_nds_bootstrap_learn 里逐条吸收（候选池 + 种子库）；
	 * 顺手 prune 到上限——批 5 后旧路断料，这里就是唯一还会长表的地方。 */
	midr_store_seed_prune(MIDR_STORE_SEED_KEEP);

	MIDR_FLOW_LOG("MIDR 引导名单：收到 %u 台活引导（来自 %pIA），候选池现 %u 条",
		      count, &src, listcount(mi->bootstrap_list));

	/* 名单到手 → 挑 K 台挂靠（批 5 接线；批 3 只写到"拿到名单"为止）。 */
	midr_nds_attach_pick(bgp);
}

/*
 * 当前挂靠边数 = **台账里 ATTACH 的笔数**，不看会话状态。
 *
 * ⚠ 口径必须与下面 attach_noted()（"这台已挂过就跳过"）一致，否则会重复补挂：
 * 首跑实测踩过——want 按"已 Established 的边"算、跳过按"有没有账"算，于是在途
 * 还没建成的边占着账却不算数，每收到一份名单就多挑一台，K=2 挂成了 3 台。
 * 按账数算才对：一笔 ATTACH 账 = "我要挂这台"，在途的占名额；真连不上的由死心
 * 逻辑（重传耗尽 / 收到 PEER_REJECT）拆半边 + 销账，名额随之自动释放。
 */
static unsigned int midr_nds_attach_count(struct bgp *bgp)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct listnode *node;
	struct midr_session_ledger_entry *e;
	unsigned int n = 0;

	for (ALL_LIST_ELEMENTS_RO(mi->session_ledger, node, e))
		if (e->reason == MIDR_SESSION_ATTACH)
			n++;
	return n;
}

/*
 * 确保挂靠达标（批 6）：**是**群代表 ∧ ATTACH 账不足 K → 去拉一份活引导名单
 * （挑台在名单回来时做）。判据与 config_end 那条补拉一字不差，只是触发点不同。
 *
 * 为什么要有它：挂靠钩子原先只认"GROUP_REP 位**翻真**"，于是"本来就是代表"
 * 的节点怎么折腾都不会补挂靠——挂靠全灭之后另两个触发源（挂靠会话 Down /
 * 收到名单）也都不存在，节点就此永远脱离骨干网，实测连 `midr bootstrap` 都拉
 * 不回来（只有重启能救）。改成"是代表就确保达标"后，凡经过 setter 的角色动作、
 * join 落定、config_end 三处都会补一次。
 *
 * **幂等靠"账 < K"这道判据**：挂满了直接返回，所以可以放心从多处调用；反过来
 * 也正因为有它，才敢把 setter 的判据从"翻转"放宽到"为真"（否则每次角色操作都
 * 会白拉一次名单）。
 */
static void midr_nds_attach_ensure(struct bgp *bgp, const char *why)
{
	struct bgp_midr_nds *mi;
	unsigned int live;

	if (!bgp || !bgp->midr_nds_info)
		return;
	mi = bgp->midr_nds_info;

	if (!(mi->local_capabilities & MIDR_CAP_GROUP_REP))
		return;

	live = midr_nds_attach_count(bgp);
	if (live >= MIDR_ATTACH_K)
		return;

	zlog_info("MIDR 挂靠：%s（群 %u）——已挂 %u/%d 台，拉取活引导名单",
		  why, mi->local_group_id, live, MIDR_ATTACH_K);
	midr_nds_bootstrap_list_fetch(bgp);
}

/*
 * 挂靠挑台（保底轮 2 批 5，方案定稿结论 9 + 执行计划批注 58）。
 *
 * 挑法 = **哈希定起点 + 顺次取 K**：
 *   ① 候选池里的引导按 **router-id 排成一个环**（升序，各节点看到的次序一致）；
 *   ② `hash(本机 router-id) % N` 定**起点**——不同代表的起点被打散，五台引导
 *      不会被所有代表都挤在同一台上；
 *   ③ 从起点起**连续**取（步长恒为 1），已挂过/试失败的顺延下一个，**绕环一圈
 *      为止**；
 *   ④ 实挂 = min(K, 活引导数)：K 是上限不是硬指标（答疑 40①）。
 * 这样选出来的组合**确定可重放**——排查时能算出"这台代表该挂谁"，而不是看运气。
 *
 * 补差不重挂：先数现有的活 ATTACH 边，只补到 K 台为止。钩子 (b) 换台时因此不会
 * 把还活着的那条也重挂一遍。
 *
 * 建连本身交给 midr_ctrl_connect(..., MIDR_SESSION_ATTACH)：台账记 ATTACH、发的
 * 是 ATTACH_REQUEST（前置①），引导侧据此跳过负面守卫直接回配。
 */
/*
 * 一趟挑选：在给定子集（已按 rid 升序）上排环取模，最多挑 want 台，返回实挑数。
 * second = 这是第二批（名单外候选）——只影响两件事：日志措辞、建连后把重传预算
 * 压到 MIDR_ATTACH_RETX_SECOND（3s×2）。挑选规则两批完全一致。
 */
static unsigned int midr_attach_pick_pass(struct bgp *bgp,
					  struct midr_bootstrap_entry **ring,
					  unsigned int n, unsigned int want,
					  bool second)
{
	struct ipaddr local;
	unsigned int i, start, picked = 0;

	if (!n || !want || !midr_nds_local_transport_get(bgp, &local))
		return 0;

	/*
	 * 起点 = hash(本机 rid) % N。**必须用真哈希，不能裸取模**（08-13 实测）：
	 * 直接 `ntohl(rid) % n` 时，同网段等间隔的 router-id 会成片撞到同一起点——
	 * r1=10.0.0.111 与 r2=10.0.0.121 差 10，n=5 时 10 % 5 == 0，两台代表算出
	 * 完全相同的起点，挂靠全挤在同一对引导上，"分散"彻底失效。jhash 把低位差
	 * 扩散到全字长，等间隔 rid 才会被打散。
	 * 两批各自在自己的子集上取模，分散性各自保持。
	 */
	start = jhash_1word(ntohl(bgp->router_id.s_addr), 0x4d494452) % n;

	for (i = 0; i < n && picked < want; i++) {
		struct midr_bootstrap_entry *cand = ring[(start + i) % n];
		const struct midr_session_ledger_entry *led;
		struct midr_node_entry e = {};

		/* asn 不再列为必需（轮 4 放宽，见 midr_ctrl_connect）。 */
		if (!midr_ipaddr_valid_locator(&cand->transport) ||
		    ipaddr_family(&cand->transport) != ipaddr_family(&local))
			continue;
		/*
		 * 本轮已试过且挂不上的跳过（D2 failover），这才谈得上"绕环一圈"。
		 * ⚠ 与被否掉的"名单代际"方案的差别：名单**没报**它死的那些引导
		 * （第二批）第一次仍会被撞一次——只是死心只花 6s。名单报了它活着
		 * 却连不上，才是真该慢慢试的（第一批 15s）。
		 */
		if (cand->attach_failed)
			continue;

		led = midr_nds_ledger_lookup(bgp, cand->transport);
		/*
		 * 已挂过（账在）就跳——包含钩子 (b) 场景里还活着的那条。会话未必已
		 * Established，**在途的也算**：口径必须与 attach_count（数账不数会话）
		 * 一致，否则在途的边占着账却不算数，每收一份名单就多挑一台（首跑实测
		 * 把 K=2 挂成了 3 台）。
		 */
		if (led && led->reason == MIDR_SESSION_ATTACH)
			continue;
		/*
		 * 缺口 B（批 6）：账是 MANUAL 就跳过——**不看那条会话通没通**。
		 * 它是运维点名要的边，不是挂靠边：撞上去的话，第一道去重会让
		 * midr_ctrl_connect() 直接 return（ATTACH_REQUEST 根本发不出去），
		 * 既不算成功也不算失败，挂靠数就此少一条且永不自愈（E13 实测）。
		 * 补发请求治不了本——MANUAL 粘性使那条边永远不计入挂靠数（挂靠数
		 * 只数 ATTACH 笔数），只会多造一条"通着却不算数"的边。
		 * info 级：稳态低频，且这是给运维看的线索（要么去对端补
		 * `midr session` 对称配上，要么在本机 `no midr session` 让它回到
		 * 普通候选）。
		 */
		if (led && led->reason == MIDR_SESSION_MANUAL) {
			zlog_info("MIDR 挂靠：跳过引导 %pIA（rid %pI4）——该地址上已有运维手配的会话（台账 MANUAL），挂靠不占用运维的边；要让它参与挂靠请在本机 no midr session %pIA",
				  &cand->transport, &cand->rid,
				  &cand->transport);
			continue;
		}

		/*
		 * 拼临时条目：专职引导通常不在 remote-view 节点表里，没有现成条目可传。
		 * 同 midr_ctrl_udp_recv 收 PEER_REQUEST 回配那条路的做法——信息自带、
		 * 不依赖节点表。node_id 取候选的 rid（池内恒非 0），排除名单查得到、
		 * 第二道按 rid 去重也正常。group_id 置 0：引导不属任何群。
		 */
		midr_prefix_from_in_addr(&e.node_id, cand->rid);
		e.asn = cand->asn;
		e.transport_addr = cand->transport;
		e.has_transport_addr = true;

		zlog_info("MIDR 挂靠：向引导 %pIA（rid %pI4）建挂靠会话（%s，环序第 %u/%u，起点 %u）",
			  &cand->transport, &cand->rid,
			  second ? "第二批：不在最新活引导名单里"
				 : "第一批：在最新活引导名单里",
			  (start + i) % n, n, start);
		midr_ctrl_connect(bgp, &e, MIDR_SESSION_ATTACH, true);
		/*
		 * 第二批压重传预算（批 6 的 A-2）。放在 connect **之后**：预算改的是
		 * connect 刚入队的那条 pending；若 connect 因去重没入队（不该发生
		 * ——MANUAL/ATTACH 两种账上面都拦掉了），setter 查不到就空转，无害。
		 */
		if (second)
			midr_ctrl_set_retx_budget(bgp, cand->transport,
						  MIDR_CTRL_ATTACH_REQUEST,
						  MIDR_ATTACH_RETX_SECOND);
		picked++;
	}

	return picked;
}

void midr_nds_attach_pick(struct bgp *bgp)
{
	midr_nds_attach_pick_from(bgp, MIDR_ATTACH_BATCH_FIRST);
}

void midr_nds_attach_pick_from(struct bgp *bgp, enum midr_attach_batch from)
{
	struct bgp_midr_nds *mi;
	struct listnode *node;
	struct midr_bootstrap_entry *b;
	struct midr_bootstrap_entry **ring, **first, **second;
	unsigned int n = 0, i, n1 = 0, n2 = 0, live, want, picked = 0;

	if (!bgp || !bgp->midr_nds_info)
		return;
	mi = bgp->midr_nds_info;

	/* 只有群代表才挂靠（钩子挂在能力位 setter 上，这里再兜一道：名单是异步回
	 * 来的，回来时本机可能已经卸任了）。 */
	if (!(mi->local_capabilities & MIDR_CAP_GROUP_REP))
		return;

	live = midr_nds_attach_count(bgp);
	if (live >= MIDR_ATTACH_K)
		return; /* 已够 K 台，不必再挑 */
	want = MIDR_ATTACH_K - live;

	if (!mi->bootstrap_list || list_isempty(mi->bootstrap_list))
		return;

	/* 排环：拷指针到数组按 rid 升序排。候选池本身的次序有别的语义（手配在前、
	 * 种子在后，是 failover 的尝试优先级），不能就地重排。 */
	ring = XCALLOC(MTYPE_TMP, listcount(mi->bootstrap_list) * sizeof(*ring));
	for (ALL_LIST_ELEMENTS_RO(mi->bootstrap_list, node, b))
		ring[n++] = b;
	for (i = 1; i < n; i++) { /* 插入排序：候选数是个位数量级 */
		struct midr_bootstrap_entry *key = ring[i];
		int j = (int)i - 1;

		while (j >= 0 && ntohl(ring[j]->rid.s_addr) >
					ntohl(key->rid.s_addr)) {
			ring[j + 1] = ring[j];
			j--;
		}
		ring[j + 1] = key;
	}

	/*
	 * 分流成两批（批 6 缺口 A）：在最新名单里的进第一批，其余进第二批。
	 * 两批各自保持 rid 升序（从排好序的环里顺序分流），故各自取模仍确定可重放。
	 * **一份名单都没收到过时全部进第一批**——那时"谁都不在名单里"是常态不是
	 * 信号，照分批办会把 conf 里配的候选全贬去走 6s 低可信路（判据④ 验的正是
	 * 首次自举这条路：挑满 K 且阈值仍是 15s）。
	 */
	first = XCALLOC(MTYPE_TMP, n * sizeof(*first));
	second = XCALLOC(MTYPE_TMP, n * sizeof(*second));
	for (i = 0; i < n; i++) {
		if (!mi->bootstrap_list_seen || ring[i]->in_last_list)
			first[n1++] = ring[i];
		else
			second[n2++] = ring[i];
	}

	/*
	 * A-3：从死者所在的那一批继续。从第一批开始 = 两趟都跑（第一批挑不够才进
	 * 第二批）；从第二批开始 = 跳过第一趟——能走到那一步，说明第一批当时已经
	 * 试尽了，再扫一遍只是白撞一圈已盖章的候选。
	 */
	if (from == MIDR_ATTACH_BATCH_FIRST)
		picked = midr_attach_pick_pass(bgp, first, n1, want, false);
	if (picked < want)
		picked += midr_attach_pick_pass(bgp, second, n2, want - picked,
						true);

	XFREE(MTYPE_TMP, first);
	XFREE(MTYPE_TMP, second);
	XFREE(MTYPE_TMP, ring);

	/*
	 * 两批都试尽仍不足 K：**接受挂靠不足并停住**（结论 9 的纯事件驱动），不再
	 * 自动重试——等下一个事件（挂靠会话 Down、收到新名单、运维命令）叫醒。
	 */
	if (picked < want)
		zlog_warn("MIDR 挂靠：只挂上 %u 台引导（想要 %u 台，K=%d）——活引导不足，降级运行（跨群情报仍通，但没有备台）",
			  live + picked, MIDR_ATTACH_K, MIDR_ATTACH_K);
}

/*
 * 挂靠 failover 的"盖章"半边（D2）。两条路进来：ATTACH_REQUEST 重传耗尽死心、
 * 对端回 PEER_REJECT。盖上章之后挑台绕开它，下一轮拉名单时统一清章。
 *
 * 为什么挂靠需要 failover 而 PEER_REQUEST 不需要（结论 23 写"没有换一个试"）：
 * PEER_REQUEST 的三个场景全是"指名连这一个"（同群某成员、CL 点名的锚点、运维
 * 手配），没有替补概念；挂靠是全代码第一个**有替补池**的建连场景——名单里五台
 * 引导挂哪两台都行，那一台挂不上就该换一台。
 *
 * 只置状态不动作：重挑（midr_nds_attach_pick_from）会 connect、往 ctrl_pending
 * 追加条目，调用方得挑个不在遍历那张表的地方调。
 *
 * 返回死者所在的批次（批 6 的 A-3）：调用方原样传给 pick_from，好让重挑从这一批
 * 接着走。查不到该候选（已被 no midr bootstrap 删掉等）时返回 FIRST——保守选项，
 * 大不了多扫一遍第一批，总好过跳过还没试过的候选。
 */
enum midr_attach_batch midr_nds_attach_mark_failed(struct bgp *bgp,
						   struct ipaddr transport)
{
	struct midr_bootstrap_entry *b;

	if (!bgp || !bgp->midr_nds_info)
		return MIDR_ATTACH_BATCH_FIRST;

	b = midr_bootstrap_find(bgp->midr_nds_info, transport, NULL);
	if (!b)
		return MIDR_ATTACH_BATCH_FIRST;

	/*
	 * 批次取自条目自身的标记，故"重挑从死者那批继续"不需要任何跨调用状态
	 * （A-1 那句"无跨调用状态、无失效标记"对这里同样成立）。
	 * seen 为假时全部算第一批，与 pick_from 的分流口径保持一致。
	 */
	if (!b->attach_failed) {
		b->attach_failed = true;
		zlog_info("MIDR 挂靠：引导 %pIA（rid %pI4）挂不上，本轮不再选它——改挑下一台",
			  &b->transport, &b->rid);
	}

	if (!bgp->midr_nds_info->bootstrap_list_seen || b->in_last_list)
		return MIDR_ATTACH_BATCH_FIRST;
	return MIDR_ATTACH_BATCH_SECOND;
}

/*
 * 钩子 (b)：一条挂靠会话掉线（答疑 93 定案）。
 *
 * **先拆旧边再挑新台**——次序有讲究：不先拆的话，死引导的半边 peer 会一直挂在
 * 代表身上（对端已死、会话永远回不来），换几次台就堆几条僵尸。
 * 拆完账也没了，随后 fetch 的第①级"问还活着的挂靠引导"少一个候选——但 K=2 时
 * 另一条还连着、照样问得到；两条都死才落兜底遍历（挨个试问候选池）。这正是
 * K=2 留备份的意义。
 */
void midr_nds_attach_on_session_down(struct bgp *bgp, struct ipaddr transport)
{
	struct bgp_midr_nds *mi;
	const struct midr_session_ledger_entry *e;
	struct in_addr rid;

	if (!bgp || !bgp->midr_nds_info)
		return;
	mi = bgp->midr_nds_info;

	/*
	 * 下面两道判据同时担着两个职责：
	 *   ① 只管挂靠边——别的边掉线各有各的处理（同群边归发现链/清理反应等）；
	 *   ② **幂等**（D4 事件化之后新加的职责）：钩子只记名、本函数下一拍才动手，
	 *      这一拍之间世界可能已经变了——运维敲了 `no midr session` 把边拆了、
	 *      卸任 detach_all 先跑掉了、同一条边短时间抖两次被记了两笔。查不到账
	 *      （if①）或账已不是挂靠（if②）就空手而归，做第二遍与做一遍等价。
	 * 反例说明为什么非查不可：旧边拆掉后运维**立刻手配了同地址的 MANUAL 会话**，
	 * 而我们那笔旧记名还排在队里——不查账就动手 = 把运维刚建的边当死挂靠拆掉。
	 */
	e = midr_nds_ledger_lookup(bgp, transport);
	if (!e || e->reason != MIDR_SESSION_ATTACH)
		return;
	rid = e->remote_rid;

	zlog_info("MIDR 挂靠：到引导 %pIA（rid %pI4）的挂靠会话掉线——先拆旧边，再拉新名单重挑",
		  &transport, &rid);
	midr_ctrl_detach_transport(bgp, transport, rid, false,
				   MIDR_STOP_SESSION_DOWN);

	/* 已卸任就只拆不补（名单回来时 attach_pick 也会再兜一道）。 */
	if (!(mi->local_capabilities & MIDR_CAP_GROUP_REP))
		return;
	midr_nds_bootstrap_list_fetch(bgp);
}

/* 向兜底游标当前指向的候选发一次 BOOTSTRAP_LIST_REQ。 */
static void midr_nds_bootstrap_probe_attempt(struct bgp *bgp)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct midr_bootstrap_entry *b;
	struct ipaddr local;

	if (!midr_nds_local_transport_get(bgp, &local))
		return;
	while (mi->bootstrap_probe_cur) {
		b = listgetdata(mi->bootstrap_probe_cur);
		if (midr_ipaddr_valid_locator(&b->transport) &&
		    ipaddr_family(&b->transport) == ipaddr_family(&local))
			break;
		mi->bootstrap_probe_cur =
			listnextnode(mi->bootstrap_probe_cur);
	}
	if (!mi->bootstrap_probe_cur) {
		zlog_warn("MIDR 引导名单：无候选与本机 active locator 同族");
		return;
	}
	b = listgetdata(mi->bootstrap_probe_cur);
	midr_ctrl_send_bootstrap_list_request(bgp, b->transport);
	MIDR_FLOW_LOG("MIDR 引导名单：向候选 %pIA [%s] 要名单（第 %u/%u 个候选，兜底遍历）",
		      &b->transport,
		      b->source == MIDR_BOOTSTRAP_SEED ? "seed" : "manual",
		      midr_bootstrap_index(mi, mi->bootstrap_probe_cur),
		      listcount(mi->bootstrap_list));
}

void midr_nds_bootstrap_list_fetch(struct bgp *bgp)
{
	struct bgp_midr_nds *mi;
	struct listnode *node;
	struct midr_session_ledger_entry *e;
	struct midr_bootstrap_entry *b;

	if (!bgp || !bgp->midr_nds_info)
		return;
	mi = bgp->midr_nds_info;

	/*
	 * 开新一轮：清掉上一轮的挂靠失败章（D2）。清在"开一轮"处而不是"收到名单"
	 * 处，照 §8.32 join failover 的成例——拉名单这一步本身也可能失败，若等收到
	 * 名单才清，一轮拉不到名单就会把上一轮的章一直背着。
	 */
	for (ALL_LIST_ELEMENTS_RO(mi->bootstrap_list, node, b))
		b->attach_failed = false;

	/* ① 先问活着的挂靠引导（台账认边：ATTACH 就是"我挂在它上面"那条）。 */
	for (ALL_LIST_ELEMENTS_RO(mi->session_ledger, node, e)) {
		if (e->reason != MIDR_SESSION_ATTACH)
			continue;
		if (!midr_nds_transport_session_up(bgp, e->transport))
			continue;
		midr_ctrl_send_bootstrap_list_request(bgp, e->transport);
		MIDR_FLOW_LOG("MIDR 引导名单：向挂靠中的引导 %pIA 要名单",
			      &e->transport);
		return;
	}

	/* ② 一台可问的都没有 → 兜底遍历（防代表孤岛的最后防线）。 */
	if (!mi->bootstrap_list || list_isempty(mi->bootstrap_list)) {
		zlog_warn("MIDR 引导名单：无挂靠会话可问，且候选清单为空——无法拉取活引导名单（请配 midr bootstrap 或 midr session）");
		return;
	}
	mi->bootstrap_probe_cur = listhead(mi->bootstrap_list);
	MIDR_LOG("MIDR 引导名单：无挂靠会话可问，退候选清单挨个试（%u 个候选）",
		 listcount(mi->bootstrap_list));
	midr_nds_bootstrap_probe_attempt(bgp);
}

void midr_nds_bootstrap_list_failed(struct bgp *bgp, struct ipaddr failed)
{
	struct bgp_midr_nds *mi;
	struct midr_bootstrap_entry *b;

	if (!bgp || !bgp->midr_nds_info)
		return;
	mi = bgp->midr_nds_info;

	/* 守卫（照 §8.32 failover 成例）：只认"当前正在试的那个候选"的死心。
	 * 走①问挂靠引导那条路死心时 probe_cur 为 NULL——不是兜底遍历，不换台，
	 * 由下一次钩子事件（另一条挂靠会话 Down）再来一轮。 */
	if (!mi->bootstrap_probe_cur)
		return;
	b = listgetdata(mi->bootstrap_probe_cur);
	if (!midr_ipaddr_same(&b->transport, &failed))
		return;

	mi->bootstrap_probe_cur = listnextnode(mi->bootstrap_probe_cur);
	if (mi->bootstrap_probe_cur) {
		struct midr_bootstrap_entry *next =
			listgetdata(mi->bootstrap_probe_cur);

		zlog_info("MIDR 引导名单：%pIA 问不到，改试 %pIA（第 %u/%u 个候选）",
			  &failed, &next->transport,
			  midr_bootstrap_index(mi, mi->bootstrap_probe_cur),
			  listcount(mi->bootstrap_list));
		midr_nds_bootstrap_probe_attempt(bgp);
		return;
	}

	/* 全部耗尽：脱骨干。响亮 warn 后停住，**不设重试定时器**（结论 9）——
	 * 群内互联不受影响，断的只是跨群情报路径；恢复靠人工命令
	 * （midr bootstrap / midr session）、或本机重启走种子自举。 */
	zlog_warn("MIDR 引导名单：全部 %u 个候选都问不到，本节点已脱离骨干网（群内不受影响，跨群情报中断）——请检查引导节点存活与 underlay 路由，修好后在本机敲一条 midr bootstrap 即可拉回",
		  listcount(mi->bootstrap_list));
}

void midr_join_on_rep_list(struct bgp *bgp)
{
	struct bgp_midr_nds *mi;
	struct listnode *node;
	struct midr_rep_entry *r;

	if (!bgp || !bgp->midr_nds_info)
		return;
	mi = bgp->midr_nds_info;

	/*
	 * 加入意图守卫：只有意图仍有效才进入 join。运维手动换组会作废意图，此后
	 * 迟到的 REP_LIST_RESP（REP_LIST_REQ 3s×5 重传可能在换组后才回来）到这里
	 * 被静默丢弃，避免 join 把运维强制切换的群号覆盖掉。
	 */
	if (!mi->join_intent) {
		MIDR_LOG("MIDR 加入：加入意图已作废（多半被手动换组中止），丢弃迟到的 REP_LIST_RESP");
		return;
	}

	if (!mi->rep_dir || list_isempty(mi->rep_dir)) {
		MIDR_FLOW_LOG("MIDR 加入：群代表目录为空，放弃加入");
		return;
	}

	/*
	 * 进入"探测群代表"阶段：此后 PM 经 I-5 回灌时按 join_phase 发
	 * REP_PROBE_DONE，交给 CL 选最优代表（I-7 RECOMMEND），再由
	 * midr_nds_on_cluster_decision 发 MEMBER_LIST_REQ 取成员列表。
	 * NDS 不再自己"取首条"选群——选群职责归 CL。
	 */
	mi->join_in_progress = true;
	mi->join_phase = MIDR_JOIN_PROBING_REPS;
	/* §8.32：第一跳完成，收起 failover 游标；候选清单保留（供展示与
	 * §8.31 种子写库）。此后目录已到手，无第一跳可 failover。 */
	mi->bootstrap_cur = NULL;

	/* I-1：对每个群代表启动探测（PM stub 同步把指标灌进 link_entry，不再 notify）。 */
	for (ALL_LIST_ELEMENTS_RO(mi->rep_dir, node, r)) {
		struct prefix locator;
		struct midr_node_entry key = {};
		struct midr_node_entry *entry;

		/*
		 * 群代表此时尚未有权威 remote-view 条目（bootstrap 交换只提供目录），
		 * 而 midr_pm_add_target() 要求
		 * 探测目标已存在于 global_view 才会启动探测，故先灌一条条目。
		 * 键恒取真名（router-id）：稍后该代表的权威 Node fact / MEMBER_LIST
		 * 正式条目同键命中同一条，一台机器不占两条表项、两个探测 ctx 不会
		 * 抢同一地址的回复。
		 *
		 * rid=0 的旧占位路（拿 transport 冒充 node_id 建条目）已于
		 * 2026-08-11（批 1.5）删除——它正是影子条目的成因：占位条目与真名
		 * 条目两键并存、无人合并，污染 CL 的群规模/好链路计数，且"会不会
		 * 自愈"取决于 pm_is_known_transport 遍历先命中谁（哈希桶顺序）。
		 * 三个 rid=0 来源均已灭：旧 v1 引导被版本字节挡在门外、手配目录
		 * 随 `midr rep group` 删除、引导未收敛由"空目录沉默 + 3s×5 死心 +
		 * failover"盖住。故此处收到 rid=0 只可能是对端异常，**告警 + 跳过
		 * 该条**（该候选群不参与本轮评估，宁少评不造影子）。
		 *
		 * 守卫：rid 是自己时跳过（会撞 self 条目——自己就是该群代表时，
		 * self 条目启动即有，无需也不该在此重建/重探）。
		 */
		if (r->rep_rid.s_addr == INADDR_ANY) {
			zlog_warn("MIDR 加入：群 %u 代表 %pIA 的目录条目无 router-id（rid=0），跳过该条——不建占位条目",
				  r->group_id, &r->rep_transport);
			continue;
		}
		if (IPV4_ADDR_SAME(&r->rep_rid, &bgp->router_id))
			continue;

		midr_prefix_from_in_addr(&locator, r->rep_rid);
		key.node_id = locator;
		entry = midr_node_hash_find(&mi->global_view->nodes, &key);
		if (!midr_nds_control_locator_usable(
			    bgp, &key.node_id, entry, &r->rep_transport,
			    "群代表目录"))
			continue;
		if (!entry) {
			entry = XCALLOC(MTYPE_MIDR_NODE_ENTRY, sizeof(*entry));
			entry->node_id = locator;
			midr_node_hash_add(&mi->global_view->nodes, entry);
		}
		entry->asn = r->rep_asn;
		entry->group_id = r->group_id;
		entry->transport_addr = r->rep_transport;
		entry->has_transport_addr = true;
		entry->last_update = monotime(NULL);

		/*
		 * Only the bootstrap rep (the one that received our
		 * REP_LIST_REQ) already knows who we are — bootstrap != a
		 * given rep in general (a rep_dir can list reps other than
		 * the bootstrap itself, e.g. group 2's rep here). Any other
		 * rep has never exchanged a single control message with us,
		 * so its own global_view has no entry for us at all and
		 * pm_is_known_transport() will reject every PM probe we send
		 * it. Self-announce to each rep before I-1 starts probing so
		 * midr_nds_learn_requester() seeds that entry up front
		 * (harmless — and idempotent — if the rep already knows us).
		 */
		midr_ctrl_send_announce(bgp, r->rep_transport);

		midr_pm_add_target(bgp, &locator, MIDR_SRC_BOOTSTRAP, 0);
		MIDR_FLOW_LOG("MIDR 加入：I-1 探测群代表 %pIA（群 %u）",
			      &r->rep_transport, r->group_id);
	}

	/* 延迟 MIDR_JOIN_PROBE_WAIT_SECS 秒再发 REP_PROBE_DONE，让 PM 的
	 * 长期 EWMA（α=0.05）先积累足够样本，使 CL 能区分好/坏链路。 */
	event_cancel(&mi->t_rep_probe_done);
	event_add_timer(bm->master, midr_join_rep_probe_done_cb, bgp,
			MIDR_JOIN_PROBE_WAIT_SECS, &mi->t_rep_probe_done);
	MIDR_FLOW_LOG("MIDR 加入：REP_PROBE_DONE 将在 %d 秒后触发（等待 EWMA 热身）",
		      MIDR_JOIN_PROBE_WAIT_SECS);
}

/* ===========================================================================
 * Module lifecycle
 * =========================================================================*/

/*
 * 配置读完后的挂靠补拉（D1，挂 FRR 的 bgp_config_end 钩子，bgpd.h 声明处）。
 * 治的是"顺序病"：conf 里 `midr role group-rep` 排在 `midr bootstrap` 之前时，
 * setter 那一刻候选池是空的，动作被推迟到这里统一补。
 * 只在两条都成立时补拉：本机是群代表 ∧ ATTACH 账不足 K。其余情况静默——普通
 * 节点、非代表、已挂够的，本回调什么也不做。
 *
 * 〔前身 = 5a 的 30s 一次性补检定时器，08-13 审查否掉（"猜一个秒数太看运气，
 *   真实网络的收敛时长没人担保"）。它当时一人兼治两病，拆开后：顺序病归本
 *   回调，启动期的就绪病归两层重试窗（拉名单逐台 15s×5 + D2 的 ATTACH
 *   failover），谁也不靠猜秒数。〕
 */
static int midr_nds_attach_after_cfg(struct bgp *bgp)
{
	/* 判据与触发原因都收在 ensure 里（批 6 起三处共用：本回调、能力位 setter、
	 * join 落定），这里只负责在对的时机叫它一次；空指针也由它挡。 */
	midr_nds_attach_ensure(bgp, "配置读完（bgp_config_end）");
	return 0;
}

/*
 * 钩子 (b) 的下一拍（D4）：把钩子记下的掉线地址逐条取出来处理。
 *
 * 为什么要隔这一拍——见 attach_down_pending 的字段注释（FSM 喊完还要回来摸这条
 * 会话，钩子上下文里 peer_delete 是重入雷区）。真正的判据留在
 * midr_nds_attach_on_session_down() 里再查一遍（那两个 if 就是幂等闸）。
 */
static void midr_attach_reap_cb(struct event *t)
{
	struct bgp *bgp = EVENT_ARG(t);
	struct bgp_midr_nds *mi;
	struct listnode *node, *nnode;
	struct ipaddr *tr;

	if (!bgp || !bgp->midr_nds_info)
		return;
	mi = bgp->midr_nds_info;

	/* 先摘节点再处理：on_session_down 会拆会话、拉名单，途中不该再看见这条。 */
	for (ALL_LIST_ELEMENTS(mi->attach_down_pending, node, nnode, tr)) {
		struct ipaddr transport = *tr;

		list_delete_node(mi->attach_down_pending, node);
		XFREE(MTYPE_MIDR_ATTACH_DOWN, tr);
		midr_nds_attach_on_session_down(bgp, transport);
	}
}

/*
 * 钩子 (b) 在钩子上下文里做的全部事情：只读地筛一道（这地址上是不是我的挂靠
 * 边），是就把地址记进小本本、排一个零延时事件，**什么也不拆**。
 * 预筛只为省掉无关会话的记名开销，不作数——作数的判据在下一拍再查一遍。
 */
static void midr_nds_attach_note_down(struct bgp *bgp, struct ipaddr transport)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	const struct midr_session_ledger_entry *e;
	struct ipaddr *slot;

	e = midr_nds_ledger_lookup(bgp, transport);
	if (!e || e->reason != MIDR_SESSION_ATTACH)
		return;

	slot = XCALLOC(MTYPE_MIDR_ATTACH_DOWN, sizeof(*slot));
	*slot = transport;
	listnode_add(mi->attach_down_pending, slot);

	event_add_event(bm->master, midr_attach_reap_cb, bgp, 0,
			&mi->t_attach_reap);
}

/*
 * 会话建立上沿的 link 首报。只有当前 locator 世代已经取得真实测量值时才上报；
 * 否则等待 PM 的首次回灌。不得为新会话沿用旧 locator 的指标，也不得用占位值
 * 制造一条上层看来已经可度量的链路。
 */
static void midr_nds_report_link_on_established(struct bgp *bgp,
						struct ipaddr peer_addr)
{
	struct midr_node_entry *entry;

	frr_each (midr_node_hash, &bgp->midr_nds_info->global_view->nodes,
		  entry) {
		struct midr_link_entry *le;
		if (!entry->has_transport_addr ||
		    !midr_ipaddr_same(&entry->transport_addr, &peer_addr))
			continue;

		le = midr_global_view_find_link(bgp->midr_nds_info->global_view,
						&entry->node_id);
		if (le && le->short_term.rtt_us) {
			midr_nds_report_link(bgp, le);
		}
		return;
	}
}

/*
 * 件④（轮 4）掉沿清账：MIDR 会话掉出 Established = 对端真死或真要重来
 * （默认 holdtime 180s 下 underlay 抖动根本走不到这个沿），当场把这条边的账
 * 清干净——拆 peer + 销台账 + 停探 + 撤 link 事件 + 清 is_adjacent + 通知 CL，
 * 即 midr_ctrl_detach_transport 那一整套（B1 老化调的也是它，本批只是把同一
 * 动作从 300s 提前到当场）。MANUAL 边由 force=false 保住配置本体（α 豁免）。
 *
 * 重连不在这里管：同群边由 periodic_sync 的补边兜底扫描（≤30s）重建，
 * midr_ctrl_connect 的 SAME_GROUP 分支会把 is_adjacent/探测/台账一次置全。
 *
 * 钩子里只记名、下一拍再动手，理由同 attach_down_pending 的字段注释。
 */
struct midr_session_down {
	struct ipaddr transport;
	struct in_addr rid;
	bool invalid_identity;
};

static void midr_session_reap_cb(struct event *t)
{
	struct bgp *bgp = EVENT_ARG(t);
	struct bgp_midr_nds *mi;
	struct listnode *node, *nnode;
	struct midr_session_down *sd;

	if (!bgp || !bgp->midr_nds_info)
		return;
	mi = bgp->midr_nds_info;

	for (ALL_LIST_ELEMENTS(mi->session_down_pending, node, nnode, sd)) {
		struct midr_session_down cur = *sd;

		list_delete_node(mi->session_down_pending, node);
		XFREE(MTYPE_MIDR_SESSION_DOWN, sd);
		if (cur.invalid_identity) {
			/* Defer peer deletion until outside peer_status_changed. */
			(void)midr_nds_detach_by_locator(bgp, cur.transport);
			midr_nds_ledger_drop(bgp, cur.transport);
			midr_ctrl_forget_target(bgp, cur.transport);
			continue;
		}

		zlog_info("MIDR 会话掉线：%pIA（rid %pI4）掉出 Established，拆边清账",
			  &cur.transport, &cur.rid);
		midr_ctrl_detach_transport(bgp, cur.transport, cur.rid, false,
					   MIDR_STOP_SESSION_DOWN);
	}
}

static void midr_nds_session_note_down(struct bgp *bgp,
				       struct ipaddr transport)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	const struct midr_session_ledger_entry *e;
	struct midr_session_down *slot;

	/*
	 * ⚠ 退网中不记名（08-23 实测必崩）：本机退网是"先撤身份、再自己把会话
	 * 全拆掉"，清表/停探/销账 shutdown_enter 已一次做完，这里再排一拍无事
	 * 可做；而那一拍会在**身份已撤**之后再碰一次路由处理，撞上第二组
	 * `bgp_midr_rib_process_main()` 的 `active_identity_count` 断言 → abort
	 * （与记档 56 那个 5s 延时同一个坑，只是从另一条路踩进去）。
	 * 对端不受影响：它不在退网态，掉沿照常清账。
	 */
	if (mi->shutdown)
		return;

	/* 无账的会话不归 MIDR 管（⑦ 归属守卫同款判据的账本侧）。 */
	e = midr_nds_ledger_lookup(bgp, transport);
	if (!e)
		return;

	slot = XCALLOC(MTYPE_MIDR_SESSION_DOWN, sizeof(*slot));
	slot->transport = transport;
	slot->rid = e->remote_rid;
	listnode_add(mi->session_down_pending, slot);

	event_add_event(bm->master, midr_session_reap_cb, bgp, 0,
			&mi->t_session_reap);
}

/*
 * 钩子 (b) 与 B1 断连老化共用的触发源：FRR 自带的 peer 状态变化钩子
 * （bgp_fsm.c 的 DEFINE_HOOK(peer_status_changed)，bmp/dump/snmp 三处先例）。
 * 结构 = 先认人（是不是 MIDR 建的会话）后分流（连上了 / 断了），两个消费者：
 *   · 回到 Established → B1 停表；
 *   · 掉出 Established → B1 起表 + 钩子 (b) 记名（下一拍去拆、去补挂靠；D4：
 *     官方那三个消费者也都不在钩子上下文里动 peer 的生死，我们照同款姿势）。
 *
 * 认人只看 PEER_FLAG_MIDR_OVERLAY 标记（批 5c 起全树统一，⑦ 判据的签名那半已
 * 删）：运维原生会话永远没有这个标记，标记这一条就挡住了。
 *
 * hook 是**进程级**的（不是 per-bgp-instance），所以注册一次即可——用 static
 * 守卫挡住多实例重复注册（重复注册会让回调被调多次）。
 */
static int midr_nds_peer_status_hook(struct peer *peer)
{
	union sockunion *su;
	struct ipaddr transport;

	if (!peer || !peer->bgp || !peer->bgp->midr_nds_info || !peer->connection)
		return 0;
	if (peer->bgp->midr_nds_info->transport_reconfiguring)
		return 0;
	if (!CHECK_FLAG(peer->flags, PEER_FLAG_MIDR_OVERLAY))
		return 0; /* 非 MIDR 建的会话，与挂靠、台账都无关 */

	su = &peer->connection->su;
	if (!midr_sockunion_to_ipaddr(su, &transport) ||
	    !midr_ipaddr_valid_locator(&transport))
		return 0;

	if (peer->connection->status == Established) {
		if (!midr_manual_session_note_identity(peer->bgp, transport,
						      peer->remote_id)) {
			struct bgp_midr_nds *mi = peer->bgp->midr_nds_info;
			struct midr_session_down *slot;

			slot = XCALLOC(MTYPE_MIDR_SESSION_DOWN, sizeof(*slot));
			slot->transport = transport;
			slot->rid = peer->remote_id;
			slot->invalid_identity = true;
			listnode_add(mi->session_down_pending, slot);
			event_add_event(bm->master, midr_session_reap_cb,
					peer->bgp, 0, &mi->t_session_reap);
			return 0;
		}
		midr_ledger_note_up(peer->bgp, transport);
		midr_nds_report_link_on_established(peer->bgp, transport);
		return 0;
	}

	/*
	 * 往下只认**掉出 Established 的那个沿**：ostatus == Established ∧ 现在不是。
	 *
	 * ⚠ 少了 ostatus 这一半会自己跟自己打架（08-13 实测踩到）：新建的挂靠 peer
	 * 一路 Idle→Connect→Active 每步都触发本钩子，而它此刻已经有 ATTACH 账，于是
	 * 被当成"挂靠掉线" → 拆账 → 重拉名单 → 重挑 → 再建 peer → 再触发……一秒内
	 * 循环好几轮，会话永远建不成。"没连上"和"连上后又断了"是两回事。
	 * B1 的计时同理挂在这个沿上——起点是 BGP 判死那一刻，不是物理断开那一刻。
	 */
	if (peer->connection->ostatus != Established)
		return 0;

	midr_ledger_note_down(peer->bgp, transport);
	midr_nds_attach_note_down(peer->bgp, transport);
	midr_nds_session_note_down(peer->bgp, transport);
	return 0;
}

/* ===========================================================================
 * 件③：第二组 remote-view 回调（轮 4）—— 节点表的权威数据源
 *
 * 旧 Node NLRI 收包入口已经删除。本回调写 global_view 并复用
 * midr_nds_node_react 的反应链；Control list/request 只允许临时补全且不能迁移
 * 已有身份的 locator。
 *
 * ⚠ 回调签名不带 bgp（第二组接口所定），只能取默认实例。
 * ⚠ asn 他们的对象里没有（membership 只有 group/transport/caps），故换源后
 *    entry->asn 恒 0 —— 相关守卫已在本轮放宽，见 midr_ctrl_connect。
 * =========================================================================*/

static struct bgp *midr_nds_remote_bgp(void)
{
	struct bgp *bgp = bgp_get_default();

	return (bgp && bgp->midr_nds_info) ? bgp : NULL;
}

/* 撤销时记一笔（虚报观察探针）。同 rid 只留最近一次。 */
static void midr_nds_remote_note_withdraw(struct bgp *bgp, struct in_addr rid)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct listnode *node, *nnode;
	struct midr_remote_withdrawn *w;

	if (!mi->remote_withdrawn)
		return;

	for (ALL_LIST_ELEMENTS(mi->remote_withdrawn, node, nnode, w)) {
		if (w->rid.s_addr == rid.s_addr) {
			w->at = monotime(NULL);
			return;
		}
	}

	w = XCALLOC(MTYPE_BGP_MIDR, sizeof(*w));
	w->rid = rid;
	w->at = monotime(NULL);
	listnode_add(mi->remote_withdrawn, w);
}

/* update 到达时查探针：撤销后短窗内又出现即计一次疑似虚报，顺手清过期条目。 */
static void midr_nds_remote_seen_update(struct bgp *bgp, struct in_addr rid)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct listnode *node, *nnode;
	struct midr_remote_withdrawn *w;
	time_t now = monotime(NULL);

	if (!mi->remote_withdrawn)
		return;

	for (ALL_LIST_ELEMENTS(mi->remote_withdrawn, node, nnode, w)) {
		if (w->rid.s_addr != rid.s_addr) {
			if (now - w->at > MIDR_REMOTE_SUSPECT_WINDOW) {
				listnode_delete(mi->remote_withdrawn, w);
				XFREE(MTYPE_BGP_MIDR, w);
			}
			continue;
		}

		if (now - w->at <= MIDR_REMOTE_SUSPECT_WINDOW) {
			mi->remote_suspect_count++;
			zlog_info("MIDR 远端视图：%pI4 撤销后 %lds 内又被通告，疑似虚报（累计 %" PRIu64 " 次）",
				  &rid, (long)(now - w->at),
				  mi->remote_suspect_count);
		}
		listnode_delete(mi->remote_withdrawn, w);
		XFREE(MTYPE_BGP_MIDR, w);
		return;
	}
}

/*
 * 下行逆换算的**单一出口**：第二组的 remote 结构 → 我方节点条目的字段。
 * 增量回调与 snapshot 对账共用，避免两处各写一遍、口径漂移。
 *
 * 三条口径（写错任一条都不会报错、只会静默不一致）：
 *   rid —— 他们的 node_id 与 router_id.s_addr 同为网络序 4 字节，**勿再套 htonl**；
 *   caps —— cap_flags 是 uint64，我方 capabilities 是 uint32，取低 32 位（与上行
 *           facts 层"低 32 位放能力位"对称）；
 *   transport —— 接受合法 IPv4/IPv6 单播 locator；缺失或非法一律当没有。
 */
void midr_nds_remote_node_decode(const struct midr_remote_node_info *node,
				 struct in_addr *rid, uint32_t *caps,
				 struct ipaddr *transport, bool *has_transport)
{
	rid->s_addr = node->node_id;
	*caps = (uint32_t)node->cap_flags;
	*transport = node->transport_address;
	*has_transport = node->has_transport_address &&
			 midr_ipaddr_valid_locator(transport);
	if (!*has_transport)
		SET_IPADDR_NONE(transport);
}

struct midr_remote_locator_intent {
	bool present;
	enum midr_session_reason reason;
	struct in_addr remote_rid;
	as_t remote_asn;
	uint32_t remote_group;
};

/* Drop deferred peer-down work for an address whose locator generation has
 * ended.  Otherwise the zero-delay callbacks can later delete the replacement
 * edge after the node entry has already moved to its new address. */
static void midr_nds_remote_clear_deferred(struct bgp_midr_nds *mi,
					   struct ipaddr old_transport)
{
	struct listnode *node, *nnode;
	struct ipaddr *attach;
	struct midr_session_down *down;

	for (ALL_LIST_ELEMENTS(mi->attach_down_pending, node, nnode, attach)) {
		if (!midr_ipaddr_same(attach, &old_transport))
			continue;
		list_delete_node(mi->attach_down_pending, node);
		XFREE(MTYPE_MIDR_ATTACH_DOWN, attach);
	}
	if (list_isempty(mi->attach_down_pending))
		event_cancel(&mi->t_attach_reap);

	for (ALL_LIST_ELEMENTS(mi->session_down_pending, node, nnode, down)) {
		if (!midr_ipaddr_same(&down->transport, &old_transport))
			continue;
		list_delete_node(mi->session_down_pending, node);
		XFREE(MTYPE_MIDR_SESSION_DOWN, down);
	}
	if (list_isempty(mi->session_down_pending))
		event_cancel(&mi->t_session_reap);
}

/* rep_dir is learned runtime state, so entries that identify this node move
 * with its advertised locator.  A missing/conflicting replacement removes
 * them.  The second pass folds any duplicate created by the re-key. */
static void midr_nds_remote_rekey_rep_dir(struct bgp_midr_nds *mi,
					  struct in_addr rid,
					  const struct ipaddr *old_transport,
					  const struct ipaddr *new_transport)
{
	struct listnode *node, *nnode;
	struct midr_rep_entry *rep;
	bool old_valid = midr_ipaddr_valid_locator(old_transport);
	bool new_valid = midr_ipaddr_valid_locator(new_transport);

	for (ALL_LIST_ELEMENTS(mi->rep_dir, node, nnode, rep)) {
		bool match_rid = rep->rep_rid.s_addr == rid.s_addr;
		bool match_old = old_valid &&
				 midr_ipaddr_same(&rep->rep_transport,
						   old_transport);

		if (!match_rid && !match_old)
			continue;
		if (!new_valid) {
			list_delete_node(mi->rep_dir, node);
			XFREE(MTYPE_MIDR_REP_ENTRY, rep);
			continue;
		}
		rep->rep_transport = *new_transport;
		rep->rep_rid = rid;
	}

	for (ALL_LIST_ELEMENTS(mi->rep_dir, node, nnode, rep)) {
		struct listnode *scan;
		struct midr_rep_entry *prior;

		for (scan = listhead(mi->rep_dir); scan && scan != node;
		     scan = listnextnode(scan)) {
			prior = listgetdata(scan);
			if (prior->group_id != rep->group_id ||
			    !midr_ipaddr_same(&prior->rep_transport,
						  &rep->rep_transport))
				continue;
			if (!prior->rep_asn)
				prior->rep_asn = rep->rep_asn;
			if (prior->rep_rid.s_addr == INADDR_ANY)
				prior->rep_rid = rep->rep_rid;
			list_delete_node(mi->rep_dir, node);
			XFREE(MTYPE_MIDR_REP_ENTRY, rep);
			break;
		}
	}
}

/* Learned seed entries follow a node identity.  MANUAL entries are persistent
 * exact-address operator intent: preserve their text, but skip a stale one in
 * an in-flight traversal so it cannot block failover forever. */
static void midr_nds_remote_rekey_bootstraps(struct bgp *bgp,
					     struct in_addr rid,
					     const struct ipaddr *old_transport,
					     const struct ipaddr *new_transport)
{
	struct bgp_midr_nds *mi = bgp->midr_nds_info;
	struct listnode *node, *nnode;
	struct midr_bootstrap_entry *bootstrap;
	bool old_valid = midr_ipaddr_valid_locator(old_transport);
	bool new_valid = midr_ipaddr_valid_locator(new_transport);
	bool restart_join = false, restart_probe = false;

	for (ALL_LIST_ELEMENTS(mi->bootstrap_list, node, nnode, bootstrap)) {
		struct listnode *duplicate_node = NULL;
		struct midr_bootstrap_entry *duplicate = NULL;
		bool match_rid = bootstrap->rid.s_addr == rid.s_addr;
		bool match_old = old_valid &&
				 midr_ipaddr_same(&bootstrap->transport,
						   old_transport);

		if (!match_rid && !match_old)
			continue;

		if (bootstrap->source == MIDR_BOOTSTRAP_MANUAL) {
			if (new_valid &&
			    midr_ipaddr_same(&bootstrap->transport, new_transport))
				continue;
			bootstrap->failed = true;
			bootstrap->attach_failed = true;
			if (mi->bootstrap_cur == node) {
				mi->bootstrap_cur = nnode;
				restart_join = true;
			}
			if (mi->bootstrap_probe_cur == node) {
				mi->bootstrap_probe_cur = nnode;
				restart_probe = true;
			}
			continue;
		}

		if (new_valid)
			duplicate = midr_bootstrap_find(mi, *new_transport,
							 &duplicate_node);
		if (duplicate == bootstrap) {
			bootstrap->rid = rid;
			continue;
		}

		if (new_valid && duplicate &&
		    duplicate->rid.s_addr == rid.s_addr) {
			if (!duplicate->asn)
				duplicate->asn = bootstrap->asn;
			duplicate->in_last_list |= bootstrap->in_last_list;
			if (mi->bootstrap_cur == node) {
				mi->bootstrap_cur = duplicate_node;
				restart_join = true;
			}
			if (mi->bootstrap_probe_cur == node) {
				mi->bootstrap_probe_cur = duplicate_node;
				restart_probe = true;
			}
			list_delete_node(mi->bootstrap_list, node);
			XFREE(MTYPE_MIDR_BOOTSTRAP_ENTRY, bootstrap);
			continue;
		}

		if (new_valid && !duplicate) {
			bootstrap->transport = *new_transport;
			bootstrap->rid = rid;
			if (mi->bootstrap_cur == node)
				restart_join = true;
			if (mi->bootstrap_probe_cur == node)
				restart_probe = true;
			continue;
		}

		/* No usable replacement, or the new locator is already owned by a
		 * different configured identity. */
		if (mi->bootstrap_cur == node) {
			mi->bootstrap_cur = nnode;
			restart_join = true;
		}
		if (mi->bootstrap_probe_cur == node) {
			mi->bootstrap_probe_cur = nnode;
			restart_probe = true;
		}
		list_delete_node(mi->bootstrap_list, node);
		XFREE(MTYPE_MIDR_BOOTSTRAP_ENTRY, bootstrap);
	}

	if (restart_join && mi->join_intent) {
		if (mi->bootstrap_cur)
			midr_bootstrap_start_attempt(bgp);
		else
			zlog_warn("MIDR JOIN: locator 变更后已无可继续尝试的引导候选（加入意图保留）");
	}
	if (restart_probe && mi->bootstrap_probe_cur)
		midr_nds_bootstrap_probe_attempt(bgp);
}

static bool midr_nds_remote_take_ledger(struct bgp *bgp,
					struct ipaddr old_transport,
					struct midr_remote_locator_intent *intent)
{
	const struct midr_session_ledger_entry *ledger;

	memset(intent, 0, sizeof(*intent));
	ledger = midr_nds_ledger_lookup(bgp, old_transport);
	if (!ledger)
		return false;
	intent->present = true;
	intent->reason = ledger->reason;
	intent->remote_rid = ledger->remote_rid;
	intent->remote_asn = ledger->remote_asn;
	intent->remote_group = ledger->remote_group;
	midr_nds_ledger_drop(bgp, old_transport);
	return true;
}

static void midr_nds_remote_restore_intent(
	struct bgp *bgp, struct midr_node_entry *entry,
	const struct midr_remote_locator_intent *intent)
{
	struct midr_node_entry target;
	bool send_nudge;

	if (!intent->present || intent->reason == MIDR_SESSION_MANUAL ||
	    !entry->has_transport_addr ||
	    !midr_ipaddr_valid_locator(&entry->transport_addr) ||
	    !midr_nds_locator_unique(bgp, &entry->node_id,
				     &entry->transport_addr))
		return;
	/* SAME_GROUP is durable membership intent, not a statement that the edge
	 * can be opened under the locator currently active locally.  In particular,
	 * preserve it across an IPv4/IPv6 mismatch so a later local transport
	 * switch can restore the edge.  Re-check only family-independent membership
	 * invariants here; midr_ctrl_connect() remains the fail-closed runtime gate. */
	if (intent->reason == MIDR_SESSION_SAME_GROUP &&
	    (!entry->group_id ||
	     entry->group_id != bgp->midr_nds_info->local_group_id ||
	     entry->is_self ||
	     !midr_nds_locator_unique(bgp, &entry->node_id,
				      &entry->transport_addr)))
		return;
	if (midr_nds_is_session_excluded(bgp, entry->node_id.u.prefix4))
		return;

	/* Record intent even when the currently active local locator is another
	 * family; a later local transport switch can then restore it. */
	midr_nds_ledger_note(bgp, entry->transport_addr, intent->reason,
			     entry->node_id.u.prefix4,
			     entry->asn ? entry->asn : intent->remote_asn,
			     entry->group_id);
	target = *entry;
	if (!target.asn)
		target.asn = intent->remote_asn;
	send_nudge = intent->reason != MIDR_SESSION_PEER_REQ_REPLY;
	midr_ctrl_connect(bgp, &target, intent->reason, send_nudge);
}

static void midr_nds_remote_node_update(const struct midr_remote_node_info *node)
{
	struct bgp *bgp = midr_nds_remote_bgp();
	struct midr_global_view *gv;
	struct midr_node_entry key = {};
	struct midr_node_entry *entry;
	struct in_addr rid;
	uint32_t caps;
	struct ipaddr transport = {};
	struct ipaddr old_transport = midr_ipaddr_none();
	struct midr_remote_locator_intent intent = {};
	bool has_transport;
	bool is_new = false, changed = false, group_changed = false;
	bool old_has_transport, locator_changed, locator_conflict = false;
	uint32_t prev_gid = 0;

	if (!bgp || !node)
		return;

	/* 退网守卫（与 on_node_nlri 开头那道同源，一字不改的语义）：本机退网期间
	 * 一律不吃远端节点事实，否则刚清空的节点表会被回调灌回来。 */
	if (bgp->midr_nds_info->shutdown) {
		MIDR_LOG("MIDR 退网：丢弃收到的远端 Node fact（本机已退网）");
		return;
	}

	midr_nds_remote_node_decode(node, &rid, &caps, &transport,
				    &has_transport);
	/* Reject a mixed-family update before touching identity or metadata. */
	if (has_transport && bgp->midr_nds_info->transport_addr_set &&
	    ipaddr_family(&transport) !=
		    ipaddr_family(&bgp->midr_nds_info->local_transport_addr)) {
		zlog_warn("MIDR remote view: ignoring mixed-family Node locator %pIA",
			  &transport);
		return;
	}
	if (rid.s_addr == INADDR_ANY) {
		zlog_warn("MIDR 远端视图：拒收 router-id 0.0.0.0 的 Node 事实");
		return;
	}
	midr_prefix_from_in_addr(&key.node_id, rid);

	/* is_self：防自己的回声。他们那侧也按 originator == router_id 滤过一道
	 * （bgp_midr_lsdb.c），这里是不依赖对方实现的第二道。 */
	if (midr_prefix_is_self(bgp, &key.node_id))
		return;

	bgp->midr_nds_info->remote_node_events++;

	gv = bgp->midr_nds_info->global_view;
	entry = midr_node_hash_find(&gv->nodes, &key);

	if (!entry) {
		entry = XCALLOC(MTYPE_MIDR_NODE_ENTRY, sizeof(*entry));
		entry->node_id = key.node_id;
		midr_node_hash_add(&gv->nodes, entry);
		is_new = true;
	}

	if (entry->group_id != node->group_id) {
		changed = true;
		group_changed = true;
		prev_gid = entry->group_id;
		entry->group_id = node->group_id;
	}

	/* ls_sequence 当能力位的序号用（与 TLV 1187 的 seqno 同语义：只收更新的）。 */
	if (node->ls_sequence >= entry->cap_seqno) {
		if (entry->capabilities != caps)
			changed = true;
		entry->capabilities = caps;
		entry->cap_seqno = node->ls_sequence;
	}

	old_has_transport = entry->has_transport_addr &&
			    midr_ipaddr_valid_locator(&entry->transport_addr);
	if (old_has_transport)
		old_transport = entry->transport_addr;

	/* A locator is a unique routable endpoint, not merely an address-shaped
	 * attribute.  On conflict fail closed and retire the previous generation. */
	if (has_transport &&
	    ((bgp->midr_nds_info->transport_active &&
	      midr_ipaddr_same(&transport,
			       &bgp->midr_nds_info->active_transport_addr)) ||
	     !midr_nds_locator_unique(bgp, &entry->node_id, &transport))) {
		zlog_warn("MIDR 远端视图：node %pI4 的 locator %pIA 与其它节点或本机冲突，拒绝激活",
			  &rid, &transport);
		/* Retain the advertised claim so neither RID wins by arrival order.
		 * All runtime users must pass the uniqueness gate. */
		locator_conflict = true;
	}

	locator_changed = old_has_transport != has_transport ||
			   (old_has_transport && has_transport &&
			    !midr_ipaddr_same(&old_transport, &transport)) ||
			   (entry->has_transport_addr && !old_has_transport);
	if (locator_changed) {
		changed = true;
		if (old_has_transport) {
			(void)midr_nds_remote_take_ledger(bgp, old_transport,
						  &intent);
			midr_nds_detach_node(bgp, entry, MIDR_STOP_SESSION_DOWN,
					     false);
			midr_ctrl_forget_target(bgp, old_transport);
			midr_nds_remote_clear_deferred(bgp->midr_nds_info,
						       old_transport);
		}

		if (has_transport) {
			entry->transport_addr = transport;
			entry->has_transport_addr = true;
		} else {
			SET_IPADDR_NONE(&entry->transport_addr);
			entry->has_transport_addr = false;
		}

		midr_nds_remote_rekey_rep_dir(bgp->midr_nds_info, rid,
					       &old_transport,
					       has_transport && !locator_conflict
						       ? &transport : NULL);
		midr_nds_remote_rekey_bootstraps(
			bgp, rid, &old_transport,
			has_transport && !locator_conflict ? &transport : NULL);
	} else if (has_transport) {
		entry->transport_addr = transport;
		entry->has_transport_addr = true;
	} else {
		SET_IPADDR_NONE(&entry->transport_addr);
		entry->has_transport_addr = false;
	}

	if (locator_conflict) {
		struct midr_node_entry *claimant;

		/* Retire every old user, including the earlier claimant.  Keep node
		 * identities/claims for future authoritative updates or withdraws. */
		frr_each (midr_node_hash, &gv->nodes, claimant) {
			if (claimant->is_self || !claimant->has_transport_addr ||
			    !midr_ipaddr_same(&claimant->transport_addr, &transport))
				continue;
			midr_nds_detach_node(bgp, claimant, MIDR_STOP_SESSION_DOWN,
					     false);
			midr_nds_remote_rekey_rep_dir(bgp->midr_nds_info,
				claimant->node_id.u.prefix4, &transport, NULL);
		}
		midr_nds_ledger_drop(bgp, transport);
		midr_ctrl_forget_target(bgp, transport);
		midr_nds_remote_clear_deferred(bgp->midr_nds_info, transport);
	}

	entry->last_update = monotime(NULL);
	entry->is_self = false; /* 上面已 return，走到这里必非本机 */

	/* 只在新建/有变化时出声——稳态每拍都有回调，全打会淹掉日志。 */
	if (is_new || changed)
		MIDR_LOG("MIDR 远端视图：node %pI4 群 %u caps=0x%x（第二组回调，%s）",
			 &rid, node->group_id, caps,
			 is_new ? "新建" : "更新");

	midr_nds_remote_seen_update(bgp, rid);

	midr_nds_node_react(bgp, entry, is_new, changed, group_changed,
			    prev_gid);
	if (locator_changed)
		midr_nds_remote_restore_intent(bgp, entry, &intent);
	if (locator_conflict && bgp->midr_nds_info->transport_active &&
	    midr_ipaddr_same(&transport,
			     &bgp->midr_nds_info->active_transport_addr))
		(void)midr_nds_transport_reconcile(bgp);
}

static void midr_nds_remote_node_withdraw(uint32_t node_id, uint64_t ls_sequence)
{
	struct bgp *bgp = midr_nds_remote_bgp();
	struct midr_global_view *gv;
	struct midr_node_entry key = {};
	struct midr_node_entry *entry;
	struct in_addr rid;

	(void)ls_sequence;

	if (!bgp)
		return;
	if (bgp->midr_nds_info->shutdown) {
		MIDR_LOG("MIDR 退网：丢弃收到的远端 Node withdraw（本机已退网）");
		return;
	}

	rid.s_addr = node_id;
	midr_prefix_from_in_addr(&key.node_id, rid);
	if (midr_prefix_is_self(bgp, &key.node_id))
		return;

	bgp->midr_nds_info->remote_node_events++;

	gv = bgp->midr_nds_info->global_view;
	entry = midr_node_hash_find(&gv->nodes, &key);
	if (!entry)
		return;

	midr_nds_remote_note_withdraw(bgp, rid);

	/* ⚠ 这条日志是"谁拆的会话"的定位手段。件②（轮 4）删掉旧 NLRI 线之后，
	 * 与它成对的那条（"…（旧 NLRI 线）"）永久归零——记档 49 复现时不必再分辨
	 * 来路，走 detach 拆会话的只剩本回调一条。 */
	MIDR_LOG("MIDR 远端视图：node 撤销 %pI4（第二组回调）→ 走 detach 清理链",
		 &rid);

	/* 清理链零重写：与 NLRI withdraw 同一条路（停探 / 清 link / 拆会话 /
	 * 删条目 / 通知 CL），只是叫醒者换了。 */
	midr_nds_detach_node(bgp, entry, MIDR_STOP_GRACEFUL_SHUTDOWN, true);
	midr_node_hash_del(&gv->nodes, entry);
	XFREE(MTYPE_MIDR_NODE_ENTRY, entry);
	midr_nds_notify_cl(bgp, MIDR_TRIGGER_NODE_CHANGE);
}

/*
 * link 两个回调本轮**只观察不消费**：我方 link_entry 是本机 PM 的测量结果，
 * 与"别人报的链路"不是一回事；远端链路的消费者是 CL 的全局拓扑，接不接、怎么接
 * 归 CL 定。先接上计数与日志，联调期看清到达情况再议（plan 件③ 范围只到节点表）。
 */
static void midr_nds_remote_link_update(const struct midr_remote_link_info *link)
{
	struct bgp *bgp = midr_nds_remote_bgp();
	struct in_addr local, remote;

	if (!bgp || !link)
		return;

	/* 退网守卫（与 node 侧两处同源）：退网期间不吃任何远端事实。现在 link 回调
	 * 只观察不消费，漏了顶多多两行日志；但 CL 将来真去消费远端 link 时，这个
	 * 洞就张开了。 */
	if (bgp->midr_nds_info->shutdown) {
		MIDR_LOG("MIDR 退网：丢弃收到的远端 Link 事实（本机已退网）");
		return;
	}

	local.s_addr = link->key.local_node_id;
	remote.s_addr = link->key.remote_node_id;
	bgp->midr_nds_info->remote_link_events++;
	MIDR_LOG("MIDR 远端视图：link %pI4 -> %pI4 cost=%u（本轮只观察）",
		 &local, &remote, link->canonical_cost);
}

static void midr_nds_remote_link_withdraw(const struct midr_link_key *key,
					  uint64_t ls_sequence)
{
	struct bgp *bgp = midr_nds_remote_bgp();
	struct in_addr local, remote;

	(void)ls_sequence;

	if (!bgp || !key)
		return;

	/* 退网守卫：同 midr_nds_remote_link_update() 那道。 */
	if (bgp->midr_nds_info->shutdown) {
		MIDR_LOG("MIDR 退网：丢弃收到的远端 Link 事实（本机已退网）");
		return;
	}

	local.s_addr = key->local_node_id;
	remote.s_addr = key->remote_node_id;
	bgp->midr_nds_info->remote_link_events++;
	MIDR_LOG("MIDR 远端视图：link 撤销 %pI4 -> %pI4（本轮只观察）", &local,
		 &remote);
}

/*
 * 回调注册。幂等，可多次调 —— 因为 bgp_midr_nds_init 那一刻**取不到 ctx**：
 * midr_context_get_default() 走的是 bgp_get_default()，而我方 init 在 bgp_create()
 * 执行期间跑，默认实例那时还没挂上去（他们自己的 init 不受影响，用的是传入的
 * bgp 参数）。
 *
 * 三个时机：① init 试一次（将来若早就绪即生效，现在必失败）；② bgp_config_end
 * 补一次（正常路径靠它成功）；③ periodic_sync 每拍兜底重试（轮 5 补）——②
 * 若也失败，此前再没有人试第二次，件③ 整条下行链会**静默**哑掉（节点表永远空
 * 却不报错）。有幂等闸，成功之后 ③ 就是空调用。
 */
static void midr_nds_remote_view_register(struct bgp *bgp)
{
	static const struct midr_remote_view_callbacks cbs = {
		.remote_node_update = midr_nds_remote_node_update,
		.remote_node_withdraw = midr_nds_remote_node_withdraw,
		.remote_link_update = midr_nds_remote_link_update,
		.remote_link_withdraw = midr_nds_remote_link_withdraw,
	};
	struct bgp_midr_nds *mi;
	struct midr_context *ctx;
	int ret;

	if (!bgp || !bgp->midr_nds_info)
		return;
	mi = bgp->midr_nds_info;
	if (mi->remote_view_registered)
		return;

	ctx = midr_nds_group2_ctx(bgp);
	if (!ctx)
		return; /* 还没就绪，等 config_end 那次；init 期属预期，不告警 */

	ret = midr_remote_view_callbacks_register(ctx, &cbs);
	if (ret) {
		/* 首次出声即可：periodic_sync 每 30s 重试一次，全打成 warn 会刷屏。
		 * 后续失败压到 debug，运维侧看 `show midr group2` 的"未注册"。 */
		if (!mi->remote_view_reg_failed) {
			mi->remote_view_reg_failed = true;
			zlog_warn("MIDR 远端视图：回调注册失败 ret=%d —— 已转入每 %d 秒重试，期间本机学不到任何远端节点（`show midr group2` 可查）",
				  ret, MIDR_PERIODIC_SYNC_INTERVAL);
		} else {
			MIDR_LOG("MIDR 远端视图：回调注册重试仍失败 ret=%d", ret);
		}
		return;
	}

	mi->remote_view_registered = true;
	if (mi->remote_view_reg_failed)
		zlog_warn("MIDR 远端视图：回调注册在重试中恢复成功 —— 此前失败期间学到的远端信息可能不全");
	zlog_info("MIDR 远端视图：已向第二组注册 node/link 权威回调");
}

/* config_end 补注册（init 期 ctx 尚未就绪，见上）。 */
static int midr_nds_remote_view_after_cfg(struct bgp *bgp)
{
	midr_nds_remote_view_register(bgp);
	return 0;
}

static void midr_nds_transport_clear_deferred(struct bgp_midr_nds *mi)
{
	struct listnode *node, *nnode;
	struct ipaddr *transport;
	struct midr_session_down *down;

	event_cancel(&mi->t_attach_reap);
	event_cancel(&mi->t_session_reap);
	for (ALL_LIST_ELEMENTS(mi->attach_down_pending, node, nnode,
			       transport)) {
		list_delete_node(mi->attach_down_pending, node);
		XFREE(MTYPE_MIDR_ATTACH_DOWN, transport);
	}
	for (ALL_LIST_ELEMENTS(mi->session_down_pending, node, nnode, down)) {
		list_delete_node(mi->session_down_pending, node);
		XFREE(MTYPE_MIDR_SESSION_DOWN, down);
	}
}

static void midr_nds_transport_clear_measurements(struct bgp_midr_nds *mi)
{
	struct listnode *node, *nnode;
	struct midr_link_entry *link;

	for (ALL_LIST_ELEMENTS(mi->global_view->links, node, nnode, link)) {
		list_delete_node(mi->global_view->links, node);
		XFREE(MTYPE_MIDR_LINK_ENTRY, link);
	}
}

/* Reconcile configured transport with runtime state as one fail-closed
 * transaction.  Ledger/config intent survives; old sockets, async requests,
 * probes, measurements and NDS-owned peers do not. */
int midr_nds_transport_reconcile(struct bgp *bgp)
{
	struct bgp_midr_nds *mi;
	struct prefix self_id;
	struct peer **doomed = NULL;
	struct peer *peer;
	struct midr_node_entry *view_entry;
	struct listnode *node;
	struct midr_session_ledger_entry *ledger;
	struct ipaddr desired = midr_ipaddr_none();
	bool want_transport, desired_unique = true, restart_join;
	size_t peer_cap, peer_count = 0;
	int ret = 0;

	if (!bgp || !bgp->midr_nds_info)
		return -EINVAL;
	mi = bgp->midr_nds_info;
	want_transport = mi->transport_addr_set;
	if (want_transport) {
		desired = mi->local_transport_addr;
		if (!midr_ipaddr_valid_locator(&desired))
			return -EINVAL;
		midr_prefix_from_in_addr(&self_id, bgp->router_id);
		desired_unique = midr_nds_locator_unique(bgp, &self_id,
						   &desired);
	}

	if ((!want_transport && !mi->transport_active) ||
	    (want_transport && desired_unique && mi->transport_active &&
	     midr_ipaddr_same(&desired, &mi->active_transport_addr))) {
		midr_nds_manual_sessions_restore(bgp);
		return 0;
	}

	restart_join = mi->join_intent || mi->join_in_progress ||
		       mi->join_phase != MIDR_JOIN_IDLE;
	mi->transport_reconfiguring = true;

	/* Withdraw endpoints while the old active locator is still available. */
	midr_nds_facts_withdraw_all_links(bgp);
	midr_pm_finish(bgp);
	midr_ctrl_close(bgp);
	midr_nds_transport_clear_deferred(mi);
	event_cancel(&mi->t_rep_probe_done);
	event_cancel(&mi->t_member_probe_done);
	midr_nds_anchor_ctx_clear(bgp);
	midr_rep_dir_clear(bgp);
	mi->join_phase = MIDR_JOIN_IDLE;
	mi->join_in_progress = false;
	mi->join_group_id = 0;
	mi->bootstrap_cur = NULL;
	mi->bootstrap_probe_cur = NULL;

	/* Deleting a peer mutates bgp->peer, so snapshot pointers first. */
	peer_cap = listcount(bgp->peer);
	if (peer_cap)
		doomed = XCALLOC(MTYPE_TMP, peer_cap * sizeof(*doomed));
	for (ALL_LIST_ELEMENTS_RO(bgp->peer, node, peer))
		if (midr_nds_peer_is_overlay(peer))
			doomed[peer_count++] = peer;
	for (size_t i = 0; i < peer_count; i++)
		peer_delete(doomed[i]);
	XFREE(MTYPE_TMP, doomed);
	/* is_adjacent describes a live session/probe relationship, not durable
	 * membership intent.  Every such relationship belonged to the locator
	 * generation just torn down; compatible SAME_GROUP ledgers set it again
	 * through midr_ctrl_connect() below. */
	frr_each (midr_node_hash, &mi->global_view->nodes, view_entry)
		view_entry->is_adjacent = false;
	midr_nds_transport_clear_measurements(mi);

	mi->transport_active = false;
	SET_IPADDR_NONE(&mi->active_transport_addr);

	if (want_transport && desired_unique && midr_ctrl_open(bgp, &desired)) {
		mi->active_transport_addr = desired;
		mi->transport_active = true;
	} else if (want_transport) {
		if (!desired_unique) {
			zlog_err("MIDR transport: configured locator %pIA belongs to another node; runtime remains disabled",
				 &desired);
			ret = -EADDRINUSE;
		} else {
			zlog_err("MIDR transport: configured locator %pIA could not become active; runtime remains disabled",
				 &desired);
			ret = -EADDRNOTAVAIL;
		}
	}

	/* PM owns its socket implementation.  Reinitialising through its public
	 * lifecycle lets the PM branch bind the same newly active family. */
	midr_pm_init(bgp);
	midr_nds_local_node_update(bgp);
	midr_nds_report_node(bgp, MIDR_ORIGIN_TRANSPORT_UPDATE);

	mi->transport_reconfiguring = false;
	if (mi->transport_active) {
		/* Restore durable edge intent without carrying old peer objects or
		 * measurements across the locator generation. */
		for (ALL_LIST_ELEMENTS_RO(mi->session_ledger, node, ledger)) {
			struct midr_node_entry target = {};
			bool send_nudge;

			if (!midr_ipaddr_valid_locator(&ledger->transport) ||
			    ipaddr_family(&ledger->transport) !=
				    ipaddr_family(&mi->active_transport_addr))
				continue;
			target.node_id.family = AF_INET;
			target.node_id.prefixlen = IPV4_MAX_BITLEN;
			target.node_id.u.prefix4 = ledger->remote_rid;
			target.transport_addr = ledger->transport;
			target.has_transport_addr = true;
			target.asn = ledger->remote_asn;
			target.group_id = ledger->remote_group;
			ledger->down_since = 0;
			send_nudge = ledger->reason != MIDR_SESSION_MANUAL &&
				      ledger->reason != MIDR_SESSION_PEER_REQ_REPLY;
			midr_ctrl_connect(bgp, &target, ledger->reason,
					  send_nudge);
		}
		midr_nds_manual_sessions_restore(bgp);
		midr_pm_on_transport_addr_set(bgp);
		if (restart_join && !list_isempty(mi->bootstrap_list))
			midr_join_round_start(bgp);
		/* close() also cancelled any bootstrap-list fetch before it could
		 * create an ATTACH ledger.  Replay alone cannot recover that work;
		 * resume the representative's existing attachment policy as well. */
		if (!mi->shutdown)
			midr_nds_attach_ensure(bgp, "transport activated");
	}

	return ret;
}

int midr_nds_transport_configure(struct bgp *bgp,
				 const struct ipaddr *transport)
{
	struct bgp_midr_nds *mi;
	struct listnode *node;
	struct midr_bootstrap_entry *bootstrap;
	struct midr_manual_session *session;

	if (!bgp || !bgp->midr_nds_info)
		return -EINVAL;
	if (transport && !midr_ipaddr_valid_locator(transport))
		return -EINVAL;
	mi = bgp->midr_nds_info;
	if (transport) {
		/* Keep the public setter fail-closed even outside the VTY path. */
		int family = ipaddr_family(transport);

		if ((mi->transport_addr_set &&
		     family != ipaddr_family(&mi->local_transport_addr)) ||
		    (mi->transport_active &&
		     family != ipaddr_family(&mi->active_transport_addr)))
			return -EAFNOSUPPORT;
		for (ALL_LIST_ELEMENTS_RO(mi->bootstrap_list, node, bootstrap))
			if (family != ipaddr_family(&bootstrap->transport))
				return -EAFNOSUPPORT;
		for (ALL_LIST_ELEMENTS_RO(mi->manual_sessions, node, session))
			if (family != ipaddr_family(&session->transport))
				return -EAFNOSUPPORT;
		mi->local_transport_addr = *transport;
		mi->transport_addr_set = true;
	} else {
		SET_IPADDR_NONE(&mi->local_transport_addr);
		mi->transport_addr_set = false;
	}

	/* During frr.conf loading, later lines may still change dependent state. */
	if (bgp_config_inprocess())
		return 0;
	return midr_nds_transport_reconcile(bgp);
}

static int midr_nds_transport_after_cfg(struct bgp *bgp)
{
	(void)midr_nds_transport_reconcile(bgp);
	return 0;
}

void bgp_midr_nds_init(struct bgp *bgp)
{
	struct bgp_midr_nds *mi;
	static bool hooks_registered;

	if (bgp->inst_type != BGP_INSTANCE_TYPE_DEFAULT)
		return;

	/* 这几个 hook 都是**进程级**的（不随实例），只注册一次：
	 *   peer_status_changed → 钩子 (b) 挂靠会话掉线；
	 *   bgp_config_end      → D1 配置读完后的挂靠补拉；
	 *                       → 引导专职化收尾（清群代表位 / 群号）；
	 *                       → 配置期被抑制的 node 上报补一笔（**必须排在引导
	 *                         收尾之后**，否则报出的身份马上作废）；
	 *   bgp_routerid_update → rid 就绪后重放一笔（守卫要求 rid 非 0）。 */
	if (!hooks_registered) {
		hook_register(peer_status_changed, midr_nds_peer_status_hook);
		hook_register(bgp_config_end, midr_nds_transport_after_cfg);
		hook_register(bgp_config_end, midr_nds_attach_after_cfg);
		hook_register(bgp_config_end, midr_nds_bootstrap_after_cfg);
		hook_register(bgp_config_end, midr_nds_remote_view_after_cfg);
		hook_register(bgp_config_end, midr_nds_facts_report_after_cfg);
		hook_register(bgp_routerid_update,
			      midr_nds_facts_report_after_rid);
		hooks_registered = true;
	}

	mi = XCALLOC(MTYPE_BGP_MIDR, sizeof(*mi));
	mi->bgp = bgp;
	mi->global_view = midr_global_view_new();
	mi->probe_contexts = NULL; /* PM: created on demand (skeleton) */
	mi->local_group_id = 0;
	mi->local_capabilities = 0;
	mi->rep_dir = list_new();
	mi->bootstrap_list = list_new(); /* §8.32 候选引导节点清单 */
	mi->session_blacklist = list_new(); /* 会话排除名单 */
	mi->session_ledger = list_new();    /* 会话台账（结论 20） */
	mi->manual_sessions = list_new();   /* `midr session` 持久配置意图 */
	mi->attach_down_pending = list_new(); /* 钩子 (b) 待处理掉线（D4） */
	mi->session_down_pending = list_new(); /* 件④ 掉沿清账待办 */
	mi->remote_withdrawn = list_new();    /* 件③ 虚报观察探针 */
	mi->perf_seqno = 0;
	mi->cap_seqno = 0;
	SET_IPADDR_NONE(&mi->local_transport_addr);
	SET_IPADDR_NONE(&mi->active_transport_addr);

	bgp->midr_nds_info = mi;

	/* 对接第二组 topology 接口：取一次 context 句柄存下（Q7 透传约定），
	 * 再建本地事实表（轮 1 起由 midr_nds_report_node 写入并上报）。 */
	mi->g2_ctx = midr_context_get_default();
	midr_nds_facts_init(bgp);

	/* 件③：注册 remote-view 回调（节点表的权威远端数据源）。
	 * 排在 facts_init 之后、各子模块之前——回调一旦注册就可能立刻被叫。 */
	midr_nds_remote_view_register(bgp);

	/* CL registers its global-view callback */
	midr_cl_init(bgp);

	/* Open the peer-request UDP control channel (bidirectional build-up) */
	midr_ctrl_init(bgp);

	/* PM arms its periodic probe-of-connected-nodes timer (I-1 loop) */
	midr_pm_init(bgp);

	/* Arm the CL periodic-sync timer */
	event_add_timer(bm->master, midr_periodic_sync_timer, bgp,
			MIDR_PERIODIC_SYNC_INTERVAL, &mi->t_periodic_sync);

	/* §8.31：建种子表（失败仅 warn、种子功能静默降级，主链路照跑）；读回
	 * 种子进候选清单，并挂一次性自举定时器（守卫见 self_boot_cb）。 */
	if (midr_store_init() == 0) {
		midr_store_seed_load(midr_bootstrap_seed_load_cb, bgp);
		event_add_timer(bm->master, midr_bootstrap_self_boot_cb, bgp,
				MIDR_BOOTSTRAP_SELF_BOOT_SECS,
				&mi->t_bootstrap_boot);
	}

	MIDR_LOG("MIDR: module initialized for instance %s",
		  bgp->name_pretty);
}

void bgp_midr_nds_finish(struct bgp *bgp)
{
	struct bgp_midr_nds *mi;

	if (!bgp || !bgp->midr_nds_info)
		return;

	mi = bgp->midr_nds_info;

	event_cancel(&mi->t_periodic_sync);
	event_cancel(&mi->t_probe_timeout);
	event_cancel(&mi->t_rep_probe_done);
	event_cancel(&mi->t_member_probe_done);
	event_cancel(&mi->t_anchor_probe_done);
	event_cancel(&mi->t_shutdown_teardown);
	event_cancel(&mi->t_bootstrap_boot);
	event_cancel(&mi->t_attach_reap);
	event_cancel(&mi->t_session_reap);

	/* Stop PM periodic probe timer */
	midr_pm_finish(bgp);

	/* Close the peer-request UDP control channel */
	midr_ctrl_finish(bgp);

	/* 对接第二组的本地事实表 */
	midr_nds_facts_finish(bgp);

	midr_global_view_free(mi->global_view);
	if (mi->rep_dir) {
		midr_rep_dir_clear(bgp);
		list_delete(&mi->rep_dir);
	}
	if (mi->bootstrap_list) { /* §8.32 候选清单（条目归本文件 MTYPE） */
		struct listnode *node, *nnode;
		struct midr_bootstrap_entry *b;

		for (ALL_LIST_ELEMENTS(mi->bootstrap_list, node, nnode, b))
			XFREE(MTYPE_MIDR_BOOTSTRAP_ENTRY, b);
		list_delete(&mi->bootstrap_list);
		mi->bootstrap_cur = NULL;
	}
	if (mi->session_blacklist) { /* 排除名单 */
		struct listnode *node, *nnode;
		struct in_addr *a;

		for (ALL_LIST_ELEMENTS(mi->session_blacklist, node, nnode, a))
			XFREE(MTYPE_MIDR_SESSION_EXCLUDE, a);
		list_delete(&mi->session_blacklist);
	}
	if (mi->session_ledger) { /* 会话台账：实例销毁 = 全部会话消失，账清空 */
		struct listnode *node, *nnode;
		struct midr_session_ledger_entry *e;

		for (ALL_LIST_ELEMENTS(mi->session_ledger, node, nnode, e))
			XFREE(MTYPE_MIDR_SESSION_LEDGER, e);
		list_delete(&mi->session_ledger);
	}
	if (mi->manual_sessions) {
		struct listnode *node, *nnode;
		struct midr_manual_session *session;

		for (ALL_LIST_ELEMENTS(mi->manual_sessions, node, nnode, session))
			XFREE(MTYPE_MIDR_MANUAL_SESSION, session);
		list_delete(&mi->manual_sessions);
	}
	if (mi->attach_down_pending) { /* 钩子 (b) 待处理掉线（D4） */
		struct listnode *node, *nnode;
		struct ipaddr *a;

		for (ALL_LIST_ELEMENTS(mi->attach_down_pending, node, nnode, a))
			XFREE(MTYPE_MIDR_ATTACH_DOWN, a);
		list_delete(&mi->attach_down_pending);
	}
	if (mi->session_down_pending) { /* 件④ 掉沿清账待办 */
		struct listnode *node, *nnode;
		struct midr_session_down *sd;

		for (ALL_LIST_ELEMENTS(mi->session_down_pending, node, nnode,
				       sd))
			XFREE(MTYPE_MIDR_SESSION_DOWN, sd);
		list_delete(&mi->session_down_pending);
	}
	if (mi->remote_withdrawn) { /* 件③ 虚报观察探针 */
		struct listnode *node, *nnode;
		struct midr_remote_withdrawn *w;

		for (ALL_LIST_ELEMENTS(mi->remote_withdrawn, node, nnode, w))
			XFREE(MTYPE_BGP_MIDR, w);
		list_delete(&mi->remote_withdrawn);
	}

	XFREE(MTYPE_BGP_MIDR, mi);
	bgp->midr_nds_info = NULL;

	MIDR_LOG("MIDR: module terminated for instance %s",
		  bgp->name_pretty);
}
