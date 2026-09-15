# MIDR R7-EXT-2 Consumer/TED/SPF 边界

## 阶段状态

R7-EXT-2 已完成。`midrd` 在不链接 `bgpd` 的条件下，将 canonical 引擎输出提交为带 generation 的完整 Consumer snapshot，并提供独立的最小 SPF route-result adapter。旧的 `bgpd` MIDR 实现仍保留，作为后续 parity gate 的差分参考。

## Consumer 提交语义

Consumer 同时保留两类接口：事件队列用于观察和传输调试，committed snapshot 用于下游 TED/SPF 读取。每次提交包含 `SNAPSHOT_BEGIN`、同一 generation 的 Link/Node Prefix/Group Prefix 事件和 `SNAPSHOT_END`；提交完成后才替换内部快照。下游通过 acquire 获得独立副本，释放前不会受后续 generation 替换影响。engine 的 batch 在 EOR 期间延迟发布，所有对象应用成功后只提交一次完整 snapshot。

## TED/SPF 适配

`midr-spf.[ch]` 只依赖中立 Consumer 类型，不引用 BGP、FRR 或 Zebra。它从 committed snapshot 提取 Node Prefix/Group Prefix，按 `(family, prefix, prefix length)` 聚合，使用最小 metric、再以 originator 作为稳定 tie-break，返回带 generation 的 route result。IPv4 和 IPv6 使用同一接口并分别测试；Link 事件继续保留在 snapshot 中，供后续完整 TED 图适配使用。

## 证据

独立门禁 `make -C midrd clean test` 通过以下目标：contract、core、Prefix Provider、wire、双地址族 transport、Consumer committed snapshot、SPF route-result、engine batch/withdraw 以及 standalone boundary scan。`./midrd/r7-smoke.sh` 的 IPv4 convergence、IPv6 convergence 和 IPv4 expiry 三项 containerlab smoke 均通过，运行时不启动 `bgpd`。

## 后续闸门

下一阶段为 R7-EXT-3：实现 Prefix Feed IPC 的 snapshot/upsert/withdraw/EoR、断线重连和双地址族测试，并把当前开发 daemon 接入正式安装和生命周期目标。`bgpd` 清理仍推迟到 R7-hardening、R5-full-B、R6-B-B 和 parity gate 全部通过之后。
