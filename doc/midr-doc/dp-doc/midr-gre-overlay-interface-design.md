# MIDR GRE/overlay 接口归属与第一组诉求裁决（第三组接口设计）

> 作者：第三组（数据面）接口设计员
> 审计/复核基线：仓库 `/Users/yangmengyu/githubdocuments/frr`，`git rev-parse HEAD` = `027bde5bc8ad0a6ac4d8afb3b2286014a6641ce1`。
> 准一来源（需求）：`doc/midr-doc/dp-doc/midr-virtual-link-third-group-implementation.md`（尤其 §4、§5.1、§7）
> 已冻结契约：`doc/midr-doc/dp-doc/midr-virtual-link-api.md`（651 行）+ 会签包 `doc/midr-doc/dp-doc/midr-virtual-link-api-countersign.md`
> 权威签名（代码）：`midrd/midr-virtual-link.h`；底层设备 API：`midrd/midr-gre.h`
> 方法：只读文档 + 读代码 + grep，全部结论带 `文件:行号`。本文不改任何代码，不动他人文档。
> 纪律：严格区分「代码里真的有」/「文档声称有」/「需要跨组决策」。

---

## §1 结论

**overlay 字段归属裁决（一句话）**：`overlay_local` / `overlay_remote` / `overlay_prefix_len` / `overlay_ready` 四件套**只归属 `struct midr_virtual_link_status`**（`midrd/midr-virtual-link.h:145-159`），**绝不进入 `struct midr_gre_status`**；第一组通过 `midr_virtual_link_register_notify()` 注册**虚链路层**回调，在 `MIDR_VLINK_EV_READY` 通知里即可拿到全部四件套，无需任何新增字段或额外查询。

**是否需要改代码**：**否 —— 无代码变更（no code change）**。
- 第三组接口面已完备：第一组建邻所需数据（overlay local/remote/prefix、ifindex、就绪信号、就绪后的丢失信号）在现有 `midr_virtual_link_status` 与三条有序通知里全部可取（证据见 §2、§3、§6）。
- 唯一的真实跨组缺口在第一组 ↔ 第二组之间（`struct midr_link_update` 无 prefix/overlay 语义），**不属于第三组设备 API 范围**（需求文档 §7 边界），第三组不越界，只给建议（§5）。
- 若第一组坚持把 overlay 字段加进 `midr_gre_status`：按已冻结契约 **C6**（`midr-virtual-link-api-countersign.md:118`）与契约 §3.1（`midr-virtual-link-api.md:173-182`），**必须先改契约再改实现**，当前契约不采纳——因此「直接改代码」在本轮被明确禁止（§6 列代价）。

---

## §2 第一组四条诉求逐条裁决表

| # | 第一组诉求（原话大意） | 裁决 | 依据 file:line | 第一组该怎么做 | 是否需改契约 |
|---|---|---|---|---|---|
| a | 在 `midr_gre_status` 里增加 `overlay_local` / `overlay_remote` / `overlay_prefixlen` / `overlay_ready` 四个字段 | **拒绝**（不加）。用现有等价面满足：`struct midr_virtual_link_status` 已有全部四字段 | `midrd/midr-gre.h:129-137`（现状确认无 overlay 字段）；`midrd/midr-virtual-link.h:153-156`（四字段已在）；`midrd/midr-virtual-link.h:136-144`（代码内 NOTE 明写刻意不放 GRE 层）；`midr-virtual-link-api-countersign.md:118`（C6）；`midr-virtual-link-api.md:173-182`（契约理由） | 读 `struct midr_virtual_link_status`：`st->overlay_local` / `st->overlay_remote` / `st->overlay_prefix_len` / `st->overlay_ready`，随每次回调传入 | **否**（现有契约已覆盖）。若坚持要改 GRE 层，须**先改 C6 契约**（字段/时机/责任方/失败语义），当前不采纳 |
| b | 「地址配置完或者链路就绪等等事件的时候都触发 notify 回调」 | **已满足**（现有通知时机完整，无需新增）。成功路径固定三条有序通知 `DEVICE_UP -> ADDRESS_SET -> READY`，另含自主 `FAILED` 与 `DOWN` | 触发点见 §3：`midrd/midr-virtual-link.c:642` / `:710-711` / `:732` / `:464` / `:499` / `:973`；通知实现 `:234-251`，状态填充 `:173-215` | 注册**虚链路层**回调：`midr_virtual_link_register_notify(cb, arg)`（`midrd/midr-virtual-link.h:212`），**必须按 `st->event` 分支**，不能只看 `st->state` | **否** |
| c | 「配合上已经有的 `midr_gre_interface_add()` 和 `del()`」 | **澄清 + 纠正**：面向控制面的入口是 `midr_virtual_link_add()` / `midr_virtual_link_del()`，**不是** GRE 层的 `midr_gre_interface_add/del`。GRE 层 API 由虚链路层内部调用 | 面向 CP：`midrd/midr-virtual-link.h:176-185`；GRE 层：`midrd/midr-gre.h:164-185`；虚链路层内部调用证据：`midrd/midr-virtual-link.c:880`（add）、`:446` / `:962`（del/回滚） | 只调 `midr_virtual_link_add/del`。**不得**直接调 `midr_gre_interface_add/del` 绕过 overlay 地址配置与 READY 判定（等价于需求 §7「不得要求第一组通过 shell 配置设备」的风险） | **否**（契约 §6 边界分工已写明 `midr-virtual-link-api-countersign.md:258-270`） |
| d | 「这个 IP 需要由第一组之后传给第二组」 | **认可需求，但落在别处**：属第一组 → 第二组接口，**非第三组责任**。真实缺口是 `struct midr_link_update` 无 prefix/overlay 字段 | `midrd/midr-topology.h:54-63`（`midr_link_update` 只有 `link_local_address`/`link_remote_address`，无 `prefix_len`/`overlay`）；`midr-virtual-link-api.md:643-651`（契约 §11 第 5 条跨组提示）；需求 §7 `midr-virtual-link-third-group-implementation.md:287-316` | 第一组从 `READY` 通知拿到 overlay 三件套后自行落库，并在**第一/第二组之间**补字段（建议见 §5） | **否（对第三组契约）**；需第一/第二组决定其接口是否更新 |

**裁决口径提示**：a/b/c 均在第三组已冻结面内闭合，**第一组不需要第三组改一行代码**；d 是跨组（第一↔第二）事项，第三组只把 `overlay_prefix_len` 如实交付（已交付）。

---

## §3 通知契约（event → 触发点 → 第一组可读到什么 → 推荐动作）

**注册哪一个回调**：注册**虚链路层**回调 `midr_virtual_link_register_notify(cb, arg)`（`midrd/midr-virtual-link.h:212`），回调类型 `midr_vlink_notify_cb(const struct midr_virtual_link_status *st, void *arg)`（`midrd/midr-virtual-link.h:161-162`）。
**不要**注册 GRE 层回调 `midr_gre_register_notify()`（`midrd/midr-gre.h:147`）：该回调由虚链路层在 `midrd/midr-virtual-link.c:1158` 内部订阅（`midr_vlink_gre_notify`，`:546`），只携带 `struct midr_gre_status`（无 overlay 概念，`midrd/midr-gre.h:129-137`），第一组直接用它会把「设备 UP」误当「可以建邻」。

每条通知的状态由 `midr_vlink_notify()`（`midrd/midr-virtual-link.c:234-251`）统一填充：`midr_vlink_fill_status()`（`:173-215`）把 `overlay_local`/`overlay_remote`/`overlay_prefix_len` 从 entry 的 desc 拷入 status，`overlay_ready = (state == READY)`（`:190`），再 `st.event = ev`（`:245`）后逐回调发出。

| event | 触发点 file:line | 触发语义 | 第一组可读到什么 | 推荐动作 |
|---|---|---|---|---|
| `MIDR_VLINK_EV_DEVICE_UP` | `midrd/midr-virtual-link.c:642`（`midr_vlink_advance()` 的 `MIDR_VLINK_CREATING` 分支，`probe_device` 成功后） | zebra 已确认设备、ifindex 有效；**not READY** | `st->ifindex`（有效，`:208-210`）、`st->iftype` 可能已填、overlay 三件套已填但 `st->overlay_ready == false` | 仅记录 ifindex；**不得**据此建邻（头文件 `midrd/midr-virtual-link.h:28-31` 明确 `DEVICE_UP` 不是 READY） |
| `MIDR_VLINK_EV_ADDRESS_SET` | `midrd/midr-virtual-link.c:710-711`（`MIDR_VLINK_CONFIGURING` 分支，`e->addr_seen` 首次为真，`midr_vlink_overlay_addr_present()` 返回真，`:704-711`） | overlay local/prefix 已在设备上可见；**不要求 IFF_UP** | `st->overlay_local` / `st->overlay_remote` / `st->overlay_prefix_len` 正确，`st->overlay_ready == false`（`:190`，因 state 仍是 CONFIGURING） | 可预取 overlay 三件套做规划；**仍不得建邻**（接口未必 UP） |
| `MIDR_VLINK_EV_READY` | `midrd/midr-virtual-link.c:732`（`CONFIGURING` 分支，`midr_virtual_link_overlay_reachable()` 为真，`:723-734`） | 设备 + overlay 地址 + `IFF_UP` 三者齐备 | `st->overlay_ready == true`、`st->overlay_local` / `st->overlay_remote` / `st->overlay_prefix_len`、`st->ifindex`（`:192-194`） | **唯一**允许建邻的时机：读取 overlay 三件套 + ifindex，记录后调用 `midr_session_connect()`（第三组不调用该函数，`midrd/midr-virtual-link.h:37-38`） |
| `MIDR_VLINK_EV_FAILED`（主动失败） | `midr_vlink_fail()`，`midrd/midr-virtual-link.c:456-465`（调用点如 `:647-649` / `:674-676` / `:685-687` / `:715-718` / `:739-741`） | 某阶段失败/被拒/超时 | `st->stage`（失败阶段）、`st->last_error`、`st->ifindex`（D2 规则：`DEVICE_CONFIRM` 失败为 0，其他为最后一次已确认值，`:204-206`） | 按 `st->stage` / `st->last_error` 决定重试或放弃；不要重试已 `EEXIST` 的情形 |
| `MIDR_VLINK_EV_FAILED`（**自主检测**） | `midr_vlink_lost()`，`midrd/midr-virtual-link.c:476-500`；上游订阅点 `midr_vlink_gre_notify()`（`:546` 起）；周期探针 `midr_vlink_reconcile_entry()`（`:508-538`） | 设备消失，或 READY 后丢失 overlay 地址 / `IFF_UP` | 同上；`st->stage == MIDR_VLINK_STAGE_DEVICE_CONFIRM`，`st->ifindex == 0`（`:485-487`） | 收到即视为链路失效：**必须注册回调才能收到**（否则要靠轮询，见 C7/D1，`midr-virtual-link-api-countersign.md:119`） |
| `MIDR_VLINK_EV_DOWN` | `midrd/midr-virtual-link.c:973`（`midr_virtual_link_del()` 路径，`st.event = EV_DOWN`） | 已删除 | `st->state == MIDR_VLINK_DOWN`、`st->event == EV_DOWN` | 清理本地邻接状态（第一组先处理自身逻辑邻接，再 del，契约 `:252`） |
| `MIDR_VLINK_EV_NONE` | 查询路径：`midr_virtual_link_get_state()` / `midr_virtual_link_wait_ready()`（`midrd/midr-virtual-link.h:187-202`；填充时 `memset` 归零，`midrd/midr-virtual-link.c:179-182`） | **查询永不发通知** | `st->event == MIDR_VLINK_EV_NONE`；`overlay_ready` 仍按 state 计算 | 同步等待用 `wait_ready()` 并检查返回 true 且 `st.overlay_ready == true`（契约 `:247-250`） |

**关键区分**：`ADDRESS_SET` 与 `READY` 是**两条独立通知**，`overlay_ready` 只在 `READY` 为真——这正是「地址已配置」与「接口已 UP、可建邻」的分界（契约 C6，`midr-virtual-link-api-countersign.md:118`；测试断言见 `midr-test/virtual-link-test.c:963-992`）。


---

## §4 层次图与职责边界

**正向路径（创建/配置/就绪）**，每条边标注所用函数与 file:line：

```text
第一组 (CP)
  │  midr_virtual_link_add(ctx, desc, status)          midrd/midr-virtual-link.h:176-178
  │  （异步受理；wait_ready / 回调用作最终判定）
  ▼
midrd/midr-virtual-link.c  （唯一 CP 入口，状态机 advance() :624-754）
  │  ① 创建设备：midr_gre_interface_add(ctx, &tun, &gst)      midrd/midr-virtual-link.c:880
  │         └─► midr-gre.c:458  zclient_send_gre_add()  →  ZEBRA_GRE_ADD
  │  ② 配 overlay 地址：zclient_send_interface_address_set()  midrd/midr-virtual-link.c:670-672
  │         └─► lib/zclient.c  →  ZEBRA_INTERFACE_ADDRESS_SET
  │  ③ 接口 UP：zclient_send_interface_admin_up()            midrd/midr-virtual-link.c:681-683
  │         └─► lib/zclient.c  →  ZEBRA_INTERFACE_SET_ADMIN_UP
  │  ④ 确认设备：midr_virtual_link_probe_device()            midrd/midr-virtual-link.c:635-636（声明 midr-virtual-link.h:233-234）
  │  ⑤ 确认 overlay：midr_virtual_link_overlay_reachable()   midrd/midr-virtual-link.c:723-726（声明 midr-virtual-link.h:242-244）
  ▼
midrd/midr-gre.c  （底层设备层）
  │  zclient_send_gre_add() / _delete()                     midrd/midr-gre.c:458 / :551
  │  经 MIDR 数据面后端的同一 zclient 发送（midr-gre.h:44-46）
  ▼
lib/zclient.c  （ZAPI 编码）
  ▼
zebra（zapi_msg.c handler → zebra_dplane.c → if_netlink.c）   zapi_msg.c:4317/4379/4451（契约 §6.3，midr-virtual-link-api.md:423-432）
  ▼
内核 netdevice（GRE/ip6gre + overlay 地址 + IFF_UP）
```

**反向/删除路径**：

```text
midr_virtual_link_del(ctx, ifname, status)                 midrd/midr-virtual-link.h:184-185
  │  ① 撤地址：zclient_send_interface_address_unset()       midrd/midr-virtual-link.c:956
  │  ② 删设备：midr_gre_interface_del(NULL, ifname, NULL)   midrd/midr-virtual-link.c:962
  │  ③ 报 DOWN + 清 registry                                midrd/midr-virtual-link.c:973-977
  ▼
（回滚路径同构：midr_vlink_rollback()  midrd/midr-virtual-link.c:419-454
   —— :440 撤地址、:446 删设备）
```

**职责边界（与需求 §7、契约 §6 一致）**：

| 层 | 拥有什么 | 不拥有什么 | 证据 |
|---|---|---|---|
| 第一组（CP） | 决定哪两个节点建 overlay；提供 outer endpoint 与**同前缀** overlay 地址规划；**注册回调**；READY 后调 `midr_session_connect()`；向第二组上报 Link | 不碰内核/设备；不分配第三组设备内部状态 | 需求 `midr-virtual-link-third-group-implementation.md:287-296`；契约 `midr-virtual-link-api-countersign.md:258-270` |
| `midr-virtual-link.{c,h}` | overlay 概念、状态机、READY 语义、通知、自主丢失检测 | 不判断物理/虚链路、不分配 `link_id`、不调 `midr_session_connect()`/`midr_topology_link_upsert()` | `midrd/midr-virtual-link.h:37-39`；`midr-virtual-link.c` 内 grep 无这些调用 |
| `midr-gre.{c,h}` | GRE/ip6gre 设备创建/删除、ifindex/UP 确认（**无 overlay 概念**） | 不配 overlay 地址、不置 admin up（那是 vlink 层经 ZAPI 做的） | `midrd/midr-gre.h:129-137`（status 无 overlay）；`midrd/midr-virtual-link.c:663-689`（vlink 层自己发地址/up） |
| zclient / zebra | ZAPI 编解码、设备与地址下发到内核 | — | 契约 `midr-virtual-link-api.md:423-432` |

**误用风险（对应诉求 c）**：若第一组直接调 `midr_gre_interface_add()`（`midrd/midr-gre.h:164-166`），只会创建设备；overlay 地址与 IFF_UP 都不会下发，也不会产生 `READY`——第一组会拿到一个「设备存在但不可建邻」的假象。因此**面向控制面的正确入口只有 `midr_virtual_link_add/del`**。

---

## §5 跨组缺口：overlay nexthop + prefix 的交接（第一组 → 第二组）

**现状（已复核）**：`struct midr_link_update`（`midrd/midr-topology.h:54-63`）字段为：`key` / `local_ifindex` / `link_local_address` / `link_remote_address` / `metrics` / `policy_state` / `policy_tags` / `version`。
**缺口**：其中**没有** `prefix_len` / `overlay` 字段——`struct midr_link_update`（`midrd/midr-topology.h:54-63`）与 `midr-topology` 相关实现（`midrd/midrd.c:2169-2322`）内均无 `prefix_len`/`overlay`（本轮 grep 复核，与契约 §11 第 5 条 `midr-virtual-link-api.md:643-651` 一致）。
**下游如何被消费**：第一组用 `link_local_address` / `link_remote_address` 传的是**接力两端 locator**（`midrd/group1/midr_nds_facts.c:783-784`），`local_ifindex` 恒为 0（`:780-781`，文档约定 0=不适用）；第二组经 `midr_topology_link_upsert()`（`midrd/midrd.c:2291-2304`）→ `topology_link_event()`（`:2169-2211`）取用。同一条数据还经 local fact / wire TLV 传输（`midrd/midr-wire.c:118/132-133/146/158-159`，`struct midr_core_identity.prefix_len` @ `midrd/midr-core.h:58-68`，`local_address`/`remote_address` @ `:81-82`）。

**建议（供第一组/第二组决策，第三组不落地）**：在第一/第二组自己的接口里**追加**一个 overlay 前缀长度语义，最小改动方案如下（追加式，不改现有成员顺序/语义）：

```c
/* 建议：midrd/midr-topology.h  struct midr_link_update（:54-63）追加 */
+	/* 链路为 overlay（GRE）承载时，link_local_address 的本地前缀长度。
+	 * 0 表示未提供/非 overlay；has_overlay_prefix 用于区分「0」与「未提供」。 */
+	uint8_t overlay_prefix_len;
+	bool has_overlay_prefix;
```

- **责任方**：**第一组**（在 READY 后从 `st` 取 `overlay_prefix_len` 并填入上报）与**第二组**（在其校验/消费侧接收），**不是第三组**（需求 §7 `midr-virtual-link-third-group-implementation.md:309-316`：第三组不调 `midr_topology_link_upsert()`，也不决定 overlay 下一跳语义）。
- **兼容性影响**：`struct midr_link_update` 是进程内 C 结构（`midrd/midrd.c` 内构造/消费），追加字段对现有源码兼容；但**同一语义若也要过 wire**，需同步扩 `struct midr_core_identity` / `struct midr_core_object`（`midrd/midr-core.h:58-83`）与 wire 编解码（`midrd/midr-wire.c:118/132-133/146/158-159`）的 TLV 布局，并升版本——**这是跨组协议变更，须第一/第二/第三组共同确认，不能由第三组单方面改**。
- **替代（零字段）方案**：若不改 wire，可由第一组在**旁路**记录 `(overlay_remote, overlay_prefix_len)` 映射，第二组按 `link_remote_address` 反查；缺点是映射与拓扑两处状态可能不一致，属第一/第二组自行取舍。


---

## §6 代码变更结论

**结论：无代码变更（no code change to `midrd/`、`lib/`、`zebra/`）。**

**为什么不需要改**（代码证据，逐一对应第一组诉求）：

1. **overlay 四件套已可取**：`struct midr_virtual_link_status`（`midrd/midr-virtual-link.h:145-159`）已含 `overlay_local`（:153）、`overlay_remote`（:154）、`overlay_prefix_len`（:155）、`overlay_ready`（:156）。
2. **数据已真正填满**：`midr_vlink_fill_status()`（`midrd/midr-virtual-link.c:173-215`）每次通知都会把这三件套从 desc 拷入，并按 state 计算 `overlay_ready`（:187-190）。
3. **通知已真正发出且有序**：`midr_vlink_notify()`（:234-251）逐回调发送；建立路径三条独立通知 `DEVICE_UP`（:642）→ `ADDRESS_SET`（:710-711）→ `READY`（:732）；失联自主 `FAILED`（:499）；删除 `DOWN`（:973）。
4. **回调签名未变、源码兼容**：`midr_vlink_notify_cb`（`midrd/midr-virtual-link.h:161-162`）与 `midr_virtual_link_register_notify()`（:212）即第一组所需的注册点。
5. **第一组无需轮询**：`READY` 通知与自主 `FAILED` 通知都经回调送达（C7/D1，`midr-virtual-link-api-countersign.md:119`）。
6. **不得绕开 P0 冻结**：把 overlay 字段加进 `midr_gre_status` 直接违反 `midrd/midr-virtual-link.h:136-144` 的 NOTE 与 C6（`midr-virtual-link-api-countersign.md:118`）；契约 §3.1（`midr-virtual-link-api.md:173-182`）明写「备选方案本契约不采纳，除非第一组明确要求，且须先改契约」。

**「若第一组坚持要改」的具体代价（本组不推荐、本轮不执行）**——仅用于让 lead 看清成本：

- 必须**先改契约**：`doc/midr-doc/dp-doc/midr-virtual-link-api.md` §2/§3.1（`midr-virtual-link-api.md:126-182`）与会签包 C6（`midr-virtual-link-api-countersign.md:118`）要显式写明新字段、填充时机、责任方、失败语义（尤其 `overlay_ready` 在 GRE 层的假就绪如何避免）。
- 才改代码：`midrd/midr-gre.h`（`struct midr_gre_status` @ :129-137 追加字段）、`midrd/midr-gre.c`（`midr_gre_notify` @ :299 / :353 / :363 / :581 / :656 的填充点要回填 overlay），并让 vlink 层在每次状态变化时**回写** GRE 层（`midrd/midr-virtual-link.c` 多处）；或新增镜像 API。
- 风险：重新引入「设备 UP 但地址未配」的**假就绪陷阱**；GRE 层本无 overlay 概念（`midrd/midr-gre.h:3-46` 设计声明），语义被污染。
- 测试影响（追加式字段本身一般安全，但语义改动会影响断言）：
  - `midr-test/virtual-link-test.c` 大量按名引用 `overlay_*` 与断言通知顺序（如 `:963-992` 断言 `notify_snap[0..2].overlay_ready` 与 `overlay_prefix_len`），回写/镜像若改变通知内容会波及这些断言。
  - `midr-test/gre-registry-test.c` 断言 GRE 层 registry 行为，若 GRE status 语义被扩展需复核。
  - 本轮 grep：`midr-test/` 内**没有** `sizeof(struct midr_gre_status)` / `sizeof(struct midr_virtual_link_status)` / `offsetof(...)` 依赖（grep 命中为空），因此纯追加字段**不会**因结构体大小/布局变化直接打破现有断言；风险集中在「语义/通知内容」而非「内存布局」。

**对 §5 的建议 diff 同样不落地**：它属于第一/第二组接口（`midrd/midr-topology.h` 等），且若涉及 wire 需跨组协议变更，按 §5 交由 lead 与第一/第二组决策。故本轮**第三组零代码变更**。

---

## §7 开放项（需第一组确认 / 需实测交叉校验）

1. **需第一组书面确认（沿用会签 9 问 + §4.1 五条调用约定）**（`midr-virtual-link-api-countersign.md:144-156`）：
   - 必须**注册** `midr_virtual_link_register_notify()`（否则收不到自主 `FAILED`，:146-147）；
   - 必须**按 `st->event` 分支**、在 `EV_READY` 且 `overlay_ready == true` 时取 overlay 三件套（:148-150）；
   - 明确 `FAILED` 时 `ifindex` 语义（设备消失类为 0，配置类为最后确认值，:151-152）；
   - overlay 必须**同前缀**（C5，:156）；复用 outer endpoint 对前建议先 `del()`（:153-155）。
2. **需第一组确认对本裁决的接受**：`midr_gre_status` **不加** overlay 字段（§2-a、§6）；第一组若不同意，须走「先改契约」流程，由 lead 决定。
3. **需第一/第二组确认 §5 的字段归属与 wire 是否同步**：`struct midr_link_update` 追加 `overlay_prefix_len`/`has_overlay_prefix` 是否可接受、是否要扩 wire（`midrd/midr-core.h:58-83`、`midrd/midr-wire.c:118/132-133/146/158-159`）。**第三组不越界**。
4. **需实测交叉校验（T2 产物已出现）**：`doc/midr-doc/dp-doc/midr-virtual-link-runtime-evidence.md`
   - 本轮汇总 **PASS=179 / FAIL=3**（该文档 `:22`）；其中 **2 个 FAIL 是「以 root 运行组件套件」的环境偏差**（`test_midr_sequence` 的 EACCES 期望只在非特权下成立），**1 个 FAIL 是出厂 manifest 与注册集不一致**（`run-midr-component-suite.sh` 未跑用例即 exit 2，`:20` / `:313-323`）。
   - **结论：3 个 FAIL 均为 manifest / root 环境偏差，无一属于 overlay 逻辑失败**；overlay 关键路径在内核级已被证实（overlay 为 host 地址 `192.168.100.1/30`、`192.168.100.0/30` 命中 0、IFF_UP 存在、拆除后清理干净，`:19`）。
5. **需与 T1 静态符合性审计交叉校验**：`doc/midr-doc/dp-doc/midr-virtual-link-requirement-conformance.md`
   - T1 判定「整体部分符合（≈95%）」，其标记的 5 处「部分符合」含 **§3.4 生产调用方缺失**：`midrd` 进程内只有 `init`（`midrd.c:3488`）/`fini`（`midrd.c:3175`），**没有生产 `add()` 调用方**，创建仍只由 `gre-link-tool.c:331`（独立工具）与组件测试驱动。**这一点与本裁决一致**：第三组接口已就绪，缺的是第一组把「请求 → `midr_virtual_link_add()`」接进守护进程（属第一组调用方工作，非第三组改代码）。
   - T1 另指「overlay 可达性检查为本地前缀推断而非真实探测」——与本裁决不冲突（契约 §5.3 已声明 v1 约束）；若第一组要求真实探测，须按契约 §11 走 Session/路由扩展（`midr-virtual-link-api.md:313-320`），不在本设备 API 范围。
6. **待办交叉引用**：本裁决以 T1（静态符合性）与 T2（运行时证据）为准一事实来源的补充；若两者后续结论与本裁决冲突，**以 lead 的最终裁定为准**，并优先回到契约 C6 的「先改契约再改实现」流程。

