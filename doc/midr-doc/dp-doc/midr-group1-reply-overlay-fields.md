# 第三组对第一组「overlay 字段与通知」诉求的回应稿

> 收件人：第一组（控制面 / 调用方）
> 发件人：第三组（数据面 / OVERLAY 组）
> 日期：2026-09-29
> 基线：分支 `feat/midr-three-way-integration`，HEAD `027bde5bc8`
> 依据（唯一准一来源）：`doc/midr-doc/dp-doc/midr-virtual-link-api.md`（P0 契约冻结，651 行）
> ＋ `doc/midr-doc/dp-doc/midr-virtual-link-api-countersign.md`（会签包，C1–C7）
> ＋ 本轮三份结论：`midr-virtual-link-requirement-conformance.md`（需求符合性）、
>   `midr-virtual-link-runtime-evidence.md`（实测：PASS=179 / FAIL=3，3 项 FAIL 已归因）、
>   `midr-gre-overlay-interface-design.md`（接口归属与层次裁决）
> ＋ 需求 / 职责边界：`midr-virtual-link-third-group-implementation.md`（§7、§8）
> ＋ 权威签名 `midrd/midr-virtual-link.h`

---

## §0 结论

**你要的四个字段已经存在，只是不在 `midr_gre_status`。** 它们在
`struct midr_virtual_link_status`（`midrd/midr-virtual-link.h:145-159`）里，
实际字段名是 `overlay_local` / `overlay_remote` / `overlay_prefix_len` / `overlay_ready`
（注意第三个数带下划线：`overlay_prefix_len`）。

- 你**不需要**给 GRE 层加任何字段：把调用面从 `midr_gre_interface_add()/del()` 换成
  `midr_virtual_link_add()/del()`，再注册 `midr_virtual_link_register_notify()`，
  成功路径上的三条有序通知 `DEVICE_UP -> ADDRESS_SET -> READY` 会把 overlay 三件套直接送达。
- 你诉求背后的真实目标——把 overlay 信息**可靠地**传给第二组——我们完全同意，但
  **卡点不在 GRE 层**：真正的缺口是「第一组 → 第二组」的交接口 `struct midr_link_update`
  （`midrd/midr-topology.h:54-63`）没有前缀长度字段，见 §4。
- 你具体要做的三件事，见 §1。

---

## §1 你需要做的三件事

三件事都发生在你自己这一侧：第三组不需要你改任何结构体，也不需要你等我们发版。

| # | 做什么 | 为什么 | 去哪看 |
| --- | --- | --- | --- |
| 1 | 调用面换成 `midr_virtual_link_add()` / `midr_virtual_link_del()`（查询用 `get_state()`，同步等待用 `wait_ready()`）；**不要**直接调 `midr_gre_interface_add()/del()` | GRE 层 API 只管建 / 删 netdevice，不配 overlay 地址、不置 `ADMIN_UP`，因此给不出 `READY` 语义，也拿不到 overlay 值 | CP 门面：`midrd/midr-virtual-link.h:176-202`；对比底层：`midrd/midr-gre.h:164-174`；代码见 §6.2 |
| 2 | 启动时注册一次 `midr_virtual_link_register_notify(cb, arg)`；回调里**按 `st->event` 分支**，不能只看 `st->state` | 成功路径固定发三条**独立且有序**的通知 `DEVICE_UP -> ADDRESS_SET -> READY`；`FAILED` 有两种来源（阶段失败 / READY 后自主检测到丢失），只有 `event` 能区分 | `midrd/midr-virtual-link.h:105-112`（事件枚举）、`:161-162`（回调签名）、`:212`（注册入口）、`:96-104`（三条通知语义）；代码见 §6.3 |
| 3 | 在 `READY` 回调里取 overlay 三件套（`overlay_local` / `overlay_remote` / `overlay_prefix_len`）并自行落库；同时请你们与第二组共同决定 `struct midr_link_update` 是否补前缀长度字段（谁产生、谁消费、兼容性如何定，由你们两边定） | 只有 `READY` 时的 overlay 值才是权威值（此时 `overlay_ready == true`）；而跨组结构目前没有前缀长度，第二组只能靠隐含约定推断前缀 | 取值：`midrd/midr-virtual-link.h:145-159`、`:141-143`；缺口见 §4；待确认见 §7 的 Q4 |

> **我们主动加的一条必须项**：`READY` 之后若设备消失、或 overlay 地址 / `IFF_UP` 丢失，
> **第三组会自主发 `FAILED`**（1000 ms reconcile 定时器，不需要你轮询）——但前提是
> **你必须先注册回调**，否则收不到。该行为写进了会签条款 C7（会签包 §3「变更清单」C7），
> 并已列为调用约定第 1 条（会签包 §4.1）。

---

## §2 你的四条诉求逐条回应

先把你的原话拆成 4 条，逐条给现状、结论、依据。结论一句话：**第 4 条已经就绪、第 1 条不用新增、
第 3 条请改调用面，而第 2 条你的目标正确、只是卡点找偏了。**

| # | 你的诉求（原文） | 现状（可核查的事实） | 结论 | 依据 file:line |
| --- | --- | --- | --- | --- |
| 1 | 「需要在 `midr_gre_status` 里面增加几个字段：`overlay_local` / `overlay_remote` / `overlay_prefixlen` / `overlay_ready`」 | 四个字段**已存在**，位于 `struct midr_virtual_link_status`：`overlay_local`、`overlay_remote`、`overlay_prefix_len`、`overlay_ready`；`struct midr_gre_status` 保持冻结、无 overlay 字段 | **不新增**。请改用 `midr_virtual_link_status`。另请注意实际字段名是 `overlay_prefix_len`（下划线），不是 `overlay_prefixlen` | `midrd/midr-virtual-link.h:145-159`（逐字段：`:153` / `:154` / `:155` / `:156`）；`midrd/midr-gre.h:129-137`；理由见本稿 §3 |
| 2 | 「这个给 GRE 隧道配好的 IP 需要由第一组之后传给第二组」 | 从 `READY` 通知里能**完整拿到** overlay 三件套（local / remote / prefix_len）；但承载「第一组→第二组」交接口的 `struct midr_link_update` 只有 `link_local_address` / `link_remote_address`，**没有**前缀长度字段（本轮复核：`midrd/midr-topology.{h,c}` 内 grep 不到 `prefix_len` / `prefixlen` / `overlay`） | **目标正确，卡点找偏了**：卡点不在 GRE 层，在这个跨组结构。详见本稿 §4 | `midrd/midr-topology.h:54-63`（字段 `:57` / `:58`）；契约 `midr-virtual-link-api.md:643-651`（§11 第 5 条）；会签包 §8「跨组提示」 |
| 3 | 「再配合上已经有的 `midr_gre_interface_add()` 和 `del()`」 | `midr_gre_interface_add/del` 是**底层设备 API**（只管建 / 删 GRE netdevice，不管 overlay 地址、不管 `ADMIN_UP`）；CP 门面是 `midr_virtual_link_add/del/get_state/wait_ready`。需求文档 §7 明确「第三组不应……要求第一组通过 shell 配置设备」，契约 §1 也把「不要求调用方用 shell 或 raw netlink」列为硬约束 | **请改调 vlink 层**。若经 GRE 层直接建隧道，会绕过 overlay 地址配置与 `ADMIN_UP`，也拿不到 `READY` 语义 | `midrd/midr-gre.h:164-174`（底层 add/del）；`midrd/midr-virtual-link.h:176-202`（CP 门面）；`midrd/midr-virtual-link.h:8-20`、`:46-48`（wire path）；`midr-virtual-link-third-group-implementation.md:309-316`；契约 `midr-virtual-link-api.md:35-42` |
| 4 | 「在地址配置完或者链路就绪等等事件的时候都触发 notify 回调我们小组的代码」 | **已经就绪**：`enum midr_virtual_link_event`（`NONE` / `DEVICE_UP` / `ADDRESS_SET` / `READY` / `FAILED` / `DOWN`），成功建立路径固定三条**独立且有序**的通知 `DEVICE_UP -> ADDRESS_SET -> READY`；注册入口 `midr_virtual_link_register_notify()`；回调签名 `midr_vlink_notify_cb` 本轮未变，事件随 `status` 传递 | **无需改动**。注册回调 + 按 `event` 分支即可；`ADDRESS_SET` 恰好就是你想要的「地址配置完」时刻 | `midrd/midr-virtual-link.h:105-112`（事件枚举）、`:161-162`（回调签名）、`:204-213`（注册 / 注销）、`:96-104`（三条有序通知语义）；会签包 §2.1.1 |

一句话补充：条款 C7 还给第 4 条**加了一条必须项**——`READY` 之后设备消失或 overlay 地址 / `IFF_UP`
丢失时，第三组**自主**发 `FAILED`（1000 ms reconcile 定时器，无需轮询），前提是你**已注册回调**
（见 §1 的提示框；代码见 §6.3 的 `MIDR_VLINK_EV_FAILED` 分支）。

---

## §3 为什么 overlay 字段刻意不放 `midr_gre_status`

这一条不是「我们不想做」，而是**放进去会制造一个比现在更糟的 API**。三层理由。

### 3.1 「假就绪」陷阱（最核心）

`midr-gre` 只负责设备本身。一个 GRE 设备可以已经是 `MIDR_GRE_STATE_UP`
（zebra 确认了 ifindex），而 overlay 地址**一个字都还没配**。此时如果
`midr_gre_status` 里有一个 `overlay_ready`：

- 调用方只要看到 `state == UP`，很容易顺手读 `overlay_ready`，而它在「设备已 UP」这个
  `status` 里天然会被填成某种默认值——这正是**假就绪**的来源；
- 更糟的是「设备 UP」这一层无法区分「地址已配置」和「地址未配置」，字段本身
  没有可靠的数据源来填。

头文件原文（`midrd/midr-virtual-link.h:135-144`）：

> `NOTE: overlay_local/overlay_remote/overlay_prefix_len/overlay_ready live`
> `here and deliberately NOT in struct midr_gre_status.  midr-gre owns no`
> `overlay concept - a GRE device can be UP while the overlay address is not`
> `configured yet, so an overlay_ready flag there would be a false-ready trap;`
> `midr-virtual-link-api.md §2 also freezes midr-gre semantics ("do not`
> `change").  midr_virtual_link_status is the authoritative CP-facing surface,`
> `and the overlay values travel with every notification through the unchanged`
> `callback signature below.`

同一论证在 `midr_gre_status` 的定义处也成立：该结构只有
`state` / `ifname` / `vrf_id` / `ifindex` / `iftype` / `err` / `up_ms`
（`midrd/midr-gre.h:129-137`），注释明确 `ifindex` / `iftype` 只在
`state == MIDR_GRE_STATE_UP` 时有效（`:126`）——它表达的就是「设备层」状态。

### 3.2 这是**已会签的条款**，不是第三组单方面决定

会签包变更清单 **C6** 的原文：

> overlay 字段**刻意不放在 `midr_gre_status`**：`midr-gre` 无 overlay 概念；设备可已 UP
> 而地址未配，若放 `overlay_ready` 会制造假就绪；契约 §2 同时冻结 `midr-gre` 语义。
> 备选方案（vlink 层镜像 / 回填到 GRE 层）本契约不采纳；**若第一组要求，必须先由契约显式
> 写明字段 / 时机 / 责任方再改实现**

相关条款还有：C5（`overlay_remote` 必须同前缀）、C7（自主检测与 `ifindex` 语义）。
会签表备注也写明了同一门槛（会签包 §7）。

### 3.3 维护成本：会造成「两处状态源」

若把 overlay 信息回填进 GRE 层，`midr-gre` 必须写它本不拥有的概念，于是
「overlay 是否就绪」同时存在于 `midr-gre.c` 与 `midr-virtual-link.c` 两个模块的内存里。
两份状态一旦不同步（例如 `_unset` 地址成功、GRE 侧回填失败），调用方读到哪一份是对的？
契约 §2 把 `midrd/midr-gre.{c,h}` 标为「**不修改语义**」（契约 `midr-virtual-link-api.md:59`），
就是为了避免这个局面。

### 3.4 重复一遍：**你不需要这个字段，就能拿到全部信息**

`READY` 通知里的 `st.overlay_local` / `st.overlay_remote` / `st.overlay_prefix_len` /
`st.overlay_ready` 是同一份数据的**权威来源**，随每次通知一起到达（`:141-143`）。
换句话说，加字段的收益 ≈ 0，风险 > 0。

---

## §4 真正的缺口：第一组 → 第二组的 overlay nexthop + prefix 交接

这是我们同意你判断「信息要传下去」的那一半，也是唯一需要动结构的地方——但**不在 GRE 层**。

### 4.1 事实

```c
/* midrd/midr-topology.h:54-63 */
struct midr_link_update {
        struct midr_link_key key;
        ifindex_t local_ifindex;
        struct ipaddr link_local_address;   /* :57 */
        struct ipaddr link_remote_address;  /* :58 */
        struct midr_link_metrics metrics;
        enum midr_policy_state policy_state;
        uint64_t policy_tags;
        uint64_t version;
};
```

- 有 `link_local_address` / `link_remote_address`（命名上对应 overlay 本端 / 对端，
  具体语义由第一组与第二组确认），**但没有前缀长度**；
- 本轮复核：`midrd/midr-topology.{h,c}` 内 **grep 不到** `prefix_len` / `prefixlen` / `overlay`；
- 同一发现已记录在契约 §11 第 5 条（`midr-virtual-link-api.md:643-651`）与会签包 §8「跨组提示」。

### 4.2 为什么这是缺口，而不是「第一组自己想办法」

第二组在消费该结构做拓扑 / 路由时，需要知道「去对端 overlay 地址走哪个虚接口、该地址在什么前缀里」。
没有前缀长度，只能靠 `link_local_address` + 隐含约定推断前缀——一旦第一组改用
非 /30 规划（例如 /31、/29），第二组就可能算错。前缀长度**看起来应该显式传递**
（是否如此、由谁补齐，请第一组 + 第二组共同定）。

### 4.3 建议字段（**仅供参考，需要第一组 + 第二组共同定稿**）

```c
/* 建议：追加式，带 has_ 标志，保证旧发布 / 旧消费方按 0 处理 */
struct midr_link_update {
        ... /* 现有字段不动 */
        bool has_overlay;                /* false => 下面三个字段无效 */
        struct ipaddr overlay_local;     /* 若复用 link_local_address 亦可，需二选一并写进契约 */
        struct ipaddr overlay_remote;    /* 即 overlay 下一跳 */
        uint8_t overlay_prefix_len;      /* 与 overlay_local 配对 */
        ifindex_t overlay_ifindex;       /* 可选：本端虚接口（来自 st->ifindex） */
};
```

### 4.4 责任方与兼容性

| 事项 | 我们的建议 | 说明 |
| --- | --- | --- |
| 字段定义 | **第一组与第二组联合定稿**（谁产生、谁消费） | 这是第一组↔第二组的接口，不在第三组设备 API 范围内（契约 `midr-virtual-link-api.md:650-651`） |
| 谁填值 | 第一组（它从 `READY` 通知 / 状态里拿到三件套；`midr-virtual-link-third-group-implementation.md:287-296` 已把「向第二组上报 Link」划给第一组） | 第三组只负责**如实交付** `overlay_prefix_len` 到第一组 |
| 兼容性 | 追加字段 + `has_overlay` 标志；沿用现有 `version` 字段做版本语义 | 旧消费方按 `has_overlay == false` 忽略 |
| 第三组边界 | 第三组**不改** `midr-topology.{h,c}`、不调用 `midr_topology_link_upsert()` | `midrd/midr-virtual-link.h:37-39`；契约 `midr-virtual-link-api.md:40`；`midr-virtual-link-third-group-implementation.md:314` |

> 如果第一组 / 第二组决定不新增字段，另一个选择是**约定前缀长度**（例如固定 /30）并写进
> 你们的接口文档。第三组对此不发表技术判断，只提醒：任何隐含约定都要写下来，否则
> 地址规划一改就会静默出错。

---

## §5 如果你坚持要改 GRE 层：正式流程与代价

我们不排斥讨论，但这条路的**顺序不能反**：契约先改、会签先过，再动代码。原因是
`midr_gre_status` 是已经实现、已被 `midrd-gre-tool` / 组件测试消费的结构（`midrd/midr-gre.h:145-148`
的 `midr_gre_notify_cb`、`:191-192`、`:201-204`），改它属于**契约变更**，不是追加一个私有字段。

### 5.1 正式流程（4 步，缺一不可）

| 步骤 | 内容 | 谁负责 / 谁签字 |
| --- | --- | --- |
| 1 | 第一组**书面**提出：要哪几个字段、在什么时机填入、由谁写、写入失败时如何表达 | 第一组提出（可在本稿 §7 回签表里写，或另出修改清单） |
| 2 | **改契约**：在 `doc/midr-doc/dp-doc/midr-virtual-link-api.md` 显式写明字段 / 时机 / 责任方（要求原文见会签包 C6），并同步修订 `midr-virtual-link-api-countersign.md` 的 C6 | 第三组草拟，第 3 步会签确认 |
| 3 | **会签**：第一组签字（「照此批准」或修改清单）+ 第三组签字 | 第一组 + 第三组 |
| 4 | 在**同一次提交**内同时改文档与代码（会签包明确要求「单次提交内同步文档 + 代码」），并补契约 §9.2 用例、重跑组件套件 + 编号门禁 | 第三组执行；实测计数以独立实测报告 `midr-virtual-link-runtime-evidence.md` 为准 |

### 5.2 代价（请一并权衡）

| 维度 | 影响 |
| --- | --- |
| 签名 / 结构 | 必须**追加式**改 `struct midr_gre_status`（不改 `midr_gre_notify_cb` 签名），否则调用方源码不兼容 |
| 语义 | 违反契约 §2「`midrd/midr-gre.{c,h}` 不修改语义」（`midr-virtual-link-api.md:59`）——需要显式豁免并说明豁免范围 |
| 数据源 | `midr-gre` 不拥有 overlay 数据 → 必须由 vlink 层回填，引入跨模块写回；回填失败 / 时序错位即出现 §3.3 的双状态源问题 |
| 时机 | 必须定义清楚：`ADDRESS_SET` 时写不写 `overlay_ready`？（vlink 层此时显式为 `false`，`:100`）若 GRE 侧写成 `true` 就直接制造假就绪 |
| 时机（续） | `FAILED` 后 overlay 字段如何失效？（参照 D2 的 `ifindex` 规则，会签包 C7） |
| 测试 | 契约 §9.2 用例 13-18（`midr-virtual-link-api.md:599-604`）需扩用例覆盖「GRE 层镜像字段与 vlink 层一致」，并重跑；会签 C6/C7 对应面的实测证据以 `midr-virtual-link-runtime-evidence.md` 为准 |
| 收益 | **≈ 0**：`READY` 通知已携带全部四个值 |

### 5.3 我们建议的前提

若第一组仍要推进，请在第 1 步里**同时回答**：这个镜像字段**解决了什么现有方案解决不了的问题**？
如果答案是「少一次注册 / 少一次查询」，那么 §5.4 有零改动的替代做法，成本更低。

### 5.4 零改动的替代做法（若你只是想省一次查询 / 省一次注册）

| 你的顾虑 | 零改动做法 | 依据 |
| --- | --- | --- |
| 「取 overlay 还要再查一次，麻烦」 | **不用查**。`READY` 通知本身就携带 overlay 三件套，回调里直接读 `st->overlay_local` / `st->overlay_remote` / `st->overlay_prefix_len` | `midrd/midr-virtual-link.h:141-143`、`:153-155`；会签 C6 |
| 「我有同步逻辑，不想写回调状态机」 | 用阻塞式 `midr_virtual_link_wait_ready(ctx, name, 5000, &st)`；返回 `true` 时从 `st` 取三件套后调 `midr_session_connect()`（见 §6.4） | `midrd/midr-virtual-link.h:195-202`；会签包 §5 |
| 「我只想拿到地址，不关心事件」 | 用 `midr_virtual_link_get_state(ctx, name, &st)` 非阻塞查询；注意它**不发通知**，`st.event` 恒为 `NONE` | `midrd/midr-virtual-link.h:187-193` |
| 「收不到自主 `FAILED` 怎么办」 | **没有替代方案**：必须 `midr_virtual_link_register_notify()`。这是会签条款 C7 的硬前提，我们无法用查询代替 | 会签 C7；会签包 §4.1 |
| 「我能不能不注册回调、只轮询 `get_state()`」 | 技术可行但**不推荐**：`READY` 后的 per-entry 轮询已被取消，设备丢失的感知延迟取决于你的轮询周期，且 1000 ms reconcile 是默认节奏 | 会签 C7 |

---

## §6 你照着就能写的调用序列

以下伪代码全部基于**真实签名**（`midrd/midr-virtual-link.h` + `midrd/midr-session.h`），
可直接对照编码。第三组不替第一组决定邻接策略，也不调用 `midr_session_connect()`
（`midrd/midr-virtual-link.h:37-39`）。

### 6.1 启动时注册一次（否则收不到自主 `FAILED`）

```c
#include "midrd/midr-virtual-link.h"

/* 回调签名：void (*)(const struct midr_virtual_link_status *st, void *arg)
 * 见 midrd/midr-virtual-link.h:161-162；签名本轮未变。 */
static void on_vlink(const struct midr_virtual_link_status *st, void *arg);

midr_virtual_link_register_notify(on_vlink, ctx);  /* :212 */
/* 退出时（可选）：midr_virtual_link_unregister_notify(on_vlink);  :213 */
```

> 模块本身的 `init/fini` 由 midrd 负责（`midrd/midrd.c:3488` 已调用
> `midr_virtual_link_init()`，`:3175` 调用 `_fini()`）；第一组不需要自己调 init。

### 6.2 下发链路：`add()` 之后，用通知取最终结论

```c
struct midr_virtual_link_desc d = {};
struct midr_virtual_link_status st;
const char *name;

d.vrf_id = VRF_DEFAULT;                       /* 或你侧的 VRF */
strlcpy(d.ifname, "midr-gre-1", sizeof(d.ifname));  /* 可留空 -> 自动 midr-gre-<n> */

SET_IPADDR_V4(&d.outer_local);   inet_pton(AF_INET, "10.1.1.11", &d.outer_local.ipaddr_v4);
SET_IPADDR_V4(&d.outer_remote);  inet_pton(AF_INET, "10.1.1.12", &d.outer_remote.ipaddr_v4);

/* overlay 地址规划由第一组负责（midr-virtual-link-third-group-implementation.md:287-296）；
 * 约束：overlay_remote 必须落在 overlay_local/prefix_len 内，否则 VALIDATE/EINVAL。
 * 例：local .1/30 <-> remote .2/30。（会签 C5） */
SET_IPADDR_V4(&d.overlay_local);  inet_pton(AF_INET, "192.168.100.1", &d.overlay_local.ipaddr_v4);
SET_IPADDR_V4(&d.overlay_remote); inet_pton(AF_INET, "192.168.100.2", &d.overlay_remote.ipaddr_v4);
d.overlay_prefix_len = 30;

if (midr_virtual_link_add(ctx, &d, &st) != 0) {   /* :176-178 */
        /* 返回非 0 => 已失败：st.state == FAILED，
         * 用 st.stage（VALIDATE/GRE_CREATE/DEVICE_CONFIRM/ADDRESS_SET/
         * ADMIN_UP/REACHABILITY）与 st.last_error 定位（:86-94 / :157-158）。 */
        return -1;
}
/* 返回 0 只表示「已受理」（CREATING/DEVICE_UP/CONFIGURING/已 READY），
 * 不等于就绪 —— 最终结论必须来自通知回调或 wait_ready()。
 * 见 midrd/midr-virtual-link.h:170-175。 */
name = d.ifname[0] ? d.ifname : st.ifname;
```

### 6.3 回调里按 `event` 分支（**不能只看 `state`**）

```c
static void on_vlink(const struct midr_virtual_link_status *st, void *arg)
{
        struct midr_context *ctx = arg;
        struct midr_session_endpoint peer = {};
        struct ipaddr  ov_local, ov_remote;
        uint8_t        ov_pfx;
        ifindex_t      ov_ifindex;

        switch (st->event) {                      /* :105-112 */

        case MIDR_VLINK_EV_DEVICE_UP:
                /* zebra 已确认设备，st->ifindex 有效（:151）；
                 * 但 overlay 地址尚未配置 —— 不要在这里建邻。 */
                break;

        case MIDR_VLINK_EV_ADDRESS_SET:
                /* 这就是「地址配置完」时刻：overlay local/prefix 已在设备上可见，
                 * 但 overlay_ready == false、IFF_UP 尚未要求置位（:101-103 / :156）。
                 * 若你只是想尽早记录 overlay 信息，可在此处读 st->overlay_*。 */
                break;

        case MIDR_VLINK_EV_READY:
                /* 只有这里才继续。此处 overlay_ready == true（:156）。 */
                if (!st->overlay_ready)
                        break;

                ov_local   = st->overlay_local;      /* :153 */
                ov_remote  = st->overlay_remote;     /* :154 */
                ov_pfx     = st->overlay_prefix_len; /* :155 */
                ov_ifindex = st->ifindex;            /* :151 */

                /* (1) 记录 (ov_local, ov_pfx, ov_ifindex) 作为交给第二组的
                 *     overlay 下一跳 + prefix —— 注意 midr_link_update 目前
                 *     没有 prefix 字段，见本稿 §4 / §7 的 Q4。
                 * (2) 建 Session：第三组不调用 midr_session_connect()。 */
                peer.address = ov_remote;
                /* peer.port / peer.scope_id 由第一组按自身监听配置填写
                 * （midrd/midr-session.h:11-15） */
                midr_session_connect(ctx, &peer);    /* midrd/midr-session.h:49-50 */
                break;

        case MIDR_VLINK_EV_FAILED:
                /* 两种来源：某阶段失败，**或** 自主检测到的丢失
                 * （设备消失 / READY 后丢 overlay 地址或 IFF_UP）。
                 * ifindex 语义（D2）：stage == DEVICE_CONFIRM -> 0；
                 * 其他阶段 -> 最后一次已确认的 ifindex。
                 * 见 :148 / :151 与会签 C7。 */
                break;

        case MIDR_VLINK_EV_DOWN:
        case MIDR_VLINK_EV_NONE:
        default:
                break;
        }
}
```

> `st` 只在回调期间有效（`:209`），需要长期保存请自行拷贝 `overlay_local` / `overlay_remote` /
> `overlay_prefix_len` / `ifindex` 这几个标量。

### 6.4 若你不想写回调：同步等价写法

```c
struct midr_virtual_link_status st;
const char *name = "midr-gre-1";

/* 阻塞等待 READY/FAILED/超时；必须在 midrd 主线程调用（:195-202）。 */
if (midr_virtual_link_wait_ready(ctx, name, 5000, &st)) {
        /* true => st.state == READY 且 st.overlay_ready == true */
        struct midr_session_endpoint peer = {};
        peer.address = st.overlay_remote;      /* 三件套同样在 st 里 */
        /* 记录 st.overlay_local / st.overlay_prefix_len / st.ifindex */
        midr_session_connect(ctx, &peer);
}
/* 注意：查询路径 get_state()/wait_ready() 不发通知，
 * 且 st.event 恒为 MIDR_VLINK_EV_NONE
 * （midrd/midr-virtual-link.h:97-99、:190；会签包 §5）。 */
```

非阻塞查询（不触发通知）：`midr_virtual_link_get_state(ctx, name, &st)`（`:192-193`）。

### 6.5 删除流程

```c
/* 先处理第一组自己的逻辑邻接状态（midr-virtual-link-third-group-implementation.md:250），
 * 再删设备侧。 */
midr_virtual_link_del(ctx, name, &st);          /* :184-185，成功时 st.state == DOWN */
```

- 复用同一 outer endpoint 对时，**显式 `del()` 仍是干净做法**；本轮已收窄：
  本模块自己的旧条目会**自动先彻底拆除再重建**，`EEXIST` 只留给「GRE 层仍持有该
  endpoint 对的 LIVE 设备且名字不同 / 为自动生成名」的外来隧道（会签 C1 / C7）。
- 同 ifname 换新 desc 默认 `EEXIST` 拒绝；确需重建请带 `MIDR_VLINK_F_REBIND`（`:115`）。

### 6.6 错误分支速查

| 现象 | 判据 | 你该做什么 |
| --- | --- | --- |
| `add()` 返回 -1 | `st.state == FAILED` | 读 `st.stage` + `st.last_error` 定位 |
| overlay 地址规划非法 | `stage == VALIDATE` / `EINVAL` | 检查 `overlay_remote` 是否在同一前缀内（C5） |
| 同 ifname 绑定冲突 / 外来 LIVE 隧道 | `stage == VALIDATE` / `EEXIST` | 先 `del()`，或加 `MIDR_VLINK_F_REBIND` |
| zebra 未就绪 | `stage == GRE_CREATE` / `ENOTCONN` | 稍后重试 |
| 一直等不到设备 | `stage == DEVICE_CONFIRM` / `ETIMEDOUT` | 检查 zebra/ZAPI 连接 |
| 运行中设备或地址丢失 | 回调 `EV_FAILED`（**自主**） | 你必须**已注册回调**才收得到；按 D2 规则读 `ifindex` |

---

## §7 需要第一组 / 第二组确认的清单

为了让你们**直接回签**，每条都给了选项。请在第一组列填 `同意` / `改为：____`。

| # | 待确认内容 | 我们的现实现 / 事实 | 依据 | 第一组回答 |
| --- | --- | --- | --- | --- |
| **Q1** | **overlay 地址规划的权威方是第一组？** | 需求文档把「提供稳定的 outer endpoint 和 overlay 地址规划」划给第一组；第三组只按 `desc` 配置地址，不做规划 | `midr-virtual-link-third-group-implementation.md:287-296`；会签包 §6 | |
| **Q2** | **接受「`overlay_remote` 必须与 `overlay_local` 同前缀，否则 `VALIDATE/EINVAL` 拒绝」？** 选项：☐ 接受 ☐ 需要「两地址不在同一前缀」的规划（须另行提出路由能力） | v1 显式 fail-fast；例：`.1/30 ↔ .2/30` 通过 | 会签 C5；契约 `midr-virtual-link-api.md:636-637`；`midrd/midr-virtual-link.h:86-94`（`STAGE_VALIDATE`） | |
| **Q3** | **确认「`READY` 不等于 MIDR Session 必然走 GRE」？** 并确定**第一组 + 第二组**联合验证下列 5 项 | 第三组只交付设备 + overlay 数据面，不宣称 Session 承载路径：`midr_session_connect()` 不接收本地 ifindex / 源地址，出站按 midrd 监听配置 bind | 契约 §11 第 3 条 `midr-virtual-link-api.md:638-641`；`midr-virtual-link-third-group-implementation.md:318-332`；`midrd/midr-virtual-link.h:37-39` | |
| **Q3-a** | midrd 监听地址是否**覆盖** overlay 地址？ | 需第一组 + 第二组确认 | `midr-virtual-link-third-group-implementation.md:325` | |
| **Q3-b** | 出站连接**源地址** bind 是否正确（是否会从 underlay 地址出去）？ | 同上 | 同上 `:326` | |
| **Q3-c** | `route get overlay_remote` 是否显示**走 GRE 设备**？ | 需实测确认（第三组可提供设备 / 地址就绪证据，选路归 Session 侧） | 同上 `:327` | |
| **Q3-d** | 物理 Link 与 overlay Link **并存**时能否选到正确 endpoint？ | 同上 | 同上 `:328` | |
| **Q3-e** | 对端 HELLO 的**身份准入**是否正确（不会把物理邻居认成 overlay 邻居）？ | 同上 | 同上 `:329` | |
| **Q4** | **`struct midr_link_update` 补 prefix / overlay 字段：责任方与兼容性谁定？** 选项：☐ 第一组 + 第二组定稿后通知第三组 ☐ 由第三组提建议 diff（但第三组不落代码） | 现状无字段；建议见本稿 §4.3 / §4.4 | `midrd/midr-topology.h:54-63`；契约 `midr-virtual-link-api.md:643-651`；会签包 §8 | |
| **Q5** | **确认 overlay 信息从 `midr_virtual_link_status`（而非 `midr_gre_status`）获取，无需给 GRE 层加字段？** 选项：☐ 同意（本稿 §2 / §3） ☐ 仍需 GRE 层镜像 → 走本稿 §5 的 4 步流程 | 四个字段已在 vlink status；GRE 层保持冻结 | `midrd/midr-virtual-link.h:145-159`、`:135-144`；会签 C6 | |
| **Q6** | **确认调用面用 `midr_virtual_link_add/del/get_state/wait_ready`，而不是 `midr_gre_interface_add/del`？** | GRE 层 API 不配置 overlay 地址、不置 `ADMIN_UP`，无法给出 `READY` | `midrd/midr-gre.h:164-174`；`midrd/midr-virtual-link.h:176-202`；`midr-virtual-link-third-group-implementation.md:309-316` | |
| **Q7** | **确认「必须注册 `midr_virtual_link_register_notify()` 才能收到自主 `FAILED`」**（无法用查询替代）？ | 1000 ms reconcile；设备消失或 `READY` 后丢地址 / `IFF_UP` 时自主发 `FAILED` | 会签 C7；会签包 §4.1 | |
| **Q8** | **确认按 `status->event` 分支，而不是只看 `state`？** 特别是 `ADDRESS_SET`（`overlay_ready == false`）不得用于建邻 | 成功路径固定三条有序通知 | `midrd/midr-virtual-link.h:96-104`；会签包 §4.1 | |
| **Q9** | **确认以 `midr-virtual-link-api.md` 为唯一准一来源，并回签会签包**（「照此批准」或修改清单）？ | 回签后第三组在单次提交内同步文档 + 代码 | 会签包 §4 回签表、§7 | |
| **Q10** | **`FAILED` 时 `ifindex` 语义是否接受**（设备消失类 = 0；其他配置类 = 最后一次已确认的 ifindex）？ | 已实现 D2 规则 | 会签 C7 | |

> 关于**实测状态**：会签 C6 / C7 对应面（事件枚举 + overlay 字段 + 自主检测）的实测证据，
> 当前权威来源是 `midr-virtual-link-runtime-evidence.md`（PASS=179 / FAIL=3，3 项 FAIL 已归因）；
> 更早轮次出现过的计数（如 106 / 112）只作历史对照，不再是当前权威。
> 本稿只陈述接口与语义，不引用未产生的结论。

---

## §8 证据索引与自查命令

> 本稿所有 `file:line` 以基线 HEAD `027bde5bc8` 为准。会签包仍在修订中、行号可能变动，
> 因此本稿对会签包一律用**条款号（C1–C7）与节号**引用，不用行号。

### 8.1 代码（权威签名）

| 文件:行号 | 内容 |
| --- | --- |
| `midrd/midr-virtual-link.h:105-112` | `enum midr_virtual_link_event`（`NONE` / `DEVICE_UP` / `ADDRESS_SET` / `READY` / `FAILED` / `DOWN`） |
| `midrd/midr-virtual-link.h:96-104` | 三条有序通知与 `ADDRESS_SET` 的语义注释 |
| `midrd/midr-virtual-link.h:135-144` | 「假就绪」注释：overlay 字段刻意不放 `midr_gre_status` |
| `midrd/midr-virtual-link.h:145-159` | `struct midr_virtual_link_status`：`event` `:147`、`ifindex` `:151`、`overlay_local` `:153`、`overlay_remote` `:154`、`overlay_prefix_len` `:155`、`overlay_ready` `:156`、`last_error` `:157` |
| `midrd/midr-virtual-link.h:161-162` | 回调签名 `midr_vlink_notify_cb`（本轮未变） |
| `midrd/midr-virtual-link.h:176-202` | CP 门面：`add` `:176-178` / `del` `:184-185` / `get_state` `:192-193` / `wait_ready` `:200-202` |
| `midrd/midr-virtual-link.h:212-213` | `midr_virtual_link_register_notify()` / `_unregister_notify()` |
| `midrd/midr-virtual-link.h:117-133` | `struct midr_virtual_link_desc`（`overlay_local` `:127` / `overlay_remote` `:128` / `overlay_prefix_len` `:129`） |
| `midrd/midr-virtual-link.h:76-94`、`:115`、`:242-244` | state / stage 枚举、`MIDR_VLINK_F_REBIND`、`overlay_reachable`（v1 不做主动探测） |
| `midrd/midr-virtual-link.h:8-20`、`:37-39`、`:46-48` | 模块职责与 wire path（第三组不调 session / topology） |
| `midrd/midr-gre.h:129-137` | `struct midr_gre_status`（**无 overlay 字段**） |
| `midrd/midr-gre.h:145-148`、`:164-174`、`:191-204` | `midr_gre_notify_cb`、底层 `midr_gre_interface_add/del`、`get_state` / `wait_up` |
| `midrd/midr-topology.h:54-63` | `struct midr_link_update`（`link_local_address` `:57` / `link_remote_address` `:58`，**无 prefix 字段**） |
| `midrd/midr-session.h:11-15`、`:49-53` | `struct midr_session_endpoint`、`midr_session_connect/disconnect` |
| `midrd/midrd.c:3488`、`:3175` | `midr_virtual_link_init()` / `_fini()` 生产接线；工具侧示例 `midrd/gre-link-tool.c:307`、`:379` |

### 8.2 文档（契约、会签、需求、本轮结论）

| 文件:行号 / 节 | 内容 |
| --- | --- |
| `midr-virtual-link-api.md:35-42` | 第三组不做清单（含「不要求调用方用 shell / raw netlink」） |
| 同上 `:59` | `midrd/midr-gre.{c,h}` **不修改语义** |
| 同上 `:599-604` | §9.2 用例 13-18（事件 / overlay / D1-D4 面）标注待实测 |
| 同上 `:632-641` | §11 第 1-3 条：追加字段且回调签名不变；`overlay_remote` 跨前缀被拒；**`READY` 不保证 Session 走 GRE** |
| 同上 `:643-651` | §11 第 5 条：**跨组提示——`midr_link_update` 无 prefix 长度字段** |
| `midr-virtual-link-api-countersign.md` §2.1.1 | 成功路径固定 `DEVICE_UP -> ADDRESS_SET -> READY` |
| 同上 §3 变更清单 | **C5** `overlay_remote` 必须同前缀；**C6** overlay 字段刻意不放 `midr_gre_status`（若第一组要求，须先改契约再改实现）；**C7** 自主检测 / `ifindex` 语义 / 先拆后建 / 对账 |
| 同上 §4.1、§4 回签表、§6、§7、§8 | 需要第一组书面明确的调用约定 5 条；回签；边界分工表；会签表备注；跨组提示 |
| `midr-virtual-link-third-group-implementation.md:250`、`:282-283` | 删除时不得调第一组 Session / Link API；不得静默覆盖；不得把 `DEVICE_UP` 当 `READY` |
| 同上 `:285-316`、`:318-332` | §7 第一组 / 第三组职责边界与「第三组不应」清单；§8 Session 实际选路检查项（5 项由第一、第二组联合确认） |
| `midr-virtual-link-runtime-evidence.md` | 当前权威实测（PASS=179 / FAIL=3，3 项 FAIL 已归因） |
| `midr-virtual-link-requirement-conformance.md` | 需求符合性（静态审计） |
| `midr-gre-overlay-interface-design.md` | 本轮接口归属与层次裁决；本稿结论与其一致 |

### 8.3 本稿的核查方式（供你们自行复核）

以下命令在仓库根目录执行即可逐条验证本稿的每个论断：

```bash
# 1) 四个字段确实在 vlink status，不在 gre status
sed -n '129,159p' midrd/midr-virtual-link.h
sed -n '129,137p' midrd/midr-gre.h

# 2) 「假就绪」注释原文
sed -n '135,144p' midrd/midr-virtual-link.h

# 3) 事件枚举与注册入口
sed -n '105,112p;204,213p' midrd/midr-virtual-link.h

# 4) 真正缺口：topology 无 prefix/overlay 字段（应为空输出）
grep -n 'prefix_len\|prefixlen\|overlay' midrd/midr-topology.h

# 5) CP 门面 vs 底层 GRE API（别调错层）
sed -n '176,202p' midrd/midr-virtual-link.h
sed -n '164,174p' midrd/midr-gre.h
```

> 备注：本稿的技术结论与 `doc/midr-doc/dp-doc/midr-gre-overlay-interface-design.md`
> （本轮 GRE / overlay 接口归属与层次裁决）一致。若两者出现出入，以该裁决文件与
> `midr-virtual-link-api.md` 为准，并请告知第三组修订本稿。

