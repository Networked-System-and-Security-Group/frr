# MIDR P4：重同步、恢复与正常退网

## 当前状态

分支：`feat/yhy-midr-single-instance-ls-flooding`。
基线：`99498d5eab`。

P4 的生产代码和组件验证已经完成，真实多进程退网/重连联调仍未使用 P4 镜像执行。

## 已实现部分

每次建立 MIDR 会话创建独立的同步状态和递增 generation，不继承 peer 上一次连接的 EoR 标志。初始同步屏障记录对应的 session generation。已终止会话的 EoR 不解除新会话的等待。

会话建立或发送失败后的重同步复用 FRR 的 Adj-RIB-Out 全表通告流程。同步层为每个会话创建新的 snapshot generation；MIDR update subgroup 在生成全表通告前后标记快照范围，实际排入 socket 的 stream 保存完整报文字节，后续 RIB 变化不会修改已经排队的内容。同步期间的新变化继续进入 FRR 的增量通告队列。

MIDR UPDATE 和 EoR 入队时登记发送记录。I/O 线程仅在整个报文写出或丢弃时提交完成通知，由主线程更新同步状态。登记表通过互斥锁保护，完成通知持有独立的会话 token；会话清理会取消未处理通知并释放记录。通知不再跨线程解引用 MIDR context，也不再用已释放 stream 的地址查找主线程对象。发送失败会把会话置为 `RESYNC`，重新触发该 peer 的全表通告。

发送 EoR 前检查本地输入、待发送 UPDATE、待处理输入以及路由通告任务。普通地址族的 EoR 不计入 MIDR 发送记录。完整写出仅表示本地 socket 写出完成，不表示对端接受。

接收 EoR 后，需等待当前输入处理结束、LSDB dirty 队列清空且派生无错误。LSDB 成功处理后重新检查接收同步门槛。发送丢弃或输入处理失败时记录 `RESYNC`，不报告本次同步成功，并在会话仍存活时重新触发全表通告。

正常退网先停止新的本地身份通告并生成 owner 高版本撤销，然后启动有界等待。等待条件包括撤销产生的 MIDR 通告任务已生成、队列已清空、所有相关报文已完整写入 socket；达到五秒期限后记录 `DEGRADED`，再按会话台账拆除会话。重入或进程销毁会取消等待事务。socket 写出不等同于 TCP ACK 或对端应用接受。

`show midr sync` 增加活动会话、同步中会话、远端 EoR、本地 EoR 写出、接收处理完成、需要重同步、待处理 UPDATE/输入和 session generation 等统计。

## 已执行验证

2026-09-11 在当前分支执行 clean build，使用 `ASAN_OPTIONS=detect_leaks=0` 绕过仓库 Clippy Python 运行时的环境泄漏检查，成功构建 `bgpd/bgpd`、`vtysh/vtysh` 和 MIDR 组件测试。

以下组件测试全部通过：

- `test_midr_attr`、`test_midr_codec`、`test_midr_cost`、`test_midr_input`
- `test_midr_instance`、`test_midr_ls_object`、`test_midr_lsdb`、`test_midr_owned`
- `test_midr_packet`、`test_midr_prefix`、`test_midr_resync`、`test_midr_rib`
- `test_midr_safi`、`test_midr_scope`、`test_midr_sequence`、`test_midr_spf`
- `test_midr_spf_install`、`test_midr_spf_pipeline`、`test_midr_sync`、`test_midr_ted`
- `test_midr_zebra`

重同步场景驱动器 `midr-test/run-resync-scenarios.sh all` 全部通过：

- `M2-INPUT-001`：Snapshot 与并发增量的原子性
- `M2-INPUT-002`：Provider `EAGAIN` 重试
- `M2-INPUT-003`：非法 Snapshot 的回滚
- `M2-INPUT-004`：重同步队列溢出回滚
- `M2-INPUT-005`：Router-ID 身份重启

生产代码中已无 `Propagation Path` 的引用。`MP_UNREACH`、owner 高版本 `WITHDRAWN`、会话 generation、发送完成通知、重启自源版本观察和正常退网状态机均已纳入代码级测试。

新增同步测试覆盖延迟发送完成通知、连接及 stream 地址复用、遗留 EoR 标志、EoR 写出、输入失败、LSDB 派生失败重试、上下文销毁以及 I/O 通知与会话销毁的线程竞争。

本轮 ASAN 运行设置 `detect_leaks=0`，不计作 LeakSanitizer 或完整 Sanitizer 验收。sequence 测试中的持久化拒绝为故障注入预期结果；PM 未配置 transport-address 的提示来自组件 fixture。直接运行测试需要设置 `LD_LIBRARY_PATH` 指向仓库的 `lib/.libs`，否则只会因为找不到 `libfrr.so.0` 启动失败。

## 真实联调状态

当前 `clab-midr-backbone-g123-*` 共 15 个容器仍运行两周前的镜像 `frr-midr-g123:c7523f87b8`，不是本分支 P4 工作区构建的镜像。因此该台子的 `show midr sync` 为 `READY` 只能作为已有集成环境基线，不能作为 P4 的重连、重启恢复或正常退网验收。

P4 真实联调仍待在本分支构建镜像后执行以下三项：

1. 断开并重连一个 MIDR peer，核对新的 session generation、当前 ACTIVE/WITHDRAWN 全量同步和 EoR 顺序。
2. 在同步期间注入一次对象更新，核对增量不丢失且旧会话事件不污染新会话。
3. 执行一次 `midr shutdown`，核对 owner 高版本 `WITHDRAWN` 先完成写出或进入有界 `DEGRADED`，随后释放目标会话。

因此当前可以标记为“P4 代码与组件测试完成”，不能标记“P4 真实多进程联调完成”。FOLLOW-11-A、FOLLOW-11-B、P5、P6 和 P7 的状态不变。
