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
- **当前总体状态**：数据面迁移与 group-3 隔离验证已完成（P0-P6 完成，P7
  部分完成）；三节点 LS 洪泛集成因第二组会话层未收敛而**阻塞**（见第 6 节）。
- **接口契约**：
  - `doc/midr-doc/14_midrd与第三组输出接口交接.md`
  - `doc/midr-doc/MIDR-TED路径计算接口规范.md`

## 2. 文档清单

| 文档 | 内容 |
| --- | --- |
| `README-dp-migration.md` | 本索引、阶段状态、责任划分、已核实证据、阻塞项、快速开始 |
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
| P7 | 集成/功能验收 | 部分完成：功能测试 3 项通过（组件套件、GRE 双容器、group-3 FIB）；三项受阻（见第 6 节） |
| P8 | 三节点 LS 洪泛 + SPF 收敛（`midrd/r7-dp-integration-smoke.sh`、`midrd/r7-dp-e2e-zapi.sh`） | 阻塞（待第二组修复会话层） |
| P9 | 文档与提交 | 进行中 |

> P7 三项受阻：(1)(2) `r7-dp-e2e-zapi.sh` 与 `r7-dp-integration-smoke.sh` 依赖
> 第二组会话层，当前无法收敛；(3) SRv6 内核 FIB 断言因容器无 `seg6` 模块被跳过
> （ZAPI/zebra 接受性已验证）。

## 5. 已核实证据

以下结果均由协调者在容器 `frr-ubuntu24-ymy`（源码树 `/home/frr/frr-midrd3`）
今天执行得到。

### 5.1 FRR 顶层构建（P1）

```sh
cd /home/frr/frr-midrd3 && make -j112
```

摘要：**EXIT=0，0 errors / 0 warnings**。日志 `/tmp/build-full.log`（容器
`frr-ubuntu24-ymy`）。

### 5.2 midrd 组件套件（P2/P4）

```sh
cd /home/frr/frr-midrd3/midrd && make -j112 BUILD_DIR=/tmp/midrd-build test
```

摘要：**25 个程序 PASS**，含 `midrd-dp-test: PASS`、
`standalone libfrr boundary scan: PASS`，**EXIT=0**。日志 `/tmp/verify-comp.log`。

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

摘要：**PASS=24 FAIL=0**。新 harness（真实 zebra + 真实 netdevice）：`node1`
源码树 `midrd/gre-link-tool.c` + 新构建的 zebra 被 staged 到 `node1`/`node2` 的
`/opt/midr-dp`；IPv4 GRE（`gre`）与 IPv6 GRE（`ip6gre`）均建立
（`state=up` 且回报 `ifindex`），`ip -d link show` kind 正确，IPv4 overlay 双向
ping 通，IPv6 overlay over IPv4 隧道双向 ping 通，重复 add / 状态查询幂等，
teardown 删除全部接口。日志 `/tmp/gre-run/test2.log`（宿主）。

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

## 6. 阻塞项：三节点 LS 洪泛收敛依赖第二组会话层

**该阻塞与第三组无关**，在禁用第三组时同样复现：

- 证据：`midrd/r7-native-smoke.sh` **EXIT=1**，无论使用新后端还是
  `--no-zebra` 包装（group-3 关闭）。
- 全部 peer 使用 `127.0.0.1` 时出现重连风暴：节点日志约 **799 次 `closed` /
  797 次 `established`**（reason `-104`/`-32`），且
  `spf generation=... routes=1` 而非期望的 `routes=3`（对照
  `midrd/r7-native-smoke.sh` 第 82 行的断言）。
- 使用不同的 `127.0.0.x` peer 地址时，会话**始终无法建立**。
- 根因怀疑（移交第二组）：`midrd/midr-transport.c` 第 203 行
  `find_peer_by_address()` 只按地址匹配；多个 peer 共用 `127.0.0.1` 时，
  来自另一 peer 的连接被挂到错误的 peer 槽位；随后
  `midrd/midr-session.c` 第 218 行 `hello_receive()` 返回
  `MIDR_SESSION_IDENTITY_MISMATCH`（`-EEXIST`，见
  `midrd/midr-session.c` 第 231 行）并丢弃该连接。

因此 `midrd/r7-dp-e2e-zapi.sh` 与 `midrd/r7-dp-integration-smoke.sh` 在各自头部
标记为**阻塞（待第二组修复会话层）**，当前无法验证；它们的预期断言是
prefix-specific，并会先清理遗留的 proto-199 路由。本文档不对三节点场景做任何
通过性声明。

## 7. 快速开始

```sh
# 1) FRR 顶层构建（容器内，源码树 /home/frr/frr-midrd3）
cd /home/frr/frr-midrd3 && make -j112                      # 期望 EXIT=0

# 2) midrd 组件套件（含 dp 单测与边界扫描）
cd /home/frr/frr-midrd3/midrd
make -j112 BUILD_DIR=/tmp/midrd-build test                  # 期望 25 程序 + boundary scan 全 PASS

# 3) group-3 隔离真实 ZAPI/FIB（容器内）
bash /home/frr/frr-midrd3/midrd/r7-dp-zapi-fib.sh           # 期望 PASS=29 FAIL=0

# 4) GRE 双容器连通性（docker 宿主；node1/node2 已就绪）
bash midrd/midr-gre-connectivity-test.sh                    # 期望 PASS=24 FAIL=0
```

详细构建、日志位置与容器同步步骤见 `dp-02-build-and-test.md`。

## 8. 证据索引

| 验证 | 结果摘要 | 日志 |
| --- | --- | --- |
| FRR 顶层构建 | EXIT=0，0 errors/0 warnings | 容器 `/tmp/build-full.log` |
| midrd 组件套件 | 25 程序 PASS，EXIT=0 | 容器 `/tmp/verify-comp.log` |
| GRE 双容器连通性 | PASS=24 FAIL=0 | 宿主 `/tmp/gre-run/test2.log` |
| group-3 隔离 ZAPI/FIB | PASS=29 FAIL=0 EXIT=0 | 容器 `/tmp/fib-final.log`、`/tmp/midrd-dp-zapi-fib/run/` |
| 三节点 LS 洪泛 | 阻塞（第二组会话层） | `r7-native-smoke.sh` EXIT=1；节点日志 closed/established 统计 |
| deferred-batch 恢复缺陷 | 已修复并覆盖 | `midrd/dp-backend-test.c` `test_adapter_pipeline_and_recovery()` |

