# MIDR R6-A 独立抽取接口冻结

状态：接口契约已冻结；R7-MVP 运行时实现尚未开始。

## 目标与边界

R6-A 只冻结独立 midrd 的公共边界，不实现完整 daemon，不接入 BGP Prefix IPC，不接入第三组 TED/SPF/Zebra。R7-MVP 必须能够在没有 bgpd、BGP OPEN/UPDATE、TCP/179 和 AFI/SAFI 的情况下运行原生 MIDR。

边界固定为：

```text
midr-core
  identity / canonical / sequence / ACTIVE-WITHDRAWN / lifetime
  scope / flood / LSDB / TED 事件

midr-transport
  原生 IPv4/IPv6 endpoint、MIDR frame、HELLO、KEEPALIVE
  snapshot、EoR、增量更新、withdraw、重连

midr-prefix-provider
  static/local Prefix event、snapshot、generation

midr-consumer（可选）
  中立 TED/path result event；后续由第三组适配器消费

bgpd adapter（后续）
  bgpd RIB -> 中立 Prefix event -> IPC -> midrd
```

公共头文件位于 midrd/，分别为 midr-core.h、midr-transport.h、midr-prefix-provider.h 和 midr-consumer.h。这些头文件不包含 FRR/BGP/socket 私有类型；所有输入均为调用方持有的值，回调中的 frame/event 只在回调期间有效，异步实现必须复制。

## 冻结的语义

- midr_core_identity 是 canonical key；Node Prefix 和 Group Prefix 显式携带 IPv4/IPv6 family 与 prefix length，Link/Membership 不携带地址族前缀。
- sequence 单调准入；同版本同语义是重复，同版本异语义是冲突，低版本不回退。
- ACTIVE 与 WITHDRAWN 都是可洪泛对象状态；生命周期由单调毫秒时间和对象 lifetime 决定。
- midr_core_event_next() 是一次性值复制事件出口；接受的更新只发出一次，失败不得产生半事件。
- transport endpoint 使用固定 16 字节地址区、family、port 和 IPv6 scope id；IPv4/IPv6 是同一接口的两种合法地址族。
- transport frame 只承载 MIDR frame type、版本、flags、sequence 和不透明 payload；snapshot/EoR 与增量 UPDATE/WITHDRAW 明确区分。
- Prefix Provider 以 generation 标记 snapshot；provider 不向 core 暴露 BGP 类型，后续 IPC 只转换为同样的 Prefix event。
- Consumer 只接收中立 TED 事件；R6-A 不规定 SPF 算法、Zebra API 或 FIB 行为。

## 禁止反向依赖

冻结接口不得出现 struct bgp、struct peer、struct bgp_path_info、BGP attribute、MP_REACH/MP_UNREACH、Adj-RIB-Out、update-group、selected path、AFI/SAFI、TCP/179 或 BGP session FSM。BGP adapter 只能在进程外或独立适配层完成；midrd 退出不得要求 bgpd 存活。

## 验证

midrd/Makefile test 编译并运行 midrd-contract-test。该测试只包含四个公共头文件，构造 IPv4/IPv6 Prefix、endpoint、MIDR object、provider event 和 consumer event；它不链接 bgpd，也不启动 socket。R7-MVP 再分别为 core、transport、provider 和双地址族 containerlab smoke 增加实现测试。
