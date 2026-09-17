# midrd 与第三组输出接口交接

## 1. 交接结论

第二组独立 `midrd` 已收口，主体提交为 `6bf1f9c524de2f8828c8c05942b0e72390d5c1d2`，当前收口后审查提交为 `5eab85832a`。第三组后续只需消费 committed TED/SPF route result，在自己的 adapter 中完成 Zebra/ZAPI/Linux FIB、ECMP/UCMP/SRv6 等下游安装策略。

第三组不修改 MIDR canonical、洪泛、sequence、lifetime、scope 或 LSDB/TED 内部状态；第二组不将 Zebra、ZAPI、Linux FIB 或 BGP 类型引入 `midrd` 核心。

```text
midrd canonical -> scope -> LSDB -> committed TED -> layered SPF
                                                      |
                                                route result
                                                      |
                                             第三组 adapter
                                                      |
                                                Zebra / ZAPI
                                                      |
                                                 Linux FIB
```

## 2. 公开输出接口

| 用途 | 公开头文件 |
| --- | --- |
| committed Consumer snapshot | `midrd/midr-consumer.h` |
| TED READY/NOT_READY、视图和变更通知 | `midrd/midr-ted.h` |
| IPv4/IPv6 SPF route result 和 nexthop 集合 | `midrd/midr-spf.h` |

第三组 adapter 只 include 上述公开头文件，不得读写 `struct midrd`、canonical store、owned store、LSDB stage 或 TED 私有内存。

## 3. TED 发布与 generation

第三组 adapter 通过 `midr_ted_consumer_register()` 注册 `snapshot_changed` 回调。回调只表示新 TED generation 已提交，不能拒绝或回滚上游。adapter 在回调后使用 `midr_ted_view_acquire()` 获取完整视图，调用 `midr_spf_compute_ted()` 计算路由，再在自己的 staging 中完成 diff 和 ZAPI 编码。

一个 generation 的所有添加、替换和删除应一次性发布，不得将新旧 generation 混装进 FIB。迟到旧 generation 和重复 callback 不得覆盖当前已安装结果。

`MIDR_TED_NOT_READY` 或 `midr_ted_view_acquire() == -EAGAIN` 时不得生成部分新 FIB 结果。是否暂时保留上一个已安装的一致 FIB、设置失效定时器或执行撤销，属于第三组转发面策略；但不能将旧视图标记为新 generation，也不能绕过 NOT_READY 读取私有 TED 内存。

## 4. SPF route result 语义

`struct midr_spf_route` 包含：

- `family`/`prefix_len`/`prefix`：IPv4 或 IPv6 目的前缀。
- `generation`：生成该结果的 committed TED generation，adapter 必须拒绝迟到旧 generation。
- `metric`、`group_score`、`local_cost`：分层 SPF 结果；下游不重新解释 MIDR scope 或重算 canonical cost。
- `scope`：`MIDR_SPF_ROUTE_LOCAL`、`MIDR_SPF_ROUTE_INTRA_GROUP`、`MIDR_SPF_ROUTE_INTER_GROUP` 或 `MIDR_SPF_ROUTE_UNREACHABLE`。
- `reachable`/`local_destination`：本地前缀不需要 nexthop；不可达前缀的 metric 为无穷大且 nexthop 为空。
- `nexthops[]`：等价最优首跳集合，每项包含 address family、address、ifindex、local/remote node、group、next group 和 link ID。

`midr_spf_routes_clear()` 负责释放每条 route 内部的 nexthop 数组。当前输出是等价多首跳集合；UCMP 权重、SRv6 封装和 Zebra route type/instance 不在该结构中，由第三组下游策略处理。

## 5. 推荐接线流程

```text
snapshot_changed(ted, generation, change_flags)
  -> 检查 generation 新于当前已安装版本
  -> midr_ted_view_acquire()
  -> midr_spf_compute_ted()
  -> 在第三组 adapter 内构建完整 candidate route set
  -> 计算 add / replace / delete diff
  -> 通过 ZAPI 提交
  -> 全部成功后记录 installed generation
  -> midr_spf_routes_clear() + midr_ted_view_release()
```

ZAPI 部分失败时，adapter 不得将 candidate generation 标记为已安装。重试必须从完整 committed TED/SPF 结果重建，不得继续使用半次 diff 作为新基线。

## 6. 第三组验收条件

- IPv4/IPv6 local、intra-group、inter-group、unreachable 路由转换正确。
- 单首跳和 ECMP 多首跳的 address/ifindex 与 SPF result 一致。
- 新 generation 以完整 diff 安装；迟到旧 generation、重复 callback 和回调内注销不污染当前 FIB。
- NOT_READY、ZAPI 部分失败、Zebra 断连和重连都不留下半个新 generation。
- route replacement、withdraw、`midrd`/Zebra restart 和退网后 Linux FIB 没有残留。
- adapter 只 include 公开 Consumer/TED/SPF 头文件，不把 Zebra 头文件引入 MIDR 核心。

## 7. 第二组已有证据

| 验证 | 结果 | 证据 |
| --- | --- | --- |
| TED/SPF 组件 | Consumer、LSDB、TED、engine、SPF、Local transaction 测试 PASS | `/home/guest/yhy/midr-gate-runs/r7-second-group-close-20260916/component-final.log` |
| SPF 场景 | IPv4/IPv6 nexthop、ifindex、ECMP、分层 intra/inter-group 和 unreachable 已覆盖 | `midrd/spf-test.c`、`midrd/engine-test.c` |
| 无 BGP 双栈预验证 | IPv4/IPv6 收敛、triangle、partition/recovery 和 owner expiry PASS | `/home/guest/yhy/midr-gate-runs/r7-hardening-20260916-phase1*`、`phase2*`、`phase3-spf` |
| 边界门禁 | Consumer/TED/SPF 公开头可独立编译，`midrd` 无 Zebra/BGP 依赖 | `midrd/extraction-boundary-test.sh` |

`bgpd` 旧 MIDR/Zebra 实现暂作对照保留，但不是新输出接口的运行依赖。第二组分支当前提交未 push。
