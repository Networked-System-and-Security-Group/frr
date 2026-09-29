# MIDR 虚链路 API 会签包（第一组 ↔ 第三组）

> 用途：第三组（数据面）请第一组（控制面/调用方）对虚链路设备服务接口**逐条确认并回签**。
> 基线：分支 `feat/midr-three-way-integration`，HEAD `443c51474b`。
> 会签对象（唯一准一来源）：`doc/midr-doc/dp-doc/midr-virtual-link-api.md`（**651 行**；口径：以回签当场 `wc -l` 实测为准，出包日 2026-09-29 实测 = 651）。
> 实现（权威签名）：`midrd/midr-virtual-link.h`。
> 上游任务说明：`doc/midr-doc/dp-doc/midr-virtual-link-third-group-implementation.md`。
> 出包日期：2026-09-29。

---

## 1. 为什么需要会签

任务文档 §4 明确要求：**「建议的概念接口如下，具体 C 签名由第一组和第三组联合确认」**。

第三组按该草案冻结了实现（**W3 之前一轮**已通过组件套件 + 双容器 GRE + 跨 BGP-only underlay
全部实测，合计 **106 PASS / 0 FAIL（该轮值，仅作历史对照）**，见验证报告 §0.1），但**第一组是本 API 的调用方**，
因此下列语义必须由第一组确认，否则会出现编译期不一致，或更危险的**语义错配**
（例如把 `DEVICE_UP` 当作可以建邻、或复用同一 outer endpoint 对时不先 `del`）。会签即为此。

> **本轮（W3，2026-09-29）新增面**：新增 `enum midr_virtual_link_event` 与 status 的
> `event`/`overlay_prefix_len`/`overlay_ready` 字段，并按 D1-D4 修正自主检测与重建语义
> （见 §3 的 **C6 / C7**）。**回调签名不变，调用方源码兼容。**
> **本轮扩展后的实测已完成**：W3 当轮为 **112 PASS / 0 FAIL**（见
> `doc/midr-doc/dp-doc/midr-virtual-link-third-group-verification.md` §0）；**最新独立实测（当前权威）见
> `doc/midr-doc/dp-doc/midr-virtual-link-runtime-evidence.md`：PASS=179 / FAIL=3**，
> 3 个 FAIL 均已归因（① 出厂 manifest 与 `tests/bgpd/subdir.am` 不同步 → exit=2；
> ② 以 root 跑组件套件时 `test_midr_sequence` 的 `-EACCES` 断言失败，换 `-u frr` 即 PASS；
> ③ fixture 二进制当时未构建，构建后 PASS），**与 overlay 逻辑无关**；
> 核心套件全绿：编号门禁 4/0、非特权组件套件 30/0、midrd 套件 27/0、双容器 GRE 58/0、
> stage E 34/0、ZAPI/FIB smoke 26/0。
> 上一轮的 **106 PASS（W3 之前）**只对应修正前的源码，**不得**当作本轮扩展面的证据。

**回签方式（二选一）**：
- 对本文 §5 的 9 个问题在「第一组回答」列填写 `同意` / `改为：____`；
- 或直接对 `midr-virtual-link-api.md` 给出「照此批准」或「字段/签名修改清单」。

回签后由第三组在**同一次提交**内更新文档与代码（`midrd/midr-virtual-link.{c,h}` + 契约文件）。

### 1.1 计数轮次与当前权威（更正块）

本包出现**三个不同轮次**的计数；历史值**保留不改**，此处只明确其轮次、证据与适用性：

| 计数 | 轮次 | 证据文档 | 适用性 / 定位 |
|---|---|---|---|
| **106 PASS / 0 FAIL**（组件 27 + GRE 45 + stage E 34） | **W3 之前**一轮（修正前源码） | `midr-virtual-link-third-group-verification.md` §0.1（「上一轮（旧源码）记录」） | **仅历史**；§1 与 §3 引用的即此值，不代表 W3 及以后 |
| **112 PASS / 0 FAIL**（组件 27 + GRE 51 + stage E 34） | **W3**（2026-09-29，D1–D4 修正后） | `midr-virtual-link-third-group-verification.md` §0（§0.2：结构迁移后 GRE 升为 58） | 对 W3 树有效；≠ 106（GRE 51 vs 45），也 ≠ 当前树 |
| **PASS=179 / FAIL=3** | **本轮最新独立实测**（2026-09-29 之后） | `midr-virtual-link-runtime-evidence.md`（987 行） | **当前权威**；3 个 FAIL 已逐一归因，与 overlay 逻辑无关 |

**当前权威实测 = `midr-virtual-link-runtime-evidence.md`（PASS=179 / FAIL=3，3 个 FAIL 已归因、与 overlay 无关）。**
回签核对以该文为准；§7.1 的行数以**当场 `wc -l` 实测**为准，上表 106 / 112 只作历史对照。

---

## 2. 最终对外 API 摘要（`midrd/midr-virtual-link.h`）

```c
int  midr_virtual_link_add(struct midr_context *ctx,
                           const struct midr_virtual_link_desc *desc,
                           struct midr_virtual_link_status *status);
int  midr_virtual_link_del(struct midr_context *ctx, const char *ifname,
                           struct midr_virtual_link_status *status);
int  midr_virtual_link_get_state(struct midr_context *ctx, const char *ifname,
                                 struct midr_virtual_link_status *status);
bool midr_virtual_link_wait_ready(struct midr_context *ctx, const char *ifname,
                                  uint32_t timeout_ms,
                                  struct midr_virtual_link_status *status);
void midr_virtual_link_register_notify(midr_vlink_notify_cb cb, void *arg);
void midr_virtual_link_unregister_notify(midr_vlink_notify_cb cb);
const char *midr_virtual_link_state_str(enum midr_virtual_link_state state);
void midr_virtual_link_init(struct event_loop *master);
void midr_virtual_link_fini(void);
```

### 2.1 状态枚举（**只有 READY 代表可以继续建邻**）

| 值 | 含义 |
|---|---|
| `MIDR_VLINK_DOWN` | 未请求，或已删除 |
| `MIDR_VLINK_CREATING` | GRE 创建已下发，等 ifindex |
| `MIDR_VLINK_DEVICE_UP` | zebra 已知设备，ifindex 有效（**≠ READY**） |
| `MIDR_VLINK_CONFIGURING` | 正在下发 overlay 地址 / 链路状态 |
| `MIDR_VLINK_READY` | **设备 + overlay 地址 + 接口 UP 全部完成** |
| `MIDR_VLINK_FAILED` | 某步失败或被拒绝，见 `stage` / `last_error` |

失败阶段枚举 `midr_virtual_link_stage`：
`NONE / VALIDATE / GRE_CREATE / DEVICE_CONFIRM / ADDRESS_SET / ADMIN_UP / REACHABILITY`。

### 2.1.1 通知事件枚举（**本轮新增 C6**）

`enum midr_virtual_link_event`（触发本次通知的迁移；**随未改变的回调签名传递**）：

| 值 | 含义 |
|---|---|
| `MIDR_VLINK_EV_NONE` (=0) | 仅**查询路径**（`get_state()`/`wait_ready()`）返回；永不作为通知发出 |
| `MIDR_VLINK_EV_DEVICE_UP` | zebra 已确认设备、ifindex 有效（**≠ READY**） |
| `MIDR_VLINK_EV_ADDRESS_SET` | overlay local/prefix 已在设备上可见；`overlay_ready == false` |
| `MIDR_VLINK_EV_READY` | 设备 + overlay 地址 + `IFF_UP` 齐备；`overlay_ready == true` |
| `MIDR_VLINK_EV_FAILED` | 某阶段失败，含**自主检测**到的设备/地址丢失 |
| `MIDR_VLINK_EV_DOWN` | 已删除 |

成功建立路径上顺序固定为 **`DEVICE_UP` -> `ADDRESS_SET` -> `READY`（三条独立通知）**。

### 2.2 关键字段

`struct midr_virtual_link_desc`：`ifname`（可空→自动生成 `midr-gre-<n>`）、`vrf_id`、
`outer_local` / `outer_remote` / `outer_link_ifindex`、
`overlay_local` / `overlay_remote` / `overlay_prefix_len`、`mtu`、`flags`（`MIDR_VLINK_F_REBIND`）。

`struct midr_virtual_link_status`：`state`、**`event`**、`stage`、`ifname`、`vrf_id`、
`ifindex`（见下方 ifindex 规则）、`iftype`、`overlay_local`、`overlay_remote`、
**`overlay_prefix_len`**、**`overlay_ready`**、`last_error`（errno 风格）、`ready_ms`。

本轮新增字段（**C6**）：

| 字段 | 类型 | 含义 / 何时有效 |
|---|---|---|
| `event` | `enum midr_virtual_link_event` | 触发本次回调的迁移；查询路径恒为 `MIDR_VLINK_EV_NONE` |
| `overlay_prefix_len` | `uint8_t` | overlay 本地前缀长度；每次通知均携带（与 `overlay_local` 配对） |
| `overlay_ready` | `bool` | 仅 `state == MIDR_VLINK_READY` 时为 `true`；`ADDRESS_SET` 时刻仍为 `false` |

**`FAILED` 时的 `ifindex` 规则（C7，见契约 §7.3）**：`stage == DEVICE_CONFIRM`
（设备消失）→ `ifindex == 0`；**其他任何失败阶段** → 最后一次已确认的 ifindex。

**`link_id` 不在本 API 内**：由第一组拥有与分配，第三组不读取、不生成、不修改。

---

## 3. 变更清单（自草案冻结以来；**请第一组重点看「对第一组的影响」列**）

| # | 变更 | 原因 / 证据 | 对第一组的影响 |
|---|---|---|---|
| C1 | **新增 EEXIST 守卫**：同 outer endpoint 对（`vrf + outer_local + outer_remote`）若要换一个新设备名创建，`add()` 直接失败 `FAILED / stage=VALIDATE / last_error=EEXIST` | 底层 `midrd-gre` 按 endpoint 对建索引，换名只会**重命名 registry**、内核设备仍是旧名 → 新名字永远探测不到 → 表现为神秘超时。实测中 Case B 就踩了这个坑（修复前 PASS=15/FAIL=29） | **调用约定新增一条**：复用同一 outer endpoint 对之前，必须**先 `del()`**；否则拿到的不是超时而是 `EEXIST`（更早、更明确） |
| C2 | **契约 §5.4 勘误**：原稿断言 `lib_handlers[]` 已包含地址通知处理器，**该断言错误**。实际 `lib_handlers[]`（`lib/zclient.c:4757-4775`）只有 `ZEBRA_INTERFACE_ADD/DELETE/UP/DOWN`，**不含** `ZEBRA_INTERFACE_ADDRESS_ADD/DELETE` | 评审发现 + 复核确认；不修则 midrd 接口视图永无 overlay 地址 → `CONFIGURING` 永不 READY | 无直接接口影响；但说明「READY 的判定确实基于真实地址可见性」，非近似 |
| C3 | **删除 4 个 test-only seam**：`midr_virtual_link_test_set_ifindex` / `_set_ready_probe` / `_fail_stage` / `_set_timeout_ms` 及其静态状态与 `MIDR_VLINK_TEST_MAX` | 组件测试实际**一个都没用**（grep 引用数 0）→ 属未受保护的死 API 面。删除后套件 EXIT=0 / 0 warning，双容器回归 PASS=45 FAIL=0（行为不变） | **调用方不能依赖这些符号**（本来也不应）。生产可见 API 仅 §2 所列，外加两个可 wrap 的确认接缝 `midr_virtual_link_probe_device` / `midr_virtual_link_overlay_reachable` |
| C4 | **底层缺陷修复**（`zebra/zapi_msg.c` 的 `zebra_midr_ipaddr_to_prefix()` 删除 `apply_mask()`） | 原实现把 overlay 地址主机位清零，内核装成 `192.168.100.0/30`（应为 `.1/30`），导致精确匹配恒失败 → READY 超时回滚 | 无接口影响；修复后 `READY` 语义才真正成立（否则第一组永远等不到 READY） |
| C5 | `overlay_remote` 必须落在 `overlay_local/overlay_prefix_len` 内，否则 `EINVAL` | 任务文档 §5.1 注的第二种地址规划（两地址不在同一前缀）本版**显式不支持** | **地址规划约束**：第一组分配的 overlay 地址必须同前缀（如 `.1/30` ↔ `.2/30`） |
| C6 | **新增对外可观察面（追加字段，不改签名）**：`enum midr_virtual_link_event` + `struct midr_virtual_link_status` 的 `event` / `overlay_prefix_len` / `overlay_ready`；成功路径固定三条有序通知 `DEVICE_UP -> ADDRESS_SET -> READY`；查询路径 `get_state()`/`wait_ready()` **不发通知**且 `event == MIDR_VLINK_EV_NONE` | 第一组需要从通知里直接拿到 overlay 三件套（否则要额外查询，且无法区分「地址已配置」与「接口已 UP」）；`ADDRESS_SET` 单列出来避免把 READY 语义折叠。overlay 字段**刻意不放在 `midr_gre_status`**：`midr-gre` 无 overlay 概念；设备可已 UP 而地址未配，若放 `overlay_ready` 会制造假就绪；契约 §2 同时冻结 `midr-gre` 语义。备选方案（vlink 层镜像/回填到 GRE 层）本契约不采纳；**若第一组要求，必须先由契约显式写明字段/时机/责任方再改实现** | **源码兼容**：回调签名 `midr_vlink_notify_cb` 不变，新信息经 `status->event` 等传递。第一组可在 `READY` 通知里直接读取 `overlay_local`/`overlay_remote`/`overlay_prefix_len` 后调 `midr_session_connect()`，无需额外查询；**必须按 `event` 分支，不能只看 `state`** |
| C7 | **语义修正 D1-D4**：(D1) **自主检测**：订阅 GRE 层通知 + 模块级 reconcile 定时器（**1000 ms**，只要存在任一非 `DOWN` 条目（含 `READY`）就保持 armed），设备消失、或 `READY` 后丢失 overlay 地址/`IFF_UP` 时**自主**置 `FAILED` 并发通知，**无需调用方轮询**；(D2) `FAILED` 的 `ifindex`：`stage=DEVICE_CONFIRM`（设备消失）→ 0，**其他任何失败阶段** → 最后一次已确认的 ifindex；(D3) 重建（`MIDR_VLINK_F_REBIND` / `FAILED` 重试 / 空 ifname 复用同一 outer endpoint 对）先**彻底拆除**（overlay 地址 `_unset` + GRE 设备删除 + 定时器取消）再建；`VALIDATE/EEXIST` 只留给「GRE 层仍为该 endpoint 对持有 **LIVE** 设备且名字不同/为自动生成名」的外来隧道；(D4) 进入 `FAILED` 时用 `midr_gre_interface_get_state()` 对账 `gre_created`/`addr_set`，避免过期视图 | 静态评审 D1-D4（T1 报告）。D1 前设备丢失只能靠调用方轮询 `get_state()` 发现，且 `READY` 后 per-entry poll 被取消 → 失败可能永远不被感知；D2 前 `FAILED` 恒为 `ifindex==0`，第一组无法定位设备；D3 前空名/rebind 分支只释放条目、不撤地址不删设备 → 新地址叠加在旧地址上（静默覆盖）；D4 前惰性对账留下过期 `gre_created`/`addr_set` | (D1) **行为变更**：第一组现在会**异步收到**自主 `FAILED`（设备丢失，或 `READY` 后地址/`IFF_UP` 丢失），不再需要轮询；但**必须注册 `midr_virtual_link_register_notify()`** 才能收到；(D2) `FAILED` 时 `ifindex` **不总是 0**：只有设备消失类为 0，配置类失败是最后确认的 ifindex；(D3) **收窄 C1**：本模块自己旧条目的 endpoint 对复用改为自动先拆后建，`EEXIST` 仅剩外来 LIVE 隧道；第一组不再需要为「同 endpoint 对换名」先手动 `del()`（显式 `del()` 仍是干净做法）；(D4) 无直接接口影响，保证后续 `add()`/`del()` 设备 create/delete 计数准确 |

> C1–C5 对应的实测轮结果（**W3 之前一轮**；该轮值仅作历史对照，当前权威见 §1.1）：组件套件 **27 PASS / 0 FAIL**、双容器 GRE **45 PASS / 0 FAIL**、
> 跨 BGP-only underlay **34 PASS / 0 FAIL**，合计 **106 PASS / 0 FAIL**。
>
> **C6/C7 对应 W3 扩展后的实测计数（更正，2026-09-29）**：W3 当轮已实测 = 组件套件 27 / 双容器 GRE 51 / stage E 34，
> **合计 112 PASS / 0 FAIL**（`midr-virtual-link-third-group-verification.md` §0）；
> 最新权威实测 = `midr-virtual-link-runtime-evidence.md`（**PASS=179 / FAIL=3**，3 个 FAIL 已归因，与 overlay 无关）。
> 组件用例扩到 18 项（新增 13-18，见契约 §9.2）后**已重跑**；原「待实测 / not measured」表述**已作废**。
> 纪律不变：未复跑、未归因的项**不得写作通过**；本条的实测口径以 §1.1 的权威指针为准。

---

## 4. 请第一组回答的 9 个问题（在「第一组回答」列填写）

| # | 待确认内容 | 第三组当前实现（事实） | 第一组回答 |
|---|---|---|---|
| Q1 | §2 的 5 个函数名/签名与 `struct midr_virtual_link_desc/status` 字段是否可接受？ | 见 `midrd/midr-virtual-link.h`（本包 §2 摘要） | |
| Q2 | 是否同意「**只有 `READY` 才代表设备+overlay 地址+接口 UP 齐备**，`DEVICE_UP` 不可用于建邻」？ | 状态机强制：只有 `READY` 满足三者齐备 | |
| Q3 | 是否同意「`add()` 返回 `0` **不等于就绪**（异步），必须用 `wait_ready()` 或通知回调取最终结论」？ | 与既有 `midr_gre_interface_*` 语义一致；`add` 返回 0 时 state 可能是 `CREATING` | |
| Q4 | 是否同意失败语义：`FAILED` 时 `get_state()` 返回 `0` + `state=FAILED`（而非 `-1`/`DOWN`），并用 `stage`+`last_error` 定位？ | 已实现；失败不回退为「未注册」 | |
| Q5 | 是否同意同 `ifname` 换不同 desc 默认 **`EEXIST` 拒绝**，需 `MIDR_VLINK_F_REBIND` 才显式重建（先 `del` 后 `add`）？ | 契约 §7.2；默认拒绝；`MIDR_VLINK_F_REBIND` / `FAILED` 重试时**先彻底拆除再建**（D3/C7） | |
| Q6 | 是否同意**新增约定**：同 outer endpoint 对换新设备名 → `EEXIST`，**第一组必须先 `del()` 再复用该 endpoint 对**？（见变更 C1） | 本轮 D3 收窄：本模块自己的旧条目改为**自动先拆后建**，`EEXIST` 仅剩**外来 LIVE 隧道**（C7） | |
| Q7 | 是否接受 v1 约束：`overlay_remote` 必须落在 `overlay_local/prefix_len` 内，否则 `EINVAL` 拒绝？若需要「两地址不在同一前缀」的规划，请提出（需新增到对端路由的能力） | 契约 §5.3/§5.5；本版显式不支持并 fail-fast | |
| Q8 | 是否确认 **`READY` 不代表 MIDR Session 一定走 GRE**？（`midr_session_connect()` 不接收本地 ifindex/源地址，出站按 midrd 监听配置 bind —— 任务文档 §8）该风险由**第一、第二组**联合处理，第三组只交付设备/overlay 数据面 | 契约 §11 第 3 条；第三组不宣称 Session 走 overlay | |
| Q9 | 是否确认以 `midr-virtual-link-api.md` 为**唯一准一来源**，并由第一组回签（「照此批准」或修改清单）？ | 回签后第三组在单次提交内同步文档+代码 | |

### 4.1 需要第一组书面明确的「调用约定」（本轮由 C6/C7 扩展）

1. **必须注册通知回调** `midr_virtual_link_register_notify()`：设备丢失、或 `READY` 后丢失
   overlay 地址/`IFF_UP` 时，模块会**自主**发出 `FAILED`，不再依赖调用方轮询（D1 / C7）。
2. **必须按 `status->event` 分支**（不能只看 `state`）：在 `MIDR_VLINK_EV_READY` 且
   `overlay_ready == true` 时，直接读取通知里的 `overlay_local` / `overlay_remote` /
   `overlay_prefix_len`，据此再调 `midr_session_connect()`（C6）。
3. **`FAILED` 时 `ifindex` 的语义**：设备消失类（`stage=DEVICE_CONFIRM`）为 `0`；
   其他配置类失败为**最后一次已确认的 ifindex**（D2 / C7）。
4. **复用 outer endpoint 对前建议先 `del()`**（Q6 / 变更 C1）。本轮 D3 收窄后：
   本模块自己的旧条目会被**自动先彻底拆除再重建**，`EEXIST` 只留给「GRE 层仍持有该
   endpoint 对的 LIVE 设备且名字不同/自动生成」的**外来隧道**；显式 `del()` 仍是干净做法。
5. **overlay 地址必须同前缀**，且 `overlay_remote` 由第一组保证可达性规划（Q7 / 变更 C5）。

---

## 5. 期望的调用序列（供第一组对照编码）

**主推写法：异步通知回调。** `READY` 通知本身就携带 overlay 三件套，
第一组应在回调里直接读取，再调用 `midr_session_connect()`：

```c
/* ---- 第一组侧：启动时注册一次 ---- */
static void on_vlink(const struct midr_virtual_link_status *st, void *arg)
{
        struct midr_context *ctx = arg;
        struct midr_session_endpoint peer = {};
        struct ipaddr ov_local, ov_remote;
        uint8_t ov_pfx;
        ifindex_t local_ifindex;

        switch (st->event) {                 /* 必须按 event 分支 */
        case MIDR_VLINK_EV_DEVICE_UP:
                /* zebra 已确认设备；st->ifindex 有效，但**不是** READY */
                break;

        case MIDR_VLINK_EV_ADDRESS_SET:
                /* overlay local/prefix 已在设备上可见；
                 * overlay_ready == false，IFF_UP 可能尚未置位 —— 还不能建邻 */
                break;

        case MIDR_VLINK_EV_READY:
                /* 只有这里才继续；此处 overlay_ready == true。
                 * 直接从 READY 通知取 overlay 三件套，**无需再调 get_state()**： */
                if (!st->overlay_ready)
                        break;

                ov_local       = st->overlay_local;   /* 本端 overlay 地址   */
                ov_remote      = st->overlay_remote;  /* 对端 overlay 地址   */
                ov_pfx         = st->overlay_prefix_len; /* 前缀长度（C6 新增） */
                local_ifindex  = st->ifindex;         /* 本端虚接口 ifindex  */

                /* 第一组记录 (ov_local, ov_pfx, local_ifindex) 作为交给第二组的
                 * overlay 下一跳 + prefix（跨组提示见契约 §11 第 5 条：目前
                 * midr-topology.h 的 struct midr_link_update 无 prefix 长度字段）。 */

                peer.address = ov_remote;   /* 用 overlay 对端地址建 Session */
                /* peer.port / peer.scope_id 由第一组按自身监听配置填写 */
                midr_session_connect(ctx, &peer);   /* 第三组不调用本函数 */
                break;

        case MIDR_VLINK_EV_FAILED:
                /* 阶段失败**或**自主检测到的丢失（设备消失 / 地址或 IFF_UP 丢失）。
                 * st->stage / st->last_error / st->ifindex 的语义见 §4.1 第 3 条 */
                break;

        case MIDR_VLINK_EV_DOWN:
        case MIDR_VLINK_EV_NONE:
                break;
        }
}

/* ---- 下发请求 ---- */
midr_virtual_link_register_notify(on_vlink, ctx);   /* 否则收不到自主 FAILED */

struct midr_virtual_link_desc d = {};
struct midr_virtual_link_status st;
const char *name;

d.vrf_id = VRF_DEFAULT;
/* ifname 可留空 -> 自动生成 "midr-gre-<n>"，实际名字从 st.ifname 取回 */
strlcpy(d.ifname, "midr-gre-1", sizeof(d.ifname));

SET_IPADDR_V4(&d.outer_local);   inet_pton(AF_INET, "10.1.1.11", &d.outer_local.ipaddr_v4);
SET_IPADDR_V4(&d.outer_remote);  inet_pton(AF_INET, "10.1.1.12", &d.outer_remote.ipaddr_v4);

SET_IPADDR_V4(&d.overlay_local); inet_pton(AF_INET, "192.168.100.1", &d.overlay_local.ipaddr_v4);
SET_IPADDR_V4(&d.overlay_remote);inet_pton(AF_INET, "192.168.100.2", &d.overlay_remote.ipaddr_v4);
d.overlay_prefix_len = 30;   /* overlay_remote 必须落在该前缀内 */
d.mtu = 1400;

if (midr_virtual_link_add(ctx, &d, &st) != 0) {
        /* 返回 0 只表示“已受理”，不等于就绪。
         * st.state == FAILED；st.stage / st.last_error 指出失败点。常见：
         *   VALIDATE/EINVAL(缺 endpoint 或前缀非法)、
         *   VALIDATE/EEXIST(同 ifname 绑定冲突或外来 LIVE 隧道)、
         *   GRE_CREATE/ENOTCONN(zebra 未就绪) 等 */
        return -1;
}

name = d.ifname[0] ? d.ifname : st.ifname;   /* ifname 留空时用 st.ifname */
```

**同步等价写法**（必须在 midrd 主线程）：`midr_virtual_link_wait_ready(ctx, name,
5000, &st)` 返回 `true` 时 `st.state == READY` 且 `st.overlay_ready == true`；
查询路径**不携带事件**，`st.event == MIDR_VLINK_EV_NONE`。此时同样从 `st` 取
`overlay_local` / `overlay_remote` / `overlay_prefix_len` 后调用 `midr_session_connect()`。

**删除**：先由第一组处理自身逻辑邻接状态，再 `midr_virtual_link_del(ctx, name, NULL)`；
本轮 D3 收窄后，本模块自己的旧条目在重建时会自动先彻底拆除，`EEXIST` 只留给外来 LIVE
隧道（见 §3 C7）。

---

## 6. 边界分工（与任务文档 §7 一致）

| 第一组负责 | 第三组负责 |
|---|---|
| 决定哪两个 MIDR 节点建 overlay | 创建/删除 GRE 或 ip6gre 设备 |
| 提供稳定的 outer endpoint 与 **同前缀** overlay 地址规划 | 配置 overlay 地址（经 ZAPI，非 shell） |
| **注册通知回调**，按 `status->event` 分支 | 发出 `DEVICE_UP/ADDRESS_SET/READY/FAILED/DOWN` 通知 |
| 在 `READY`（`overlay_ready == true`）通知里取 overlay 三件套，再调 `midr_session_connect()` | 设置接口状态、提供虚接口 ifindex |
| （建议）复用 outer endpoint 对前先 `del()`（C1；D3 已自动先拆后建） | **自主**检测 Zebra 重连与内核设备消失，不需调用方轮询 |
| 等待 HELLO/Established 后向第二组上报 Link（含 overlay nexthop + prefix，见契约 §11 第 5 条） | 在 `FAILED` 时按 D2 规则填写 `ifindex` |

第三组**不**：判断物理/虚链路、选对端、分配 `link_id`、调用 `midr_session_connect()` /
`midr_topology_link_upsert()` / `_withdraw()`、执行 SPF。

---

## 7. 会签表

| 项 | 内容 |
|---|---|
| 会签对象 | `doc/midr-doc/dp-doc/midr-virtual-link-api.md`（行数见 §7.1）+ 本包 §3 变更清单（C1–C7）+ §4 九问 |
| 第三组（数据面） | 接口已冻结。**W3 实测：组件套件 27 PASS / 0 FAIL、GRE 51 PASS / 0 FAIL、stage E 34 PASS / 0 FAIL（合计 112 PASS），另 ZAPI 编号门禁 PASS**；**可核查摘要见 `midr-virtual-link-runtime-evidence.md`（本轮最新独立实测 PASS=179 / FAIL=3，3 个 FAIL 已归因、与 overlay 无关；W3 的 112 见 `midr-virtual-link-third-group-verification.md` §0）** —— 签名：__________ 日期：__________ |
| 第一组（控制面/调用方） | 结论：☐ 照此批准　☐ 需修改（见下） —— 签名：__________ 日期：__________ |
| 修改清单（若需修改） | 1. ____________________　2. ____________________　3. ____________________ |
| 备注 | 本轮新增字段为**追加式**，回调签名不变，调用方源码兼容；若第一组要求把 overlay 字段镜像到 GRE 层，需先在本契约显式写明（见 C6）。 |

### 7.1 文档行数（供回签核对）

| 文件 | 行数 | 测量命令 |
|---|---|---|
| `doc/midr-doc/dp-doc/midr-virtual-link-api.md` | **651**（2026-09-29 出包实测） | `wc -l`（口径：以回签当场实测为准） |
| `doc/midr-doc/dp-doc/midr-virtual-link-api-countersign.md`（本包） | **328**（2026-09-29 出包实测） | `wc -l`（口径：以回签当场实测为准） |

---

## 8. 参考与证据

| 类型 | 位置 |
|---|---|
| 接口契约（准一来源） | `doc/midr-doc/dp-doc/midr-virtual-link-api.md` |
| 任务说明 | `doc/midr-doc/dp-doc/midr-virtual-link-third-group-implementation.md` |
| 验证报告（实测） | `doc/midr-doc/dp-doc/midr-virtual-link-third-group-verification.md` |
| 权威签名（代码） | `midrd/midr-virtual-link.h` |
| 底层设备 API（被复用，语义未改） | `midrd/midr-gre.{c,h}` |
| 新增 ZAPI | `lib/zclient.{c,h}`（`ZEBRA_INTERFACE_ADDRESS_SET/_UNSET/SET_ADMIN_UP`，追加在 enum 末尾）、`zebra/zapi_msg.c` |
| 实测证据日志 | 组件套件、双容器 GRE、stage E、vtysh 与缺陷 1 根因的原始日志（本轮与上一轮的运行记录，均在本组测试机上，未随仓库分发） |
| 编号门禁 | `midr-test/check-zapi-numbering.sh`（基线 `1adb4c92d0`，PASS） |
| W3 轮新增面（历史轮；当前权威实测见 `midr-virtual-link-runtime-evidence.md`） | 契约 §3.1（event/overlay 字段）、§7.2/§7.3（D2/D3）、§8.2（D1/D4）、§9.2 用例 13-19；会签 §3 的 C6/C7。**W3 实测：组件套件 27 PASS / 0 FAIL、GRE 51 PASS / 0 FAIL、stage E 34 PASS / 0 FAIL（合计 112 PASS，W3 当轮值），另编号门禁 PASS**；|
| 跨组提示 | 契约 §11 第 5 条：`midrd/midr-topology.h` 的 `struct midr_link_update` 无 prefix-length 字段（本轮 grep：`midrd/midr-topology.{h,c}` 无 `prefix_len`/`prefixlen`/`overlay`），第一组→第二组的 overlay nexthop+prefix 交接可能需在第一组/第二组侧补字段 |
