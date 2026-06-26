/* ===== bgp_neighbor_store.h ===== */

#ifndef BGP_NEIGHBOR_STORE_H
#define BGP_NEIGHBOR_STORE_H

#include <time.h>
#include <stdint.h>

/*
 * 单条能力记录
 * 用于描述节点自身能力或邻居能力。
 * name/value 均用字符串存储，布尔值存 "true"/"false"，数值存对应字符串如 "10"。
 */
struct frr_capability {
	char name[64];   /* 能力名称，如 "SRv6" */
	char value[128]; /* 能力值 */
};

/*
 * 链路性能数据
 * 由测量组负责填充，跨重启持久化，是本模块存入数据库的核心数据。
 */
struct frr_link_perf {
	double latency_ms;     /* 时延，单位毫秒 */
	double bandwidth_mbps; /* 带宽，单位 Mbps */
	double loss_rate;      /* 丢包率，0.0~1.0 */
	time_t measured_at;    /* 上次测量的 Unix 时间戳 */
};

/*
 * 邻居节点元数据
 * 只保存需要跨重启持久化的数据：
 *   - peer_ip / peer_as：邻居标识，配合 frr.conf 使用
 *   - last_seen：上次与该邻居有通信的时间戳，用于历史记录和长期可用性分析
 *   - perf：链路性能测量结果
 *   - caps / cap_count：邻居能力缓存，避免每次重连都从零协商
 *
 * 注意：BGP session 的实时状态（up/down）由 FRR FSM 管理，不在此处记录。
 */
struct frr_peer_meta {
	char peer_ip[46];             /* 邻居 IP 地址 */
	uint32_t peer_as;             /* 邻居 AS 号 */
	time_t last_seen;             /* 上次收到邻居任意消息的 Unix 时间戳 */
	struct frr_link_perf perf;    /* 到该邻居的链路性能数据 */
	struct frr_capability *caps;  /* 该邻居的能力列表 */
	int cap_count;                /* 能力列表长度 */
};

/*
 * 节点自身信息
 * 全局唯一，数据库中只存一行。
 * as_number 已在 frr.conf 中，运行时从 struct bgp 读取，不在此重复存储。
 */
struct frr_self_info {
	char ip[46];                 /* 本节点 IP 地址（router-id） */
	int max_peers;               /* 允许建立的最大邻居数 */
	struct frr_capability *caps; /* 本节点能力列表 */
	int cap_count;               /* 能力列表长度 */
};

/*
 * 节点完整状态，整合自身信息与邻居列表。
 * 内存中的顶层数据结构，不直接对应数据库表，运行时使用。
 */
struct frr_node_store {
	struct frr_self_info self;   /* 节点自身信息 */
	struct frr_peer_meta *peers; /* 邻居列表数组 */
	int peer_count;              /* 当前邻居数量 */
};


/* 存储层初始化与释放 */
#ifdef HAVE_SQLITE3
int bgp_neighbor_store_init(void);
int bgp_neighbor_store_init_self(const char *ip, int max_peers);
const struct frr_self_info *bgp_neighbor_store_get_self(void);
void bgp_neighbor_store_close(void);
#else
#define bgp_neighbor_store_init()              (0)
#define bgp_neighbor_store_init_self(ip, maxp) (0)
#define bgp_neighbor_store_get_self()          (NULL)
#define bgp_neighbor_store_close()             do {} while (0)
#endif /* HAVE_SQLITE3 */

#endif /* BGP_NEIGHBOR_STORE_H */
