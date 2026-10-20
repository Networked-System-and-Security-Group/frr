# 归档文档

以下文档已被取代或只记录某个阶段，保留作历史参考，不再维护。正文中的路径、分支和状态以各自的写作日期为准，部分链接可能已失效。

| 文档 | 归档原因 |
|---|---|
| 00-公共前置与版本部署 | 由 [发布与部署手册](../deployment/MIDR发布与部署手册.md) 取代 |
| 03-多节点生产式自动发现与整网验收 | FITI 专用的部署与验收步骤，由手册第 9、11 节和 [FITI profile](../../../tools/midr-deploy/profiles/fiti/README.md) 取代 |
| 04-单节点部署运维与自动化手册 | 由手册第 8、10、12 节取代 |
| MIDR-FITI部署测试Session指南 | 旧的部署文档索引，由 [文档入口](../README.md) 取代 |
| midr-single-instance-p1～p6、r3～r7 | 单实例链路状态泛洪从 bgpd 内嵌到独立 midrd 的各阶段实施记录 |
| midr-second-group-close | 第二组 R7-full 汇总验收 |
| midr-test-coverage-parity | midrd 与 bgpd MIDR 测试覆盖对照，迁移完成后不再需要 |
| midr-tier1-static-review | Tier1 准入的一次静态复查；现行实现见 [midr-tier1-admission](../design/midr-tier1-admission.md) |
| 第三组 midrd 审查与修复报告（2026-09-20、09-21，共四份） | 第三组分支合入前的审查和修复记录 |
| fetch_caida_prefix2as.py、README_prefix2as_fetcher | CAIDA Prefix2AS 快照的独立抓取脚本，不属于构建或部署流程 |

2026-10-20 重组目录时，顶层与 `dp-doc/` 重复的数据面文档（`midr-cp-dp-interface`、`midr-dataplane-*`、`traffic-engineering`、`test-steps/` 等）只保留 [dataplane/](../dataplane/README-dp-migration.md) 中较新的一份，旧副本可从 git 历史查看。
