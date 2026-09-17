# midrd 与 bgpd MIDR 测试覆盖对照

## 1. 结论

独立 `midrd` 在第二组负责范围内已达到功能测试较完整、可收口状态。当前 20 个组件程序覆盖 canonical/wire、原生 TCP transport、owned 生命周期、Prefix/Local Fact 事务、scope、LSDB/TED、SPF、同步、容量和独立边界；containerlab 和 hardening 覆盖真实 socket、双栈、分区恢复、过期、长稳和规模。

该结论不等于“与旧 `bgpd` 31 个测试程序数量相同”。旧集合包含 BGP Attribute/SAFI/packet adapter、Capability、Zebra/ZAPI、Linux FIB 和 SRv6 等非第二组独立核心内容，不能用程序数或断言数直接比较。

## 2. 功能映射

| 协议领域 | 旧 `bgpd` 主要测试 | 当前 `midrd` 测试 | 状态 |
| --- | --- | --- | --- |
| 对象、identity、sequence、版本准入 | `ls_object`、`sequence`、`rib`、`instance` | `core`、`engine` | 已覆盖高/同/低版本、冲突、重复、非字节对齐 Prefix 和 floor 防复活 |
| wire 与双栈对象 | `codec`、`attr`、`packet`、`safi` | `wire`、`contract` | 已覆盖 IPv4/IPv6、ACTIVE/WITHDRAWN、保留字段和非法载荷；BGP Attribute/SAFI 不再属于新核心 |
| 会话、重连、写出和 EoR | `packet`、`sync`、`batch` | `transport`、`transport-budget`、`sync` | 已覆盖 TCP partial read/write、连接碰撞、Hold Timer、generation、snapshot/EoR、迟到 completion 和退网三态 |
| Prefix 输入 | `input`、`resync`、`prefix` | `prefix-provider`、`prefix-ipc`、`prefix-transaction` | 已覆盖双栈 snapshot/upsert/withdraw/EoR、断连 abort、迟到 generation 和容量失败 |
| Membership/Link 输入 | `input`、`resync`、`cost` | `local-ipc`、`local-transaction`、`cost` | 已覆盖 snapshot/增量/withdraw、version、原子提交、ifindex、cost/deadband |
| owned、刷新、撤销、重启 | `owned`、`sequence` | `owned`、`engine`、restart smoke | 已覆盖持久 sequence、失败保留旧状态、纯刷新、正常退网和异常 owner 到期 |
| scope、LSDB、TED 事务 | `scope`、`lsdb`、`ted`、`ted_consumer` | `scope`、`lsdb`、`ted`、`consumer`、`engine` | 已覆盖 dependency pending、READY/NOT_READY、prepare/commit/abort、generation 和六类正式视图 |
| SPF | `spf` | `spf`、`engine` | 已覆盖 IPv4/IPv6、local/intra/inter/unreachable、nexthop/ifindex、ECMP 和迟到 generation |
| 容量与回收 | `scale`、`instance`、`rib` | `scale`、`core` | 已覆盖 4096 objects、floor 保留、veto GC、槽位复用和 8 邻居 |
| 下游路由安装 | `spf_install`、`spf_pipeline`、`zebra`、`zapi_*`、`proto199`、`zebra_e2e` | 无 | 第三组边界，不计为 `midrd` 核心测试缺口 |
| BGP capability/GR/LLGR | `capability`、`safi` | 无 | 新协议不使用 BGP Capability/GR/LLGR，不需迁移 |

## 3. 当前自动化基线

`midrd/Makefile` 的 `test` 目标执行 20 个程序：

```text
contract, core, prefix-provider, wire, transport, transport-budget,
consumer, spf, engine, prefix-ipc, owned, prefix-transaction, scope,
cost, local-ipc, local-transaction, lsdb, ted, sync, scale
```

2026-09-16 在提交 `5eab85832a` 上使用独立 `/tmp` 构建目录执行，20 个程序和 `extraction-boundary-test.sh` 全部通过。同一收口周期的 Clang ASAN/UBSAN/LSAN、IPv4/IPv6 containerlab、triangle、双栈 partition/recovery、owner expiry、refresh soak、4096 objects 和 8 neighbors 已通过。

覆盖率仅用于定位明显未执行路径。`5eab85832a` 前一轮基线的生产源码汇总为：

```text
line:     4675 / 6184 = 75.6%
function:  358 /  391 = 91.6%
branch:   2799 / 4777 = 58.6%
```

该数字不是收口门槛。`midrd.c` 中 CLI、真实 socket、periodic 和 shutdown 路径主要由 containerlab/hardening 黑盒场景覆盖，不为提高 branch 百分比重复构造低价值单测。

## 4. 收口补强

`5eab85832a` 增加以下功能回归：

- `midr_engine_refresh()` 纯刷新只推进对象 sequence，不改变语义 generation；batch 内刷新失败使整个事务 abort。
- `midr_engine_export()` / `midr_engine_usable()` 覆盖 Membership、Node Prefix 和 WITHDRAWN 语义。
- `midr_engine_apply_prefix_event()` 覆盖 IPv4/IPv6 upsert、withdraw 和 snapshot control event。
- Prefix withdraw 生成 WITHDRAWN 对象时清零 ACTIVE metric，修复合法 Provider withdraw 被 core 拒绝的缺陷。
- IPv4 `/25`、IPv6 `/65` 的非字节对齐归一化，以及 wire 层非法 WITHDRAWN 载荷拒绝。
- Prefix Provider generation 的初始、upsert、withdraw 和空指针查询。

## 5. 剩余边界

第二组范围内没有已知阻塞性测试缺口。剩余验收是跨组完整链路：第一组真实输入 adapter、第三组 Zebra/FIB adapter、containerlab 三组接线以及真实 IPv6 网联合验收。IPv4 回归仍是联合验收的必选项。
