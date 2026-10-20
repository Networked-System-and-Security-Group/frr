# MIDR R6-B-A：抽取清单、生命周期证明与迁移材料

## 1. 阶段状态

R6-B-A 已完成。本文档是 `R5-full-A` 抽取前行为基线与 `R7-EXT-0` 之间的冻结材料；它只描述现有实现的职责、依赖、生命周期和迁移归属，不宣称 `midrd` 已完成完整抽取。R7-MVP 的独立双地址族最小闭环仍只作为接口和运行基线，不能替代 R7-full。

代码基线：`3c805d38f7`（R5-full-A 文档）。前置材料：R6-A `554fe3b4a7`、R7-MVP `5d2662eccf`。下一阶段固定为 `R7-EXT-0`，未完成本材料前不得开始完整迁移。

## 2. 抽取清单

### 2.1 MIDR 专属源码

以下 `bgpd/` 文件当前承载协议算法、FRR 接线或两者的混合职责；R7-EXT-0 必须逐文件拆分为 `midrd` 核心、原生 Transport、Prefix/Consumer adapter、Diagnostics 或可删除的 BGP adapter，不能把整个 `bgpd` 目录复制到 `midrd`：

```text
bgp_midr.c/.h                 进程接线、初始化、公共生命周期
bgp_midr_addr.h               地址和端点表示
bgp_midr_attr.c/.h            对象属性、共享/编码属性
bgp_midr_canonical.c/.h       canonical identity/版本准入
bgp_midr_cl.c/.h              本地事实/控制输入（需与 BGP 控制面拆分）
bgp_midr_codec.c/.h           LS Object 编解码和严格校验
bgp_midr_cost.c/.h            cost 测量、去抖和版本刷新输入
bgp_midr_ctrl.c               控制入口
bgp_midr_ctrl_tcp.c           现有 BGP/TCP 控制适配，不能进入核心
bgp_midr_input.c              接收、计龄、输入队列和事件转换
bgp_midr_instance.c/.h       identity、实例和外部引用
bgp_midr_ls.c/.h              LS Object 语义与四类 identity
bgp_midr_lsdb.c/.h            staging、提交和撤销视图
bgp_midr_nds.c/.h             Node/邻接事实和代表计算
bgp_midr_nds_facts.c/.h       NDS 本地事实来源
bgp_midr_nds_vty.c/.h         NDS/诊断 CLI
bgp_midr_owned.c/.h           owned、refresh、withdraw、takeover
bgp_midr_packet.c/.h          BGP 报文适配、发送跟踪和恢复
bgp_midr_pm.c/.h              Prefix/member 派生和策略
bgp_midr_prefix.c/.h          Prefix contribution、snapshot 和撤销
bgp_midr_private.h            共享私有状态，需按依赖拆散
bgp_midr_rib.c/.h             canonical RIB、advertisement、floor/GC
bgp_midr_sequence.c/.h        sequence allocator 和持久化
bgp_midr_spf.c/.h             TED/SPF 派生边界
bgp_midr_spf_install.h/.c     SPF 结果安装适配
bgp_midr_spf_runtime.c        SPF 运行时适配
bgp_midr_store.c/.h           对象存储、计数和生命周期
bgp_midr_sync.c/.h            snapshot/EoR、session、重同步
bgp_midr_ted.c/.h             TED prepare/commit、snapshot 引用
bgp_midr_ted_private.h        TED 私有引用和锁边界
bgp_midr_vty.c/.h             show/config/诊断
bgp_midr_zebra.c/.h           Zebra/FIB 适配，不能进入核心
midr_ip2asn.c/.h              外部事实/诊断工具适配
midr_tier1.c/.h               Tier-1 派生/实验适配
midr_tier1_vty.c/.h           Tier-1 CLI
midr_trace_exec.c/.h          trace 执行适配
midr_trace_observer.c/.h      trace 观察适配
midr_trace_scheduler.c/.h     trace 调度适配
```

每个文件在 R7-EXT-0 的清单中必须标注函数级拆分结果；上述列表是文件级完整入口，不是允许原样复制的目标目录。

### 2.2 FRR 构建、共享常量和路由接线

需要单独审计并在迁移后决定保留位置：

- `bgpd/subdir.am`、`tests/bgpd/subdir.am` 中的源文件、头文件和测试目标；
- `configure.ac`、`config.h.in` 中的 MIDR 专属配置（包括受控 traceroute 路径）；
- `lib/iana_afi.h`、`lib/zebra.h`、`lib/route_types.txt` 中的 MIDR SAFI、route type 和名称映射；
- `zebra/rt_netlink.*`、`zebra/kernel_netlink.c`、`zebra/fpm_listener.c`、`zebra/debug_nl.c`、`zebra/zebra_rib.c` 中的 protocol 199 路由表示；
- 所有 `tests/bgpd/` MIDR 组件测试、TED fixture、fuzz/codec 测试、R5 runner、R7 containerlab runner 与脚本；
- `doc/midr-doc/` 的 P0-P6、R3/R4、R5-full-A、R6-A、R7-MVP 记录以及本地实施计划。

共享 AFI/SAFI 和 protocol 199 不能在没有归属决定的情况下直接删除：R7-EXT-2 之前保留兼容映射，R7-CLEANUP 再依据 Consumer/daemon 的最终路由接线收口。

## 3. 生命周期与所有权证明

### 3.1 canonical、实例和 RIB

identity 是唯一 canonical key；合法输入按 `(sequence, semantic state)` 准入。同版本同语义是重复，同版本异语义进入冲突路径，低版本不回退。当前实例、retired instance、floor 和外部引用分开计数。实例进入 retired 不等于释放：必须同时满足对象寿命、传播任务清空、旧编码引用释放、LSDB staging 不再引用、TED snapshot 不再引用及外部 Consumer 引用归零。floor 删除后，旧 ACTIVE/WITHDRAWN 和旧字节不得再次成为当前版本。

RIB identity、synthetic ID、canonical 实例和逐 peer advertisement 是不同所有权。断连只删该 peer 的 advertisement 关系；不删除 canonical、selected/path 语义、LSDB、TED 或其他 peer 关系。R7 核心不保留 FRR selected path；BGP adapter 可有自己的导出句柄，但不得反向成为 canonical 的准入依据。

### 3.2 owned、刷新、撤销和 sequence

owned 记录保存本地权威事实、当前 owner sequence、refresh cursor 和待生成的 ACTIVE/WITHDRAWN 事件。刷新必须分配更高 sequence；纯寿命刷新不改变语义或 TED generation，但仍必须洪泛。撤销生成失败不能清除旧 `advertised_present`、旧实例或刷新游标；恢复后由 reconcile、下一刷新批次或 shutdown poll 重试。owner 退出产生的权威撤销与 owner 意外失联后的寿命到期是两条不同路径，不能互相伪造。

### 3.3 lifetime、expire 和 floor GC

接收 age、进程内停留、编码/排队/部分写和每跳预算共同决定可用寿命。expire event 可以在本轮 GC 出错时保留已成功生成的结果；GC 只能在所有引用和传播义务清空后回收。`-EAGAIN`、`-ENOENT` 以及 R3 reclaim callback 的正常 veto 不是故障。生产 floor GC 继续默认关闭，直到部署条件验证完成。

### 3.4 LSDB/TED 和外部 Consumer

LSDB 使用 canonical 实例创建 prepared candidate，再以 generation 原子提交；失败不得发布半成品，也不得用旧 selected path 判断新实例是否可导出。TED snapshot 持有独立引用，Consumer 在释放前不得访问已替换内容；旧的一致快照可在断连、分区或屏障后继续服务。只有无法形成完整初始视图或 canonical/TED 无法保持一致时才进入 NOT_READY。

### 3.5 sync、packet、completion

session generation/token 区分新旧会话。packet 链表引用与 completion 引用分开持有；written、dropped、timeout 先在锁内摘链和认领，再解锁调度事件。teardown 只释放链表引用，不取消已认领 completion；事件执行时用 token/generation 判断是否仍属于活动会话。两份引用均释放后 packet 才销毁，迟到或重复 completion 无副作用。

### 3.6 Prefix Provider

Provider 以 generation 标识 snapshot，事件只包含中立的 IPv4/IPv6 Prefix 值、状态和序列；Provider 重连或 generation 跳变触发 snapshot/EoR 重同步。核心不得看到 `struct bgp`、`struct peer`、`struct bgp_path_info` 或 BGP attribute。BGP 只能作为可选外部 adapter，把 RIB 变化转换为本地 IPC 的同一 Prefix event/snapshot。

## 4. `H = L` 条件证明边界

令 `L` 为对象寿命，`H` 为对象从原始接收至允许其在系统中继续作为有效视图的最大总时延。对每一跳，编码年龄必须不小于接收年龄、接收后的本地处理停留和该跳预算 `B` 之和；共享模板、MRAI/排队、socket 部分写和超时必须在 peer-private stream 上重新检查。接收 FIFO 排空、编码前定龄、写出 tracker、超 `B` 断连并丢弃旧缓存、重同步是可执行的本地约束。

在以下部署条件成立时，未计龄区间的每一跳延迟总和不超过 `B`，且重同步不重置 age，则对象有效路径保持 `H = L`：

```text
age_at_next_receive >= age_at_previous_receive
                       + local_residence
                       + unmetered_hop_budget(B)
```

网络传播时延、远端内核 socket 等待、调度暂停和部署级拥塞并未在当前代码证据中全部测得；它们不能被证明材料默认为零。若部署不能满足 B，必须依靠超时检测、旧连接终止、缓存丢弃和重同步恢复，而不能仅增大 floor 保留期。故生产 floor GC 继续关闭；R7-hardening 才补真实 IPv4/IPv6 部署测量。

## 5. 归属矩阵

| 能力 | `midr-core` | 原生 MIDR Transport | Prefix Provider/IPC | LSDB/TED/Consumer/SPF | Diagnostics/CLI | BGP adapter（最终可删） |
|---|---|---|---|---|---|---|
| identity、sequence、ACTIVE/WITHDRAWN、canonical | 负责 | 调用 | 不负责 | 消费事件 | 展示 | 不负责 |
| scope、flood、refresh、expire、floor | 负责 | 携带事件 | 触发 Prefix | 生成视图 | 计数 | 不负责 |
| HELLO、KEEPALIVE、snapshot、EoR、重连 | 不负责 | 负责 IPv4/IPv6 | IPC 重连复用 snapshot 语义 | 观察 generation | 展示 | 不负责 |
| LSDB staging、TED atomic commit | 负责协议事件 | 不负责 | 不负责 | 负责下游接线 | 展示状态 | 不负责 |
| Prefix upsert/withdraw/generation | 接收中立事件 | 可传播 | 负责 source/IPC | 贡献 TED/SPF | 展示 | 只转换 BGP RIB |
| SPF、Zebra、Linux FIB | 不负责算法 | 不负责 | 不负责 | 负责 Consumer | 展示 | 不负责 |
| selected path、Adj-RIB-Out、BGP FSM、GR/LLGR | 禁止 | 禁止 | 禁止 | 禁止 | 只报告 | 仅 adapter 内部 |

## 6. R5-full-A 到 R7 的 parity matrix

| R5-full-A case | R7 迁移阶段 | 独立验证入口 |
|---|---|---|
| P6 read-only / TED-SPF-Zebra | EXT-2、hardening | 无 BGP containerlab；最终再做下游联调 |
| component-sync | EXT-1、EXT-3 | midrd sync/packet tests |
| component-owned | EXT-1 | core/owned tests |
| component-rib | EXT-1 | core/RIB/lifetime tests |
| component-scope | EXT-1 | scope/flood tests |
| line、triangle | EXT-1 | 双栈原生 Transport smoke |
| withdraw、prefix-withdraw | EXT-1、EXT-2 | object/Prefix e2e |
| EoR timeout | EXT-1、EXT-3 | session generation/EoR tests |
| route refresh | EXT-1、EXT-3 | snapshot/resync tests，不迁移 BGP Route Refresh |
| prefix、prefix takeover | EXT-1、EXT-3 | static Provider，再接 IPC |
| peer reconnect | EXT-1 | transport reconnect tests |
| partition recovery | EXT-1、hardening | containerlab fault injection |
| bgpd restart | EXT-3 | 改为 midrd/Provider 独立重启与 IPC 重连 |
| shutdown | EXT-3 | daemon shutdown/withdraw tests |
| WITHDRAWN/lifetime expiry | EXT-1 | 虚拟时间和短 L smoke |
| long refresh soak | hardening、R5-full-B | 跨刷新周期的双栈长稳 |

每项先保留 R5-full-A 的结果作为差分基线；R7 阶段不把 BGP selected path、TCP/179、BGP GR/LLGR 或 update-group 当作必须迁移的行为。IPv4 与 IPv6 必须在 core、Transport、Prefix Provider、IPC 和 Consumer parity 中分别出现。

## 7. R6-B-A 完成条件与下一闸门

- 所有 MIDR 专属源码、共享接线、构建项、测试、脚本和文档均已列出且有目标归属；
- canonical、RIB identity、retired/floor、owned、lifetime、LSDB/TED、sync/packet、Provider 的引用边界已写明；
- `H = L` 只作为带部署条件的证明，不把未测得的网络 B 写成已验证；
- R5-full-A 的 18 项结果均有 R7 parity 入口；
- 当前 `bgpd` 实现继续保留作为差分参考，不在 R6-B-A 删除或复制；
- 下一阶段唯一代码闸门为 `R7-EXT-0`：完整目录/构建归属、接口定稿和迁移测试骨架；
- R7-EXT-0 至 R7-EXT-3 全部通过前，不得清理 `bgpd` MIDR，也不得宣称完整抽取完成。

开发阶段只运行受影响的定向测试、独立目标编译和双栈 containerlab smoke；R7-hardening、R5-full-B 和最终交付再重建候选镜像并运行全量门禁。
