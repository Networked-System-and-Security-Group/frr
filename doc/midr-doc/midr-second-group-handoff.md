# midrd 第二组收口与一三组交接

## 1. 收口结论

第二组的链路状态传播与 SPF 实现已收口，代码提交为 `6bf1f9c524de2f8828c8c05942b0e72390d5c1d2`。该提交只修改 `Makefile.am` 和 `midrd/**`，没有修改或删除 `bgpd` 旧 MIDR 实现，也没有代替第一组的数据采集代码或第三组的 Zebra/FIB 代码。

第二组已交付：

- 可在无 `bgpd` 条件下独立构建和运行的 `midrd`。
- MIDR 自有 IPv4/IPv6 TCP listener/connector、MIDR frame、HELLO、KEEPALIVE、Hold Timer、snapshot/EoR、session generation、断连重连和发送完成跟踪。
- Membership、Link、Node Prefix、Group Prefix 四类对象的 canonical、owned、sequence、ACTIVE/WITHDRAWN、lifetime、floor、scope 和洪泛。
- LSDB/TED 原子 staging、dependency pending、READY/NOT_READY、六类正式 TED 视图和分层 SPF。
- 协议中立的 Prefix Provider/IPC、Local Fact Provider/IPC、TED Consumer 和 SPF route result 接口。
- IPv4/IPv6 nexthop、ifindex、local/unreachable 语义和等价多首跳集合。

第二组不交付：

- Prefix、Membership、Link 或 NDS 的真实系统采集实现。
- Zebra/ZAPI/Linux FIB 适配、UCMP 权重、SRv6 封装或内核路由管理。
- `bgpd` 旧 MIDR 实现的删除。
- 三组真实 IPv6 网的最终联合验收。

## 2. 责任边界

| 小组 | 负责范围 | 禁止的反向依赖 |
| --- | --- | --- |
| 第一组 | 采集真实 Prefix、Membership、Link/NDS 事实；转换为 Prefix/Local Fact snapshot 和增量事件 | 不读写 canonical、owned、LSDB、TED、SPF 或 transport 私有状态 |
| 第二组 | MIDR 会话、对象传播、生命周期、scope、LSDB/TED 和 SPF；维护中立输入/输出契约 | 不引入 BGP RIB/selected path、Zebra、ZAPI 或 Linux FIB 类型 |
| 第三组 | 消费 committed TED/SPF route result；实现 Zebra/ZAPI/Linux FIB、ECMP/UCMP/SRv6 等下游策略 | 不修改 MIDR canonical、洪泛、sequence、lifetime、scope 或 LSDB/TED 内部状态 |

```text
第一组采集器
  |  Prefix Provider/IPC + Local Fact Provider/IPC
  v
midrd
  |  native MIDR TCP -> canonical -> scope -> LSDB -> TED -> SPF
  v
第三组 adapter
  |  Zebra/ZAPI
  v
Linux FIB
```

`midrd` 与 `bgpd` 是互相独立的协议进程。BGP 如需提供 Prefix，只能作为 Prefix IPC 的外部发布端；`midrd` 不依赖 BGP OPEN/UPDATE、BGP FSM、BGP capability、BGP RIB 或 TCP/179。

## 3. 第一组输入交接

### 3.1 公开头文件与运行入口

| 用途 | 公开头文件 | `midrd` 参数 |
| --- | --- | --- |
| Prefix 数据模型与 Provider | `midrd/midr-prefix-provider.h` | 静态预验证可用 `--prefix ADDRESS/LEN` |
| Prefix Unix stream IPC | `midrd/midr-prefix-ipc.h` | `--prefix-socket PATH` |
| Membership/Link 数据模型 | `midrd/midr-local-provider.h` | 静态预验证可用 `--group`/`--link` |
| Local Fact Unix stream IPC | `midrd/midr-local-ipc.h` | `--local-fact-socket PATH` |

`--prefix` 与 `--prefix-socket` 互斥；`--group`/`--link` 与 `--local-fact-socket` 互斥。第一组 adapter 作为 IPC client，`midrd` 作为 IPC server。两类 IPC 都使用定长帧和结构化编解码，不应由对端复制私有内存布局。

### 3.2 Local Fact snapshot 与增量

完整 snapshot 顺序固定为：

```text
MIDR_LOCAL_SNAPSHOT_BEGIN(generation=G)
  MIDR_LOCAL_MEMBERSHIP(0 或 1 条)
  MIDR_LOCAL_LINK(0..N 条)
MIDR_LOCAL_SNAPSHOT_END(generation=G)
MIDR_LOCAL_EOR(generation=G)
```

同一 snapshot 的 `originator` 必须等于本地 `midrd --node-id`，所有事件必须使用同一 generation。`midrd` 只在正确的 `SNAPSHOT_END + EOR` 后一次性替换正式视图；帧校验失败、容量失败或 IPC 中途断开都丢弃 staging，保留上一个已提交视图。

snapshot 提交后，`MIDR_LOCAL_MEMBERSHIP`、`MIDR_LOCAL_MEMBERSHIP_WITHDRAW`、`MIDR_LOCAL_LINK` 和 `MIDR_LOCAL_LINK_WITHDRAW` 可作为当前 generation 的增量。Membership 以 `version` 单调排序；Link 以 `(remote_node_id, link_id)` 为键，以 `version` 单调排序。对于增量，同版本同内容为幂等重复，同版本异内容为冲突，低版本不得覆盖新版本。完整 snapshot 中不得重复发送同一 Membership 或同一 `(remote_node_id, link_id)`。新的完整 resync 使用更高 generation。

Link 输入必须提供 `family`、`local_address`、`remote_address`、`local_ifindex`、RTT、丢包、可用带宽、`measurement_sequence` 和 `measurement_timestamp_ms`。`local_ifindex` 是本地元数据，不作为远端 LS Object identity 传播，但会进入本地 TED/SPF nexthop 结果。

### 3.3 Prefix snapshot 与增量

完整 Prefix snapshot 顺序固定为：

```text
MIDR_PREFIX_SNAPSHOT_BEGIN(generation=G)
  MIDR_PREFIX_UPSERT(0..N 条)
MIDR_PREFIX_SNAPSHOT_END(generation=G)
MIDR_PREFIX_EOR(generation=G)
```

Prefix `originator` 必须等于本地 `node-id`。地址族只能为 `MIDR_CORE_AF_IPV4` 或 `MIDR_CORE_AF_IPV6`，prefix length 和地址必须通过 `midr_prefix_validate()`。完整 snapshot 只在 EoR 后提交；中途断开丢弃 staging，保留旧视图。

snapshot 之后的每个 `MIDR_PREFIX_UPSERT` 或 `MIDR_PREFIX_WITHDRAW` 增量使用严格更高的 generation。重连后 Provider 可从 generation 1 开始新 epoch，但必须先发送完整 snapshot，不能用一条增量假定 `midrd` 仍持有上一条连接的 generation 上下文。

### 3.4 第一组验收条件

- IPv4 和 IPv6 Prefix 都能完成 snapshot、upsert、withdraw 和断连重同步。
- Membership 和 Link/NDS 都能完成 snapshot、增量、withdraw 和 generation 切换。
- snapshot 中途断开、重复 EoR、迟到旧 generation、同版本冲突和容量拒绝不产生半个新视图。
- Provider 重启后完整 resync，且不要求重启 `midrd`。
- adapter 只 include 公开 Provider/IPC 头文件，不依赖 `struct midrd`、owned、core store、LSDB 或 TED 内部结构。

## 4. 第三组输出交接

### 4.1 公开头文件

| 用途 | 公开头文件 |
| --- | --- |
| committed Consumer snapshot | `midrd/midr-consumer.h` |
| TED READY/NOT_READY、视图和变更通知 | `midrd/midr-ted.h` |
| IPv4/IPv6 SPF route result 和 nexthop 集合 | `midrd/midr-spf.h` |

第三组 adapter 在 `midr_ted_consumer_register()` 注册 `snapshot_changed` 回调。回调只表示新 TED generation 已提交，不能拒绝或回滚上游。adapter 应在回调后使用 `midr_ted_view_acquire()` 获取完整视图，调用 `midr_spf_compute_ted()` 计算路由，并在自己的 staging 中完成 diff 和 ZAPI 编码。一个 generation 的所有添加、替换和删除应一次性发布，不得将新旧 generation 混装进 FIB。

`MIDR_TED_NOT_READY` 或 `midr_ted_view_acquire() == -EAGAIN` 时不得生成部分新 FIB 结果。是否暂时保留上一个已安装的一致 FIB、设置失效定时器或执行撤销，属于第三组转发面策略；但不能将旧视图标记为新 generation，也不能绕过 NOT_READY 读取私有 TED 内存。

### 4.2 SPF route result 语义

`struct midr_spf_route` 包含：

- `family`/`prefix_len`/`prefix`：IPv4 或 IPv6 目的前缀。
- `generation`：生成该结果的 committed TED generation，adapter 必须拒绝迟到旧 generation。
- `metric`、`group_score`、`local_cost`：分层 SPF 结果；下游不应重新解释 MIDR scope 或重算 canonical cost。
- `scope`：`LOCAL`、`INTRA_GROUP`、`INTER_GROUP` 或 `UNREACHABLE`。
- `reachable`/`local_destination`：本地前缀不需要 nexthop；不可达前缀的 metric 为无穷大且 nexthop 为空。
- `nexthops[]`：等价最优首跳集合，每项包含 address family、address、ifindex、local/remote node、group、next group 和 link ID。

`midr_spf_routes_clear()` 负责释放每条 route 内部的 nexthop 数组。当前输出是等价多首跳集合；UCMP 权重、SRv6 封装和 Zebra route type/instance 都不在该结构中，由第三组下游策略处理。

### 4.3 第三组验收条件

- IPv4/IPv6 local、intra-group、inter-group、unreachable 路由转换正确。
- 单首跳和 ECMP 多首跳的 address/ifindex 与 SPF result 一致。
- 新 generation 以原子 diff 安装；迟到旧 generation、重复 callback 和回调内注销不污染当前 FIB。
- NOT_READY、ZAPI 部分失败、Zebra 断连和重连都不留下半个新 generation。
- route replacement、withdraw、daemon restart 和退网后 Linux FIB 没有残留。
- adapter 只 include 公开 Consumer/TED/SPF 头文件，不读写 `midrd` 私有对象或把 Zebra 头文件引入 MIDR 核心。

## 5. 联合验收边界

真实 IPv6 网首轮联调不启动 `bgpd`，由第一组 adapter、`midrd` 和第三组 adapter 组成完整路径：

```text
真实 Prefix/Membership/Link/NDS
  -> Provider IPC
  -> midrd IPv6 TCP 会话与 LS 对象传播
  -> canonical/LSDB/TED/SPF
  -> Zebra/ZAPI
  -> Linux IPv6 FIB
```

联合验收至少覆盖建邻、snapshot/EoR、Prefix 发布与撤销、Link 变化、断连重连、异常 owner 到期、SPF nexthop/ifindex 和 FIB 更新。IPv6 是首轮真网场景，但 IPv4 支持不得被删除；三组接线后还必须在 containerlab 重放 IPv4 与 IPv6 双地址族回归。

## 6. 第二组验收证据

| 验证 | 结果 | 证据 |
| --- | --- | --- |
| Linux GCC 独立组件 | 20 个测试程序（含 scale）与 boundary scan PASS | `/home/guest/yhy/midr-gate-runs/r7-second-group-close-20260916/component-final.log` |
| FRR 顶层构建 | `make -j4 midrd/midrd` PASS，无未解释 warning | `/home/guest/yhy/midr-gate-runs/r7-second-group-close-20260916/frr-build-final.log` |
| 无 BGP containerlab | IPv4/IPv6 收敛、IPv4 graceful withdraw PASS | `/home/guest/yhy/midr-gate-runs/r7-second-group-close-20260916/containerlab.log` |
| Sanitizer | Clang ASAN/UBSAN/LSAN 全量 PASS | `/home/guest/yhy/midr-gate-runs/r7-hardening-20260916-phase1` 及 `phase1-fixed` |
| 故障、长稳与规模 | triangle、双栈 partition/recovery、owner expiry、refresh soak、4096 objects、8 neighbors PASS | `/home/guest/yhy/midr-gate-runs/r7-hardening-20260916-phase2`、`phase2-fixed`、`phase3-spf` |

`midrd/extraction-boundary-test.sh` 已禁止 `bgpd`/`bgp_`/Zebra include、`struct bgp`、`struct peer`、`struct bgp_path_info`、`AFI_BGP`、`SAFI_MIDR_LS` 以及 BGP OPEN/UPDATE 符号，并编译公开交接头文件。该门禁必须在一/三组接线后继续通过。

## 7. 保留项

- 生产 floor GC 继续默认关闭；`H = L` 只在逐跳未计龄延迟受预算 B 约束等条件下成立。
- FOLLOW-11-A/B 保持现有状态，不因第二组代码收口自动关闭。
- `bgpd` 旧 MIDR 实现暂作差分对照保留，本轮不执行 R7-CLEANUP。
- 第二组分支当前提交未 push，后续 push/合并按项目流程单独执行。
