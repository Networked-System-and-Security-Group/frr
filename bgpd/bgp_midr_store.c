// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR 持久化层实现（§8.31 bootstrap 种子）。
 *
 * 数据库句柄由 libfrr 在启动时统一 db_init() 打开（bgpd.db），本模块只负责在
 * 其中建自己的表、读写自己的行；开库/关库不归本模块管。所有 SQLite 访问经
 * lib/db.h 的 db_prepare/db_bindf/db_run/db_loadf/db_execute 封装。
 */
#include "zebra.h"

#include "log.h"
#include "lib/db.h"

#include "bgpd/bgp_midr_store.h"

#ifdef HAVE_SQLITE3

/* 建表标志，避免重复建表；建表失败则保持 0，后续读写全部短路（静默降级）。 */
static int g_midr_store_initialized;

int midr_store_init(void)
{
	if (g_midr_store_initialized)
		return 0;

	/*
	 * rid 列 = 引导节点的 router-id（保底轮 2 批 5 R 系列）。
	 * **不写迁移代码**（定案）：本项目尚未离开 lab，换新二进制时按部署纪律
	 * 各删一次 bgpd.db 即可；旧库（无 rid 列）读回时 SELECT 会失败、走静默
	 * 降级，不会崩。真实部署前须补正式迁移——已记进 5a 批次记录。
	 */
	if (db_execute("CREATE TABLE IF NOT EXISTS midr_bootstrap_seed ("
		       "    transport TEXT PRIMARY KEY,"
		       "    asn       INTEGER NOT NULL,"
		       "    rid       TEXT NOT NULL,"
		       "    last_seen INTEGER NOT NULL"
		       ");") != 0) {
		zlog_err("MIDR store: failed to create midr_bootstrap_seed table");
		return -1;
	}

	g_midr_store_initialized = 1;
	return 0;
}

void midr_store_seed_save(const char *transport, uint32_t asn, const char *rid,
			  time_t now)
{
	struct sqlite3_stmt *ss;

	if (!g_midr_store_initialized || !transport || !rid)
		return;

	/* 有则更新、无则插入（SQLite UPSERT，与第二组 node_upsert 无关，纯撞名）。 */
	ss = db_prepare(
		"INSERT INTO midr_bootstrap_seed (transport, asn, rid, last_seen)"
		" VALUES (?, ?, ?, ?)"
		" ON CONFLICT(transport) DO UPDATE SET"
		"   asn = excluded.asn, rid = excluded.rid,"
		"   last_seen = excluded.last_seen;");
	if (!ss)
		return;

	if (db_bindf(ss, "%s%i%s%d", transport, (int)strlen(transport), asn,
		     rid, (int)strlen(rid), (uint64_t)now) != 0) {
		db_finalize(&ss);
		return;
	}

	if (db_run(ss) != SQLITE_OK)
		zlog_warn("MIDR store: seed save for %s failed", transport);

	db_finalize(&ss);
}

void midr_store_seed_load(midr_seed_cb cb, void *arg)
{
	struct sqlite3_stmt *ss;

	if (!g_midr_store_initialized || !cb)
		return;

	ss = db_prepare("SELECT transport, asn, rid, last_seen FROM midr_bootstrap_seed"
			" ORDER BY last_seen DESC;");
	if (!ss)
		return;

	while (db_run(ss) == SQLITE_ROW) {
		const char *transport = NULL;
		const char *rid = NULL;
		uint32_t asn = 0;
		uint64_t last_seen = 0; /* %d 取 int64（db_loadf 的约定，非 printf） */

		/* %s 借用 SQLite 列内存（到下次 db_run/finalize 失效），立即用掉。 */
		db_loadf(ss, "%s%i%s%d", &transport, &asn, &rid, &last_seen);
		if (transport && rid)
			cb(transport, asn, rid, (time_t)last_seen, arg);
	}

	db_finalize(&ss);
}

void midr_store_seed_prune(int keep_n)
{
	if (!g_midr_store_initialized || keep_n < 0)
		return;

	/* 只留 last_seen 最新的 keep_n 条，其余删除。 */
	db_execute("DELETE FROM midr_bootstrap_seed WHERE transport NOT IN ("
		   "    SELECT transport FROM midr_bootstrap_seed"
		   "    ORDER BY last_seen DESC LIMIT %d);",
		   keep_n);
}

#endif /* HAVE_SQLITE3 */
