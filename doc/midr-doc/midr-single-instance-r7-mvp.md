# MIDR 单实例 LS：R7-MVP 独立 `midrd`

## 范围

R7-MVP 在 R6-A 冻结的协议中立接口上提供一个不依赖 BGP 的最小可运行 `midrd`。该阶段验证的是 MIDR 自身的对象核心、原生 UDP 传输、双地址族端点、静态本地 Prefix Provider 和三节点洪泛闭环；不把 BGP UPDATE、BGP Session、AFI/SAFI、TCP/179、TED/SPF/Zebra 或真实 IPv6 网络部署纳入 MVP 前置条件。

## 实现边界

- `midr-core.c/.h` 负责 identity 规范化、版本准入、同序列冲突、ACTIVE/WITHDRAWN、刷新、老化和事件队列。
- `midr-transport.h` 定义原生 IPv4/IPv6 endpoint 以及 HELLO、KEEPALIVE、snapshot、EoR、UPDATE、WITHDRAW 帧类型。
- `midr-prefix-provider.c/.h` 提供静态/本地 Prefix event、generation 和 snapshot 接口；`midrd --prefix` 通过该接口产生本地 NODE_PREFIX。
- `midrd.c` 使用原生 UDP socket，完成 HELLO、周期 KEEPALIVE、snapshot/EoR、增量 UPDATE/WITHDRAW、重复 HELLO 重连和本地刷新/远端过期传播。
- 所有独立目标均只包含 `midrd/` 的中立头文件，不包含 BGP 类型或 FRR `bgpd` 头文件。

## 独立构建与测试

在目标分支的独立目录执行：

```sh
make -C midrd clean
make -C midrd test
```

通过的目标包括：

```text
midrd R6-A contract: PASS
midr-core-test: PASS
midr-prefix-provider-test: PASS
```

构建产物位于 `midrd/build/`，不写入 FRR 源码树的其他目录。

## Containerlab 等价 smoke

```sh
cd midrd
./r7-smoke.sh
```

runner 不启动 `bgpd`，依次验证：

1. 三节点 IPv4 收敛：每个节点最终持有三个对象，并完成 snapshot/EoR 和 KEEPALIVE；
2. 三节点 IPv6 收敛：使用 `[::1]` 原生 UDP endpoint 验证相同闭环；
3. IPv4 生命周期：Owner 提前退出，两个存活节点在 lifetime 到期后生成并洪泛 WITHDRAWN，最终各持有两个对象。

当前 runner 输出三组 `PASS` 后再输出 `r7 smoke: PASS`。日志目录由 runner 打印并保留在远端 `/tmp`，便于检查 HELLO、KEEPALIVE、EoR、sequence、WITHDRAWN 和最终对象数。

## 验收结论与后续边界

R7-MVP 已证明 `midrd` 可以在无 BGP 的条件下独立构建和运行，IPv4 与 IPv6 均具备最小对象产生、洪泛、刷新、老化和撤销闭环。BGP Prefix IPC adapter、第一组/第三组与独立进程的联合接线、TED/SPF/Zebra、规模与 sanitizer、真实 IPv6 网络部署和 IPv4 生产回归属于 R7-hardening/部署验收，不在本 MVP 完成声明内。
