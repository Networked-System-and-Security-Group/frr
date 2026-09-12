# MIDR GRE 虚拟接口接口说明与使用文档

> 头文件：`bgpd/bgp_midr_gre.h`
> 实现：`bgpd/bgp_midr_gre.c`
> 底层：`lib/zclient.{c,h}` → `zebra/{zapi_msg.c,zebra_dplane.c,kernel_netlink.c,if_netlink.c}`
> 设计说明见：`doc/midr-doc/midr-gre-interface.md`
> 测试报告见：`doc/midr-doc/midr-gre-test-report.md`

本组接口是 MIDR 控制面（标准 SPF / TE CSPF）**唯一**用于创建与消除 GRE
虚拟链路接口的入口。控制面只描述"要什么"（两端点地址等），不接触 netlink，
所有内核动作由 zebra 统一执行。

---

## 1. 接口一览

| 接口 | 功能 | 是否阻塞 |
|------|------|:--------:|
| `midr_gre_interface_add()` | 创建/更新 GRE 接口 | 否（异步确认） |
| `midr_gre_interface_del()` | 按名字删除 GRE 接口 | 否 |
| `midr_gre_interface_del_by_endpoints()` | 按两端点删除 | 否 |
| `midr_gre_interface_get_state()` | 查询当前（已校准）状态 | 否 |
| `midr_gre_interface_wait_up()` | **同步等待并返回是否建立成功** | 是（有超时上限） |
| `midr_gre_interface_name()` | 由两端点反查接口名 | 否 |
| `midr_gre_register_notify()` / `midr_gre_unregister_notify()` | 注册/注销状态变化回调 | 否 |
| `midr_gre_state_str()` | 状态枚举转字符串 | 否 |
| `midr_gre_fini()` | 释放本地注册表（进程退出时调用） | 否 |

---

## 2. 数据结构

### 2.1 `struct midr_gre_tunnel`（输入）

隧道描述，作为 `midr_gre_interface_add()` 的输入。

| 字段 | 类型 | 必填 | 说明 |
|------|------|:----:|------|
| `ifname` | `char[IFNAMSIZ]` | 否 | 设备名；**留空则自动生成** `midr-gre-<n>` |
| `vrf_id` | `vrf_id_t` | 否 | `VRF_UNKNOWN` 表示使用 BGP 实例的 VRF；通常填 `VRF_DEFAULT`(0) |
| `local` | `struct ipaddr` | 是 | 隧道源地址（IPv4 或 IPv6） |
| `remote` | `struct ipaddr` | 是 | 隧道目的地址（须与 `local` 同族） |
| `link_ifindex` | `ifindex_t` | 否 | 底层出接口；0 表示由内核自行解析 |
| `ikey` / `okey` | `uint32_t` | 否 | GRE key，无则 0 |
| `encap_flags` | `uint16_t` | 否 | 可选校验和标志，无则 0 |
| `mtu` | `uint32_t` | 否 | 隧道 MTU；0 表示内核默认 |

`struct ipaddr` 用法：先置 `ipa_type = IPADDR_V4` / `IPADDR_V6`，
再填 `ipaddr_v4` / `ipaddr_v6`。

### 2.2 `enum midr_gre_state`（输出）

| 值 | 含义 |
|----|------|
| `MIDR_GRE_STATE_DOWN` | 未请求，或设备已被删除 |
| `MIDR_GRE_STATE_PENDING` | 请求已发出，等待内核确认 |
| `MIDR_GRE_STATE_UP` | zebra 已上报该设备，`ifindex` 有效 |
| `MIDR_GRE_STATE_FAILED` | 请求被拒绝，或确认超时（见 `err`） |

### 2.3 `struct midr_gre_status`（输出）

| 字段 | 类型 | 有效条件 | 说明 |
|------|------|----------|------|
| `state` | `enum midr_gre_state` | 总是 | 当前状态 |
| `ifname` | `char[IFNAMSIZ]` | 总是 | 实际使用的设备名（含自动生成） |
| `vrf_id` | `vrf_id_t` | 总是 | VRF |
| `ifindex` | `ifindex_t` | `state==UP` | 内核接口索引 |
| `iftype` | `uint8_t` | `state==UP` | `enum zebra_iftype`（GRE=8、IP6GRE=9） |
| `err` | `int` | `state==FAILED` | errno 风格原因 |
| `up_ms` | `uint32_t` | `state==UP` | 已 UP 的毫秒数 |

### 2.4 回调类型

```c
typedef void (*midr_gre_notify_cb)(const struct midr_gre_status *status,
                                   void *arg);
```

在 `PENDING→UP`、`PENDING→FAILED`、`UP→DOWN` 时被调用。
`status` 指针仅在回调期间有效，需长期保存请自行拷贝。

---

## 3. 接口详解

### 3.1 `midr_gre_interface_add()`

```c
int midr_gre_interface_add(struct bgp *bgp,
                           const struct midr_gre_tunnel *tun,
                           struct midr_gre_status *status);
```

**功能**：向 zebra 下发创建/更新 GRE 虚拟接口的请求。

| 参数 | 说明 |
|------|------|
| `bgp` | BGP 实例；用于取缺省 VRF。可传 `NULL`（则用 `tun->vrf_id` 或 `VRF_DEFAULT`） |
| `tun` | 隧道描述（见 2.1），必填 |
| `status` | 可空。返回**立即结果**：`UP`（已存在）/ `PENDING`（已受理）/ `FAILED` |

**返回值**

| 返回 | 含义 |
|------|------|
| `0` | 请求已受理（`status->state` 为 `UP` 或 `PENDING`） |
| `-1` | 立即失败（`status->state = FAILED`，原因见 `status->err`） |

常见 `err`：`EINVAL`（端点缺失）、`EAFNOSUPPORT`（端点族不一致）、
`ENOTCONN`（zclient 未就绪）、`EIO`（发送失败）。

**注意**：返回 `PENDING` 只代表请求已受理，**不代表接口已建立**。
要拿到"是否建立成功"的结论，请使用 `midr_gre_interface_wait_up()`
（同步）或 `midr_gre_register_notify()`（异步）。

### 3.2 `midr_gre_interface_wait_up()` —— 返回是否建立成功

```c
bool midr_gre_interface_wait_up(struct bgp *bgp, const char *ifname,
                                uint32_t timeout_ms,
                                struct midr_gre_status *status);
```

**功能**：等待指定接口进入 `UP`（或 `FAILED`），最长 `timeout_ms`。
内部会驱动 bgpd 事件循环以接收 zebra 的接口通知。

| 参数 | 说明 |
|------|------|
| `ifname` | 设备名（自动生成的名字可用 `midr_gre_interface_name()` 获取） |
| `timeout_ms` | 等待预算，例如 3000 |
| `status` | 可空。返回最终状态（`UP` 时含 `ifindex` / `iftype` / `up_ms`） |

**返回值**：`true` = 接口已建立；`false` = 超时 / 失败 / 未知接口。

**约束**：必须在 bgpd 主线程调用（会阻塞至多 `timeout_ms`）。

### 3.3 `midr_gre_interface_get_state()`

```c
int midr_gre_interface_get_state(vrf_id_t vrf_id, const char *ifname,
                                 struct midr_gre_status *status);
```

**功能**：非阻塞查询当前状态，并顺带校准（若曾 `UP` 但设备已消失则降为 `DOWN`）。

**返回值**：`0` = 已知并填充 `status`；`-1` = 本地注册表无此隧道。

### 3.4 `midr_gre_interface_del()`

```c
int midr_gre_interface_del(struct bgp *bgp, const char *ifname,
                           struct midr_gre_status *status);
```

**功能**：按名字请求删除 GRE 接口。
**返回**：`0` 成功受理（`status->state = DOWN`）；`-1` 失败（`err` 见上）。

> 删除是异步的：zebra 经 dplane → netlink 下发 `RTM_DELLINK` 后设备才消失。

### 3.5 `midr_gre_interface_del_by_endpoints()`

```c
int midr_gre_interface_del_by_endpoints(struct bgp *bgp, vrf_id_t vrf_id,
                                        const struct ipaddr *local,
                                        const struct ipaddr *remote,
                                        struct midr_gre_status *status);
```

**功能**：按 `(vrf, local, remote)` 找到接口并删除。
**返回**：`0` 成功；`-1` 未找到（`err = ENOENT`）或发送失败。

### 3.6 `midr_gre_interface_name()`

```c
const char *midr_gre_interface_name(vrf_id_t vrf_id,
                                    const struct ipaddr *local,
                                    const struct ipaddr *remote);
```

**功能**：由两端点反查（可能是自动生成的）接口名。
**返回**：名字指针（只读，进程内有效）；无则 `NULL`。

### 3.7 状态变化回调

```c
void midr_gre_register_notify(midr_gre_notify_cb cb, void *arg);
void midr_gre_unregister_notify(midr_gre_notify_cb cb);
```

**功能**：注册/注销全局回调。注册后，任何隧道的状态迁移都会触发；
`status->ifname` 用于区分具体隧道。

### 3.8 `midr_gre_state_str()` / `midr_gre_fini()`

```c
const char *midr_gre_state_str(enum midr_gre_state state); /* "up"/"pending"/... */
void midr_gre_fini(void);   /* bgpd 退出时释放注册表（不删除内核设备） */
```

---

## 4. 调用方法

### 4.1 同步确认（需要立即知道"是否建立成功"）

```c
struct midr_gre_status st;
struct midr_gre_tunnel tun = {};

strlcpy(tun.ifname, "midr0", sizeof(tun.ifname));
tun.vrf_id = VRF_DEFAULT;
tun.local.ipa_type  = IPADDR_V4;
tun.remote.ipa_type = IPADDR_V4;
inet_pton(AF_INET, "10.1.1.11", &tun.local.ipaddr_v4);
inet_pton(AF_INET, "10.1.1.12", &tun.remote.ipaddr_v4);
tun.mtu = 1400;

/* 1) 下发请求：立即结果是 PENDING 或 UP */
if (midr_gre_interface_add(bgp, &tun, &st) != 0) {
        zlog_err("GRE add rejected: state=%s err=%d",
                 midr_gre_state_str(st.state), st.err);
        return -1;
}

/* 2) 等待并拿到"是否建立成功"的结论 */
if (midr_gre_interface_wait_up(bgp, "midr0", 3000, &st)) {
        zlog_info("GRE midr0 UP: ifindex=%u iftype=%u",
                  st.ifindex, st.iftype);
} else {
        zlog_err("GRE midr0 NOT established: state=%s err=%d",
                 midr_gre_state_str(st.state), st.err);
}
```

### 4.2 异步回调（长时间运行的控制面）

```c
static void gre_evt(const struct midr_gre_status *st, void *arg)
{
        /* PENDING→UP / PENDING→FAILED / UP→DOWN */
        zlog_info("GRE %s state=%s ifindex=%u err=%d", st->ifname,
                  midr_gre_state_str(st->state), st->ifindex, st->err);
}

midr_gre_register_notify(gre_evt, bgp);      /* 启动时注册一次 */

midr_gre_interface_add(bgp, &tun, &st);      /* 返回 PENDING，稍后回调告知结果 */
...
/* 也可随时查询 */
if (midr_gre_interface_get_state(VRF_DEFAULT, "midr0", &st) == 0)
        printf("state=%s ifindex=%u\n", midr_gre_state_str(st.state),
               st.ifindex);

midr_gre_unregister_notify(gre_evt);         /* 退出时注销 */
```

### 4.3 IPv6（自动选择 ip6gre）

```c
struct midr_gre_tunnel tun6 = {};

tun6.vrf_id = VRF_DEFAULT;
tun6.local.ipa_type  = IPADDR_V6;
tun6.remote.ipa_type = IPADDR_V6;
inet_pton(AF_INET6, "2001:db8::1", &tun6.local.ipaddr_v6);
inet_pton(AF_INET6, "2001:db8::2", &tun6.remote.ipaddr_v6);

midr_gre_interface_add(bgp, &tun6, &st);          /* 自动命名 midr-gre-<n> */
const char *nm = midr_gre_interface_name(VRF_DEFAULT, &tun6.local,
                                         &tun6.remote);
midr_gre_interface_wait_up(bgp, nm, 3000, &st);
```

---

## 5. 内部时序（"建立成功"如何被确认）

```
CP: midr_gre_interface_add()
      └─► ZEBRA_GRE_ADD ──► zebra ──► dplane ──► RTM_NEWLINK(+NLM_F_CREATE|EXCL)
                                                   │ 内核创建 netdevice
                                                   ▼
                                          zebra 收到 RTM_NEWLINK 通知
                                                   │
      zebra 广播 ZEBRA_INTERFACE_ADD ◄─────────────┘
      │
      ▼   (lib/zclient.c 默认 handler 建好 bgpd 侧 interface 对象)
bgpd: if_lookup_by_name() 命中 → state=PENDING→UP，记录 ifindex，触发回调
      （若 MIDR_GRE_CONFIRM_TIMEOUT_MS=3000 内未命中 → state=FAILED, err=ETIMEDOUT）
```

即：**"建立成功"= zebra 已确认该 netdevice 存在**，而不是仅"消息已发出"。

---

## 6. 注意事项

1. **`add` 返回 0 ≠ 建立成功**：请用 `wait_up()` 或回调获得最终结论。
2. **`wait_up()` 会阻塞**：只能在 bgpd 主线程调用，且应给有限超时（如 3s）。
3. **确认超时**：默认 3000 ms（`MIDR_GRE_CONFIRM_TIMEOUT_MS`），
   期间以 100 ms 间隔轮询 zebra 的接口视图。
4. **创建出的设备默认 DOWN**：本组接口只负责创建/配置设备本身，
   不会拉起设备或配置地址；需要 CP 另行处理。
5. **两端点必须同族**；混用返回 `-1` / `EAFNOSUPPORT`。
6. **删除是异步的**，且**调用方进程不要立即退出**：若发出 `ZEBRA_GRE_DELETE`
   后立刻关闭 zclient，zebra 可能在该次读中同时读到数据与 EOF 而丢弃请求。
   长时间运行的 bgpd 无此问题；一次性客户端应在发送后保持存活约 1s。
7. `midr_gre_fini()` 只释放本地注册表，**不会删除内核设备**。


