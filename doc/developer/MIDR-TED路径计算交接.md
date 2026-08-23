# MIDR TED 路径计算交接

## 1. 依赖边界

路径计算模块只包含：

```c
#include "bgpd/bgp_midr_ted.h"
```

不得包含 `bgp_midr_private.h`，也不得读取 MIDR RIB、LSDB、Local Fact 或 FRR Unicast RIB。传播侧通过 immutable snapshot 提供全部输入；路径计算侧不拥有 snapshot 内数组。

## 2. Snapshot 内容

`struct midr_ted_snapshot` 同一 generation 内原子提供：

| 数组 | 语义 |
|---|---|
| `nodes` | 本 group 的 Router-level 节点 |
| `intra_links` | 本 group 内有向 Router-level Link |
| `egress_links` | 从本 group 指向其他 group 的有向 Link |
| `node_prefixes` | 本 group Prefix 到具体 node 的 attachment |
| `group_edges` | 全局有向 Group-level 聚合边 |
| `prefix_groups` | Prefix 到可达 group 的一对多映射 |

`group_edges(G1,G2)` 与 `group_edges(G2,G1)` 是两个独立条目，各自取该方向所有 usable inter-group Link 的最小 `canonical_cost`。Link 不自动生成反向边。

`local_ifindex` 只对本节点 originate 的 Link 有本地意义；远端 Link 为 `0`。Router-ID 保持 FRR 网络字节序，group、link、cost 和 count 为主机字节序。

## 3. 获取与释放

```c
const struct midr_ted_snapshot *snapshot = NULL;
int ret;

ret = midr_ted_snapshot_get(ctx, &snapshot);
if (ret == -EAGAIN)
	return; /* TED 尚未 READY */
if (ret != 0)
	return;

run_path_computation(snapshot);
midr_ted_snapshot_release(&snapshot);
```

规则：

- `snapshot_get()` 返回 `0` 后必须且只能通过 `midr_ted_snapshot_release()` 释放引用。
- 数组只读，引用存续期间有效；发布新 generation 不会破坏已持有的旧 snapshot。
- 不得在 release 后保存数组元素指针。
- 计算结果准备提交前调用 `midr_ted_generation_is_current(ctx, generation)`；返回 false 时丢弃旧结果。
- `sync_reason_flags != 0` 表示 snapshot 可用但存在 `EOR_TIMEOUT` 或 `RESYNC_FAILED` 诊断，路径模块可按自身策略降级，但不得把它解释为数组所有权变化。

## 4. Consumer 使用

```c
static void ted_changed(struct midr_context *ctx, uint64_t generation,
			uint32_t change_flags, void *arg)
{
	struct path_worker *worker = arg;

	enqueue_recompute(worker, generation, change_flags);
}

static const struct midr_ted_consumer_ops ops = {
	.snapshot_changed = ted_changed,
};

struct midr_ted_consumer *consumer = NULL;

ret = midr_ted_consumer_register(ctx, &ops, worker, &consumer);
if (ret != 0)
	return ret;

/* 注册不会补发历史 callback，注册方应主动获取一次当前 snapshot。 */
ret = midr_ted_snapshot_get(ctx, &snapshot);
if (ret == 0) {
	enqueue_recompute(worker, snapshot->generation, MIDR_TED_CHANGE_ALL);
	midr_ted_snapshot_release(&snapshot);
}

/* 退出时先停止任务，再注销。 */
midr_ted_consumer_unregister(ctx, &consumer);
```

Callback 只通知 generation 和变化分类，不借出 snapshot，也不应在 callback 中执行长时间 SPF。推荐将 `(generation, change_flags)` 合并投递到路径计算队列，再主动获取最新 snapshot。连续 callback 可以合并；只需保证最终计算最新 generation。

## 5. 计算输入关系

群内 SPF 使用 `nodes` 和 `intra_links`。到其他 group 的下一跳候选来自 `egress_links`；Group-level 计算使用 `group_edges`。目标 Prefix 先通过 `prefix_groups` 得到一个或多个目标 group，再根据目标 group 和 `node_prefixes` 选择相应层级的计算过程。

传播侧不提供 SPF、不调度计算、不安装路由，也不发送 ZAPI。路径计算侧未来安装的所有 MIDR 路由必须标记 `ZEBRA_ROUTE_MIDR`；传播侧 PFX-01 会在 route-map 前硬排除该 provenance，防止计算结果反馈成为新的 Contributor。

## 6. 可执行参考

最小 Consumer 生命周期和 Production TED 接线位于：

```text
tests/bgpd/test_midr_ted_consumer.c
tests/bgpd/test_midr_lsdb.c
```

验证命令：

```bash
./tests/bgpd/test_midr_lsdb
python3 ./midr-test/run-m6-m7-scenarios.py M7-TED-001
python3 ./midr-test/run-m6-m7-scenarios.py M7-TED-002
```

当前状态：

```text
M7 engineering and automated QA: COMPLETE
M7 path-computation joint verification: PENDING
```
