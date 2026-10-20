# midrd R7-full 汇总验收

## 1. 验收结论

R7-full 及第二组 hardening 已完成，主体收口提交为 `6bf1f9c524`，收口后审查修正和功能测试补强提交为 `5eab85832a`。独立 `midrd` 已具备不启动 `bgpd` 时的构建、启动、IPv4/IPv6 原生 TCP 会话、对象传播、snapshot/EoR、canonical/owned 生命周期、Prefix/Local Fact 中立输入、LSDB/TED 原子派生和分层 SPF 计算能力。核心、Transport、Provider、Consumer 与 daemon 边界中不包含 BGP OPEN/UPDATE、BGP FSM、selected path、Adj-RIB-Out、AFI/SAFI 或 BGP GR/LLGR 依赖。

本结论包含第二组负责的故障、规模、长稳与 Sanitizer 验证。真实 Prefix、Membership、Link/NDS 供数由第一组接线，Zebra/ZAPI/Linux FIB 由第三组接线；这两项以及三组真实 IPv6 网联调不计为第二组未完成实现。生产 floor GC 继续默认关闭；`bgpd` 旧 MIDR 实现暂作对照保留。

## 2. 验收基线

```text
日期：2026-09-16
主体收口：6bf1f9c524de2f8828c8c05942b0e72390d5c1d2
当前代码：5eab85832a
远端 Git worktree：/home/guest/yhy/frr-single-instance-ls
远端验证主机：NetArchLab90（fit-server-90）
收口证据：/home/guest/yhy/midr-gate-runs/r7-second-group-close-20260916
前置 R7-full 证据：/home/guest/yhy/midr-gate-runs/r7-full-20260916-closure4
前置 hardening 证据：/home/guest/yhy/midr-gate-runs/r7-hardening-20260916-phase1*、phase2*、phase3-spf
```

收口提交只包含 `Makefile.am` 和 `midrd/**`，没有删除或修改 `bgpd` 旧 MIDR 实现。独立组件使用 `/tmp` 构建目录；FRR 顶层目标使用一次性干净源码副本验证，没有在正式 worktree 内生成构建产物。

## 3. 实现闭环

| 边界 | R7-full 状态 |
| --- | --- |
| Canonical/Wire | 四类 identity、ACTIVE/WITHDRAWN、sequence 准入、冲突/重复/低版本处理、年龄与寿命、防复活 floor 已完成 |
| Owned | 本地权威 upsert/refresh/withdraw、sequence 高水位持久化、失败保留旧状态、正常退网撤销已完成 |
| Sync/事务 | snapshot、增量、EoR、session generation、Prefix/Local Fact staging、失败 abort、完整提交后发布已完成 |
| Scope/LSDB/TED | scope 派生、LSDB prepared/commit、dependency pending、TED READY/NOT_READY、六类正式视图已完成 |
| SPF | 分层有向图、IPv4/IPv6 Prefix、出口选择、不可达结果和 generation 门禁已完成 |
| 第一组输入契约 | Local Fact/Prefix snapshot、增量 upsert/withdraw、resync barrier、generation 和 `local_ifindex` 已完成；真实采集 adapter 由第一组实现 |
| 第三组输出契约 | committed TED callback、READY/NOT_READY、generation、IPv4/IPv6 Prefix、nexthop/ifindex/ECMP 首跳集已完成；Zebra/FIB adapter 由第三组实现 |
| 原生 Transport | 独立 TCP listener/connector、HELLO/KEEPALIVE、partial read/write、Hold Timer、重连和发送完成跟踪已完成 |
| Prefix 输入 | static Provider 与 Unix stream Prefix IPC 的 snapshot/upsert/withdraw/EoR 已完成；BGP 仅可作为外部可选发布端 |
| 独立边界 | boundary scan 通过；核心和 daemon 不依赖 `bgpd`、BGP 类型或 BGP 报文 |

本轮真实 socket 验证发现并修复两项阻塞缺陷：双方同时主动连接时各自保留 outbound 导致持续重连；frame callback 同步关闭当前连接后 RX parser 继续按旧 `rx_length` 搬移数据导致 `SIGBUS`。Transport 现按实际监听端点确定性裁决重复连接，并以 fd/session generation 阻止旧 stream callback 返回后的继续消费。

## 4. 自动化证据

Linux GCC 独立 `/tmp` 构建执行 `make test`，20 个测试程序全部通过：contract、core、Prefix Provider、wire、transport、transport budget、Consumer、SPF、engine、Prefix IPC、owned、Prefix transaction、scope、cost、Local IPC、Local transaction、LSDB、TED、sync、scale。`extraction-boundary-test.sh` 同轮通过。`5eab85832a` 上以独立 `/tmp/midrd-close-final-20260916` 构建目录复测同一门禁，结果仍为全部 PASS。FRR 顶层 `make -j4 midrd/midrd` 构建通过，无未解释 warning（该目标只依赖 libfrr，不编译 `zebra/`、`bgpd/`，故不含其告警；对当前第三组分支做完整从零重建 `make clean && make -j112` 时 `zebra/dplane_fpm_nl.c` 与 `bgpd/` 仍有告警，见 `dp-doc/dp-02-build-and-test.md` 第 3.1 节）。收口日志为 `component-final.log`、`frr-build-final.log` 和 `containerlab.log`。

收口后审查补充了 refresh/scope query、Prefix event、非字节对齐 Prefix、WITHDRAWN wire payload 和 Provider generation 回归，并修正两项真实缺陷：合法 Prefix withdraw 生成的 WITHDRAWN 对象未清零 ACTIVE metric；batch 内 refresh identity 查找失败未标记整个事务失败。

本机补充验证结果：

| 场景 | 结果 |
| --- | --- |
| IPv4 三节点同时启动/双向连接/并发退网重复 8 轮 | PASS，所有进程退出码为 0，每轮均达到 3 route |
| IPv6 三节点同路径重复 3 轮 | PASS，所有进程退出码为 0，每轮均达到 3 route |
| `r7-native-smoke.sh` IPv4/IPv6 | PASS |
| `r7-restart-smoke.sh` owner 强杀、sequence 文件复用和重启 | PASS |
| simultaneous connect 与 callback disconnect 定向回归 | 连续 5 轮 PASS，并纳入 `midrd-transport-test` |

NetArchLab90 containerlab 使用独立网络命名空间且不启动 `bgpd`：

| 场景 | 结果 | 证据 |
| --- | --- | --- |
| IPv4 A-B-C 三节点收敛 | PASS；三节点达到 3 route，A 到 C 与 C 到 A 的链路累计 metric 为 12 | `ipv4-convergence/` |
| IPv6 A-B-C 三节点收敛 | PASS；三节点达到 3 route | `ipv6-convergence/` |
| IPv4 owner 正常提前退网 | PASS；高版本撤销传播后其余节点收敛到 0 route | `ipv4-expiry/`（运行时场景名，实际语义为 graceful withdraw） |

完整 GCC、boundary 和 containerlab 输出分别保存在 `gate.log`、`containerlab.log` 与上述场景目录。测试源码归档为 `midrd-source.tar.gz`。

## 5. R5-full-A parity 归属

| R5-full-A 行为 | R7-full 证据或后续归属 |
| --- | --- |
| component-sync | `midrd-sync-test`、transport completion/budget、EoR generation 与退网三态测试 |
| component-owned | `midrd-owned-test`、sequence 持久化和 owner restart smoke |
| component-rib | core/engine 的版本准入、lifetime、floor/GC 和失败恢复测试 |
| component-scope | scope、LSDB/TED、纯刷新不改派生 generation 的测试 |
| line | IPv4/IPv6 三节点 native 与 containerlab smoke |
| triangle | scope/engine/SPF 多邻居定向测试和 R7-hardening IPv4 triangle 均已通过 |
| withdraw、prefix-withdraw | owned/Prefix transaction 测试与 containerlab 正常退网撤销传播 |
| EoR timeout | sync generation、barrier、迟到 EoR 和断连 abort 测试 |
| route refresh | 以原生 snapshot/resync 取代 BGP Route Refresh，sync 测试覆盖 |
| prefix、prefix takeover | Prefix/Local transaction、representative 与 staging 测试 |
| peer reconnect | transport reconnect、snapshot generation 和 owner restart smoke |
| partition recovery | 保留旧一致 canonical/TED 的组件语义以及 IPv4/IPv6 partition/recovery 均已通过 |
| bgpd restart | 协议等价项改为 `midrd`/Provider 独立重启；owner restart smoke 已通过 |
| shutdown | COMPLETE/DEGRADED/GENERATION_FAILED 组件测试及真实 TCP 正常退网 |
| WITHDRAWN/lifetime expiry | 虚拟时间、floor 和 owner `SIGKILL` 后多节点自然到期均已通过 |
| long refresh soak | `L=6000ms/R=2000ms/B=1000ms` 跨多个刷新周期长稳已通过 |
| TED/SPF/Zebra/Linux FIB | 独立 TED/SPF、nexthop、ifindex 和 ECMP 首跳集已完成；Zebra/ZAPI/Linux FIB adapter 与安装由第三组验收 |

## 6. 收口与交接

第二组负责的 R7-hardening 已完成。后续以 `13_midrd与第一组输入接口交接.md` 和 `14_midrd与第三组输出接口交接.md` 为边界，代码仓库内对应副本为 `doc/midr-doc/midr-first-group-input-handoff.md` 和 `doc/midr-doc/midr-third-group-output-handoff.md`：第一组实现真实 Prefix/Membership/Link/NDS 供数，第三组实现 Zebra/ZAPI/Linux FIB adapter，三组共同完成真实 IPv6 网验收。第二组测试对照见 `15_midrd与bgpd测试覆盖对照.md`，代码仓库内对应 `doc/midr-doc/midr-test-coverage-parity.md`。R5-full-B 在第二组范围内只验证传播、生命周期、LSDB/TED 和 SPF parity。`bgpd` 旧 MIDR 实现暂作对照保留，R7-CLEANUP 本轮不执行。
