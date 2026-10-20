# 08 MIDR 部署汇报与录屏台本

## 1. 整体拓扑与节点分工
本部分主要展示 MIDR_fudan 到 MIDR_wlq 的路由结果。

## 2. Overlay 拓扑与路由展示




选择复旦作为源节点、乌鲁木齐业务前缀作为目标，页面按照快照中逐台节点的已安装路由高亮路径。该路径为复旦、武汉 R1、北航和乌鲁木齐 MIDR；到达乌鲁木齐后，再由本站 BGP 路由器送往业务终端。它不是把复旦的一条路由当作完整路径，而是依次读取各节点的下一跳直到业务出口。

## 3. 命令行录屏：复旦到乌鲁木齐

### 3.1 目标、顺序和操作范围

目标终端：`240a:a00f:300::10`；匹配的业务前缀：`240a:a00f:300::/48`；业务源：复旦 MIDR 地址 `240a:a00f:100:1::2`。

录像顺序：逐跳确认 Overlay 路由、分段展示 underlay 路径、验证真实业务连通。整段演示仅查询状态和发送少量探测包，不停止服务、不重放 node.commands、不修改群组、隧道或路由。

| 步骤 | 节点 | SSH 端口 | 当前业务出口 | 对应下一跳 |
|---|---|---:|---|---|
| Overlay 1 | MIDR_fudan | 15503 | mgre-6a000000，proto 199 | MIDR-R1_wh |
| Overlay 2 | MIDR-R1_wh | 13006 | mgre-66000000，proto 199 | MIDR_buaa |
| Overlay 3 | MIDR_buaa | 15003 | mgre-67000000，proto 199 | MIDR_wlq |
| 业务出口 | MIDR_wlq | 29502 | ens5，BGP 路由 | 本站 BGP-R_wlq |

以上是 2026-10-08 现场核对结果，不保证未来路径固定。录制时若某一步的下一跳改变，后续应沿新的实际下一跳检查，不拼接不同状态下的结果。

### 3.2 进入管理机

在本地终端执行：

```bash
ssh NetArchLab90
```

下面每个 SSH 命令均从 90 执行。每台节点的最后一条 `exit` 返回 90，不停止服务。

### 3.3 Overlay 第一跳：复旦到武汉 R1

```bash
ssh -p 15503 root@118.229.43.234
hostname
date -Iseconds
export MIDR_DIR=/opt/midr-current
export LD_LIBRARY_PATH="$MIDR_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

"$MIDR_DIR/vtysh" --vty_socket /run/midr -d midrd \
  -c 'show midr spf' | grep -F '240a:a00f:300::/48'

ip -6 route show proto 199 | grep -F '240a:a00f:300::/48'

ip -6 route get 240a:a00f:300::10 \
  from 240a:a00f:100:1::2

"$MIDR_DIR/vtysh" --vty_socket /run/midr -d midrd \
  -c 'show midr virtual-links'

exit
```

讲解台本：

复旦的 SPF 为乌鲁木齐业务前缀选择节点 106，也就是武汉 R1。内核路由查询显示实际使用 proto 199 和 mgre-6a000000；虚链路表进一步确认该接口的外层远端地址为 240a:a00f:800:1::2。这三项对应同一个下一跳。

核对字段：

```text
SPF：300::/48 ... via fe80::6a00:0:6500:0 (node 106.0.0.0)
FIB：dev mgre-6a000000 proto 199
映射：mgre-6a000000，node 106.0.0.0
outer：240a:a00f:100:1::2 -> 240a:a00f:800:1::2
```

### 3.4 Overlay 第二跳：武汉 R1 到北航

```bash
ssh -p 13006 root@118.229.43.234
hostname
export MIDR_DIR=/opt/midr-current
export LD_LIBRARY_PATH="$MIDR_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

"$MIDR_DIR/vtysh" --vty_socket /run/midr -d midrd \
  -c 'show midr spf' | grep -F '240a:a00f:300::/48'

ip -6 route show proto 199 | grep -F '240a:a00f:300::/48'

ip -6 route get 240a:a00f:300::10 \
  from 240a:a00f:100:1::2 iif mgre-65000000

"$MIDR_DIR/vtysh" --vty_socket /run/midr -d midrd \
  -c 'show midr virtual-links'

exit
```

讲解台本：

业务包到达武汉 R1 后，仍以乌鲁木齐终端为目的地址。武汉 R1 的已安装路由将它继续送往北航，出接口为 mgre-66000000，外层目的地址为 240a:a00f:200:1::2。

核对字段：

```text
SPF：300::/48 ... via fe80::6600:0:6a00:0 (node 102.0.0.0)
FIB：dev mgre-66000000 proto 199
映射：mgre-66000000，node 102.0.0.0
outer：240a:a00f:800:1::2 -> 240a:a00f:200:1::2
```

`from` 保留原始业务源，`iif` 指定从复旦进入的虚拟接口；这条查询模拟转发流量，不是武汉 R1 自己发起的新业务。

### 3.5 Overlay 第三跳：北航到乌鲁木齐

```bash
ssh -p 15003 root@118.229.43.234
hostname
export MIDR_DIR=/opt/midr-current
export LD_LIBRARY_PATH="$MIDR_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

"$MIDR_DIR/vtysh" --vty_socket /run/midr -d midrd \
  -c 'show midr spf' | grep -F '240a:a00f:300::/48'

ip -6 route show proto 199 | grep -F '240a:a00f:300::/48'

ip -6 route get 240a:a00f:300::10 \
  from 240a:a00f:100:1::2 iif mgre-6a000000

"$MIDR_DIR/vtysh" --vty_socket /run/midr -d midrd \
  -c 'show midr virtual-links'

exit
```

讲解台本：

北航的路由将相同业务目的送往乌鲁木齐 MIDR。对应接口是 mgre-67000000，外层远端地址是 240a:a00f:300:1::2。到这里，复旦、武汉 R1、北航、乌鲁木齐三个 Overlay 段都有逐跳路由和接口映射证据。

核对字段：

```text
SPF：300::/48 ... via fe80::6700:0:6600:0 (node 103.0.0.0)
FIB：dev mgre-67000000 proto 199
映射：mgre-67000000，node 103.0.0.0
outer：240a:a00f:200:1::2 -> 240a:a00f:300:1::2
```

### 3.6 乌鲁木齐：从 Overlay 到终端

```bash
ssh -p 29502 root@118.229.43.234
hostname
export MIDR_DIR=/opt/midr-current
export LD_LIBRARY_PATH="$MIDR_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

"$MIDR_DIR/vtysh" --vty_socket /run/midr -d midrd \
  -c 'show midr spf' | grep -F '240a:a00f:300::/48'

ip -6 route get 240a:a00f:300::10 \
  from 240a:a00f:100:1::2 iif mgre-66000000

"$MIDR_DIR/vtysh" --vty_socket /run/midr -d zebra \
  -c 'show ipv6 route 240a:a00f:300::/48'

exit
```

讲解台本：

乌鲁木齐将该业务前缀作为本站出口发布到 MIDR。业务包在这里离开 Overlay，内核选择 ens5 上的本站 BGP 路由，再由乌鲁木齐 BGP 路由器送到终端。这里的 local 表示本节点发布的出口，不代表终端直接接在 MIDR 上。

### 3.7 分段展开 underlay

讲解台本：

下面分别追踪三个 GRE 段的外层端点，展示这些 Overlay 逻辑边实际由哪些 underlay 路由器承载。该探测补充说明外层路径；Overlay 业务选路本身由前面的逐跳 FIB 和虚链路映射确认。

在 90 上依次执行下列四组命令。`route get` 分别核对 GRE 外层报文（IPv6 Next Header 47）和 UDP 探测包到同一外层目的地址时的路由；随后用 `traceroute` 展开该 underlay 目的地址的逐跳响应。Traceroute 发的是 UDP 探测包，不是 GRE 报文逐跳抓取；它展示 underlay 到 GRE 外层端点的路由路径，实际 GRE 报文的出口和外层目的地址由 47 号协议的查询确认。`traceroute` 使用节点已有的工具，无需安装或修改网络。

每一段的两个 `route get` 预期都指向同一个本站 underlay 网关和出口接口。若 47 号协议查询与 UDP 查询出现不同出口，先记录差异并检查 `ip -6 rule show`、`ip -6 route show table 1990`，不要把 UDP traceroute 结果直接当作 GRE 外层的实际路径。

第一段，复旦到武汉 R1：

```bash
ssh -p 15503 root@118.229.43.234
hostname
ip -6 route get 240a:a00f:800:1::2 \
  from 240a:a00f:100:1::2 ipproto 47
ip -6 route get 240a:a00f:800:1::2 \
  from 240a:a00f:100:1::2 ipproto udp dport 33434
traceroute -6 -n -s 240a:a00f:100:1::2 \
  -q 1 -w 2 -m 16 240a:a00f:800:1::2
exit
```

第二段，武汉 R1 到北航：

```bash
ssh -p 13006 root@118.229.43.234
hostname
ip -6 route get 240a:a00f:200:1::2 \
  from 240a:a00f:800:1::2 ipproto 47
ip -6 route get 240a:a00f:200:1::2 \
  from 240a:a00f:800:1::2 ipproto udp dport 33434
traceroute -6 -n -s 240a:a00f:800:1::2 \
  -q 1 -w 2 -m 16 240a:a00f:200:1::2
exit
```

第三段，北航到乌鲁木齐：

```bash
ssh -p 15003 root@118.229.43.234
hostname
ip -6 route get 240a:a00f:300:1::2 \
  from 240a:a00f:200:1::2 ipproto 47
ip -6 route get 240a:a00f:300:1::2 \
  from 240a:a00f:200:1::2 ipproto udp dport 33434
traceroute -6 -n -s 240a:a00f:200:1::2 \
  -q 1 -w 2 -m 16 240a:a00f:300:1::2
exit
```

业务出口，乌鲁木齐 MIDR 到终端：

```bash
ssh -p 29502 root@118.229.43.234
hostname
ip -6 route get 240a:a00f:300::10 \
  from 240a:a00f:300:1::2 ipproto 47
ip -6 route get 240a:a00f:300::10 \
  from 240a:a00f:300:1::2 ipproto udp dport 33434
traceroute -6 -n -s 240a:a00f:300:1::2 \
  -q 1 -w 2 -m 16 240a:a00f:300::10
exit
```

2026-10-08 实测结果：

| 段 | traceroute 的逐跳响应地址 | 对应设备顺序（含源节点） |
|---|---|---|
| 复旦 → 武汉 R1 | 100:1::1 → f000:812::f008 → 800:1::2 | MIDR_fudan → BGP-R_fudan → BGP-R_wh → MIDR-R1_wh |
| 武汉 R1 → 北航 | 800:1::1 → f000:811::f011 → 200:1::2 | MIDR-R1_wh → BGP-R_wh → BGP-R_buaa → MIDR_buaa |
| 北航 → 乌鲁木齐 | 200:1::1 → f000:211::f002 → f000:23::f003 → 300:1::2 | MIDR_buaa → BGP-R_buaa → BGP-R_sh → BGP-R_wlq → MIDR_wlq |
| 乌鲁木齐 MIDR → 终端 | 300:1::1 → 300::10 | MIDR_wlq → BGP-R_wlq → Terminal_wlq |

表中响应地址均省略共同前缀 `240a:a00f:`。一个路由器可能以某个接口地址应答；同一台 BGP-R 可以出现在相邻两个 GRE 段中，不能把每个响应地址都当作额外的一台设备。

这四段串联得到当前去向的展开路径：

```text
MIDR_fudan
  -> BGP-R_fudan -> BGP-R_wh -> MIDR-R1_wh
  -> BGP-R_wh -> BGP-R_buaa -> MIDR_buaa
  -> BGP-R_buaa -> BGP-R_sh -> BGP-R_wlq -> MIDR_wlq
  -> BGP-R_wlq -> Terminal_wlq
```

UDP 探测由本机 locator 发出，命中 table 1990 的 underlay 路由。它与 GRE 外层使用同一 underlay 出口配置，但协议不同；若存在 ECMP 或设备策略差异，不能仅凭 UDP traceroute 保证每个实际 GRE 包经过完全相同的物理跳点。中间出现 `*` 表示没有获得可识别响应，不能补写未经核实的节点。

### 3.8 真实业务连通与 BGP 基线

回到复旦，用 ICMP 验证本机业务路由和终端连通：

```bash
ssh -p 15503 root@118.229.43.234
hostname
ip -6 route get 240a:a00f:300::10 \
  from 240a:a00f:100:1::2
ping -6 -c 3 -W 2 -I 240a:a00f:100:1::2 240a:a00f:300::10
```

讲解台本：

复旦到乌鲁木齐真实终端的业务路由使用 MIDR 虚拟接口，Ping 检查往返连通。前面的逐跳查询展示去向路径，Ping 本身不证明回程一定沿相同路径。

可在同一 SSH 会话中展示绕过 Overlay 的 underlay 基线：

```bash
ip -6 route get 240a:a00f:300::10 \
  from 240a:a00f:100:1::2 ipproto udp dport 33434
traceroute -6 -n -s 240a:a00f:100:1::2 \
  -q 1 -w 2 -m 16 240a:a00f:300::10
exit
```

2026-10-08 实测基线为：

```text
MIDR_fudan -> BGP-R_fudan -> BGP-R_sh -> BGP-R_wlq -> Terminal_wlq
```

讲解台本：

直接使用 underlay 时，复旦经上海 Tier-1 到乌鲁木齐。MIDR 的去向路径先经过武汉 R1 和北航，再到乌鲁木齐，说明已安装的 MIDR 路由改变了业务的 Overlay 选择。当前路径仍有一段经过 Tier-1，且不据此宣称性能优于 BGP 基线。这个对照利用现有策略路由绕过 Overlay，没有关闭 MIDR，也不属于相同协议下的 MIDR 开关实验。

当前业务 ICMP traceroute 因 GRE 跳数继承和 ICMP 超时回复问题可能只显示终端、看不到中间 MIDR 节点。本演示不使用该输出还原 Overlay 路径，也不通过停止 midrd 来制作对比。

### 3.9 可选：抓包确认真实业务经过中间节点

2026-10-08 的核对中，复旦 mgre-6a000000、武汉 R1 mgre-66000000、北航 mgre-67000000、乌鲁木齐 mgre-66000000 均捕获到同一组 Ping 请求，ICMP 标识和序号一致；终端返回 3 个响应、零丢包。这项证据确认实际业务经过上述 Overlay 路径，不仅是路由表中存在对应条目。

录屏可选取武汉 R1 作为中间观察点。打开两个登录 90 的终端窗口：

窗口 A，先开始抓包，最多等待 20 秒，收到 3 个请求即退出：

```bash
ssh -p 13006 root@118.229.43.234 \
  "timeout 20 tcpdump -nn -l -i mgre-66000000 -c 3 \
  'icmp6 and src host 240a:a00f:100:1::2 and dst host 240a:a00f:300::10'"
```

窗口 B，随后发送业务 Ping：

```bash
ssh -p 15503 root@118.229.43.234 \
  'ping -6 -c 3 -W 2 -I 240a:a00f:100:1::2 240a:a00f:300::10'
```

讲解台本：

武汉 R1 的出站虚拟接口捕获了这组从复旦发往乌鲁木齐终端的业务请求，证明业务实际经过中间 MIDR 节点；目的终端的响应同时确认业务连通。

## 4. 已完成与待完成任务

### 4.1 功能状态

| 功能 | 状态 | 主责建议 |
|---|---|---|
| 邻居发现、Bootstrap 引导与自动建连 | 已完成 12 节点冷启动验收 | 第一组 |
| 自动分群与跨群邻接策略 | 基础流程已完成，分群和代表选择仍需优化 | 第一组 |
| 链路状态传播与 SPF 计算 | 已完成当前 12 节点场景验收 | 第二组 |
| MIDR 路由安装和 Overlay 业务转发 | 已完成 MIDR 节点到远端业务前缀的验证 | 第三组 |
| 从本站 BGP 获取外部前缀 | 已完成，包含 AS_PATH 过滤 | 第三组 |
| 节点更新后的自动恢复入群 | 尚未完成 | 第一组，部署编排配合 |
| BGP 反向发布与普通终端进入 MIDR | 尚未完成；需要将 MIDR 路由受控发布到本站 BGP-R | 第三组 |
| 路由回灌防护（防环） | 随 BGP 反向发布实现和验证 | 第三组 |
| avoid-tier1 策略 | 功能尚未启用和验收 | 第一组 |
| 分群和路由策略在不同链路条件下的表现 | 尚未完成受控实验 | 第一组、第二组及环境维护方 |

责任分工以邻居关系构建第一组、链路状态传播与 SPF 第二组、路由安装第三组为边界；上表为任务主责建议，完成时间需按各组工作量确认。

### 4.2 总结台本

当前已在 12 台 MIDR 节点上完成冷启动、自动发现、分群、链路状态传播、SPF 计算、GRE 创建和业务路由安装，并验证由 MIDR 节点发起的真实业务能够转发。

后续工作分为三组：第一组完善邻居发现、自动入群恢复和分群策略，并启用、验证 avoid-tier1；第二组负责链路状态传播与 SPF，并在不同链路条件下检查计算结果；第三组完成 MIDR 路由安装与 BGP-R 反向发布，使普通终端可以经边缘 MIDR 进入网络，同时验证路由回灌防护。

当前验收范围是 MIDR 核心流程及从 MIDR 节点发起的数据面转发。普通终端自动接入、更新后的自动恢复和避开 Tier-1 的效果尚未计入已完成范围。
