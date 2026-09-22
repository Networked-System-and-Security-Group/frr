# dp-01 迁移总览

## 1. 结论

第三组把旧 `bgpd/bgp_midr_zebra.c` 的 Zebra/FIB 实现迁入独立 `midrd`，
实现接口规范定义的 `struct midr_zebra_backend_ops` 并注册；GRE 支撑从数据面
分支 `f8f4e81f2f` 逐字复制进通用 libfrr/zebra 公共层。旧 `bgpd` 实现保持
不变，作为差分对照。

## 2. 旧 → 新映射

### 2.1 控制流与宿主

| 旧（`bgpd/bgp_midr_zebra.c`，865 行） | 新（`midrd/midr-dp-backend.c`，1043 行） |
| --- | --- |
| `struct bgp_midr_dp`（旧文件第 149 行） | backend 私有 runtime `struct midr_dp_backend`（`midr-dp-backend.c` 第 99 行），经 `ops` 的 `arg` 传入 |
| `midr_zebra_route_add()` 主体 | `ops.route_add` → `dp_route_add()`（第 765 行） |
| `midr_zebra_route_del()` 主体 | `ops.route_del` → `dp_route_del()`（第 799 行） |
| `midr_zebra_route_update_deferred()` 主体 | `ops.update_deferred` → `dp_route_update_deferred()`（第 827 行） |
| `midr_zebra_route_flush()` 主体 | `ops.flush` → `dp_route_flush()`（第 851 行） |
| 清空本轮 pending ops | `ops.abort_pending` → `dp_route_abort_pending()`（第 862 行） |
| `bgp->midr_dp` | backend `arg` |
| `bgp->vrf_id` | backend 自有 `vrf_id`（`b->vrf_id`） |
| `bgp_zclient`（旧文件第 73 行 extern） | `midrd` 自有 zclient（`b->zc`，`zclient_new()` 第 930 行） |
| `bm->master`（旧文件第 841 行） | `midrd` FRR event loop（`midr_dp_backend_start(ctx, master, vrf_id)`） |
| `union g_addr nexthop` | `struct ipaddr`，按 `nexthop.ipa_type` 读取 |
| 六个无返回值入口（`route_add/del/update_deferred/flush/init/fini`） | 五个返回 `int` 的 backend ops（init/fini 由 `midr_dp_backend_start/stop` 承担） |

旧公开入口签名见 `bgpd/bgp_midr_zebra.h` 第 186/195/207/217/223/229 行；
新契约见 `doc/midr-doc/MIDR-TED路径计算接口规范.md` 第 88-107 行。

### 2.2 可直接复用

- pending op 与 installed entry 结构（`struct midr_pending_op`、`struct midr_installed_entry`）；
- prefix+instance hash key（`dp_prefix_hash_key` / `dp_prefix_cmp`）；
- add/update/delete 去重（`dp_process_one_op`，第 496 行）；
- 100 ms deferred timer（`MIDR_BATCH_INTERVAL_MS`，第 61 行）；
- `zapi_route`/`zapi_nexthop` 编码（`dp_result_to_zapi`，第 367 行）；
- BASIC/ECMP/UCMP/SRv6 模式选择；
- `ZEBRA_ROUTE_BGP_MIDR`、管理距离、递归解析、双 instance 逻辑。

### 2.3 不需要迁移

- `bgp_midr_spf_install.c`（新职责由 `midrd/midr-spf-install.c` 承接）；
- SPF result set 遍历、引用与 diff；
- TED consumer、READY/NOT_READY 判断；
- generation 缓存。

## 3. 文件清单

### 3.1 新增/修改的 midrd 源（宿主仓库 `/Users/yangmengyu/githubdocuments/frr`）

| 文件 | 行数 | 状态 | 说明 |
| --- | ---: | --- | --- |
| `midrd/midr-dp-backend.c` | 1043 | 新增 | 第三组 backend：五 ops、ZAPI 编码、pending/installed、重试与 resync、重连 replay |
| `midrd/midr-dp-backend.h` | 76 | 新增 | `midr_dp_backend_start/stop`、`midr_dp_backend_zclient/vrf_id`、`struct midr_dp_status` |
| `midrd/midr-gre.c` | 709 | 新增 | GRE 下发（bgpd→midrd 移植），复用后端 zclient |
| `midrd/midr-gre.h` | 220 | 新增 | `struct midr_gre_tunnel`、`struct midr_gre_status`、`midr_gre_interface_*`、`midr_gre_init/fini` |
| `midrd/dp-backend-test.c` | 602 | 新增 | 包裹 `zclient_route_send` 的单测 |
| `midrd/dp-e2e-tool.c` | 423 | 新增 | group-3 路由安装 harness（`midrd-dp-e2e-tool`），驱动公开 facade；模式见 `dp-03` 第 2 节 |
| `midrd/gre-link-tool.c` | 238 | 新增 | GRE 连通性测试用微型 midrd 宿主（`midrd-gre-tool`），见 `dp-04` 第 5 节 |
| `midrd/r7-dp-zapi-fib.sh` | 369 | 新增 | group-3 隔离真实 ZAPI/zebra-rib/Linux-FIB 测试（PASS=29） |
| `midrd/midr-gre-connectivity-test.sh` | 263 | 新增 | 双容器 GRE/ip6gre 连通性测试（PASS=24） |
| `midrd/r7-dp-e2e-zapi.sh` | 309 | 新增 | 单容器 3×midrd + zebra 端到端；**阻塞（第二组会话层）** |
| `midrd/r7-dp-integration-smoke.sh` | 275 | 新增 | 三节点 containerlab 集成；**阻塞（第二组会话层）** |
| `midrd/Makefile` | 309+ | 修改 | 新增 `DP_OBJECTS`、三个 harness/test 目标（`midrd-dp-test`/`midrd-gre-tool`/`midrd-dp-e2e-tool`）与 libfrr 头的 `-Wno-error` 说明 |
| `Makefile.am` | — | 修改 | `midrd_midrd_SOURCES` 增列 `midrd/midr-dp-backend.{c,h}`、`midrd/midr-gre.{c,h}`（第 157-158 行） |
| `midrd/midrd.c` | 3364 | 修改 | `--zserv-path/--vrf-id/--no-zebra`、context 初始化后启动后端、shutdown 顺序 |
| `midrd/extraction-boundary-test.sh` | 60 | 修改 | 两阶段边界编译 |

> 按任务约束，本文档只描述上述文件；第三组不修改第二组的 SPF/diff/generation 代码。

### 3.2 从数据面分支 `f8f4e81f2f` 逐字复制的公共层

`git show --stat f8f4e81f2f`（兄弟检出 `frr/frr`）显示该提交共改 17 个文件、+2982/-7。
本次迁移复制其中的 GRE 支撑：

| 文件 | 关键内容 | 位置 |
| --- | --- | --- |
| `lib/log.c` | `DESC_ENTRY(ZEBRA_GRE_ADD/DELETE)` | 第 460-461 行 |
| `lib/zclient.h` | `ZEBRA_GRE_ADD`/`ZEBRA_GRE_DELETE`（第 233-234 行）、`struct zclient_gre_if`（第 1443 行）、`zclient_send_gre_add/delete`（第 1468/1471 行） | — |
| `lib/zclient.c` | `zclient_send_gre_add`（第 5324 行）、`zclient_send_gre_delete`（第 5361 行） | — |
| `zebra/zapi_msg.c` | `zebra_gre_add`（第 4133 行）、`zebra_gre_delete`（第 4198 行）、命令表（第 4345-4346 行） | — |
| `zebra/zebra_dplane.h` | `DPLANE_OP_GRE_ADD/DELETE`（第 191-192 行）、`dplane_gre_interface_add/delete`（第 1115/1126 行） | — |
| `zebra/zebra_dplane.c` | `dplane_gre_interface_add`（第 6419 行）、`dplane_gre_interface_delete`（第 6432 行） | — |
| `zebra/if_netlink.c` | GRE 编码器 `netlink_gre_set_msg_encoder`（第 367 行）：DELETE → `RTM_DELLINK`（第 400 行）、ADD → `RTM_NEWLINK`（第 416 行）；`IFLA_GRE_*` 属性（第 460-493 行） | — |
| `zebra/kernel_netlink.c` | `DPLANE_OP_GRE_ADD/DELETE` → `netlink_put_gre_set_msg`（第 1480-1483 行） | — |

## 4. 关键语义：installed 状态与 deferred-batch 恢复

### 4.1 只有成功发送才更新 installed hash

```text
成功编码并被 zclient 接受
  -> 才能更新 installed hash
编码、排队或发送失败
  -> 返回真实错误
  -> 不更新 installed hash
  -> 保留/恢复 pending 状态
  -> 发送恢复后调用 midr_spf_install_resync(ctx)
Zebra 连接重建
  -> 清空 backend 旧 installed hash
  -> 调用 midr_spf_install_replay(ctx) 完整重放
```

代码依据：`dp_process_one_op()` 在 `dp_zapi_send()` 成功后才
`dp_installed_set()`（第 525-531 行）；DELETE 同理（第 540-546 行）；
`dp_zebra_connected()` 先 `dp_installed_clear()` 再 replay（第 729-750 行）。

### 4.2 deferred-batch 失败恢复（两步，已修复的真实缺陷）

`update_deferred()` 已返回成功后，延迟批仍可能在定时器回调里发送失败；
此时 adapter 看不到错误、且已推进 desired generation，因此单纯的
`midr_spf_install_resync()` 是 no-op，FIB 会残留旧 metric（观测到
`DEBUG captures=2 ... resyncs=1`）。修复后的恢复由 `dp_recovery_cb()` 分两步：

1. **先重试保留的 pending 批**——队列中失败项及其后项即“已接受结果与当前
   结果的未应用余量”，重提即幂等收敛 FIB，无需 adapter 重建；
2. **批应用成功（或队列为空）后**再调用 `midr_spf_install_resync()` 作为
   规范可见的对账；当 adapter 已达 desired generation 时它是 no-op。

重试有界（`MIDR_DP_RECOVERY_MAX_ATTEMPTS = 50`，间隔
`MIDR_DP_RECOVERY_MS = 100` ms）。zebra 不可达时恢复回调不再重排，收敛交给
zebra 重连路径（清 installed hash + `midr_spf_install_replay()`）。依据：
`midr-dp-backend.c` 第 21-32、657-722 行；缺陷与修复覆盖见
`midrd/dp-backend-test.c` 的 `test_adapter_pipeline_and_recovery()`。

`midrd/dp-backend-test.c` 的 `test_adapter_pipeline_and_recovery()` 断言：
`capture_count == 4`（captures[2] DELETE、captures[3] ADD）、
`send_failures == 1`、`pending == 0`、`installed == 1`、`last_error == 0`、
`resyncs == 1`（第 481-512 行）。

## 5. 边界规则

- `midrd` 可使用通用 libfrr 设施（daemon 生命周期、event/stream/buffer、
  `prefix`/`ipaddr`/`ifindex_t`、公共 `zclient`/ZAPI）。
- 禁止出现 `bgpd` 私有头、`struct bgp`/`struct peer`/`struct
  bgp_path_info`、BGP OPEN/UPDATE/FSM/Capability、AFI/SAFI 状态、zebra
  daemon 私有头或进程内 zebra 状态。
- 边界门禁 `midrd/extraction-boundary-test.sh`（60 行）对 `midrd/*.c`、`midrd/*.h`
  执行两阶段扫描（第 8-17、49、59 行）：
  1. 源级 grep 禁止 `#include <bgpd|bgp_|zebra/>` 及 BGP 类型；
  2. `contract.c` 用严格 `-Werror` 编译纯 MIDR 公开头；
  3. `dp-contract.c` 编译第三组头 `midr-dp-backend.h`、`midr-gre.h`
     （第三组头经 libfrr `vrf.h → vty.h` 拉入匿名结构体成员，触发 GCC
     默认开启且无 `-W` 开关的警告，故该翻译单元放宽 `-Werror`；FRR 顶层
     构建仍是权威严格构建）。

## 6. 证据

| 项 | 依据 |
| --- | --- |
| 旧实现行数与入口 | 兄弟检出 `frr/frr`（分支 `feat/te-dp-interface`，HEAD `f8f4e81f2f`）的 `bgpd/bgp_midr_zebra.{c,h}`、`bgpd/bgp_midr_gre.{c,h}` |
| 公共层 GRE 支撑 | 同上仓库 `lib/log.c`、`lib/zclient.{c,h}`、`zebra/zapi_msg.c`、`zebra/zebra_dplane.{c,h}`、`zebra/if_netlink.c`、`zebra/kernel_netlink.c` |
| 新实现 | `midrd/midr-dp-backend.{c,h}`、`midrd/midr-gre.{c,h}`、`midrd/dp-backend-test.c`、`midrd/Makefile`、`Makefile.am`、`midrd/midrd.c` |
| 接口契约 | `doc/midr-doc/14_midrd与第三组输出接口交接.md`、`doc/midr-doc/MIDR-TED路径计算接口规范.md` |
