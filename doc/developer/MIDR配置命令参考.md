# MIDR 配置命令参考

本文档对应 `feat/pc-ls-propagation` 的 M6+M7 实现。命令分为持久化生产配置、当前联调操作命令、只读诊断和测试注入四类。只有写入 running-config 且已明确生产语义的命令属于持久化生产配置；除显式标注的测试入口外，生产部署不依赖手工 Node/Link 注入。

## 1. 持久化生产配置

### 1.1 Prefix AS_PATH 长度上限

CLI 节点：`BGP_IPV4_NODE` 或 `BGP_IPV6_NODE`

```text
midr prefix-export max-as-path-length (0-4294967295)
no midr prefix-export max-as-path-length [(0-4294967295)]
```

该命令设置当前 IPv4/IPv6 Unicast AF 可成为 Contributor 的最大 AS_PATH hop 数。默认值为 `1`：本地产生的 AS_PATH 长度 `0` 路由和一跳 BGP 路由可被接受，更长路径默认拒绝；需要引入更远的普通 BGP 可达性时可显式增大上限。`no` 命令恢复默认值 `1`，可选参数仅用于与配置语法对称，不改变恢复结果。

路径还必须为 valid、selected/multipath 且 non-stale；如配置了 route-map，还必须被 route-map permit。配置写入 running-config，非默认值才会显示；变化会重扫当前 Unicast RIB 并原子重建 Contributor Table。`ZEBRA_ROUTE_BGP_MIDR` 作为 MIDR 路由安装的独立 FRR route type 保留，但不参与本准入条件的硬过滤。

示例：

```text
router bgp 65000
 address-family ipv4 unicast
  midr prefix-export max-as-path-length 3
  no midr prefix-export max-as-path-length
```

当前阶段：可用于生产。

### 1.2 Prefix Export Route-map

CLI 节点：`BGP_IPV4_NODE` 或 `BGP_IPV6_NODE`

```text
midr prefix-export route-map WORD
no midr prefix-export route-map [WORD]
```

每个 IPv4/IPv6 Unicast AF 可独立配置一个 route-map 作为附加导出策略。未配置 route-map 时不增加策略过滤；配置后只有 route-map 返回 permit 的路径才能成为 Contributor，引用不存在的 route-map 按 deny 处理。Default route 不享受 route-map 例外：配置了 route-map 时也必须被显式 permit。配置写入 running-config；配置、撤销或被引用 route-map 的运行期变化会触发该 AF 的完整重评估。

示例：

```text
route-map EXPORT-MIDR-V4 permit 10
exit
router bgp 65000
 address-family ipv4 unicast
  midr prefix-export route-map EXPORT-MIDR-V4
  no midr prefix-export route-map
```

当前阶段：可用于生产。

### 1.3 Group Prefix 代表接管延迟

CLI 节点：`BGP_NODE`

```text
midr group-prefix takeover-delay-ms (0-600000)
no midr group-prefix takeover-delay-ms [(0-600000)]
```

单位为毫秒，默认值为 `3000`，`0` 表示不等待。候选代表变化后，旧本地代表立即撤销自己产生的 Group Prefix；新本地代表只有在延迟到期、Prefix baseline READY 且 EoR barrier 已完成或 timeout 后才提交代表状态并生成 Group Prefix。配置写入 running-config；`no` 命令恢复默认值。越界值被拒绝。

当前阶段：可用于生产。

### 1.4 EoR 超时

CLI 节点：`BGP_NODE`

```text
midr eor-timeout (1-3600)
no midr eor-timeout [(1-3600)]
```

单位为秒，默认值为 `60`。超时后 MIDR 可发布带 `EOR_TIMEOUT` reason 的完整可用视图；迟到 EoR 到达后 reason 自动清除。配置写入 running-config；`no` 命令恢复默认值。越界值被拒绝。

当前阶段：可用于生产。

## 2. 当前联调操作命令

### 2.1 MIDR Peer/Session

CLI 节点：`ENABLE_NODE`

```text
midr peer session A.B.C.D remote-as (1-4294967295) <ipv4-unicast|midr-link-state>
midr peer session A.B.C.D release <ipv4-unicast|midr-link-state>
```

`A.B.C.D` 是对端传输地址，`remote-as` 是对端 AS。`ipv4-unicast` 请求或释放普通 IPv4 Unicast AF，`midr-link-state` 请求或释放 MIDR Link-State AF。该命令是第一组 Session wrapper 的当前命令式入口，不写入 running-config，bgpd 重启后不会自动恢复，因此不能替代持久化生产配置。地址、AS 或 AF 非法时返回 warning；第一组尚不支持的可选 Session 参数不会被静默忽略。

示例：

```text
midr peer session 192.0.2.2 remote-as 65000 midr-link-state
midr peer session 192.0.2.2 release midr-link-state
```

当前阶段：MIDR wire pipeline 可用于联调；正式生产部署前仍需与第一组确认持久化 Session 配置形式。

## 3. 只读 Show 与诊断

以下命令位于 `VIEW_NODE`，不修改状态，也不写入 running-config。

```text
show midr prefix summary
show midr prefix contributors
show midr owned
show midr rib summary
show midr lsdb summary
show midr ted summary
show midr ted generation
show midr sync
show midr topology nodes
show midr topology links
show midr topology tombstones
show midr topology sync
show midr events
```

主要用途：

| 命令 | 输出 |
|---|---|
| `show midr prefix summary` | Prefix manager 状态、generation、Contributor 数量和拒绝计数 |
| `show midr prefix contributors` | 当前 active 的本地 Contributor Prefix |
| `show midr owned` | 本节点产生的四类对象、代表状态和 takeover timer |
| `show midr rib summary` | MIDR RIB identity/path/selected/conflict 计数 |
| `show midr lsdb summary` | selected object、usable/pending 和事务状态 |
| `show midr ted summary` | READY、generation 和六类 TED 数组数量 |
| `show midr ted generation` | 当前 immutable snapshot generation |
| `show midr sync` | Peer EoR barrier、等待和 timeout 状态 |
| `show midr topology nodes/links` | 第一组当前 active Local Fact |
| `show midr topology tombstones` | 本地撤销 key 与最后 input version |
| `show midr topology sync` | Snapshot/Resync Provider 和输入队列状态 |
| `show midr events` | 输入、拒绝、丢弃、Resync 和传播事件计数 |

TED 为 `NOT_READY` 时 `show midr ted summary` 仍能显示原因和 generation，但不会伪造可计算数组。

## 4. 仅测试注入

以下命令位于 `ENABLE_NODE`，用于在第一组真实 Provider 尚未接通时注入 Local Fact。它们不写入 running-config，不属于生产部署依赖。

```text
midr topology node upsert A.B.C.D group (0-4294967295) version WORD
midr topology node upsert A.B.C.D group (0-4294967295) transport <A.B.C.D|X:X::X:X> version WORD
midr topology node withdraw A.B.C.D version WORD
midr topology link upsert A.B.C.D A.B.C.D id WORD local-address <A.B.C.D|X:X::X:X> remote-address <A.B.C.D|X:X::X:X> rtt-us (0-4294967295) loss-ppm (0-4294967295) available-bandwidth-kbps (0-4294967295) version WORD [ifindex WORD] [seqno WORD] [timestamp-ms WORD]
midr topology link withdraw A.B.C.D A.B.C.D id WORD version WORD
```

Node 的 `A.B.C.D` 必须等于本地 Router-ID。Link 的第一个 Router-ID 必须是本地 Router-ID，第二个必须非零。Link endpoint 必须同时存在且地址族一致。`id`、`version`、`seqno` 和 `timestamp-ms` 是严格 `uint64_t`；`ifindex` 允许 `0`；RTT 和 available bandwidth 必须大于 `0`，loss 必须小于 `1000000 ppm`。更旧或相同 version 被忽略；withdraw 生成 Local Fact tombstone。非法输入返回 warning，不入事件队列。

示例：

```text
midr topology node upsert 10.0.0.1 group 10 transport 10.0.0.1 version 1
midr topology link upsert 10.0.0.1 10.0.0.2 id 12 local-address 198.51.100.12 remote-address 198.51.101.12 rtt-us 1000 loss-ppm 100 available-bandwidth-kbps 100000 version 1 ifindex 0 seqno 1 timestamp-ms 1000
midr topology link withdraw 10.0.0.1 10.0.0.2 id 12 version 2
midr topology node withdraw 10.0.0.1 version 2
```

## 5. 配置与联调示例

### 5.1 使用默认 AS_PATH 上限

```text
router bgp 65000
 neighbor 192.0.2.2 remote-as 65100
 address-family ipv4 unicast
  neighbor 192.0.2.2 activate
 exit-address-family
```

未配置 `max-as-path-length` 时默认上限为 `1`，本地 AS_PATH 长度 `0` 的路由与一跳 BGP 路由均可参与 Contributor 评估。

### 5.2 接受更长的普通 BGP 路径

```text
router bgp 65000
 address-family ipv4 unicast
  midr prefix-export max-as-path-length 3
 exit-address-family
```

### 5.3 IPv4 与 IPv6 使用不同 Policy

```text
route-map EXPORT-MIDR-V4 permit 10
exit
route-map EXPORT-MIDR-V6 permit 10
exit
router bgp 65000
 address-family ipv4 unicast
  midr prefix-export max-as-path-length 1
  midr prefix-export route-map EXPORT-MIDR-V4
 exit-address-family
 address-family ipv6 unicast
  midr prefix-export max-as-path-length 2
  midr prefix-export route-map EXPORT-MIDR-V6
 exit-address-family
```

### 5.4 显式导出 Default Route

```text
ip prefix-list MIDR-DEFAULT seq 10 permit 0.0.0.0/0
route-map EXPORT-DEFAULT permit 10
 match ip address prefix-list MIDR-DEFAULT
exit
router bgp 65000
 address-family ipv4 unicast
  network 0.0.0.0/0
  midr prefix-export route-map EXPORT-DEFAULT
 exit-address-family
```

### 5.5 代表接管与 EoR

```text
router bgp 65000
 midr group-prefix takeover-delay-ms 3000
 midr eor-timeout 60
```

### 5.6 当前三节点 MIDR 联调

本示例使用不写入 running-config 的 `midr peer session` 命令建立 MIDR Link-State AF，适用于当前多节点联调，不是重启后自动恢复的完整生产配置。

R1：

```text
route-map EXPORT-MIDR permit 10
exit
router bgp 65000
 bgp router-id 10.0.0.1
 neighbor 10.0.0.2 remote-as 65000
 neighbor 10.0.0.2 update-source 10.0.0.1
 address-family ipv4 unicast
  midr prefix-export route-map EXPORT-MIDR
  network 203.0.113.0/24
 exit-address-family
end
midr peer session 10.0.0.2 remote-as 65000 midr-link-state
```

R2：

```text
router bgp 65000
 bgp router-id 10.0.0.2
 neighbor 10.0.0.1 remote-as 65000
 neighbor 10.0.0.3 remote-as 65000
end
midr peer session 10.0.0.1 remote-as 65000 midr-link-state
midr peer session 10.0.0.3 remote-as 65000 midr-link-state
```

R3：

```text
router bgp 65000
 bgp router-id 10.0.0.3
 neighbor 10.0.0.2 remote-as 65000
end
midr peer session 10.0.0.2 remote-as 65000 midr-link-state
show midr ted summary
```

测试环境还需通过第一组 Provider 或测试注入为三节点建立 Membership；只有 Membership、Prefix baseline 和 EoR 条件满足后，代表节点才会生成 Group Prefix。
