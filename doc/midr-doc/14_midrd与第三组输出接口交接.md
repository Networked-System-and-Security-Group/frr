# midrd 与第三组路由安装接口交接

## 1. 交接结论

第二组已在独立 `midrd` 中完成 SPF 结果缓存、generation 管理、新旧结果 diff、READY/NOT_READY 状态处理和旧式 Zebra 接口兼容门面。第三组不再消费 `midr_spf_results`，也不需要迁移旧 `bgp_midr_spf_install.c`。

第三组只需将原 `bgp_midr_zebra.c` 的 Zebra/FIB 实现迁入 `midrd`，实现 `struct midr_zebra_backend_ops` 并注册。正式公开头文件为：

```c
#include "midrd/midr-context.h"
#include "midrd/midr-zebra.h"
```

完整接口语义见《MIDR SPF 与路由安装接口规范》。

## 2. 责任边界

```text
第二组：committed TED -> SPF -> result cache -> route diff
                                                   |
                                      midr_zebra_* facade
                                                   |
第三组：                         pending/ZAPI -> Zebra -> Linux FIB
```

第二组负责：

- 从单个 committed TED generation 生成完整双栈结果；
- 缓存上一份 backend 已接受的 desired result；
- 计算 add、replace、delete；
- metric-only 变化生成 `DELETE + ADD`；
- backend 拒绝时 abort 本批并且不推进 generation；
- TED NOT_READY 时统一撤销旧 SPF 路由；
- 发送失败恢复时提供增量 resync，Zebra 重连时提供完整 replay。

第三组负责：

- 创建并持有独立 zclient、VRF、pending queue、installed hash 和 timer；
- 实现 BASIC、ECMP、UCMP 和 SRv6 ZAPI 编码；
- 深拷贝 add 输入；
- 实现 batch abort、deferred submit 和立即 flush；
- ZAPI 失败时不错误更新 installed hash；
- Zebra 重连、发送失败重试和 shutdown 清理。

## 3. 第三组需要实现的 Backend

```c
struct midr_zebra_backend_ops {
    int (*route_add)(void *arg,
                     const struct prefix *prefix,
                     const struct midr_path_result *result);
    int (*route_del)(void *arg,
                     const struct prefix *prefix,
                     uint8_t instance);
    int (*update_deferred)(void *arg);
    int (*flush)(void *arg);
    void (*abort_pending)(void *arg);
};
```

初始化完成后注册：

```c
ret = midr_zebra_backend_register(ctx, &ops, backend);
```

注册会自动订阅 SPF/TED，并在存在 READY 结果时立即发出首个完整 diff。退出时按以下顺序处理：

```c
ret = midr_zebra_backend_unregister(ctx);
/* unregister 已执行 SPF 撤销和立即 flush */
destroy_backend(backend);
```

backend 对象、ops 表和 zclient 必须保持有效，直到 unregister 返回。

## 4. 旧代码迁移映射

第三组现有实现可以按下表迁移：

| 旧代码 | 新位置/形式 |
| --- | --- |
| `struct bgp_midr_dp` | 第三组 backend 私有 runtime |
| `midr_zebra_route_add()` 主体 | `ops.route_add` |
| `midr_zebra_route_del()` 主体 | `ops.route_del` |
| `midr_zebra_route_update_deferred()` 主体 | `ops.update_deferred` |
| `midr_zebra_route_flush()` 主体 | `ops.flush` |
| 清空本轮 pending ops | `ops.abort_pending` |
| `bgp->midr_dp` | backend `arg` |
| `bgp->vrf_id` | backend 自有 `vrf_id` |
| `bgp_zclient` | `midrd` 自有 zclient |
| `bm->master` | `midrd` FRR event loop |

可以直接复用：

- pending op 和 installed entry 结构；
- prefix+instance hash key；
- add/update/delete 去重；
- 100 ms deferred timer；
- `zapi_route` / `zapi_nexthop` 编码；
- BASIC、ECMP、UCMP 和 SRv6 模式选择；
- `ZEBRA_ROUTE_BGP_MIDR`、管理距离、递归解析和 dual-instance 逻辑。

不需要迁移：

- `bgp_midr_spf_install.c`；
- SPF result set 遍历、引用和 diff；
- TED consumer、READY/NOT_READY 判断；
- generation 缓存。

## 5. 唯一必要的数据结构调整

`struct midr_path` 的字段整体保持旧接口，只有 nexthop 类型发生变化：

```c
/* 旧 */
union g_addr nexthop;

/* 新 */
struct ipaddr nexthop;
```

旧编码函数中按目的前缀 family 读取 union 的逻辑，改成按 `nexthop.ipa_type` 读取：

```c
switch (path->nexthop.ipa_type) {
case IPADDR_V4:
    /* path->nexthop.ipaddr_v4 */
    break;
case IPADDR_V6:
    /* path->nexthop.ipaddr_v6 */
    break;
default:
    return -EAFNOSUPPORT;
}
```

其余字段继续为 `ifindex`、`metric`、`path_avail_bw`、`weight`、`path_count`、SID list 和 `instance`。这项调整使用 FRR 通用类型，不引入 BGP 依赖。

## 6. 错误与 installed 状态

旧实现无条件更新 installed hash 的路径不能原样保留。新的硬性规则为：

```text
成功编码并被 zclient 接受
  -> 才能更新 installed hash

编码、排队或发送失败
  -> 返回真实错误
  -> 不更新 installed hash
  -> 保留/恢复 pending 状态
  -> 发送恢复后调用 midr_spf_install_resync(ctx)

Zebra 连接重建
  -> 清空 backend 的旧 installed hash
  -> 调用 midr_spf_install_replay(ctx) 完整重放
```

adapter 在 diff 中任何一步失败都会调用 `abort_pending`，并保持上一 desired generation。`abort_pending` 必须清除本轮尚未提交的全部操作，不能留下半批 add/delete。

## 7. READY 与 NOT_READY

第三组不再自行选择 NOT_READY 策略：

- 临时输入中断但旧 committed TED 仍为 READY：不调用第三组，旧 FIB 保持不变。
- 新 READY generation：第二组生成完整 diff。
- TED 明确 NOT_READY：第二组生成全量 SPF route delete。
- TED 恢复 READY：第二组重新生成完整 add/update diff。

第三组只执行收到的 route 操作，不读取 TED 状态，也不把旧 installed set 标成新的 generation。

## 8. 第三组迁移完成条件

- backend 可以在 `midrd` 内初始化、注册、注销和销毁；
- 不包含 `bgpd` 私有头、BGP 实例、BGP peer 或 BGP RIB 依赖；
- IPv4/IPv6 add、update、delete 进入 Zebra RIB 和 Linux FIB；
- BASIC、ECMP、UCMP、SRv6 与 SPF/TE instance 行为正确；
- metric-only 更新实际替换旧路由；
- ZAPI 失败不污染 installed hash；
- abort 不提交半批结果；
- 发送失败后调用 resync，Zebra 重连后清空旧 installed hash 并调用 replay；
- shutdown 撤销和 flush 完成后才销毁 zclient；
- 不启动 `bgpd` 时完整运行。

第二组现有定向测试已经覆盖 facade、双栈转换、diff、失败 abort、generation 保持、恢复 resync、同 generation 完整 replay、UCMP/SRv6 载荷、NOT_READY 撤销和注销 flush。第三组接入后只需补真实 zclient/Zebra/FIB 侧测试。
