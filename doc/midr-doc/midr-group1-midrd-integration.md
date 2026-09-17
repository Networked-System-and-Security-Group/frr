# 第一组迁入 midrd：接入说明与跨组改动记录

## 1. 结论

第一组（NDS、Control、PM、CL、Facts、Tier1 准入、traceroute、IP2ASN、Tier1 名单）已作为
`midrd` 进程内模块运行，代码在 `midrd/group1/`。bgpd 里的旧实现原样保留作对照，新
backbone lab 中 bgpd 只承担 eBGP underlay。

对接方式：

| 方向 | 接口 | 说明 |
| --- | --- | --- |
| 第一组 → 第二组 | `midrd/midr-topology.h` | 本机 Node/Link 事实的增量上报；事实表通过 `midr_topology_provider_register()` 提供完整快照 |
| 第一组 → midrd 传输层 | `midrd/midr-session.h`（第一组新增） | 运行时请求/释放原生 MIDR 会话，获知会话起落 |
| 第一组只读 | `midrd/midr-spf.h` | `show midr spf` 读取 SPF 结果，用于验收 |

第一组没有读写 owned、canonical、LSDB、TED 或 transport 的私有状态。

## 2. 对第二组 / 公共文件的改动

以下改动都是第一组迁入所必需的最小改动，代码中每处都标注了“第一组新增/修改”和原因。

| 文件 | 改动 | 原因 |
| --- | --- | --- |
| `midrd/midr-session.h`（新文件） | 会话接口：`midr_session_owner_register`、`midr_session_request`、`midr_session_release`、`midr_session_is_up`、`midr_context_node_id`、`midr_context_listen_address` | midrd 原来只能用 `--peer` 静态配置邻居，第一组的邻居发现需要运行时增删会话 |
| `midrd/midrd.c` | 上述接口的实现；会话由第一组管理时关闭未请求的入向连接；收到对端 HELLO 后才向第一组报“会话建立”；会话断开时通知；记录监听端点；弱符号钩子 `midr_group1_init()` / `midr_group1_terminate()` | 准入策略（Tier1）要对被动方生效；HELLO 带来对端身份（相当于 BGP OPEN）；弱符号让不含第一组的独立构建和组件测试照常链接 |
| `midrd/midr-context-private.h` | `midrd_peer_config.up`；`midr_context` 增加监听端点和会话持有者回调 | 上述实现所需的状态 |
| `midrd/midr-transport.c` | 主动建连前绑定到监听地址 | overlay 会话是多跳的，不绑定时内核选出口链路地址作源，对端认不出这是它请求过的会话（BGP 里对应 update-source） |
| `Makefile.am` | `include midrd/group1/subdir.am` | 把第一组源文件编进 midrd，文件清单放在第一组自己的目录里 |
| `vtysh/vtysh.h`、`vtysh/vtysh.c` | 新增 `VTYSH_MIDRD` 和 `midrd` 客户端 | midrd 带 CLI 后 vtysh 要能分发 MIDR 命令、下发配置 |
| `tools/frrcommon.sh.in` | `DAEMONS` 加入 `midrd` | frrinit 按 daemons 文件启动 midrd |
| `midr-test/backbone-group2/verify_group1_group2_evidence.py` | 合并时取第二组版本 | 之前第一组对它的修改（群规模、多跳传播路径）在单实例洪泛后已不适用 |

会话接口语义（详见头文件注释）：

- 会话以远端 transport 地址为键，远端端口等于本机监听端口（同一部署内所有 midrd 用同一 MIDR 端口）。
- 请求过的会话与 `--peer` 会话一样参与泛洪；`node_id` 可先填 0，由对端 HELLO 补上。
- 一旦注册了持有者，未请求的入向连接会被关闭。持有者未注册时 midrd 行为不变。
- 回调在 midrd 收包路径里触发，持有者不得在回调中同步请求/释放会话（第一组把处理放进事件队列）。

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

## 6. 已知限制

- midrd 没有 zclient（第三组 F6 未做），MIDR 路由不进 FIB，转发仍走 underlay。
- 组件测试仍针对 bgpd 里的第一组副本；midrd 副本靠两节点冒烟和 backbone lab 验证。
