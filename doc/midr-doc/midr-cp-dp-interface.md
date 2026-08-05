# MIDR 控制平面-数据平面路由安装接口

## 一、接口定位

这是控制平面将计算结果交给数据平面的**唯一正式通道**，**单向**（CP → DP）。

```
控制平面 (bgpd)          接口层              数据平面 (bgpd)
┌─────────────┐      ┌──────────────┐      ┌──────────────┐
│ SPF/CSPF    │─────►│ midr_spf_    │─────►│ midr_zebra_  │
│ 计算引擎    │      │ result       │      │ route_add()  │
│             │      │              │      │              │
└─────────────┘      └──────────────┘      └──────────────┘
                                              │
                                              ▼ ZAPI
                                           zebra
```

**设计原则**：
- CP 输出路径计算原始结果（nexthop、weight、SID list）
- CP 不感知 DP 的下发细节：zapi 编码、批量下发、防抖聚合
- DP 根据 `sid_count` 选择内核路由格式：`sid_count=0` → 纯 IP（BASIC/UCMP），`sid_count>0` → SRv6
- DP 批量下发：100ms 窗口聚合多条路由变更，diff 后原子安装

---

## 二、模块调用路径

```
控制平面 (bgpd)
    │
    ├── 标准 SPF ──► 纯 IP + ECMP/UCMP
    │   (bgp_midr_spf.c)
    │
    └── TE 策略 + CSPF ──► SRv6 严格路径
        (bgp_midr_te.c)
    │
    ▼ midr_zebra_route_add()
        (bgp_midr_zebra.c)
数据平面 (bgpd)
    │
    ├── 模式识别 (BASIC / UCMP / SRV6)
    │
    ├── 填充 zapi_route + zapi_nexthop
    │
    └── zclient_route_send() ──► zebra
```

---

## 三、支持的能力

统一接口 `midr_zebra_route_add()` 通过同一数据结构支持四种模式：

| 模式 | 条件 | 数据平面行为 | 计算来源 | 典型场景 |
|------|------|-------------|---------|---------|
| **BASIC** | `path_count=1, sid_count=0` | 标准单路径路由 | 标准 SPF | 唯一最短路径 |
| **ECMP** | `path_count>1, weight=0, sid_count=0` | 等权重多路径 | 标准 SPF | 多条等 cost 路径 |
| **UCMP** | `path_count>1, weight>0, sid_count=0` | 按 weight 比例加权分发 | 标准 SPF + 带宽感知权重 | 多条等 cost 路径，按可用带宽比例分配权重 |
| **SRv6** | `sid_count>0` | SRH 封装，逐跳严格路径 | CSPF（策略约束） | 链路拥塞迁移、跨域低延迟路径、合规路径 |

---

## 四、数据结构

```c
/* ---------- 单条路径 ---------- */
struct midr_path {
    union g_addr        nexthop;        /* 物理下一跳地址 */
    uint32_t            ifindex;        /* 本地出接口（0 = 由 zebra 解析） */
    uint32_t            metric;         /* 路径度量值 */
    float               path_avail_bw;  /* 瓶颈可用带宽（Mbps），UCMP 权重源 */
    uint8_t             weight;         /* UCMP 权重 1-255；0 = 等权重 ECMP */
};

/* ---------- 计算结果（CP → DP 唯一数据载体）---------- */
struct midr_path_result {
    struct midr_path    *paths;
    uint8_t                  path_count;    /* 1=单路径, >1=多路径 */

    /* ---- 零值即默认：sid_count=0 表示纯 IP（默认行为） ---- */
    struct {
        struct in6_addr     sid_list[SRV6_MAX_SEGS];
        uint8_t             sid_count;      /* 0 = 纯 IP, >0 = SRv6 */
    } explicit;
};
```

**零值语义**:

| 字段 | 零值 | 非零 |
|------|------|------|
| `path_count` | — | `=1` 单路径，`>1` 多路径 |
| `weight` | `0` | `>0` UCMP 加权 |
| `sid_count` | `0` | `>0` SRv6 严格路径 |

---

## 五、使用示例

### 示例 1：单路径纯 IP（BASIC）

```c
/* 标准 SPF 计算出唯一最短路径 */
struct midr_path_result r = {
    .path_count = 1,
    .paths = &(struct midr_path){
        .nexthop = { .ipv6 = 2001:db8:a::1 },
        .weight = 0,
    },
    .explicit.sid_count = 0,
};
midr_zebra_route_add(bgp, &prefix_10_0_0_0_24, &r);

/* 内核效果： */
/* ip route add 10.0.0.0/24 via 2001:db8:a::1 */
```

### 示例 2：等权重 ECMP

```c
/* 标准 SPF 发现 3 条等 cost 路径 */
struct midr_path ps[3] = {
    { .nexthop = { .ipv6 = 2001:db8:a::1 }, .weight = 0 },
    { .nexthop = { .ipv6 = 2001:db8:b::1 }, .weight = 0 },
    { .nexthop = { .ipv6 = 2001:db8:c::1 }, .weight = 0 },
};
struct midr_path_result r = {
    .path_count = 3,
    .paths = ps,
    .explicit.sid_count = 0,
};
midr_zebra_route_add(bgp, &prefix_10_0_0_0_24, &r);

/* 内核效果： */
/* ip route add 10.0.0.0/24 \
 *   nexthop via 2001:db8:a::1 \
 *   nexthop via 2001:db8:b::1 \
 *   nexthop via 2001:db8:c::1 */
```

### 示例 3：UCMP 加权

```c
/* CSPF 计算出 2 条不等 cost 路径，按瓶颈带宽 800:200 加权 */
struct midr_path ps[2] = {
    { .nexthop = { .ipv6 = 2001:db8:a::1 },
      .path_avail_bw = 800, .weight = 204 },  /* 800/(800+200)*255 ≈ 204 */
    { .nexthop = { .ipv6 = 2001:db8:b::1 },
      .path_avail_bw = 200, .weight = 51 },   /* 200/(800+200)*255 ≈ 51 */
};
struct midr_path_result r = {
    .path_count = 2,
    .paths = ps,
    .explicit.sid_count = 0,
};
midr_zebra_route_add(bgp, &prefix_10_0_0_0_24, &r);

/* 内核效果： */
/* ip route add 10.0.0.0/24 \
 *   nexthop via 2001:db8:a::1 weight 204 \
 *   nexthop via 2001:db8:b::1 weight 51   */
/* ip nexthop show  →  id 42 group 204,51 */
```

### 示例 4：SRv6 严格路径

```c
/* CSPF + SID 构造，路径：R1 → R2 → R3 */
struct midr_path_result r = {
    .path_count = 1,
    .paths = &(struct midr_path){
        .nexthop = { .ipv6 = 2001:db8:a::1 },  /* 物理下一跳 */
        .weight = 0,
    },
    .explicit = {
        .sid_count = 3,
        .sid_list = {
            2001:db8:1::1,   /* R1 的 End SID */
            2001:db8:2::1,   /* R2 的 End SID */
            2001:db8:3::1,   /* R3 的 End.DT46 SID */
        },
    },
};
midr_zebra_route_add(bgp, &prefix_10_1_0_0_16, &r);

/* 内核效果： */
/* ip -6 route add 10.1.0.0/16 \
 *   encap seg6 mode inline \
 *   segs [2001:db8:1::1,2001:db8:2::1,2001:db8:3::1] \
 *   via 2001:db8:a::1 */
```
> 注：nexthop 为全局 IPv6 地址时，内核自动解析出接口，无需 `dev eth0`。若使用 link-local 地址（如 `fe80::1`），则需显式指定 `dev <ifname>`。

---

## 六、接口函数

```c
/* bgpd/bgp_midr_zebra.h */

/* 安装/更新一条 MIDR 路由（只暂存，不立即下发） */
void midr_zebra_route_add(struct bgp *bgp,
                          struct prefix *p,
                          struct midr_path_result *result);

/* 撤销一条 MIDR 路由（只暂存，不立即下发） */
void midr_zebra_route_del(struct bgp *bgp,
                          struct prefix *p);

/* 触发批量下发（二选一） */
void midr_zebra_route_update_deferred(struct bgp *bgp);   /* 100ms 延迟，正常场景 */
void midr_zebra_route_flush(struct bgp *bgp);             /* 立即 flush，紧急场景 */
```

