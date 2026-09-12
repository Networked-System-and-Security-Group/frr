# MIDR GRE 虚拟接口创建/消除接口设计说明

> 适用分支：`feat/te-dp-interface`（MIDR 数据平面）
> 相关文件：`bgpd/bgp_midr_gre.{c,h}`、`lib/zclient.{c,h}`、`zebra/{zapi_msg.c,zebra_dplane.{c,h},kernel_netlink.c,if_netlink.c}`

---

## 0. 改动总览

### 新增文件

| 文件 | 作用 |
|------|------|
| `bgpd/bgp_midr_gre.h` | CP 面 API 声明 + `struct midr_gre_tunnel` |
| `bgpd/bgp_midr_gre.c` | 参数校验、名称注册表、自动命名、**建立状态跟踪**、ZAPI 下发 |
| `tests/bgpd/test_midr_gre_e2e.c` | 单机 E2E 测试（IPv4 + IPv6 + 状态 API） |
| `tests/bgpd/test_midr_gre_link.c` | 双容器连通性测试辅助程序（setup/teardown） |
| `tests/bgpd/midr_gre_connectivity_test.sh` | 双容器 IPv4/IPv6 连通性测试脚本 |
| `doc/midr-doc/midr-gre-interface-api.md` | **接口说明与使用文档** |
| `doc/midr-doc/midr-gre-test-report.md` | **连通性测试报告** |
| `doc/midr-doc/midr-gre-interface.md` | 本文档 |

### 修改文件

| 文件 | 改动 |
|------|------|
| `lib/zclient.h` | 新增 `ZEBRA_GRE_ADD` / `ZEBRA_GRE_DELETE` 命令号；`struct zclient_gre_if`；发送函数声明 |
| `lib/zclient.c` | 实现 `zclient_send_gre_add()` / `zclient_send_gre_delete()`（地址族自适应编码） |
| `lib/log.c` | `command_types[]` 新增两条 `DESC_ENTRY(...)` |
| `zebra/zebra_dplane.h` | 新增 `DPLANE_OP_GRE_ADD` / `DPLANE_OP_GRE_DELETE` 与两个函数声明 |
| `zebra/zebra_dplane.c` | 实现 `dplane_gre_interface_add()` / `dplane_gre_interface_delete()` |
| `zebra/kernel_netlink.c` | 新 op 分发到 `netlink_put_gre_set_msg()` |
| `zebra/if_netlink.c` | 编码 `RTM_NEWLINK`(`NLM_F_CREATE\|NLM_F_EXCL`) / `RTM_DELLINK`；按地址族选 `gre`/`ip6gre` |
| `zebra/zapi_msg.c` | 新增 `zebra_gre_add()` / `zebra_gre_delete()` 并注册到 `zserv_handlers[]` |
| `bgpd/subdir.am` | 登记新增的 `.c` / `.h` |

---

## 1. 背景与目标

MIDR（多连接域间路由）在标准 SPF / TE CSPF 计算出跨域路径后，需要把 overlay
流量通过 GRE 隧道送到下一跳域边界路由器。为此控制面（CP）需要能够：

1. **创建**一条 GRE 虚拟接口（指定 tunnel 源/目的地址、可选 key、底层出接口、MTU）；
2. **消除**这条 GRE 虚拟接口；
3. **显式指定接口名**；若未指定，则由数据平面**自动生成**一个稳定、唯一的接口名。

设计原则与原有 `bgp_midr_zebra.c`（路由下发）保持一致：

- CP 只描述“要什么”，不接触 netlink / zebra 内部结构；
- 所有内核动作都由 **zebra** 统一执行（zebra 是设备与路由的唯一 owner）；
- bgpd 侧新增一个薄封装层，把 CP 的语义请求翻译成 ZAPI 消息。

> **结论：需要修改 zebra。** 原因见第 3 节——zebra 原本只有 GRE 的“解析/更新”能力，
> 缺少“创建（`RTM_NEWLINK` + `NLM_F_CREATE|NLM_F_EXCL` + `IFLA_IFNAME`）”和
> “删除（`RTM_DELLINK`）”能力。

---

## 2. 架构与消息流

```
MIDR 控制面 (bgp_midr_spf.c / bgp_midr_te.c / bgp_midr_cspf.c)
        │  midr_gre_interface_add() / midr_gre_interface_del()
        ▼
bgpd/bgp_midr_gre.c          新增：CP 面 API + 名称注册表 + 自动命名
        │  zclient_send_gre_add() / zclient_send_gre_delete()   (lib/zclient.c)
        │  ── ZAPI: ZEBRA_GRE_ADD / ZEBRA_GRE_DELETE ──►
        ▼
zebra/zapi_msg.c             zebra_gre_add() / zebra_gre_delete()  解码
        │  dplane_gre_interface_add() / dplane_gre_interface_delete()
        ▼
zebra/zebra_dplane.c/.h      DPLANE_OP_GRE_ADD / DPLANE_OP_GRE_DELETE
        ▼
zebra/kernel_netlink.c       op 分发 → netlink_put_gre_set_msg()
        ▼
zebra/if_netlink.c           编码 RTM_NEWLINK / RTM_DELLINK
        ▼
Linux 内核 (ip_gre / ip6_gre)
```

---

## 3. zebra 侧改动明细

### 3.1 为什么必须改 zebra

`zebra/if_netlink.c` 里原有的 `netlink_gre_set_msg_encoder()`：

- 发送 `RTM_NEWLINK`，但只有 `NLM_F_REQUEST`，**没有** `NLM_F_CREATE|NLM_F_EXCL`；
- **没有** `IFLA_IFNAME`。

因此它只能**修改**一个已经存在的 GRE 接口（由 nhrpd 或 `ip link add` 预先创建），
无法新建。同时 `DPLANE_OP_INTF_INSTALL/DELETE` 通道当前只实现了 `RTM_SETLINK`
（protodown），也不能复用。

### 3.2 新增 ZAPI 命令（`lib/zclient.h` / `lib/log.c`）

| 命令 | 方向 | 说明 |
|------|------|------|
| `ZEBRA_GRE_ADD` | client → zebra | 创建/更新 GRE 接口 |
| `ZEBRA_GRE_DELETE` | client → zebra | 删除 GRE 接口 |

`lib/log.c` 的 `command_types[]` 同步新增 `DESC_ENTRY(...)`，便于抓包/日志可读。

### 3.3 新增数据平面操作（`zebra/zebra_dplane.h/.c`）

| op | 内核消息 | 说明 |
|----|----------|------|
| `DPLANE_OP_GRE_ADD` | `RTM_NEWLINK` + `NLM_F_CREATE\|NLM_F_EXCL` | 新建 GRE 设备 |
| `DPLANE_OP_GRE_DELETE` | `RTM_DELLINK` | 删除 GRE 设备 |

新增公开函数：

```c
enum zebra_dplane_result
dplane_gre_interface_add(const char *ifname, vrf_id_t vrf_id,
                         const struct zebra_l2info_gre *gre_info,
                         ifindex_t link_ifindex, unsigned int mtu);

enum zebra_dplane_result
dplane_gre_interface_delete(const char *ifname, vrf_id_t vrf_id,
                            ifindex_t ifindex);
```

两者共用静态 helper `dplane_gre_intf_update_internal()`，复用原有 `struct dplane_gre_ctx`
（`link_ifindex` / `mtu` / `info`）以及 `ctx->zd_ifname`（存放接口名）。

### 3.4 netlink 编码（`zebra/if_netlink.c`）

`netlink_gre_set_msg_encoder()` 现在按 `dplane_ctx_get_op()` 区分三种情况：

- **DELETE** → `RTM_DELLINK`：优先用 `ifi_index`；若为 0 则用 `IFLA_IFNAME`；
- **ADD** → `RTM_NEWLINK`，附带 `NLM_F_CREATE | NLM_F_EXCL` 和 `IFLA_IFNAME`，
  并按需带上 `IFLA_MTU`、`IFLA_LINKINFO{IFLA_INFO_KIND="gre"/"ip6gre",
  IFLA_INFO_DATA{...}}`；
- **SET**（原有路径）→ 行为保持不变（保留 `zebra_gre_source_set()` 语义）。

`IFLA_INFO_DATA` 内包含：`IFLA_GRE_LOCAL`、`IFLA_GRE_REMOTE`、`IFLA_GRE_LINK`
（创建时为 0 则省略）、`IFLA_GRE_IKEY`、`IFLA_GRE_OKEY`、`IFLA_GRE_ENCAP_FLAGS`。

**IPv4 / IPv6 双栈支持**：`IFLA_INFO_KIND` 不再固定，而是由端点地址族决定
（`netlink_gre_link_kind()`）：

| 端点地址族 | `IFLA_INFO_KIND` | 内核模块 | 典型设备 |
|-----------|------------------|----------|----------|
| IPv4 | `"gre"` | `ip_gre` | `gre0` |
| IPv6 | `"ip6gre"` | `ip6_gre` | `ip6gre0` |

两种 kind 共用同一套 `IFLA_GRE_*` 封装属性，区别仅在于
`IFLA_GRE_LOCAL/REMOTE` 是 4 字节还是 16 字节——这一点在 ZAPI 编码
（`zclient_gre_encode_addr()`）与解码（`zebra_gre_add()`）中已经按
地址族自适应。zebra 的**读取/解析**路径原本就已支持 `ip6gre`/`ip6gretap`
（`netlink_determine_zebra_iftype()` → `ZEBRA_IF_IP6GRE`），因此创建后
zebra 能正确识别接口类型。

`netlink_put_gre_set_msg()` 的断言放宽为接受上述三种 op。

### 3.5 ZAPI 处理与分发

- `zebra/zapi_msg.c` 新增 `zebra_gre_add()` / `zebra_gre_delete()` 并注册到
  `zserv_handlers[]`；删除时若 zebra 已经知道该设备，则用其 `ifindex`。
- `zebra/kernel_netlink.c` 的 op 分发表把 `DPLANE_OP_GRE_ADD/DELETE`
  一并交给 `netlink_put_gre_set_msg()`。

### 3.6 ZAPI 报文格式

`zclient_create_header()` 之后的负载（编码见 `lib/zclient.c`，解码见
`zebra/zapi_msg.c`，两端字段顺序严格一致）：

`ZEBRA_GRE_ADD`：

| 字段 | 长度(byte) | 说明 |
|------|:----------:|------|
| `ifname` | `IFNAMSIZ`(16) | 以 `'\0'` 结尾的设备名，**必填** |
| `local`  family | 1 | `AF_INET`(2) 或 `AF_INET6`(10) |
| `local`  address | 4 / 16 | 随 family 变长 |
| `remote` family | 1 | 同上 |
| `remote` address | 4 / 16 | 随 family 变长 |
| `link_ifindex` | 4 | 底层出接口，0 = 让内核自行解析 |
| `ikey` | 4 | GRE key，无则 0 |
| `okey` | 4 | GRE key，无则 0 |
| `encap_flags` | 2 | 可选校验和标志，无则 0 |
| `mtu` | 4 | 0 = 内核默认 |

`ZEBRA_GRE_DELETE`：

| 字段 | 长度(byte) | 说明 |
|------|:----------:|------|
| `ifname` | `IFNAMSIZ`(16) | 以 `'\0'` 结尾的设备名 |

地址族同时决定了内核 link kind（见 3.4）：单条 `ZEBRA_GRE_ADD` 的负载长度在
44（IPv4）~ 68（IPv6）字节之间（不含 10 字节 ZAPI 头）。

---

## 4. 控制面（bgpd）接口

### 4.1 新增文件

| 文件 | 内容 |
|------|------|
| `bgpd/bgp_midr_gre.h` | CP 面 API 声明、`struct midr_gre_tunnel` |
| `bgpd/bgp_midr_gre.c` | 参数校验、名称注册表、自动命名、ZAPI 发送 |

### 4.2 隧道描述结构

```c
struct midr_gre_tunnel {
        char      ifname[IFNAMSIZ]; /* 可空：空则自动生成 */
        vrf_id_t  vrf_id;           /* VRF_UNKNOWN = 用 BGP 实例的 VRF */
        struct ipaddr local;        /* 隧道源 (IFLA_GRE_LOCAL)  */
        struct ipaddr remote;       /* 隧道目的 (IFLA_GRE_REMOTE) */
        ifindex_t link_ifindex;     /* 底层出接口，0 = 内核自行解析 */
        uint32_t  ikey, okey;       /* 可选 GRE key */
        uint16_t  encap_flags;      /* 可选校验和标志 */
        uint32_t  mtu;              /* 0 = 内核默认 */
};
```

### 4.3 对外 API

> 完整的接口说明（功能/参数/返回值/调用示例）见
> `doc/midr-doc/midr-gre-interface-api.md`。

```c
/* 创建/更新 GRE 接口；status 返回立即结果（UP / PENDING / FAILED） */
int midr_gre_interface_add(struct bgp *bgp, const struct midr_gre_tunnel *tun,
                           struct midr_gre_status *status);

/* 同步等待并返回“是否建立成功”（会驱动 bgpd 事件循环，有超时上限） */
bool midr_gre_interface_wait_up(struct bgp *bgp, const char *ifname,
                                uint32_t timeout_ms,
                                struct midr_gre_status *status);

/* 非阻塞查询当前状态 */
int midr_gre_interface_get_state(vrf_id_t vrf_id, const char *ifname,
                                 struct midr_gre_status *status);

/* 按名字删除（status 返回 DOWN/FAILED） */
int midr_gre_interface_del(struct bgp *bgp, const char *ifname,
                           struct midr_gre_status *status);

/* 按两端点删除 */
int midr_gre_interface_del_by_endpoints(struct bgp *bgp, vrf_id_t vrf_id,
                                        const struct ipaddr *local,
                                        const struct ipaddr *remote,
                                        struct midr_gre_status *status);

/* 状态变化回调（PENDING→UP / PENDING→FAILED / UP→DOWN） */
void midr_gre_register_notify(midr_gre_notify_cb cb, void *arg);
void midr_gre_unregister_notify(midr_gre_notify_cb cb);

/* 查询某两端点对应的（可能是自动生成的）接口名，无则返回 NULL */
const char *midr_gre_interface_name(vrf_id_t vrf_id,
                                    const struct ipaddr *local,
                                    const struct ipaddr *remote);

/* 状态枚举转字符串 */
const char *midr_gre_state_str(enum midr_gre_state state);

/* bgpd 退出时释放本地注册表（不删除内核设备） */
void midr_gre_fini(void);
```

**建立状态（`enum midr_gre_state`）**：`DOWN` / `PENDING` / `UP` / `FAILED`；
`struct midr_gre_status` 在 `UP` 时给出 `ifindex`、`iftype`、`up_ms`，
在 `FAILED` 时给出 `err`。

**"建立成功"的确认机制**：zebra 创建 netdevice 后会广播
`ZEBRA_INTERFACE_ADD`，bgpd 由 `lib/zclient.c` 的默认 handler 建好本地
interface 对象；本模块据此把状态由 `PENDING` 提升为 `UP`（默认 3000 ms
超时，100 ms 轮询），并由回调/`wait_up()` 对外返回。

### 4.4 命名规则（显式优先，否则自动生成）

`bgp_midr_gre.c` 维护一个小的进程内注册表
（`(vrf_id, local, remote) ↔ ifname`）：

1. 若 `tun->ifname` 非空 → **按调用者给定的名字** 创建；
2. 若为空 → 先按 `(vrf, local, remote)` 查表复用；
   查不到才生成 `midr-gre-<n>`（`n` 为自增序号），并写入注册表。

因此：

- 对同一对端点反复调用 `midr_gre_interface_add()` 是**幂等**的（复用同名接口）；
- 既可以用名字删除，也可以用 `del_by_endpoints()` 删除；
- 名字在进程内唯一，且删除时会清理注册表项。

### 4.5 控制面调用示例

```c
struct midr_gre_tunnel tun = {};

/* 显式命名 */
strlcpy(tun.ifname, "midr0", sizeof(tun.ifname));
tun.vrf_id = VRF_DEFAULT;
tun.local.ipa_type = IPADDR_V4;
tun.local.ipaddr_v4.s_addr = htonl(local_v4);
tun.remote.ipa_type = IPADDR_V4;
tun.remote.ipaddr_v4.s_addr = htonl(remote_v4);
tun.mtu = 1400;

if (midr_gre_interface_add(bgp, &tun) != 0)
        /* 错误处理 */;

/* 自动命名：ifname 留空即可 */
struct midr_gre_tunnel auto_tun = {
        .vrf_id = VRF_DEFAULT,
        .mtu = 1400,
};
auto_tun.local.ipa_type = IPADDR_V4;
auto_tun.local.ipaddr_v4.s_addr = htonl(local_v4);
auto_tun.remote.ipa_type = IPADDR_V4;
auto_tun.remote.ipaddr_v4.s_addr = htonl(remote_v4);

midr_gre_interface_add(bgp, &auto_tun);
/* 之后可查询实际名字 */
const char *nm = midr_gre_interface_name(VRF_DEFAULT, &auto_tun.local,
                                         &auto_tun.remote);

/* 删除 */
midr_gre_interface_del(bgp, "midr0");
midr_gre_interface_del_by_endpoints(bgp, VRF_DEFAULT, &auto_tun.local,
                                    &auto_tun.remote);
```

IPv6 隧道（自动选择 `ip6gre` kind，其余用法完全相同）：

```c
struct midr_gre_tunnel tun6 = {};

tun6.vrf_id = VRF_DEFAULT;
tun6.local.ipa_type = IPADDR_V6;
inet_pton(AF_INET6, "2001:db8::1", &tun6.local.ipaddr_v6);
tun6.remote.ipa_type = IPADDR_V6;
inet_pton(AF_INET6, "2001:db8::2", &tun6.remote.ipaddr_v6);
tun6.mtu = 1440;

midr_gre_interface_add(bgp, &tun6);          /* 自动生成 midr-gre-<n> */
midr_gre_interface_del_by_endpoints(bgp, VRF_DEFAULT, &tun6.local,
                                    &tun6.remote);
```

> `local` 与 `remote` 的地址族必须一致；zebra 会据此把
> `IFLA_INFO_KIND` 设为 `"ip6gre"`，内核以 `ip6_gre` 模块创建设备。

---

## 5. 编译与测试

### 5.1 构建

新增源文件同时登记在：

- `bgpd/subdir.am` → `bgpd_libbgp_a_SOURCES`（`bgp_midr_gre.c`）
- `bgpd/subdir.am` → 头文件列表（`bgp_midr_gre.h`）

由于 `subdir.am` 变化，**若 configure 时未开启 maintainer 模式**，需要手动
重新生成 Makefile：

```sh
automake                                  # 重新生成 Makefile.in
./config.status Makefile                  # 重新生成 Makefile
make -j"$(nproc)"
```

### 5.2 功能验证

在具备 `CAP_NET_ADMIN` 的 Linux 环境（如 frr-ubuntu24 容器）中：

```sh
# 1) 触发 CP 调用 midr_gre_interface_add(...)

# 2) 确认内核设备已创建（IPv4 / IPv6）
ip -d link show midr0
# 期望：<POINTOPOINT,NOARP> ... link/gre  <A> peer <B> ...
#        gre remote <B> local <A> ...
ip -d link show midr6
# 期望：<POINTOPOINT> ... link/gre6 <2001:db8::1> peer <2001:db8::2> ...
#        ip6gre remote <...> local <...> ...

# 3) zebra 视角
vtysh -c "show interface midr0"
vtysh -c "show interface midr6"

# 4) 触发 midr_gre_interface_del(...)
ip link show midr0     # 应报 "Device ... does not exist."
ip link show midr6
```

> 新建的 GRE 设备默认处于 `DOWN` 状态（与 `ip link add` 行为一致）。
> 若隧道需要转发流量，CP 还需把它拉起并配置地址/覆盖路由——这部分不在本接口
> 范围内，可复用 zebra 既有的接口 up/地址下发能力。

### 5.3 日志观察点

- bgpd：`MIDR GRE interface <name> created (local ... remote ...)`
- zebra（`debug zebra kernel`）：`GRE interface <name> add: local ... remote ...`

### 5.4 随码提供的 E2E 测试

`tests/bgpd/test_midr_gre_e2e.c` 是一个独立客户端，连到 zebra 后依次调用
`midr_gre_interface_add()` / `midr_gre_interface_del()` /
`midr_gre_interface_del_by_endpoints()`，并用 `ip link` 校验内核结果，覆盖：

1. 显式命名创建（`midr0`，local/remote/MTU 校验）；
2. IPv6 隧道创建/删除（`midr6`，校验内核 kind 为 `ip6gre`）；
3. 自动命名创建（`midr-gre-1`）与按端点删除；
4. 按名字删除。

编译方法（在 FRR 构建目录内，需链接 bgpd 的 MTYPE group）：

```sh
gcc -std=gnu11 -g -O0 -include config.h -I lib -I bgpd -I . \
    $(pkg-config --cflags libyang) \
    -o /tmp/test_midr_gre_e2e tests/bgpd/test_midr_gre_e2e.c \
    bgpd/bgp_midr_gre.o bgpd/bgp_memory.o \
    -L lib/.libs -lfrr -Wl,-rpath,$PWD/lib/.libs \
    -lcap -lcrypt -ljson-c -lrt -lpthread -lsqlite3 -lresolv \
    -ldl -lm -lfl -lyang -llua5.3
```

> 注：`bgp_midr_gre.c` 使用 `DEFINE_MTYPE_STATIC(BGPD, MIDR_GRE, ...)`，因此独立
> 链接时需要 `bgpd/bgp_memory.o` 提供 `_mg_BGPD`；在 `bgpd` 正常构建中该符号已存在。

### 5.5 验证状态

在 `frr-ubuntu24-ymy` 容器内用上述测试客户端实测（zebra 以 root 启动）：

| 用例 | 结果 |
|------|------|
| IPv4 显式命名创建 `midr0`（local/remote/MTU 校验） | ✅ PASS |
| IPv6 创建 `midr6`，内核 kind 为 `ip6gre` | ✅ PASS |
| IPv6 删除 `midr6` | ✅ PASS |
| 自动命名创建 `midr-gre-1` 并按端点删除 | ✅ PASS |
| 按名字删除 `midr0` | ✅ PASS |

`zebra/if_netlink.c` 重新编译无 warning/error；`bgp_midr_gre.o` 正常参与
`libbgp.a` / `bgpd` 的链接。

---

## 6. 限制与注意事项

1. **IPv6 GRE 已支持**：端点地址族为 IPv6 时自动使用 `ip6gre` kind，
   依赖内核 `ip6_gre` 模块（设备名以 `ip6gre*` 为前缀）。仅点对点 `ip6gre`
   被暴露；`ip6gretap`（二层）尚未提供，属于后续增强项。
2. **端点地址族必须一致**：`local` 与 `remote` 必须同为 IPv4 或同为 IPv6，
   `midr_gre_interface_add()` 会拒绝混用。
3. **删除语义**：`ZEBRA_GRE_DELETE` 只按名字/ifindex 删除，不做引用计数；
   若同一接口被多处引用，应由 CP 保证引用唯一性。
4. **删除是异步的**：zebra 收到请求后经 dplane → netlink 下发，设备的
   `RTM_DELLINK` 通知回来后 zebra 才会移除本地 interface 对象。
5. **注册表生命周期**：`midr_gre_fini()` 需要在 bgpd 退出（或 MIDR DP 关闭）时调用，
   用于释放注册表内存；它不会删除内核设备（设备生命周期由 CP 显式管理）。
6. **权限**：创建/删除 netdevice 需要 `CAP_NET_ADMIN`，容器需以 `--privileged`
   或相应 capability 运行，并确保 `ip_gre` / `ip6_gre` 模块可用。
7. **设备初始状态为 DOWN**：`ZEBRA_GRE_ADD` 只负责创建/配置设备本身，不会把它
   拉起，也不会配置隧道地址；CP 需另行处理（见 5.2 的说明）。
8. **ZAPI 命令号顺序**：`ZEBRA_GRE_ADD` / `ZEBRA_GRE_DELETE` 插入在
   `ZEBRA_GRE_SOURCE_SET` 之后，会使后续枚举值整体后移。由于该枚举是
   daemon 间协议，**升级时必须整体重编所有 daemon（含 vtysh）**，不可只替换
   单个二进制。

