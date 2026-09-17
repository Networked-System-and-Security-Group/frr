# 第一组迁入 midrd：接入说明与跨组改动记录

## 1. 结论

第一组（NDS、Control、PM、CL、Facts、Tier1 准入、traceroute、IP2ASN、Tier1 名单）已作为
`midrd` 进程内模块运行，代码在 `midrd/group1/`。bgpd 里的旧实现原样保留作对照，新
backbone lab 中 bgpd 只承担 eBGP underlay。

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
| `midrd/midrd.c` | 上述两个函数的实现；`main()` 记录监听端点；弱符号钩子 `midr_group1_init()` / `midr_group1_terminate()` | 弱符号让不含第一组的独立构建和组件测试照常链接 |
| `midrd/midr-context-private.h` | `midr_context` 增加监听端点 `listen` | 上述查询所需的状态 |
| `midrd/midr-transport.c` | 主动建连前绑定到监听地址 | overlay 会话是多跳的，不绑定时内核选出口链路地址作源，对端认不出这是它请求过的会话（BGP 里对应 update-source） |
| `Makefile.am` | `include midrd/group1/subdir.am` | 把第一组源文件编进 midrd，文件清单放在第一组自己的目录里 |
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

## 3. 节点目录（第一组自有协议）

bgpd 里第二组替第一组把每个节点的群号、locator、角色位泛洪给全网，并通过远端视图回调交给
NDS。midrd 的 Membership 对象只带群号，交接文档也明确 `transport_address`、`cap_flags`
不再传播，因此第一组在自己的控制通道上补了一份节点目录（`midrd/group1/midr_nodedir.c`）：

- 每个节点把它交给 `midr_topology_node_upsert/withdraw` 的本机 Node 事实作为
  `NODE_ADV`（UDP 5859，48 字节）发给所有已建立的 overlay 邻居；
- 收到更高序号的通告就更新目录、回调 NDS，并转发给其他邻居（全网泛洪，与 Membership 范围一致）；
- 每 10 秒刷新一次，45 秒未刷新视为节点离开；撤销保留墓碑，防止旧通告复活；
- 新会话建立时把整份目录发给对端。

NDS 仍通过原来的远端视图回调接收这些数据，逻辑未改。`show midr directory` 查看目录。
**待与第二组讨论**：若第二组以后在 Membership 中携带 locator 和角色位并提供远端视图回调，
第一组可以删掉这份目录。

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

覆盖：underlay 转发（12 个 MIDR 节点传输地址两两互通）、原生会话（群内全互联、引导骨干网、
r1–r2 手配边、代表挂靠）、按性能选群（零配置 z2 进群 1）、按策略选群（z1 跳过经 Tier1 的群 1、
进群 2、与 r1/m1a 始终无会话）、节点目录一致、第二组接受 Node/Link、SPF 可达同群全部成员。

## 7. 已知限制与待转达问题

- midrd 没有 zclient（第三组 F6 未做），MIDR 路由不进 FIB，转发仍走 underlay。
- 群间路由为 0：各成员 `show midr spf` 的 inter 恒为 0，另一群的前缀不可达（与 bgpd 版的
  G3-2 现象相同，归第二/三组）。
- **会话因过期对象被关闭（请第二组处理）**：负载高时会话会断开重连，引导节点之间最明显
  （IPv6 lab 一轮中每个引导节点 20–50 次）。第一组记录的关闭原因只有两种：-116（ESTALE）与
  -104（对端复位）。ESTALE 来自 `midrd.c` 的 `age_object_lifetime()`：收到的对象剩余寿命为 0
  时返回 -ESTALE，`on_frame()` 把错误交回传输层，传输层随即关闭整条会话，对端看到 -104。
  即一个在途中过期的对象（默认寿命 6 秒、转发预算 1 秒）就会拆掉会话。建议过期对象只丢弃、
  不断会话，或调整寿命。第一组已不因会话掉线撤销 Link，验收不再受影响。
- 同时启动时，对端在本端读完配置、请求会话之前连进来的连接会被拒绝，对端 100ms 后重连，
  启动期日志里会有一批 `closed ... reason=0`，属预期。
- 组件测试仍针对 bgpd 里的第一组副本；midrd 副本靠两节点冒烟和 backbone lab 验证。

## 8. 迁移中顺带修正的第一组问题

- 加入流程在 60 秒探测期间向引导节点重拉一次群代表目录（同时启动时首个目录可能不全）。
- 配置了群号的代表在目录里看到自己时，直接按配置群落定，不再向自己要成员表。
- 生成 lab 配置时在 `router bgp` 块末尾写 `exit`，否则 vtysh 会把顶层 `midr` 命令发给 bgpd。
