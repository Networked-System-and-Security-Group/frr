# CL 候选群回退与状态流程检查

检查“按代表性能取前 3 个可用群并行验证，第一个达标的群 JOIN；这 3 个都不达标就 CREATE，不再尝试排名更靠后的群；JOIN/CREATE 附带除本群外排名最高的两个锚点群”。实现位于 midrd 的 CL/NDS/Control（bgpd 已还原为上游 10.7，不再包含 MIDR 代码）。

## 运行

需要 Python 3 和 GCC/Clang；Windows 可使用 MinGW GCC。无需 FRR 构建、root、网络命名空间或运行中的 daemon。

```sh
python midr-test/cl-test/candidate-fallback/run_test.py
python midr-test/cl-test/candidate-fallback/run_test.py --suite baseline
python midr-test/cl-test/candidate-fallback/run_test.py --suite regression
python midr-test/cl-test/candidate-fallback/run_test.py --suite restart
python midr-test/cl-test/candidate-fallback/run_test.py --cc gcc
```

编译 `midrd/group1/midr_cl.c` 并运行全部断言。退出码：0 表示全部通过，1 表示行为断言失败，2 表示环境/编译错误。完整检查在修复后的实现上应全部通过。

Linux/WSL 已安装 FRR 编译依赖时，可另行运行完整生产构建与相关组件测试：

```sh
python3 midr-test/cl-test/candidate-fallback/validate_linux.py
```

此入口复制源码到一次性临时目录，编译 `midrd`、上游 `bgpd` 和 `test_midr_group1`，确认 midrd 确实链接了 group 1，再执行组件和候选回退检查。日志位于本目录 `artifacts/linux-validation/`，原源码目录不产生编译产物。

## 用例

13 个基线用例覆盖：首个代表未探测/已断开后继续查找、最优代表被准入拒绝后继续查找、目录顺序不覆盖性能排名、RTT/丢包/带宽/群号排序、大小群正常 JOIN、空目录 CREATE、全部代表被拒绝 CREATE、唯一群不合格 CREATE，以及次优群作为候选时能够 JOIN 的正向对照。

5 个回归用例：REP_PROBE_DONE 选出一批候选（群 1 与次优群），成员表全部送达后触发 MEMBER_PROBE_DONE：

| 场景 | 当前行为 | 最初行为（回退修复前） |
| --- | --- | --- |
| 群 1 成员 RTT 恰为 20 ms，群 2 可用 | 同一窗口内 JOIN 群 2 | CREATE 群 3 |
| 群 1 成员丢包率恰为 5%，群 2 可用 | JOIN 群 2 | CREATE 群 3 |
| 群 1 仅 4/5 条好链路，群 2 可用 | JOIN 群 2 | CREATE 群 3 |
| 群 1 成员表为空（未标记已收到），群 2 可用 | JOIN 群 2 | CREATE 群 3 |
| 群 1 不合格、群 2 代表被准入拒绝、群 3 可用 | JOIN 群 3 | CREATE 群 4 |

完整检查另外运行 10 个状态流程场景（执行生产 NDS 的 RECOMMEND 分支与 Control 请求/定时器代码）：

- 一批 3 个群并行请求、共用一个等待定时器，第 2 名达标即 JOIN，锚点群为第 1、3 名；
- 前 3 名全部失败即 CREATE，即使第 5 名可用也不再请求第 4、5 名；CREATE 同样携带锚点群（第 1、2 名）；
- 成员表超时不使用旧成员数据 JOIN，未收到成员表的群不作锚点群；
- 代表不回成员表的群被跳过，锚点群顺延到未尝试过的下一名；
- 候选群代表解析不到可用地址时留在本批但不发请求，按验证失败跳过，同批其他群照常 JOIN；
- 整批都解析不到地址时立即评估（0 秒定时）并 CREATE，加入流程不会卡住；
- 同群多个代表只占一个批次槽位；
- 新一轮加入清除已收到标记与旧定时器；
- 手动指定群号只评估配置群，不自动切换，配置群不作锚点群。

同时断言成员评估期间（含整批失败后）重复或迟到的 RECOMMEND 不会抢占当前批次，也不会开启第二批。

## 实测结果（2026-10-02）

修复前，两处实现各 18 个检查中有 13 通过、5 失败，复现 `RECOMMEND(1) → CREATE(3)` / `CREATE(4)`。

修复后，在 Windows / Python 3.12 / MinGW GCC 16.1.0 下运行完整检查，两个模块均成功编译（启用 `-Wall -Wextra -Werror`），各 44 个检查全部通过，合计 88 个检查通过。

Ubuntu 24.04 / GCC 下两个模块的 88 个检查也全部通过；实际 FRR 的 `midrd`、`bgpd` 完整构建、group 1 链接检查、`test_midr_group1` 与 `test_midr_admission` 均通过。构建与组件日志见 `artifacts/linux-validation/`，修正 Linux 临时二进制命名后的回归结果见 `candidate-test-retry.log`。尚未执行真实网络多节点端到端验收。

### 只试前 3 个（2026-10-12）

改为只评估排名前 3 的可用群：整批失败直接 CREATE，删除了回退所需的 `failed_group_id` 与 `join_failed`，NDS 只在探代表阶段接受 RECOMMEND。两个模块各 45 个检查全部通过。

### 分批评估（2026-10-04）

改为分批并行评估并补上代表地址解析失败的兜底后，Linux / GCC 下两个模块各 51 个检查全部通过；`midrd`、`bgpd` 零告警构建，`test_midr_group1`、`test_midr_admission` 与 group 1 链接检查通过。backbone lab（midrd）IPv6：基础 85/0、Tier1 26/0、IPv6-only 10/0、virtual-link 34/0；IPv4：Tier1 26/0、virtual-link 15/0，基础验收一次 67/0、一次 59/8。失败的那次是 z2 测到两个代表 RTT 为 308 µs 对 314 µs，按性能排到群 2 并加入，其余 7 项均由此连带；这是验收脚本假设 z2 必入群 1 而 IPv4 拓扑中两者只差一跳造成的偶发，不是分群逻辑错误。

### 合并 fix/stock-bgpd-10.7-midrd（2026-10-19）

bgpd 还原为上游实现后删除了 `bgpd/bgp_midr_*`，本检查只保留 midrd 模块。macOS / Apple clang 下 45 个检查全部通过。

### 节点重启后入群卡死（2026-10-19）

对应《重启后入群卡死问题说明-致第一组-20261019》，新增 `restart` 套件 15 个检查：

| 用例 | 修复后 | 修复前 |
| --- | --- | --- |
| 目录里有代表但一个都没测到（D1/D2） | REJOIN，轮数用尽后 CREATE | 直接 CREATE |
| 没有引导候选时同上 | CREATE | CREATE |
| 整批代表都没回成员表（D1） | REJOIN | CREATE |
| 收到成员表但不达标 | CREATE | CREATE |
| 视图被清空、目录仍有群 1..6（D3） | CREATE(7) | CREATE(1)，与现有群撞号 |
| 链路连续 2 次不达标（D4） | 不退群，第 3 次才 LEAVE | 每次都 LEAVE |
| 中间恢复一次 | 计数清零 | — |
| 同时 CREATE 出同号群、见面后一群两代表（D3 残留） | 连续 2 次后 router-id 大的一方 REP_RESIGN | 两个代表一直共存，代表不做退群判定 |
| router-id 小的一方 / 手配群号的一方 / 对方已不是代表 | 不卸任 | 不卸任 |
| 对方 router-id 更大但一直不让位（手配了群号） | 连续 6 次后本节点 REP_RESIGN | 两个代表一直共存 |

macOS / Apple clang 下 60 个检查全部通过；用合并后、修复前的 `midr_cl.c` 跑 `restart` 套件为 6 过 9 失败。D2（加入期间暂缓远端撤销）、D5（稳态补锚点）与 NDS 的 REJOIN 分支在 NDS/Control，不在本轻量检查范围，由 `midrd/group1/test_midr_group1` 的三个用例覆盖（Ubuntu 24.04 arm64 下通过；分别去掉暂缓与补锚点后对应用例失败）。多节点行为仍需 backbone lab / 12 节点实验验证。

## 结论与边界

CL 按代表性能为群排名，跳过代表被准入拒绝的群，取前 3 个评估；这 3 个都不达标即 CREATE。

NDS 只在探代表阶段接受 RECOMMEND，成员评估开始后的重复或迟到推荐一律忽略。每个主候选都有成员列表等待超时；收到有效列表后重新等待 PM 热身。没有本次有效成员表时不使用旧成员数据判 JOIN。迟到的代表目录不会覆盖成员评估中的目录。

runner 用 `#include` 直接编译完整生产 CL 源文件，经 `midr_cl_init()` 注册的 I-3 回调驱动事件，并同步复制 I-7 决策。CL 算法没有在 Python 中重写。状态流程检查从生产源码原样提取 NDS 的 RECOMMEND 分支、代表身份解析、新一轮加入函数，以及 Control 的成员请求/定时器回调并执行；外层 switch 包装和外围服务为测试替代。

基线和原始回归用例使用决策记录器，显式设置 NDS 输入；状态流程用例执行实际 RECOMMEND 消费分支和 Control 请求代码。列表解码、成员灌入、PM 数据与事件调度由测试模拟，JOIN/CREATE 消费分支及 bootstrap 全局守卫不在此轻量检查范围。数据结构仅保留所需字段，不验证 FRR ABI；此检查不替代真实网络上的多节点端到端测试。

所有编译产物和替代头文件位于自动清理的临时目录；runner 在运行前后比较生产 CL 源文件 SHA-256，确认未被修改。
