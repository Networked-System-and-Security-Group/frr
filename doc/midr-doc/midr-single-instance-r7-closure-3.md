# MIDR R7 closure-3：owned、sequence、老化与 Prefix 生命周期

## 阶段状态

R7-closure-3 已完成。该阶段在独立 `midrd` 中补齐本地 owned store、跨进程 sequence 高水位、远端老化和 Prefix snapshot 的生命周期边界；不引入 BGP 依赖，也不改变现有 TCP frame 或 Consumer 接口。

## 实现

- `midr-owned.[ch]` 保存本地权威对象和 ACTIVE/WITHDRAWN 状态，按 identity 分配单调 sequence。生成、刷新和撤销均先持久化新的 sequence，再通过中立回调提交到 engine；提交失败时保留旧对象、旧 advertised 语义和刷新游标，后续调用可重试。
- sequence 高水位可由 `--sequence-file PATH` 配置。文件使用临时文件加 rename 更新；daemon 重启后从上次高水位继续，避免远端 floor 将新的 owner ACTIVE 误判为旧版本。
- `midrd --prefix` 走 owned store；周期刷新和退出撤销不再直接绕过 owned。Prefix IPC 仍接收外部 originator/generation，只有本地静态 Prefix Provider 使用本地 owned 生成 sequence。
- 远端对象寿命到期只进入本地 floor 并从 usable snapshot 移除，不伪造带远端 originator 的 WITHDRAWN；真实撤销只能由 owner 的 owned store 生成。
- Prefix Provider snapshot 现在以 `SNAPSHOT_BEGIN`、对象、`SNAPSHOT_END`、`EOR` 完整结束，断线或 staging 失败时由上层丢弃未完成批次。

## 验证

```text
make clean && make test
  contract/core/Prefix Provider/wire/transport/Consumer/SPF/engine/
  Prefix IPC/owned：全部 PASS
  standalone boundary scan：PASS

./r7-restart-smoke.sh
  owner 进程强制退出后使用同一 sequence file 重启；远端收到更高
  sequence 的 ACTIVE，脚本 PASS

./r7-smoke.sh
  IPv4 convergence：PASS
  IPv6 convergence：PASS
  IPv4 expiry：PASS
```

`core-test` 明确验证远端 ACTIVE 到期不产生 WITHDRAWN event；`owned-test` 覆盖 upsert、refresh、withdraw、提交失败后旧状态保留和重启后的 sequence 延续。所有代码通过 `git diff --check`，worktree 不包含构建产物或未跟踪测试输出。

## 边界

本阶段不实现 Membership/NDS、scope/代表节点、Link cost、完整 LSDB/TED 图接线；这些能力归入 R7-closure-4。`bgpd` 旧实现继续保留作为 R5-full-B parity oracle，生产 floor GC 仍保持关闭。
