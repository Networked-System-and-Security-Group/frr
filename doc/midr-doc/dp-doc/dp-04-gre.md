# dp-04 GRE 虚拟接口

## 1. 结论

`midrd/midr-gre.{c,h}` 是 MIDR 控制面创建/删除 GRE 虚拟接口的唯一入口，由
旧 `bgpd/bgp_midr_gre.c` 移植而来。它复用数据面后端的 zclient，经
`ZEBRA_GRE_ADD/DELETE` 交给 zebra，最终由 netlink 生成 `RTM_NEWLINK` /
`RTM_DELLINK`。真实双容器连通性已实测通过（**PASS=24 FAIL=0**），见第 5 节。

## 2. 范围

- `midrd` GRE API 与状态机。
- ZAPI 线格式路径（`lib/zclient.{c,h}` → `zebra/…`）。
- `gre-link-tool` + `midr-gre-connectivity-test.sh` 测试流程（已实测 PASS=24）。
- 与旧 `bgpd` 版本的差异。

## 3. midrd GRE API

### 3.1 输入结构

```c
struct midr_gre_tunnel {
    char ifname[IFNAMSIZ];   /* 留空 => 自动生成 "midr-gre-<n>" */
    vrf_id_t vrf_id;         /* VRF_UNKNOWN => 用后端 VRF */
    struct ipaddr local;     /* IFLA_GRE_LOCAL  */
    struct ipaddr remote;    /* IFLA_GRE_REMOTE */
    ifindex_t link_ifindex;  /* 0 => 内核自行解析 */
    uint32_t ikey, okey;     /* 可选 GRE key */
    uint16_t encap_flags;    /* 可选校验和标志 */
    uint32_t mtu;            /* 0 => 内核默认 */
};
```

### 3.2 建立状态

```text
MIDR_GRE_STATE_DOWN     无请求或设备已删除
MIDR_GRE_STATE_PENDING  请求已发，等待内核确认
MIDR_GRE_STATE_UP       zebra 已上报设备，ifindex 有效
MIDR_GRE_STATE_FAILED  被拒绝或确认超时（见 err）
```

`struct midr_gre_status` 含 `state`、`ifname`、`vrf_id`、`ifindex`、`iftype`、
`err`、`up_ms`。

### 3.3 函数签名（`midrd/midr-gre.h`）

```c
extern int  midr_gre_interface_add(struct midr_context *ctx,
                                   const struct midr_gre_tunnel *tun,
                                   struct midr_gre_status *status);
extern int  midr_gre_interface_del(struct midr_context *ctx, const char *ifname,
                                   struct midr_gre_status *status);
extern int  midr_gre_interface_del_by_endpoints(struct midr_context *ctx,
                                   vrf_id_t vrf_id,
                                   const struct ipaddr *local,
                                   const struct ipaddr *remote,
                                   struct midr_gre_status *status);
extern int  midr_gre_interface_get_state(vrf_id_t vrf_id, const char *ifname,
                                   struct midr_gre_status *status);
extern bool midr_gre_interface_wait_up(struct midr_context *ctx,
                                   const char *ifname, uint32_t timeout_ms,
                                   struct midr_gre_status *status);
extern const char *midr_gre_interface_name(vrf_id_t vrf_id,
                                   const struct ipaddr *local,
                                   const struct ipaddr *remote);
extern void midr_gre_register_notify(midr_gre_notify_cb cb, void *arg);
extern void midr_gre_unregister_notify(midr_gre_notify_cb cb);
extern const char *midr_gre_state_str(enum midr_gre_state state);
extern void midr_gre_init(struct event_loop *master);
extern void midr_gre_fini(void);
```

返回约定：`add`/`del`/`del_by_endpoints` 成功返回 0，失败返回 -1
（`status->err` 带 errno 风格原因：`EINVAL`/`EAFNOSUPPORT`/`ENOTCONN`/`EIO`/
`ETIMEDOUT`）。`add` 返回 0 仅表示请求被受理（UP 或 PENDING），**不等于**
设备已建立——须用 `wait_up()` 或回调确认。

### 3.4 确认时序

- 请求发出后进入 PENDING；`midr_gre_confirm_start()` 以
  `MIDR_GRE_CONFIRM_INTERVAL_MS = 100` ms 轮询 zebra 接口视图；
- `MIDR_GRE_CONFIRM_TIMEOUT_MS = 3000` ms 内命中 `if_lookup_by_name()` 则
  PENDING→UP，记录 `ifindex`/`iftype`；超时则 FAILED（`err = ETIMEDOUT`）；
- 若请求时设备已存在，直接返回 UP（`midr_gre_entry_check_present()`）。
- 依据：`midrd/midr-gre.c` 第 38-40、379-430 行。

## 4. ZAPI 线格式路径

```text
midr_gre_interface_add()/del()            (midrd/midr-gre.c)
  │  填充 struct zclient_gre_if
  ▼
zclient_send_gre_add() / zclient_send_gre_delete()   (lib/zclient.c 第 5324/5361 行)
  │  命令 ZEBRA_GRE_ADD / ZEBRA_GRE_DELETE
  ▼
zebra_gre_add() / zebra_gre_delete()      (zebra/zapi_msg.c 第 4133/4198 行)
  │  解析流：ifname + 两端点 family/addr + link_ifindex + ikey/okey
  │  + encap_flags + mtu
  ▼
dplane_gre_interface_add() / _delete()    (zebra/zebra_dplane.c 第 6419/6432 行)
  │  DPLANE_OP_GRE_ADD / DPLANE_OP_GRE_DELETE
  ▼
netlink_put_gre_set_msg()                 (zebra/kernel_netlink.c 第 1480-1483 行)
  ▼
netlink_gre_set_msg_encoder()             (zebra/if_netlink.c 第 367 行)
  │  ADD    -> RTM_NEWLINK  (+ NLM_F_CREATE|EXCL, 第 416/420 行)
  │  DELETE -> RTM_DELLINK  (优先内核 ifindex, 第 399-400 行)
  │  属性 IFLA_GRE_LOCAL/REMOTE/LINK/IKEY/OKEY/ENCAP_FLAGS (第 460-493 行)
  ▼
内核 netdevice
```

相关常量与结构：
- `lib/zclient.h`：`ZEBRA_GRE_ADD`/`ZEBRA_GRE_DELETE`（第 233-234 行）、
  `struct zclient_gre_if`（第 1443 行）、`zclient_send_gre_add/delete`
  （第 1468/1471 行）。
- `zebra/zebra_dplane.h`：`DPLANE_OP_GRE_ADD/DELETE`（第 191-192 行）、
  `dplane_gre_interface_add/delete`（第 1115/1126 行）——`ADD` 与 `SET` 在
  dplane 层共用 `netlink_put_gre_set_msg()`，由 op 决定 `nlmsg_type`。
- 设备 kind 选择由 `netlink_gre_link_kind()`（`if_netlink.c` 第 357 行）按
  端点地址族产生 `gre` / `ip6gre`。

## 5. 工具与脚本测试流程

工具：`midrd/gre-link-tool.c`（构建产物 `midrd-gre-tool`，见 `midrd/Makefile`
的 `midrd-gre-tool` 目标）。它是一个“微型 midrd 宿主”，用 `#include "midrd.c"` 技巧拿到
私有 `struct midr_context`，并搭建 `event loop → TED → data-plane backend
(zclient) → GRE module` 同一路径；不自建 zclient。

```sh
# setup|teardown，选项：--sock --name --local --remote --mtu --wait-ms
stage/midrd-gre-tool setup \
    --sock /tmp/zserv.api --name midr0 \
    --local 10.1.1.11 --remote 10.1.1.12 --mtu 1400 --wait-ms 3000
# 机器可读输出: MIDR_GRE name=<n> state=<up|down|pending|failed> ifindex=<n> err=<n>
# 退出码: setup 达到 UP、teardown 达到 DOWN 为 0，否则非 0

stage/midrd-gre-tool teardown --sock /tmp/zserv.api --name midr0
```

> 工具只描述隧道端点/MTU（`struct midr_gre_tunnel`），不含 overlay 地址或
> 覆盖路由——这些由 CP 负责。旧 `test_midr_gre_link` 的 `--ip/--ip6` 选项
> 在新工具中不存在；overlay 地址须在脚本里用 `ip addr add` 配置。

脚本 `midrd/midr-gre-connectivity-test.sh`（263 行）在 docker 宿主上执行：

```sh
# 前置：node1/node2 已存在并运行 frr-ubuntu24-ymy:init，且在同一 bridge
bash midrd/midr-gre-connectivity-test.sh
```

**实测结果（协调者今日执行）：PASS=24 FAIL=0。** 日志 `/tmp/gre-run/test2.log`
（宿主）。脚本把构建容器的源码树 `midrd/gre-link-tool.c` 的产物
`midrd-gre-tool` 与新构建的 zebra staged 到 `node1`/`node2` 的 `/opt/midr-dp`。
覆盖：IPv4 GRE（`gre`）与 IPv6 GRE（`ip6gre`）均建立（`state=up` 且回报
`ifindex`），`ip -d link show` kind 正确，IPv4 overlay 双向 ping，IPv6 overlay
over IPv4 隧道双向 ping，重复 add / 状态查询幂等，teardown 删除全部接口。

| 项 | 值 |
| --- | --- |
| 前置 | 两容器各运行 zebra；`/opt/midr-dp` 含 zebra + libfrr + `midrd-gre-tool`；root 在 `frrvty` 组；IPv6 用例前 `sysctl net.ipv6.conf.eth0.disable_ipv6=0` |
| 命令 | `bash midrd/midr-gre-connectivity-test.sh`（docker 宿主，root） |
| 结果 | PASS=24 FAIL=0（旧版同类脚本为 PASS=21） |
| 日志 | 宿主 `/tmp/gre-run/test2.log` |
| 状态 | 已验证 |

注意事项：删除请求发出后短命客户端不要立即退出（保持约 1 s），否则 zebra
可能在同一次读中同时读到数据与 EOF 而丢弃请求（旧报告已记录）。

## 6. 与旧 bgpd 版本的差异

| 维度 | 旧 `bgpd/bgp_midr_gre.{c,h}`（693/208 行） | 新 `midrd/midr-gre.{c,h}`（709/220 行） |
| --- | --- | --- |
| 宿主参数 | `struct bgp *bgp`（`bgp_midr_gre.h` 第 150/159/167/187 行） | `struct midr_context *ctx`（`midr-gre.h` 第 155/164/172/192 行） |
| zclient | 全局单例 `bgp_zclient` | 共享数据面后端 zclient：`midr_dp_backend_zclient()` |
| 初始化 | 无独立 init（随 bgpd） | `midr_gre_init(master)`，**必须在后端 `midr_dp_backend_start()` 之后**调用（`midrd.c` 第 3283 行） |
| VRF 缺省 | `VRF_UNKNOWN` → BGP 实例 VRF | `VRF_UNKNOWN` → `midr_dp_backend_vrf_id()`（`midr-gre.c` 第 385-386 行） |
| 事件循环 | `bm->master` | `midr_gre_init()` 传入的 `struct event_loop *` |
| 卸载顺序 | 随 bgpd 退出 | `midr_gre_fini()` 先于 `midr_dp_backend_stop()`（`midrd.c` 第 3024-3028 行），因为 GRE 借用后端 zclient |
| 接口语义 | 不变 | `state`/`ifindex`/回调/`wait_up()`/自动命名/幂等 create 均保持 |

## 7. 证据

| 项 | 依据 |
| --- | --- |
| 新 API 签名 | `midrd/midr-gre.h` 第 67-90、105-141、155-214 行 |
| 新实现时序 | `midrd/midr-gre.c` 第 32-40、88-91、373-430、687-709 行 |
| 共享 zclient | `midrd/midr-gre.c` 第 32-35 行 → `midrd/midr-dp-backend.c` 第 927 行 |
| ZAPI 路径 | `lib/zclient.{c,h}`、`zebra/zapi_msg.c`、`zebra/zebra_dplane.{c,h}`、`zebra/if_netlink.c`、`zebra/kernel_netlink.c` |
| 旧版本 | 兄弟检出 `frr/frr`（HEAD `f8f4e81f2f`）的 `bgpd/bgp_midr_gre.{c,h}` |
| 旧测试基线 | `frr/frr/doc/midr-doc/midr-gre-test-report.md`（21/21 PASS）、`frr/frr/doc/midr-doc/midr-gre-interface-api.md` |
| 新测试（组件层） | `midrd/dp-backend-test.c` `test_gre_api()`（校验路径） |
| 新测试（真实双容器） | `midrd/midr-gre-connectivity-test.sh` PASS=24 FAIL=0；宿主日志 `/tmp/gre-run/test2.log` |

