# MIDR R5-full-A：抽取前行为基线

## 1. 状态

R5-full-A 已完成。该阶段固定嵌入 `bgpd` 的 MIDR 实现中必须由独立 `midrd` 保留的协议行为，不启动 R7-full 迁移，也不把 BGP adapter 的实现机制定义为新协议接口。

基线信息：

```text
分支：feat/yhy-midr-single-instance-ls-flooding
测试提交：a1d57902e1f44f9e688a51cf186476248938575e
独立构建目录：/home/guest/yhy/midr-r5-lite-build
15 节点实验台：clab-midr-backbone-p6-final-*
实验台镜像：frr-midr-p6:f9beedd0d653
镜像 ID：sha256:1c59b1dfc2753086c5fde55615082090d7707c817edc6df6641c02a9fbc55ecc
证据目录：/home/guest/yhy/midr-gate-runs/r5-full-a-20260915-committed
结果：18 passed, 0 failed, 0 skipped
```

独立构建目录中的嵌入式 MIDR 代码对应 `bf20cfd4fa`；从该提交至测试提交只新增 R6-A、R7-MVP 文档及 `midrd` MVP 文件，`bgpd` MIDR 实现没有变化。15 节点台用于只读检查既有 P6 镜像；当前分支的故障与恢复行为由独立构建目录运行的多进程场景固定。

## 2. 验收入口

```sh
MIDR_R5_FULL_A_BUILD_ROOT=/home/guest/yhy/midr-r5-lite-build \
MIDR_R5_FULL_A_LAB_PREFIX=clab-midr-backbone-p6-final \
MIDR_R5_FULL_A_RUN_ROOT=/home/guest/yhy/midr-gate-runs/r5-full-a-20260915-committed \
./midr-test/run-r5-full-a.sh
```

runner 不重建镜像，不在源码树构建。15 节点检查只读现有容器；多进程场景在独立 user/mount/network namespace 中运行。VTY socket 使用短的 `/tmp` 根目录，日志与状态证据仍保存在指定证据目录，避免 Unix-domain socket 路径上限影响长证据路径。

## 3. 行为基线

| 场景 | 结果 | 固定的行为 |
| --- | --- | --- |
| `p6-readonly` | 14/14 | 7 个 MIDR 节点的 SPF 均为 READY，每个节点的 6 个远端服务前缀均安装到 Zebra 与 Linux `proto 199`；每个 owner 的最新 Membership sequence 在全网一致 |
| `component-sync` | PASS | session generation、snapshot/EoR、迟到 completion、写出超时、重同步和 `COMPLETE`/`DEGRADED`/`GENERATION_FAILED` 退网结果保持可观察且有界 |
| `component-owned` | PASS | owner 刷新、撤销、sequence 持久化、fight-back 和失败后重试不破坏旧 advertised 状态 |
| `component-rib` | PASS | canonical 版本准入、peer advertisement 清理、WITHDRAWN/ACTIVE 寿命到期、floor/identity 回收和失败后下一轮重试保持原子性 |
| `component-scope` | PASS | GLOBAL/GROUP scope、未知 WITHDRAWN 暂定全局传播和纯寿命刷新传播保持现有语义 |
| `m5-line`、`m5-triangle` | PASS | 单实例 identity 在单路径和多邻居拓扑中收敛；环路重复不制造额外 canonical path |
| `m5-withdraw`、`m5-prefix-withdraw` | PASS | Link 与 Prefix 撤销以更高版本传播，远端 LSDB/TED 删除对应可用对象 |
| `m5-scope` | PASS | 不同组节点保留完整 canonical 信息，只按 scope 派生本地可用 Link 视图 |
| `m5-eor-timeout` | PASS | EoR timeout 是可用同步结果；活动 session 在 receive stream 排空前可以保持 `REMOTE_WAIT`，不强制伪造 READY |
| `m5-route-refresh` | PASS | 旧 adapter 的全量重发不制造 conflict，canonical identity 数保持稳定 |
| `m5-prefix`、`m5-prefix-takeover` | PASS | Prefix contribution 形成 Group Prefix；owner 意外退出后备选 owner 接管并维持远端 TED Prefix 视图 |
| `m5-peer-reconnect` | PASS | peer 断连只删除该 peer 的 advertisement 关系；旧 canonical 视图继续可用，重连后 session generation 增加并以 snapshot/EoR 恢复 |
| `m5-partition-recovery` | PASS | 全链路分区期间各节点保留已有的 3 个 canonical Membership；链路恢复后重新同步并回到 READY |
| `m5-bgpd-restart` | PASS | 中继进程退出期间两端保留旧 canonical 视图；中继重启、重新注入本地事实并完成 snapshot/EoR 后，三端重新收敛 |
| `m5-shutdown` | PASS | 正常退网先传播 owner 高版本撤销再结束会话；本轮结果为 `COMPLETE`，远端 Membership 从 3 降为 2 |

15 节点台从 `2026-09-13T09:44:57Z` 持续运行至本轮检查，已跨过多个 5 分钟 owner 刷新周期；本轮 7 个 owner 的最新 Membership sequence 仍在所有 MIDR 节点一致。纯刷新不改变 TED 语义 generation、但必须继续洪泛以维持远端寿命的规则同时由 `component-scope` 固定。WITHDRAWN 到期与安全回收使用 `component-rib` 的虚拟时间稳定覆盖，不等待生产一小时寿命。

## 4. R7-full 迁移边界

以下属于协议行为，必须迁入独立 `midrd`：对象 identity 与 sequence 比较、canonical 版本准入、ACTIVE/WITHDRAWN、ownership 与 takeover、scope、刷新与寿命到期、snapshot/EoR、断连后旧一致视图、重同步、LSDB staging、TED 原子发布、Prefix contribution 和双地址族对象语义。

以下属于旧 `bgpd` adapter 机制，不迁入 `midrd` 核心：BGP selected path、`struct peer` advertisement 指针、BGP UPDATE/MP_REACH/MP_UNREACH、AFI/SAFI、TCP/179、update-group/Adj-RIB-Out、BGP Route Refresh、BGP GR/LLGR capability 和 BGP Session FSM。R7 可以用本节相同行为检查做差分验证，但必须由原生 MIDR Transport、协议中立的 Prefix Provider/IPC 和 Consumer 接口实现。

断连或暂时分区不要求立即把 TED 置为 NOT_READY。只要 canonical 与 TED 仍指向同一份已提交旧视图，旧视图继续可用；新的 snapshot 完整提交后再原子替换。同步屏障期间无法形成完整初始视图、或派生失败导致 canonical 与 TED 无法保持一致时，才进入 NOT_READY。

## 5. 后续闸门

R5-full-A 完成后进入 R6-B-A，只整理抽取清单、生命周期证明、依赖归属和 parity matrix。R6-B-A 完成并复核前不得启动 R7-EXT-0。最终独立 `midrd` 的故障矩阵、Prefix IPC、正式 daemon、双栈 Consumer、最终镜像和真实 IPv6 网络结果由 R7-hardening 与 R5-full-B 验收，不在本阶段提前填写。
