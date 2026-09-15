# MIDR 内置 UDP traceroute 与 Tier-1 ASN 文件

实现日期：2026-09-15。基于 `d1195d2d607b0a5a21be4c97f1f2160c319d6bf4` 的工作树修改，尚未提交。

## 功能与边界

- Linux IPv4/IPv6 UDP traceroute 在 bgpd 内执行，使用 FRR 的 VRF socket 封装、TTL/Hop Limit 设置、非阻塞读事件和定时器。
- 每 job 顺序探测 TTL 1–30，每跳一个 UDP probe，等待 1000 ms；多个 job 沿用原 scheduler 的并发、队列、single-flight 和轮询/回调。
- 全进程发送尝试之间至少 10 ms，无突发额度；探测期间仍受默认 35 秒整体 timeout 限制。排队 timeout 独立计算。
- 每 probe 使用不同高目的端口，原目标/端口和可用 payload 必须匹配；迟到、重复及无法关联的数据不修改已结算 hop。
- 端口按地址族分配，范围 33434–65535。每个端口预留 240 秒，覆盖配置允许的最长 120 秒 job 和额外 120 秒冷却；无可用端口形成 `resource-error`。该上界与整体 timeout 校验必须一起维护。
- 仅默认网络上下文，不支持 VRF 参数、源接口、源地址选择、带 zone 的 IPv6 link-local、TCP/ICMP Echo 或 Paris traceroute。不会复用 BGP TCP 连接或 PM echo socket。
- 移除了外部 executor、SIGCHLD、管道解析和 `--with-midr-traceroute`。Linux 错误队列不可用时不回退外部命令。

`ipv4Supported/ipv6Supported` 表示编译期接口可用。内核、权限或网络策略导致的运行期 socket/发送错误会作为具体 job 结果报告；这些字段不表示已进行联网自检。

## 配置 Tier-1 清单

文件格式：

```text
MIDR-TIER1-ASNS 1
LIST-ID operator-policy-20260915
# 示例仅演示格式；请使用自行维护的完整策略清单
174
1299
```

文件规则：前两条非空非注释行依次为版本头和 LIST-ID，后续每行一个十进制 ASN。支持 ASCII 空白和整行注释；不支持行尾注释、asdot、AS 前缀、逗号、范围。拒绝 ASN 0、当前核心定义的私有 ASN、数值溢出与空列表。重复 ASN 去重。

LIST-ID 最长 64 字符，允许 ASCII 字母、数字及 `._:-`。文件须为普通文件，最多 128 KiB、每行 256 字节（不含换行）、最多 4096 条 ASN 记录（包括重复记录）。不自动下载或内置生产编号。

```text
configure terminal
 midr ip2asn file /var/lib/midr/prefix2as.txt
 midr tier1 file /var/lib/midr/tier1-asns.txt
end

show midr tier1 list
show midr tier1 list json

! ENABLE 模式预检，不改变活动清单
midr tier1 validate file /var/lib/midr/next-tier1-asns.txt
```

重新执行 `midr tier1 file <path>` 即重新读取并整批替换。失败保留旧清单、文件路径和 generation；预检不改变活动状态。`no midr tier1 file` 清空清单，后续 Tier-1 查询返回 `tier1-list-not-loaded`，不会以 false 代替“无法判定”。

`write memory` 保存文件路径，不保存文件内容。重启时文件必须存在且 bgpd 可读；文件内容由部署流程维护。generation 为进程内激活序号，不跨重启保持。

## 查询

```text
show midr traceroute 8.8.8.8 json
show midr traceroute 2001:4860:4860::8888 refresh json
show midr traceroute job 12 json
show midr traceroute scheduler json

show midr tier1 8.8.8.8 json
show midr tier1 job 12 json
show midr tier1 8.8.8.8 observed-as-path 64512 174 0 1299
```

首次 cache miss 返回 job ID，仍需通过 job 查询获取完成结果。手工 observed-as-path 仍只有文本输出，不要求 IP2ASN 或探测能力，但需要已加载 Tier-1 列表。

traceroute 查询沿用原有“先加载 IP2ASN”的 scheduler 前置要求。Tier-1 清单缺失不影响单独的 IP2ASN 或 traceroute 查询。

原 `rawHops`、`observedAsPath`、job ID 和 IP2ASN generation 保留。traceroute 新增 `backend=linux-udp`、profile version 2、`targetReached`、`stopReason`、`systemErrno`，以及与 rawHops 对齐的 `hopDetails`（TTL、可见性、可选 RTT 微秒和 ICMP type/code）。原 child exit/signal 字段已移除。

`status=ok` 表示测量流程完成。`stopReason=max-hops` 或 `unreachable` 时可以没有到达目标，甚至所有跳都不可见；`targetReached` 单独表示是否收到来自目标的匹配 port-unreachable。无响应跳保留为 0；本地发送/接收错误和整体 timeout 不输出有效的 Tier-1 否定结论。

Tier-1 结果增加 `tier1ListGeneration`，版本以结果自有字符数组保存，重载/清空后旧结果不会持有已释放版本指针。自动判定和 job poll 使用查询时的当前 IP2ASN 与当前 Tier-1 清单，双 generation 可追踪；raw cache 不保存 ASN 或 Tier-1 结论。

`tier1Observed=false` 仅表示已观测路径未命中当前清单，不是全路径不存在 Tier-1 的证明。两个数据文件独立加载，不提供跨文件联合事务。

## 代码位置

| 文件 | 职责 |
|---|---|
| `bgpd/midr_trace_engine.[ch]` | TTL 推进、限速、端口分配、事件与 probe 超时、结果完成 |
| `bgpd/midr_trace_udp.[ch]` | Linux UDP socket/错误队列、目标和控制消息验证 |
| `bgpd/midr_trace_types.h` | 公共 raw hop/result/status 定义 |
| `bgpd/midr_trace_scheduler.[ch]` | 现有 request/job 管理、queue/cache/history、统一 finalize |
| `bgpd/midr_trace_observer.[ch]` | 查询时 IP2ASN 映射、cache-only 兼容 observer |
| `bgpd/midr_tier1_list.[ch]` | 清单文件读取、候选快照替换、状态和配置写回 |
| `bgpd/midr_tier1.[ch]` | 原判定算法，删除默认种子、修复结果版本所有权 |
| `bgpd/midr_tier1_vty.c` | 配置、预检、状态、查询和输出 |

旧 backend 专有配置字段已从 C 结构移除；历史 error 枚举数值保留为 reserved，新代码不发布这些旧错误。内部 C 结构有变化，使用方应随 FRR 源码一起重新编译。

相对设计文档的实现取舍：abort 合并在幂等 `engine_destroy()` 中，公开引擎 API 没有另设 abort；profile 为固定常量；端口采用保守固定预留；没有新增 probe 级累计统计、MTU 展示或独立 load 时间审计。RTT/ICMP 诊断逐跳保留，不影响两个主需求。

## 测试与本轮检查

恢复原 Tier-1/IP2ASN 和事务更新测试，并增加：

- `test_midr_tier1_list`：格式拒绝、去重、预检、替换、清空、版本拷贝。
- `test_midr_trace_udp`：注入 IPv4/IPv6 ancillary data，验证端口、payload、截断、最短 quote、本地错误和目标到达。
- `test_midr_trace_engine`：真实 FRR event loop 配合 fake UDP transport，验证逐跳推进、缺失跳、发送失败和取消启动。
- `test_midr_trace_scheduler`：fake engine 验证 single-flight、单 request 取消、slot 释放、cache 延迟交付及回调内 fini。

新旧适用测试均加入 Automake，并通过 Python wrapper 注册到现有 FRR 测试框架。恢复的旧测试中，“仅私有 ASN 忽略就一定语义降级”的断言与当前核心掩码不符，已调整断言以保持原核心实际行为；未修改该过滤算法。旧文本 parser 测试不适用于内置后端，未恢复。

本机无 FRR 构建环境，本轮没有执行 configure、编译、C 单元测试或真实网络探测。

可在仓库根目录执行只读静态检查：

```text
python tools/check_midr_native_static.py
git diff --check
```

静态脚本检查：C 词法括号/预处理结构、本地 include、公共函数实现存在性、旧执行器/外部命令残留、构建注册、CLI 声明/注册和 Python 语法。它不替代 C 编译器，也不验证内核错误队列实际唤醒行为。

后续 Linux 环境仍需验证：configure 的 errqueue 检测、bgpd/vtysh 编译、上述 C 测试、真实 IPv4/IPv6 多跳、仅 EPOLLERR 的 read 唤醒与重新注册、目标屏蔽 ICMP、极端并发/端口资源、超时与 shutdown 竞态。当前结论只限于静态检查。
