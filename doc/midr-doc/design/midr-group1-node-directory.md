# 第一组节点目录协议（NODE_ADV）

归属：第一组。实现：`midrd/group1/midr_nodedir.c`，报文类型定义在 `midrd/group1/midr_ctrl.h`。
本协议由第一组负责维护和测试。它不属于第二组的 LS Object wire，内容也不进入第二组的
owned、canonical、LSDB 或 TED。

## 1. 用途

第一组做发现和选群时，要知道全网每个节点的几项信息：

| 信息 | 用途 |
| --- | --- |
| 群号 | CL 按群统计成员、选群；入群后找同群成员建全互联会话 |
| locator（传输地址） | 向某个节点发起会话、发控制消息、做 PM 测量时使用的地址 |
| 能力位（群代表、引导节点等） | 找群代表要成员表；识别引导骨干 |

在 bgpd 里，这些信息由第二组随 MIDR-LS 泛洪，再通过远端视图回调
（`midr_remote_view_callbacks`）交给 NDS。`midrd` 的 Membership 对象只带群号，交接文档也明确
`transport_address` 和 `cap_flags` 不再传播。因此第一组在自己的控制通道上分发这份目录，
NDS 仍通过原来的远端视图回调读取，NDS 代码不用改。

## 2. 传输

- 报文走第一组的 UDP 控制通道，端口 5859（`MIDR_CTRL_UDP_PORT`）。同一端口上还有第一组
  原有的 PEER_REQUEST、ATTACH_REQUEST、ANNOUNCE 等控制消息；TCP 5859 用于成员表等列表交换。
  这几样都与第二组 midrd 会话使用的 MIDR 端口无关。
- 只在已建立的 overlay 会话邻居之间收发：发送方只发给 `established` 的邻居，接收方丢弃
  不是来自已建立邻居的报文。
- 目标是会话对端的 transport 地址。IPv4 和全局 IPv6 都支持。

## 3. 报文格式

定长 48 字节，多字节字段均为网络字节序。

| 偏移 | 长度 | 字段 | 说明 |
| --- | --- | --- | --- |
| 0 | 1 | version | `MIDR_CTRL_MSG_VERSION`（当前为 4） |
| 1 | 1 | type | `MIDR_CTRL_NODE_ADV`（11） |
| 2 | 2 | flags | bit 0 = withdrawn（撤销），其余位为 0 |
| 4 | 4 | node id | 通告的节点，取 router-id 的 `s_addr` 原值，与 Node 事实的 `node_id` 相同 |
| 8 | 4 | group id | 群号 |
| 12 | 4 | capability bits | 能力位（Node 事实 `cap_flags` 的低 32 位） |
| 16 | 20 | locator | 2 字节 AFI（1 = IPv4，2 = IPv6），2 字节保留，之后是 4 或 16 字节地址，剩余补 0；撤销时全 0 |
| 36 | 8 | sequence | 序号，见第 4 节 |
| 44 | 2 | lifetime | 秒；0 表示使用默认值 45 |
| 46 | 2 | reserved | 发送填 0，接收忽略 |

接收方满足以下任一条件就丢弃报文，并计入 `invalid` 计数：长度不是 48 字节，version 或
type 不符，来源不是已建立的邻居，node id 为 0，sequence 为 0，非撤销报文的 locator 无法解码。

## 4. 序号

- 每个节点只为自己的条目生成序号。每次发出新内容或定时刷新时，序号取
  `max(当前序号 + 1, 当前 Unix 时间 << 16)`，所以节点重启后的序号仍大于重启前发出的。
- 接收方只接受比本地保存的序号严格更大的通告；序号相同或更小的计入 `stale`，
  既不处理也不转发。节点自己发出的当前通告被邻居转发回来时也是这样处理，这一点保证了
  泛洪能够停止。
- 如果收到的"自己的"通告序号比本地还大，说明网络里还留着本节点上一次运行时的通告。
  这时节点把序号跳到它之后并重新通告。

## 5. 泛洪、刷新、老化与撤销

| 过程 | 规则 |
| --- | --- |
| 发起 | 本机 Node 事实交给 `midr_topology_node_upsert/withdraw()` 时，同一份内容生成本节点的 NODE_ADV，发给所有已建立的邻居 |
| 转发 | 接受一条通告后，转发给除来源之外的所有已建立邻居，全网泛洪；范围与 Membership 相同 |
| 刷新 | 每 10 秒用新序号重发一次本节点通告 |
| 老化 | 条目的过期时间取报文里的 lifetime（默认 45 秒）；过期仍未刷新就视为节点离开，经 `remote_node_withdraw` 通知 NDS |
| 撤销 | 本节点撤销 Node 事实时发出 withdrawn 通告，并在 45 秒内继续随刷新重发 |
| 墓碑 | 收到撤销或条目过期后，条目保留 45 秒作为墓碑，防止还在路上的旧通告把节点复活；之后删除 |

## 6. 丢包与恢复

- UDP 不重传。单个报文丢失后，最迟在下一个 10 秒刷新周期由新序号的通告补上；
  lifetime 45 秒可以容忍连续丢失 4 次刷新。
- 新会话建立时，双方把自己的通告和目录里的全部条目（包括墓碑）发给对方，
  新加入的节点不用等刷新就能拿到完整目录。
- 目录只在内存里，重启后从空开始，靠邻居的全量发送和刷新重建。第 4 节的序号规则
  保证重启后自己的通告不会被旧通告压住。

## 7. 与 NDS 的接口

目录通过 `midr_g1.h` 中的远端视图接口交给 NDS，接口与 bgpd 版第二组提供的相同：

- `midr_remote_view_callbacks_register()`：注册时先回放已知的有效条目；
- `remote_node_update()`：接受一条非撤销通告时调用；
- `remote_node_withdraw()`：收到撤销或条目老化时调用（只在条目原本有效时调用）；
- `midr_remote_view_snapshot_get/release()`：取当前全部有效条目。

`show midr directory` 显示本节点通告、每个条目的群号、能力位、locator、剩余寿命、来源，
以及 accepted / stale / invalid / tx 计数。

## 8. 测试

backbone lab 的 `check_midrd_lab.sh` 检查每个节点的目录和全网节点集合一致。

## 9. 以后

如果第二组以后在 Membership 中携带 locator 和能力位，并提供远端视图回调，第一组可以
删掉这份目录，改用第二组的数据。在那之前，本协议由第一组维护。
