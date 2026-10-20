# MIDR 物理链路与虚链路接入实施交接说明

> 文档用途：交给第一组和第三组，作为虚链路接入的代码实施说明。
>
> 文档状态：实现交接草案，不替代最终正式接口冻结文档。
>
> 当前代码基线：分支 `feat/midr-three-way-integration`，HEAD
> `443c51474b827a8851aa8a0039c61b65e43db080`。

## 1. 要做什么

MIDR 路由器可能并不连续部署在每一段底层网络上。两个 MIDR 节点之间可能隔着只运行 BGP、但不运行 MIDR 的中间区域。现有 L3 IP 链路及 BGP 可达性属于 underlay；第一组若决定在两个 MIDR 节点之间建立 overlay 邻接，则调用第三组的 GRE/ip6gre 接口构建虚拟设备，并在设备可用后向第二组上报一条逻辑 MIDR Link。中间 BGP-only 路由器不因转发隧道报文而成为 MIDR 节点。

两类 Link 的来源不同：

| MIDR Link | 来源 | 本机上报所用接口 |
| --- | --- | --- |
| 一跳 L3 直连的 MIDR 邻接 | 现存物理 IP 链路，经第一组发现或配置确认 | 物理接口 |
| 跨非 MIDR 区域的 overlay 邻接 | 第一组根据部署策略发起，调用第三组 API 建立隧道 | GRE/ip6gre 虚接口 |

“underlay 存在”不等于“应向 MIDR 上报 Link”：连接到 BGP-only 节点的物理链路只提供底层可达性，不是两个 MIDR 节点之间的一跳 MIDR Link。虚链路建成后，目标处理链为：

```text
底层 BGP 区域提供外层 IP 可达性
        |
        v
GRE/ip6gre 或其他虚拟接口
        |
        v
隧道内部 overlay 地址
        |
        v
MIDR TCP Session / HELLO / Snapshot / EOR
        |
        v
MIDR Link -> TED -> SPF -> Zebra -> Linux FIB
```

本任务不是单纯创建一个 GRE netdevice，而是把“物理链路”和“虚链路”都接入第一组的 Link 上报、MIDR Session、第二组 SPF 和第三组 Zebra/FIB 生命周期。

最终要求：

1. 第一组仅对满足 MIDR 邻接条件的一跳物理链路上报真实物理接口和链路地址，不把每段 underlay 都上报为 MIDR Link。
2. 第一组发起 overlay 建立并调用第三组虚链路接口；设备可用后上报真实 GRE 虚接口和隧道内部 overlay 地址。
3. 若 Session 设计为运行在隧道内，必须验证监听、源地址和到远端地址的实际选路，不能仅凭 `midr_session_connect()` 的返回值断定流量走隧道。
4. SPF/Zebra 使用正确的本地接口索引安装路由。
5. 链路创建、建立、故障、撤销、恢复和 ifindex 变化具有完整生命周期语义。

## 2. 链路类型的来源与当前代码状态

从职责上，第一组能够区分两类链路：一跳物理 MIDR 邻接来自现存接口的发现或配置；虚拟邻接由第一组主动发起并调用第三组接口构建。**但现行代码尚未把这个来源关系保存为可用于 Link 上报的物理/虚拟分类。**不能把设计上的可区分性误认为已经实现。

现有第二组公开 Link 输入是：

```c
struct midr_link_update {
	struct midr_link_key key;
	ifindex_t local_ifindex;
	struct ipaddr link_local_address;
	struct ipaddr link_remote_address;
	struct midr_link_metrics metrics;
	enum midr_policy_state policy_state;
	uint64_t policy_tags;
	uint64_t version;
};
```

定义位置：

```text
midrd/midr-topology.h
```

这个结构没有以下信息：

```text
physical / virtual 类型
物理接口名称
GRE 隧道类型
GRE 外层 local/remote
overlay 地址分配信息
```

第一组自己的 `struct midr_link_entry` 也没有链路类型或 `ifindex` 字段，位置为：

```text
midrd/group1/midr_nds.h
```

而且当前第一组在生成 Link Fact 时明确把 `local_ifindex` 设为 `0`，位置为：

```text
midrd/group1/midr_nds_facts.c
```

目前也没有第一组调用 `midr_gre_interface_add()` 的生产路径。因此虚链路的“谁发起、对应哪条 MIDR Link”还没有落入当前事实表。

### 2.1 不能依靠推断判断链路类型

以下信息都不足以可靠判断链路类型：

- 两个节点的 transport 地址；
- BGP 是否能够到达对端；
- TCP 是否经过中间路由器；
- `local_ifindex` 是否非零；
- 本端看到的下一跳地址。

例如，经过 BGP 区域递归可达的普通 TCP 连接可能没有 GRE；GRE 外层地址也可能由 BGP 提供可达性。第一组应以一跳物理邻接的发现/配置结果，或自己发起并成功关联的虚链路创建记录作为分类依据，而不是从路由结果反推。

### 2.2 链路类型必须有明确来源

第一阶段可用**显式链路描述/配置**记录来源：

```text
Link descriptor
  - stable link_id
  - kind = PHYSICAL 或 VIRTUAL
  - remote node
  - physical interface 信息，或
  - virtual tunnel 信息
```

第一组负责维护这个 descriptor，并关联物理接口或本组发起的第三组隧道请求。后续若 NDS 能明确发现一跳物理邻接，也可按相同结构保存发现结果；不能仅由“远端可达”产生物理 MIDR Link，也不能让第二组根据地址猜测链路类型。

链路类型和接口名称首先属于本地控制面 metadata，不应直接塞进远端传播的 MIDR LS Object。第二组真正需要的结果仍然是：

```text
稳定的 link_id
本地实际 ifindex
链路两端用于 MIDR/SPF 的地址
metric 和 measurement
version
```

## 3. 当前代码已经具备的能力

### 3.1 Link 已经预留接口索引

`local_ifindex` 已经从第一组 Link 输入进入本地事实、TED、SPF nexthop 和 Zebra：

```text
midr-topology.h
midr-local-provider.h
midr-ted.h
midr-spf.h
midr-spf-install.c
midr-dp-backend.c
```

它是**本地接口索引**，不是远端需要传播的接口索引。只有产生该 Link 的节点才能把它用于本机 FIB 安装。

### 3.2 GRE 创建接口已经存在

第三组已提供：

```c
midr_gre_interface_add()
midr_gre_interface_del()
midr_gre_interface_get_state()
midr_gre_interface_wait_up()
midr_gre_register_notify()
```

头文件：

```text
midrd/midr-gre.h
```

已有调用链：

```text
midrd GRE API
  -> zclient_send_gre_add()
  -> ZEBRA_GRE_ADD
  -> zebra data plane
  -> netlink
  -> gre/ip6gre netdevice
```

已有测试：

```text
midrd/midr-gre-connectivity-test.sh
midrd/gre-link-tool.c
```

### 3.3 当前能力的边界

GRE API 当前主要负责：

- 创建 GRE/ip6gre 设备；
- 删除设备；
- 等待 Zebra 看到设备；
- 返回设备注册状态和 ifindex。

但当前生产流程没有把 GRE 创建接入第一组 Link 和 Session。`midrd.c` 只在启动时调用 `midr_gre_init()`，不会根据 Link 自动调用 `midr_gre_interface_add()`。

另外，当前 GRE API 不负责：

- 配置隧道内部 overlay 地址；
- 把 GRE 接口置为 UP；
- 配置 overlay 路由；
- 自动建立 MIDR Session；
- 自动生成和上报 `midr_link_update`。

`midr_gre_status.state == UP` 表示 Zebra 已识别 netdevice，不等于内核接口已被置为 administratively UP，也不等于 overlay 对端可达。现有 GRE 测试仍通过外部命令配置地址和接口状态。因此 GRE netdevice 存在不等于 MIDR 虚链路已经可用。

## 4. 链路字段的正确语义

### 4.1 物理链路

当两个 MIDR 节点通过一跳 L3 物理链路直接相邻，且第一组已确认其为 MIDR 邻接时，Link 上报填写：

```text
local_ifindex       = 本端物理接口 ifindex
link_local_address  = 本端物理链路地址
link_remote_address = 对端物理链路地址
```

例如：

```text
接口：eth1
ifindex：5
本端：2001:db8:1::1
对端：2001:db8:1::2
```

此例中，`local_ifindex` 为 5，两端地址分别为 `2001:db8:1::1` 和 `2001:db8:1::2`。如果物理链路的对端只是 BGP-only 路由器，该链路属于 underlay，不应直接作为 MIDR-A 到 MIDR-B 的 Link 上报。

### 4.2 虚链路

虚链路必须区分外层 tunnel endpoint 和内层 MIDR Link 地址。

例如：

```text
GRE 外层：
  local  = 10.1.1.1
  remote = 10.9.9.9

GRE 内层：
  interface = midr-gre-1
  ifindex   = 27
  local     = 192.0.2.1
  remote    = 192.0.2.2
```

此例中，第一组确认虚链路可用后上报：

```text
local_ifindex       = 27
link_local_address  = 192.0.2.1
link_remote_address = 192.0.2.2
```

不能把 GRE 外层 `10.1.1.1/10.9.9.9` 当作 MIDR Link 的下一跳地址。SPF/Zebra 需要使用隧道内部对端地址和 GRE 的本地 ifindex。

## 5. 虚链路目标流程与现有 Session 限制

```text
第一组根据部署配置/策略确定 overlay 邻接，并保存 Link descriptor
        |
        v
确认 GRE 外层 endpoint 通过 BGP/underlay 可达
        |
        v
第一组调用第三组 API；第三组创建 GRE/ip6gre
        |
        v
等待 Zebra 报告设备存在，并获得 ifindex
        |
        v
配置隧道内部 overlay 地址、将接口置 UP，验证双向可达
        |
        v
确认 MIDR 监听/源地址和到 overlay remote 的选路，再建立 Session
        |
        v
等待 HELLO 身份确认和 Session Established
        |
        v
第一组调用 midr_topology_link_upsert()
        |
        v
第二组 TED/SPF 使用 overlay 地址和 GRE ifindex
        |
        v
第三组通过 ZAPI 将路由安装到 Linux FIB
```

当前 `midr_session_connect()` 只接收远端 endpoint，没有指定本地出接口或源地址的参数；`midr-transport.c` 建立出站连接时会绑定 `midrd` 的本地监听地址。因此，**仅把远端 overlay 地址传给 Session API，不足以保证 Session 流量走 GRE**。一跳物理邻接和虚链路共存时，还需核对监听地址、源地址绑定、内核到 overlay 地址的路由和对端准入规则；若当前单一监听地址无法满足两类邻接，需先联合确定并实现 Session 接口/运行时调整。

第一组不得仅因 GRE `state == UP` 就上报 ACTIVE Link。至少需确认设备已置 UP、overlay 地址及双向可达性已具备，并按现行 Link 上报约束确认 MIDR Session 身份及状态。

## 6. 第一组需要实现的内容

### 6.1 增加本地 Link descriptor

第一组需要有一个本地链路描述，记录一跳物理邻接的发现/配置来源，或由第一组发起的 overlay 创建意图及第三组返回的设备状态，至少表达：

```c
enum midr_link_kind {
	MIDR_LINK_PHYSICAL,
	MIDR_LINK_VIRTUAL,
};

struct midr_local_link_descriptor {
	enum midr_link_kind kind;
	uint64_t link_id;
	uint32_t remote_node_id;
	/* physical interface or virtual-link manager data */
};
```

具体公开 API 需要和第三组共同确认。第一阶段可以先作为第一组内部 metadata，不必立即加入 MIDR wire 或远端 LS Object。

### 6.2 物理链路分支

物理链路流程负责：

1. 确认两个 MIDR 节点在现存 L3 链路上确实一跳相邻，而非仅通过 BGP 多跳可达；
2. 取得物理接口 ifindex 和本端/对端链路地址；
3. 调用 `midr_session_connect()`；
4. Session Established 后调用 `midr_topology_link_upsert()`；
5. ifindex、地址或链路状态发生变化时按 version 规则更新或撤销。

### 6.3 虚链路分支

虚链路流程负责：

1. 根据配置/策略确定需要连接的 MIDR 对端，保存稳定的虚链路 descriptor；
2. 以该 descriptor 调用第三组 GRE/ip6gre 接口，跟踪请求和设备生命周期；
3. 等待设备被 Zebra 识别并取得有效 ifindex，再确认接口已置 UP、overlay 地址及对端可达性；
4. 验证 MIDR Session 的监听、出站源地址和路由实际使用 overlay，必要时与第二组调整 Session 能力；
5. 调用 `midr_session_connect()`，在身份确认并达到 Established 后上报 Link；
6. Link 上报使用 GRE ifindex 和 overlay 地址，而不是 GRE 外层地址。

第一组不应直接调用 raw netlink，也不应通过 shell 调用 `ip` 命令完成生产配置。应由第三组提供完成虚链路配置的 C API。

## 7. 第三组接口与分工边界

现有 `midr_gre_interface_add()` 只覆盖 GRE 设备创建/更新，不代表完整 overlay 可用。第一组负责决定**哪些 MIDR 节点之间需要虚链路**、发起调用并维护逻辑 Link 与 Session；第三组负责提供隧道设备创建、配置和状态能力。双方需要确定可调用的设备配置边界，覆盖：

```text
创建/删除 GRE 或 ip6gre
配置 overlay 地址
设置接口 UP/DOWN
返回 ifindex、接口状态和 overlay 地址
报告异步失败、删除和恢复
```

第三组可以扩展现有 GRE API，也可以增加更高层的设备配置 API。接口应分别表达“Zebra 已识别设备”和“接口已置 UP、地址已配置”的状态，不应沿用一个 `UP` 含混表示二者。第一组负责把第三组的设备状态与 Session 状态合成为 MIDR Link 的可上报状态；生产流程不应依赖测试脚本的 `ip` 命令。

设备建立过程至少需要能区分以下状态：

```text
PENDING
UP
FAILED
DOWN
```

第三组当前的 GRE `UP` 只覆盖设备存在和 ifindex。第一组上报 ACTIVE Link 还需合并第三组的设备/地址状态、自己的邻接检查和 Session 状态：

```text
GRE 设备存在
ifindex 有效
overlay 地址配置完成
接口已 UP
overlay remote 可达
MIDR Session 已完成 HELLO 身份确认
```

## 8. `link_id` 规则

当前第一组的 `midr_nds_facts_link_id()` 恒返回 `0`。这只适用于每对节点只有一条逻辑链路的旧假设。

引入并行链路时必须保证：

```text
同一条逻辑链路重启前后 link_id 不变；
物理链路和虚链路并存时 link_id 不冲突；
ifindex 变化不能导致 link_id 随机变化；
不能每次探测或重连都分配新 link_id。
```

需要确定的规则：

- 物理链路和虚链路同时存在时使用两个稳定的 `link_id`；
- 如果虚链路只是物理链路的替代实现，必须明确采用同一逻辑 Link 还是两个候选 Link；
- 稳定 ID 可来自配置或持久化分配器；不能只依赖每次进程启动重新从零开始的自增值。

## 9. 故障和恢复语义

### 9.1 链路失效

以下情况应使对应的本地 MIDR Link 失效，而不影响另一条仍正常的物理/虚拟并行 Link：

- 物理接口消失；
- GRE 进入 FAILED/DOWN；
- overlay 地址丢失；
- overlay remote 不可达；
- MIDR Session 断开。

处理时应及时：

```text
1. 阻止继续把失效设备当作 ACTIVE Link 上报；
2. 以更高 version 调用 midr_topology_link_withdraw()；
3. 按关联关系断开或重新建立对应 MIDR Session；
4. 更新本地接口/ifindex 映射，按需删除或重建虚链路。
```

撤销和 Session 断开应共同响应同一故障；不要求所有故障类型都先完成断连才允许撤销 Link。不能继续使用旧 ifindex，也不能用 `local_ifindex=0` 假装链路仍然有效。

### 9.2 链路恢复

恢复时：

```text
1. 重新创建物理接口或 GRE；
2. 重新获取当前 ifindex；
3. 对虚链路确认 overlay 地址、接口状态及 Session 实际选路；
4. 重新建立 MIDR Session；
5. 使用相同 link_id 和更高 version 重新 upsert；
6. 等待 EOR、TED/SPF 和 FIB 恢复。
```

ifindex 可以变化，但同一逻辑 Link 的 `link_id` 不应变化。

## 10. 不应加入远端 MIDR Link Object 的信息

以下信息属于本地实现细节，不应作为远端 LS Object 的语义字段传播：

```text
eth1 / midr-gre-1 等接口名称
本机 ifindex
GRE 外层 local/remote
本机内核接口类型
本地 Zebra registry 状态
```

远端只需要获得逻辑 Link 的：

```text
local_node_id
remote_node_id
link_id
链路两端 overlay/物理地址
metric 和 measurement
version
```

`local_ifindex` 只用于产生该 Link 的本机 TED/SPF/Zebra 安装，不应当把远端 ifindex 当成本机接口使用。

## 11. 验收要求

### 11.1 物理链路

- 仅一跳物理 MIDR 邻接形成 Link；BGP-only underlay 链路不会被误报；
- Link 上报包含真实物理 ifindex；
- TED 和 SPF 保留该 ifindex；
- Zebra 路由使用正确物理接口；
- `ip route get` 或 `ip -6 route get` 显示正确 `dev`。

### 11.2 虚链路

至少构造以下拓扑：

```text
MIDR-A ---- BGP-only 区域 ---- MIDR-B
```

检查：

- GRE/ip6gre 外层 endpoint 通过 BGP/underlay 可达；
- 第一组确实调用了第三组隧道 API，而非仅靠 BGP 多跳 TCP 连通；
- 两端虚接口既被 Zebra 识别，又在内核中置为 UP；
- 两端 overlay 地址可达；
- MIDR Session 的源地址、目的地址及实际路由均符合经 overlay 建邻的要求；
- HELLO、Snapshot、EOR 和 SPF 成功；
- Link 的 `local_ifindex` 等于 GRE 接口 ifindex；
- 业务路由的实际出口是 GRE 接口；
- 业务前缀双向 ping 成功。

### 11.3 故障恢复

- 删除 GRE 后 Session 变为 DOWN；
- Link 被 withdraw；
- 旧 FIB 路由撤销；
- GRE 重建后 ifindex 可以变化；
- `link_id` 保持不变；
- 使用更高 version 恢复 Link、Session、SPF 和 FIB。

### 11.4 并行链路

如果物理链路和虚链路同时存在：

- 两条链路的 `link_id` 不冲突；
- 分别能够建立和撤销；
- 撤销一条不影响另一条；
- SPF 能够按照各自 metric 选择路径。

## 12. 实施顺序

建议按以下依赖顺序推进：

```text
1. 第一组：固定一跳物理邻接与主动构建 overlay 的来源记录，避免把所有 underlay 误报为 MIDR Link；
2. 第一组：为一跳物理 Link 填写实际 local_ifindex，确定并行 Link 的稳定 link_id；
3. 第一组 + 第三组：确定 GRE 创建、overlay 地址配置、接口 UP 和状态回报的 API 边界；
4. 第一组 + 第二组：验证当前 Session 监听/源地址机制是否允许经隧道建邻，必要时调整；
5. 第一组：发起隧道、关联设备状态与 Session、上报或撤销虚 Link；
6. 第二组 + 第三组：核对 local_ifindex/overlay 下一跳从 Link 输入到 FIB 的实际路径；
7. 联合验收：物理邻接、跨 BGP-only 区域的 overlay、故障恢复、并行链路和 IPv4/IPv6。
```

当前代码只完成 GRE 设备创建基础和 Link 的本地 ifindex 传递；第一组尚未发起/管理虚链路，设备配置与 Session 隧道选路尚未闭环。因此不能仅凭已有 GRE API 声称完整的 MIDR 虚链路已实现。
