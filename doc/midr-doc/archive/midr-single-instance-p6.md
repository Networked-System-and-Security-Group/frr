# MIDR P6 全流程端到端验收

## 范围

P6 验收当前分支中三组功能的连续链路：

```text
第一组邻居关系与本地拓扑事实
-> 第二组 canonical RIB -> LSDB -> TED
-> 第三组 SPF -> ZAPI -> Zebra -> Linux FIB
```

本阶段不重新定义 LS Object、owner `WITHDRAWN`、`MP_UNREACH` 或 FOLLOW-11 语义。

## P6 收口修正

- `bgp_midr_owned.c` 将自源回送按 identity 处理：正常回送不再触发全库 fight-back，观察到更高版本时只纠正涉事 identity。
- `bgp_midr_lsdb.c` 允许 Membership 和 Group Prefix 在目标 Membership 尚未到达的冷启动阶段按全局范围导出，解除首次同步的依赖环。
- `bgp_midr_lsdb.c` 允许语义一致、sequence 更高的纯寿命刷新在不推进 LSDB/TED generation 的前提下继续洪泛，避免远端节点最终按寿命删除仍由 owner 持续刷新的对象。
- `bgp_midr_sync.c` 将专职 bootstrap 的空本地快照视为合法同步输出。bootstrap 不需要本地 Membership 或 READY TED 即可发送 EoR，并可在远端 EoR、输入队列和重同步条件满足后完成 receive-drained。初始 barrier 已完成后才建立的会话保留独立 snapshot/EoR 状态，但不重新加入已完成的初始 peer 集合。
- `bgp_midr_zebra.c` 为 MIDR ZAPI 路由设置 `ZEBRA_FLAG_ALLOW_RECURSION`，使 Zebra 可以通过 underlay RIB 递归解析 `10.99.0.x` transport loopback 下一跳。
- P6 验证器逐节点检查 SPF 结果、Zebra 已安装/已选择状态和 Linux `proto 199` FIB；FOLLOW-11 验证不再要求已经删除的 Propagation Path。

## 实验拓扑

`midr-test/backbone-lab/midr-backbone.clab.yaml` 提供 15 个容器：

- `b1-b5`：第一组引导节点。
- `t1-t3`：underlay 三角转发节点，不运行 MIDR。
- `r1/r2/m1a/m1b/m2a/z1/z2`：MIDR 节点，承载三组联调流程。

underlay BGP 仅向各邻居传播 `10.99.0.0/24` transport loopback，避免普通 BGP 路由覆盖 MIDR SPF 测试前缀。

7 个 MIDR 节点各自配置一个唯一的 `198.18.0.x/32` 服务前缀，并通过 `MIDR-PREFIX` route-map 只把该前缀交给 Prefix 输入。管理网段、underlay 链路网段和 transport loopback 不作为 P6 路由安装判据；每个节点必须经 MIDR 学到其余 6 个服务前缀。

## 运行

```bash
./midr-test/backbone-lab/run_p6_e2e.sh preflight
sudo -E ./midr-test/backbone-lab/run_p6_e2e.sh all
./midr-test/backbone-lab/run_p6_e2e.sh soak
```

已有台子时只使用 `check`，不要使用 `all`：

```bash
MIDR_LAB_NAME=midr-backbone-p6 \
MIDR_LAB_IMAGE=frr-midr-p6:<revision> \
MIDR_LAB_RUN_ROOT=/home/guest/yhy/midr-lab-runs/midr-backbone-p6 \
./midr-test/backbone-lab/run_p6_e2e.sh check
```

每个 MIDR 节点必须同时满足：

- `show midr spf summary` 为 `READY`，无 pending recompute、无错误且有路由结果。
- SPF 为其余 6 个 `198.18.0.x/32` 服务前缀产生可达、非本地且含 nexthop 的结果。
- 这 6 个前缀在 Zebra 中均以 `midr` 类型为 `Installed` 并被选中。
- 同一组前缀在 Linux FIB 中均以 `proto 199` 存在。
- 每个 owner 的最新 Membership sequence 在 7 个 MIDR 节点上一致；`soak` 在跨过一次 5 分钟 owner 刷新周期前后各执行一次完整检查。

## 证据

构建、部署、收敛和验证日志保存在 `midr-lab-runs/<lab-name>/`。已有采集结果包括第一组接口、Local Fact、Owned、RIB、LSDB、TED、SPF 和完整 FRR 日志；P6 验收另外保存第三组 SPF/Zebra/FIB 判据。

`p6-verification.log` 保存 P6 专用验收的逐节点结果，`p6-summary.txt` 保存最终汇总行；`soak` 另外保存 `p6-verification-pre-soak.log` 和 `p6-verification-post-soak.log`。基础的第一组/第二组验收与对象采集仍分别保存在 `acceptance.log`、`capture-post-convergence.log` 和 evidence 目录中。

只有第一组/第二组对象链路和第三组 SPF/Zebra/FIB 判据全部通过，P6 才算通过。P4 的重连、重启恢复和正常退网真实联调若未在本次运行中单独执行，继续按 P4 遗留项记录。

## 本次验收结果

验收环境：

```text
主机：NetArchLab90
分支：feat/yhy-midr-single-instance-ls-flooding
验收提交：f9beedd0d653
镜像：frr-midr-p6:f9beedd0d653
镜像 ID：1c59b1dfc275
镜像创建时间：2026-09-13T09:44:37.998297556Z
运行目录：/home/guest/yhy/midr-lab-runs/midr-backbone-p6-final
刷新前证据：evidence/20260913-094800-pre-soak/post-convergence
刷新后证据：evidence/20260913-094800-post-soak/post-convergence
```

镜像由验收提交 `f9beedd0d653` 直接构建，标签、镜像内容和文档记录一致。

验收结果：

- 冷启动收敛后的第一轮完整验收：第一组与第二组运行检查 60 passed、0 failed；对象、LSDB 和 TED 证据检查 128 passed、0 failed；P6 SPF/Zebra/Linux FIB 与 Membership sequence 检查 14 passed、0 failed。
- 330 秒 soak 跨过至少一个 5 分钟 owner 刷新周期后，第二轮完整验收仍为 60 passed、0 failed；128 passed、0 failed；14 passed、0 failed。7 个 MIDR 节点均安装其余 6 个服务前缀，每个 owner 的最新 Membership sequence 在全网一致。
- 镜像内组件回归（`image-component-tests.log`/`.tsv`）：21 个 MIDR 组件测试与 12 个 TED fixture 场景全部通过。其中 `test_midr_ted_fixture` 需经 `run-ted-fixtures.py` 驱动；`test_midr_zebra_e2e` 需活动 zebra，按证据文件中记录的特权容器配方运行后全部通过（双实例 SPF+TE SRv6 与内核 FIB 增删验证）。
- 镜像内全量 `make check` 为 341 passed、2 skipped、105 failed；105 项失败集中于既有 `test_aspath` 和 `test_peer_attr` 基线，因此不记录为全量 FRR 测试通过。

长稳复测曾发现 Membership 视图在运行约 3 小时后分裂：owner 每 5 分钟生成更高 sequence 的纯寿命刷新，但 LSDB/TED 语义未变时仍保留旧 selected-path 指针，导出门禁因此抑制新路径继续洪泛，远端最终按 1 小时寿命删除对象。提交 `7cd2dd979e` 允许同一 peer 上 sequence 更高且语义与已提交对象一致的纯刷新继续洪泛，同时保持 LSDB/TED generation 不变。最终 soak 中所有 owner 的 Membership sequence 均由初始 `4294967297` 推进并在 7 个 MIDR 节点上一致，确认刷新周期主路径闭环；旧失败台的证据保留在 `/home/guest/yhy/midr-lab-runs/midr-backbone-p6-run2/evidence`。

最终 bootstrap 同步状态为一次 barrier、零 timeout、零 waiting peer，且所有活动会话均完成双向 EoR 和 receive-drained。`r1/r2` 的后建立会话不再增加初始 peer 计数或留下 READY/`waiting peers` 状态冲突。FOLLOW-11-A/B、生产 `H = L` 证明、异常慢写恢复、生产 floor 回收启用和 allocator 文件丢失恢复不属于本次通过项。
