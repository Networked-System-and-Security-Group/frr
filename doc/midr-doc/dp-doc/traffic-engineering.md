# MIDR 基于 SRv6 的流量工程设计方案

> **定位**: 流量工程组对外接口规格（含与路由组、数据平面的交互边界）  
> **范围**: 组间分工、与第二组（路由协议和消息传递）的接口对接、CP-DP 路由下发接口使用  

---

## 一、概述

本文档定义 MIDR 项目中 **基于 SRv6 的流量工程（TE）** 的完整设计方案，重点明确：

1. **组间分工边界**：流量工程组（本组）与路由协议组（第二组）、数据平面执行层的职责划分
2. **模块间接口**：第二组提供的数据结构与服务，本组 consumed 的 API 契约
3. **CP-DP 路由下发接口**：控制平面计算结果如何交给数据平面，最终安装到 Linux 内核 FIB

**设计原则**：
- **Phase 1（当前）**：**QoS 保障**。管理员为特定前缀配置性能约束，CSPF 计算满足约束的 SRv6 路径并持久生效
- **Phase 2（后续）**：**拥塞避免**。自动监测链路拥塞，动态迁移拥塞链路上的流量
- **默认标准路由 + 策略覆盖**：无策略前缀走标准 SPF；有策略前缀由 CSPF 覆盖
- **单套 LSDB**：标准 SPF 与 CSPF 共用同一 `ls_ted`
- **零值默认**：`sid_count = 0` 表示标准路由
- **同进程内嵌**：TE 内嵌在 `bgpd`，不引入 pathd/PCEP
- **TE 不计算权重**：标准 ECMP 由第二组负责

---

## 二、模块边界：路由、流量工程、数据平面三层关系

### 2.1 三层架构

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         控制平面 (Control Plane)                         │
│  ─────────────────────────────────────────────────────────────────────  │
│                                                                         │
│  ┌──────────────────────┐    ┌──────────────────────┐                  │
│  │   路由协议组         │    │   流量工程组         │                  │
│  │   （第二组）         │    │   （本组）           │                  │
│  │                      │    │                      │                  │
│  │  • BGP-LS 收发       │    │  • 拥塞监测/触发     │                  │
│  │  • NLRI 解析         │    │  • TE 策略表         │                  │
│  │  • TED/LSDB 维护     │    │  • CSPF 约束计算     │                  │
│  │  • 标准 SPF 计算     │    │  • SRv6 SID 构造     │                  │
│  │    （默认路由）      │    │    （拥塞迁移路径）  │                  │
│  └──────────┬───────────┘    └──────────┬───────────┘                  │
│             │                           │                               │
│             │ 标准 SPF 结果             │ CSPF + SID 结果               │
│             │ (sid_count=0)             │ (sid_count>0)                 │
│             │                           │                               │
│             └───────────────┬───────────┘                               │
│                             ▼                                           │
│              ┌──────────────────────────────┐                          │
│              │   midr_path_result (统一载体) │                          │
│              │   控制平面 → 数据平面的唯一输出 │                         │
│              └──────────────┬───────────────┘                          │
│                             │                                           │
└─────────────────────────────┼───────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                         数据平面 (Data Plane)                            │
│  ─────────────────────────────────────────────────────────────────────  │
│                                                                         │
│  ┌──────────────────────────────────────────────────────────────┐      │
│  │  bgp_midr_zebra.c                                            │      │
│  │  • 模式识别 (BASIC / ECMP / SRv6)                           │      │
│  │  • 填充 zapi_route + zapi_nexthop                            │      │
│  │  • 批量延迟聚合 (100ms) → zebra                              │      │
│  └──────────────────────────┬───────────────────────────────────┘      │
│                             │                                        │
└─────────────────────────────┼───────────────────────────────────────────┘
                              │ ZAPI
                              ▼
                     ┌───────────────────┐
                     │ zebra → Kernel    │
                     │ (RIB→NHG→Netlink) │
                     └───────────────────┘
```

**核心关系**：
- **路由**与**流量工程**是控制平面的**并列**能力
- **标准 SPF**（第二组）计算默认路径；**CSPF**（本组）为策略前缀计算约束路径
- **数据平面**统一接收两组输出，通过 `midr_zebra_route_add()` 下发

### 2.2 模块依赖图

```
控制平面
─────────────────────────────────────────────────────────────────────────

第二组（路由协议）              本组（流量工程）
──────                          ────

┌──────────────┐                ┌──────────────┐
│ BGP-LS NLRI  │─── TED(只读)──►│ 拥塞监测     │
│ 解析/收发    │                │ • 链路利用率 │
└──────────────┘                │ • 延迟阈值   │
                                └──────┬───────┘
┌──────────────┐                       │
│   TED/LSDB   │                       │ 拥塞触发
│  (ls_ted)    │                       ▼
└──────┬───────┘         ┌──────────────────────────┐
       │                 │ TE 策略表                │
       │                 │ (前缀 → 启用 TE 迁移)    │
       │                 └────────────┬─────────────┘
       │                              │
       │ 标准 SPF 结果                │ CSPF 计算
       │ (所有前缀, 默认路径)         │ (仅拥塞时,
       │                              │  仅策略前缀)
       ▼                              ▼
┌──────────────┐              ┌──────────────────────────┐
│ 标准 SPF     │              │ CSPF 引擎                │
│ (默认路由)   │              │ • 剪枝 (排除拥塞链路)    │
│ • 单路径     │              │ • K-SPF                  │
│ • ECMP       │              │ • SID 列表构造           │
└──────┬───────┘              └────────────┬─────────────┘
       │                                   │
       └───────────────────────────────────┘
                   │
                   ▼
        midr_path_result (CP → DP 统一载体)
            • paths[].nexthop
            • explicit.sid_list[] / sid_count
                       │
                       ▼
数据平面（执行层）
─────────────────────────────────────────────────────────────────────────

┌──────────────────────────────────────────────────────────────┐
│ CP-DP 接口: midr_zebra_route_add()                           │
│ 数据平面: bgp_midr_zebra.c                                    │
│ • 模式识别 (BASIC / ECMP / SRv6)                            │
│ • 填充 zapi_route + zapi_nexthop                              │
│ • 批量延迟聚合 (100ms)                                        │
└─────────────────────────────┬────────────────────────────────┘
                              │ ZAPI
                              ▼
                     ┌─────────────────┐
                     │ zebra → Kernel  │
                     └─────────────────┘
```

**关键点**：
- **标准 SPF**（路由组）和 **CSPF**（流量工程组）是并列的路径计算模块
- **本组 TE 不计算负载均衡权重**；标准路由的 ECMP 由第二组负责，内核自动均衡
- 数据平面**不区分**结果来源，统一通过 `midr_zebra_route_add()` 下发
- **SRv6 严格路径为单路径**（无多路径权重概念）

### 2.3 关键边界约定

| 边界 | 第二组承诺 | 本组承诺 |
|------|-----------|---------|
| **LSDB** | `ls_ted` 只读；拓扑变化通过 `bgp_midr_spf_trigger()` 回调通知 | 绝不修改 `ls_vertex`/`ls_edge` |
| **标准 SPF** | 为所有前缀计算默认路由（`sid_count=0`） | 不干预标准 SPF 的 ECMP |
| **SRv6 SID** | TED 中 `ls_pref->srv6.sid` / `ls_node->srv6` 正确填充 | CSPF 从 TED 读取 SID，不自行分配 |
| **TE 属性** | `ls_edge->attributes` 包含 `delay`, `ava_bw`, `admin_group`, `srlgs` | 基于这些属性做剪枝 |
| **负载均衡** | 标准路由 ECMP 由第二组负责 | **本组 TE 不计算权重** |

---

## 三、与第二组（路由协议）的接口对接

### 3.1 接口定位

第二组（路由协议）和本组（流量工程）是**控制平面内并列的两个计算层**：

- **第二组**：负责**默认路由**计算（标准 SPF），为**所有前缀**输出 `midr_path_result`（`sid_count = 0`）；多路径时为 ECMP
- **本组**：负责**QoS 保障**与**拥塞避免**：
  - **QoS 保障（Phase 1）**：管理员为特定前缀配置性能约束，本组通过 CSPF 计算始终满足约束的 SRv6 路径并持久生效
  - **拥塞避免（Phase 2）**：自动监测链路拥塞，将受影响流量动态迁移到替代路径；拥塞解除后自动回退
- **数据平面**：统一接收两组输出，通过 `midr_zebra_route_add()` 无差别下发

**流量工程不是路由的替代，而是对标准路由的补充**：
- **无策略前缀**：所有流量走第二组的标准 SPF 默认路径
- **QoS 保障前缀**：本组计算满足约束的 SRv6 路径，持久生效，为关键业务提供 SLA 保障
- **拥塞迁移前缀**：拥塞时本组计算替代路径并迁移流量；拥塞解除后回退到标准 SPF 默认路径

### 3.2 第二组提供的核心数据结构

本组**只读访问**以下第二组维护的数据结构：

#### (1) TED 图结构 (`ls_ted`)

```c
/* lib/link_state.h — 第二组维护 */

struct ls_ted {
    struct ls_table *vertices;      /* 网络节点 */
    struct ls_table *edges;         /* 单向链路 */
    struct ls_table *subnets;       /* 前缀信息 */
};

struct ls_edge {
    struct ls_vertex  *source;
    struct ls_vertex  *destination;
    struct ls_attributes *attributes;  /* TE 属性 */
};

struct ls_attributes {
    struct ls_standard {
        uint32_t te_metric;
        uint32_t admin_group;
        float    max_bw;
    } standard;
    struct ls_extended {
        uint32_t delay;
        float    ava_bw;
        float    used_bw;           /* Phase 2 拥塞监测 */
    } extended;
    uint32_t *srlgs;
    uint8_t   srlg_len;
};
```

#### (2) SRv6 SID 信息

```c
struct ls_prefix {
    struct srv6_sid   srv6;         /* SRv6 SID (128-bit) */
};

struct ls_node {
    struct srv6_locator *srv6_locator;
};
```

#### (3) 标准 SPF 结果

第二组为**所有前缀**输出 `midr_path_result`（`sid_count = 0`）作为默认路由。本组仅对**策略前缀**输出 `sid_count > 0` 的 TE 路径。

### 3.3 第二组提供的回调/触发机制

| 事件 | 第二组动作 | 本组响应 |
|------|-----------|---------|
| **BGP-LS UPDATE** | 更新 TED → `bgp_midr_spf_trigger()` | Phase 1: 重新评估策略约束路径；Phase 2: 检查拥塞状态 |
| **TED 全量同步完成** | `bgp_midr_spf_run()` | 初始化拥塞监测基线（Phase 2） |

## 四、SRv6 TE 设计方案：QoS 保障与拥塞避免

### 4.1 架构全景

```
控制平面 (bgpd)
─────────────────────────────────────────────────────────────────────────

                    ┌─────────────────────────────────────┐
                    │  路由协议组（第二组）                │
                    │  • BGP-LS 收发 / NLRI 解析          │
                    │  • TED/LSDB 维护                    │
                    │  • 标准 SPF 计算（默认路由）        │
                    │  • ECMP（等权重多路径）             │
                    └──────────────┬──────────────────────┘
                                   │
                                   │ 标准 SPF 结果
                                   │ (所有前缀, sid_count=0)
                                   │
                                   ▼
                    ┌─────────────────────────────────────┐
                    │  流量工程组（本组）                │
                    │  • 拥塞监测（利用率/延迟阈值）     │
                    │  • TE 策略表（前缀→启用迁移）      │
                    │  • CSPF 约束计算（排除拥塞链路）   │
                    │  • SID 列表构造（SRv6 替代路径）   │
                    └──────────────┬──────────────────────┘
                                   │
                                   │ CSPF + SID 结果
                                   │ (仅拥塞时, sid_count>0)
                                   │
              ┌────────────────────┼────────────────────┐
              │                    │                    │
              │  正常状态          │  拥塞状态          │  拥塞解除
              │  (默认)            │  (TE 介入)         │  (回退)
              │                    │                    │
              ▼                    ▼                    ▼
        标准 SPF 结果        CSPF 结果           标准 SPF 结果
        (sid_count=0)      (sid_count>0)        (sid_count=0)
              │                    │                    │
              └────────────────────┴────────────────────┘
                                   │
                                   ▼
                ┌─────────────────────────────────────┐
                │  数据平面（执行层）                 │
                │  midr_zebra_route_add()             │
                │  • 模式识别 / ZAPI 构造             │
                │  • 批量延迟聚合 → zebra             │
                └─────────────────────────────────────┘
                                   │
                                   ▼ ZAPI
                             zebra → Kernel
```

**核心设计**：
- **Phase 1（当前）— QoS 保障**：管理员为特定前缀配置性能约束（如延迟上限、带宽保证、丢包率、避开特定 SRLG）→ CSPF 计算始终满足约束的 SRv6 路径 → 下发并持久生效 → 策略撤销后回退到标准 SPF。**目标：为关键业务提供确定性 SLA。**
- **Phase 2（后续）— 拥塞避免**：自动监测链路拥塞状态 → 对拥塞链路所经流量（或配置了 `srv6-migrate` 的前缀）动态计算替代路径并迁移 → 拥塞解除后自动回退。**目标：从全网角度缓解拥塞，避免拥塞扩散。**
- **无策略前缀**：所有流量走第二组的标准 SPF 默认路径（单路径或 ECMP）
- **本组 TE 不计算负载均衡权重**；SRv6 严格路径为单路径
- **Phase 1 与 Phase 2 可同时生效**：一个前缀可同时受 QoS 策略约束和拥塞迁移影响，CSPF 取两种约束的交集。

### 4.2 TE 策略表：决定哪些流量需要迁移

极简策略映射层：前缀 → 是否在拥塞时启用 TE 迁移。

**设计思路**：

用一个前缀索引的策略表（radix tree）将目标前缀映射到对应的策略类型及约束条件。每条策略记录包含：
- **策略类型**：区分 Phase 1（`admin-steer`，管理员配置后持续生效）和 Phase 2（`srv6-migrate`，拥塞时自动触发）。
- **共用约束**：延迟上限、最大跳数限制，两个 Phase 均可使用。
- **Phase 1 专用约束**：最小带宽保证、最大丢包率、Admin-Group 亲和掩码、需避开的 SRLG 列表。
- **Phase 2 专用约束**：链路利用率阈值，超过该值即视为拥塞。

策略表对外暴露增删查接口；前缀未命中时默认走标准 SPF，不做 TE 干预。

**关键设计**：
- **Phase 1（当前）— `admin-steer`**：管理员为特定前缀配置 QoS 约束 → CSPF 立即计算满足约束的 SRv6 路径 → 持久生效 → 管理员删除策略后回退到 SPF。**与拥塞状态无关**，始终为关键业务保障 SLA。
- **Phase 2（后续）— `srv6-migrate`**：对配置的前缀持续监测链路利用率 → 拥塞时自动触发 CSPF 计算并迁移；拥塞解除后自动回退。**从全网角度**缓解拥塞，不区分业务类型。
- **无策略前缀**：走标准 SPF 默认路径，本组不干预
- **两种策略可同时作用于不同前缀**：关键业务走 `admin-steer`（QoS 保障），普通流量在拥塞时通过 `srv6-migrate` 自动绕行

### 4.3 拥塞监测与触发

**设计思路**：

拥塞监测模块定期（或事件驱动）遍历 TED 中的链路，依据策略表中对应前缀的约束条件判断是否存在拥塞：
- **利用率检测**：读取链路的已用带宽与最大带宽，计算利用率百分比，超过阈值即判定拥塞。
- **延迟检测**：直接比较链路延迟与配置上限。
- 一旦检测到任一链路满足拥塞条件，立即返回该链路指针，供后续 CSPF 剪枝使用。
- 为避免频繁抖动，可引入**滞回阈值**（Hysteresis）：拥塞触发阈值与解除阈值分离，防止在边界值上来回切换。

### 4.4 CSPF + SRv6 替代路径计算

CSPF 引擎为 Phase 1 和 Phase 2 共用，剪枝逻辑根据 `policy->type` 和 `constraints` 自动适配。

#### 4.4.1 约束剪枝

**设计思路**：

CSPF 引擎为 Phase 1 和 Phase 2 共用，核心流程分三步：

1. **约束剪枝**：遍历 TED 所有链路，依据策略类型和约束条件逐条判断是否保留。共用约束（延迟上限、最大跳数）两类策略均检查；Phase 1 额外检查带宽保证、丢包率、Admin-Group 亲和性、SRLG 排除；Phase 2 额外检查链路利用率，将超阈值的拥塞链路剪除。剪枝后的子图即为候选拓扑。

2. **K-最短路径计算**：在剪枝后的子图上运行 K-SPF（如 Yen 算法），计算从源到目的的多条候选路径。取排名前 K 的路径供下一步 SID 构造。

3. **SID 列表构造**：对选中的最优路径逐跳查询可用的 SRv6 SID，生成显式路径列表。若任意一跳无法解析到 SID，则该路径构造失败，尝试下一条候选路径。三段式容错保证最终输出的路径可下发。

#### 4.4.2 SID 列表构造

**设计思路**：

SID 列表构造将 CSPF 计算出的物理路径逐跳映射为 SRv6 SID 序列，遵循以下优先级策略：

- **优先级 1：Adjacency SID (End.X)** — 显式指定出接口链路，粒度最细，优先选用。从 TED 的 `ls_edge` 中查询 BGP-LS 携带的 End.X SID。
- **优先级 2：Node SID (End)** — 若链路级 SID 不可用，退而求其次使用节点级 SID，从 `ls_node` 的 SRv6 Locator 派生。
- **优先级 3：End.DT46** — 路径末尾若需解封装到 VRF，追加目的节点的 End.DT46 SID。

逐跳解析：若任意一跳在两个优先级内均无法找到可用 SID，则整条路径构造失败，返回错误。外层调用可回退到 K-SPF 的下一条候选路径重试，或最终放弃 TE 干预、保持标准 SPF。

| SID 类型 | 优先级 | 用途 | 来源 |
|---------|--------|------|------|
| **Adjacency SID (End.X)** | 1 | 显式指定链路 | `ls_edge` 的 BGP-LS End.X SID TLV |
| **Node SID (End)** | 2 | 路由到节点 | `ls_node->srv6_locator` + Function |
| **End.DT46** | 3 | 解封装到 VRF | 目的节点 `ls_pref->srv6.sid` |

### 4.5 回退策略

| 场景 | 功能 | 策略类型 | 触发条件 | 回退行为 | 输出 |
|------|------|---------|---------|---------|------|
| **无策略** | — | — | 前缀不在策略表中 | 使用第二组标准 SPF 默认路径 | `sid_count = 0`，纯 IP |
| **QoS 保障生效** | Phase 1 | `ADMIN_STEER` | 管理员配置策略 | CSPF 计算约束路径，**持久生效**；QoS 条件不满足时告警但不回退 | `sid_count > 0`，SRv6 |
| **QoS 保障撤销** | Phase 1 | `ADMIN_STEER` | 管理员删除策略 | **显式回退**：撤销 SRv6 路由，恢复标准 SPF | `sid_count = 0`，纯 IP |
| **QoS：CSPF 失败** | Phase 1 | `ADMIN_STEER` | 剪枝后无可达路径 | 不下发 TE 路由，告警，继续使用标准 SPF | `sid_count = 0`，纯 IP |
| **拥塞迁移** | Phase 2 | `SRV6_MIGRATE` | 链路利用率 > 阈值 | CSPF 计算替代路径，临时生效 | `sid_count > 0`，SRv6 |
| **拥塞解除** | Phase 2 | `SRV6_MIGRATE` | 链路利用率回落 < 阈值 | **自动回退**：撤销 SRv6 路由，恢复标准 SPF | `sid_count = 0`，纯 IP |
| **SID 构造失败** | 任意 | 任意 | 某跳缺少 SRv6 SID | 无法下发 TE 路由，继续使用标准 SPF | `sid_count = 0`，纯 IP |

**关键区别**：
- **QoS 保障（Phase 1）**：回退是**管理员驱动的**（删除策略时才回退）。即使 QoS 约束临时无法满足（如链路故障导致延迟超标），系统告警但**不回退到 SPF**，因为关键业务的 SLA 要求优先。
- **拥塞避免（Phase 2）**：回退是**自动的**（拥塞解除即回退）。拥塞迁移是临时性全网优化，拥塞消失后应恢复标准路由，避免长期占用非最优路径。

### 4.6 Zebra RIB Dual-Instance 机制：覆盖与自动回退

#### 4.6.1 设计目标

TE 路由（SRv6 策略路径）需要**覆盖**标准 SPF 路由。对于 Phase 2（拥塞自动迁移），当拥塞解除、TE 路由被撤销时，必须**自动回退**到标准 SPF 路由，且无需重新计算或重新下发 SPF 路由。对于 Phase 1（管理员配置策略），回退仅在管理员显式删除策略时发生。

**解决方案**：利用 zebra RIB 的 `instance` 字段，让同一前缀的两条路由在 zebra RIB 中**共存**，通过 `metric` 竞争最优。

#### 4.6.2 机制原理

```
zebra RIB (用户态)
├── prefix 10.1.0.0/16
│   ├── route_entry A: instance=0, metric=100, type=ZEBRA_ROUTE_BGP
│   │   └── nexthop: 2001:db8:a::1          ← 标准 SPF 路径 (始终保留)
│   └── route_entry B: instance=1, metric=1, type=ZEBRA_ROUTE_BGP
│       └── nexthop: 2001:db8:a::1 + SRv6 SID List  ← TE SRv6 路径 (策略配置时添加，Phase 1/2 共用)
│
rib_choose_best() → 比较 metric: 1 < 100 → 选中 B
│
rib_install() → 下发到内核 FIB:
    ip -6 route add 10.1.0.0/16 encap seg6 ... via 2001:db8:a::1
```

**关键代码机制**（`zebra/zebra_rib.c`）

1. **路由唯一标识**：`rib_compare_routes()` 要求 `type` 和 `instance` **都相等**才认为是同一条路由：
   ```c
   static bool rib_compare_routes(const struct route_entry *re1,
                                  const struct route_entry *re2, bool replace)
   {
       if (re1->type != re2->type)
           return false;
       if (re1->instance != re2->instance)
           return false;
       ...
   }
   ```
   - `instance` 不同时 → `rib_compare_routes()` 返回 `false` → **不隐式撤销**，新路由直接加入链表

2. **Best Route 选择**：`rib_process()` 遍历前缀下的**所有**路由条目，`rib_choose_best()` 按优先级选择：
   ```c
   /* 优先级：LOCAL > CONNECT > lower distance > lower metric > older route */
   if (alternate->metric <= current->metric)
       return alternate;
   ```

3. **删除与自动回退**：`process_subq_early_route_delete()` 按 `(type, instance)` 精确匹配删除：
   ```c
   RNODE_FOREACH_RE (rn, re) {
       if (re->type != ere->re->type)    continue;
       if (re->instance != ere->re->instance) continue;
       ...
   }
   ```
   - 删除 TE 路由 (`instance=1`) 后，`rib_process()` 重新遍历
   - SPF 路由 (`instance=0`) 成为新的 best → `rib_install()` **自动**将其下发到内核替换旧路由
   - **无需显式重新添加 SPF 路由**

#### 4.6.3 参数约定

| 路由来源 | `zapi_route.instance` | `metric` | 行为 |
|---------|----------------------|----------|------|
| **标准 SPF**（第二组） | `0` | IGP metric（如 100） | 默认路由，始终存在于 zebra RIB |
| **TE SRv6**（本组） | `1` | `1`（固定，优于 IGP） | 策略配置时添加，覆盖 SPF 路由 |

**为什么 metric=1 可行**：
- zebra 选择最优路由时，`metric` 是核心 tie-breaker
- `TE metric=1 << SPF metric=IGP`，确保 TE 路由被选中
- 当 TE 路由删除后，SPF 路由自动被 zebra 提升为 best 并重新安装

#### 4.6.4 内核层面的事实

**Linux 内核 FIB 本身没有 `instance` 概念**。zebra 只将当前选中的 best route 通过 netlink 下发到内核：

```
zebra RIB (用户态，多 instance 共存)
        │
        ▼ rib_install()
   Netlink RTM_NEWROUTE
        │
        ▼
Kernel FIB (单条生效路由)
```

- 内核中同一 prefix 只有**一条**生效路由（当前 best）
- 当 best 从 TE 切换回 SPF 时，zebra 自动用 netlink 替换内核路由
- 无需特殊内核补丁或硬件支持，纯 FRR 软件行为

#### 4.6.5 为什么不用单 instance 设计

| 维度 | dual-instance（本方案） | 单 instance（替代方案） |
|---|---|---|
| **RIB 状态** | SPF 路由始终保留在 RIB | 同一 prefix 只有一条，TE 替换 SPF |
| **TE 撤回** | **自动 fallback**：删除 TE 路由，zebra 自动选中 SPF | **需显式恢复**：TE 模块必须重新下发 SPF 路由 |
| **模块耦合** | **松耦合**：TE 模块不感知 SPF 路由内容 | **紧耦合**：TE 模块需缓存 SPF 的 nexthop/metric |
| **可靠性** | TE 模块崩溃 → 其路由被清理 → **SPF 自动恢复** | TE 模块崩溃 → 可能丢失缓存的 SPF 状态 → **路由黑洞** |
| **回退速度** | zebra 本地处理，无跨模块通信 | 需 TE → 数据平面的重新下发流程 |

对于 MIDR **TE 路由覆盖 SPF 并需要可靠回退** 的场景（无论是 Phase 1 的策略撤销还是 Phase 2 的拥塞解除），dual-instance 明显更优。

---

## 五、CP-DP 路由下发接口使用

### 5.1 接口定位

`midr_zebra_route_add()` 是**控制平面将计算结果交给数据平面的唯一正式通道**，单向（CP → DP）。

**重要**：
- 数据平面**不区分**计算结果来自标准 SPF（路由组）还是 CSPF（流量工程组），统一接收、统一处理
- 数据平面负责将 `midr_path_result` 转换为 `zapi_route` 并设置 `instance` 和 `metric`，具体实现见 `midr-dataplane-implementation.md`
- 本组只关心输出 `midr_path_result`，不感知 `zapi_route` 的编码细节

```
控制平面                                    数据平面
────────                                    ────────

┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│ 标准 SPF     │     │              │     │              │
│ (第二组)     │────►│              │     │              │
└──────────────┘     │  midr_path_  │────►│ midr_zebra_  │
                     │  result      │     │ route_add()  │
┌──────────────┐     │  (统一载体)  │     │              │
│ CSPF         │────►│              │     │              │
│ (本组)       │     │              │     │              │
└──────────────┘     └──────────────┘     └──────┬───────┘
                                                  │
                                                  ▼ ZAPI
                                               zebra
```

### 5.2 本组输出数据结构

```c
/* 计算结果（CP → DP 数据载体）— 本组关心的字段 */
struct midr_path_result {
    struct midr_path    *paths;
    uint8_t             path_count;

    struct {
        struct in6_addr sid_list[SRV6_MAX_SEGS];  /* SRv6 SID 列表 */
        uint8_t         sid_count;      /* 0 = 标准路由, >0 = TE SRv6 路径 */
    } explicit;
};
```

**TE 输出约定**：`sid_count > 0` 时数据平面构造 SRv6 路由（`instance=1, metric=1`）；`weight` 始终为 0。

### 5.3 模式识别

数据平面根据 `sid_count` 区分：

| 条件 | 行为 | 来源 |
|------|------|------|
| `sid_count = 0` | 标准路由（BASIC/ECMP），由第二组标准 SPF 计算 | 第二组 |
| `sid_count > 0` | **SRv6 严格路径**，由本组 CSPF 计算 | **本组** |

### 5.4 接口函数

```c
/* 安装/更新 TE 路由（数据平面内部暂存，延迟下发） */
void midr_zebra_route_add(struct bgp *bgp, struct prefix *p,
                          struct midr_path_result *result);

/* 撤销 TE 路由 */
void midr_zebra_route_del(struct bgp *bgp, struct prefix *p);

/* 触发批量下发 */
void midr_zebra_route_flush(struct bgp *bgp);   /* 立即下发 */
```

### 5.5 使用示例（TE SRv6 模式）

#### 5.5.1 示例：CSPF → SRv6 路径计算

```c
/* 本组检测到链路拥塞，对配置了策略的前缀执行 CSPF + SID 构造
 * 输出 SRv6 严格路径（单路径），交给数据平面下发
 * 数据平面需设置 instance=1, metric=1 以覆盖标准 SPF 路由
 */
struct midr_path_result r = {
    .path_count = 1,
    .paths = &(struct midr_path){
        .nexthop = { .ipv6 = 2001:db8:a::1 },  /* 物理下一跳 */
        .weight = 0,                            /* TE 不计算权重 */
    },
    .explicit = {
        .sid_count = 3,
        .sid_list = {
            2001:db8:1::1,   /* R1 End SID */
            2001:db8:2::1,   /* R2 End SID */
            2001:db8:3::1,   /* R3 End.DT46 SID */
        },
    },
};
midr_zebra_route_add(bgp, &prefix_10_1_0_0_16, &r);
midr_zebra_route_update_deferred(bgp);

/* 内核效果（由 zebra 安装）：
 * ip -6 route add 10.1.0.0/16 \
 *   encap seg6 mode inline \
 *   segs [2001:db8:1::1,2001:db8:2::1,2001:db8:3::1] \
 *   via 2001:db8:a::1
 *
 * zebra RIB 中同时存在：
 *   instance=0, metric=100  (SPF 路由，保留)
 *   instance=1, metric=1    (TE SRv6 路由，当前生效)
 */
```



---

## 六、完整数据流（端到端）

```
第二组（路由协议）              本组（流量工程）            zebra RIB / Kernel
──────                          ────                        ─────────────────

[1] 收到 BGP-LS UPDATE
    │
    ▼
[2] 解析 NLRI → 更新 TED (ls_ted)
    │
    ├──► [3a] 标准 SPF 计算
    │      │ 为所有前缀计算默认路径
    │      │
    │      ▼
    │   midr_path_result (sid_count=0)
    │   instance=0, metric=IGP
    │      │
    │      │                     [3b] 查 TE 策略表
    │      │                     │ 仅策略前缀
    │      │                     ▼
    │      │                   CSPF 约束计算
    │      │                   (Phase 1: 延迟/带宽/SRLG)
    │      │                   (Phase 2: 额外排除拥塞链路)
    │      │                     │
    │      │                     ▼
    │      │                 midr_path_result (sid_count>0)
    │      │                 instance=1, metric=1
    │      │                     │
    │      └──────────┬──────────┘
    │                 ▼
    │            zebra RIB 共存
    │            ├── instance=0, metric=IGP (SPF 默认路径)
    │            └── instance=1, metric=1   (TE 约束路径)
    │                 │
    │                 ▼ rib_choose_best()
    │            选中 metric 更优者
    │                 │
    │                 ▼
    │              Kernel FIB (单条生效)
```

**关键路径**：
- **标准 SPF 与 TE 策略计算是并行独立的**：第二组标准 SPF 为所有前缀计算默认路径；本组仅对策略前缀计算约束路径
- **zebra RIB 中共存**：同一前缀下，`instance=0`（SPF）与 `instance=1`（TE）作为独立条目共存
- **metric 竞争择优**：TE 路由 `metric=1` 优于 SPF 路由 `metric=IGP`，zebra 选中 TE 路径下发到内核
- **回退机制**：删除 `instance=1` 的 TE 路由后（Phase 1 由管理员驱动，Phase 2 由拥塞解除自动触发），zebra 自动重新选中 `instance=0` 的 SPF 路由

---

## 七、文件清单与分工

### 7.1 本组新增/负责文件

| 优先级 | 文件 | 说明 | 代码量估算 |
|--------|------|------|-----------|
| P0 | `bgpd/bgp_midr_te.c/h` | **TE 策略表** + 策略下发/撤销；拥塞监测框架（Phase 2） | ~400 行 |
| P0 | `bgpd/bgp_midr_cspf.c/h` | **CSPF** 约束路径计算 | ~500 行 |
| P1 | `bgpd/bgp_midr_vty.c/h` | CLI / 调试命令 | ~300 行 |

**注意**：本组**不包含** `bgp_midr_lb.c`（负载均衡权重计算），TE 不计算多路径权重。

### 7.2 第二组提供、本组依赖的文件（不修改）

| 文件 | 说明 |
|------|------|
| `bgpd/bgp_ls.c/h` | BGP-LS UPDATE 收发 |
| `bgpd/bgp_ls_nlri.c/h` | NLRI 解析 |
| `bgpd/bgp_ls_ted.c/h` | TED 维护 |
| `bgpd/bgp_midr.c/h` | MIDR 基础框架、**SPF 触发** |
| `bgpd/bgp_midr_spf.c/h` | **标准 SPF 计算**（默认路由） |
| `lib/link_state.h` | TED 数据结构 |
| `lib/zclient.c/h` | ZAPI 通信 |

### 7.3 零修改复用（zebra）

| 文件 | 说明 |
|------|------|
| `zebra/zapi_msg.c` | ZAPI 消息接收 |
| `zebra/zebra_rib.c` | RIB 决策 |
| `zebra/zebra_nhg.c` | NHG 管理（ECMP） |
| `zebra/zebra_dplane.c` | 异步 Dataplane |
| `zebra/rt_netlink.c` | Netlink SRv6/weight 编码 |

---

## 八、风险与缓解

| 风险 | 影响 | 缓解措施 | 责任方 |
|------|------|---------|--------|
| **CSPF 无满足约束路径** | 策略前缀无法获得 SRv6 路径 | 告警并继续使用标准 SPF；管理员可调整约束 | 本组 |
| **SRH 长度导致分片** | 大包分片影响性能 | 限制 SID 数量（建议 ≤ 5） | 本组 |
| **TED 与 BGP-LS 不同步** | CSPF 看不到远程 SRv6 信息 | 本组独立维护 MIDR SRv6 DB 作为补充 | 本组 |
| **跨 AS Locator 不可达** | SRv6 数据包被丢弃 | 确保 locator 前缀通过标准 BGP/IGP 传播 | 第二组/运营商 |
| **Phase 2 频繁震荡** | 利用率在阈值附近波动导致路由反复切换 | 引入 hysteresis：高于阈值 10% 才迁移，低于阈值 5% 才回退 | 本组 |

---

## 九、方案实施计划

| 组件 | Phase 1（当前） | Phase 2（后续） |
|------|----------------|----------------|
| `ADMIN_STEER` | ✅ 实现 | — |
| `SRV6_MIGRATE` | ⏳ 枚举预留 | ✅ 实现 |
| QoS 约束剪枝 | ✅ `delay`, `bw`, `loss`, `SRLG` | ✅ 复用 |
| 拥塞监测 | ⏳ 框架预留 | ✅ `midr_te_detect_congestion()` |
| Hysteresis | — | ✅ 上/下阈值分离 |
| Dual-Instance | ✅ `instance=1` + `metric=1` | ✅ 复用 |

**Phase 2 增量工作量**：仅在 `bgp_midr_te.c` 中增加拥塞检测循环和状态机；CSPF、SID 构造、数据平面均无需改动。

---

## 十、总结

| 项目 | 结论 |
|------|------|
| **组间接口** | 第二组提供 `ls_ted`（只读）、标准 SPF、拓扑通知；本组输出 `midr_path_result`（`sid_count>0`） |
| **CP-DP 接口** | `midr_path_result` 是统一载体，`sid_count` 区分标准路由（`0`）/ TE SRv6（`>0`） |
| **Phase 1（当前）** | **QoS 保障**：管理员配置约束 → CSPF 计算 SRv6 路径 → 持久生效 |
| **Phase 2（后续）** | **拥塞避免**：自动监测拥塞 → 动态迁移流量 → 拥塞解除后自动回退 |
| **Zebra RIB** | **Dual-Instance**：`instance=0` (SPF) + `instance=1` (TE) 共存，通过 `metric` 竞争；删除 TE 后自动 fallback |
