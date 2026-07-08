# 邻居关系构建‐链路状态传播接口文档

## 1. 总体边界

第一组和第二组在同一个 FRR 进程内实现，组间接口采用 C 函数调用，不定义 IPC、protobuf、socket 或外部消息格式。v1 工程实现以内嵌 `bgpd` 模块为准；后续如需形成独立 `midrd` daemon，应从 `bgpd` 代码框架派生并保留对 peer/session、RIB、hook、BGP-LS 等内部能力的直接复用。

第一组负责产生 `bgpd` 无法直接获得的 MIDR 拓扑事实：

```text
BGP session 建立意图
本节点 group 归属
本节点 MIDR 能力
本节点 transport address
本节点出向 directed link 状态
link performance measurement
link policy / node policy
```

第二组负责消费第一组提供的拓扑事实，并结合 `bgpd` 已有状态维护 MIDR 链路状态视图：

```text
读取 bgpd peer/session/RIB/hook
维护 router-to-group / link table / LSDB / TED
生成和解析 MIDR Link-State 对象
决定 Link-State 对象传播范围
生成 Link-State update / withdraw
执行链路状态传播
执行路径计算
向第三组输出路径结果
```

第一组可以在邻居发现或性能测量模块内部使用自身的 NDS/PM TLV 或消息格式，但最终进入 MIDR 链路状态传播系统的 Node / Link / Prefix / Group Prefix Link-State 对象由第二组统一生成、解析、维护和传播。第一组向第二组提交的是 node、link、measurement、policy 等事实状态，不直接写第二组 LSDB/TED，也不直接生成最终 Link-State NLRI。

以下信息由 `bgpd` 或第二组内部直接获得，不通过第一组重复传递：

```text
BGP Router-ID
BGP session FSM 状态
OPEN / KEEPALIVE / UPDATE 协议处理
普通 BGP prefix NLRI
BGP RIB
best-path / multipath selection
BGP attribute 基础解析结果
```

Prefix 不作为第一组接口对象。第二组从 `bgpd` RIB 和 `bgp_route_update` hook 读取 prefix route，再结合 `peer -> node -> group` 映射生成 MIDR 内部的 `Group Prefix Reachability` 或 `prefix-to-group` 状态。

## 2. 对象 ownership 与上报范围

v1 采用本地 ownership 模型：

```text
每个节点只上报自己的 Node / Group Membership 对象
每个节点只上报以自己为 local_node_id 的出向 Link 对象
远端节点对象由远端节点自己 originate
远端 link 对象由对应 link 的 local_node_id 节点 originate
```

第一组不替远端节点代报 node/group/capability，也不替远端节点 withdraw 远端 originate 的对象。远端对象通过第二组链路状态传播、收包解析、LSDB/TED 维护被其他节点学习。

本地 Node / Group Membership 对象不依赖任何 BGP session，可以在本节点 MIDR 初始化后立即上报。依赖远端 Router-ID 的 topology event 必须等对应 BGP session Established 且 `peer->remote_id` 可用后上报。

## 3. BGP Session 接口

第一组负责决定何时、向谁建立 MIDR BGP session，并负责通过邻居发现机制确保两端都触发建连请求。第二组提供本端 `bgpd` peer/session 封装接口，不负责跨节点通知对端配置。

`midr_peer_session_request()` 只完成本端 peer 配置和 AFI/SAFI 激活。对端 peer 配置可由第一组现有 UDP `PEER_REQUEST` 机制触发，也可以由两端根据 discovery 结果对称调用 `midr_peer_session_request()` 完成。v1 推荐采用两端对称调用模型：

```text
A 侧发现需要与 B 建连
  -> A 本地调用 midr_peer_session_request(B)
  -> 第一组 NDS 机制通知 B
  -> B 本地调用 midr_peer_session_request(A)
```

第一组不直接维护 BGP session FSM，不直接处理 TCP、OPEN、KEEPALIVE 或 UPDATE 报文。

`midr_context` 是 MIDR 模块上下文句柄，对第一组表现为 opaque handle。第一组通过 `midr_context_get_default()` 获取默认上下文，并把 `struct midr_context *` 传给接口函数即可，不需要知道结构体内部字段。

### 3.1 函数原型

```c
struct midr_context;

struct midr_context *midr_context_get_default(void);

int midr_peer_session_request(struct midr_context *ctx,
                              const struct midr_peer_session_request_info *req);

int midr_peer_session_release(struct midr_context *ctx,
                              const union sockunion *remote_address,
                              afi_t afi,
                              safi_t safi,
                              enum midr_peer_release_reason reason);
```

`midr_peer_session_request()` 返回成功不表示 BGP session 已经 Established，只表示本端建邻请求已被接收并交给 `bgpd` peer/session 机制处理。

`midr_peer_session_release()` 用于拆除本端 peer 的指定 AFI/SAFI，必要时删除 peer。优雅下线流程中，第一组应先上报 topology withdraw，再释放 peer session。

### 3.2 请求字段

```c
struct midr_peer_session_request_info {
    union sockunion remote_address;
    as_t remote_as;
    afi_t afi;
    safi_t safi;
    bool has_update_source;
    union sockunion update_source;
    uint8_t ebgp_multihop;
    const char *password;
    uint64_t policy_tags;
};
```

| 字段 | 必要性 | 说明 |
|---|---|---|
| `remote_address` | 必需 | 远端 BGP peer 地址 |
| `remote_as` | 必需 | 远端 AS 号 |
| `afi` / `safi` | 必需 | 需要激活的 AFI/SAFI，例如 IPv4 unicast、IPv6 unicast、BGP-LS 或 MIDR 自定义地址族 |
| `update_source` | 可选 | 本端 session 源地址或接口 |
| `ebgp_multihop` | 可选 | eBGP multihop TTL；`0` 表示不设置 |
| `password` | 可选 | BGP session 认证信息 |
| `policy_tags` | 可选 | 建邻策略标签，供 `midrd` 配置或校验使用 |

### 3.3 幂等语义

`midr_peer_session_request()` 定义为幂等接口：

```text
peer 不存在：
  创建 peer，设置 remote AS，激活 AFI/SAFI，返回 0

peer 已存在且配置一致：
  不重复创建，返回 0

peer 已存在且 AFI/SAFI 已激活：
  返回 0

peer 已存在但 remote_as 或关键配置冲突：
  返回 -EEXIST 或 -EINVAL

peer 已存在但请求新增 AFI/SAFI：
  激活新增 AFI/SAFI，返回 0
```

第一组可以在认为需要建连时直接调用该接口，不需要在调用前自行维护复杂去重状态；第一组仍应对建连触发做频率控制，避免高频重复调用。

### 3.4 Established 状态获取

第一组如需监听 BGP session Established/Down，可直接注册 `bgpd` 的 `peer_status_changed` hook。FRR hook 支持多方注册，第一组和第二组可以同时监听该 hook。

第二组也会注册 `peer_status_changed` hook，用于自身 LS 对象 stale/aging、路径重算或状态同步。`peer_status_changed` 不是第一组向第二组上报 topology event 的接口。

## 4. Topology Event 接口

Topology event 用于第一组向第二组提交 MIDR 拓扑事实。所有 upsert / withdraw 事件采用异步入队语义：接口调用成功只表示事件已被第二组接收，不表示 TED 已完成更新、Link-State update 已完成通告或路径已完成重算。

Topology event 使用 `upsert + withdraw` 动作模型。新增和更新统一使用 upsert；删除、失效或撤销统一使用 withdraw。

每个对象使用 per-object `uint64_t version`。第二组按对象 key 和 version 过滤旧事件，旧版本事件不得覆盖新版本状态。

### 4.1 Node / Group 事件

```c
int midr_topology_node_upsert(struct midr_context *ctx,
                              const struct midr_node_update *node);

int midr_topology_node_withdraw(struct midr_context *ctx,
                                uint32_t node_id,
                                uint64_t version);
```

`node_id` 使用 BGP Router-ID 对应的 `uint32_t`。第一组不分配独立 node ID。

### 4.2 Link 事件

```c
int midr_topology_link_upsert(struct midr_context *ctx,
                              const struct midr_link_update *link);

int midr_topology_link_withdraw(struct midr_context *ctx,
                                const struct midr_link_key *key,
                                uint64_t version);
```

Link Object 按有向边上报。`R1 -> R2` 和 `R2 -> R1` 是两条不同的 directed link，可以拥有不同的 state、policy、measurement 和 local ifindex。

如果一条物理链路双向可用，第一组应分别上报两个方向的 Link Object。

### 4.3 Snapshot 接口

```c
int midr_topology_snapshot_get(struct midr_context *ctx,
                               struct midr_topology_snapshot *snapshot);

void midr_topology_snapshot_release(struct midr_context *ctx,
                                    struct midr_topology_snapshot *snapshot);
```

正式 snapshot 语义为：第二组从第一组或第一组 topology provider 拉取第一组本地拥有并上报的对象全量状态。snapshot 不包含远端节点、远端链路或第二组通过链路状态泛洪学习到的对象。

snapshot 至少包含：

```text
本节点当前有效 Node / Group Membership
本节点 originate 的当前有效 directed Link
每个对象当前 version
第一组本地持有的 measurement / policy / capability 当前值
```

第二组在启动、重启、重同步、检测到事件缺失或怀疑状态不一致时，可通过 snapshot 修复第一组到第二组之间的本地对象状态。

当前接口骨架中的 snapshot 可先返回第二组内部表用于调试；正式联调语义以第一组 provider snapshot 为准。

### 4.4 返回值语义

Topology event 接口返回值采用负 errno 风格：

| 返回值 | 含义 |
|---|---|
| `0` | 事件已被第二组接收并入队 |
| `-EINVAL` | 参数非法 |
| `-ENOENT` | `ctx` 或必要依赖不存在 |
| `-ENOMEM` | 内存不足，事件未入队 |
| `-ENOSPC` | 队列满，事件未入队 |
| `-EAGAIN` | 第二组暂时不可接收，调用方可稍后重试 |

如果返回非 `0`，第一组应认为事件未被第二组接收。第一组可重试，或等待后续 snapshot 重同步修复状态。

### 4.5 Version 语义与重同步

`version` 使用 `uint64_t` 单调递增计数，由对象 owner 维护。对同一对象 key，新的 upsert / withdraw 事件必须使用大于当前版本的 `version`。`version` 不允许回退，不允许复用。

第二组按对象 key 记录当前最新 `version`。当收到 `version <= current_version` 的事件时，第二组应将该事件视为旧事件并忽略，避免乱序事件覆盖新状态。

v1 不处理 `uint64_t version` wrap-around。正常更新频率下，`uint64_t` 计数空间足够大；如果实现检测到某个对象的 `version` 接近 `UINT64_MAX`，应触发本地重同步或重新生成对象 key，而不是继续递增到回绕。

第一组重启、状态丢失或无法延续旧 `version` 时，不应直接从较小 `version` 继续发送增量事件。此类场景应通过 snapshot 重同步修复：

```text
第一组重启或 version 无法延续
  -> 第二组清理该 owner 的本地自有对象旧状态
  -> 第二组调用 topology snapshot provider
  -> 第一组返回当前有效的本地 Node / Link 全量状态
  -> 第二组以 snapshot 为新的基线恢复对象状态
  -> 后续增量事件在新基线上继续单调递增
```

## 5. 数据对象字段

### 5.1 通用枚举

```c
enum midr_policy_state {
    MIDR_POLICY_ALLOWED,
    MIDR_POLICY_BLOCKED,
    MIDR_POLICY_DEGRADED,
};

enum midr_link_state {
    MIDR_LINK_UP,
    MIDR_LINK_DOWN,
    MIDR_LINK_DEGRADED,
    MIDR_LINK_UNKNOWN,
};

enum midr_peer_release_reason {
    MIDR_PEER_RELEASE_ADMIN,
    MIDR_PEER_RELEASE_NODE_DOWN,
    MIDR_PEER_RELEASE_POLICY,
};
```

语义如下：

| 状态 | 含义 |
|---|---|
| `ALLOWED` / `UP` | 对象可进入第二组链路状态视图，并可参与路径计算 |
| `BLOCKED` / `DOWN` | 对象不应继续参与路径计算，第二组可生成 withdraw 或标记失效 |
| `DEGRADED` | 对象仍可保留，但第二组可提高内部路径代价或降低路径优先级 |
| `UNKNOWN` | 链路状态不确定，v1 默认不参与路径计算 |

v1 中，第一组如未实现复杂策略判断，可默认填 `MIDR_POLICY_ALLOWED`。

### 5.2 Node / Group Membership

接口主键采用 `node_id = BGP Router-ID`。第一组不分配独立 node ID，也不使用 peer/session 标识替代稳定节点身份。

```c
struct midr_node_update {
    uint32_t node_id;
    uint32_t group_id;
    bool has_transport_address;
    union g_addr transport_address;
    uint64_t cap_flags;
    enum midr_policy_state policy_state;
    uint64_t policy_tags;
    uint64_t version;
};
```

字段说明：

| 字段 | 说明 |
|---|---|
| `node_id` | BGP Router-ID 对应的 `uint32_t` |
| `group_id` | 第一组确定的 group 编号，类型为 `uint32_t` |
| `has_transport_address` | `transport_address` 是否有效 |
| `transport_address` | 用于建连、反向建连、多跳 eBGP 或探测寻址的地址，通常为 loopback/transport 地址 |
| `cap_flags` | MIDR 数据面能力位图，例如转发能力、SRv6、封装能力、边缘/中继角色 |
| `policy_state` | 节点是否允许参与 MIDR |
| `policy_tags` | 节点策略标签，供第二组路径计算或过滤使用 |
| `version` | 该 Node / Group Membership 对象的单调版本号 |

`transport_address` 变化视为 node update，需要递增 `version`。`has_transport_address == false` 时，第二组不得使用 `transport_address` 内容；全零地址可作为未知地址的兼容表达，但正式判断以 `has_transport_address` 为准。

`cap_flags` 位含义由第一组定义，并应在接口文档附录维护。未分配 bit 必须为 `0`；第二组收到未知 bit 时应原样保存和透传，不得清零。

### 5.3 Link Key

```c
struct midr_link_key {
    uint32_t local_node_id;
    uint32_t remote_node_id;
    uint64_t link_id;
};
```

字段说明：

| 字段 | 说明 |
|---|---|
| `local_node_id` | 有向链路本端节点，使用 BGP Router-ID |
| `remote_node_id` | 有向链路对端节点，使用 BGP Router-ID |
| `link_id` | 第一组生成的稳定 opaque ID，作用域为同一对 `local_node_id -> remote_node_id` |

`link_id` 用于区分同一对节点之间的平行链路。v1 如果每对节点只有一条逻辑链路，第一组可以固定填 `0`。后续支持多链路时，第一组必须保证同一对有向节点下的 `link_id` 稳定。

### 5.4 Link Measurement

```c
struct midr_link_metrics {
    bool has_rtt_us;
    uint32_t rtt_us;

    bool has_loss_ppm;
    uint32_t loss_ppm;

    bool has_bandwidth_score;
    uint32_t bandwidth_score;

    uint64_t measurement_seqno;
    uint64_t measurement_timestamp_ms;
};
```

字段说明：

| 字段 | 说明 |
|---|---|
| `rtt_us` | RTT，单位为 microsecond |
| `loss_ppm` | 丢包率，单位为 parts per million |
| `bandwidth_score` | 第一组性能测量模块输出的带宽分数 |
| `measurement_seqno` | 测量序号，用于过滤乱序测量结果 |
| `measurement_timestamp_ms` | 测量时间戳，单位为 millisecond |

第一组负责产生原始测量值，并在上报前进行去抖、阈值过滤和低频兜底上报。第二组负责根据原始测量值生成链路状态属性，并按路径计算需求统一折算 SPF/CSPF 使用的内部路径代价。

### 5.5 Link Attributes

```c
struct midr_link_update {
    struct midr_link_key key;
    enum midr_link_state link_state;
    uint32_t local_ifindex;
    union g_addr link_local_address;
    union g_addr link_remote_address;
    struct midr_link_metrics metrics;
    enum midr_policy_state policy_state;
    uint64_t policy_tags;
    uint64_t version;
};
```

字段说明：

| 字段 | 必要性 | 说明 |
|---|---|---|
| `key` | 必需 | 有向链路唯一键 |
| `link_state` | 必需 | 链路状态 |
| `local_ifindex` | 建议提供 | 本地出接口；`0` 表示后续交给 zebra 根据 nexthop 解析 |
| `link_local_address` | 条件必需 | 本端链路地址或 overlay endpoint，用于 Link NLRI descriptor、路径安装或调试 |
| `link_remote_address` | 条件必需 | 对端链路地址或 overlay endpoint |
| `metrics` | 建议提供 | RTT、丢包率、带宽分数、seqno、timestamp 等原始测量信息 |
| `policy_state` | 必需 | 链路是否允许进入 MIDR 链路状态视图 |
| `policy_tags` | 可选 | 链路策略标签 |
| `version` | 必需 | 该 Link Object 的单调版本号 |

overlay 多跳链路 v1 约定：

```text
local_ifindex = 0
link_local_address = local transport address
link_remote_address = remote transport address
```

物理直连链路 v1 暂不建模；后续如需建模，应填真实 `local_ifindex` 和接口地址。

### 5.6 Topology Snapshot

```c
struct midr_topology_snapshot {
    const struct midr_node_update *nodes;
    size_t node_count;

    const struct midr_link_update *links;
    size_t link_count;

    uint64_t snapshot_version;
};
```

字段说明：

| 字段 | 说明 |
|---|---|
| `nodes` | 第一组本地拥有并上报的有效 Node / Group Membership 对象数组；当 `node_count == 0` 时可以为 `NULL` |
| `node_count` | `nodes` 数组元素数量 |
| `links` | 第一组本地拥有并上报的有效 directed Link 对象数组；当 `link_count == 0` 时可以为 `NULL` |
| `link_count` | `links` 数组元素数量 |
| `snapshot_version` | snapshot 整体版本，用于调试、重同步和一致性检查；不替代每个对象自己的 `version` |

`nodes` 和 `links` 指向的数组对第二组只读。第二组不得修改数组内容，也不得长期保存数组指针。第二组应在消费 snapshot 后把需要的对象复制到自身 node table、link table、TED 或 LSDB 中，然后调用 `midr_topology_snapshot_release()`。

## 6. 第二组到第一组的只读远端视图接口

第二组负责解析远端 Link-State NLRI 并维护 LSDB/TED。第一组如果需要基于全网 node/group/capability 信息执行 CL 分群或代表选择，可通过第二组提供的只读远端视图接口获取数据。

第一组本地天然拥有的信息包括：

```text
本节点身份
本节点 transport address
本节点 capability
本节点 group
本节点出向链路和测量值
```

第一组本地不天然拥有、但第二组会通过链路状态传播学习的信息包括：

```text
非邻接远端节点的 group/capability/transport 摘要
远端节点 originate 的 Link-State 对象
Group Prefix Reachability
```

v1 优先采用查询式只读接口；如果第一组需要实时增量通知，可再注册可选 callback。

### 6.1 查询式接口

```c
struct midr_remote_node_info {
    uint32_t node_id;
    uint32_t group_id;
    bool has_transport_address;
    union g_addr transport_address;
    uint64_t cap_flags;
    enum midr_policy_state policy_state;
    uint64_t policy_tags;
    uint64_t version;
};

struct midr_remote_link_info {
    struct midr_link_key key;
    enum midr_link_state link_state;
    union g_addr link_local_address;
    union g_addr link_remote_address;
    struct midr_link_metrics metrics;
    enum midr_policy_state policy_state;
    uint64_t policy_tags;
    uint64_t version;
};

struct midr_remote_view_snapshot {
    const struct midr_remote_node_info *nodes;
    size_t node_count;

    const struct midr_remote_link_info *links;
    size_t link_count;

    uint64_t snapshot_version;
};

int midr_remote_view_snapshot_get(struct midr_context *ctx,
                                  struct midr_remote_view_snapshot *snapshot);

void midr_remote_view_snapshot_release(struct midr_context *ctx,
                                       struct midr_remote_view_snapshot *snapshot);
```

该接口由第一组调用，第二组实现。返回内容为第二组当前 LSDB/TED 中可向第一组暴露的远端 node/link 摘要，不允许第一组通过该接口修改第二组状态。

### 6.2 可选回调接口

```c
struct midr_remote_view_callbacks {
    void (*remote_node_update)(const struct midr_remote_node_info *node);
    void (*remote_node_withdraw)(uint32_t node_id, uint64_t version);
    void (*remote_link_update)(const struct midr_remote_link_info *link);
    void (*remote_link_withdraw)(const struct midr_link_key *key,
                                 uint64_t version);
};

int midr_remote_view_callbacks_register(
    struct midr_context *ctx,
    const struct midr_remote_view_callbacks *callbacks);
```

回调语义为只读通知：第二组在解析远端 Link-State 对象并更新本地 LSDB/TED 后，可通知第一组相关远端摘要变化。第一组不得在回调中直接修改第二组 LSDB/TED。

## 7. 第二组从 bgpd 获取的信息

第二组直接复用 `bgpd` 的 session、RIB、best-path 和 hook 能力，不要求第一组重复提供。

| 信息 | bgpd 来源 | 第二组用途 |
|---|---|---|
| 本地 Router-ID | `bgp->router_id` | 本地 `node_id` |
| 远端 Router-ID | `peer->remote_id` | 远端 `node_id` |
| peer/session 状态 | `struct peer`、`peer_status_changed` hook | 判断 session up/down，触发对象 stale/aging 或路径重算 |
| AFI/SAFI capability | `struct peer` | 判断地址族是否可用 |
| BGP RIB route | `bgp->rib[afi][safi]`、`struct bgp_dest`、`struct bgp_path_info` | 读取 prefix route |
| best-path/multipath | `BGP_PATH_SELECTED`、`BGP_PATH_MULTIPATH` | 同一 prefix 多路径时复用 bgpd 选路结果 |
| route update | `bgp_route_update` hook | 监听 RIB / best-path 变化 |

Prefix 获取流程如下：

```text
BGP UPDATE
  -> bgp_update_receive()
  -> bgp_attr_parse()
  -> bgp_nlri_parse()
  -> bgp_process()
  -> bgp_best_selection()
  -> bgp_route_update hook
  -> 第二组读取 selected peer / nexthop / prefix
  -> 结合 node-to-group 生成 Group Prefix Reachability
```

`peer_status_changed` 和 `bgp_route_update` 只用于监听 `bgpd` 内部状态变化，不作为第一组向第二组上报 topology event 的接口。

## 8. 链路状态传播范围

MIDR 链路状态传播采用“群内细节 + 全局骨架”的范围控制原则。

| 对象 | 含义 | 传播范围 |
|---|---|---|
| Group Membership TLV | 描述节点所属 group、transport address、capability 等摘要 | 全局通告 |
| Link NLRI | 描述 router-level directed link | 群内链路仅群内通告；群间链路全局通告 |
| Prefix NLRI | 描述节点可达 prefix | 群内通告 |
| Group Prefix NLRI | 描述 group 可达 prefix 摘要 | 全局通告 |

因此，每个节点最终应能收到全网 Group Membership 摘要，但不保证收到全网所有群内 Link NLRI 或普通 Prefix NLRI。全局可见的是节点 group/能力/transport 摘要、群间连接骨架和 group 到 prefix 的可达摘要；群内细粒度 router-level topology 仅在本 group 内传播。

Link Object 分类规则：

```text
local_group == remote_group
  -> intra-group link
  -> scope = intra_group
  -> 仅本 group 内通告

local_group != remote_group
  -> inter-group link
  -> scope = global
  -> 全局通告
```

Group Prefix Object 以 `(group_id, afi, prefix)` 为逻辑 key，避免按边缘节点或 inter-group link 重复生成多份逻辑对象。v1 不引入 DR 设计；如存在多个 originator 通告同一 group-prefix，第二组应通过 BGP 路由选择、对象去重或后续策略控制处理。

## 9. 失联、老化与撤销语义

### 9.1 优雅下线

节点优雅下线时，应由该节点第一组主动上报：

```text
withdraw 本节点 Node / Group Membership
withdraw 本节点出向 Link
必要时调用 midr_peer_session_release()
```

第二组收到 withdraw 后，更新本地 node/link table、LSDB/TED，并生成对应 Link-State withdraw。

### 9.2 非优雅失联

非优雅失联包括进程崩溃、机器断电、链路断开、网络分区或对端不可达。失联节点无法主动发送 withdraw。

在非优雅失联场景下，邻接节点只负责撤销自己拥有的本地对象，例如：

```text
R1 检测到 R2 不可达
  -> R1 withdraw R1 -> R2 出向 link
  -> R1 不替 R2 withdraw R2 的 node object
  -> R1 不替 R2 withdraw R2 -> R1 或 R2 -> R3
```

远端节点 originate 的 Node/Link/Prefix 对象由第二组根据 session down、LSDB aging、hold timer 或 route withdraw 处理。

推荐状态转换：

```text
active
  -> session down 或 originator 不可达
  -> stale
  -> aging timer 到期仍未恢复
  -> expired / withdrawn
```

`stale` 状态下，对象可暂时不参与路径计算，或按降级策略处理，避免短暂 session 抖动导致 TED 大规模震荡。

### 9.3 会话断开窗口期

`peer_status_changed` 可能早于第一组 PM 模块的 `LINK_DOWN` 事件触发。v1 采用保守处理：

```text
BGP session down：
  第二组将依赖该 peer/originator 的远端对象标记 stale，不立即彻底删除

第一组 LINK_DOWN / link withdraw 到达：
  第二组正式撤销本地出向 link

stale 超时仍未恢复：
  第二组清理远端 originate 对象或生成 withdraw
```

## 10. 调用流程

建连流程：

```text
第一组发现需要建立 MIDR BGP peer
  -> 本端调用 midr_peer_session_request()
  -> 第一组 NDS 机制确保对端也触发建连请求
  -> 对端调用 midr_peer_session_request()
  -> 两端 midrd/bgpd 创建或更新 peer 配置
  -> bgpd FSM 完成 TCP / OPEN / KEEPALIVE
  -> session Established
  -> 远端 Router-ID 可用
```

本地 topology 上报流程：

```text
第一组初始化本地 node/group/capability/transport
  -> 调用 midr_topology_node_upsert()

第一组发现或更新本地出向 link
  -> 调用 midr_topology_link_upsert()

第二组异步消费 topology event
  -> 更新 router-to-group / link table
  -> 派生 Link-State 对象与传播 scope
  -> 更新 LSDB / TED
  -> 判断是否生成 Link-State update / withdraw
  -> 触发链路状态传播和路径重算
```

第二组启动或重同步流程：

```text
第二组初始化
  -> 调用 topology snapshot provider
  -> 加载第一组本地拥有并上报的 Node / Link 状态
  -> 建立 router-to-group / link table / TED 初始状态
  -> 开始消费后续增量 upsert / withdraw 事件
```

第二组到第一组远端视图流程：

```text
第二组解析远端 Link-State NLRI
  -> 更新 LSDB / TED
  -> 第一组按需查询 remote view
  -> 或第二组通过可选 callback 通知第一组远端摘要变化
```

## 11. 函数归属

| 函数 | 调用方 | 实现方 | 说明 |
|---|---|---|---|
| `midr_context_get_default()` | 第一组 / 第二组 | 第二组 | 获取 MIDR opaque context |
| `midr_peer_session_request()` | 第一组 | 第二组 | 本端 peer/session 建立请求 |
| `midr_peer_session_release()` | 第一组 | 第二组 | 本端 peer/session 释放请求 |
| `midr_topology_node_upsert/withdraw()` | 第一组 | 第二组 | 本节点 Node / Group Membership 上报 |
| `midr_topology_link_upsert/withdraw()` | 第一组 | 第二组 | 本节点出向 Link 上报 |
| `midr_topology_snapshot_get/release()` | 第二组 | 第一组 provider | 第二组拉取第一组本地自有对象全量 |
| `midr_remote_view_snapshot_get/release()` | 第一组 | 第二组 | 第一组读取第二组远端 LS 只读视图 |
| `midr_remote_view_callbacks_register()` | 第一组注册，第二组调用 | 第二组提供注册点，第一组提供回调函数 | 可选增量通知接口 |

## 12. 接口约束

第一组必须保证：

```text
node_id 使用 BGP Router-ID
group_id 使用 uint32_t
link_id 使用稳定 uint64_t opaque ID
Link Object 按有向边上报
本地 Node 可以在本地初始化后上报
依赖远端 Router-ID 的 event 只在 session Established 后上报
每个对象 version 单调递增
version 不回退、不复用；无法延续时通过 snapshot 重同步
snapshot 反映第一组本地拥有并上报的当前有效状态
measurement 上报前完成去抖和低频兜底
```

第一组不应执行以下操作：

```text
直接写第二组 TED / LSDB
直接生成最终 Link-State Link NLRI 或 Group Prefix NLRI
向第二组传递普通 prefix NLRI
向第二组传递 bgpd 已经完成解析的 BGP attribute 基础信息
替第二组选择 best path
替远端节点代报或撤销远端 owned Node/Link 对象
把 peer_status_changed / bgp_route_update 当作 topology 上报接口
```

第二组必须保证：

```text
按对象 key 和 version 过滤旧事件
异步处理 topology event
从 bgpd hook 获取 session / RIB / route 变化
从 bgpd RIB 推导 Prefix Reachability
根据 Node / Link 变化决定 Link-State object / update / withdraw
根据传播范围控制 Link-State 对象泛洪范围
根据 session down / LSDB aging 清理非优雅失联节点的远端对象
根据 TED / LSDB 变化触发路径重算
```

## 13. 测量去抖与策略默认值

第一组负责对测量结果进行去抖、阈值过滤和低频兜底上报。第二组默认收到的 measurement event 已经是值得进入链路状态传播或路径重算流程的变化。

联调前第一组应给出以下参数：

```text
RTT 变化阈值
loss 变化阈值
bandwidth_score 变化阈值
最小上报间隔
最大兜底上报间隔
```

`policy_state` v1 默认值为 `MIDR_POLICY_ALLOWED`。如果第一组已有策略判断结果，可以填 `MIDR_POLICY_BLOCKED` 或 `MIDR_POLICY_DEGRADED`。第二组消费该字段并决定对象是否进入链路状态视图或路径计算。

## 附录 A. `cap_flags` 能力位

`cap_flags` 为 `uint64_t` 位图。能力位含义由第一组维护；第一组已有 `uint32_t` 能力位可放在低 32 位。未分配 bit 必须为 `0`，第二组收到未知 bit 时应原样保存和透传，不得清零。

| bit 范围 | 维护方 | 约定 |
|---|---|---|
| `0-31` | 第一组 | 兼容第一组已有 `uint32_t` 能力位 |
| `32-63` | 第一组 | 预留扩展能力位 |
