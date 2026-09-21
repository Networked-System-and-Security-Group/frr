# 第一组迁入 midrd：接入说明与跨组改动记录

## 1. 结论

第一组（NDS、Control、PM、CL、Facts、Tier1 准入、traceroute、IP2ASN、Tier1 名单）已作为
`midrd` 进程内模块运行，代码在 `midrd/group1/`。新 backbone lab 中 bgpd 只承担 eBGP underlay。

本分支 `feat/midr-group1-midrd` 是第二组集成基线与第一组代码合并后的结果：先后合入第二组的
`feat/yhy-midr-single-instance-ls-flooding`、公共 Session 服务 `9a88097e2d`，以及
`fix/midrd-integration-hardening`（到 `c7320a2c9a`，含兼容 Zebra 门面、SPF install、
过期对象不再断会话、snapshot 发送失败处理、midrd 统一持有 VRF 生命周期）。

bgpd 对照路径：分支里保留了可运行的 bgpd 版第一组实现（`MIDR_LAB_STACK=bgpd`），但它**不是
原样不变的旧代码**。分支还带着第一组此前在 bgpd 上的 IPv6、Tier1 准入、traceroute 等提交
（相对第二组主线改了 37 个 bgpd 文件，并删除旧的 `midr_trace_exec.c/.h`）。迁入 midrd 之后，
第一组不再修改 bgpd 版，新功能只在 `midrd/group1/` 里做。

对接方式：

| 方向 | 接口 | 说明 |
| --- | --- | --- |
| 第一组 → 第二组 | `midrd/midr-topology.h` | 本机 Node/Link 事实的增量上报；事实表通过 `midr_topology_provider_register()` 提供完整快照 |
| 第一组 → 公共 Session 服务 | `midrd/midr-session.h`（第二组提供） | 运行时请求/释放原生 MIDR 会话，经 observer 获知会话起落与对端 `node_id` |
| 第一组 → midrd | `midrd/midr-context.h` 中的 `midr_context_node_id()`、`midr_context_listen_endpoint()`（第一组新增） | 本节点 node_id、MIDR 监听地址与端口 |
| 第一组只读 | `midrd/midr-spf.h` | `show midr spf` 读取 SPF 结果，用于验收 |

第一组没有读写 owned、canonical、LSDB、TED 或 transport 的私有状态。

## 2. 对第二组 / 公共文件的改动

以下改动都是第一组迁入所必需的最小改动，代码中每处都标注了“第一组新增/修改”和原因。

| 文件 | 改动 | 原因 |
| --- | --- | --- |
| `midrd/midr-context.h` | 声明 `midr_context_node_id()`、`midr_context_listen_endpoint()` | 第一组的 Node/Link 要填本节点 node_id；公共 Session 服务要求 endpoint 端口非 0，第一组用本机监听端口（全部署同一 MIDR 端口）；准入校验要比对会话源地址（即监听地址）。公共接口里没有这两项查询 |
| `midrd/midrd.c` | 上述两个函数的实现；`main()` 记录监听端点；弱符号钩子 `midr_group1_init()` / `midr_group1_terminate()`；`midr_group1_init()` 返回错误时 midrd 不进入运行状态 | 弱符号让不含第一组的独立构建和组件测试照常链接；第一组依赖的公共服务注册失败时不能带病运行 |
| `midrd/midr-context-private.h` | `midr_context` 增加监听端点 `listen` | 上述查询所需的状态 |
| `midrd/midr-transport.c` | 主动建连前绑定到监听地址 | overlay 会话是多跳的，不绑定时内核选出口链路地址作源，对端认不出这是它请求过的会话（BGP 里对应 update-source） |
| `midrd/midr-transport.c`、`midrd/midr-transport.h`、`midrd/midr-session.c`、`midrd/transport-test.c`、`midrd/session-test.c` | 移植第三组 `64fd75560e` 的 rendezvous 修复（非第一组原创代码，逐函数移植，保留第一组自己的 `midr_transport_reset()`/`midr_session_manager_reset()` 和 group1 回调） | 第二组 `0920第一组对接.md` 审查要求：accept 到的连接不能按地址猜邻居 |
| `midrd/Makefile` | 仅文件头加注释，不改任何构建规则 | 说明该入口的 `build/midrd` 不含第一组，避免把第二组组件测试通过当成第一组联合验收通过 |
| `Makefile.am` | `include midrd/group1/subdir.am`，放在 `include tests/subdir.am` 之后 | 把第一组源文件和组件测试编进来，清单放在第一组自己的目录里；测试程序要追加到 `check_PROGRAMS`，必须排在它的定义之后 |
| `vtysh/vtysh.h`、`vtysh/vtysh.c` | 新增 `VTYSH_MIDRD` 和 `midrd` 客户端 | midrd 带 CLI 后 vtysh 要能分发 MIDR 命令、下发配置 |
| `tools/frrcommon.sh.in` | `DAEMONS` 加入 `midrd` | frrinit 按 daemons 文件启动 midrd |
| `midr-test/backbone-group2/verify_group1_group2_evidence.py` | 合并时取第二组版本 | 之前第一组对它的修改（群规模、多跳传播路径）在单实例洪泛后已不适用 |

会话接口（2026-09-17 起使用第二组的公共 Session 服务 `9a88097e2d`）：

- 此前第二组尚未提供 Session 服务，第一组按交接文档第二版自写了一份 `midr-session.h`
  及其在 `midrd.c`、`midr-context-private.h` 中的实现。合并第二组实现时这些代码已全部删除，
  改用第二组版本；第一组只在 `midr_g1.c` 中适配：endpoint 端口填本机监听端口，observer
  改用 `midr_session_observer_ops`（远端 endpoint 在 status 里），注销用
  `midr_session_observer_unregister()`，邻居失效用 `MIDR_SESSION_CLOSE_NODE_DOWN`、准入拒绝用
  `MIDR_SESSION_CLOSE_POLICY`。
- 公共服务对建连失败也报 DOWN，第一组只对已建立会话的掉线记日志和关闭原因。
- 公共服务总是关闭没有意图的入向连接（不再以是否注册 observer 区分），准入策略对被动方照样生效。
- observer 在 midrd 收包路径里触发，第一组把处理放进事件队列，不在回调中同步 connect/disconnect。
- 第一组不因 Session Down 撤销 Link：掉线只起断连计时，断开超过 5 分钟仍未恢复才拆边销账；
  PM 判定链路不可达、换群、节点离开、手工拆除、策略拒绝时照常撤销。

构建入口约定（2026-09-21，回应 `0920第一组对接.md` 第 2.3 条）：采用该文档的第二种方案。
`midrd/Makefile` 只负责第二组组件测试，其 `build/midrd` 按设计不含第一组；第一组的联合验收和
生产构建一律使用顶层 `make midrd/midrd`。选第二种是因为它不需要改第二组的 Makefile 构建规则，
符合第一组尽量不动其他组代码的约定。

`midrd.c` 的 `midr_group1_init()`/`midr_group1_terminate()` 是弱符号，缺少第一组时 midrd 照样
能链接并启动，只是静默地没有第一组——这正是审查所说的“假通过”。因此第一组自备校验脚本
`midrd/group1/verify-group1-linked.sh`：它要求这两个符号在最终 ELF 里是**已定义**（`T`）而不是
未解析的弱引用（`w`），并打印所用构建入口和最终二进制路径供验收日志记录（顶层构建的 ELF 实体在
`midrd/.libs/midrd`，`midrd/midrd` 只是 libtool 包装脚本）。CI/交接测试应在联合验收前调用它。

## 3. 节点目录（第一组自有协议）

bgpd 里第二组替第一组把每个节点的群号、locator、角色位泛洪给全网，并通过远端视图回调交给
NDS。midrd 的 Membership 对象只带群号，交接文档也明确 `transport_address`、`cap_flags`
不再传播，因此第一组在自己的 UDP 5859 控制通道上用 `NODE_ADV` 报文分发一份节点目录
（`midrd/group1/midr_nodedir.c`）。NDS 仍通过原来的远端视图回调接收这些数据，逻辑未改。

这份目录归第一组所有，不属于第二组的 LS Object wire，也不进入 canonical、LSDB 或 TED。
报文格式、序号、刷新、老化、撤销和丢包恢复的规则见
[`midr-group1-node-directory.md`](midr-group1-node-directory.md)。

如果第二组以后在 Membership 中携带 locator 和角色位并提供远端视图回调，第一组可以删掉这份目录。

## 3a. 生命周期

- 启动：midrd 先初始化 VRF 和自己的运行时，再调用 `midr_group1_init()`。第一组先注册 Session
  observer 和 topology snapshot provider，任一失败就撤销已完成的注册、释放状态并返回错误，
  midrd 随即退出，不进入运行状态。两项都成功后才初始化 NDS、PM、CL、traceroute 等模块。
- VRF：第一组不再调用 `vrf_init()`/`vrf_terminate()`，traceroute 的 `vrf_socket()` 使用 midrd
  初始化的默认 VRF。
- 退出顺序（`midrd_terminate()`）：`midr_group1_terminate()` 停止第一组，注销 observer 和
  provider，此后不再产生新的 Node/Link，但不主动断开会话 → 第二组 `shutdown_withdraw()` 通过
  这些会话泛洪撤销 → `midr_context_finish()` 注销第三组 Zebra backend 并释放公共运行时 →
  midrd 终止 VRF。

## 4. 与 bgpd 版的行为差异

| 项目 | bgpd | midrd |
| --- | --- | --- |
| 配置位置 | `router bgp` 下 | `configure` 顶层（命令字符串不变） |
| 本机身份 | BGP router-id | `midrd --node-id`，取点分 router-id 在主机上的 `s_addr` 原值 |
| transport | `midr transport-address` | 同左，且必须等于 `midrd --listen` 的地址 |
| 会话 | BGP peer（multihop、update-source、MIDR-LS AF） | midrd 原生会话，`show midr neighbors [json]` |
| 调试 | `debug bgp midr [discovery]` | `debug midr [discovery]` |
| 运行配置 | `show running-config` | `show midr running-config`（midrd 没有自己的配置节点） |
| 优雅退网 | 等第二组确认撤销写出 | 固定延时后拆会话，撤销由 midrd 泛洪 |
| `midr help` | VIEW 与配置态 | 只在配置态（同一字符串两边节点集合不同会让 vtysh 命令表撞名） |
| AS_PATH 版 Tier1 检查 | 有 | 未搬（midrd 没有 BGP RIB） |
| Link 的 `available_bandwidth_kbps` | 固定 1 Gbps 占位 | PM 短期 `bw_score`（下限 1），见下 |

**带宽口径（需第二组知悉）**：PM 无法打流测带宽，`bw_score` 用 RTT 与丢包按 Mathis TCP
吞吐公式估计带宽：`sqrt(1.5) / (RTT_s × sqrt(max(loss, 1%)))`（省去 MSS）。第一组把它直接
填进 `available_bandwidth_kbps`。它是各节点口径一致的相对估计，不是真实 kbps。代入
`midr_cost_from_metrics()` 后传输时间项约为 43×RTT，代价大致按 `RTT / sqrt(max(loss,1%))`
排序，丢包的权重比原先 1 Gbps 占位时明显增大。例：RTT 179µs、无丢包时上报 68421，代价 79。

midrd 以 root 运行且不降权，vty socket 只有 root 能连，检查脚本用 `docker exec -u root`。
在同时装有 bgpd 旧实现的节点上查询 MIDR 状态请用 `vtysh -d midrd`。

## 5. backbone lab

```bash
# midrd 版（默认）；IPv6 加 MIDR_LAB_FAMILY=ipv6；bgpd 对照版加 MIDR_LAB_STACK=bgpd
sg clab_admins -c './midr-test/backbone-lab/run_group1_group2_lab.sh all'
```

- `gen-midrd-lab.py` 由 bgpd 版配置生成 `configs-midrd/`、`configs-midrd-v6/`：bgpd 只留
  underlay，`midr` 行移到顶层，daemons 文件启动 midrd（node id、监听地址、业务前缀
  `198.18.0.N/32` 或 `fd00:18::N/128`）。
- `check_midrd_lab.sh`：守护进程与 underlay、传输地址两两互通、群组形成、原生会话网格、
  节点目录一致、第二组接受 Node/Link、SPF 可达同群全部成员。
- `check_tier1_admission.sh`、`check_ipv6_only.sh` 在 `MIDR_LAB_STACK=midrd` 时读 midrd 状态。

## 6. 验证结果（2026-09-17）

| 实验 | 结果 |
| --- | --- |
| midrd 版 backbone lab，IPv4 | 基础验收 53/53，Tier1 准入 25/25 |
| midrd 版 backbone lab，IPv6-only | 基础验收 53/53，Tier1 准入 25/25，IPv6-only 10/10 |
| 整树编译 | 零告警；bgpd 29 个 MIDR 单元测试、midrd 21 个组件测试与边界扫描通过 |
| 合并第二组公共 Session 服务后（`5d039b9e1a`） | IPv6：基础 53/53、Tier1 25/25、IPv6-only 10/10；IPv4：基础 53/53、Tier1 25/25；整树零告警，midrd `make test`（含 session-test）与边界扫描通过 |
| 合并 `fix/midrd-integration-hardening` 并处理审查反馈后（`e9c502c6d7`，2026-09-19） | midrd 与 `test_midr_group1` 编译零告警；midrd 23 个组件测试、边界扫描、`git diff --check` 通过；`test_midr_group1` 5 项通过。IPv6：基础 53/53、Tier1 25/25、IPv6-only 10/10；IPv4：基础 53/53、Tier1 25/25（3 个引导节点运行约 6 分钟，会话关闭 0 次）。IPv6 lab 运行约 7.5 分钟，6 个抽查节点（含 3 个引导节点）的会话关闭次数为 0（此前每个引导节点每轮 20–50 次）。z2 收到 SIGTERM 后 0.2 秒内正常退出，同群 m1a 的 SPF 随即不再包含 z2（reachable 4 → 3），说明第一组停止后第二组的撤销照常泛洪 |

覆盖：underlay 转发（12 个 MIDR 节点传输地址两两互通）、原生会话（群内全互联、引导骨干网、
r1–r2 手配边、代表挂靠）、按性能选群（零配置 z2 进群 1）、按策略选群（z1 跳过经 Tier1 的群 1、
进群 2、与 r1/m1a 始终无会话）、节点目录一致、第二组接受 Node/Link、SPF 可达同群全部成员。

## 7. 已知限制与待转达问题

- midrd 没有 zclient（第三组 F6 未做），MIDR 路由不进 FIB，转发仍走 underlay。
- 群间路由为 0：各成员 `show midr spf` 的 inter 恒为 0，另一群的前缀不可达（与 bgpd 版的
  G3-2 现象相同，归第二/三组）。
- 会话因过期对象被关闭：第二组已在 `4066f4debb` 中修复（过期对象只丢弃，不再断会话）。
  合入后的 IPv6 lab 中抽查节点的会话关闭次数为 0。
- 同时启动时，对端在本端读完配置、请求会话之前连进来的连接会被拒绝，对端 100ms 后重连，
  启动期日志里会有一批 `closed ... reason=0`，属预期。
- IPv6 link-local transport 暂不支持：第一组只支持 IPv4 和全局 IPv6 transport 地址。公共 Session
  API 对 link-local endpoint 要求非零 `scope_id`（出接口编号），而第一组的 locator、邻居和节点目录
  只保存地址、不保存接口。直连邻居用 link-local 建邻列为后续工作。
- `midrd/group1` 的组件测试 `test_midr_group1`（`midrd/group1/test_midr_group1.c`）把第一组的
  真实模块接到 Session/Topology 服务的桩上，覆盖：observer 或 provider 注册失败时的回滚；
  Session Up、Down、Identity Mismatch、迟到事件和未知对端事件；Node upsert 失败后事实保留、
  进入 resync 快照并在下次上报时重试；terminate 时仍在队列里的 Session 事件不访问已释放的状态，
  第一组停止后不断开会话、不再上报。其余功能仍由两节点冒烟和 backbone lab 覆盖；bgpd 里第一组
  副本的单元测试保留。

## 8. 迁移中顺带修正的第一组问题

- 加入流程在 60 秒探测期间向引导节点重拉一次群代表目录（同时启动时首个目录可能不全）。
- 配置了群号的代表在目录里看到自己时，直接按配置群落定，不再向自己要成员表。
- 生成 lab 配置时在 `router bgp` 块末尾写 `exit`，否则 vtysh 会把顶层 `midr` 命令发给 bgpd。
