# MIDR R7-EXT-3 Prefix IPC 与正式 daemon 生命周期

## 阶段状态

R7-EXT-3 已完成。`midrd` 可在没有 `bgpd` 的进程环境中运行，Prefix Provider 通过本地 Unix stream IPC 输入中立 Prefix 事件；BGP 如需接入，只能作为外部 adapter 向该 IPC 发布事件。

## Prefix Feed IPC

IPC 使用固定长度、带 magic/version/generation/originator 的帧，承载 `SNAPSHOT_BEGIN`、`UPSERT`、`WITHDRAW`、`SNAPSHOT_END` 和 `EOR`。服务端支持单客户端断开、重新连接和重同步；收到 begin 后 engine 进入 batch，EOR 时统一提交 Consumer snapshot。Prefix 的 family、长度、地址和 metric 均以协议字段编码，IPv4 与 IPv6 共用同一校验和传输路径。

## daemon 生命周期

命令行支持 `--prefix-socket PATH` 接收 Prefix Feed，`--pidfile PATH` 写入进程号；SIGINT/SIGTERM 触发受控退出，退出前撤销本地 Prefix、停止 IPC 和原生 MIDR transport，并删除 pidfile。现有 `--prefix ADDRESS/LEN` 仍可作为独立静态 Provider，便于无外部控制面的开发和 containerlab smoke。顶层 `Makefile.am` 注册 standalone `midrd` 安装目标，`midrd/Makefile` 保留独立开发构建和 `make install`。

## 证据

`make -C midrd clean test` 通过 contract、core、Prefix Provider、wire、双地址族 transport、Consumer committed snapshot、SPF、engine batch、Prefix IPC 和 standalone boundary scan。Prefix IPC daemon smoke 验证 IPv6 Prefix 经 Unix stream 输入后产生 Consumer/SPF route result，并在 runtime 到期后清理 pidfile。`./midrd/r7-smoke.sh` 的 IPv4 convergence、IPv6 convergence 和 IPv4 expiry 继续通过，所有场景不启动 `bgpd`。

## 清理闸门

EXT-3 只完成独立模块运行边界，不删除 `bgpd` 旧实现。后续顺序固定为 `R7-hardening`、`R5-full-B`、`R6-B-B`、parity gate，最后才执行 `R7-CLEANUP`。
