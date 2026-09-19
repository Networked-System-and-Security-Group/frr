# dp-03 端到端与集成场景

## 1. 结论

数据面端到端验收分三类：group-3 隔离真实 ZAPI/zebra-rib/Linux-FIB、
双容器 GRE/ip6gre 连通性、三节点 group2+group3 集成。

- **group-3 隔离 FIB 测试**：`midrd/r7-dp-zapi-fib.sh` **PASS=29 FAIL=0 EXIT=0**；
  另有第三组 ZAPI/FIB smoke `midrd/r7-dp-fib-smoke.sh` **PASS=15 FAIL=0 EXIT=0**，
  两者均不依赖第二组会话层。
- **双容器 GRE 连通性**：`midrd/midr-gre-connectivity-test.sh`
  **`=== summary: PASS=24 FAIL=0 ===`**。
- **三节点集成**：`midrd/r7-dp-integration-smoke.sh`（containerlab）
  **`=== summary: PASS=14 FAIL=0 ===`**，EXIT=0；仅单容器
  `midrd/r7-dp-e2e-zapi.sh`（loopback）**待验证（第二组会话层）**。

## 2. 范围与脚本清单

| 脚本 | 场景 | 运行位置 | 状态 |
| --- | --- | --- | --- |
| `midrd/r7-dp-zapi-fib.sh` | group-3 隔离真实 ZAPI/zebra-rib/Linux-FIB | 容器 `frr-ubuntu24-ymy`（root） | 完成（PASS=29） |
| `midrd/r7-dp-fib-smoke.sh` | 第三组 ZAPI/FIB smoke（真实 zebra + 内核 FIB，无会话层） | 容器 `frr-ubuntu24-ymy`（root） | 完成（PASS=15） |
| `midrd/midr-gre-connectivity-test.sh` | 双容器 GRE/ip6gre 连通性 | docker 宿主（root） | 完成（PASS=24） |
| `midrd/r7-dp-integration-smoke.sh` | 三节点 containerlab 集成 | docker 宿主 + containerlab | 完成（PASS=14） |
| `midrd/r7-dp-e2e-zapi.sh` | 单容器 3×midrd + zebra 端到端（loopback） | 容器 | 待验证（第二组会话层） |

辅助 harness：
- `midrd/dp-e2e-tool.c`（构建产物 `midrd-dp-e2e-tool`）：驱动第三组公开 facade
  `midr_zebra_route_add/del/flush`，模式含 `add4/add6`、`add4-ecmp`、`add4-ucmp`、
  `add6-srv6`、`add4-te`、`del`、`status`、`serve-ted`；每条动作打印机器可读行
  `DP action=... prefix=... rc=... installed=... adds=... dels=... fails=...`。
- `midrd/gre-link-tool.c`（构建产物 `midrd-gre-tool`）：微型 midrd 宿主，用于
  双容器 GRE 场景。

## 3. 场景

### 3.1 场景 A：group-3 隔离真实 ZAPI/zebra-rib/Linux-FIB（完成）

| 项 | 内容 |
| --- | --- |
| 目标 | 不依赖第二组 LS 洪泛，验证第三组编码/installed-hash/双实例/Linux FIB |
| 命令 | `docker exec -u 0 frr-ubuntu24-ymy bash /home/frr/frr-midrd3/midrd/r7-dp-zapi-fib.sh` |
| 结果 | **PASS=29 FAIL=0 EXIT=0** |
| 日志 | `/tmp/fib-final.log`；分步日志 `/tmp/midrd-dp-zapi-fib/run/` |

脚本启动真实、新构建的 zebra 于私有 ZAPI socket，并用 `midrd-dp-e2e-tool`
驱动。覆盖与验收：

| # | 观测 | 结果 |
| --- | --- | --- |
| A1 | IPv4 add 到达内核 FIB（递归 nexthop，proto 199 + `ip route get`） | 通过 |
| A2 | 相同 re-add 为 no-op（adds/dels 计数不变） | 通过 |
| A3 | nexthop 变化触发替换（旧 DEL + 新 ADD，旧 nexthop 消失） | 通过 |
| A4 | ECMP（2 nexthop）进入 FIB | 通过 |
| A5 | UCMP（加权）进入 FIB | 通过 |
| A6 | TE instance 叠加在 SPF instance 上、TE 删除后 SPF 条目恢复 | 通过（见待办 a） |
| A7 | IPv6 add 到达 IPv6 FIB（`fd00:100::/64` via `fd00:200::2`） | 通过 |
| A8 | SRv6 path result 被 ZAPI/zebra 路径接受 | 通过（内核断言见待办 b） |
| A9 | 单客户端 add→dwell→del 从 FIB 撤销 | 通过 |
| A10 | 合成 TED → SPF → adapter → backend → ZAPI → zebra → FIB；含 zebra 重启 replay、确定性 shutdown 撤销（installed=0） | 通过 |

```sh
# 容器 frr-ubuntu24-ymy 内，root
docker exec -u 0 frr-ubuntu24-ymy bash /home/frr/frr-midrd3/midrd/r7-dp-zapi-fib.sh
# 摘要行: PASS=29 FAIL=0 (logs: /tmp/midrd-dp-zapi-fib)
```

已记录待办（脚本输出）：

- **(a)** 本环境中 zebra 在 TE add 后仍保留 SPF 条目为选中项；双实例共存已在
  ZAPI 层与单测验证，但 TE instance 的 FIB 提升需 zebra 侧后续处理
  （脚本第 261 行 `OPEN ITEM`）。
- **(b)** 本容器内核无 `seg6` 模块，SRv6 的**内核 FIB** 断言被跳过
  （脚本第 286-300 行 `[SKIP] kernel seg6 module absent`）；ZAPI/zebra 接受性
  已验证。

**场景 A2：第三组 ZAPI/FIB smoke（`midrd/r7-dp-fib-smoke.sh`，完成）**

| 项 | 内容 |
| --- | --- |
| 目标 | 构建容器内用真实 zebra + 真实内核 FIB 验证完整 ZAPI/FIB 链路，**不依赖第二组会话层** |
| 命令 | `docker exec -u 0 frr-ubuntu24-ymy bash /home/frr/frr-midrd3/midrd/r7-dp-fib-smoke.sh` |
| 结果 | **`=== summary: PASS=15 FAIL=0 ===`**，末行 `third-group ZAPI/FIB smoke: PASS`，EXIT=0 |
| 日志 | `/tmp/fib-smoke.log`（容器 `frr-ubuntu24-ymy`） |

用例与观测：

| # | 观测 | 结果 |
| --- | --- | --- |
| S1 | 直接 facade add/delete（递归 nexthop）：`DP action=cycle4-add prefix=10.10.0.0/24 rc=0 installed=1 adds=1 dels=0 fails=0`；FIB `10.10.0.0/24 nhid 52 via 192.168.200.2 dev midr-dp0 metric 20`（proto 199）；`ip route get 10.10.0.1` 经 dummy underlay 解析；客户端退出后撤销 | 通过 |
| S2 | 完整 TED → SPF → adapter → backend → ZAPI → zebra → FIB（`serve-ted`，合成 committed TED：link 1->2 nexthop 192.0.200.2，节点前缀 198.51.100.0/24）：FIB `198.51.100.0/24 via 192.0.200.2`；zebra 停止重启后重连 replay 恢复；SIGTERM 撤销（unregister + 立即 flush）。状态 `route-adds=2 route-deletes=1 send-failures=0 replays=2 last-error=0` | 通过 |
| S3 | ECMP → `10.20.0.0/24 nhid 75`，group `id 75 group 76/77`、`id 76 via 192.168.200.2`、`id 77 via 192.168.200.3` | 通过 |
| S4 | UCMP 加权 → `id 81 group 76,255/77,85` | 通过 |
| S5 | IPv6 → `fd00:10::/64` 经 `id 83 via fd00:200::2 dev midr-dp6`（须用 `ip -6 route show proto 199` 检查） | 通过 |
| S6 | 去重 `DP dup4 adds_first=1 adds_second=1`（相同 add 不重发）与替换切换 nexthop | 通过 |

> **FIB 断言须同时解析两种安装形式（group-aware）**：zebra 既可能内联安装
> （路由行含 `via N`），也可能经 nexthop group 安装（路由行含 `nhid N`）。断言
> 必须先取 `nhid`，用 `ip nexthop show id <nhid>` 展开，并跟随 `group` 列表继续
> 展开成员 nexthop（`r7-dp-fib-smoke.sh` 第 80-101 行 `fib_nhid()`/`fib_nh_list()`）。
> 只匹配单一形式曾造成多次误报失败。

### 3.2 场景 B：双容器 GRE/ip6gre 连通性（完成）

| 项 | 内容 |
| --- | --- |
| 拓扑 | `node1` 10.1.1.11 ↔ docker bridge ↔ `node2` 10.1.1.12；overlay `192.168.100.0/30`、`fd00:100::/64`；ip6gre overlay `fd00:200::/64` |
| 命令 | docker 宿主上 `bash midrd/midr-gre-connectivity-test.sh`（root） |
| 结果 | **`=== summary: PASS=24 FAIL=0 ===`** |
| 日志 | 宿主 `/tmp/gre-test.log` |

```sh
# docker 宿主，root
bash midrd/midr-gre-connectivity-test.sh
# 摘要行: === summary: PASS=24 FAIL=0 ===（日志 /tmp/gre-test.log）
```

前置：`node1`/`node2` 已存在并运行 `frr-ubuntu24-ymy:init`，且在同一 bridge；
脚本把构建容器内新构建的 zebra、libfrr 与 `midrd-gre-tool` staged 到两节点的
`/opt/midr-dp`（保留旧 `/opt/midr` 资产不动）。

覆盖与验收：

| 用例 | 观测 | 结果 |
| --- | --- | --- |
| A | 构建产物 staged 进两节点；两节点 zebra 运行 | 通过 |
| B | underlay IPv4 可达 | 通过 |
| C | IPv4 GRE（`gre`）在两节点建立（`state=up` 且回报 `ifindex`，`ip -d link show` kind 正确） | 通过 |
| D | IPv4 与 IPv6 overlay 流量经该隧道双向 ping 通 | 通过 |
| E | IPv6 GRE（`ip6gre`）建立并承载 IPv6 流量 | 通过 |
| F | 重复 add / 状态查询幂等 | 通过 |
| G | teardown 在两节点删除 `gre1`/`gre6` | 通过 |

### 3.3 场景 C：三节点 containerlab 集成（完成）与单容器 E2E（待验证）

三节点 **containerlab** 联合测试 `midrd/r7-dp-integration-smoke.sh` 已通过；
单容器 `midrd/r7-dp-e2e-zapi.sh` 仍**待验证**（第二组会话层）。

**C1 三节点 containerlab 集成（完成）**

| 项 | 内容 |
| --- | --- |
| 拓扑 | A(10.77.1.1) === B(10.77.1.2 / 10.77.2.1) === C(10.77.2.2)，节点镜像 `frr-ubuntu24-ymy:init` |
| 命令 | docker 宿主上（root；containerlab + docker，从构建容器绑定新构建的 zebra + midrd）`bash midrd/r7-dp-integration-smoke.sh` |
| 结果 | **`=== summary: PASS=14 FAIL=0 ===`**，EXIT=0 |
| 日志 | 宿主 `/tmp/verify-integration.log` |

证据：三个节点均记录 `spf generation=... routes=3`；每节点 proto-199 FIB 携带两条
远端前缀与期望下一跳（例：节点 a `10.0.2.2 nhid 17 via 10.77.1.2 dev eth1` 与
`10.0.3.3 nhid 17 via 10.77.1.2 dev eth1`）；节点 b 向 c 转发；双边端到端 ping 经
MIDR 路径通过：`ping a -> 10.0.3.3 (c prefix over b) (source 10.0.1.1 over eth1)`
与 `ping c -> 10.0.1.1 (a prefix over b) (source 10.0.3.3 over eth1)`。

**C1 测试设计要点（曾造成误报结果，必须遵守）**

- **ping 源必须是节点自身的 MIDR 前缀**（如 `10.0.1.1`），**不能**用点对点链路
  地址：链路地址对远端不可路由，中转应答会经 containerlab 管理默认路由离开导致
  ping 失败。绑定源并断言 `ip route get <dst> from <src>` 经数据链路解析，才使该
  测试有意义。
- **裸 ping 是假阳性**：MIDR 前缀是 `lo` 上的 /32，节点会在任意接口为它们应答
  ARP，containerlab 管理网桥可能代替 MIDR 路径应答（观测到
  `ip route get 10.0.1.1` -> `via 172.20.20.1 dev eth0`）。**必须绑定源。**
- **诊断须用 `docker exec -u 0`**：containerlab 节点内普通 `docker exec` 以镜像
  用户 `frr` 运行，而 ZAPI socket `/tmp/zserv.api` 为 `srwx------ root`，非 root
  连接被拒（曾产生误导性的 “zclient did not connect”）。
- **输入需含邻居地址**：开发期静态 link 需邻居地址，否则 SPF 结果 nexthop 为空，
  后端正确地拒绝安装黑洞（返回 `-EHOSTUNREACH`）。这就是 `midrd/midrd.c` 新增
  向后兼容选项 `--link-addr NODE:ADDR` 的原因（配合集成脚本拓扑）；无该选项时
  即使洪泛/SPF 收敛，FIB 仍为空（`send-failures=16 last-error=-113`）。

**C2 单容器 E2E（待验证，归第二组）**

| 项 | 内容 |
| --- | --- |
| 脚本 | `midrd/r7-dp-e2e-zapi.sh`（单容器 3×midrd + zebra，三进程共用 loopback `127.0.0.1`，两侧 `--peer`） |
| 状态 | **待验证（第二组会话层）**，当前无法收敛 |
| 原因 | 见 `README-dp-migration.md` 第 6 节 |

```sh
# 目前无法验证（依赖第二组会话层收敛）
bash midrd/r7-dp-e2e-zapi.sh            # 单容器 loopback
```

证据：会话建立后立即重置成风暴——节点 101 在 60 s 内 **799 次 `closed` / 797 次
`established`**（关闭原因 `-104`/ECONNRESET）；SPF 达不到 `routes=3`。禁用第三组
后端（`midrd --no-zebra`）时同样复现，故**不是数据面回归**。根因指向
`midrd/midr-transport.c` `find_peer_by_address()`（第 203 行，仅按 IP 匹配入向
连接，loopback 多 peer 时歧义）与 `midrd/midr-session.c` `hello_receive()`
（绑定 peer X 的连接携带另一节点 HELLO 时返回 `MIDR_SESSION_IDENTITY_MISMATCH`
`-EEXIST`）。containerlab 形式（地址各不相同）与 FIB smoke（无会话层）均通过，
故第三组验收**不依赖**该形式。**本文档不对该单容器形式做任何通过性声明。**

## 4. 覆盖对照（相对旧 DP 报告）

| 旧用例/报告 | 旧覆盖内容 | 新对应 | 状态 |
| --- | --- | --- | --- |
| `test-steps/01-bgpd-build-verification.md` | `bgp_midr_zebra.{c,h}` 可编译 | FRR 顶层 + midrd 组件构建（`dp-02`） | 完成 |
| `test-steps/02-netlink-c-proto199.md` | 内核识别 `RTPROT_BGP_MIDR = 199` | 场景 A（`ip route get` + proto 199） | 完成 |
| `test-steps/03-unit-tests.md` | mock `zclient_route_send` 的 DP 单测 | `midrd/dp-backend-test.c` | 完成 |
| `test-steps/04-e2e-zapi.md` | 真实 ZAPI：SPF 单路由 + Dual-Instance SPF+TE SRv6，FIB 验证/删除 | 场景 A（含 SPF/TE、SRv6 接受性、replay、shutdown） | 完成（内核 SRv6 断言因无 `seg6` 跳过） |
| `test-steps/05-zapi-batch-stress.md` | 128 路由批量安装/删除、10 前缀双实例共存 | 场景 A 的 ECMP/UCMP/双实例子集 | 部分覆盖（批量压力待补） |
| `test-steps/06-zapi-clab-connectivity.md` | 2 节点 clab blackhole/redirect/stress 连通性 | 场景 C（三节点真实转发，containerlab） | 完成（PASS=14） |
| `midr-gre-test-report.md` | 双容器 GRE/ip6gre 21/21 PASS、状态 API 返回建立成功 | 场景 B（`midrd` GRE API + `midrd-gre-tool`） | 完成（PASS=24） |
| `midrd/r7-dp-e2e-zapi.sh` 的 `--no-zebra` 对照 | 旧报告不含 | 场景 C | 待验证（单容器 loopback 形式） |

差异点：新场景增加了 `--no-zebra` 对照、zebra 重启 replay（清 installed hash +
`midr_spf_install_replay()`）、以及 deferred-batch 发送失败两步恢复。组件层恢复
由 `dp-backend-test.c` 覆盖，真实 ZAPI 层由 `r7-dp-fib-smoke.sh`（场景 A2）与
三节点 containerlab（场景 C1）覆盖；单容器 loopback 形式仍待验证。

## 5. 证据

| 项 | 依据 |
| --- | --- |
| group-3 隔离 FIB | `midrd/r7-dp-zapi-fib.sh` PASS=29 FAIL=0 EXIT=0；日志 `/tmp/fib-final.log`、`/tmp/midrd-dp-zapi-fib/run/` |
| 第三组 ZAPI/FIB smoke | `midrd/r7-dp-fib-smoke.sh` PASS=15 FAIL=0 EXIT=0；容器日志 `/tmp/fib-smoke.log` |
| 三节点 containerlab 集成 | `midrd/r7-dp-integration-smoke.sh` PASS=14 FAIL=0 EXIT=0；宿主日志 `/tmp/verify-integration.log` |
| GRE 双容器 | `midrd/midr-gre-connectivity-test.sh` PASS=24 FAIL=0；宿主日志 `/tmp/gre-test.log` |
| harness | `midrd/dp-e2e-tool.c`、`midrd/gre-link-tool.c` |
| FIB 断言（group-aware） | `midrd/r7-dp-fib-smoke.sh` 第 80-101 行 `fib_nhid()`/`fib_nh_list()` |
| 待验证证据 | `midrd/r7-dp-e2e-zapi.sh`（loopback），`midrd --no-zebra` 同样不收敛；`midrd/midr-transport.c` 第 203 行、`midrd/midr-session.c` 第 218/231 行 |
| 旧覆盖基线 | `frr/frr/doc/midr-doc/midr-gre-test-report.md`、`frr/frr/doc/midr-doc/test-steps/01..06` |

