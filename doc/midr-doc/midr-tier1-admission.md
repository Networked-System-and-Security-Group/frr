# MIDR 邻居建立与 Tier1 绕行准入：实际实现

日期：2026-09-16。基于 HEAD `89168d2cc7` 的未提交工作区，对应项目外层的 `05_MIDR_邻居建立与Tier1绕行准入_设计.md`。本文记录实际代码及实现边界；按新增需求，原设计“无法确定则暂缓”已改为“有明确 Tier1 命中才拒绝”，以本文为准。

## 1. 配置和最终行为

```text
configure terminal
 midr ip2asn file /var/lib/frr/ip2asn/prefix2as.txt
 midr tier1 file /var/lib/frr/ip2asn/tier1_asns.txt
 router bgp 65001
  midr transport-address 192.0.2.1
  midr avoid-tier1
 exit
end
show midr admission
show midr tier1 list
```

上面的 ASN、地址和路径仅作格式示例，需替换为现有配置。IP2ASN/Tier1 文件命令仍属于全局 CONFIG_NODE；`[no] midr avoid-tier1` 属于 BGP_NODE，`show midr admission` 注册于 VIEW_NODE，并由 FRR 同步到 ENABLE_NODE。开关写入 BGP 配置段，缺省关闭；名单格式沿用 [原实现说明](midr-native-traceroute.md)。

- 关闭：保持原有建邻资格。已保存的准入等待意图恢复建连，仍执行身份、排除、transport、shutdown 等原有检查。
- 开启：MANUAL、SAME_GROUP、CL_ANCHOR、ATTACH、接收回配，以及既有 MIDR peer 的新连接尝试，均受准入约束。
- 任意已观测 hop 映射命中当前 Tier1 清单即拒绝，包括只得到部分路径的情况。
- 每个无响应 TTL 最多发送 3 次（含首次），耗尽后保留 `*` 并继续后续跳。任务结束后，只要可映射的已观测 hop 未命中 Tier1，就允许建联；不要求到达目标、所有 hop 可见或 ASN 映射完整。全 `*`、部分 `*`、未映射/ASN 0、截断以及执行超时保留的部分路径均不单独导致策略拒绝，也不因这些缺失而循环重探。
- 放行原因显示为 `no-tier1-observed`，表示“未观测到 Tier1”，不宣称已证明整条路径没有 Tier1。有效许可仍有新鲜度约束；后续新的连接尝试、数据或 transport 改变可以重新探测。
- 清单或 IP2ASN 未加载、源上下文无效、排队/资源不足、socket/send/receive 错误、取消和关停仍按配置/执行故障处理，不冒充已经完成的网络探测。部分结果即使伴随错误，只要已有明确 Tier1 命中仍然拒绝。
- 开关或数据变化不主动拆除已有 Established 会话；掉线后的新尝试重新准入。原有 NDS 撤销、角色变化和退网等生命周期仍可正常拆邻。
- 原生 BGP peer 以 `PEER_FLAG_MIDR_OVERLAY` 区分，不受该策略改变。MIDR 不接管已被原生 peer 占用的地址。

“绕行”在这里是邻居准入和候选替换，不修改 underlay 路由。UDP traceroute 观测不能保证 BGP TCP 在 ECMP、策略路由或后续路由变化中永远经过相同路径。

## 2. 已落地的调用链

```text
原有候选性能测量 / CL / 手工配置 / 控制请求
  -> midr_ctrl_connect[_received]
  -> 身份、locator、排除及原生 peer 冲突检查
  -> midr_admission_gate
       pending/blocked：此处返回，不建台账、不 adopt、不发建连 nudge
       ready：台账 -> SAME_GROUP adopt / 锚点 PM -> nudge -> 创建/整形 peer

异步 trace 完成
  -> 校验意图、数据代数、源上下文、期限、远端活跃性
  -> 按当前 IP2ASN/Tier1 数据重映射 raw hops
  -> 发布许可 -> 排队 resume_connect -> 再进入上述统一入口

BGP Start / 主动 socket / 被动 accept
  -> MIDR 专用许可检查
  -> 本次 connection 绑定许可
  -> Established 之前再次验证
```

新增 `bgpd/bgp_midr_admission.[ch]` 保存意图、原因位集合、远端请求活跃时间、trace token、许可和退避状态。复制的是节点身份及 locator 等值，不持有节点哈希指针、VTY 指针或跨回调借用的数据快照。

同一目标的本地/远端原因合并，手工需求保持独立。取消一种原因不会直接取消其它原因；删除目标、退网和 transport 重配取消对应意图。无 peer/台账的纯等待意图失效时，仅释放准入资源，不停止原有独立候选 PM。

`midr_trace_cancel()` 仍会交付一次终结回调。因此取消前先把 token 的 owner 置空，token 最终由回调释放；实例销毁先停准入，再释放 control/PM/视图。全局 scheduler 不因删除一个实例而销毁。

## 3. BGP FSM 与探测上下文

`bgp_start()` 等待准入时返回 `BGP_FSM_DEFERRED`。事件分发器调用原 `bgp_stop()` 清理，并让保留的配置 peer 等待于 Idle，由准入管理器唤醒；不制造无 fd 的连接进行中状态。被动 clone 可能在 `bgp_stop()` 中删除，分发器检查返回值后才访问 connection。

`bgp_connect()` 创建 socket 前、`bgp_accept()` 创建并交接被动连接前均有检查。`bgp_establish()` 在连接迁移及发布 Established 前再验证许可。策略/数据改变立即撤销许可并排队停止未建立的尝试，配置加载期间也撤销；恢复动作等待整个配置加载结束。

许可新鲜度只限制新尝试，已绑定许可的握手不会仅因超过 30 秒被取消。但源地址、数据或策略变化仍使它失效。配置的 peer update-source 必须等于 MIDR transport，不能使用另一个源接口；已有 socket 的本地地址也必须匹配。

静态复查补正：在途握手判断同时检查配置 peer 与被动 doppelganger；新尝试要求刷新观测时，先等待已有有效握手结束，避免启动重探时清零其许可。取消锚点评估也撤销 CL_ANCHOR 准入 owner，即使 ANCHOR 分支已经清空评估槽位；其它 owner（例如 MANUAL）保留。

探测目标是 remote transport，源地址绑定 active local transport，IPv4/IPv6 均沿用原 Linux UDP 错误队列、FRR 事件和逐跳异步实现。源地址绑定失败不回退自动源地址或外部程序。

Scheduler 的 key/hash/比较同时包含 target、profile、source、实例 cookie、VRF；诊断 CLI 的自动源上下文与准入上下文不共享观测。按目标清缓存会清除此目标的全部上下文。数据更新不必丢弃原始路径：足够新鲜的 raw hops 可以重新映射；成功的 IP2ASN/Tier1 发布或清除通过公共 hook 使旧许可失效，加载失败不触发换代。

## 4. 候选、控制时序与资源限制

| 项目 | 实际值/行为 |
|---|---|
| 新尝试接受的观测年龄 | 不超过 30 秒；超龄缓存强制刷新 |
| 单次准入期限 | 105 秒，包含排队；默认排队 5 秒、执行 95 秒，余量 5 秒；管理器每秒检查超时 |
| 无响应 hop 重试 | 最多 3 次发送（含首次），每次等 1 秒；用尽后保留 `*` 并继续 |
| 配置/执行故障重试 | 5 秒起，指数退避到 60 秒，加不足 1 秒的实例抖动；正常探测缺失不进入此退避 |
| 命中 Tier1 冷却 | 60 秒；到期可重测，数据变化立即撤销旧判定 |
| 远端独占意图 | 尚未 Established 且连续 10 秒未见请求后撤销 |
| 控制建连请求宽限 | 首次入队后 120 秒，继续原重传节奏；随后消费原重试预算 |
| 意图资源上限 | 每实例最多保留 128 个条目，另受原 scheduler 限制 |
| 满额恢复 | 优先回收 Established 的观测条目；仍满时返回 INVALID，不虚报 pending。保存的手工配置和 Idle peer 每 5 秒获得恢复机会 |

上述时间受主事件循环调度延迟影响，不是实时系统硬期限。持续保留的拒绝/未知意图也占用 128 个条目；这比设计的“仅在途 128”更保守，满额时新候选可能需要等待其它需求撤销。`show midr admission` 显示容量，便于区分资源耗尽和未命中。

接收重复请求刷新活跃时间，复用同一准入任务；本地重复发现不会延长已经发送的控制宽限。接收侧命中 Tier1 或发生执行故障时使用原 PEER_REJECT，详细原因留在本地；正常结束但含 `*` 的结果不再发送策略拒绝。等待数据的请求由远端重传/超时及意图过期约束。

ATTACH 在探测和恢复事件期间预留槽位，拒绝/退避释放预留。恢复时再次检查 K=2 容量，避免旧候选晚到后超过替代候选已填满的槽位。第二批挂靠重试预算随意图保存，真实 nudge 入队后应用；手工意图不能被当作挂靠槽位。

CL 对冷却中的 Tier1 拒绝/执行故障候选过滤，异步通知后可重选锚点或挂靠；`*` 或未映射 ASN 不再使正常结束的观测进入该过滤状态。原 PM 指标不被覆盖为失败数值；成员质量计数另检查策略资格。准入通知不会提前触发尚在等待 PM 定时器的入群决策。组建/锚点函数的返回计数表示已接受的意图，不表示 Established 数量。

协议仍为原 v4 控制报文，没有新增 PENDING 报文或拒绝原因字段。即使本地开关关闭，发送端也保留 120 秒宽限，等待无响应节点的时间因此变长。混合旧端可能在慢探测完成前放弃，建议双方同时升级。

## 5. 相对设计的具体边界

- 当前 NDS 原本只初始化 DEFAULT 实例；本次没有扩展整个 MIDR 的多 VRF 支持。准入与底层探测显式拒绝非默认 VRF，不会静默探测默认路由表后给另一 VRF 放行。
- 未增加 source/interface/VRF 的诊断 CLI。源绑定为准入的内部 API，诊断查询保持原行为。
- 准入模块直接重映射 raw path 中可见 hop，忽略不可映射位置，只以明确 Tier1 命中作策略拒绝依据，没有修改原 observer 的诊断结果语义。
- `show midr admission` 为文本输出：开关、容量、源/目的、状态原因、两份数据 generation、request ID、owner 位图、Established 保留标记。未增加准入 JSON；通用 job JSON 也未加入网络上下文字段，关联上下文请看准入 show 中的 request ID。
- 原有 PM 协议及控制 TCP 列表交换代码无需重写；其间接建邻继续进入统一 control 入口。

## 6. 文件与验证状态

生产入口修改涉及 `bgp_midr_admission.[ch]`、`bgp_midr_ctrl.[ch]`、`bgp_midr_nds.[ch]`、`bgp_midr_nds_vty.c`、`bgp_midr_cl.c`、`bgp_network.c`、`bgp_fsm.[ch]` 和 `bgpd.h`；探测及数据通知涉及 `midr_trace_{types,scheduler,engine,udp}`、`midr_ip2asn.c`、`midr_tier1_list.[ch]`。新模块已加入 `bgpd/subdir.am`。

已补充并注册 `test_midr_admission.c`：完整/部分路径判定、IPv4/IPv6、取消后回调、实例销毁、数据换代、许可及源/VRF/开关检查。原 engine/scheduler/UDP 测试补充了源上下文传递、缓存隔离及不支持上下文拒绝。测试已接入 Automake 和现有 Python 测试入口。

最新策略回归增加了：IPv4/IPv6 第三次探测收到响应、3 次无响应后继续下一 TTL、全星号/未映射/截断/未到达放行、执行超时仍检查已有 Tier1 命中、部分结果可取得建连许可且不立即重探。socket 错误继续等待的用例用于确认执行故障边界。静态脚本同时检查逐跳重试预算与默认执行、准入及控制宽限的大小关系。这些 C 用例仅已编写，尚未在本机执行。

本机实际验证仅包括：

```sh
python tools/check_midr_native_static.py
git diff --check
```

另以源码逐项审查主被动入口、FSM 清理、token 所有权、命令启动注册、配置写回和构建清单。结构检查不等于 C 类型检查、链接或运行测试。本机未编译 FRR、未执行 C 测试或 Linux 网络集成测试；需在 Linux 编译环境继续验证主被动握手、重连、40 秒冷探测、配置重载和真实 IPv4/IPv6 socket 行为。

本次恢复首先核对了已有文件及工作区差异，上次最后新增的测试已经落盘；没有重置、暂存或提交代码，未发现与前次工作记录不符的额外修改。由于中断前没有完整文件哈希快照，这一核对不能证明中断期间绝无外部写入。

部署仍需更新并实际重启 bgpd，仅 `make install` 后重新打开 vtysh 不会替换旧进程；具体排查见 [Unknown command 说明](midr-native-traceroute.md#安装新版本后出现-unknown-command)。
