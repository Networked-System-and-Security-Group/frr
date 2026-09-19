# dp-03 端到端与集成场景

## 1. 结论

数据面端到端验收分三类：group-3 隔离真实 ZAPI/zebra-rib/Linux-FIB、
双容器 GRE/ip6gre 连通性、三节点 group2+group3 集成。

- **group-3 隔离 FIB 测试**：`midrd/r7-dp-zapi-fib.sh` **PASS=29 FAIL=0 EXIT=0**。
- **双容器 GRE 连通性**：`midrd/midr-gre-connectivity-test.sh` **PASS=24 FAIL=0**。
- **三节点集成**：`midrd/r7-dp-integration-smoke.sh` 与单容器
  `midrd/r7-dp-e2e-zapi.sh` **阻塞（待第二组修复会话层）**，当前无法验证。

## 2. 范围与脚本清单

| 脚本 | 场景 | 运行位置 | 状态 |
| --- | --- | --- | --- |
| `midrd/r7-dp-zapi-fib.sh` | group-3 隔离真实 ZAPI/zebra-rib/Linux-FIB | 容器 `frr-ubuntu24-ymy`（root） | 完成 |
| `midrd/midr-gre-connectivity-test.sh` | 双容器 GRE/ip6gre 连通性 | docker 宿主（root） | 完成 |
| `midrd/r7-dp-e2e-zapi.sh` | 单容器 3×midrd + zebra 端到端 | 容器 | 阻塞 |
| `midrd/r7-dp-integration-smoke.sh` | 三节点 containerlab 集成 | docker 宿主 + containerlab | 阻塞 |

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

### 3.2 场景 B：双容器 GRE/ip6gre 连通性（完成）

| 项 | 内容 |
| --- | --- |
| 拓扑 | `node1` 10.1.1.11 ↔ docker bridge ↔ `node2` 10.1.1.12；overlay `192.168.100.0/30`、`fd00:100::/64`；ip6gre overlay `fd00:200::/64` |
| 命令 | docker 宿主上 `bash midrd/midr-gre-connectivity-test.sh`（root） |
| 结果 | **PASS=24 FAIL=0** |
| 日志 | 宿主 `/tmp/gre-run/test2.log` |

```sh
# docker 宿主，root
bash midrd/midr-gre-connectivity-test.sh
# 摘要行: === summary: PASS=24 FAIL=0 ===（日志 /tmp/gre-run/test2.log）
```

前置：`node1`/`node2` 已存在并运行 `frr-ubuntu24-ymy:init`，且在同一 bridge；
脚本把构建容器内新构建的 zebra、libfrr 与 `midrd-gre-tool` staged 到两节点的
`/opt/midr-dp`（保留旧 `/opt/midr` 资产不动）。

覆盖与验收：

| 用例 | 观测 | 结果 |
| --- | --- | --- |
| A | IPv4 GRE（`gre`）建立 + IPv4 overlay 双向 ping | 通过 |
| B | 同一 IPv4 隧道承载 IPv6 overlay（`fd00:100::/64`） | 通过 |
| C | IPv6 GRE（`ip6gre`）建立 + IPv6 overlay 双向 ping | 通过 |
| D | 建链状态 API 返回 `state=up` 且 `ifindex` 有效；`ip -d link show` kind 正确 | 通过 |
| E | 重复 add / 状态查询幂等 | 通过 |
| F | teardown 删除全部接口 | 通过 |

### 3.3 场景 C：三节点集成与单容器 E2E（阻塞，待第二组修复会话层）

| 项 | 内容 |
| --- | --- |
| 脚本 | `midrd/r7-dp-integration-smoke.sh`（三节点 A-B-C，`10.77.x.x/30`）、`midrd/r7-dp-e2e-zapi.sh`（单容器 3×midrd + zebra） |
| 状态 | **阻塞（待第二组修复会话层）**，当前无法验证 |
| 原因 | 见 `README-dp-migration.md` 第 6 节 |

```sh
# 目前无法验证（依赖第二组会话层收敛）
bash midrd/r7-dp-e2e-zapi.sh            # 单容器
bash midrd/r7-dp-integration-smoke.sh   # 三节点 containerlab
```

脚本预期断言为 prefix-specific，并会先清理遗留的 proto-199 路由。阻塞证据：
`midrd/r7-native-smoke.sh` EXIT=1（禁用第三组时同样复现）；peer 全用
`127.0.0.1` 时重连风暴（约 799 `closed`/797 `established`，reason `-104`/`-32`）
且 `spf generation=... routes=1` 而非 `routes=3`；改用不同 `127.0.0.x` 时
会话无法建立。根因怀疑在 `midrd/midr-transport.c`
`find_peer_by_address()`（第 203 行，只按地址匹配）与
`midrd/midr-session.c` `hello_receive()`（第 218/231 行
`MIDR_SESSION_IDENTITY_MISMATCH`）。**本文档不对这两个场景做任何通过性声明。**

## 4. 覆盖对照（相对旧 DP 报告）

| 旧用例/报告 | 旧覆盖内容 | 新对应 | 状态 |
| --- | --- | --- | --- |
| `test-steps/01-bgpd-build-verification.md` | `bgp_midr_zebra.{c,h}` 可编译 | FRR 顶层 + midrd 组件构建（`dp-02`） | 完成 |
| `test-steps/02-netlink-c-proto199.md` | 内核识别 `RTPROT_BGP_MIDR = 199` | 场景 A（`ip route get` + proto 199） | 完成 |
| `test-steps/03-unit-tests.md` | mock `zclient_route_send` 的 DP 单测 | `midrd/dp-backend-test.c` | 完成 |
| `test-steps/04-e2e-zapi.md` | 真实 ZAPI：SPF 单路由 + Dual-Instance SPF+TE SRv6，FIB 验证/删除 | 场景 A（含 SPF/TE、SRv6 接受性、replay、shutdown） | 完成（内核 SRv6 断言因无 `seg6` 跳过） |
| `test-steps/05-zapi-batch-stress.md` | 128 路由批量安装/删除、10 前缀双实例共存 | 场景 A 的 ECMP/UCMP/双实例子集 | 部分覆盖（批量压力待补） |
| `test-steps/06-zapi-clab-connectivity.md` | 2 节点 clab blackhole/redirect/stress 连通性 | 场景 C（三节点真实转发） | 阻塞（第二组会话层） |
| `midr-gre-test-report.md` | 双容器 GRE/ip6gre 21/21 PASS、状态 API 返回建立成功 | 场景 B（`midrd` GRE API + `midrd-gre-tool`） | 完成（PASS=24） |
| `midrd/r7-dp-e2e-zapi.sh` 的 `--no-zebra` 对照 | 旧报告不含 | 场景 C | 阻塞 |

差异点：新场景增加了 `--no-zebra` 对照、zebra 重启 replay（清 installed hash +
`midr_spf_install_replay()`）、以及 deferred-batch 发送失败两步恢复（组件层已由
`dp-backend-test.c` 覆盖；真实 ZAPI 层的 `r7-dp-e2e-zapi.sh` 阻塞）。

## 5. 证据

| 项 | 依据 |
| --- | --- |
| group-3 隔离 FIB | `midrd/r7-dp-zapi-fib.sh` PASS=29 FAIL=0 EXIT=0；日志 `/tmp/fib-final.log`、`/tmp/midrd-dp-zapi-fib/run/` |
| GRE 双容器 | `midrd/midr-gre-connectivity-test.sh` PASS=24 FAIL=0；宿主日志 `/tmp/gre-run/test2.log` |
| harness | `midrd/dp-e2e-tool.c`、`midrd/gre-link-tool.c` |
| 阻塞证据 | `midrd/r7-native-smoke.sh` EXIT=1；`midrd/midr-transport.c` 第 203 行、`midrd/midr-session.c` 第 218/231 行 |
| 旧覆盖基线 | `frr/frr/doc/midr-doc/midr-gre-test-report.md`、`frr/frr/doc/midr-doc/test-steps/01..06` |

