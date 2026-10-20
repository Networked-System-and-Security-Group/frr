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
- **当前总体状态**：数据面迁移与验收已完成（P0-P8 完成）。三节点
  containerlab 联合测试（第二组 + 第三组，IPv4 + IPv6，`PASS=31 FAIL=0`）与单容器 loopback 形式
  `midr-test/r7-dp-e2e-zapi.sh` 均已通过。此前阻塞单容器形式的 group-2 传输层
  缺陷已由队友修复（`midrd/midr-transport.c` 的 `find_peer_by_address()` 不再
  在多个 peer 共用同一地址时猜测，改由 HELLO 经 `midr_transport_promote()`
  重新绑定；`midrd/midr-session.c` 先将其持有为 provisional peer）。修复后
  IPv4/IPv6 native smoke、distinct loopback alias 探针、单容器 E2E 均通过
  （见第 6 节）。
- **接口契约**：
  - `doc/midr-doc/14_midrd与第三组输出接口交接.md`
  - `doc/midr-doc/MIDR-TED路径计算接口规范.md`

## 2. 文档清单

本目录共 22 份 `.md`，按用途分为四组（§2.1–§2.4）。
**权威性**一列给出争议时的采信顺序：权威签名（代码）> 准一来源（契约）> 本轮结论 > 历史记录。

### 2.1 索引与总览

| 文档 | 角色 | 权威性 | 内容一句话 |
| --- | --- | --- | --- |
| `README-dp-migration.md` | 本文件，目录索引与总览 | 本目录入口（索引本身不含验收结论） | 阶段状态、责任划分、已核实证据、单容器形式修复说明、快速开始；§2.5 给出虚链路主线阅读顺序 |
| `dp-01-migration-overview.md` | 迁移总览 | 迁移范围的说明性权威 | 旧 `bgpd` → 新 `midrd` 映射表、文件清单、边界规则（旧实现保留为差分对照） |

### 2.2 构建 / 端到端 / 迁移分册

| 文档 | 角色 | 权威性 | 内容一句话 |
| --- | --- | --- | --- |
| `dp-02-build-and-test.md` | 构建与测试分册 | 构建口径的说明性权威 | 三档构建（FRR 顶层严构建 / `midrd` 组件套件 / 容器同步）、边界扫描、第三组 ZAPI/FIB smoke、日志位置 |
| `dp-03-e2e-and-integration.md` | 端到端与集成场景分册 | E2E 口径的说明性权威 | 三类验收（group-3 隔离真实 ZAPI/zebra-rib/FIB、双容器 GRE/ip6gre、三节点 group2+group3 集成）与覆盖对照 |
| `dp-04-gre.md` | GRE 虚拟接口分册 | **设备层（底层）**说明性权威 | `midrd/midr-gre.{c,h}` API、ZAPI 线格式路径、工具/脚本流程、与旧版差异 |

### 2.3 虚链路（GRE overlay）主线

`dp-04` 讲的是**设备层**（`midr-gre.h`，谁来建 netdevice）；本节是**虚链路 overlay 主线**
（第一组可直接调用的高层服务，权威签名为 `midrd/midr-virtual-link.h`），共 8 份。

| 文档 | 角色 | 权威性 | 内容一句话 |
| --- | --- | --- | --- |
| `midr-virtual-link-third-group-implementation.md` | 需求（上游任务说明） | **需求权威**（C 签名最终以会签为准） | 定义第三组要补齐的虚链路设备服务与接口边界（§1–§10）；不决定第一组邻接选择策略 |
| `midr-virtual-link-api.md` | 接口契约（P0 冻结） | **唯一准一来源** | 虚链路设备服务 API 契约；W3 修订在**回调签名不变**前提下新增 `event` / `overlay_prefix_len` / `overlay_ready` |
| `midr-virtual-link-api-countersign.md` | 会签包 | 第一组 ↔ 第三组逐条确认的载体 | 请第一组对契约逐条回签（C1–C7 + 九问 + 会签表），逐条列变更及对调用方的影响 |
| `midr-virtual-link-requirement-conformance.md` | 需求符合性审计（本轮新增） | 本轮结论（**静态，不实测**） | 53 行需求矩阵：符合 48 / 部分符合 5 / 缺失 0，符合度 ≈95%；需实测项另列 |
| `midr-virtual-link-runtime-evidence.md` | 运行时实测证据（本轮新增） | **当前权威实测** | 本轮（2026-09-29）在容器 `frr-ubuntu24-ymy` 亲眼观察到的原始输出快照 |
| `midr-gre-overlay-interface-design.md` | 接口裁决与层次设计（本轮新增） | 本轮结论（纯裁决，**无代码变更**） | 裁决 overlay 四件套只归 `struct midr_virtual_link_status`，绝不进 `struct midr_gre_status` |
| `midr-group1-reply-overlay-fields.md` | 对第一组回应稿（本轮新增） | 可直接发送的答复稿 | 回应第一组「overlay 字段与通知」诉求：四字段已存在，只是不在 `midr_gre_status` |
| `midr-virtual-link-third-group-verification.md` | 历史实测报告 | **已被取代**（计数仅对 W3 轮有效） | W3 轮（D1–D4 修复 + W0 通知合入后）再从零重建的实测记录 |

### 2.4 设计 / 历史报告

以下文档或属 `bgpd` 时代（迁入 `midrd` 之前）的设计，或为更早轮次的进展/测试报告，
**不作为当前验收依据**，仅作背景与差分对照。

| 文档 | 角色 | 权威性 | 内容一句话 |
| --- | --- | --- | --- |
| `midr-cp-dp-interface.md` | 接口设计 | 历史（`bgpd` 时代） | 控制平面 → 数据平面路由安装接口：**单向**、CP → DP 的唯一正式通道 |
| `midr-dataplane-design-and-implementation.md` | 设计规范 + 实现说明（v2.0） | 历史（整合稿） | 融合 `midr-cp-dp-interface.md` + `traffic-engineering.md` + RFC 9815 的数据平面设计与实现 |
| `midr-dataplane-dev-doc.md` | 开发文档 | 历史（`bgpd` 时代） | 数据平面模块职责与核心文件（`bgpd/bgp_midr_zebra.{c,h}` 等）的职责/规模 |
| `midr-dataplane-progress-report.md` | 进展汇报 | 历史（汇报稿） | 模块定位、结构体关系图、函数调用流、Dual-Instance TE、100 ms 批处理、质量指标、演进规划 |
| `midr-gre-interface.md` | 设计说明 | 历史（分支 `feat/te-dp-interface`） | `bgpd/bgp_midr_gre.{c,h}` 的 GRE 虚拟接口创建/消除设计与改动总览 |
| `midr-gre-interface-api.md` | 接口说明与使用文档 | 历史（`bgpd` 时代） | `midr_gre_interface_add()` / `midr_gre_interface_del()` 的用法（头文件 `bgpd/bgp_midr_gre.h`） |
| `midr-gre-test-report.md` | 测试报告 | 历史（`bgpd` 时代） | 两容器间 GRE/ip6gre 创建、承载 IPv4/IPv6 业务、消除的连通性报告 |
| `midr-test-report-20260709.md` | 测试报告（v5.0，2026-07-09） | 历史（更早轮次） | `tests/bgpd/scripts/test01-06_*.sh` 六项概览（编译 / proto-199 / 单测 / E2E / 批量压力 / containerlab） |
| `traffic-engineering.md` | 设计方案 | 历史（范围与主线重叠） | 基于 SRv6 的 TE 设计：组间分工、与第二组接口、CP-DP 路由下发接口 |

### 2.5 虚链路主线：阅读顺序（推荐）

按下面顺序读：前一步解决「要什么」，后一步回答「满足了没有、怎么确认」。

1. **需求** → `midr-virtual-link-third-group-implementation.md`（408 行）。
   为什么放第一：它是本轮全部工作的上游，定义「要做到什么」和第三组的接口边界；
   后面所有文档都在回答「需求满足了没有、怎么满足的」。
2. **契约** → `midr-virtual-link-api.md`（651 行）。**唯一准一来源，P0 冻结**。
   为什么放第二：需求里的 C 签名到此才成为可编码、可会签的正式接口；凡与其它文档冲突，
   以本文为准。W3 修订新增 `event` / `overlay_prefix_len` / `overlay_ready`，但**回调签名不变**
   （调用方源码兼容）。
3. **会签** → `midr-virtual-link-api-countersign.md`。
   为什么放第三：契约生效需要第一组回签；本文逐条（C1–C7 + 九问 + 会签表）列出变更点及
   **对调用方的影响**，是契约与调用方之间的桥梁。
4. **本轮结论（三份并读，同一基线）** → 分别回答「静态是否满足 / 实测是否通过 / 接口怎么归层」：
   - `midr-virtual-link-requirement-conformance.md` —— 符合性：53 行矩阵（符合 48 / 部分符合 5 / 缺失 0，≈95%）。
   - `midr-virtual-link-runtime-evidence.md` —— **实测：当前权威**。
   - `midr-gre-overlay-interface-design.md` —— 接口裁决与层次归属（无代码变更）。
   为什么三份并读：单看任一份都会偏——静态审计不实测、实测不判需求覆盖、裁决不管测试。
5. **发给第一组** → `midr-group1-reply-overlay-fields.md`。
   为什么放第五：第 1–4 步是第三组内部结论的整理，本文才是**对外口径**，把「overlay 四件套
   只在 `struct midr_virtual_link_status`」这一裁决变成第一组可直接照做的答复。
6. **历史背景（非当前权威）** → `midr-virtual-link-third-group-verification.md`。
   为什么放最后：它记录 W3 轮（D1–D4 修复 + W0 通知合入后）的读数，是理解「契约为何修订成
   现在这样」的背景，但**计数只对 W3 轮有效**。

> **时效性（务必注意，引用数字时必须标注轮次）**：
> - **当前权威实测 = `midr-virtual-link-runtime-evidence.md`**（本轮，2026-09-29）。脚本自带计数
>   汇总 **PASS=179 / FAIL=3**，3 个 FAIL 已归因为 manifest / root / fixtures 类问题，
>   与 GRE overlay 能力无关。
> - `midr-virtual-link-third-group-verification.md` 的 **106 / 112** 计数**仅对 W3 轮有效**
>   （W3 轮 = 组件 27 + GRE 51 + stage E 34 = 112 PASS，FAIL=0）；其中更早的 106 PASS
>   只对应**修正前**源码，不得当作当前树的证据。
> - 二者**不是同一轮**：179/3（本轮）与 112（W3 轮）不可互相比较、不可混用。

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
| P4 | `midr-test/dp-backend-test.c`、`midrd/Makefile`（`DP_OBJECTS` + 三个 harness/test 目标）、`Makefile.am`、`midrd/midrd.c` 接线 | 完成 |
| P5 | group-3 隔离真实 ZAPI/zebra-rib/Linux-FIB（`midr-test/r7-dp-zapi-fib.sh`） | 完成 |
| P6 | 双容器 GRE/ip6gre 连通性（`midr-test/midr-gre-connectivity-test.sh` + `midrd/gre-link-tool.c`） | 完成 |
| P7 | 集成/功能验收 | 完成：组件套件、group-3 FIB smoke、三节点 containerlab 集成（IPv4 + IPv6）、GRE 双容器均通过；真实 zebra + 内核 FIB 收尾（proto-199 隔离、双实例 TE、SRv6 H.Insert）见 5.8 |
| P8 | 三节点 LS 洪泛 + SPF 收敛（`midr-test/r7-dp-integration-smoke.sh`、`midr-test/r7-dp-e2e-zapi.sh`） | 完成：containerlab 形式（IPv4 + IPv6，PASS=31）与单容器 loopback 形式均通过（group-2 传输层修复后，见第 6 节） |
| P9 | 文档与提交 | 进行中 |

> 说明：`r7-dp-integration-smoke.sh`（containerlab，每节点独立地址）与单容器
> `r7-dp-e2e-zapi.sh`（三节点共用 `127.0.0.1`）均已通过；后者曾在 group-2
> 会话/传输层修复前无法收敛，修复细节见第 6 节。SRv6 内核 FIB 断言因容器无
> `seg6` 模块被跳过（ZAPI/zebra 接受性已验证）。

## 5. 已核实证据

以下结果均由协调者在容器 `frr-ubuntu24-ymy`（源码树 `/home/frr/frr-midrd3`）
今天执行得到。

### 5.1 FRR 顶层构建（P1）

```sh
cd /home/frr/frr-midrd3 && make clean && make -j112      # 从零重建
```

摘要：**EXIT=0，但 11 行 `warning:`（非 0）**，且这 11 条**全部**是既有 bgpd midr 告警。
T9 修复 `zebra/dplane_fpm_nl.c` 的 2 条 `DPLANE_OP_GRE_ADD`/`DPLANE_OP_GRE_DELETE`
`-Wswitch` 之前为 13 条；修复后对最终日志 `grep warning: ... | grep dplane` **0 命中**。
最终日志 `/tmp/verify-clean-T10.log`、`/tmp/verify-build-T10.log`（T9 前：
`/tmp/verify-build-7ad4671558.log`；容器 `frr-ubuntu24-ymy`，源码树 `/home/frr/frr-midrd3`）。

> **更正**：本节早前写的「0 errors / 0 warnings」来自一次**增量构建**——源码树当时
> 基本最新，`make` 只重编了 10 个 `vtysh/vtysh_cmd.*.o` 并重链 `vtysh`，没有重编任何
> `zebra/`、`bgpd/` 翻译单元，故潜伏的 `-Wswitch` 缺口未触发。只有 `make clean` 后的
> 完整重建才会暴露。11 条 bgpd 告警（`bgp_midr_pm.c` ×2 `-Wbad-function-cast`、
> `midr_trace_scheduler.c:1682` ×8 `-Wswitch-enum`、`bgp_midr_nds.c:3375` ×1
> `-Wunused-function`）都在本分支改动集之外，属**既存问题、OUT OF SCOPE**；
> 审查 `第三组最新midrd分支审查反馈与同步建议-2026-09-20.md` 第 6 节禁止把第一组
> group1 接线 / peer discovery / session 改动拉进第三组。2 条 `zebra/*` DPLANE 告警已由
> T9 修复并复测归零（最终 11 条全部为 bgpd）。详见 `dp-02-build-and-test.md` 第 3.1 节。

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
resync。覆盖见 `midr-test/dp-backend-test.c` 的
`test_adapter_pipeline_and_recovery()`。

### 5.3 GRE 双容器连通性（P3/P6）

```sh
# docker 宿主上执行
bash midr-test/midr-gre-connectivity-test.sh
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
bash midr-test/r7-dp-zapi-fib.sh
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

该脚本首次运行记录的两个待办（脚本输出中的 `OPEN ITEM`/`[SKIP]`）已在 **P7 收尾**
于真实 zebra + 真实内核 FIB 上复验通过（见 5.8）：(a) 双实例 TE 已在真实内核 FIB
提升 TE；(b) SRv6 H.Insert 已到达内核；另加 proto-199 内核隔离测试。首次运行的
本环境内核曾保留 SPF 条目（`r7-dp-zapi-fib.sh` 第 261 行 `OPEN ITEM`）、无
`seg6` 模块（第 286-300 行 `[SKIP] kernel seg6 module absent`）。

### 5.5 第三组 ZAPI/FIB smoke（P5/P7）

```sh
# 容器 frr-ubuntu24-ymy 内，root
bash /home/frr/frr-midrd3/midr-test/r7-dp-fib-smoke.sh
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
> `group` 列表展开（`midr-test/r7-dp-fib-smoke.sh` 第 80-101 行）。只匹配单一形式曾
> 造成多次误报失败。

### 5.6 三节点 containerlab 联合测试（group 2 + group 3，IPv4 + IPv6，P8）

```sh
# docker 宿主，root（containerlab + docker）
bash midr-test/r7-dp-integration-smoke.sh
```

摘要：**`=== summary: PASS=31 FAIL=0 ===`**（IPv4 14 + IPv6 17），**EXIT=0**。
日志 `/tmp/ipv6-verify.log`（旧 IPv4-only 记录 `/tmp/verify-integration.log`）。
脚本先跑 IPv4 拓扑 A-B-C（`10.77.x.x/30`），再以 IPv6 点对点链路
`2001:db8:77:x::/64`（A `2001:db8:77:1::1` == B `2001:db8:77:1::2`/`2001:db8:77:2::1`
== C `2001:db8:77:2::2`）与 IPv6 MIDR 前缀在独立 lab `midr-dp-int6` 重跑，节点镜像
`frr-ubuntu24-ymy:init`，从构建容器绑定新构建的 zebra + midrd。

**IPv4 半**：三个节点均记录 `spf generation=... routes=3`；每节点 proto-199 FIB
携带两条远端前缀与期望下一跳（例：节点 a `10.0.2.2 nhid 17 via 10.77.1.2 dev
eth1` 与 `10.0.3.3 nhid 17 via 10.77.1.2 dev eth1`）；节点 b 向 c 转发；双边端到
端 ping 经 MIDR 路径通过：`ping a -> 10.0.3.3 (c prefix over b) (source 10.0.1.1
over eth1)` 与 `ping c -> 10.0.1.1 (a prefix over b) (source 10.0.3.3 over eth1)`。

**IPv6 半**：三个节点均 `spf generation=... routes=3`；每节点 IPv6 proto-199 FIB
携带两条远端前缀与期望下一跳，例如节点 a `2001:db8:12::1 nhid 16 via
2001:db8:77:1::2 dev eth1` 与 `2001:db8:13::1 nhid 16 via 2001:db8:77:1::2 dev
eth1`，节点 b `2001:db8:13::1 nhid 20 via 2001:db8:77:2::2 dev eth2`；节点 b 向 c
转发；双边端到端 ping6 绑定节点自身 MIDR 前缀为源后经 MIDR 路径通过：
`[PASS] ping6 a -> 2001:db8:13::1 (c prefix over b) (source 2001:db8:11::1 over
eth1)` 与 `[PASS] ping6 c -> 2001:db8:11::1 (a prefix over b) (source
2001:db8:13::1 over eth1)`；两族 proto-199 表互不串族；teardown 后无残留
`clab-` 容器。该 lab 人工核对 `midrd status` 为 `send-failures=0 last-error=0
replays=1`。测试设计要点（绑定源、`-u 0`、`--link-addr`）见
`dp-03-e2e-and-integration.md` 第 3.3 节。

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
  `midr-test/dp-backend-test.c` 实际发现并覆盖。

### 5.8 P7 收尾：真实 zebra + 真实内核 FIB 复验（proto-199、双实例 TE、SRv6）

在容器 `frr-ubuntu24-ymy` 上以真实 zebra + 真实内核 FIB 复验 P7 清单项，日志
`/tmp/closeout.log`：

- **proto-199 内核隔离**：`tests/bgpd/test_midr_proto199.c`
  `=== ALL CHECKS PASSED (proto 199 = midr verified) ===`。
- **双实例 TE 在内核 FIB 提升 TE**：SPF instance 与 TE instance 同装
  `10.60.0.0/24` 时，内核 FIB 显示 TE nexthop
  `10.60.0.0/24 nhid 33 via 192.168.200.3 dev midr-dp0 metric 20`（TE 胜出），
  工具报告 `DP action=add4-te ... rc=0 installed=1 adds=1`。这解决了
  `r7-dp-zapi-fib.sh` 首次运行的 `OPEN ITEM`（见 5.4）。
- **SRv6（H.Insert inline）到达内核**：`fd00:60::/64 nhid 35 encap seg6 mode
  inline segs 2 [ 2001:db8:100::1 2001:db8:100::2 ] via fd00:200::2 dev midr-dp6
  metric 20 pref medium`，工具报告 `DP action=add6-srv6 ... rc=0 installed=1 adds=1`。
- **真实环境未覆盖（待验证）**：`-ENOBUFS` 类发送失败注入仅由单测
  `midr-test/dp-backend-test.c` 覆盖（被包裹（wrap）的 `zclient_route_send` 返回失败
  ⇒ installed hash 不更新、保留批重试/重同步）；真实环境已覆盖的失败路径是 zebra
  stop/restart 重连 replay 与 SIGTERM 撤销（`r7-dp-fib-smoke.sh` case B 与集成撤销
  检查）。

## 6. 单容器 loopback E2E 的根因与修复（已验证）

三节点 **containerlab** 联合测试（每节点独立地址）已通过（见 5.6），单容器
形式 `midr-test/r7-dp-e2e-zapi.sh`（三个 midrd 进程共用 loopback `127.0.0.1`，两侧
`--peer`）此前在本环境中**无法收敛**，曾标记为待验证并归属第二组。该缺陷现已
修复并复验通过。

**根因（一条 rendezvous 规则同时造成两个症状）**：`midrd/midr-transport.c`
的 `find_peer_by_address()` 仅按源地址匹配入向连接；当多个已配置 peer 共用同一
地址（loopback `127.0.0.1` 或连成一片的 loopback alias）时该提示无区分度。旧实现
会猜测绑定到首个匹配 peer，导致：

- 连接立刻被重置成风暴：节点 101 在 60 s 内 **799 次 `closed` / 797 次
  `established`**，关闭原因 `-104`（ECONNRESET）；SPF 始终达不到 `routes=3`。
- distinct loopback alias（`127.0.0.11`/`127.0.0.12`）场景下“始终无法建立”；
  两症状同一根因。（禁用第三组后端 `midrd --no-zebra` 时同样复现，故不是数据面
  回归。）

**修复（`midrd/midr-transport.c`、`midrd/midr-session.c`）**：

- `find_peer_by_address()` 在多个已配置 peer 共用同一地址时**返回 NULL、不再
  猜测**（第 203-229 行，`if (match) return NULL; /* ambiguous: do not guess */`）；
  未绑定的入向流保持 unbound，其身份稍后由 HELLO 帧经新增的
  `midr_transport_promote()` 解析（第 959 行起；由 `midr-test/transport-test.c`
  `test_shared_address_promote()` 覆盖）。
- `midrd/midr-session.c` 将这类流先持有为 **provisional peer** 直到 HELLO 指明
  对端（`transport_established()` 第 249-251 行、`hello_identify()` 第 330-357
  行、`MIDR_SESSION_IDENTITY_MISMATCH`/`-EEXIST` 判定第 281-287 行）。

**修复后的复验证据**：

| 验证 | 结果 | 证据 |
| --- | --- | --- |
| IPv4 + IPv6 native smoke | PASS，EXIT=0 | `MIDRD_BIN=/tmp/midrd-build/midrd bash r7-native-smoke.sh` |
| native smoke（`--no-zebra`） | PASS | `MIDRD_BIN=/tmp/midrd-nozebra bash r7-native-smoke.sh` |
| distinct loopback alias 探针 | `node=202 established remote=201 ... generation=1`，无 `closed` 风暴，SPF 收敛 | `bash /tmp/dp-probe3.sh`（`127.0.0.11`/`127.0.0.12`） |
| 单容器 loopback E2E | 三节点 `spf generation=... routes=3`；3 个远端前缀中 2 个装入 proto-199 FIB 且带 MIDR underlay nexthop（`10.0.2.2`/`10.0.3.3`）；`ip route get` 经 underlay 解析；peer loss 撤销其路由、其余重收敛到 `routes=2`；zebra 重启 replay 回 FIB | `midr-test/r7-dp-e2e-zapi.sh`（三 midrd + 真实 zebra，共用 `127.0.0.1`）；日志 `/tmp/tp-e2e4.log` |
| 组件套件 | 25 程序 PASS + 边界扫描 PASS | 修复后复验 |

修复后同样复验：native smoke、FIB smoke（15/15）、containerlab 联合测试（14/14）、
GRE（24/24）均通过。

**共享命名空间注意事项**：单容器形式中三个守护进程共用**同一个内核
namespace**，且 MIDR representative takeover 可能合法地把某个 group 前缀判定为
本地（因此不下发路由）。故脚本 `midr-test/r7-dp-e2e-zapi.sh` 改为断言
“**至少一个远端前缀以 MIDR underlay nexthop 装入 FIB**”（第 254-265 行）以及
后续的撤销/重收敛/replay/shutdown 不变量，而**严格的逐前缀、逐 nexthop 断言保留
在三命名空间 containerlab 测试**（`midr-test/r7-dp-integration-smoke.sh`，14/14）。

**MIDR 链路地址必须是真实、可解析的 underlay 地址**：若用 `127.0.0.1` 作 nexthop，
zebra 会经默认路由解析它（loopback 不在 zebra RIB 中），实测产生
`via 172.17.0.1 dev eth0`，即“管理网络”假阳性。脚本现用 dummy underlay
`192.168.77.1/24`（经 `--link-addr` 提供）并断言在该 dummy 上解析。

## 7. 快速开始

```sh
# 1) FRR 顶层构建（容器内，源码树 /home/frr/frr-midrd3）
cd /home/frr/frr-midrd3 && make -j112                      # 期望 EXIT=0

# 2) midrd 组件套件（含 dp 单测与边界扫描）
cd /home/frr/frr-midrd3/midrd
make -j112 BUILD_DIR=/tmp/midrd-build test                  # 期望 25 程序 + boundary scan 全 PASS

# 3) group-3 隔离真实 ZAPI/FIB（容器内）
bash /home/frr/frr-midrd3/midr-test/r7-dp-zapi-fib.sh           # 期望 PASS=29 FAIL=0

# 4) 第三组 ZAPI/FIB smoke（容器内，root；不依赖会话层）
bash /home/frr/frr-midrd3/midr-test/r7-dp-fib-smoke.sh          # 期望 PASS=15 FAIL=0

# 5) 三节点 containerlab 联合测试（docker 宿主，root；containerlab + docker）
bash midr-test/r7-dp-integration-smoke.sh                       # 期望 PASS=31 FAIL=0（IPv4 14 + IPv6 17）

# 6) GRE 双容器连通性（docker 宿主；node1/node2 已就绪）
bash midr-test/midr-gre-connectivity-test.sh                    # 期望 PASS=24 FAIL=0

# 7) 单容器 loopback E2E（容器内，root；三 midrd + 真实 zebra）
bash midr-test/r7-dp-e2e-zapi.sh                                # 三节点 routes=3；至少一个远端前缀经 MIDR underlay 入 FIB
```

`midrd/Makefile` 另提供显式验收目标 `fib-smoke`、`integration-smoke`、
`gre-smoke`（**不属于** `all`/`test`，需 root/命名空间/真实 zebra），分别包装上表
第 4、5、6 项脚本，可 `make -C midrd BUILD_DIR=/tmp/midrd-build fib-smoke` 等直接
调用（详见 `dp-02-build-and-test.md` 第 4 节）。

详细构建、日志位置与容器同步步骤见 `dp-02-build-and-test.md`。

## 8. 证据索引

> **时效性声明（阅读本节数字前必读）**：下表的计数与日志是 **P0–P9 早期迁移轮**
> （第三组数据面迁入 `midrd` 的验收周期，容器 `frr-ubuntu24-ymy`）的**证据快照**，
> 只在那一轮的基线与脚本下有效，本节**保留历史数字不改**。
> **虚链路（GRE overlay）主线是其后独立的一轮，其最新实测以
> `midr-virtual-link-runtime-evidence.md` 为准**（见 §2.5 的时效性说明）。
> 两处的计数**适用轮次不同**，不可互相比较、不可混用。

| 验证 | 结果摘要 | 日志 |
| --- | --- | --- |
| FRR 顶层构建（从零重建） | EXIT=0；**11 行 `warning:`（非 0）**，全部为既有 bgpd（OUT OF SCOPE）；2 条 DPLANE_OP_GRE_* 已由 T9 消除（T9 前 13） | 容器 `/tmp/verify-clean-T10.log`、`/tmp/verify-build-T10.log` |
| midrd 组件套件 | 25 程序 PASS，EXIT=0 | 容器 `/tmp/verify-comp.log`、`/tmp/final-comp.log` |
| 第三组 ZAPI/FIB smoke | PASS=15 FAIL=0 EXIT=0 | 容器 `/tmp/fib-smoke.log` |
| 三节点 containerlab 联合测试（IPv4 + IPv6） | PASS=31 FAIL=0 EXIT=0（IPv4 14 + IPv6 17） | 宿主 `/tmp/ipv6-verify.log`（旧 IPv4-only：`/tmp/verify-integration.log`） |
| group-3 隔离 ZAPI/FIB | PASS=29 FAIL=0 EXIT=0 | 容器 `/tmp/fib-final.log`、`/tmp/midrd-dp-zapi-fib/run/` |
| GRE 双容器连通性 | PASS=24 FAIL=0 | 宿主 `/tmp/gre-test.log` |
| P7 收尾（proto-199 隔离 / 双实例 TE / SRv6 H.Insert） | `=== ALL CHECKS PASSED (proto 199 = midr verified) ===`；TE 内核 FIB `10.60.0.0/24 nhid 33 via 192.168.200.3 dev midr-dp0 metric 20`；SRv6 `fd00:60::/64 nhid 35 ... inline segs 2 [ 2001:db8:100::1 2001:db8:100::2 ] via fd00:200::2 dev midr-dp6` | 容器 `frr-ubuntu24-ymy`，`/tmp/closeout.log` |
| ENOBUFS 类发送失败真实环境注入 | **待验证**（仅单测覆盖 `dp-backend-test.c`；真实环境覆盖 zebra 重连 replay 与 SIGTERM 撤销） | `midr-test/r7-dp-fib-smoke.sh` case B；集成撤销检查 |
| 单容器 loopback E2E | 已通过（group-2 传输层修复后）；三节点 `routes=3`，至少一个远端前缀经 MIDR underlay 入 FIB | `r7-dp-e2e-zapi.sh`；`/tmp/tp-e2e4.log` |
| group-2 传输层修复 | `find_peer_by_address()` 不再猜测 + `midr_transport_promote()` | `midrd/midr-transport.c`、`midrd/midr-session.c`、`midr-test/transport-test.c` `test_shared_address_promote()` |
| deferred-batch 恢复缺陷 | 已修复并覆盖 | `midr-test/dp-backend-test.c` `test_adapter_pipeline_and_recovery()` |
