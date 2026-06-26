/* ===== bgpd/bgp_neighbor_store.c ===== */

#include "zebra.h"

#include "bgp_neighbor_store.h"
#include "lib/db.h"
#include "log.h"

#ifdef HAVE_SQLITE3

/* frr_capability 表中本节点的 owner 标识 */
#define FRR_SELF_OWNER "self"

/*
 * 当前 schema 版本，每次改表结构时递增。
 * v1 → v2：移除所有"待定"字段（group_id、status、last_cap_seq、flags、
 *           as_number、capability.type），只保留有持久化价值的字段。
 */
#define BGP_NEIGHBOR_SCHEMA_VERSION 2

/* 模块初始化标志，避免重复建表 */
static int g_store_initialized = 0;

/* 节点自身信息的内存副本 */
static struct frr_self_info g_self_info;
static int g_self_initialized = 0;

/* ------------------------------------------------------------------ */
/* schema 版本管理                                                      */
/* ------------------------------------------------------------------ */

static int schema_get_version(void)
{
	struct sqlite3_stmt *ss;
	uint32_t version = 0;

	ss = db_prepare("SELECT version FROM frr_schema_version LIMIT 1;");
	if (!ss)
		return 0;

	if (db_run(ss) == SQLITE_ROW)
		db_loadf(ss, "%i", &version);

	db_finalize(&ss);
	return (int)version;
}

static int schema_set_version(int version)
{
	return db_execute(
		"DELETE FROM frr_schema_version;"
		"INSERT INTO frr_schema_version (version) VALUES (%d);",
		version);
}

/* ------------------------------------------------------------------ */
/* 建表                                                                 */
/* ------------------------------------------------------------------ */

static int create_tables(void)
{
	/* schema 版本表 */
	if (db_execute(
		    "CREATE TABLE IF NOT EXISTS frr_schema_version ("
		    "    version  INTEGER NOT NULL"
		    ");") != 0)
		return -1;

	/*
	 * 节点自身信息，全局只有一行。
	 * 不存 as_number（已在 frr.conf）、group_id、flags（语义未定义）。
	 */
	if (db_execute(
		    "CREATE TABLE IF NOT EXISTS frr_self ("
		    "    ip        TEXT NOT NULL,"
		    "    max_peers INTEGER"
		    ");") != 0)
		return -1;

	/*
	 * 邻居列表，frr_link_perf 字段内联存储。
	 * 不存 group_id、status（运行时 FSM 负责）、last_cap_seq（运行时序号）。
	 * last_seen 保留：记录历史上最后一次与该邻居通信的时间，用于可用性分析。
	 */
	if (db_execute(
		    "CREATE TABLE IF NOT EXISTS frr_peer ("
		    "    peer_ip        TEXT PRIMARY KEY,"
		    "    peer_as        INTEGER,"
		    "    last_seen      INTEGER,"
		    "    latency_ms     REAL,"
		    "    bandwidth_mbps REAL,"
		    "    loss_rate      REAL,"
		    "    measured_at    INTEGER"
		    ");") != 0)
		return -1;

	/*
	 * 能力列表，self 和 peer 共用。
	 * owner = peer_ip 或 FRR_SELF_OWNER("self")。
	 * 不存 type（静态/动态区分无持久化意义）。
	 */
	if (db_execute(
		    "CREATE TABLE IF NOT EXISTS frr_capability ("
		    "    owner  TEXT NOT NULL,"
		    "    name   TEXT NOT NULL,"
		    "    value  TEXT,"
		    "    PRIMARY KEY (owner, name)"
		    ");") != 0)
		return -1;

	return 0;
}

/* ------------------------------------------------------------------ */
/* schema 迁移                                                          */
/* ------------------------------------------------------------------ */

static int run_migrations(void)
{
	int version = schema_get_version();

	if (version == 0) {
		/* 全新数据库，表已由 create_tables() 按最新 schema 建好，直接写版本号 */
		schema_set_version(BGP_NEIGHBOR_SCHEMA_VERSION);
		return 0;
	}

	/*
	 * v1 → v2：移除了多个"待定"字段，SQLite 不支持 DROP COLUMN，
	 * 采用先删表再重建的方式完成迁移（旧数据丢弃，重启后由 BGP 重新填充）。
	 * 后续新增字段时在此追加新的 if 块，不要修改已有块。
	 */
	if (version < 2) {
		if (db_execute(
			    "DROP TABLE IF EXISTS frr_self;"
			    "DROP TABLE IF EXISTS frr_peer;"
			    "DROP TABLE IF EXISTS frr_capability;") != 0)
			return -1;

		if (create_tables() != 0)
			return -1;

		version = 2;
		schema_set_version(version);
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* 公开接口                                                             */
/* ------------------------------------------------------------------ */

/*
 * bgp_neighbor_store_init - 建表并执行迁移
 *
 * 数据库句柄由 libfrr 在启动时统一通过 db_init() 打开（bgpd.db），
 * 此函数只负责在其中建立本模块所需的表，已初始化则直接返回成功。
 * 返回 0 成功，-1 失败。
 */
int bgp_neighbor_store_init(void)
{
	if (g_store_initialized)
		return 0;

	if (create_tables() != 0) {
		zlog_warn("bgp_neighbor_store: failed to create tables");
		return -1;
	}

	if (run_migrations() != 0) {
		zlog_warn("bgp_neighbor_store: failed to run migrations");
		return -1;
	}

	g_store_initialized = 1;
	return 0;
}

/*
 * bgp_neighbor_store_init_self - 写入本节点自身信息
 *
 * 从 VTY 命令处理函数调用，此时 router-id 已经确定。
 * 同时更新数据库和内存副本。
 * as_number 不再存入数据库，运行时从 struct bgp 读取即可。
 * 返回 0 成功，-1 失败。
 */
int bgp_neighbor_store_init_self(const char *ip, int max_peers)
{
	struct sqlite3_stmt *ss;

	if (!g_store_initialized)
		return -1;

	/* 清除旧数据，保证 frr_self 只有一行 */
	if (db_execute("DELETE FROM frr_self;") != 0)
		return -1;

	ss = db_prepare(
		"INSERT INTO frr_self (ip, max_peers)"
		" VALUES (?, ?);");
	if (!ss)
		return -1;

	if (db_bindf(ss, "%s%i", ip, (int)strlen(ip), (uint32_t)max_peers) != 0) {
		db_finalize(&ss);
		return -1;
	}

	if (db_run(ss) != SQLITE_OK) {
		db_finalize(&ss);
		return -1;
	}

	db_finalize(&ss);

	/* 同步内存副本 */
	memset(&g_self_info, 0, sizeof(g_self_info));
	strlcpy(g_self_info.ip, ip, sizeof(g_self_info.ip));
	g_self_info.max_peers = max_peers;
	g_self_info.caps      = NULL;
	g_self_info.cap_count = 0;
	g_self_initialized    = 1;

	return 0;
}

/*
 * bgp_neighbor_store_get_self - 返回内存中的自身信息
 * 未初始化时返回 NULL。
 */
const struct frr_self_info *bgp_neighbor_store_get_self(void)
{
	return g_self_initialized ? &g_self_info : NULL;
}

/*
 * bgp_neighbor_store_close - 重置模块状态
 *
 * 数据库句柄由 libfrr 统一关闭，此处仅清除本模块的内存状态。
 */
void bgp_neighbor_store_close(void)
{
	g_store_initialized = 0;
	g_self_initialized  = 0;
}

#endif /* HAVE_SQLITE3 */
