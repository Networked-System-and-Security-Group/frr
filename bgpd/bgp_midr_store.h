// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR 持久化层（SQLite）——§8.31 bootstrap 种子。
 *
 * 复用 libfrr 启动时统一 db_init() 打开的 bgpd.db，本模块只在其中建自己的表。
 * 目前只存 bootstrap 种子：学到的引导节点地址落盘，节点重启后读回当"第一跳
 * 尝试清单"（种子），配 §8.32 failover 自举重入。种子只作尝试清单、不当真相
 * ——失败即换下一条，绝不灌进运行目录。
 *
 * 前身 bgp_neighbor_store.{c,h}（队友早期 self/peer/capability 三件套，从未
 * 接线、用途已被 BGP-LS 泛洪自然取代）2026-07-20 删死代码 + 改名而来。
 */
#ifndef BGP_MIDR_STORE_H
#define BGP_MIDR_STORE_H

#include <stdint.h>
#include <time.h>

/* 种子读回回调：每条一次；transport 为点分 IPv4 字符串（借用 SQLite 列内存，
 * 仅在回调内有效——调用方需立即用掉/拷走，不得留存指针）。 */
typedef void (*midr_seed_cb)(const char *transport, uint32_t asn, void *arg);

#ifdef HAVE_SQLITE3
/* 建表（幂等）；返回 0 成功、-1 失败（失败时上层静默降级、种子功能不可用）。 */
int midr_store_init(void);
/* 写入/刷新一条种子（有则更新 asn+last_seen、无则插入）。 */
void midr_store_seed_save(const char *transport, uint32_t asn, time_t now);
/* 按 last_seen 新→旧逐条回调吐出全部种子。 */
void midr_store_seed_load(midr_seed_cb cb, void *arg);
/* 只保留最新 keep_n 条种子，删掉其余（防库无限膨胀）。 */
void midr_store_seed_prune(int keep_n);
#else
#define midr_store_init()	     (-1)
#define midr_store_seed_save(t, a, n) do {                                     \
	} while (0)
#define midr_store_seed_load(cb, arg) do {                                     \
	} while (0)
#define midr_store_seed_prune(k) do {                                          \
	} while (0)
#endif /* HAVE_SQLITE3 */

#endif /* BGP_MIDR_STORE_H */
