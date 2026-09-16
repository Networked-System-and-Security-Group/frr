# midrd 与第一组输入接口交接

## 1. 交接结论

第二组独立 `midrd` 已收口，代码提交为 `6bf1f9c524de2f8828c8c05942b0e72390d5c1d2`。第一组后续只需负责真实 Prefix、Membership、Link/NDS 数据的采集和转换，通过本文冻结的 Prefix/Local Fact Provider IPC 向 `midrd` 供数。

`midrd` 与 `bgpd` 是互相独立的协议进程。BGP 如需提供 Prefix，只能作为 Prefix IPC 的外部发布端；`midrd` 不依赖 BGP OPEN/UPDATE、BGP FSM、BGP capability、BGP RIB、selected path 或 TCP/179。

## 2. 责任边界

| 责任方 | 负责内容 |
| --- | --- |
| 第一组 | 采集真实 Prefix、Membership、Link/NDS；维护输入版本和 generation；生成 snapshot、增量和 withdraw |
| 第二组 | 接收并校验中立事件；原子提交本地事实；生成 owned LS Object；负责洪泛、生命周期、LSDB/TED 和 SPF |

第一组 adapter 不得读写 canonical、owned、scope、LSDB、TED、SPF 或 transport 私有状态，也不得依赖 `struct midrd`。

```text
真实 Prefix / Membership / Link / NDS
                     |
             第一组 adapter
                     |
       Prefix IPC + Local Fact IPC
                     |
                     v
                   midrd
```

## 3. 公开接口与运行入口

| 用途 | 公开头文件 | `midrd` 参数 |
| --- | --- | --- |
| Prefix 数据模型与 Provider | `midrd/midr-prefix-provider.h` | 静态预验证可用 `--prefix ADDRESS/LEN` |
| Prefix Unix stream IPC | `midrd/midr-prefix-ipc.h` | `--prefix-socket PATH` |
| Membership/Link 数据模型 | `midrd/midr-local-provider.h` | 静态预验证可用 `--group`/`--link` |
| Local Fact Unix stream IPC | `midrd/midr-local-ipc.h` | `--local-fact-socket PATH` |

`--prefix` 与 `--prefix-socket` 互斥；`--group`/`--link` 与 `--local-fact-socket` 互斥。第一组 adapter 作为 IPC client，`midrd` 作为 IPC server。对端应调用公开 IPC API，不复制 `midrd` 私有结构的内存布局。

## 4. Local Fact 交付语义

完整 snapshot 顺序为：

```text
MIDR_LOCAL_SNAPSHOT_BEGIN(generation=G)
  MIDR_LOCAL_MEMBERSHIP(0 或 1 条)
  MIDR_LOCAL_LINK(0..N 条)
MIDR_LOCAL_SNAPSHOT_END(generation=G)
MIDR_LOCAL_EOR(generation=G)
```

同一 snapshot 的 `originator` 必须等于本地 `midrd --node-id`，所有事件使用同一 generation。`midrd` 只在正确的 `SNAPSHOT_END + EOR` 后一次性替换正式视图；帧校验失败、容量失败或 IPC 中途断开都丢弃 staging，保留上一个已提交视图。

snapshot 提交后，`MIDR_LOCAL_MEMBERSHIP`、`MIDR_LOCAL_MEMBERSHIP_WITHDRAW`、`MIDR_LOCAL_LINK` 和 `MIDR_LOCAL_LINK_WITHDRAW` 可作为当前 generation 的增量。Membership 以 `version` 单调排序；Link 以 `(remote_node_id, link_id)` 为键，以 `version` 单调排序。对于增量，同版本同内容为幂等重复，同版本异内容为冲突，低版本不得覆盖新版本。完整 snapshot 中不得重复发送同一 Membership 或同一 `(remote_node_id, link_id)`。新的完整 resync 使用更高 generation。

Link 输入必须提供 `family`、`local_address`、`remote_address`、`local_ifindex`、RTT、丢包、可用带宽、`measurement_sequence` 和 `measurement_timestamp_ms`。`local_ifindex` 是本地元数据，不作为远端 LS Object identity 传播，但会进入本地 TED/SPF nexthop 结果。

## 5. Prefix 交付语义

完整 Prefix snapshot 顺序为：

```text
MIDR_PREFIX_SNAPSHOT_BEGIN(generation=G)
  MIDR_PREFIX_UPSERT(0..N 条)
MIDR_PREFIX_SNAPSHOT_END(generation=G)
MIDR_PREFIX_EOR(generation=G)
```

Prefix `originator` 必须等于本地 `node-id`。地址族只能为 `MIDR_CORE_AF_IPV4` 或 `MIDR_CORE_AF_IPV6`，prefix length 和地址必须通过 `midr_prefix_validate()`。完整 snapshot 只在 EoR 后提交；中途断开丢弃 staging，保留旧视图。

snapshot 之后的每个 `MIDR_PREFIX_UPSERT` 或 `MIDR_PREFIX_WITHDRAW` 增量使用严格更高的 generation。重连后 Provider 可从 generation 1 开始新 epoch，但必须先发送完整 snapshot，不能用一条增量假定 `midrd` 仍持有上一条连接的 generation 上下文。

## 6. 第一组验收条件

- IPv4 和 IPv6 Prefix 都能完成 snapshot、upsert、withdraw 和断连重同步。
- Membership 和 Link/NDS 都能完成 snapshot、增量、withdraw 和 generation 切换。
- snapshot 中途断开、重复 EoR、迟到旧 generation、同版本冲突和容量拒绝不产生半个新视图。
- Provider 重启后完整 resync，且不要求重启 `midrd`。
- adapter 只 include 公开 Provider/IPC 头文件，不依赖 `struct midrd`、owned、core store、LSDB 或 TED 内部结构。

## 7. 第二组已有证据

| 验证 | 结果 | 证据 |
| --- | --- | --- |
| Prefix/Local IPC 与事务 | Prefix Provider、Prefix IPC、Prefix transaction、Local IPC、Local transaction 组件测试 PASS | `/home/guest/yhy/midr-gate-runs/r7-second-group-close-20260916/component-final.log` |
| 无 BGP 双栈预验证 | IPv4/IPv6 收敛和 IPv4 graceful withdraw PASS | `/home/guest/yhy/midr-gate-runs/r7-second-group-close-20260916/containerlab.log` |
| 边界门禁 | Provider/IPC 公开头可独立编译，`midrd` 无 BGP/Zebra 依赖 | `midrd/extraction-boundary-test.sh` |

`bgpd` 旧 MIDR 实现暂作对照保留，但不是新输入接口的运行依赖。第二组分支当前提交未 push。
