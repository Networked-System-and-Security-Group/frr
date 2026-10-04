# CL 候选群回退与状态流程检查

检查“当前候选群验证失败后继续尝试后续群，全部已发现候选群不可用才 CREATE”。修复同步应用于 midrd 与 bgpd 的 CL/NDS/Control 实现。

## 运行

需要 Python 3 和 GCC/Clang；Windows 可使用 MinGW GCC。无需 FRR 构建、root、网络命名空间或运行中的 daemon。

```sh
python midr-test/cl-test/candidate-fallback/run_test.py
python midr-test/cl-test/candidate-fallback/run_test.py --suite baseline
python midr-test/cl-test/candidate-fallback/run_test.py --suite regression
python midr-test/cl-test/candidate-fallback/run_test.py --module midrd --cc gcc
```

默认分别编译 `midrd/group1/midr_cl.c` 和 `bgpd/bgp_midr_cl.c`，运行同一组断言。退出码：0 表示全部通过，1 表示行为断言失败，2 表示环境/编译错误。完整检查在修复后的实现上应全部通过。

Linux/WSL 已安装 FRR 编译依赖时，可另行运行完整生产构建与相关组件测试：

```sh
python3 midr-test/cl-test/candidate-fallback/validate_linux.py
```

此入口复制源码到一次性临时目录，编译 `midrd`、`bgpd`、`test_midr_group1` 和 `test_midr_admission`，确认 midrd 确实链接了 group 1，再执行组件和候选回退检查。日志位于本目录 `artifacts/linux-validation/`，原源码目录不产生编译产物。

## 用例

13 个基线用例覆盖：首个代表未探测/已断开后继续查找、最优代表被准入拒绝后继续查找、目录顺序不覆盖性能排名、RTT/丢包/带宽/群号排序、大小群正常 JOIN、空目录 CREATE、全部代表被拒绝 CREATE、唯一群不合格 CREATE，以及次优群切换为主候选后能够 JOIN 的正向对照。

5 个回归用例先触发代表探测完成，确认选中群 1，再触发成员探测完成：

| 场景 | 修复后行为 | 修复前行为 |
| --- | --- | --- |
| 群 1 成员 RTT 恰为 20 ms，群 2 可用 | RECOMMEND 群 2，再探其成员 | CREATE 群 3 |
| 群 1 成员丢包率恰为 5%，群 2 可用 | RECOMMEND 群 2 | CREATE 群 3 |
| 群 1 仅 4/5 条好链路，群 2 可用 | RECOMMEND 群 2 | CREATE 群 3 |
| 群 1 成员列表为空，群 2 可用 | RECOMMEND 群 2 | CREATE 群 3 |
| 群 1 不合格、群 2 代表被准入拒绝、群 3 可用 | RECOMMEND 群 3 | CREATE 群 4 |

回归断言约定通过现有 I-7 `RECOMMEND` 决策启动下一群的评估，不要求未经成员评估就直接 JOIN。它们使用普通断言，没有 `xfail` 或跳过标记。若将来改用其他重试接口，需按同一“候选耗尽前不得 CREATE”的语义调整断言。

完整检查另外运行 6 个状态流程场景：次优群最终 JOIN；四个群依次失败后才 CREATE（验证不限于两个锚点群）；成员列表超时不使用旧成员数据 JOIN；同群多个代表不会导致重复评估；新一轮加入清除失败历史与旧定时器；手动指定群号保持原有优先语义。同时断言未完成探测与重复/迟到 RECOMMEND 不会抢占当前候选。

## 实测结果（2026-10-02）

修复前，两处实现各 18 个检查中有 13 通过、5 失败，复现 `RECOMMEND(1) → CREATE(3)` / `CREATE(4)`。

修复后，在 Windows / Python 3.12 / MinGW GCC 16.1.0 下运行完整检查，两个模块均成功编译（启用 `-Wall -Wextra -Werror`），各 44 个检查全部通过，合计 88 个检查通过。

Ubuntu 24.04 / GCC 下两个模块的 88 个检查也全部通过；实际 FRR 的 `midrd`、`bgpd` 完整构建、group 1 链接检查、`test_midr_group1` 与 `test_midr_admission` 均通过。构建与组件日志见 `artifacts/linux-validation/`，修正 Linux 临时二进制命名后的回归结果见 `candidate-test-retry.log`。尚未执行真实网络多节点端到端验收。

## 结论与边界

两处 CL 按代表性能为群排名，跳过本轮成员验证失败或代表不可用的群；失败按群号标记该群全部代表。只有整个已发现目录无剩余可用候选时才 CREATE。失败历史在开启下一轮加入时清除。

NDS 接受成员评估完成后的重试 RECOMMEND，但要求 `failed_group_id` 对应当前候选且探测定时器已经结束，阻止过期或提前重试。每个主候选都有成员列表等待超时；收到有效列表后重新等待 PM 热身。没有本次有效成员表时不使用旧成员数据判 JOIN。迟到的代表目录不会覆盖成员评估中的目录。

runner 用 `#include` 直接编译完整生产 CL 源文件，经 `midr_cl_init()` 注册的 I-3 回调驱动事件，并同步复制 I-7 决策。CL 算法没有在 Python 中重写。状态流程检查从生产源码原样提取 NDS 的 RECOMMEND 分支、代表身份解析、新一轮加入函数，以及 Control 的成员请求/定时器回调并执行；外层 switch 包装和外围服务为测试替代。

基线和原始回归用例使用决策记录器，显式设置 NDS 输入；状态流程用例执行实际 RECOMMEND 消费分支和 Control 请求代码。列表解码、成员灌入、PM 数据与事件调度由测试模拟，JOIN/CREATE 消费分支及 bootstrap 全局守卫不在此轻量检查范围。数据结构仅保留所需字段，不验证 FRR ABI；此检查不替代真实网络上的多节点端到端测试。

所有编译产物和替代头文件位于自动清理的临时目录；runner 在运行前后比较生产 CL 源文件 SHA-256，确认未被修改。
