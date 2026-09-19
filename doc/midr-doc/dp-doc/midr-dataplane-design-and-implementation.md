# MIDR 数据平面设计与实现

> **版本**: v2.0  
> **定位**: 完整的数据平面设计规范 + 代码实现说明  
> **融合文档**: `midr-cp-dp-interface.md` + `traffic-engineering.md` + RFC 9815 + 已有代码修改

---

## 一、架构总览

### 1.1 三层架构

```
┌─────────────────────────────────────────────────────────────────────────┐
│                     BGPd (同一进程)                                      │
│                                                                         │
│  控制平面 (Control Plane)                                               │
│  ┌──────────────────────┐    ┌───────────────────────────┐             │
│  │    路由协议组        │    │     流量工程组 (TE)        │             │
│  │  • BGP-LS 收发       │    │  • TE 策略表               │             │
│  │  • TED/LSDB 维护     │    │  • CSPF 约束路径计算       │             │
│  │  • 标准 SPF 计算     │    │  • SRv6 SID 构造           │             │
│  └──────────┬───────────┘    └───────────┬───────────────┘             │
│             │                            │                              │
│             │ 标准 SPF (sid_count=0)     │ TE SRv6 (sid_count>0)       │
│             │                            │                              │
│             └────────────┬───────────────┘                              │
│                          ▼                                              │
│             ┌─────────────────────────────┐                            │
│             │  midr_path_result (统一载体)│                            │
│             └─────────────┬───────────────┘                            │
│                           │                                            │
│  数据平面 (Data Plane) ─────                                           │
│  ┌───────────────────────────────────────────────────────────────────┐ │
│  │  bp_midr_zebra.c                                                  │ │
│  │  • 模式识别 (BASIC / ECMP / UCMP / SRv6)                         │ │
│  │  • zapi_route + zapi_nexthop 填充                                 │ │
│  │  • 批量延迟聚合 (100ms) + diff                                    │ │
│  │  • TE dual-instance: instance=1/metric=1 vs instance=0            │ │
│  └───────────────────────────────┬─────────────────────────────────────┘ │
│                                  │                                       │
└──────────────────────────────────┼───────────────────────────────────────┘
                                   │ ZAPI
                                   ▼
                         ┌───────────────────┐
                         │       zebra       │  全复用,不修改
                         │  RIB→NHG→Netlink  │
                         └───────────────────┘
                                   │
                                   ▼
                          Linux Kernel FIB
```

### 1.2 模块依赖图

```
                     TED (ls_ted) — 只读访问
                          │
        ┌─────────────────┼─────────────────┐
        ▼                 ▼                 ▼
  标准 SPF            TE CSPF          策略匹配
  (sid_count=0)     (sid_count>0)    (修改路由属性)
        │                 │               │
        └────────┬────────┴───────┬───────┘
                 ▼                ▼
          midr_path_result (统一接口)
                 │
                 ▼
          midr_zebra_route_add()
                 │
                 ▼
        批量延迟聚合 (100ms)
                 │
                 ▼ ZAPI
              zebra
```

---

## 二、数据结构

### 2.1 CP → DP 统一载体

```c
/* bgpd/bgp_midr_zebra.h */

/* 单条路径 */
struct midr_path {
    union g_addr    nexthop;        /* 物理下一跳地址 */
    uint32_t        ifindex;        /* 出接口 (0 = zebra 解析) */
    uint32_t        metric;         /* 路径度量 */
    float           path_avail_bw;  /* 瓶颈可用带宽 (Mbps) */
    uint8_t         weight;         /* UCMP 权重 1-255; 0 = ECMP */
};

/* CP → DP 完整计算结果 */
struct midr_path_result {
    struct midr_path    *paths;
    uint8_t             path_count;     /* >1 = 多路径 */

    /* SRv6 段列表 */
    struct {
        struct in6_addr sid_list[SRV6_MAX_SEGS];
        uint8_t         sid_count;      /* 0 = 纯IP, >0 = SRv6 */
    } explicit;
};
```

**零值语义**:

| 字段 | 零值含义 | 非零含义 |
|------|---------|---------|
| `sid_count` | 纯 IP 路由 | SRv6 严格路径 |
| `weight` | 等权重 ECMP | UCMP 加权 |
| `path_count` | — | 1=单路径, >1=多路径 |

### 2.2 模式识别

| 模式 | 条件 | 行为 | 来源 |
|------|------|------|------|
| **BASIC** | `path_count=1, sid_count=0` | 标准单路径 | 标准 SPF |
| **ECMP** | `path_count>1, weight=0, sid_count=0` | 等权重多路径 | 标准 SPF |
| **UCMP** | `path_count>1, weight>0, sid_count=0` | 加权多路径 | SPF + 带宽权重 |
| **SRv6** | `sid_count>0` | SRH 封装, 严格路径 | CSPF (TE) |

### 2.3 TE 专用模式 (Dual-Instance)

TE 路由使用 zapi_route 的 `instance` 字段与 SPF 路由共存于 zebra RIB：

```
zebra RIB 中同一前缀 10.1.0.0/16:
  ├── re[0]: instance=0, type=ZEBRA_ROUTE_BGP_MIDR, metric=100  ← 标准 SPF
  └── re[1]: instance=1, type=ZEBRA_ROUTE_BGP_MIDR, metric=1    ← TE SRv6
                                              ↑
                                        metric 更优 → rib_choose_best() 选中
```

- **标准 SPF**: `instance=0`, `metric=IGP值`, 始终保留在 RIB
- **TE SRv6**: `instance=1`, `metric=1`, 覆盖 SPF; 删除后 SPF 自动回退

---

## 三、接口函数

### 3.1 路由下发 API

```c
/* bgpd/bgp_midr_zebra.h */

/* 安装/更新路由（暂存入 batch 队列, 不立即下发） */
void midr_zebra_route_add(struct bgp *bgp, struct prefix *p,
                           struct midr_path_result *result);

/* 撤销路由 */
void midr_zebra_route_del(struct bgp *bgp, struct prefix *p);

/* 触发 100ms 延迟批量下发（正常场景） */
void midr_zebra_route_update_deferred(struct bgp *bgp);

/* 立即 flush（紧急场景: 接口 down 等） */
void midr_zebra_route_flush(struct bgp *bgp);

/* 初始化/清理 per-BGP-instance DP 状态 */
void midr_zebra_init(struct bgp *bgp);
void midr_zebra_fini(struct bgp *bgp);
```

### 3.2 批量下发机制

```
midr_zebra_route_add/del()
  │
  ▼
pending_ops 链表 (暂存)
  │
  ▼
midr_zebra_route_update_deferred()
  │ 100ms 定时器聚合
  ▼
midr_flush_pending()
  │
  ├── Ops 优先级: DEL 先于 ADD
  ├── 与 installed hash 做 diff
  │   ├── 相同 → 跳过 (无操作)
  │   └── 不同 → DEL 旧 + ADD 新
  │
  └── zclient_route_send() → ZAPI → zebra
```

### 3.3 批量下发 vs 立即下发

| 函数 | 延迟 | 场景 |
|------|------|------|
| `midr_zebra_route_update_deferred()` | 100ms | 正常拓扑变化, 聚合批量 |
| `midr_zebra_route_flush()` | 0ms | 紧急故障, 立即生效 |

---

## 四、ZAPI 编码详细流程

### 4.1 入口函数

```c
void midr_zebra_route_add(struct bgp *bgp, struct prefix *p,
                          struct midr_path_result *result)
{
    // 1. 深拷贝 result → midr_pending_op
    // 2. 推入 pending_ops 链表
    // 3. 不发送任何 ZAPI 消息
}
```

### 4.2 ZAPI 填充函数 `midr_result_to_zapi()`

```c
static int midr_result_to_zapi(struct bgp *bgp, const struct prefix *p,
                               const struct midr_path *paths,
                               uint8_t path_count, uint8_t sid_count,
                               const struct in6_addr *sid_list,
                               struct zapi_route *api)
{
    zapi_route_init(api);
    api->vrf_id = bgp->vrf_id;
    api->type = ZEBRA_ROUTE_BGP_MIDR;          // 专用路由类型
    api->instance = bgp->instance;              // 0=SPF, 1=TE (见 TE 部分)
    api->safi = SAFI_UNICAST;
    api->prefix = *p;

    // Distance: 115 (与 IS-IS 相同, 保证比 BGP 200 优先)
    SET_FLAG(api->message, ZAPI_MESSAGE_DISTANCE);
    api->distance = ZEBRA_BGP_MIDR_DISTANCE_DEFAULT;

    // 填充 nexthops
    for (i = 0; i < path_count; i++) {
        struct zapi_nexthop *znh = &api->nexthops[i];
        midr_path_fill_zapi_nh(p, &paths[i], znh, bgp->vrf_id);

        // UCMP: 有 weight 时设置 flag
        if (paths[i].weight > 0) {
            znh->weight = paths[i].weight;
            SET_FLAG(znh->flags, ZAPI_NEXTHOP_FLAG_WEIGHT);
        }
    }

    // SRv6: 仅对第一个 nexthop 设置 SID 列表
    if (sid_count > 0) {
        SET_FLAG(api->nexthops[0].flags, ZAPI_NEXTHOP_FLAG_SEG6);
        api->nexthops[0].seg_num = sid_count;
        memcpy(api->nexthops[0].seg6_segs, sid_list, ...);
        api->nexthops[0].srv6_encap_behavior = SRV6_HEADEND_BEHAVIOR_H_INSERT;
    }
}
```

### 4.3 内核效果对比

```
BASIC (单路径):
  ip route add 10.0.0.0/24 via 2001:db8:a::1

ECMP (等权重):
  ip route add 10.0.0.0/24 \
    nexthop via 2001:db8:a::1 \
    nexthop via 2001:db8:b::1

UCMP (加权):
  ip route add 10.0.0.0/24 \
    nexthop via 2001:db8:a::1 weight 204 \
    nexthop via 2001:db8:b::1 weight 51

SRv6 (严格路径):
  ip -6 route add 10.1.0.0/16 \
    encap seg6 mode inline \
    segs [2001:db8:1::1,2001:db8:2::1,2001:db8:3::1] \
    via 2001:db8:a::1
```

---

## 五、流量工程 (TE) 子系统

### 5.1 TE 策略类型

| 策略 | 触发 | 回退 | Stage |
|------|------|------|-------|
| `ADMIN_STEER` (QoS 保障) | 管理员配置 | 管理员删除策略时 | Phase 1 |
| `SRV6_MIGRATE` (拥塞避免) | 链路利用率 > 阈值 | 拥塞解除自动回退 | Phase 2 |

### 5.2 TE 策略表

```c
/* bgpd/bgp_midr_te.h */

struct midr_te_policy {
    uint32_t            id;
    struct prefix       prefix;         // 目标前缀
    uint8_t             type;           // ADMIN_STEER / SRV6_MIGRATE
    uint32_t            delay_us;       // 最大延迟 (us)
    float               min_bw_mbps;    // 最小带宽 (Mbps)
    float               max_loss_ppm;   // 最大丢包率 (ppm)
    uint32_t            admin_group;    // 亲和力位图
    uint32_t            admin_mask;     // 亲和力掩码
    uint32_t            exclude_srlgs[8];
    uint8_t             srlg_count;
    uint8_t             utilization_pct; // Phase 2 触发阈值
    bool                installed;      // 是否已生效
};
```

### 5.3 CSPF 算法

```
CSPF 流程:
  1. 剪枝: 遍历 TED 所有链路, 排除:
     - 延迟 > policy->delay_us
     - 可用带宽 < policy->min_bw_mbps
     - 丢包率 > policy->max_loss_ppm
     - 亲和力不匹配
     - SRLG 冲突
     - (Phase 2) 利用率超阈值
  2. K-SPF: 在剪枝子图上运行 Yen 算法
  3. SID 构造: 对选中的路径逐跳查 BGP-LS 发布的 SID
     - End.X (Adjacency SID) 优先
     - End (Node SID) 备选
     - End.DT46 (解封装) 末尾
```

### 5.4 Dual-Instance 回退机制

```
初始状态:
  zebra RIB: prefix 10.1.0.0/16
    ├── instance=0, metric=100, nexthop=2001:db8:a::1    ← SPF
    └── [无 TE]

管理员配置 TE 策略:
  zebra RIB: prefix 10.1.0.0/16
    ├── instance=0, metric=100, nexthop=2001:db8:a::1    ← SPF (保留)
    └── instance=1, metric=1, nexthop + SRv6 SID List   ← TE (新增, 当选)
                                    ↑
                          metric=1 < 100 → rib_choose_best() 选中 TE
                          内核 FIB 生效: SRv6 封装路径

管理员删除 TE 策略 / 拥塞解除:
  TE 模块调用 midr_zebra_route_del(bgp, &prefix)
    → zapi_route.instance = 1 → zebra 精确删除 TE 条目
    → SPF 条目 (instance=0) 重新成为 best
    → zebra 自动下发 SPF 路由到内核
    → 无需 TE 模块重新计算或推送 SPF 路由
```

---

## 六、zebra 侧修改摘要

### 6.1 新增路由类型

| 文件 | 修改 | 说明 |
|------|------|------|
| `lib/route_types.txt` | +`ZEBRA_ROUTE_BGP_MIDR` | 路由类型定义 |
| `lib/frrdistance.h` | +`ZEBRA_BGP_MIDR_DISTANCE_DEFAULT 115` | 管理距离 (同 IS-IS) |
| `zebra/zebra_rib.c` | +`ZEBRA_ROUTE_BGP_MIDR` 条目 | RIB 元数据注册 |

### 6.2 Netlink 协议号

| 文件 | 修改 | 说明 |
|------|------|------|
| `zebra/rt_netlink.h` | +`#define RTPROT_BGP_MIDR 199` | 内核协议号 |
| `zebra/rt_netlink.c` | +`zebra2proto()` / `proto2zebra()` | 双向转换 |
| `zebra/rt_netlink.c` | +`is_selfroute()` 检查 | 识别为 FRR 自有路由 |
| `zebra/kernel_netlink.c` | +`RTPROT_BGP_MIDR` | 协议名映射 |
| `zebra/debug_nl.c` | +`RTPROT_BGP_MIDR` | 调试输出 |
| `zebra/fpm_listener.c` | +`RTPROT_BGP_MIDR` | FPM 导出 |
| `tools/etc/iproute2/rt_protos.d/frr.conf` | +`199  midr` | iproute2 协议名 |

---

## 七、代码文件清单

| 优先级 | 文件 | 说明 | 状态 |
|--------|------|------|------|
| P0 | `bgpd/bgp_midr_zebra.h` | CP-DP 接口头文件 | ✅ 已完成 |
| P0 | `bgpd/bgp_midr_zebra.c` | ZAPI 编码 + 批量下发 | ✅ 已完成 |
| P0 | `bgpd/bgpd.h` | `midr_dp` 字段 | ✅ 已修改 |
| P0 | `bgpd/subdir.am` | 构建系统 | ✅ 已修改 |
| P1 | `bgpd/bgp_midr_te.h` | TE 策略表头文件 | 🆕 待添加 |
| P1 | `bgpd/bgp_midr_te.c` | TE 策略表实现 | 🆕 待添加 |
| P1 | `bgpd/bgp_midr_cspf.h` | CSPF 头文件 | 🆕 待添加 |
| P1 | `bgpd/bgp_midr_cspf.c` | CSPF 约束路径计算 | 🆕 待添加 |

---

## 八、性能特征

| 操作 | 复杂度 | 典型耗时 |
|------|--------|---------|
| ZAPI 编码 | O(path_count × seg_count) | <1ms |
| 批量 diff | O(pending_ops × hash_lookup) | <1ms |
| 100ms 批量窗口 | — | 0~100ms |
| zebra → Netlink | — | <1ms |
| CSPF (100 节点) | O((V+E)logV + K×V³) | ~50ms |
| 策略匹配 | O(radix_lookup) | <1μs |

---

## 九、总结

| 层级 | 模块 | 职责 | 输出 |
|------|------|------|------|
| CP 路由 | `bgp_midr_spf.c` | 标准 SPF 默认路由 | `midr_path_result(sid_count=0)` |
| CP TE | `bgp_midr_te.c` + `bgp_midr_cspf.c` | QoS 保障 + 拥塞避免 | `midr_path_result(sid_count>0)` |
| DP | `bgp_midr_zebra.c` | 模式识别 → ZAPI → 批量 | `zclient_route_send()` |
| zebra | RIB + NHG + Netlink | 路由管理 + 内核下发 | 内核 FIB |

**关键设计决策**:
1. **CP-DP 统一接口**: `midr_path_result` 是唯一载体, SPF 和 TE 均通过它下发
2. **Dual-Instance**: SPF(instance=0) 与 TE(instance=1) 共存于 RIB, 通过 metric 择优
3. **自动回退**: 删除 TE 路由后 SPF 自动恢复, 无需显式操作
4. **零修改 zebra**: 全部通过标准 ZAPI + route type 区分实现
5. **批量防抖**: 100ms 窗口 + diff 减少内核操作次数