# 第三组数据面迁移文档索引（dp-doc）

本目录记录 MIDR 第三组（数据面：Zebra/ZAPI/Linux FIB/GRE）从旧 `bgpd`
实现迁入独立 `midrd` 的迁移设计、构建测试与端到端场景。文档面向负责
`midrd/midr-dp-backend.*`、`midrd/midr-gre.*` 的第三组开发者。

## 1. 结论与范围

- **范围**：`midrd` 进程内的 Zebra 后端（`struct midr_zebra_backend_ops`
  的第三组实现）、GRE 虚拟接口下发、真实 ZAPI/FIB 与 GRE 连通性验收。
- **不在范围**：TED、SPF、diff、generation、READY/NOT_READY 策略由第二组
  `midrd/midr-spf-install.*` 承担（见
  `doc/midr-doc/14_midrd与第三组输出接口交接.md`）。
- **基线**：
  - 宿主仓库：`/Users/yangmengyu/githubdocuments/frr`，分支
    `feat/yhy-midr-single-instance-ls-flooding`，HEAD `e8956ea83e`。
  - 第三组数据面参考分支：兄弟检出 `/Users/yangmengyu/githubdocuments/frr/frr`，
    分支 `feat/te-dp-interface`，HEAD `f8f4e81f2f`（`gre interface`）。旧
    `bgpd/bgp_midr_zebra.c`、`bgpd/bgp_midr_gre.c` 原样保留为差分对照。
  - 验证容器：`frr-ubuntu24-ymy`，源码树 `/home/frr/frr-midrd3`。
- **当前总体状态**：数据面迁移与验收已完成（P0-P7 完成）；三节点
  containerlab 联合测试（第二组 + 第三组）已通过；仅剩单容器 loopback 形式
  `midrd/r7-dp-e2e-zapi.sh` 因第二组会话层未收敛而**待验证**（见第 6 节）。
- **接口契约**：
  - `doc/midr-doc/14_midrd与第三组输出接口交接.md`
  - `doc/midr-doc/MIDR-TED路径计算接口规范.md`

## 2. 文档清单

| 文档 | 内容 |
| --- | --- |
| `README-dp-migration.md` | 本索引、阶段状态、责任划分、已核实证据、待验证项、快速开始 |
| `dp-01-migration-overview.md` | 旧→新映射表、文件清单、边界规则 |
| `dp-02-build-and-test.md` | FRR 顶层与 midrd 组件构建、边界扫描、容器同步流程、日志 |
| `dp-03-e2e-and-integration.md` | 三个端到端场景与验收标准、覆盖对照 |
| `dp-04-gre.md` | GRE API、ZAPI 线格式路径、工具/脚本流程、与旧版差异 |

## 3. 责任划分

```text
第二组：committed TED -> SPF -> result cache -> route diff
                                                   |
                                      midr_zebra_* facade
                                                   |
第三组：                        pending/ZAPI -> Zebra -> Linux FIB
```

| 归属 | 负责内容 |
| --- | --- |
| 第二组 | 从 committed TED generation 生成完整双栈结果、缓存已接受 desired result、计算 add/replace/delete、metric-only 变化生成 `DELETE + ADD`、backend 拒绝时 abort 且不推进 generation、NOT_READY 撤销、提供 `midr_spf_install_resync()` / `midr_spf_install_replay()` |
| 第三组 | 独立 zclient、VRF、pending queue、installed hash、100 ms deferred timer；BASIC/ECMP/UCMP/SRv6 ZAPI 编码；深拷贝 add 输入；batch abort、deferred submit 与立即 flush；ZAPI 失败不更新 installed hash；Zebra 重连清 hash + replay；shutdown 清理 |

## 4. 阶段状态（P0..P9）

阶段编号沿用迁移计划；每条状态均附证据（见第 5 节与各分册）。

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| P0 | 迁移边界与清单 | 完成 | 
| P1 | 数据面分支 `f8f4e81f2f` 的 GRE 支撑复制进 `lib/log.c`、`lib/zclient.{c,h}`、`zebra/zapi_msg.c`、`zebra/zebra_dplane.{c,h}`、`zebra/if_netlink.c`、`zebra/kernel_netlink.c`；FRR 顶层构建 | 完成 |
| P2 | `midrd/midr-dp-backend.{c,h}`（五 ops、BASIC/ECMP/UCMP/SRv6 编码、双 instance、installed-hash 仅发成功后才更新、100 ms 批、有界重试 + resync、重连清 hash + replay） | 完成 |
| P3 | `midrd/midr-gre.{c,h}`（bgpd→midrd 移植，复用后端 zclient）+ 双容器 GRE/ip6gre 连通性 | 完成 |
| P4 | `midrd/dp-backend-test.c`、`midrd/Makefile`（`DP_OBJECTS` + 三个 harness/test 目标）、`Makefile.am`、`midrd/midrd.c` 接线 | 完成 |
| P5 | group-3 隔离真实 ZAPI/zebra-rib/Linux-FIB（`midrd/r7-dp-zapi-fib.sh`） | 完成 |
| P6 | 双容器 GRE/ip6gre 连通性（`midrd/midr-gre-connectivity-test.sh` + `midrd/gre-link-tool.c`） | 完成 |
| P7 | 集成/功能验收 | 完成：组件套件、group-3 FIB smoke、三节点 containerlab 集成、GRE 双容器均通过（见第 5 节） |
| P8 | 三节点 LS 洪泛 + SPF 收敛（`midrd/r7-dp-integration-smoke.sh`、`midrd/r7-dp-e2e-zapi.sh`） | containerlab 形式完成；单容器 loopback 形式待验证（第二组会话层，见第 6 节） |
| P9 | 文档与提交 | 进行中 |

> 说明：`r7-dp-integration-smoke.sh`（containerlab，每节点独立地址）已通过；
> 单容器 `r7-dp-e2e-zapi.sh`（三节点共用 `127.0.0.1`）仍因第二组会话层无法收敛
> （见第 6 节）。SRv6 内核 FIB 断言因容器无 `seg6` 模块被跳过（ZAPI/zebra 接受性
> 已验证）。

## 5. 已核实证据

以下结果均由协调者在容器 `frr-ubuntu24-ymy`（源码树 `/home/frr/frr-midrd3`）
今天执行得到。

### 5.1 FRR 顶层构建（P1）

```sh
cd /home/frr/frr-midrd3 && make -j112
```

摘要：**EXIT=0，0 errors / 0 warnings**。日志 `/tmp/verify-build.log`（容器
`frr-ubuntu24-ymy`，源码树 `/home/frr/frr-midrd3`）。

### 5.2 midrd 组件套件（P2/P4）

```sh
cd /home/frr/frr-midrd3/midrd && make -j112 BUILD_DIR=/tmp/midrd-build test
```

摘要：**25 个程序 PASS**，含 `midrd-dp-test: PASS`、
`standalone libfrr boundary scan: PASS`，**EXIT=0**。日志 `/tmp/verify-comp.log`、
`/tmp/final-comp.log`。

该单测发现并修复了 deferred-batch 发送失败恢复缺陷：`update_deferred()` 返回
成功后批仍可能在定时器里发送失败，而 adapter 已推进 generation，故单纯的
`midr_spf_install_resync()` 是 no-op；修复为先重试保留的 pending 批、成功后再调
resync。覆盖见 `midrd/dp-backend-test.c` 的
`test_adapter_pipeline_and_recovery()`。

### 5.3 GRE 双容器连通性（P3/P6）

```sh
# docker 宿主上执行
bash midrd/midr-gre-connectivity-test.sh
```

摘要：**`=== summary: PASS=24 FAIL=0 ===`**。新 harness（真实 zebra + 真实
netdevice）：`node1` 源码树 `midrd/gre-link-tool.c` + 新构建的 zebra 被 staged 到
`node1`/`node2` 的 `/opt/midr-dp`；两节点 zebra 运行、underlay IPv4 可达；
IPv4 GRE（`gre`）与 IPv6 GRE（`ip6gre`）均建立（`state=up` 且回报 `ifindex`），
`ip -d link show` kind 正确；IPv4 与 IPv6 overlay 流量经该隧道双向 ping 通；
IPv6 GRE 建立并承载 IPv6 流量；重复 add / 状态查询幂等；teardown 在两节点删除
`gre1`/`gre6`。日志 `/tmp/gre-test.log`（宿主）。

### 5.4 group-3 隔离真实 ZAPI/zebra-rib/Linux-FIB（P5）

```sh
# 容器 frr-ubuntu24-ymy 内执行
bash midrd/r7-dp-zapi-fib.sh
```

摘要：**PASS=29 FAIL=0 EXIT=0**。日志 `/tmp/fib-final.log`，分步日志
`/tmp/midrd-dp-zapi-fib/run/`。该脚本不依赖第二组 LS 洪泛。覆盖：IPv4 add 到达
内核 FIB（递归 nexthop，proto 199 + `ip route get`）；相同 re-add 为 no-op
（adds/dels 计数不变）；nexthop 变化触发替换（旧 DEL + 新 ADD，旧 nexthop 消失）；
ECMP（2 nexthop）与 UCMP（加权）进入 FIB；TE instance 叠加在 SPF instance 上、
TE 删除后 SPF 条目恢复；IPv6 add 到达 IPv6 FIB（`fd00:100::/64` via
`fd00:200::2`）；SRv6 path result 被 ZAPI/zebra 路径接受；单客户端
add→dwell→del 从 FIB 撤销；以及完整链路合成 TED → SPF → adapter → backend →
ZAPI → zebra → FIB（含 zebra 重启 replay 与确定性 shutdown 撤销、installed=0）。

两个已记录的待办（脚本输出中的 `OPEN ITEM`/`[SKIP]`）：
- (a) 本环境中 zebra 在 TE add 后仍保留 SPF 条目为选中项。双实例共存已在 ZAPI
  层与本单测验证，但 TE instance 的 FIB 提升需 zebra 侧后续处理
  （`r7-dp-zapi-fib.sh` 第 261 行 `OPEN ITEM`）。
- (b) 本容器内核无 `seg6` 模块，SRv6 的**内核 FIB** 断言被跳过
  （`[SKIP] kernel seg6 module absent`，脚本第 286-300 行）；ZAPI/zebra 接受性
  已验证。

### 5.5 第三组 ZAPI/FIB smoke（P5/P7）

```sh
# 容器 frr-ubuntu24-ymy 内，root
bash /home/frr/frr-midrd3/midrd/r7-dp-fib-smoke.sh
```

摘要：**`=== summary: PASS=15 FAIL=0 ===`**，末行
`third-group ZAPI/FIB smoke: PASS`，**EXIT=0**。日志 `/tmp/fib-smoke.log`。该脚本
在构建容器内以 root 运行真实 zebra + 真实内核 FIB，**不依赖第二组会话层**。
用例与观测：

- **A 直接 facade add/delete（递归 nexthop）**：`DP action=cycle4-add
  prefix=10.10.0.0/24 rc=0 installed=1 adds=1 dels=0 fails=0`；FIB
  `10.10.0.0/24 nhid 52 via 192.168.200.2 dev midr-dp0 metric 20`（proto 199）；
  `ip route get 10.10.0.1` 经 dummy underlay 解析；客户端退出后路由撤销。
- **B 完整 TED → SPF → adapter → backend → ZAPI → zebra → FIB（`serve-ted`）**：
  合成 committed TED（link 1->2 nexthop 192.0.200.2，节点前缀 198.51.100.0/24）→
  FIB `198.51.100.0/24 via 192.0.200.2`；停止并重启 zebra 后重连 replay 恢复该
  路由；SIGTERM 撤销（unregister + 立即 flush）。状态
  `route-adds=2 route-deletes=1 send-failures=0 replays=2 last-error=0`。
- **C 形状**：ECMP → `10.20.0.0/24 nhid 75`，nexthop group
  `id 75 group 76/77`、`id 76 via 192.168.200.2`、`id 77 via 192.168.200.3`；
  UCMP → `id 81 group 76,255/77,85`（加权）；IPv6 → `fd00:10::/64` 经
  `id 83 via fd00:200::2 dev midr-dp6`（IPv6 检查须用
  `ip -6 route show proto 199`）。
- **D 去重与替换**：`DP dup4 adds_first=1 adds_second=1`（相同 add 不重发）；
  替换把 FIB 的 nexthop 切换为新值。

> **FIB 断言必须同时解析两种安装形式**：zebra 可能内联安装（`via N`）或经
> nexthop group（`nhid N`）安装，故断言须用 `ip nexthop show id <nhid>` 并跟随
> `group` 列表展开（`midrd/r7-dp-fib-smoke.sh` 第 80-101 行）。只匹配单一形式曾
> 造成多次误报失败。

### 5.6 三节点 containerlab 联合测试（group 2 + group 3，P8）

```sh
# docker 宿主，root（containerlab + docker）
bash midrd/r7-dp-integration-smoke.sh
```

摘要：**`=== summary: PASS=14 FAIL=0 ===`**，**EXIT=0**。日志
`/tmp/verify-integration.log`。拓扑 A-B-C（`10.77.x.x/30`），节点镜像
`frr-ubuntu24-ymy:init`，从构建容器绑定新构建的 zebra + midrd。证据：三个节点均
记录 `spf generation=... routes=3`；每节点 proto-199 FIB 携带两条远端前缀与期望
下一跳（例：节点 a `10.0.2.2 nhid 17 via 10.77.1.2 dev eth1` 与
`10.0.3.3 nhid 17 via 10.77.1.2 dev eth1`）；节点 b 向 c 转发；双边端到端 ping 经
MIDR 路径通过：`ping a -> 10.0.3.3 (c prefix over b) (source 10.0.1.1 over eth1)`
与 `ping c -> 10.0.1.1 (a prefix over b) (source 10.0.3.3 over eth1)`。测试设计
要点（绑定源、`-u 0`、`--link-addr`）见 `dp-03-e2e-and-integration.md` 第 3.3 节。

### 5.7 设计/实现要点（验收期间的数据面行为变化）

以下为验收测试期间做出并已验证的数据面行为调整（均附实现位置与原因）：

- **冷路径立即提交**：后端启动后及每次 zebra 重连后的第一批立即提交，不再等待
  100 ms 窗口（`midrd/midr-dp-backend.c` 的 `immediate_next`，第 834-838、938、
  953 行）。原因是冷路径没有拓扑抖动要吸收、首批路由不应空等一个窗口；稳态路径
  仍保留 100 ms 延迟窗口以吸收抖动，故 `update_deferred` 对 adapter 的语义不变。
- **仅对投递失败重试**：`-EIO/-ENOTCONN/-ENOBUFS/-EAGAIN` 重新进入有界重试恢复；
  确定性编码失败（`-EHOSTUNREACH`、`-EINVAL`、`-E2BIG`、`-EAFNOSUPPORT`）丢弃
  不完整批并经 status 上报，不再空转到重试预算耗尽（`midrd/midr-dp-backend.c`
  `dp_error_retryable()`，第 453-470 行）。
- **延迟批失败恢复**：先重试保留的 pending 批（它正是 diff 的未应用余量），成功
  后再调用 `midr_spf_install_resync()`。原因是延迟失败时 adapter 已推进 desired
  generation，仅靠 resync 是 no-op、会留下陈旧 FIB 状态——该缺陷由
  `midrd/dp-backend-test.c` 实际发现并覆盖。

## 6. 待验证项：单容器 loopback E2E 依赖第二组会话层

三节点 **containerlab** 联合测试（每节点独立地址）已通过（见 5.6）。仅剩单容器
形式 `midrd/r7-dp-e2e-zapi.sh`（三个 midrd 进程共用 loopback `127.0.0.1`，两侧
`--peer`）在本环境中**无法收敛**，仍标记为**待验证**，归属第二组：

- 会话建立后立即被重置成风暴：节点 101 在 60 s 内 **799 次 `closed` / 797 次
  `established`**，关闭原因 `-104`（ECONNRESET）。
- SPF 始终达不到 `routes=3`（对照 `midrd/r7-native-smoke.sh` 第 82 行断言）。
- 禁用第三组后端（`midrd --no-zebra`）时**同样复现**，故**不是数据面回归**。
- 代码指针：`midrd/midr-transport.c` 第 203 行 `find_peer_by_address()` 仅按 IP
  匹配入向连接（多个 peer 共用同一地址如 loopback 时歧义）；随后
  `midrd/midr-session.c` 第 218 行 `hello_receive()` 在“绑定到 peer X 的连接
  携带另一节点 HELLO”时以 `MIDR_SESSION_IDENTITY_MISMATCH`（`-EEXIST`，第 231
  行）拆除连接。
- containerlab 形式（地址各不相同）与 FIB smoke（无会话层）均通过，故第三组
  验收**不依赖**该单容器形式。本文档不对该单容器形式做任何通过性声明。

## 7. 快速开始

```sh
# 1) FRR 顶层构建（容器内，源码树 /home/frr/frr-midrd3）
cd /home/frr/frr-midrd3 && make -j112                      # 期望 EXIT=0

# 2) midrd 组件套件（含 dp 单测与边界扫描）
cd /home/frr/frr-midrd3/midrd
make -j112 BUILD_DIR=/tmp/midrd-build test                  # 期望 25 程序 + boundary scan 全 PASS

# 3) group-3 隔离真实 ZAPI/FIB（容器内）
bash /home/frr/frr-midrd3/midrd/r7-dp-zapi-fib.sh           # 期望 PASS=29 FAIL=0

# 4) 第三组 ZAPI/FIB smoke（容器内，root；不依赖会话层）
bash /home/frr/frr-midrd3/midrd/r7-dp-fib-smoke.sh          # 期望 PASS=15 FAIL=0

# 5) 三节点 containerlab 联合测试（docker 宿主，root；containerlab + docker）
bash midrd/r7-dp-integration-smoke.sh                       # 期望 PASS=14 FAIL=0

# 6) GRE 双容器连通性（docker 宿主；node1/node2 已就绪）
bash midrd/midr-gre-connectivity-test.sh                    # 期望 PASS=24 FAIL=0
```

详细构建、日志位置与容器同步步骤见 `dp-02-build-and-test.md`。

## 8. 证据索引

| 验证 | 结果摘要 | 日志 |
| --- | --- | --- |
| FRR 顶层构建 | EXIT=0，0 errors/0 warnings | 容器 `/tmp/verify-build.log` |
| midrd 组件套件 | 25 程序 PASS，EXIT=0 | 容器 `/tmp/verify-comp.log`、`/tmp/final-comp.log` |
| 第三组 ZAPI/FIB smoke | PASS=15 FAIL=0 EXIT=0 | 容器 `/tmp/fib-smoke.log` |
| 三节点 containerlab 联合测试 | PASS=14 FAIL=0 EXIT=0 | 宿主 `/tmp/verify-integration.log` |
| group-3 隔离 ZAPI/FIB | PASS=29 FAIL=0 EXIT=0 | 容器 `/tmp/fib-final.log`、`/tmp/midrd-dp-zapi-fib/run/` |
| GRE 双容器连通性 | PASS=24 FAIL=0 | 宿主 `/tmp/gre-test.log` |
| 单容器 loopback E2E | 待验证（第二组会话层） | `r7-dp-e2e-zapi.sh`；节点日志 closed/established 统计 |
| deferred-batch 恢复缺陷 | 已修复并覆盖 | `midrd/dp-backend-test.c` `test_adapter_pipeline_and_recovery()` |

