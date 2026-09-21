# dp-03 端到端与集成场景

## 1. 结论

数据面端到端验收分三类：group-3 隔离真实 ZAPI/zebra-rib/Linux-FIB、
双容器 GRE/ip6gre 连通性、三节点 group2+group3 集成。

- **group-3 隔离 FIB 测试**：`midrd/r7-dp-zapi-fib.sh` **PASS=29 FAIL=0 EXIT=0**；
  另有第三组 ZAPI/FIB smoke `midrd/r7-dp-fib-smoke.sh` **PASS=15 FAIL=0 EXIT=0**，
  两者均不依赖第二组会话层。
- **双容器 GRE 连通性**：`midrd/midr-gre-connectivity-test.sh`
  **`=== summary: PASS=24 FAIL=0 ===`**。
- **三节点集成**：`midrd/r7-dp-integration-smoke.sh`（containerlab）在同一 A-B-C
  拓扑上先跑 IPv4、再以 IPv6 点对点链路与 IPv6 MIDR 前缀在独立 lab 重跑，
  **`=== summary: PASS=31 FAIL=0 ===`**（IPv4 14 + IPv6 17），EXIT=0；单容器
  `midrd/r7-dp-e2e-zapi.sh`（loopback）也已通过（group-2 传输层修复后，
  见第 3.3 节 C2）。

## 2. 范围与脚本清单

| 脚本 | 场景 | 运行位置 | 状态 |
| --- | --- | --- | --- |
| `midrd/r7-dp-zapi-fib.sh` | group-3 隔离真实 ZAPI/zebra-rib/Linux-FIB | 容器 `frr-ubuntu24-ymy`（root） | 完成（PASS=29） |
| `midrd/r7-dp-fib-smoke.sh` | 第三组 ZAPI/FIB smoke（真实 zebra + 内核 FIB，无会话层） | 容器 `frr-ubuntu24-ymy`（root） | 完成（PASS=15） |
| `midrd/midr-gre-connectivity-test.sh` | 双容器 GRE/ip6gre 连通性 | docker 宿主（root） | 完成（PASS=24） |
| `midrd/r7-dp-integration-smoke.sh` | 三节点 containerlab 集成（IPv4 + IPv6） | docker 宿主 + containerlab | 完成（PASS=31 = IPv4 14 + IPv6 17） |
| `midrd/r7-dp-e2e-zapi.sh` | 单容器 3×midrd + zebra 端到端（loopback） | 容器 | 完成（group-2 传输层修复后，见 3.3 C2） |

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
| A6 | TE instance 叠加在 SPF instance 上、TE 删除后 SPF 条目恢复 | 通过（TE 已在真实内核 FIB 胜出，见 P7 收尾） |
| A7 | IPv6 add 到达 IPv6 FIB（`fd00:100::/64` via `fd00:200::2`） | 通过 |
| A8 | SRv6 path result 被 ZAPI/zebra 路径接受 | 通过（H.Insert 已到达真实内核 FIB，见 P7 收尾） |
| A9 | 单客户端 add→dwell→del 从 FIB 撤销 | 通过 |
| A10 | 合成 TED → SPF → adapter → backend → ZAPI → zebra → FIB；含 zebra 重启 replay、确定性 shutdown 撤销（installed=0） | 通过 |

```sh
# 容器 frr-ubuntu24-ymy 内，root
docker exec -u 0 frr-ubuntu24-ymy bash /home/frr/frr-midrd3/midrd/r7-dp-zapi-fib.sh
# 摘要行: PASS=29 FAIL=0 (logs: /tmp/midrd-dp-zapi-fib)
```

该脚本**首次运行**在脚本输出中记录了两个待办（`OPEN ITEM`/`[SKIP]`），已在
**P7 收尾**于真实 zebra + 真实内核 FIB 上复验通过（容器 `frr-ubuntu24-ymy`，
日志 `/tmp/closeout.log`）：

- **(a) 双实例 TE 已在真实内核 FIB 提升 TE**：SPF instance 与 TE instance 同装
  `10.60.0.0/24` 时，内核 FIB 为 TE nexthop
  `10.60.0.0/24 nhid 33 via 192.168.200.3 dev midr-dp0 metric 20`（TE 胜出），
  工具报告 `DP action=add4-te ... rc=0 installed=1 adds=1`。（首次运行的本环境
  内核曾保留 SPF 条目，脚本第 261 行 `OPEN ITEM`。）
- **(b) SRv6 H.Insert 已到达真实内核**：内核 FIB
  `fd00:60::/64 nhid 35 encap seg6 mode inline segs 2 [ 2001:db8:100::1
  2001:db8:100::2 ] via fd00:200::2 dev midr-dp6 metric 20 pref medium`，工具
  报告 `DP action=add6-srv6 ... rc=0 installed=1 adds=1`。（首次运行的本容器
  内核无 `seg6` 模块、内核断言被跳过，脚本第 286-300 行
  `[SKIP] kernel seg6 module absent`。）
- **proto-199 内核隔离**：`tests/bgpd/test_midr_proto199.c`
  `=== ALL CHECKS PASSED (proto 199 = midr verified) ===`。

> **真实环境未覆盖（待验证）**：`-ENOBUFS` 类发送失败注入仅在单测
> `midrd/dp-backend-test.c` 覆盖（被包裹（wrap）的 `zclient_route_send` 返回失败 ⇒
> installed hash 不更新、保留批重试/重同步）；真实环境已覆盖的失败路径是
> zebra stop/restart 重连 replay 与 SIGTERM 撤销（`r7-dp-fib-smoke.sh` case B
> 与集成撤销检查）。

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

### 3.3 场景 C：三节点 containerlab 集成与单容器 E2E（均完成）

三节点 **containerlab** 联合测试 `midrd/r7-dp-integration-smoke.sh` 与单容器
`midrd/r7-dp-e2e-zapi.sh` 均已通过（后者在 group-2 传输层修复后）。

**C1 三节点 containerlab 集成（完成，IPv4 + IPv6）**

| 项 | 内容 |
| --- | --- |
| 拓扑 | IPv4：A(10.77.1.1) === B(10.77.1.2 / 10.77.2.1) === C(10.77.2.2)；IPv6：A(`2001:db8:77:1::1`) === B(`2001:db8:77:1::2` / `2001:db8:77:2::1`) === C(`2001:db8:77:2::2`)，在独立 lab `midr-dp-int6` 重跑，节点镜像 `frr-ubuntu24-ymy:init` |
| 命令 | docker 宿主上（root；containerlab + docker，从构建容器绑定新构建的 zebra + midrd）`bash midrd/r7-dp-integration-smoke.sh` |
| 结果 | **`=== summary: PASS=31 FAIL=0 ===`**（IPv4 14 + IPv6 17），EXIT=0 |
| 日志 | 宿主 `/tmp/ipv6-verify.log`（IPv4+IPv6 完整运行；旧 IPv4-only 记录 `/tmp/verify-integration.log`） |

**IPv4 半（14 项）**：三个节点均记录 `spf generation=... routes=3`；每节点
proto-199 FIB 携带两条远端前缀与期望下一跳（例：节点 a `10.0.2.2 nhid 17 via
10.77.1.2 dev eth1` 与 `10.0.3.3 nhid 17 via 10.77.1.2 dev eth1`）；节点 b 向 c
转发；双边端到端 ping 经 MIDR 路径通过：
`ping a -> 10.0.3.3 (c prefix over b) (source 10.0.1.1 over eth1)` 与
`ping c -> 10.0.1.1 (a prefix over b) (source 10.0.3.3 over eth1)`。

**IPv6 半（17 项）**：三个节点均 `spf generation=... routes=3`；每节点 IPv6
proto-199 FIB 携带两条远端前缀与期望下一跳，例如节点 a
`2001:db8:12::1 nhid 16 via 2001:db8:77:1::2 dev eth1` 与
`2001:db8:13::1 nhid 16 via 2001:db8:77:1::2 dev eth1`，节点 b
`2001:db8:13::1 nhid 20 via 2001:db8:77:2::2 dev eth2`；节点 b 向 c 转发；双边
端到端 ping6 绑定节点自身 MIDR 前缀为源后经 MIDR 路径通过：
`[PASS] ping6 a -> 2001:db8:13::1 (c prefix over b) (source 2001:db8:11::1 over eth1)`
与 `[PASS] ping6 c -> 2001:db8:11::1 (a prefix over b) (source 2001:db8:13::1 over eth1)`；
两族 proto-199 表互不串族（IPv4 表不含 `2001:` 前缀、IPv6 表不含 `10.0.` 前缀）；
teardown 后无残留 `clab-` 容器。该 lab 人工核对 `midrd status` 为
`send-failures=0 last-error=0 replays=1`。

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

**C2 单容器 E2E（完成，group-2 传输层修复后）**

| 项 | 内容 |
| --- | --- |
| 脚本 | `midrd/r7-dp-e2e-zapi.sh`（单容器 3×midrd + 真实 zebra，三进程共用 loopback `127.0.0.1`，两侧 `--peer`；dummy underlay `192.168.77.1/24` 经 `--link-addr` 提供） |
| 状态 | **已通过**（group-2 传输层修复后） |
| 证据日志 | `/tmp/tp-e2e4.log` |

```sh
# 容器 frr-ubuntu24-ymy 内，root
bash midrd/r7-dp-e2e-zapi.sh            # 单容器 loopback
```

**观测**：三个节点均记录 `spf generation=... routes=3`；3 个远端前缀中 2 个装入
proto-199 FIB 且带 MIDR underlay nexthop（`10.0.2.2`/`10.0.3.3`）；`ip route get`
经 underlay 解析；peer loss 撤销其路由、其余节点重收敛到 `routes=2`；zebra 重启
后 replay 把路由放回 FIB；全部停止后 proto-199 FIB 为空。

**曾经的失败与修复**：修复前该形式无法收敛——节点 101 在 60 s 内 **799 次
`closed` / 797 次 `established`**（关闭原因 `-104`/ECONNRESET）；SPF 达不到
`routes=3`；禁用第三组后端（`midrd --no-zebra`）时同样复现，故**不是数据面回归**。
根因是 `midrd/midr-transport.c` 的 `find_peer_by_address()` 仅按源地址匹配入向
连接：多个已配置 peer 共用同一地址（loopback `127.0.0.1` 或连片的 loopback
alias）时提示无区分度，旧实现猜测绑定到首个匹配 peer，随后
`midrd/midr-session.c` `hello_receive()` 因绑定 peer 与实际 HELLO 不符返回
`MIDR_SESSION_IDENTITY_MISMATCH`（`-EEXIST`）拆链。同一根因也造成 distinct
loopback alias（`127.0.0.11`/`127.0.0.12`）场景“始终无法建立”。修复后：
`find_peer_by_address()` 在地址歧义时**返回 NULL 不猜测**（第 203-229 行），
未绑定入向流由 HELLO 经新增 `midr_transport_promote()`（第 959 行）重新绑定，
`midr-session.c` 先将其持有为 provisional peer（`transport_established()` 第
249-251 行、`hello_identify()` 第 330-357 行）。

**两条测试事实（决定了该形式的断言口径）**：

- **(a) 单容器共享内核命名空间**：三个守护进程共用**同一个内核 namespace**，
  且 MIDR representative takeover 可能合法地把某个 group 前缀判定为本地（因此
  不下发路由）。故脚本**不再断言每个前缀都入 FIB**，而是断言“**至少一个远端前缀
  以 MIDR underlay nexthop 装入 FIB**”（第 254-265 行）加上撤销/重收敛/replay/
  shutdown 不变量；**严格的逐前缀、逐 nexthop 断言保留在三命名空间 containerlab
  测试**（`midrd/r7-dp-integration-smoke.sh`，14/14）。
- **(b) 链路地址必须是真实、可解析的 underlay 地址**：若用 `127.0.0.1` 作
  nexthop，zebra 会经默认路由解析（loopback 不在 zebra RIB 中），实测得到
  `via 172.17.0.1 dev eth0`，即“管理网络”假阳性。脚本现用 dummy underlay
  `192.168.77.1/24`（`--link-addr`）并断言 `ip route get` 在该 dummy 上解析。


## 4. 覆盖对照（相对旧 DP 报告）

| 旧用例/报告 | 旧覆盖内容 | 新对应 | 状态 |
| --- | --- | --- | --- |
| `test-steps/01-bgpd-build-verification.md` | `bgp_midr_zebra.{c,h}` 可编译 | FRR 顶层 + midrd 组件构建（`dp-02`） | 完成 |
| `test-steps/02-netlink-c-proto199.md` | 内核识别 `RTPROT_BGP_MIDR = 199` | 场景 A（`ip route get` + proto 199） | 完成 |
| `test-steps/03-unit-tests.md` | mock `zclient_route_send` 的 DP 单测 | `midrd/dp-backend-test.c` | 完成 |
| `test-steps/04-e2e-zapi.md` | 真实 ZAPI：SPF 单路由 + Dual-Instance SPF+TE SRv6，FIB 验证/删除 | 场景 A（含 SPF/TE、SRv6 接受性、replay、shutdown） | 完成（内核 SRv6 断言因无 `seg6` 跳过） |
| `test-steps/05-zapi-batch-stress.md` | 128 路由批量安装/删除、10 前缀双实例共存 | 场景 A 的 ECMP/UCMP/双实例子集 | 部分覆盖（批量压力待补） |
| `test-steps/06-zapi-clab-connectivity.md` | 2 节点 clab blackhole/redirect/stress 连通性 | 场景 C（三节点真实转发，containerlab，IPv4 + IPv6） | 完成（PASS=31） |
| `midr-gre-test-report.md` | 双容器 GRE/ip6gre 21/21 PASS、状态 API 返回建立成功 | 场景 B（`midrd` GRE API + `midrd-gre-tool`） | 完成（PASS=24） |
| `midrd/r7-dp-e2e-zapi.sh` 的 `--no-zebra` 对照 | 旧报告不含 | 场景 C（C2 单容器 loopback） | 完成（group-2 传输层修复后） |

差异点：新场景增加了 `--no-zebra` 对照、zebra 重启 replay（清 installed hash +
`midr_spf_install_replay()`）、以及 deferred-batch 发送失败两步恢复。组件层恢复
由 `dp-backend-test.c` 覆盖，真实 ZAPI 层由 `r7-dp-fib-smoke.sh`（场景 A2）与
三节点 containerlab（场景 C1）覆盖；单容器 loopback 形式（场景 C2）在 group-2
传输层修复后也已通过。

## 5. 证据

| 项 | 依据 |
| --- | --- |
| group-3 隔离 FIB | `midrd/r7-dp-zapi-fib.sh` PASS=29 FAIL=0 EXIT=0；日志 `/tmp/fib-final.log`、`/tmp/midrd-dp-zapi-fib/run/` |
| 第三组 ZAPI/FIB smoke | `midrd/r7-dp-fib-smoke.sh` PASS=15 FAIL=0 EXIT=0；容器日志 `/tmp/fib-smoke.log` |
| 三节点 containerlab 集成（IPv4 + IPv6） | `midrd/r7-dp-integration-smoke.sh` PASS=31 FAIL=0 EXIT=0（IPv4 14 + IPv6 17）；宿主日志 `/tmp/ipv6-verify.log`（旧 IPv4-only：`/tmp/verify-integration.log`） |
| GRE 双容器 | `midrd/midr-gre-connectivity-test.sh` PASS=24 FAIL=0；宿主日志 `/tmp/gre-test.log` |
| P7 收尾（真实 zebra + 真实内核 FIB） | `tests/bgpd/test_midr_proto199.c` `=== ALL CHECKS PASSED (proto 199 = midr verified) ===`；双实例 TE 内核 FIB `10.60.0.0/24 nhid 33 via 192.168.200.3 dev midr-dp0 metric 20`（TE 胜出；`DP action=add4-te ... rc=0 installed=1 adds=1`）；SRv6 H.Insert 内核 FIB `fd00:60::/64 nhid 35 encap seg6 mode inline segs 2 [ 2001:db8:100::1 2001:db8:100::2 ] via fd00:200::2 dev midr-dp6 metric 20 pref medium`（`DP action=add6-srv6 ... rc=0 installed=1 adds=1`）；容器 `frr-ubuntu24-ymy`，日志 `/tmp/closeout.log` |
| ENOBUFS 类发送失败真实环境注入 | **待验证**：真实环境仅覆盖 zebra stop/restart 重连 replay 与 SIGTERM 撤销（`r7-dp-fib-smoke.sh` case B 与集成撤销检查）；`-ENOBUFS` 注入仅由单测 `midrd/dp-backend-test.c` 覆盖 |
| harness | `midrd/dp-e2e-tool.c`、`midrd/gre-link-tool.c` |
| FIB 断言（group-aware） | `midrd/r7-dp-fib-smoke.sh` 第 80-101 行 `fib_nhid()`/`fib_nh_list()` |
| 单容器 loopback E2E | `midrd/r7-dp-e2e-zapi.sh`：三节点 `routes=3`，3 个远端前缀中 2 个带 MIDR underlay nexthop 入 proto-199 FIB，peer loss 撤销并重收敛 `routes=2`，zebra 重启 replay；日志 `/tmp/tp-e2e4.log` |
| group-2 传输层修复 | `midrd/midr-transport.c` 第 203-229 行 `find_peer_by_address()` 歧义返回 NULL、`midr_transport_promote()` 第 959 行；`midrd/midr-session.c` provisional 处理第 249-251/330-357 行、identity mismatch 第 281-287 行；`midrd/transport-test.c` `test_shared_address_promote()` |
| 旧覆盖基线 | `frr/frr/doc/midr-doc/midr-gre-test-report.md`、`frr/frr/doc/midr-doc/test-steps/01..06` |
