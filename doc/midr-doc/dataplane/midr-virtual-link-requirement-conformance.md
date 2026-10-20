# MIDR 虚链路第三组：需求符合性审计（静态）

> 审计员：第三组数据面需求符合性审计（静态，不实测）
> 审计基线：仓库 `/Users/yangmengyu/githubdocuments/frr`，`git rev-parse HEAD` = `027bde5bc8ad0a6ac4d8afb3b2286014a6641ce1`（工作树无未提交改动，`git status --short` 为空）。
> 准一来源（需求）：`doc/midr-doc/dp-doc/midr-virtual-link-third-group-implementation.md`
> 支撑文档：`midr-virtual-link-api.md`（接口契约）、`midr-virtual-link-api-countersign.md`（会签包）、`midr-virtual-link-third-group-verification.md`（实测报告）
> 方法：只读文档 + 读代码 + grep；每条结论带 `文件:行号`。实测结论不在本文给出，另列 §4 交 remote-verifier。
> 纪律：本文严格区分「代码里真的有」/「文档声称有」/「测试真的覆盖」/「无法静态判断，需实测」。凡属第四类集中列在 §4。

---

## §1 结论摘要

**整体判定：部分符合（设备/数据面能力维度基本完备，接口集成与口径维度存在明确缺口）。**

一句话：第三组把「GRE/ip6gre 设备 + overlay 地址 + 接口 UP + READY 状态机 + 删除/丢失/重建」这套设备面能力**基本做全了**（组件级全覆盖、双容器与 stage E 脚本齐备），但**「第一组请求 → midrd 内创建」的生产调用路径尚未接入**（`midrd` 进程内只有 `init`，没有任何 `add()/del()` 调用方，仅工具与测试在调），且 **overlay「可达性检查」实为本地前缀推断而非真实探测**、**与第一组的接口语义会签尚未回签**。

**符合度口径（务必按此理解百分比）**：
- 统计对象 = §2 逐条矩阵中**全部可静态判定**的需求行，共 **53 行**：
  - §3.1–3.4 缺口 4 行；§5.1–5.3 生命周期 3 行；§6 五条失败回滚 5 行；§7 六条「第三组不应」6 行；
  - §9.1 组件测试 12 行（**注：需求文档 §9.1 实际列 12 个点，任务描述里的「十一个」系笔误，本文按文档实列的 12 个计**）；
  - §9.2 GRE 测试 6 行；§9.3 跨 BGP-only 验收 7 行；§10 完成标准 10 行。合计 53。
- 计分：`符合 = 1`，`部分符合 = 0.5`，`缺失 = 0`；`需实测`（仅指静态无法判定者）**不计入分母**，集中列 §4。
- 结果：**符合 48 行、部分符合 5 行、缺失 0 行**；符合度 = `(48×1 + 5×0.5) / 53` = **50.5 / 53 ≈ 95%**。
- **口径说明**：95% 全部来自「是否按需求实现了对应能力/覆盖」的静态判定，**不代表端到端闭环已通过**。§9.2 六项与 §9.3 七项的「运行是否 PASS」属实测，本文只判「脚本/断言是否真的存在并覆盖该项」；端到端结论见 §4。
- 「部分符合」的 5 行分别是：§3.4（生产调用方）、§5.1（可达性口径）、§6-第 5 步（可达性失败回滚）、§10-第 9 条（联合验收闭环）、§10-第 10 条（接口语义会签）。

---

## §2 逐条符合性矩阵

### §2.0 用户最关心的五问：速答表

| # | 问题 | 结论 | 关键证据 |
|---|---|---|---|
| Q1 | §3.3 的 `DEVICE_UP` / `READY` 两阶段是否真分离？`DEVICE_UP` 会不会被当成 `READY`？ | **真分离，不会混淆** | 状态机 `midr-virtual-link.c:631-644`（CREATING→DEVICE_UP）与 `:693-742`（CONFIGURING→READY 需地址可见 **且** IFF_UP）；`overlay_ready` 仅 `state==READY` 为真 `midr-virtual-link.c:190`；头文件显式声明 `DEVICE_UP` 不是 READY `midr-virtual-link.h:28-31` |
| Q2 | §3.2 overlay 地址是否走 Zebra/ZAPI，是否有 shell/netlink 残留？ | **走 ZAPI，生产路径无 shell/netlink 残留** | `zclient_send_interface_address_set/_unset/_admin_up`（`lib/zclient.c:5413/5451/5494`）→ zebra handler（`zebra/zapi_msg.c:4317/4379/4451`，派发 `:4612-4614`）；`midr-virtual-link.c` 无 `system/popen/netlink`（grep 无命中）；`gre-link-tool.c` 无 shell（仅注释提到历史写法 `:20-26`） |
| Q3 | §5.1 overlay 可达性检查是真实探测还是本地前缀推断？ | **本地前缀推断（非真实探测）** | `midr_virtual_link_overlay_reachable()` = `connected_lookup_prefix_exact()` + `if_is_up()`（`midr-virtual-link.c:335-351`）；头文件自认「v1 依赖 overlay_remote 落在前缀内推导，不做主动探测」`midr-virtual-link.h:237-244` |
| Q4 | §6「相同 ifname 不同 endpoint/overlay 必须拒绝或显式 rebind，不得静默覆盖」在哪？行为？ | **默认拒绝（EEXIST），仅 `MIDR_VLINK_F_REBIND` 显式先拆后建** | `midr-virtual-link.c:807-814`（同名不同 desc 且无 REBIND → `VALIDATE/EEXIST`）；`:836-839`（REBIND/FAILED 重试 → 先 `del()` 彻底拆除再建）；`:866-877`（外来 LIVE 隧道占同一 endpoint 对换名 → EEXIST）；标志位 `midr-virtual-link.h:115` |
| Q5 | §3.4 `midrd.c` 是否已接入 `midr_virtual_link_init`？有无真正生产调用方？ | **init 已接入；`add()/del()` 无生产调用方，仍只有工具/测试在调** | 生产接入：`midrd.c:3488`（init）、`:3175`（fini）；**无生产 `add()` 调用**：`midr_virtual_link_add` 全仓调用方仅 `midrd/gre-link-tool.c:331`、`midr-test/virtual-link-test.c`（grep 确认 `midrd.c`/`group1/` 零命中） |

---

### §2.1 §3.1–3.4 缺口（4 行）

| 需求条目 | 需求要点 | 实现位置（file:line） | 测试位置（file:line） | 判定 | 备注 |
|---|---|---|---|---|---|
| §3.1 完整虚链路描述 | 需要高层描述：设备名/VRF/outer local+remote/underlay ifindex/overlay local+prefix/overlay remote/MTU | `struct midr_virtual_link_desc` `midr-virtual-link.h:117-133`（字段齐全；`link_id` 刻意不入结构，符合「第三组不分配 link_id」） | `desc_v4()` `virtual-link-test.c:189-203` | **符合** | desc 覆盖需求列出的全部字段 |
| §3.2 配置 overlay 地址 | 经 Zebra/ZAPI 或 FRR 统一数据面接口配置，**不应要求第一组调 shell/raw netlink** | `advance()` 的 DEVICE_UP 分支发 `zclient_send_interface_address_set` `midr-virtual-link.c:670-679` | 组件：`case05` `virtual-link-test.c:581`（wrap 计数 `:111-133`）；双容器：脚本零手工 `ip addr add dev greX` | **符合** | 见 §2.0 Q2 |
| §3.3 拆 DEVICE_UP / READY 两阶段 | 设备状态至少两阶段，只有 READY 才通知第一组建邻 | 枚举 `midr-virtual-link.h:76-83`；状态机 `midr-virtual-link.c:624-754`；`overlay_ready` `:190` | `case14` `virtual-link-test.c:932`（断言三条有序通知 DEVICE_UP→ADDRESS_SET→READY） | **符合** | 见 §2.0 Q1；`ADDRESS_SET` 单列为第三里程碑（`midr-virtual-link.h:105-112`） |
| §3.4 生产调用方与完整生命周期 | 需补齐「第一组请求 → 创建/配置 → READY/FAILED 通知 → 丢失通知 → 删除/重建」 | 生命周期完整：`init/fini` `midrd.c:3488/3175`，状态机 `:624-754`，丢失 `:476-538`，重建 `:772-918`。**但 `midrd` 内无 `add()/del()` 调用方** | `case11`（删除重建 `:786`）、`case12`（丢失 `:824`）、`case13/18/19`（自主失败） | **部分符合** | 缺口=**生产触发路径未接入**：`midrd.c` 只 `init`（订阅 GRE 通知）+ `fini`，真正的创建仍仅由 `gre-link-tool.c:331`（独立进程工具）与组件测试驱动；「第一组请求 → 创建」这一腿在守护进程内**尚不存在** |

### §2.2 §5.1–5.3 生命周期（3 行）

| 需求条目 | 需求要点 | 实现位置（file:line） | 测试位置（file:line） | 判定 | 备注 |
|---|---|---|---|---|---|
| §5.1 建立 | 校验→创建 GRE→确认 ifindex→配 overlay 地址→置 UP→确认可达→READY；失败不得 READY 并回滚 | `advance()` 全流程 `midr-virtual-link.c:624-754`；回滚 `:419-454`；超时 `:645-650 / :712-719 / :737-742` | `case04-08` `virtual-link-test.c:553-713`；`case14` 顺序 `:932` | **部分符合** | 建立链路 1–7 步齐全；**第 8 步「确认 overlay remote 可达」为本地推断**（见 §2.0 Q3），并非对端真实可达性 |
| §5.2 删除 | 停通知→撤地址/置 DOWN→删设备→清 registry→报 DOWN；删除时不得调第一组 Session/Link API | 五步逐一对应 `midr-virtual-link.c:942-980`（`deleting` 抑制 `:943`、地址撤回 `:948-958`、设备删 `:961-964`、报 DOWN `:973`、清 registry `:975-977`） | `case11` `virtual-link-test.c:786-819`（断言 unset+1、del+1、get_state=-1） | **符合** | grep 确认模块内无 `midr_session_connect`/`topology_link_*`（仅头文件注释 `:37-38`） |
| §5.3 重连与设备丢失 | 报 DEVICE_DOWN/FAILED→旧 ifindex 失效→保留 desc→重建返回新 ifindex→不改 link_id | `midr_vlink_lost()` `midr-virtual-link.c:476-500`；reconcile 探测 `:508-538`；重建 `:772-918`；`link_id` 不在结构内 | `case12` `virtual-link-test.c:824-865`（ifindex 81→0→91）、`case13` 自主丢失 `:871`、`case18` 陈旧簿记 `:1239` | **符合** | 注意：模块**无独立的「zebra 重连」钩子**，靠 reconcile 定时器（1000ms `:40`）感知设备丢失 + 调用方再次 `add()` 重建；语义上满足需求，但「重连后自动恢复」由调用方触发（stage E E7.4 为可观察验证） |

### §2.3 §6 五条失败回滚（5 行）

> §6 原文列 5 个「失败不得报 READY」的步骤（GRE 创建 / Zebra interface 确认 / overlay 地址配置 / 接口置 UP / overlay 可达性检查）。

| 步骤 | 实现位置（file:line） | 测试位置（file:line） | 判定 | 备注 |
|---|---|---|---|---|
| GRE 创建失败 | `advance()` 回滚+FAILED `midr-virtual-link.c:646-649`（实际在 add 的 GRE_CREATE 分支 `:880-884`） | `case04` `virtual-link-test.c:553-576`（`GRE_CREATE/EIO`，`gre_del_calls==0` 无回滚对象） | **符合** | 明确 errno 风格返回 |
| Zebra interface 确认失败 | `midr-virtual-link.c:645-650`（DEVICE_CONFIRM/ETIMEDOUT） | `case07` `virtual-link-test.c:643-660`（`DEVICE_CONFIRM/ETIMEDOUT`，ifindex=0） | **符合** | 「确认失败」本实现只表现为**超时**（无负向确认 ZAPI） |
| overlay 地址配置失败 | `midr-virtual-link.c:670-678`（ADDRESS_SET/EIO，回滚设备） | `case05` `virtual-link-test.c:581-604`（`ADDRESS_SET/EIO`，`gre_del+1`、`addr_unset==0`） | **符合** | ifindex 保留（D2 规则 `:204-206`） |
| 接口置 UP 失败 | `midr-virtual-link.c:681-688`（ADMIN_UP/EIO，回滚地址+设备） | `case06` `virtual-link-test.c:609-638`（`ADMIN_UP/EIO`，`addr_unset+1`、`gre_del+1`） | **符合** | — |
| overlay 可达性检查失败 | `midr-virtual-link.c:737-742`（REACHABILITY/ETIMEDOUT，回滚） | **无独立组件用例**（最接近的 `case07` 是 DEVICE_CONFIRM 超时） | **部分符合** | 代码分支存在且会回滚；**无独立测试用例**触发 REACHABILITY 阶段超时（见 §3 偏离项、§4） |

**§6 其余处理要求（同一节，逐条核）**：

| 要求 | 证据 | 判定 |
|---|---|---|
| 返回明确 errno 风格错误 | 全流程 `status.last_error`（`midr-virtual-link.c:462/486/226`） | 符合 |
| status 保留失败阶段与 ifname/ifindex | `stage`/`ifname`/`ifindex` 填充 `midr-virtual-link.c:173-215`；D2 ifindex 规则 `:204-206` | 符合 |
| 创建后配置失败则回滚地址+设备或明确可重试 FAILED | `rollback()` `midr-virtual-link.c:419-454` | 符合 |
| 重复请求相同描述必须幂等 | `midr-virtual-link.c:799-823`（同 desc → 直接返回当前状态） | 符合（`case08` `virtual-link-test.c:674`） |
| 相同 ifname 不同 endpoint/overlay 必须拒绝或显式 rebind | `:807-814`（EEXIST）/`:836-839`（REBIND 先拆后建）/`:866-877`（外来 LIVE 拒绝） | 符合（`case09` `:715`、`case10` `:747`、`case16b` `:1139`） |
| 不得把 DEVICE_UP 当成 READY | `overlay_ready` 仅 READY 为真 `midr-virtual-link.c:190` | 符合（`case14` 断言 `:932`） |


### §2.4 §7 六条「第三组不应」（6 行）

| # | 「不应」条目 | 核查方法 | 证据 | 判定 |
|---|---|---|---|---|
| 1 | 自己决定是否建立 MIDR 邻接 | grep 模块内无 session/邻接决策 | `midr-virtual-link.c` 无 `midr_session_connect`（grep 命中仅头注释 `midr-virtual-link.h:37`） | 符合 |
| 2 | 自己分配第一组逻辑 `link_id` | 结构体无 link_id | `midr-virtual-link.h:117-133` 无 link_id 字段；`midr-virtual-link-api-countersign.md:105` 明示「link_id 不在本 API 内」 | 符合 |
| 3 | 直接调用 `midr_session_connect()` | grep | `midr-virtual-link.c`/`.h` 零调用（仅注释 `:37`） | 符合 |
| 4 | 直接调用 `midr_topology_link_upsert/withdraw()` | grep | 零调用（仅注释 `:38`） | 符合 |
| 5 | 将 GRE 外层地址作为 MIDR Link 的 overlay 下一跳 | 读状态填充 | `overlay_*` 来自 `desc.overlay_*`（`midr-virtual-link.c:187-189`），与 `outer_*` 分离；`fill_status` 不导出 outer 作为 nexthop | 符合 |
| 6 | 要求第一组通过 shell 配置设备 | 读工具与库 | `midr-virtual-link.c` 无 shell；`gre-link-tool.c` 只调 API（`:331/335`）；ZAPI 全程（`lib/zclient.c:5413/5451/5494`） | 符合 |

### §2.5 §9.1 组件测试点（12 行，按文档实列）

> 载体：`midr-test/virtual-link-test.c`（miniature midrd host，`#include "midrd.c"` `:44-45`；ZAPI 用 `-Wl,--wrap=` 注入，Makefile `midrd/Makefile:240`）；runner `:1353-1380`。

| # | 测试点（文档原文） | 用例（file:line） | 判定 | 备注 |
|---|---|---|---|---|
| 1 | 缺失 outer endpoint | `case01` `virtual-link-test.c:465-492`（`VALIDATE/EINVAL`） | 符合 | — |
| 2 | 地址族不一致 | `case02` `:494-512`（`EAFNOSUPPORT`） | 符合 | 覆盖 outer 与 overlay 族不一致 |
| 3 | overlay 地址或 prefix 非法 | `case03` `:514-551`（prefix=33/0、remote 超前缀、族不符） | 符合 | — |
| 4 | Zebra 创建失败 | `case04` `:553-576`（GRE_CREATE/EIO） | 符合 | — |
| 5 | overlay 地址配置失败 | `case05` `:581-604`（ADDRESS_SET/EIO） | 符合 | — |
| 6 | 接口置 UP 失败 | `case06` `:609-638`（ADMIN_UP/EIO） | 符合 | — |
| 7 | READY 前超时 | `case07` `:643-672`（DEVICE_CONFIRM/ETIMEDOUT） | 符合 | 覆盖「设备确认」超时；**REACHABILITY 超时无独立用例**（见 §3） |
| 8 | 重复 add 幂等 | `case08` `:674-713`（同 desc 返回 READY，无第二次 ZAPI） | 符合 | 双容器 D 段同验证 `midr-gre-connectivity-test.sh:370-376` |
| 9 | 相同 ifname 不同 endpoint 拒绝或显式 rebind | `case09` `:715-745`（EEXIST）、`case10` `:747-780`（REBIND 先拆后建）、`case16b` `:1139` | 符合 | 真实内核版见 §9.2 用例 F |
| 10 | 删除和重建 | `case11` `:786-819`（ifindex 61→71） | 符合 | — |
| 11 | 设备消失后的状态通知 | `case12` `:824-865`、`case13` `:871-929`、`case19` `:1296-1332`（自主，无需轮询） | 符合 | — |
| 12 | ifindex 变化后状态正确更新 | `case12` `:856-860`、`case15` `:1005`（FAILED 保留规则） | 符合 | — |

**本节 12/12 全部有对应组件用例**；另有 7 个扩展用例（`case14` 通知顺序与 overlay 承载 `:932`、`case16a` 空名重建 `:1053`、`case17` 查询不发通知 `:1183`、`case18` 陈旧簿记 `:1239`、`test_alloc_failure` `:1334`）与 `midr-test/gre-registry-test.c`、`midr-test/dp-backend-test.c`，以及 ZAPI 编号门禁 `midr-test/check-zapi-numbering.sh`（白名单见 `:15-16`）。


### §2.6 §9.2 GRE/IP 隧道测试六项（6 行）

> 载体：`midr-test/midr-gre-connectivity-test.sh`（双容器，node1↔node2；overlay 一律经 `gre-link-tool vlink-setup` 下发，`vlink1/vlink2` `:185-194`；断言器 `assert_vlink_ready` `:225-239`、`assert_vlink_overlay` `:211-223`、`iface_kind` `:165-174`）。**注意：脚本里残留的 `ip addr add` 全部是给 underlay `eth0` 配地址（`:288-300, :412-413`），overlay 不再手工配置；`ip link del` 仅出现在 teardown 兜底/清理（`:262, :293`）。**

| # | 检查项（文档原文） | 脚本证据（file:line） | 判定 | 备注 |
|---|---|---|---|---|
| 1 | 设备类型正确（gre/ip6gre） | `iface_kind` 断言 gre `:316-317`、gre2 `:337-338`、ip6gre `:358-359` | 符合 | — |
| 2 | 设备状态为 READY | `assert_vlink_ready` gre1 `:312-313`、gre2 `:333-334`、gre6 `:354-355` | 符合 | — |
| 3 | overlay 地址已配置 | `iface_has_addr` gre1 `:318-321`、gre2 `:339-342`、gre6 `:360-363` | 符合 | 静态核对内核地址 |
| 4 | overlay 双向 ping 成功 | ping4 gre1 `:323-326`、ping6 gre2 `:344-347`、ping6 gre6 `:365-368` | 符合 | — |
| 5 | 返回 ifindex 有效 | `assert_vlink_ready` 内比对 API ifindex == 内核 `/sys/class/net/.../ifindex` `:234-238` | 符合 | — |
| 6 | 删除后设备和地址均清理 | teardown E 段 `:378-395`（设备 + overlay 地址均断言已清） | 符合 | — |

> 运行是否 PASS 属实测：见 §4（历史值：GRE 51→58 PASS，`midr-virtual-link-third-group-verification.md:27/49`）。

### §2.7 §9.3 跨 BGP-only 区域验收七项（7 行）

> 载体：`midr-test/r7-dp-stagee-bgp-underlay.sh`（两个独立 docker bridge ⇒ outer 只能经 BGP 学到，`:10-13`；overlay 经 `vlink-setup` `:225-226`）。

| # | 验收项（文档原文） | 脚本证据（file:line） | 判定 | 备注 |
|---|---|---|---|---|
| 1 | outer endpoint 经 underlay 可达 | E1 `:195-220`（proto bgp 路由 + 无直连 + `route get` + ping） | 符合 | 隔离网段设计保证「非直连」 |
| 2 | 两端 GRE/ip6gre 设备都进入 READY | E2 `:224-234`（两端 READY + API ifindex==内核） | 符合 | — |
| 3 | overlay 地址和接口状态正确 | E3 `:236-245`（地址精确匹配 + UP） | 符合 | — |
| 4 | `route get overlay-remote` 选择虚接口 | E5 `:251-254`（`route get $OVL_B` 命中 `dev $DEV`） | 符合 | — |
| 5 | 返回的 ifindex 可交给第一组/第二组 | E6 `:256-270`（FIB 安装 `:259-260`、FIB 条目 `:264-266`、数据面 lookup `:267-269`） | 符合 | — |
| 6 | Zebra/FIB 能使用该 ifindex 安装业务路由 | E6.1–E6.3 `:259-269` | 符合 | 用 `ip route add ... via $OVL_B dev $DEV` 证明 |
| 7 | 删除、Zebra 重连、设备重建后状态可恢复 | E7 `:272-297`（teardown `:273-278`、重建 `:280-283`、zebra 重启 `:285-291`、内核删设备重建 `:293-297`） | 符合 | — |

> 运行是否 PASS 属实测：见 §4（历史值：stage E 34 PASS，`midr-virtual-link-third-group-verification.md:28/460`）。**MIDR Session/EOR/SPF 闭环不在本组范围**（报告 U-5 `:595`）。

### §2.8 §10 十条完成标准（10 行）

| # | 完成标准（文档原文缩略） | 实现/文档证据（file:line） | 判定 | 备注 |
|---|---|---|---|---|
| 1 | 提供可被第一组直接调用的高层虚链路 API | `midrd/midr-virtual-link.h:176-219`（add/del/get_state/wait_ready/register_notify） | 符合 | 签名与会签包一致 `api-countersign.md:37-53` |
| 2 | API 不要求第一组执行 shell 或 raw netlink | ZAPI 全程；模块/工具无 shell/netlink（§2.0 Q2） | 符合 | — |
| 3 | API 完成 GRE 创建、overlay 地址配置和接口 UP | `midr-virtual-link.c:631-692` | 符合 | — |
| 4 | READY 明确表示设备、地址和本地数据面配置完成 | `overlay_ready` `:190`；`overlay_reachable` `:335-351` | 符合 | 「本地数据面配置完成」成立；**对端可达性不在其中**（§2.0 Q3） |
| 5 | 能返回稳定有效的虚接口 ifindex | `probe_device` `:317-333`；status `:145-159` | 符合 | — |
| 6 | 支持删除、故障通知、Zebra 重连和重建 | `del` `:921-981`；`lost/reconcile` `:476-538`；重建 `:772-918` | 符合 | Zebra 重连靠 reconcile+再 add（无独立钩子） |
| 7 | 组件测试覆盖失败回滚、幂等和状态转换 | `virtual-link-test.c` case04-19 | 符合 | REACHABILITY 失败无独立用例（见 §3） |
| 8 | GRE/IP6GRE 测试不再依赖外部手工地址配置 | overlay 经 vlink-setup；脚本 `ip addr add` 仅 underlay | 符合 | underlay 仍需脚本配地址（属 device configuration 之外的数据面拓扑搭建） |
| 9 | 跨 BGP-only 的联合验收能证明实际出口为 GRE 虚接口 | stage E E5/E6 `r7-dp-stagee-bgp-underlay.sh:251-269` | **部分符合** | 脚本与历史日志齐备；**本轮 PASS 属实测**且**联合闭环（Session/SPF/FIB）在报告 U-5 标为本组边界外**（`:595`） |
| 10 | 与第一组明确 READY/FAILED/DOWN/ifindex/overlay 地址的接口语义 | 会签包 `api-countersign.md`（状态枚举 `:55-67`、字段 `:84-103`、C1–C7 `:113-119`、九问 `:130-156`） | **部分符合** | **会签未回签**：`verification.md:598`（U-8「仍待会签（未取得回签）」），表内「第一组」栏为空白 |


---

## §3 未满足或偏离项清单（按严重度排序）

> 共 **6 项**（1 高、3 中、2 低），另 1 项为信息性提示。均为**静态可判定**的偏离；不包含需实测才能定论者（后者见 §4）。

### D-1【高】生产调用路径未接入：`midrd` 守护进程内没有任何 `midr_virtual_link_add()/del()` 调用方

- **现状**：`midrd.c:3488` 只调用 `midr_virtual_link_init()`（订阅 GRE 层丢失通知，`midr-virtual-link.c:1153-1159`），`:3175` 调用 `midr_virtual_link_fini()`。全仓 `midr_virtual_link_add` 的调用点只有两处：`midrd/gre-link-tool.c:331`（**独立进程**的调试工具）与 `midr-test/virtual-link-test.c`（组件测试）。`midrd.c` 与 `midrd/group1/` 对 `add()/del()/wait_ready()` **零命中**（grep 已确认，`EXIT=1`）。
- **文档要求**：§3.4「当前 `midrd.c` 只初始化 GRE 模块，没有根据第一组请求创建虚链路的生产调用路径。需要补齐：第一组请求 → 第三组创建/配置设备 → READY/FAILED 通知 → …」；§10.6「支持删除、故障通知、Zebra 重连和重建」。
- **差距**：设备面 API 与生命周期**已在库内实现并被测试全覆盖**，但**守护进程内没有把「第一组请求」接进 `add()` 的代码**。`add()` 目前只能由独立工具/测试进程驱动。
- **影响第一组什么**：第一组若要真正建链，必须先自行在 `midrd` 内新增调用点（并 `midr_virtual_link_register_notify()` 注册回调，否则收不到自主 `FAILED`，见会签 C7 `midr-virtual-link-api-countersign.md:118`）。当前**还不能**通过任何 `midrd` 内既有 CLI/IPC 触发一次虚链路创建。
- **建议动作**：明确该集成点是第一组的职责还是第三组的交付；若属第三组，需补一个最小的生产触发面（如 VTY/IPC）并在交付说明中写清；否则应在文档中把 §3.4 的完成口径改为「API 就绪 + 组件验证，生产触发待第一组接入」。

### D-2【中】overlay「可达性检查」实为本地前缀推断，非真实探测

- **现状**：`midr_virtual_link_overlay_reachable()`（`midr-virtual-link.c:335-351`）只做 `connected_lookup_prefix_exact(ifp, &p)`（本端 overlay 前缀在内核视图可见）+ `if_is_up(ifp)`；头文件自述「v1 relies on the connected route implied by overlay_remote being inside the prefix; **no active probe is performed**」（`midr-virtual-link.h:237-244`）。可达性间接由校验 `overlay_remote ∈ overlay_local/prefix_len` 推导（`midr-virtual-link.c:309-312`）。
- **文档要求**：§5.1 建立流程第 8 步「确认 overlay remote 可达性」；§6 第 5 条失败步骤「overlay endpoint 可达性检查」；§3.3「overlay remote 已经可达」是 READY 的应有含义之一。
- **差距**：代码**不探测对端是否为真的在线/可达**，只确认「本端地址已配 + 接口 UP + 对端地址落在本端前缀内」。对端宕机时本端仍可报 `READY`。此项在实现文档（`midr-virtual-link.h:237-240`）已明确为 v1 的设计取舍，属**口径收窄**而非缺陷。
- **影响第一组什么**：**第一组不得把 `READY` 理解为「对端可达」**——`READY` 只保证本端设备/地址/接口就绪。对端是否可达仍需第一组/第二组经 Session HELLO/Established 确认（这正是 §8 的边界，见 §5）。
- **建议动作**：将「可达性 = 本地前缀推断」写入已回签的接口语义（§10.10）；若需真实探测，由第一/二组另行提出（可能涉及对端心跳/ARP/ND 或经 Session 确认）。

### D-3【中】REACHABILITY 阶段失败路径缺独立组件用例

- **现状**：代码有 `MIDR_VLINK_STAGE_REACHABILITY` 失败分支（`midr-virtual-link.c:737-742`，超时→回滚→FAILED），但 `midr-test/virtual-link-test.c` 的 19+ 用例中**没有**触发该分支者——`case07` 只在「设备从未出现」时触发 `DEVICE_CONFIRM/ETIMEDOUT`（`:643-660`），`case14` 只走成功序（`:932`）。模拟器可设「地址已配但 IFF_UP 未置」（`sim_overlay_address` `:266-280` 不动 IFF_UP）却未被任何用例用于超时。
- **文档要求**：§6 第 5 条失败步骤必须「不能报告 READY」并有失败处理；§9.1「READY 前超时」。
- **差距**：`DEVICE_CONFIRM` 超时有 `case07` 覆盖；`REACHABILITY` 超时**只靠读代码，无组件用例**。
- **影响第一组什么**：低——分支存在且会回滚；但该路径无回归保护，未来改动可能悄悄破坏。第一组依赖的「READY 前不会假就绪」在**可达性阶段**缺测试背书。
- **建议动作**：补一个组件用例：`sim_device_up` + `sim_overlay_address`（不 `sim_set_if_up`）→ 断言 `REACHABILITY/ETIMEDOUT` + 回滚。

### D-4【中】与第一组的接口语义会签未回签（`§10.10`）

- **现状**：会签包 `midr-virtual-link-api-countersign.md` 的 §7 会签表「第一组」栏为空（`:280`）；验证报告把该项列为 `U-8「仍待会签（未取得回签）」`（`midr-virtual-link-third-group-verification.md:598`）。C6/C7 明确是对**调用方**的行为变更（必须按 `status->event` 分支、必须注册 `register_notify()` 才能收自主 `FAILED`、`FAILED` 的 ifindex 规则）。
- **文档要求**：§10.10「与第一组明确 READY、FAILED、DOWN、ifindex 和 overlay 地址的接口语义」。
- **差距**：接口已冻结且实现就绪，但**第一组尚未书面确认**。
- **影响第一组什么**：存在第一组按「旧语义」编码的风险（例如只看 `state` 不看 `event`、不注册回调、误以为 `FAILED` 时 `ifindex` 恒为 0）。这些都是**已写明的变更点**，未回签则无法冻结。
- **建议动作**：推动第一组在会签表 §7 回签（或对 C1–C7/九问逐条给结论），并把回签结果反映到本文与契约。

### D-5【低】「Zebra 重连」无独立钩子，进程内状态迁移不可由外部观察

- **现状**：模块无 `zclient` 重连回调，仅靠 1s 周期 reconcile 定时器（`midr-virtual-link.c:40/590-617`）与 `probe_device`（`:317-333`）感知设备丢失；「重连后旧 ifindex 失效、desc 保留」的**进程内**迁移无法经 CLI 观察（验证报告自述 `:566-568`），阶段 E 只证明「重启后能再次 `add()` 达成 READY」（`r7-dp-stagee-bgp-underlay.sh:285-291`）。
- **文档要求**：§5.3「如果 Zebra 重连…应报告 DEVICE_DOWN/FAILED、旧 ifindex 失效…」。
- **差距**：语义实现存在，但**依赖调用方再次 `add()`**，且**无专门的 zebra-reconnect 入口**；跨进程工具无法观测该迁移。
- **影响第一组什么**：低。第一组需知道「重连后**不会自动**重建，需要再次调用 `add()` 或依赖 reconcile 判定失败后自行处理」。
- **建议动作**：在契约里写清「zebra 重连的自动恢复边界」（模块负责报失败、调用方负责重建），避免第一组误以为守护进程会自动重连虚链路。

### D-6【低】测试脚本中 underlay 仍用 `ip addr add`（overlay 已零手工）

- **现状**：`midr-test/midr-gre-connectivity-test.sh` 仍有多处 `ip addr add $U.../24 dev eth0`（`:288-300, :412-413`）用于**外层 underlay** 地址；`ip link del`/`ip route add`/`ip link show` 用于 teardown 兜底与验证（`:262-263, :293, :259`）。stage E 亦用内核 `ip` 做验证（`r7-dp-stagee-bgp-underlay.sh:259-269`）。**overlay 地址的配置已全部走 `vlink-setup`**（`:185-194, :308-315`），无 `ip addr add ... dev greX`。
- **文档要求**：§9.2 要求把 `ip addr add ... dev greX` / `ip link set greX up` 改为经新 API；§10.8「GRE/IP6GRE 测试不再依赖外部手工地址配置」。
- **差距**：**明文要求的 overlay 手工配置已消除**；保留的 `ip` 用法属 underlay 拓扑搭建与结果核验，不违反 §9.2。仅需知悉：§9.2 的措辞「零手工 `ip`」在 underlay 层面并不成立。
- **影响第一组什么**：无（第一组不涉及这些脚本）。
- **建议动作**：无强制动作；可在脚本注释中说明「残留 `ip addr add` 仅用于 underlay」。

### D-7【信息性】`midrd` 对同源 `libfrr` 形成硬运行期依赖

- **现状**：新增 3 个 ZAPI 发送助手位于 `lib/zclient.c`（`:5413/5451/5494`），`midrd` 运行期需要同源 `libfrr`；否则报 `undefined symbol: zclient_send_interface_address_set`（验证报告 §0.2 新发现 2，`:64-72`）。
- **文档要求**：无直接条款（属部署事项）。
- **差距**：无功能偏离。
- **影响第一组什么**：部署/联调时须 `LD_LIBRARY_PATH` 指向树内 `libfrr`，否则第一组集成会踩符号解析失败。
- **建议动作**：在构建/部署文档中固化该前置条件。


---

## §4 需要实测才能定论的清单（交 remote-verifier）

> 以下各项静态只能判定「代码/脚本是否具备」，**运行结果一律不下结论**。**全部交给 remote-verifier。** 每条给出「静态已观察到的事实」与「需实测的断言」。

| 编号 | 对象 | 静态已观察到的事实（file:line） | 需实测的断言（交 remote-verifier） |
|---|---|---|---|
| R-1 | 组件套件 | `midrd-virtual-link-test` runner 20 个用例齐备（`virtual-link-test.c:1360-1380`），Makefile wrap 规则在 `midrd/Makefile:240` | 编译 0 warning、套件 `EXIT=0`、各用例断言真的通过（尤其 `case14` 通知顺序、`case16b` EEXIST、`case19` 自主失败） |
| R-2 | 双容器 GRE（A–F） | 六个场景断言在 `midr-gre-connectivity-test.sh:307-484` | 实际 PASS 计数（历史 51→58）；F 段真实内核「拒绝且不改绑外来设备」 |
| R-3 | stage E（BGP-only） | 七项断言在 `r7-dp-stagee-bgp-underlay.sh:195-297` | 实际 PASS=34；尤其 **E5 `route get overlay_remote` 命中 `dev gre1`**、E6 FIB/数据面命中、E7 zebra 重启与设备重建后恢复 |
| R-4 | 真实内核 overlay 地址正确性 | zebra 侧 `zebra_midr_ipaddr_to_prefix()` 已删除 `apply_mask()`（验证报告 §3.1 C4 `:240-267`） | 真实内核可见 `inet 192.168.100.1/30`（**主机位不被清零**）；READY 确实可达（否则第一组永远等不到 READY） |
| R-5 | 跨进程 EEXIST 语义 | 进程内 EEXIST 守卫 `midr-virtual-link.c:866-877`；脚本按「拒绝（17 或 110）」断言 `midr-gre-connectivity-test.sh:434-442` | **跨进程**时实际错误码是 `110(DEVICE_CONFIRM 超时)` 还是 `17(EEXIST)`；外来设备 ifindex/地址是否未被改动 |
| R-6 | ZAPI 编号门禁 | 白名单与逻辑在 `check-zapi-numbering.sh:15-37` | 干净工作树 PASS；人为交换 `ZEBRA_GRE_ADD/_DELETE` 编号时应 FAIL（mutation teeth） |
| R-7 | 守护进程未定义符号 | `midrd` 依赖 `lib/zclient.c` 新增 3 助手（`:5413/5451/5494`） | 用系统旧 `libfrr` 直跑应报 `undefined symbol: zclient_send_interface_address_set`；`LD_LIBRARY_PATH` 指向树内 lib 后正常 |
| R-8 | CLI 端到端 | `midrd` 有 `--vty_socket/--no-zebra` 路径（`midrd.c:172/226`） | 树内 `vtysh` 能连 `midrd`；`show midr spf`/`clear midr traceroute cache` 返回与 daemon stdout 一致 |
| R-9 | 顶全量构建 | — | `make -j -k` `EXIT=0`、`warning:` = 0（排除既有 `lib/mgmt_msg_native.h` 噪声） |
| R-10 | 「DEVICE_UP 不会被当 READY」的运行期行为 | 静态已证：`overlay_ready` 仅 READY 为真（`midr-virtual-link.c:190`）；`ADDRESS_SET` 时刻 `overlay_ready==false` | 运行期确认：`ADDRESS_SET` 通知确实 `overlay_ready=false`、`READY` 通知 `overlay_ready=true`（组件 `case14`/脚本 `:205-223`） |

> 说明：R-4/R-5 是历史报告已记录「上轮通过」的项，但**本轮静态审计未重跑**；按纪律不得据旧日志判定本轮通过，故仍列实测。

---

## §5 与需求文档 §8「Session 实际选路检查」相关的诚实说明

> 需求文档 §8（`midr-virtual-link-third-group-implementation.md:318-332`）指出：`midr_session_connect()` 不接收本地 ifindex/源地址，出站连接用 `midrd` 的本地监听配置 bind，因此**第三组不能仅凭 `READY` 保证 MIDR Session 会经 GRE**。以下用代码证据复核该论断，界定第三组「能保证 / 不能保证」的边界。

### §5.1 第三组能保证什么

- 设备存在且 `ifindex` 有效：`midr-virtual_link_probe_device()`（`midr-virtual-link.c:317-333`，`if_lookup_by_name` + `IFINDEX_INTERNAL` 过滤）。
- 本端 overlay 地址已配：经 `zclient_send_interface_address_set`（`midr-virtual-link.c:670-679`）→ zebra（`zebra/zapi_msg.c:4317-4369`，`if_ip_address_install`）。
- 接口已 `IFF_UP`：`zclient_send_interface_admin_up`（`midr-virtual-link.c:681-689`）→ zebra（`zebra/zapi_msg.c:4451-4481`，`if_no_shutdown`）。
- `READY` 通知携带 `overlay_local/overlay_remote/overlay_prefix_len`（`midr-virtual-link.c:187-190`；`midr-virtual-link.h:145-159`），第一组可直接拿来建 Session。
- 删除/丢失时本端设备与地址被清理、旧 ifindex 失效（`midr-virtual-link.c:476-500, 921-981`）。

**以上全部只描述「本端设备/地址/接口」。**

### §5.2 第三组**不能**保证什么（`READY` ≠ Session 会走 GRE）

代码证据（三条独立事实，缺一即可导致 Session 不经 GRE）：

1. **`midr_session_connect()` 不携带本地源/ifindex**：签名仅 `(struct midr_context *ctx, const struct midr_session_endpoint *remote)`（`midr-session.c:597-598`），内部只做 `request_source(..., MIDR_SESSION_SOURCE_DISCOVERY)`（`:606-607`），**不接收 overlay 地址、不走 ifindex**。
2. **出站连接的源地址是单一 `transport->config.local`**：`connect_peer_now()` 在 `sockunion_connect()` 前，用 `sockunion_bind(fd, &transport->config.local, 0, ...)` 绑定本地地址（`midr-transport.c:699-712`；注释 `:699-700` 明确「overlay 会话是多跳，不绑监听地址时内核会选出口链路地址作源」）。**该 `config.local` 是唯一值**。
3. **监听也是单一地址**：`midr_transport_start()` 只 bind 一个 `transport->config.local`（`midr-transport.c:864-908`）；`midrd` 只解析一个 `--listen HOST:PORT`（`midrd.c:3407`，摘要 `:3553-3560`，`daemon.listen = transport_config.local` `:3560`；`midr_context_listen_endpoint()` `:108-115`）。

**结论**：`READY` 只保证「本端 GRE 设备 + overlay 地址 + 接口 UP」。**内核出站选路/源地址仍由路由与这唯一一份 `config.local` 决定**；若 `config.local` 未覆盖 overlay 地址、或物理链路与 overlay 链路并存，出站 Session 可能走物理接口而非 GRE。这与文档 §8 的论断**完全一致**，由代码独立证实。

### §5.3 需要第一组/第二组联合确认的五点（原样保留文档 §8）

第三组无法单方面解决，需第一/二组联合实测：

1. `midrd` 监听地址是否覆盖 overlay 地址（单监听 vs 物理/overlay 两类邻接）；
2. 出站连接源地址是否正确（是否绑到 overlay 地址）；
3. 到 `overlay_remote` 的 `route get` 是否显示 GRE 设备（stage E E5 已对**特定**拓扑证明命中，`r7-dp-stagee-bgp-underlay.sh:251-254`，但不构成通用保证）；
4. 物理 Link 与 Overlay Link 并存时能否选择正确 endpoint；
5. 对端 HELLO 的身份准入是否正确。

> 文档 §8 亦提示：若现有单一监听地址无法同时满足物理与 overlay 两类邻接，需由第一/二组另行提出 Session API 或 runtime 调整——**这不是 GRE 设备 API 单独能解决的问题**（`midr-virtual-link-third-group-implementation.md:332`）。

---

## 附录：审计所用证据索引（关键 file:line）

| 主题 | 位置 |
|---|---|
| 状态/事件/阶段枚举 | `midrd/midr-virtual-link.h:76-112` |
| desc/status 结构 | `midrd/midr-virtual-link.h:117-159` |
| 状态机（DEVICE_UP→CONFIGURING→READY） | `midrd/midr-virtual-link.c:624-754` |
| 回滚 / 失败 / 丢失 | `midrd/midr-virtual-link.c:419-500` |
| reconcile + GRE 通知订阅 | `midrd/midr-virtual-link.c:508-617, 1153-1159` |
| add（幂等 / EEXIST / REBIND / 外来隧道） | `midrd/midr-virtual-link.c:772-918` |
| del / get_state / wait_ready | `midrd/midr-virtual-link.c:921-1077` |
| ZAPI 发送助手 | `lib/zclient.c:5413/5451/5494`；`lib/zclient.h:1507-1514` |
| zebra 侧地址/UP 处理 | `zebra/zapi_msg.c:4317/4379/4451`，派发 `:4612-4614` |
| 生产接入（init/fini） | `midrd/midrd.c:3488 / :3175` |
| Session 连接（无本地源） | `midrd/midr-session.c:597-608` |
| transport 出站 bind / 监听 | `midrd/midr-transport.c:699-712 / :864-908` |
| 工具（vlink-setup） | `midrd/gre-link-tool.c:307, 316-342` |
| 组件测试 | `midr-test/virtual-link-test.c:465-1380` |
| GRE 双容器脚本 | `midr-test/midr-gre-connectivity-test.sh:185-484` |
| stage E 脚本 | `midr-test/r7-dp-stagee-bgp-underlay.sh:195-297` |
| 会签包 / 验证报告 | `midr-virtual-link-api-countersign.md`、`midr-virtual-link-third-group-verification.md` |
| 基线 | `git rev-parse HEAD` = `027bde5bc8ad0a6ac4d8afb3b2286014a6641ce1` |

> 本文为**静态审计**产物，未执行任何构建或运行；未修改 `midrd/`、`lib/`、`zebra/` 或任何既有文档。

