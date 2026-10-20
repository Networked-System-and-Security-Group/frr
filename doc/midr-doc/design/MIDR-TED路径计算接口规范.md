# MIDR SPF 与路由安装接口规范

## 1. 范围与职责

本文定义 `midrd` 内路径计算模块与第三组 Zebra/FIB 模块之间的正式接口。公开头文件为：

```c
#include "midrd/midr-context.h"
#include "midrd/midr-zebra.h"
```

第二组负责从 committed TED generation 计算完整 IPv4/IPv6 SPF 结果，维护上一份已接受结果，完成新旧结果 diff，并将确定的 add、update、delete 操作交给第三组。第三组负责 pending queue、zclient/ZAPI、installed hash、Zebra 重连和 Linux FIB，不读取 canonical、LSDB staging、TED 私有内存或 SPF result set。

```text
owned -> canonical -> scope -> LSDB -> committed TED -> SPF
                                                        |
                                             result diff / generation
                                                        |
                                      midr_zebra_* compatibility facade
                                                        |
                           pending -> ZAPI -> Zebra RIB -> Linux FIB
```

## 2. 与旧 bgpd 接口的对应关系

旧 `bgpd` 实现中的以下业务入口继续保留：

```c
midr_zebra_route_add()
midr_zebra_route_del()
midr_zebra_route_update_deferred()
midr_zebra_route_flush()
```

宿主参数由 BGP 实例替换为独立的 `struct midr_context *`，函数由无返回值改为返回 `int`，使排队、提交和发送失败能够被上游观察。第三组原有实现迁移为 `struct midr_zebra_backend_ops` 回调；兼容门面继续使用上述函数名调用 backend。

旧 `bgp_midr_spf_install.c` 的职责已经由 `midrd/midr-spf-install.c` 承接。第三组不再实现 `midr_spf_install_results()`，也不再自行遍历 immutable SPF result set。

## 3. 路径结果数据结构

### 3.1 Path

```c
struct midr_path {
    struct ipaddr nexthop;
    uint32_t ifindex;
    uint32_t metric;
    float path_avail_bw;
    uint8_t weight;
};
```

字段名和语义保持旧接口不变。唯一的类型调整是将无地址族标签的 `union g_addr` 改为 FRR 通用 `struct ipaddr`。第三组通过 `nexthop.ipa_type` 判断 IPv4/IPv6，再读取 `ipaddr_v4` 或 `ipaddr_v6`；不再根据目的前缀猜测 nexthop 地址族。

`ifindex == 0` 表示允许 Zebra 递归解析；IPv6 link-local nexthop 必须携带有效接口索引。`weight == 0` 表示 BASIC/ECMP，非零值表示 UCMP 权重。`path_avail_bw` 保持旧接口单位与用途，基础 SPF 当前填 `0.0f`，专用 UCMP 计算模块可以明确填写。

### 3.2 Path Result

```c
#define MIDR_INSTANCE_SPF 0
#define MIDR_INSTANCE_TE  1
#define MIDR_SRV6_MAX_SEGS 8

struct midr_path_result {
    struct midr_path *paths;
    uint8_t path_count;
    struct {
        struct in6_addr sid_list[MIDR_SRV6_MAX_SEGS];
        uint8_t sid_count;
    } explicit;
    uint8_t instance;
};
```

该结构继续同时承载 BASIC、ECMP、UCMP 和 SRv6：

| 模式 | 判定 |
| --- | --- |
| BASIC | `path_count == 1`、`sid_count == 0` |
| ECMP | `path_count > 1`、所有 `weight == 0` |
| UCMP | `path_count > 1`、至少一个 `weight > 0` |
| SRv6 | `sid_count > 0` |

`instance == MIDR_INSTANCE_SPF` 对应普通 SPF；`instance == MIDR_INSTANCE_TE` 对应 TE/SRv6 override。调用 `route_add` 时 backend 必须在返回前深拷贝 paths 和 SID list。

## 4. Zebra Backend 注册

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

int midr_zebra_backend_register(
    struct midr_context *ctx,
    const struct midr_zebra_backend_ops *ops,
    void *arg);

int midr_zebra_backend_unregister(struct midr_context *ctx);
```

注册成功会自动启动 SPF 安装 adapter，订阅 committed TED 变化，并立即同步当前 READY 结果。注册前 TED 可以处于 NOT_READY；首次 READY 后自动下发完整结果。注销时 adapter 先撤销已接受的 SPF 路由并执行立即 flush，再解除订阅。backend 私有状态必须在 `midr_zebra_backend_unregister()` 返回后再释放。

注册的 ops 和 arg 在注销前必须保持有效。所有调用均发生在 `midrd` event thread，不要求跨线程锁。

## 5. 兼容调用接口

```c
int midr_zebra_route_add(
    struct midr_context *ctx,
    const struct prefix *prefix,
    const struct midr_path_result *result);

int midr_zebra_route_del(
    struct midr_context *ctx,
    const struct prefix *prefix,
    uint8_t instance);

int midr_zebra_route_update_deferred(struct midr_context *ctx);
int midr_zebra_route_flush(struct midr_context *ctx);
void midr_zebra_route_abort(struct midr_context *ctx);
```

`route_add` 和 `route_del` 只将操作加入当前 pending batch，不直接把 candidate 标记为 installed。`update_deferred` 接受当前 batch 并启动正常延迟提交；`flush` 立即提交；`route_abort` 丢弃本次尚未成功提交的全部操作。

未注册 backend 时返回 `-ENOSYS`。参数、instance、地址族或 SID 数量非法时返回相应错误。backend 返回的真实错误必须原样传回。

## 6. Diff、Generation 与状态转换

第二组 adapter 按完整 result set 生成确定的路由差异：

```text
旧有、新无       -> DELETE
旧无、新有       -> ADD
nexthop 变化     -> ADD/replace
仅 metric 变化   -> DELETE + ADD
结果相同         -> no-op
不可达或本地前缀 -> 不安装；旧结果存在时 DELETE
```

metric-only 变化显式生成 `DELETE + ADD`，因为旧第三组 installed-hash 比较不包含 metric。基础 SPF 输出 `instance=SPF`、`sid_count=0`、`weight=0`；TE/UCMP 生产者可以通过相同兼容接口提交扩展结果。

只有完整 diff 被 backend 接受后，adapter 才推进 desired generation。任一 add/delete/deferred 操作失败时，adapter 调用 `abort_pending()`，保留上一 desired generation，并等待显式 resync 或新的 TED 通知。

```c
int midr_spf_install_resync(struct midr_context *ctx);
int midr_spf_install_replay(struct midr_context *ctx);
```

backend 从发送错误恢复后调用 `midr_spf_install_resync()`，adapter 会以最后一次接受的 desired result 为基线，重新生成到最新 committed SPF 结果的差异。Zebra 连接丢失后，backend 必须先清空本地旧 installed hash，再调用 `midr_spf_install_replay()`；replay 不受 generation 是否变化影响，会从空基线重发最新完整结果。

## 7. READY、NOT_READY 与撤销

- TED 保持 READY 且旧 committed 视图仍有效时，临时 Provider 中断不会触发撤销。
- 新 READY generation 提交后，adapter 获取完整结果并生成一次 diff。
- TED 明确进入 NOT_READY 时，adapter 统一执行 `old -> NULL`，撤销已接受的 SPF 路由。
- NOT_READY 撤销失败时不推进 desired 状态，错误可通过安装状态接口观察。
- TED 恢复 READY 后，从完整新结果重新安装，不接受半个 snapshot。

因此第三组不自行决定 NOT_READY 时保留还是撤销，也不解释 TED 错误；这一策略由第二组 adapter 统一执行。

## 8. ZAPI 与 installed 状态要求

第三组 backend 可以复用旧实现的 pending queue、100 ms deferred timer、installed hash、BASIC/ECMP/UCMP/SRv6 编码和双 instance 逻辑，但必须满足以下约束：

1. `route_add` 在返回前深拷贝输入。
2. `abort_pending` 能撤销本次未提交 diff，不能留下半批操作。
3. `zclient_route_send()` 返回失败时不得更新 installed hash。
4. 发送失败的 batch 保留为待重试状态，或在恢复后调用 `midr_spf_install_resync()` 重建差异。
5. Zebra 重连后先清空本地旧 installed hash，再调用 `midr_spf_install_replay()` 重放完整结果；不能假设 Zebra 保留旧 client route。
6. DELETE 使用与 ADD 相同的 route type、VRF、table 和 instance identity。
7. shutdown 先停止新任务，再注销 backend、完成撤销和 flush，最后销毁 zclient 与 backend 私有状态。

## 9. 从旧 bgpd 代码迁移

第三组迁移时保留 `bgp_midr_zebra.c` 的核心逻辑，并执行以下替换：

| 旧依赖 | `midrd` 替换 |
| --- | --- |
| BGP 实例上的 DP 状态 | backend 私有 runtime，通过 `arg` 传入 |
| BGP 实例 VRF | backend runtime 的 `vrf_id` |
| bgpd 全局 zclient | `midrd` 自有 zclient |
| bgpd event master | `midrd` context 对应的 FRR event loop |
| `union g_addr` | `struct ipaddr` |
| 六个无返回值入口 | 五个 backend ops，返回真实错误 |
| 清空 pending queue | `abort_pending` callback |

原 `midr_result_to_zapi()`、installed hash、pending op 深拷贝、deferred timer 和 SRv6 编码主体可以直接迁移。`midr_spf_install_results()`、SPF result consumer 和 generation 缓存不需要迁移。

## 10. 验证要求

接口侧已有测试覆盖：

- IPv4/IPv6 add、metric replacement 和 delete diff；
- BASIC/ECMP 数据转换；
- UCMP weight、SRv6 SID 和 SPF/TE instance 载荷透传；
- 中途失败调用 abort，desired generation 不推进；
- 发送恢复后的显式 resync 和同 generation 的完整 replay；
- TED NOT_READY 全量撤销；
- 公开头无 BGP 私有依赖。

第三组完成 backend 后还需验证真实 ZAPI/FIB 的 IPv4/IPv6 add/update/delete、ECMP、UCMP、SRv6、Zebra 重连、发送失败重试和 shutdown flush。
