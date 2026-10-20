# MIDR 派生失败闭环

## 1. 范围

P5 定义 canonical RIB 到 LSDB、TED 和 SPF consumer 的失败处理语义。它不改变单实例 LS Object、owner `WITHDRAWN`、`MP_UNREACH` 或会话同步协议的语义，也不处理 FOLLOW-11-A/B、完整对象 ACK、Zebra 联调、`midrd` 抽取或 P6 端到端验收。

派生路径为：

```text
canonical RIB
-> LSDB staging state
-> TED candidate
-> immutable TED snapshot
-> SPF consumer
```

## 2. 事务和原子性

LSDB 每次处理 dirty 输入时都构造独立 staging state。membership、group、endpoint、pending-link 和 contributor 索引均在 staging 中建立；索引分配失败、重复项、非法对象组合和内部一致性错误会返回错误，不再被忽略。

TED candidate 也在提交前完整构造。只有 LSDB state 和 TED candidate 都准备成功，才会替换当前 state、发布 snapshot、执行远端 callback、清理 dirty 输入并通知同步层。任意步骤失败时销毁 staging/candidate，保留当前 LSDB、TED snapshot、generation、dirty 输入、force rebuild 和同步门槛。

派生失败不生成新的 TED generation，不触发 SPF 重算，也不改变已经安装的 SPF 路由。当前 snapshot 为 `READY` 时，诊断状态表现为 `READY + DERIVATION_PENDING`；失败恢复前仍可读取上一份 immutable snapshot。

输入未完成、缺少本地身份、缺少必要的同步条件或输入处于 `OUT_OF_SYNC` 时，属于正常的 `NOT_READY` 状态，不作为内部派生错误处理。提交 `NOT_READY` candidate 后，TED snapshot 不可供 SPF 使用。

## 3. 失败重试

失败任务不清理 dirty 队列，也不调用 `midr_sync_derivation_complete()`。事件循环为失败任务建立单一 retry timer，退避时间为：

```text
1s -> 2s -> 4s -> 8s -> 16s -> 32s -> 60s
```

达到 60 秒后保持该最大间隔。失败期间的新输入只更新待处理状态，不创建并行 timer，也不能绕过已有退避。成功提交后清除失败状态、重置退避计数和延迟，并重新检查同步完成条件。

## 4. READY、NOT_READY 和 EoR

- `READY`：最近一次派生成功，存在可供 SPF 使用的 immutable TED snapshot。
- `NOT_READY`：输入或同步门槛不满足，TED 不应被 SPF 使用。
- `READY + DERIVATION_PENDING`：canonical 输入已经变化，但新派生暂时失败；上一份 READY snapshot 继续有效。

接收 EoR 只有在此前 UPDATE 已完成本地处理、没有 pending input、没有 dirty LSDB、没有派生错误、TED 为 READY 且 session 没有 `needs_resync` 时才能完成。重复 EoR 不改变对象和 generation。EoR 不代表派生成功、撤销完成或全网同步完成。

## 5. SPF 行为

TED 从 READY 变为 NOT_READY 后，SPF runtime 清除缓存结果，并通过现有安装适配路径撤销此前安装的 MIDR SPF 路由；`midr_spf_results_get()` 返回 `-EAGAIN`。TED 因瞬时派生失败保留旧 READY snapshot 时，SPF 不触发重算，也不撤销旧路由。TED 恢复为 READY 后，SPF 从新 generation 计算并重新安装。

## 6. 诊断

`show midr lsdb summary` 输出当前 LSDB generation、对象计数、dirty 数量、派生状态、READY 状态、重试延迟、提交/失败计数和最后错误。

`show midr ted summary` 输出 TED 的 `READY`/`NOT_READY` 状态，并显示来源状态：

```text
CURRENT
DERIVATION_PENDING
```

诊断字段来自内部派生状态，不写入 immutable TED snapshot。

## 7. 验证

已覆盖以下场景：

- LSDB staging/TED prepare 失败后旧 generation、旧 snapshot、旧远端视图和 dirty 输入保持不变；
- 连续派生失败的 1 秒、2 秒退避以及成功后的重置；
- 派生未完成时接收 EoR 继续等待；
- TED `READY -> NOT_READY` 时 SPF 缓存清空并撤销 Zebra 路由；
- TED 恢复 READY 后 SPF 重新生成结果；
- IPv4/IPv6 TED、既有 LSDB、同步、SPF 和 Zebra 回归。

通过的聚焦测试包括：

```text
test_midr_lsdb
test_midr_sync
test_midr_ted
test_midr_spf_pipeline
```

既有 MIDR 测试集也已重新构建并运行通过。核心 `bgpd` 目标构建通过；构建系统的 `clippy` xref 阶段仍会在启用 LeakSanitizer 时报告 Python/ELF 扫描器自身的内存泄漏，该报告不来自 MIDR 派生代码。
