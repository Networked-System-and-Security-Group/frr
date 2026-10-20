# MIDR 文档

MIDR 在 FRR 10.7 上实现为独立 daemon `midrd`，与匹配版本的 zebra、bgpd 配合运行。本目录收录当前有效的设计、部署和交接文档；已被取代的阶段性材料放在 [archive/](archive/README.md)。

## 阅读路径

**初次了解项目**

1. [MIDR 系统总体设计](MIDR系统总体设计.md)：架构、三组分工、前缀输入、GRE、路由隔离和功能边界。
2. [MIDR 待解决问题](MIDR待解决问题.md)：当前未解决的问题与处理状态。
3. [项目交接 2026-10-09](handoff/MIDR项目交接-20261009.md)：当日的源码基线、环境和任务快照。

**构建与部署**

1. [MIDR 发布与部署手册](deployment/MIDR发布与部署手册.md)：构建发布包、profile 配置、分阶段批量部署、运维、验收、回滚。
2. [`tools/midr-deploy/README.md`](../../tools/midr-deploy/README.md)：部署工具的快速开始。
3. [FITI profile](../../tools/midr-deploy/profiles/fiti/README.md)：FITI 12 节点环境的角色、地址和一次性准备。
4. 两节点独立实验：[Session 测试](deployment/两节点Session测试.md)、[GRE 虚链路测试](deployment/两节点GRE虚链路测试.md)。

**开发**

- 本地编译与测试：[`midr-test/README.md`](../../midr-test/README.md)。
- 接口与设计：见下方 `design/` 和 `dataplane/`。

## 目录

| 目录 | 内容 |
|---|---|
| [deployment/](deployment/) | 发布与部署手册、两节点实验 |
| [design/](design/) | 现行的组间接口与功能设计 |
| [dataplane/](dataplane/README-dp-migration.md) | 第三组数据面：Zebra 后端、GRE、虚链路的设计、测试与验收 |
| [handoff/](handoff/) | 交接文档与发给其他组的问题说明 |
| [FITI部署记录/](FITI部署记录/) | FITI 上历次部署的结果、验收快照和状态核对，按日期保留，不再修改 |
| [archive/](archive/README.md) | 已被取代的文档 |

### design/

| 文档 | 内容 |
|---|---|
| [MIDR-TED路径计算接口规范](design/MIDR-TED路径计算接口规范.md) | SPF 与路由安装接口规范 |
| [midr-first-group-input-handoff](design/midr-first-group-input-handoff.md) | midrd 与第一组的输入接口 |
| [midr-third-group-output-handoff](design/midr-third-group-output-handoff.md) | midrd 与第三组的输出接口 |
| [14_midrd与第三组输出接口交接](design/14_midrd与第三组输出接口交接.md) | midrd 与第三组路由安装接口 |
| [midr-group1-midrd-integration](design/midr-group1-midrd-integration.md) | 第一组迁入 midrd 的接入说明与跨组改动 |
| [midr-group1-node-directory](design/midr-group1-node-directory.md) | 节点目录协议（NODE_ADV） |
| [midr-physical-virtual-link-integration-handoff](design/midr-physical-virtual-link-integration-handoff.md) | 物理链路与虚链路接入 |
| [midr-native-traceroute](design/midr-native-traceroute.md) | 内置 UDP traceroute 与 Tier-1 ASN 文件 |
| [midr-tier1-admission](design/midr-tier1-admission.md) | 邻居建立与 Tier1 绕行准入 |

### handoff/

| 文档 | 内容 |
|---|---|
| [MIDR项目交接-20261009](handoff/MIDR项目交接-20261009.md) | 2026-10-09 项目交接快照 |
| [重启后入群卡死问题说明-致第一组-20261019](handoff/重启后入群卡死问题说明-致第一组-20261019.md) | 节点重启后入群卡死问题的实验过程与修改建议 |

### FITI部署记录/

| 文档 | 内容 |
|---|---|
| 05-*-20261007（三份） | 跨群锚点、控制面隔离与双向 GRE、AS_PATH 过滤的修复与部署结果 |
| [07-MIDR当前验收快照.html](FITI部署记录/07-MIDR当前验收快照.html) | 2026-10-07 验收数据的交互式快照 |
| [08-MIDR部署汇报与录屏台本](FITI部署记录/08-MIDR部署汇报与录屏台本.md) | 拓扑、Overlay 逐跳路由与演示命令 |
| [09-项目状态核对-20261009](FITI部署记录/09-项目状态核对-20261009.md) | 2026-10-09 现网状态核对 |
