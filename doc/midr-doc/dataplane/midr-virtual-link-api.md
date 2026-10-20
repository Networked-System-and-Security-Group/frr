# MIDR 虚链路设备服务接口契约（第三组，P0 冻结）

> 状态：**P0 契约冻结**（lead 产出），作为 P1/P2/P3 共同的编码依据。
> 基线：分支 `feat/midr-three-way-integration`，HEAD `443c51474b`。
> 上游任务说明：`doc/midr-doc/dp-doc/midr-virtual-link-third-group-implementation.md`。
> 底层既有 API：`midrd/midr-gre.{c,h}`（保留，不改语义）。
>
> **本轮修订（W3，2026-09-29，经 lead 确认实现已冻结）**：在**回调签名不变**（调用方源码兼容）
> 的前提下，新增通知事件枚举 `enum midr_virtual_link_event` 与
> `struct midr_virtual_link_status` 的 `event` / `overlay_prefix_len` / `overlay_ready` 三个字段，
> 并把 §7.2（重建先彻底拆除）、§7.3（FAILED 的 ifindex 规则）、§8.2（自主故障检测）、
> §9.2（新增用例 13-18）按本轮实现重写。逐条变更及其**对第一组（调用方）的影响**见
> 会签包 `midr-virtual-link-api-countersign.md` §3 的 **C6 / C7**。
>
> 术语说明：下文 **D1–D4** 指本轮静态评审提出的缺陷编号（D1 自主检测、D2 FAILED ifindex 规则、
> D3 重建先拆除、D4 FAILED 对账），**区别于** §5.3/§5.4/§6.1 标题里的「关键决策 D1/D2/D3」标签。

## 1. 目的与边界

把「只创建 GRE/ip6gre netdevice」扩展为第一组可直接调用的完整虚链路设备服务：

```text
outer endpoint 可达
  -> 创建 GRE/ip6gre
  -> 配置 overlay 地址
  -> 设置接口 UP
  -> 确认 ifindex 与 overlay 数据面就绪
  -> 返回 READY
  -> 支持删除、故障通知、重建和状态查询
```

**第三组做**：创建/删除设备、配置 overlay 地址、设置接口状态、返回 ifindex、报告
`DEVICE_UP/ADDRESS_SET/READY/FAILED/DOWN`、**自主**检测 Zebra 重连与内核设备消失。

**第三组不做**（硬约束）：

- 不判断物理链路还是虚链路；
- 不选择对端、不分配第一组逻辑 `link_id`；
- 不调用 `midr_session_connect()`；
- 不调用 `midr_topology_link_upsert()` / `midr_topology_link_withdraw()`；
- 不执行 SPF / 不决定 MIDR 拓扑；
- 不要求调用方用 shell 或 raw netlink（含测试脚本）。

## 2. 交付物与改动清单（本契约覆盖）

| 层 | 文件 | 内容 |
|---|---|---|
| P1 | `lib/zclient.h` | 追加 3 条 enum（尾部）；新增 2 个编码结构/3 个发送助手声明 |
| P1 | `lib/zclient.c` | 3 个发送助手 + 线格式编码 |
| P1 | `lib/log.c` | `command_types[]` 尾部追加 3 条 `DESC_ENTRY` |
| P1 | `zebra/zapi_msg.c` | `zserv_handlers[]` 追加 3 个 designated initializer + 3 个 handler |
| P1 | `midr-test/check-zapi-numbering.sh` | 白名单由 2 条扩为 5 条（见 §6.4） |
| P2 | `midrd/midr-virtual-link.{c,h}` | 新增高层虚链路 API（本契约 §3/§4） |
| P2 | `midrd/midrd.c` | init/fini 接线 |
| P2 | `midrd/midr-dp-backend.c` | **MF-1**：注册 `[ZEBRA_INTERFACE_ADDRESS_ADD/DELETE]` handler（见 §5.4 勘误） |
| P2 | `midrd/Makefile`、`Makefile.am` | 源文件与目标登记 |
| P3 | `midr-test/virtual-link-test.c`、`midr-test/midr-gre-connectivity-test.sh` | 测试与脚本改造 |

`midrd/midr-gre.{c,h}` **不修改语义**，仅作为底层设备接口被 P2 复用。


## 3. 数据结构（最终定义）

```c
/* midrd/midr-virtual-link.h */

/* 对外可观察状态。只有 READY 才表示设备、overlay 地址与接口状态都已完成。 */
enum midr_virtual_link_state {
	MIDR_VLINK_DOWN = 0,	/* 未请求，或已删除 */
	MIDR_VLINK_CREATING,	/* GRE 创建请求已发出，等待 ifindex */
	MIDR_VLINK_DEVICE_UP,	/* Zebra 已识别设备，ifindex 有效（≠ READY） */
	MIDR_VLINK_CONFIGURING,	/* overlay 地址/接口状态配置中 */
	MIDR_VLINK_READY,	/* 设备 + 地址 + 接口 UP 均已完成 */
	MIDR_VLINK_FAILED,	/* 任一步骤失败或被拒绝，见 last_error/stage */
};

/* 失败阶段，用于定位“哪一步没过去”。 */
enum midr_virtual_link_stage {
	MIDR_VLINK_STAGE_NONE = 0,
	MIDR_VLINK_STAGE_VALIDATE,
	MIDR_VLINK_STAGE_GRE_CREATE,
	MIDR_VLINK_STAGE_DEVICE_CONFIRM,
	MIDR_VLINK_STAGE_ADDRESS_SET,
	MIDR_VLINK_STAGE_ADMIN_UP,
	MIDR_VLINK_STAGE_REACHABILITY,
};

/* 触发本次通知的迁移。仅查询路径（get_state()/wait_ready()）使用 NONE：
 * 它们永不发通知，且 status->event 恒为 MIDR_VLINK_EV_NONE。
 * 成功建立路径上 DEVICE_UP -> ADDRESS_SET -> READY 是三条独立的、有序的通知。
 * ADDRESS_SET 表示 overlay local/prefix 已在设备的接口视图可见，此时
 * IFF_UP 尚未要求置位（overlay_ready 仍为 false）。 */
enum midr_virtual_link_event {
	MIDR_VLINK_EV_NONE = 0,	   /* 仅查询路径；永不作为通知发出 */
	MIDR_VLINK_EV_DEVICE_UP,   /* zebra 已确认设备 */
	MIDR_VLINK_EV_ADDRESS_SET, /* overlay local/prefix 已在设备上可见 */
	MIDR_VLINK_EV_READY,	   /* 设备 + overlay 地址 + IFF_UP */
	MIDR_VLINK_EV_FAILED,	   /* 某阶段失败，含自主检测到的丢失 */
	MIDR_VLINK_EV_DOWN,	   /* 已删除 */
};

/* desc 标志位。 */
#define MIDR_VLINK_F_REBIND 0x1u /* 允许同 ifname 覆盖为新的 endpoint/overlay */

struct midr_virtual_link_desc {
	char ifname[IFNAMSIZ];	/* 可空：空则自动生成 "midr-gre-<n>" */
	vrf_id_t vrf_id;

	/* GRE 外层 / underlay 端点。 */
	struct ipaddr outer_local;
	struct ipaddr outer_remote;
	ifindex_t outer_link_ifindex; /* 0 => 内核自行解析 */

	/* GRE 内层 / overlay 端点。 */
	struct ipaddr overlay_local;
	struct ipaddr overlay_remote;
	uint8_t overlay_prefix_len;

	uint32_t mtu;	/* 0 => 内核默认 */
	uint32_t flags;	/* MIDR_VLINK_F_* */
};

struct midr_virtual_link_status {
	enum midr_virtual_link_state state;
	enum midr_virtual_link_event event; /* 触发本次通知的迁移 */
	enum midr_virtual_link_stage stage; /* 失败阶段；成功时为 NONE */
	char ifname[IFNAMSIZ];
	vrf_id_t vrf_id;
	ifindex_t ifindex;	/* 有效期规则见下方 §3.1 */
	uint8_t iftype;		/* enum zebra_iftype；READY 时有效 */
	struct ipaddr overlay_local;
	struct ipaddr overlay_remote;
	uint8_t overlay_prefix_len; /* overlay 本地前缀长度 */
	bool overlay_ready;	    /* 仅 state == MIDR_VLINK_READY 时为 true */
	int last_error;		/* errno 风格；state == FAILED 时有效 */
	uint32_t ready_ms;	/* READY 持续时间（毫秒）；0 表示尚未 READY */
};

typedef void (*midr_vlink_notify_cb)(const struct midr_virtual_link_status *st,
				     void *arg);
```

### 3.1 本轮新增字段（通知事件与 overlay 承载）

`event` / `overlay_prefix_len` / `overlay_ready` 为本轮新增，随**未改变**的回调签名
`midr_vlink_notify_cb(const struct midr_virtual_link_status *st, void *arg)` 传递：

| 字段 | 类型 | 含义 / 何时有效 |
|---|---|---|
| `event` | `enum midr_virtual_link_event` | 触发本次回调的迁移；查询路径恒为 `MIDR_VLINK_EV_NONE` |
| `overlay_prefix_len` | `uint8_t` | overlay 本地前缀长度，与 `overlay_local` 配对；每次通知均携带 |
| `overlay_ready` | `bool` | 仅 `state == MIDR_VLINK_READY` 时为 `true`；`ADDRESS_SET` 时刻仍为 `false` |

`enum midr_virtual_link_event` 取值：

| 值 | 含义 |
|---|---|
| `MIDR_VLINK_EV_NONE` (=0) | 仅查询路径返回；**永不**作为通知发出 |
| `MIDR_VLINK_EV_DEVICE_UP` | zebra 已确认设备、ifindex 有效（≠ READY） |
| `MIDR_VLINK_EV_ADDRESS_SET` | overlay local/prefix 已在设备上可见；`overlay_ready == false` |
| `MIDR_VLINK_EV_READY` | 设备 + overlay 地址 + `IFF_UP` 三者齐备；`overlay_ready == true` |
| `MIDR_VLINK_EV_FAILED` | 某阶段失败，含 §8.2 自主检测到的设备/地址丢失 |
| `MIDR_VLINK_EV_DOWN` | 已删除 |

**ifindex 有效规则（收敛 §7.3 与会签变更 C7）**：

- `state == READY`：`ifindex` = 当前有效 ifindex；
- `state == FAILED` 且 `stage == MIDR_VLINK_STAGE_DEVICE_CONFIRM`（设备消失）：`ifindex == 0`；
- `state == FAILED` 且其他 `stage`（`ADDRESS_SET` / `ADMIN_UP` / `REACHABILITY` 等）：
  `ifindex` = **最后一次已确认的 ifindex**（§7.3 保留规则，便于第一组定位设备）；
- 其他状态：`state >= DEVICE_UP` 时有效。

> **为什么 overlay 字段放在 `midr_virtual_link_status` 而不放在 `struct midr_gre_status`**：
> `midr-gre` 不拥有 overlay 概念；一个 GRE 设备可以已经 UP 而 overlay 地址尚未配置，
> 若在 `midr_gre_status` 上放一个 `overlay_ready` 就会制造「假就绪」陷阱（false-ready trap）。
> §2 同时冻结 `midr-gre` 语义（不改），因此 overlay 信息只承载在 `midr_virtual_link_status`
> 上，并随每次通知传递给调用方。
>
> **备选方案（本契约不采纳，除非第一组明确要求）**：由 vlink 层把 overlay 字段
> **镜像/回填**到 GRE 状态层。该方案会重新引入上述假就绪风险并触碰冻结的 `midr-gre` 语义，
> 因此**若第一组要求，必须在本契约中显式写明**（字段、填充时机、责任方、失败语义）之后再改实现，
> 不得绕过 P0 冻结。

**`link_id` 不进 desc**：它由第一组拥有与分配，第三组不得读取、生成或修改。

## 4. 对外 API（最终签名）

```c
/* 创建或更新一条虚链路。成功返回 0；失败返回 -1 且 status->state == FAILED。 */
int midr_virtual_link_add(struct midr_context *ctx,
			  const struct midr_virtual_link_desc *desc,
			  struct midr_virtual_link_status *status);

/* 删除虚链路（按 ifname）。成功返回 0，status->state == DOWN。 */
int midr_virtual_link_del(struct midr_context *ctx, const char *ifname,
			  struct midr_virtual_link_status *status);

/* 非阻塞查询。已知返回 0 并填 status，未知返回 -1。
 * 纯查询：不发通知，且 status->event 恒为 MIDR_VLINK_EV_NONE。 */
int midr_virtual_link_get_state(struct midr_context *ctx, const char *ifname,
				struct midr_virtual_link_status *status);

/* 阻塞等待 READY：泵事件循环直到 READY / FAILED / 超时。
 * 返回 true 当且仅当进入 READY。仅可在 midrd 主线程调用。
 * 纯查询：不发通知，且 status->event 恒为 MIDR_VLINK_EV_NONE。 */
bool midr_virtual_link_wait_ready(struct midr_context *ctx, const char *ifname,
				  uint32_t timeout_ms,
				  struct midr_virtual_link_status *status);

/* 状态变化通知：DEVICE_UP / ADDRESS_SET / READY / FAILED / DOWN 迁移时回调。
 * 触发本次回调的迁移由 status->event 给出（回调签名不变，仍为
 * midr_vlink_notify_cb）。成功建立路径上顺序固定为
 * DEVICE_UP -> ADDRESS_SET -> READY（三条独立通知）。
 * status 仅在回调期间有效。 */
void midr_virtual_link_register_notify(midr_vlink_notify_cb cb, void *arg);
void midr_virtual_link_unregister_notify(midr_vlink_notify_cb cb);

/* 生命周期与工具。 */
const char *midr_virtual_link_state_str(enum midr_virtual_link_state state);
void midr_virtual_link_init(struct event_loop *master);
void midr_virtual_link_fini(void);
```

### 4.1 返回与 errno 语义

| 场景 | 返回 | status |
|---|---|---|
| 请求已受理（PENDING/已存在） | `0` | `CREATING` 或 `READY` |
| 校验失败（见 §4.2） | `-1` | `FAILED`，`stage=VALIDATE`，`last_error=EINVAL`/`EAFNOSUPPORT` |
| 同 ifname 已绑定不同 endpoint/overlay 且未置 `MIDR_VLINK_F_REBIND` | `-1` | `FAILED`，`stage=VALIDATE`，`last_error=EEXIST` |
| 置 `MIDR_VLINK_F_REBIND` / `FAILED` 重试 / 空 ifname 复用已有 outer endpoint 对 | `0`（先彻底拆除再重建，见 §7.2） | `CREATING`（或重建完成前的中间态） |
| 换名复用同一 outer endpoint 对，而 GRE 层仍持有该对的 LIVE 设备（外来隧道） | `-1` | `FAILED`，`stage=VALIDATE`，`last_error=EEXIST` |
| zclient 未就绪 | `-1` | `FAILED`，`stage=GRE_CREATE`，`last_error=ENOTCONN` |
| 任一后续步骤失败/超时 | `-1` | `FAILED`，`stage` 指向失败步骤，`last_error` 为 errno 风格（`EIO`/`ETIMEDOUT`） |
| registry 分配失败 | `-1` | `FAILED`，`stage=GRE_CREATE`，`last_error=ENOMEM` |

**`add` 返回 0 ≠ READY**：与既有 `midr_gre_interface_add()` 一致，须用
`wait_ready()` 或通知回调取得最终结论。**不得把 `DEVICE_UP` 当作 `READY`。**

### 4.2 校验规则（fail fast，全部在 `stage=VALIDATE`）

1. `desc != NULL`；
2. `outer_local` / `outer_remote` 均已设置，地址族相同；
3. `overlay_local` / `overlay_remote` 均已设置，地址族相同；
4. `overlay_prefix_len` 对该族合法（IPv4 1..32，IPv6 1..128）；
5. **`overlay_remote` 必须落在 `overlay_local/overlay_prefix_len` 内**（v1 约束，见 §5.3）；
6. 若 `ifname` 非空且已注册：endpoint 与 overlay 完全一致 → 幂等返回；不一致且无
   `MIDR_VLINK_F_REBIND` → `EEXIST`；不一致且有 rebind → 先按 §7.2 显式重建；
   `FAILED` 同 desc 重试亦按 §7.2 先彻底拆除再重建。空 `ifname` 复用已有 outer
   endpoint 对时不再直接拒绝，而是按 §7.2 先彻底拆除旧链路；仅当 GRE 层仍为该对
   持有 LIVE 设备（外来隧道）时才 `EEXIST`。

outer 与 overlay 地址族**允许不同**（IPv6 overlay over IPv4 GRE 是合法场景），
但 outer 两端必须同族、overlay 两端必须同族。


## 5. 状态机与建立流程

### 5.1 迁移表

| 当前 | 事件 | 迁移到 | 通知（`status->event`） |
|---|---|---|---|
| `DOWN` | `add()` 受理 | `CREATING` | — |
| `CREATING` | Zebra 确认设备、ifindex 有效 | `DEVICE_UP` | `DEVICE_UP` |
| `CREATING` | GRE 创建被拒 / 确认超时 | `FAILED` | `FAILED` |
| `DEVICE_UP` | overlay 地址与 admin-up 请求已下发 | `CONFIGURING` | — |
| `CONFIGURING` | overlay local/prefix 已在本地视图可见（**不要求 `IFF_UP`**） | `CONFIGURING`（状态不变） | `ADDRESS_SET`（`overlay_ready == false`） |
| `CONFIGURING` | 地址可见**且**接口 `IFF_UP` | `READY` | `READY`（`overlay_ready == true`） |
| `CONFIGURING` | 地址下发失败 / 置 UP 失败 / 超时 | `FAILED` | `FAILED` |
| `READY` | `del()` | `DOWN` | `DOWN` |
| `DEVICE_UP`/`CONFIGURING`/`READY` | 设备消失（GRE 通知或 §8.2 对账定时器自主检测） | `FAILED`（ifindex 失效，保留 desc） | `FAILED` |
| `READY` | §8.2 对账发现 overlay 地址或 `IFF_UP` 丢失 | `FAILED`（保留 desc） | `FAILED` |
| `FAILED` | 同 desc 重新 `add()`（幂等重试；先按 §7.2 彻底拆除） | `CREATING` | — |

成功建立路径上对外固定发出**三条独立、有序**的通知：

```text
DEVICE_UP  ->  ADDRESS_SET  ->  READY
```

其中 `ADDRESS_SET` 表示 overlay local/prefix 已在设备上可见，此时 `IFF_UP` 未必置位、
`overlay_ready` 仍为 `false`；只有 `READY` 的 `overlay_ready` 才为 `true`。
`get_state()` / `wait_ready()` 是纯查询：不发通知，且返回的 `status->event` 恒为
`MIDR_VLINK_EV_NONE`。

### 5.2 建立流程（与任务文档 §5.1 一一对应）

```text
1. 校验 outer local/remote 地址族;                 -> VALIDATE
2. 校验 overlay local/remote 地址族和 prefix length; -> VALIDATE
3. 确认 Zebra zclient 可用;                        -> GRE_CREATE
4. 复用 midr_gre_interface_add() 创建设备;          -> GRE_CREATE
5. midr_gre_interface_wait_up() 取得 ifindex;      -> DEVICE_CONFIRM / DEVICE_UP
6. 经 P1 原语下发 overlay local/prefix;            -> ADDRESS_SET（地址可见时通知）
7. 经 P1 原语置接口 administratively UP;           -> ADMIN_UP
8. 确认 overlay 数据面就绪（见 §5.3）;              -> REACHABILITY
9. 报告 READY + ifindex + overlay 地址（event=READY，overlay_ready=true）。
```

通知与步骤不是一一对应：步骤 5 确认后立即发 `DEVICE_UP`；步骤 6 的地址在 midrd 接口视图
可见时发 `ADDRESS_SET`（此刻 `IFF_UP` 可能尚未置位、`overlay_ready == false`）；
步骤 8 全部通过后发 `READY`。三者是**三条独立、有序**的通知（见 §5.1）。

### 5.3 overlay 可达性判定（**关键决策 D2**）

任务文档 §5.1 第 8 步要求“确认 overlay remote 可通过该设备到达”，§9.3 要求
`route get overlay-remote` 命中虚接口。本契约的 v1 语义为：

- **前提由校验保证**：§4.2 规则 5 强制 `overlay_remote` 落在
  `overlay_local/prefix_len` 之内，因此内核必然生成一条经该虚接口的 connected 路由；
- **API 侧判定**：`READY` 要求「设备存在 + overlay 地址在 midrd 的接口视图可见 +
  接口 `IFF_UP`」三者同时成立；
- **不在 API 内做主动探测**：midrd 无 ICMP 探针，且新增 route-get/nexthop-lookup
  ZAPI 会显著扩大本组 ABI 面。真正的端到端连通性由
  ①`midr-test/midr-gre-connectivity-test.sh` 的 overlay 双向 ping，以及
  ②P4 在容器内执行的 `ip route get <overlay_remote>` 命中虚接口 来独立验证。

若 `overlay_remote` 不在 `overlay_local` 前缀内（任务文档 §5.1 注的第二种地址规划），
本契约**显式不支持**并在 `VALIDATE` 阶段以 `EINVAL` 拒绝，而不是静默返回 READY。
后续如需支持，须由第一/第二组共同提出 Session/路由扩展，不属于本组设备 API。

### 5.4 确认通路（**关键决策 D1**）

`midrd` 的共享 zclient 由
`midrd/midr-dp-backend.c:1137` 以 `zclient_new(master, &zclient_options_default, NULL, 0)`
创建。**`handlers == NULL` 并不妨碍默认库处理器**：
`lib/zclient.c:4881-4883` 在 `!auxiliary` 时始终先调用 `lib_handlers[command]`，
因此**设备级**通知无需自建 handler 即可收到：`lib_handlers[]`
（`lib/zclient.c:4757-4775`）包含 `ZEBRA_INTERFACE_ADD/DELETE/UP/DOWN`，
`midr_gre.c:326` 的 `if_lookup_by_name()` 确认路径正是依赖这一点。

> **⚠️ P0 勘误（2026-09-29，由 vlink-eng 独立评审发现并经 lead 复核）**
>
> 本契约初版曾断言 `lib_handlers[]` 也包含**地址变更**处理器。**该断言错误。**
> 实测 `lib_handlers[]` 只有上述 4 个**接口级**项，**不含**
> `ZEBRA_INTERFACE_ADDRESS_ADD` / `ZEBRA_INTERFACE_ADDRESS_DELETE`；
> 且 midrd 全仓未注册这两个 handler。
>
> 后果（严重）：midrd 的接口视图**永远没有 overlay 地址** →
> `connected_lookup_prefix_exact()`（`midr-virtual-link.c` 的可达性判定）恒为 false →
> `CONFIGURING` 永不进入 `READY` → 每次 `add` 都以超时 `FAILED` 收场。
> 组件测试用 `connected_add_by_prefix()` 直接构造客户端视图，会**掩盖**该缺口。
>
> **因此 P2 必须补一步（MF-1）**：在 midrd 的 zclient bootstrap 注册
> `[ZEBRA_INTERFACE_ADDRESS_ADD]` 与 `[ZEBRA_INTERFACE_ADDRESS_DELETE]` →
> `zebra_interface_address_read(cmd, zclient->ibuf, vrf_id)`
> （声明 `lib/zclient.h:1105`，实现 `lib/zclient.c:3191`；对照既有用法
> `bgpd/bgp_zebra.c:4458`、`ospfd/ospf_zebra.c:2194`）。
> 该修复**不新增任何 ZAPI 消息**，只是让 midrd 把 zebra 已经在广播的地址通知接进来。

**结论（仍然不需要新增确认 ZAPI）**：

- P1 只负责「下发」：创建地址 + 置 UP 的请求发送与拒绝回报；
- P2 还需**注册既有地址通知 handler**（MF-1，见上方勘误）——这不是新增消息；
- P2 负责「确认」：通过 `if_lookup_by_name()/if_lookup_by_index()` +
  `connected_lookup_prefix_exact()` + `if_is_up()` 观察 zebra 通告的结果；
- 两个方向都走既有 ZAPI 通知机制，**不新增任何确认消息**，也不启用 `auxiliary`；
- **P3 测试不得只靠 `connected_add_by_prefix()` 模拟**：必须另有用例证明 midrd
  确实注册了地址 handler（否则 MF-1 会被再次掩盖），并由 P4 在真实 zebra 环境中
  以 `READY` 达成来端到端证明。


## 6. P1 底层 ZAPI 契约

### 6.1 新增消息（**关键决策 D3：3 条独立消息**）

在 `lib/zclient.h` 的 `enum zebra_message_types_t` **末尾**追加：

```c
	ZEBRA_GRE_ADD,
	ZEBRA_GRE_DELETE,
	ZEBRA_INTERFACE_ADDRESS_SET,	/* 新增：下发 overlay 地址 */
	ZEBRA_INTERFACE_ADDRESS_UNSET,	/* 新增：撤销 overlay 地址 */
	ZEBRA_INTERFACE_SET_ADMIN_UP,	/* 新增：接口 admin up/down */
```

选择 3 条独立消息而非 2 条（地址 set/unset 合并、带 install 布尔）的理由：
与 FRR 既有 `ZEBRA_INTERFACE_SET_ARP` 等单一职责消息风格一致，错误定位与幂等
判断更直接，且避免在一条消息里复用两套语义。

### 6.2 线格式

复用既有 GRE 的 `zclient_gre_encode_addr()` 地址编码（`lib/zclient.c`）。

```text
ZEBRA_INTERFACE_ADDRESS_SET / ZEBRA_INTERFACE_ADDRESS_UNSET
+--------+------------------+--------+---------+-----+--------+--------+
| ifname | ifindex (u32)    | addr   | pfx_len | peer| label  |
| IFNAMSIZ| 0 => 按名字解析  | encode |  (u8)   |enc. | IFNAMSIZ|
+--------+------------------+--------+---------+-----+--------+

ZEBRA_INTERFACE_SET_ADMIN_UP
+--------+------------------+----------+
| ifname | ifindex (u32)    | up (u8)  |  1 = up, 0 = down
+--------+------------------+----------+
```

对应客户端编码结构（声明进 `lib/zclient.h`）：

```c
struct zclient_interface_address {
	char ifname[IFNAMSIZ];
	ifindex_t ifindex;	/* 0 => zebra 按 ifname+vrf 解析 */
	struct ipaddr addr;
	uint8_t prefixlen;
	struct ipaddr peer;	/* IPADDR_NONE 表示无 */
	char label[IFNAMSIZ];	/* 可空 */
};

extern enum zclient_send_status
zclient_send_interface_address_set(struct zclient *client, vrf_id_t vrf_id,
				   const struct zclient_interface_address *addr);
extern enum zclient_send_status
zclient_send_interface_address_unset(struct zclient *client, vrf_id_t vrf_id,
				     const struct zclient_interface_address *addr);
extern enum zclient_send_status
zclient_send_interface_admin_up(struct zclient *client, vrf_id_t vrf_id,
				const char *ifname, bool up);
```

编码一律使用枚举符号，禁止字面量编号（门禁断言 4 已检查）。

### 6.3 zebra 侧实现

`zebra/zapi_msg.c` 新增 handler，并在 `zserv_handlers[]` 中以
**designated initializer** 追加 3 条（保持既有 handler 位置不变）：

| 消息 | handler 行为 |
|---|---|
| `ZEBRA_INTERFACE_ADDRESS_SET` | 解析 → `if_lookup_by_index()` 或 `if_lookup_by_name()` → 组 `struct prefix` → `if_ip_address_install(ifp, &prefix, label, NULL)`（`zebra/interface.c:3982`，声明 `zebra/interface.h:314`） |
| `ZEBRA_INTERFACE_ADDRESS_UNSET` | 同上 → `if_ip_address_uninstall(ifp, &prefix, NULL)`（`zebra/interface.c:4037`，声明 `zebra/interface.h:316`） |
| `ZEBRA_INTERFACE_SET_ADMIN_UP` | `if_no_shutdown(ifp)`（`up=1`）或 `if_shutdown(ifp)`（`up=0`）（`zebra/interface.c:3910/3929`，声明 `zebra/interface.h:322-323`） |

若接口不存在 / ifindex 无效 → 直接丢弃并 `zlog_warn`，不 crash（zebra 对未知
客户端的畸形请求必须健壮）。`if_ip_address_install/uninstall` 与
`if_no_shutdown/if_shutdown` 均已具备幂等性（`connected_check_ptp` 复用既有连接项、
`if_set_flags`/`if_unset_flags` 幂等），无需额外去重。

> **授权面声明（本轮补记）**：这 3 个 handler 沿用 FRR ZAPI 的既有信任模型——**不区分发起方**：
> 任何已连接到同一 zserv socket 的客户端都可以对任意（存在的）接口装卸地址或设置
> admin up/down。这与既有 ZAPI 原语（例如路由安装）同一性质，**不是本组新引入的特权面**；
> 此处显式写明，避免后续被误判为缺陷。工程上的约束由 socket 的文件权限（FRR daemon 与 root）
> 承担，本 API 不做 per-client 鉴权。若将来需要鉴别发起方，属 ZAPI 层整体议题。


### 6.4 编号与门禁更新（**必须做，否则门禁会 FAIL**）

`lib/log.c` 的 `command_types[]` 尾部追加 3 条 `DESC_ENTRY`，顺序与 enum 一致。

`midr-test/check-zapi-numbering.sh` 当前**硬编码**“只允许追加 GRE 两条”，
新增消息后必然 FAIL。必须同步修改：

1. 断言 2 的白名单：`$GRE_ADD/$GRE_DEL` → 5 条追加名单
   （`ZEBRA_GRE_ADD`、`ZEBRA_GRE_DELETE`、`ZEBRA_INTERFACE_ADDRESS_SET`、
   `ZEBRA_INTERFACE_ADDRESS_UNSET`、`ZEBRA_INTERFACE_SET_ADMIN_UP`）；
2. 计数断言 `ref_n + 2` → `ref_n + 5`；
3. `$GRE_ADD`/`$GRE_DEL` 的尾部位序断言（第 178-183 行）→ 5 条依次位于尾部；
4. 断言 3 的“最后 2 行 DESC_ENTRY 必须是 GRE” → 最后 5 行；
5. 描述性位置过滤器（第 224-225 行）把新增 3 个名字一并排除后再比较非新条目顺序。

**基线不变**：`DEFAULT_BASELINE=1adb4c92d0` 保持不动，既有成员序号必须逐一不变。


## 7. 幂等、rebind 与回滚

### 7.1 幂等

- 相同 `ifname` + 相同 outer endpoint + 相同 overlay 地址/prefix：`CREATING` 中
  返回 0（不重复下发）；`READY` 中返回 0 并直接给出现状；`FAILED` 中返回 0 并
  重新走一遍建立流程（幂等重试，见 §7.2 的彻底拆除）。
- 相同 endpoint 但 `ifname` 为空：复用 `midr_gre_interface_name()` 反查既有名字，
  不创建第二个设备；若反查到的是**本模块自己的**旧条目（无论是否 `FAILED`），
  按 §7.2 先彻底拆除再重建；只有当那个 LIVE 设备属于**外来隧道**时才 `EEXIST`。

### 7.2 rebind（同 ifname 不同 endpoint/overlay）与重建（**本轮 D3**）

- **同 ifname、不同 desc、未置 `MIDR_VLINK_F_REBIND`**：默认**拒绝**
  `-1` + `FAILED` + `stage=VALIDATE` + `last_error=EEXIST`（不变）；
- **置 `MIDR_VLINK_F_REBIND`**：先按 §8.1 **彻底拆除**旧链路（撤销 overlay 地址 +
  删除 GRE 设备 + 取消定时器），再以新 desc 建立；任何一步失败都进入 `FAILED`，
  **不得静默覆盖**；
- **`FAILED` 同 desc 重试**：同样先彻底拆除旧链路，再重建；
- **空 `ifname` 复用同一 outer endpoint 对**：若已存在条目（本模块自己的，无论是否
  `FAILED`），先彻底拆除再重建，**不再直接 `EEXIST`**。

**`EEXIST` 的收窄（D3）**：对「同一 outer endpoint 对换个名字（显式名或自动生成名）
复用」这条路径，`VALIDATE/EEXIST` 现在**只**在 GRE 层仍为该 endpoint 对持有一个
**LIVE 设备**（`midr_gre_interface_get_state()` 的状态不是 `DOWN`/`FAILED`）且其名字
与本请求不同或为自动生成名时返回 —— 即该设备属于**外来（foreign）隧道**，
本模块不得将其重建或重命名。由于本模块自己的旧条目已在上面被彻底拆除，
此处仍存的 LIVE 设备必然是外来的。

> **为什么要彻底拆除**：旧实现（本轮修复前）在空 `ifname` / rebind 分支里只
> `listnode_delete()+free()` 旧条目，**不撤销 overlay 地址、也不删除 GRE 设备**；
> 随后 `midr_gre_interface_add()` 会复用同一个内核设备（GRE 层按 endpoint 对建索引），
> 把新 overlay 地址**叠加**在旧地址上 —— 正是本契约 §7.2 禁止的静默覆盖。
> D3 之后重建总是从干净状态开始。

### 7.3 失败回滚

任一步失败都不得报告 `READY`。要求：

- `GRE_CREATE` 失败：无设备需清理，registry 不登记；
- `DEVICE_CONFIRM` 超时：调用 `midr_gre_interface_del()` 回收刚建的设备；
- `ADDRESS_SET` 失败：撤销已下发的地址（`_unset`），再 `midr_gre_interface_del()`；
- `ADMIN_UP` 失败：撤销地址 + 删设备；
- 回滚本身失败时，保留 `FAILED` + `stage` + `ifname`/`ifindex`，标记为可重试，
  不清除 descriptor（允许第一组决定重建）；
- 失败后 registry 中保留条目但状态为 `FAILED`，使 `get_state()` 可解释失败原因。

**`status.ifindex` 在 `FAILED` 时的精确规则（本轮 D2）**：

| 失败来源 | `stage` | `FAILED` 时的 `status.ifindex` |
|---|---|---|
| 设备消失（`DEVICE_CONFIRM`，含 §8.2 自主检测） | `MIDR_VLINK_STAGE_DEVICE_CONFIRM` | `0`（旧 ifindex 失效） |
| **其他任何失败阶段**（`ADDRESS_SET` / `ADMIN_UP` / `REACHABILITY` 等） | 对应阶段 | **最后一次已确认的 ifindex**（§7.3 保留） |

即：只有「设备不见了」这类失败才把 `ifindex` 归零；配置类失败保留最后已确认的 ifindex，
便于第一组定位设备与排障。该规则同时体现在 §3.1 的字段说明与 §5.3 的 ifindex 失效描述中。

## 8. 删除、Zebra 重连与设备丢失

### 8.1 删除顺序（任务文档 §5.2）

```text
1. 停止该链路的新配置与状态通知;
2. 撤销 overlay 地址（P1 _unset）或将接口置 DOWN;
3. midr_gre_interface_del() 删除 GRE 设备;
4. 清理本地 registry;
5. 报告 DOWN。
```

删除**不得**调用第一组 Session/Link API；第一组负责先处理其逻辑邻接状态。

### 8.2 Zebra 重连 / 设备消失（任务文档 §5.3；**本轮 D1：自主检测**）

检测**不再依赖调用方轮询**（旧描述「仅惰性对账」已作废）。本模块用两条自主通路
感知丢失：

1. **GRE 层通知订阅**：`midr_virtual_link_init()` 订阅 `midr_gre_register_notify()`，
   当 GRE 层对本模块拥有的设备报 `DOWN`/`FAILED` 时立即处理。本模块自身
   rollback/delete 触发的帧通过 `deleting` 标志抑制，不会误判。
2. **模块级对账定时器**：整个模块**共用一个** reconcile 定时器，周期 **1000 ms**
   （`MIDR_VLINK_RECONCILE_INTERVAL_MS = 1000`）。只要 registry 中**存在任一非 `DOWN`
   条目（含 `READY`）** 就保持 armed，全部 `DOWN` 时才取消。每次触发对每个非 `DOWN`
   条目重新核对设备存在性；对 `READY` 条目**额外**核对 overlay 地址与 `IFF_UP`。

命中丢失时（设备消失，或 `READY` 条目丢失 overlay 地址 / `IFF_UP`）：

- 条目迁移到 `FAILED`，**由模块自主发出 `FAILED` 通知**（`status->event ==
  MIDR_VLINK_EV_FAILED`），**无需调用方调用 `get_state()`/`wait_ready()`**；
- 统一的失败标记：`stage = MIDR_VLINK_STAGE_DEVICE_CONFIRM`、`last_error = ENODEV`，
  **旧 `ifindex` 立即失效置 0**（与 §7.3 的 D2 规则一致）；
- **保留 descriptor**，允许第一组决定是否重建；
- 不改变第一组拥有的逻辑 `link_id`；
- 重建成功后返回**新的** `ifindex`，并再次走 `DEVICE_UP → ADDRESS_SET → READY`。

**D4（进入 `FAILED` 时对账台账）**：进入 `FAILED` 时模块用
`midr_gre_interface_get_state()` 回填 `gre_created`（GRE 层已无该设备则清掉），
并清除 `addr_set`。这样之后的 `add()`/`del()` 不会基于**过期视图**动作 —— 既不会
去删一个已经不存在的设备，也不会漏删一个仍然存在的设备。

## 9. 测试接缝（P3 必须覆盖）

### 9.1 组件测试（无真实 zebra，沿用 `midr-test/gre-registry-test.c` 模式）

测试程序骨架：`#define main midrd_program_main` + `#include "midrd.c"`；
用 `-Wl,--wrap=` 注入。所需接缝：

| 接缝 | 用途 |
|---|---|
| `--wrap=zclient_send_gre_add` / `_gre_delete` | 既有，控制设备创建成功/失败 |
| `--wrap=zclient_send_interface_address_set` | 注入地址下发成功/失败 |
| `--wrap=zclient_send_interface_address_unset` | 验证回滚确实撤销了地址 |
| `--wrap=zclient_send_interface_admin_up` | 注入置 UP 成功/失败 |
| `--wrap=calloc` | 既有 `fail_next_calloc` 模式，覆盖 registry 分配失败 |

**“zebra 已确认”的模拟**：测试直接用 libfrr 公开 API 操作客户端接口视图——
`if_get_by_name()` 建接口、`if_set_index()` 赋 ifindex、`if_set_flags(ifp, IFF_UP)`
置位、`connected_add_by_prefix()` 添加 overlay 地址——与真实 zebra 通知落到
客户端视图的效果等价，**无需为测试在生产代码里加钩子**。

### 9.2 用例清单（任务文档 §9.1 十二项 + 本轮新增 13-18）

| # | 用例 | 期望 |
|---|---|---|
| 1 | 缺 `outer_local`/`outer_remote` | `-1`/`VALIDATE`/`EINVAL` |
| 2 | outer 两端地址族不一致 | `-1`/`VALIDATE`/`EAFNOSUPPORT` |
| 3 | overlay 地址或 prefix 非法（含 remote 不在前缀内） | `-1`/`VALIDATE`/`EINVAL` |
| 4 | `zclient_send_gre_add` 失败 | `-1`/`GRE_CREATE` |
| 5 | 地址下发失败 | `-1`/`ADDRESS_SET` + 已回滚设备 |
| 6 | 置 UP 失败 | `-1`/`ADMIN_UP` + 已回滚地址与设备 |
| 7 | 一直不出现 ifindex | `-1`/`DEVICE_CONFIRM`/`ETIMEDOUT` |
| 8 | 重复 `add` 同 desc | 幂等，返回 0，不下发第二次 |
| 9 | 同 ifname 不同 endpoint，无 REBIND | `-1`/`VALIDATE`/`EEXIST` |
| 10 | 同 ifname 不同 endpoint，带 REBIND | 先彻底拆除再建，最终 READY |
| 11 | 删除 + 重建 | `DOWN` 后重建得到新 ifindex 并 `READY` |
| 12 | 设备消失 + ifindex 变化（自主） | 自主收到 `FAILED`（`stage=DEVICE_CONFIRM`/`last_error=ENODEV`/`ifindex==0`），desc 保留，重建后状态正确 |
| 13 | **D1 自主失败**：达 `READY` 后使设备从客户端接口视图消失，仅泵事件循环，**不调用 `get_state()`** | 回调收到 `FAILED`（`stage=DEVICE_CONFIRM`/`last_error=ENODEV`/`ifindex==0`），desc 保留；随后同 desc `add()` 重新达 `READY` 并得到新 ifindex |
| 14 | **事件顺序与 overlay 字段**：记录回调序列 | 依次观察到 `DEVICE_UP` -> `ADDRESS_SET` -> `READY`；`ADDRESS_SET` 回调中 `overlay_local`/`overlay_remote`/`overlay_prefix_len` 正确且 `overlay_ready == false`；`READY` 回调中 `overlay_ready == true` 且 prefix 正确 |
| 15 | **D2 ifindex 保留**：分别在 `ADDRESS_SET` 与 `ADMIN_UP` 阶段注入失败 | `FAILED` + 阶段正确 + `ifindex` 非 0 且等于已确认的 ifindex；设备消失用例仍断言 `ifindex == 0`（§5.3/§7.3） |
| 16 | **D3 空名重建无残留**：空 ifname、同 outer endpoint 对、overlay 地址不同 | 旧 overlay 地址被 `_unset`（wrap 计数递增）、GRE 设备在新建前被删除；无重复 create/泄漏；结果 status 只含新 overlay 地址；同 desc 的空名重复 `add()` 仍幂等短路（无第二次 unset/create） |
| 17 | **查询路径不发通知** | `get_state()`/`wait_ready()` 前后回调计数不变，且其返回的 `status.event == MIDR_VLINK_EV_NONE` |
| 18 | **D4 台账对账**：case13 自主失败后紧跟一次 `add()`/`del()` | 恰好一次设备 create 与一次设备 delete（无过期 `gre_created`/`addr_set` 导致的额外或缺失 delete） |

> 用例 13-18 为本轮（W3）新增，覆盖 D1-D4 与「事件/overlay 新面」。其**执行结果待实测**：
> 组件套件在远端构建容器内运行，在验证报告给出计数前，本文一律标记为 **待实测 /
> not measured**，不得写作通过。

### 9.3 脚本改造（任务文档 §9.2）

`midr-test/midr-gre-connectivity-test.sh` 中现有手工
`ip addr add ... dev greX` / `ip link set greX up` 必须替换为经新 API 完成，
并断言：设备类型（gre/ip6gre）、`state=READY`、overlay 地址已在接口上、
overlay 双向 ping 成功、返回 `ifindex` 有效、删除后设备与地址均被清理。

## 10. 构建与 Makefile

- 顶层 `Makefile.am`：把 `midrd/midr-virtual-link.{c,h}` 加入 `midrd_midrd_SOURCES`；
- `midrd/Makefile`：把 `midr-virtual-link.o` 加入 `DP_OBJECTS`（或新建
  `VLINK_OBJECTS` 并在 `midrd` 与 `midrd-gre-tool` 目标中链接）；
- `midrd/Makefile` 新增 `virtual-link-test` 目标并纳入 `test`；
- `midr-test/extraction-boundary-test.sh` 的 `dp-contract.c` 阶段头文件列表加入
  `midr-virtual-link.h`。

## 11. 待第一组会签与开放问题

> **会签材料已单独成包**：`doc/midr-doc/dp-doc/midr-virtual-link-api-countersign.md`
> （含最终 API 摘要、自草案以来的 7 条变更清单（C1–C7）及其对调用方的影响、请第一组回答的
> 9 个问题、期望调用序列示例与回签表）。请第一组以该包为准逐条回签。

1. **C 签名会签**：任务文档 §4 要求签名由第一组与第三组联合确认。本契约先按
   文档草案冻结并实现；若第一组要求改名/改字段，按本文件整体替换。
   本轮新增的 `event`/`overlay_prefix_len`/`overlay_ready` 属**追加字段**，回调签名不变，
   调用方源码兼容（会签包 C6）。
2. **`overlay_remote` 不在本地前缀内的地址规划**：本契约 v1 显式拒绝（§5.3）。
   若第一组需要该规划，须另行提出路由配置或 Session 侧调整。
3. **Session 实际选路**（任务文档 §8）：`midr_session_connect()` 不接收本地
   ifindex/源地址，出站连接按 midrd 监听配置 bind。因此 `READY` 只保证设备与
   overlay 数据面就绪，**不保证** MIDR Session 一定经 GRE 承载。该确认由第一、
   第二组联合完成，不属于本组设备 API。
4. **`iftype` 取值**：沿用 `enum zebra_iftype`（GRE=8、IP6GRE=9）。
5. **跨组提示：overlay nexthop + prefix 的交接（第一组 → 第二组）**：
   第一组从 `READY` 通知/状态中拿到的 overlay 三件套是
   `overlay_local` + `overlay_remote` + `overlay_prefix_len`。但
   `midrd/midr-topology.h` 的 `struct midr_link_update` **只有**
   `link_local_address` / `link_remote_address`，**没有前缀长度字段**
   （本轮 grep 确认 `midrd/midr-topology.{h,c}` 内无 `prefix_len`/`prefixlen`/`overlay`）。
   因此第一组把 overlay 下一跳连同前缀交给第二组时，**可能需要在第一组/第二组一侧
   新增一个 prefix-length 字段**（或约定由哪一侧补齐）。这属于第一、第二组的接口，
   不在本组设备 API 的范围内；第三组只负责把 `overlay_prefix_len` 如实交付给调用方。
