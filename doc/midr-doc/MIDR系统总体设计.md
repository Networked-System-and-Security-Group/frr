# MIDR 系统总体设计

设计基线：独立 `midrd` 三组集成架构；FITI 部署源码分支 `fix/stock-bgpd-10.7-midrd`。本文描述当前运行结构、模块边界和部署模型。具体协议字段与 API 以该分支的公开头文件为准；验收结果单独见部署日志。

## 1. 系统位置

MIDR 在已有 IP 网络之上构建由 MIDR 节点组成的 Overlay，利用链路测量、分群、链路状态传播和 SPF 选择业务出口。Underlay 负责 MIDR transport locator 之间的 IP 可达性；MIDR 决定业务数据包经过哪些 Overlay 邻居。

每台 FITI MIDR 使用本站接入路由器作为 underlay 默认网关，TEIN-MIDR_pku 的接入路由器为 TEIN-B1。默认路由或等效 underlay 路由必须由部署配置安装；连接网关或预配置 Bootstrap 地址本身不会自动产生路由。MIDR 内部节点可以不运行 BGP，已有 underlay 仍须能双向到达各节点的 locator。

GRE 封装将一条 Overlay 邻接的远端 locator 用作外层目的地址，使中间的非 MIDR 路由器按普通 IP 路由转发。GRE 不创造缺失的 underlay 可达性。业务包的内层目的地址保持为真实业务目的；它在每个 MIDR 上依据 MIDR 业务路由选择下一个 Overlay 邻居。

## 2. 进程与三组边界

三组 MIDR 模块运行在同一个独立 `midrd` 进程内，复用 FRR 的事件循环、VTY 和通用库。普通 `bgpd`、`zebra` 和 Prefix Provider 是独立进程。

```text
                 Bootstrap / MIDR neighbors
                              |
                              v
+--------------------------------------------------------------+
| midrd                                                        |
|                                                              |
| Group 1: discovery / directory / admission / measurement      |
|        -> public topology and Session interfaces             |
|                                                              |
| Group 2: owned LS objects / flooding / LSDB / TED / SPF        |
|        -> committed SPF results / route installation adapter  |
|                                                              |
| Group 3: GRE lifecycle / ZAPI backend / route submission      |
+-------------------^-------------------------+----------------+
                    | Prefix IPC              | ZAPI
                    |                         v
             Prefix Provider                zebra
                    ^                         |
                    | BGP RIB JSON            v
              local bgpd               Linux interfaces / FIB
                    |
              site BGP-R
```

| 小组 | 模块职责 | 主要代码入口 |
|---|---|---|
| 第一组：邻居关系构建 | Bootstrap 引导、节点目录、Session 意图、分群、跨群锚点、测量及 avoid-tier1 准入 | `midrd/group1/`，`midrd/midr-topology.h`、`midrd/midr-session.h` |
| 第二组：链路状态传播与 SPF | 四类对象的生成、同步、洪泛、生命周期、LSDB/TED、同群与跨群 SPF、计算结果与安装侧衔接 | `midrd/midr-core.*`、`midr-owned.*`、`midr-engine.*`、`midr-scope.*`、`midr-lsdb.*`、`midr-ted.*`、`midr-spf.*`、`midr-spf-install.*` |
| 第三组：路由安装 | GRE 接口与地址、ZAPI/Zebra 后端、FIB 提交、边界前缀接入及路由发布策略 | `midrd/midr-gre.*`、`midrd/midr-dp-backend.*`、`lib/zclient.*`、`zebra/`、`midr-test/fiti-deploy/midr-bgp-prefix-provider.py` |

MIDR 原生 Session 与对象传输使用 `midrd` 自身的协议，不依赖 BGP OPEN/UPDATE、AFI/SAFI、BGP RIB 或 TCP/179。FITI 中 MIDR 对象会话使用 TCP/5871，第一组控制和目录通道使用 5859；具体传输与帧定义见 `midrd/midr-transport.h`、`midrd/midr-wire.h` 和 `midrd/group1/`。

## 3. 节点角色与自动入网

边缘角色与 MIDR 群角色是两个维度。边缘节点运行 `zebra + bgpd + midrd + Prefix Provider`，负责从本站 BGP-R 获取外部前缀。内部业务节点运行 `zebra + midrd`，消费 MIDR 传播的前缀和路由。专职 Bootstrap 运行 `zebra + midrd`，提供引导和目录服务，不作为业务前缀出口、不承担业务 GRE/FIB。

FITI 使用武汉 R3、郑州 R3 作为专职 Bootstrap；北航为初始群 1 代表，复旦为初始群 2 代表。武汉、郑州各自使用 R1 作为边缘节点，R2 作为内部节点。其他站点 MIDR 为边缘节点。完整节点表与持久参数见 [03 多节点生产式自动发现与整网验收](FITI部署测试/03-多节点生产式自动发现与整网验收.md) 和 `midr-test/fiti-deploy/nodes-12.csv`。

```text
load local identity / locator / role / Bootstrap seeds
  -> reach Bootstrap through the underlay gateway
  -> register and obtain the live node directory
  -> probe group representatives and candidate members
  -> join an eligible group or create a new group
  -> establish permitted MIDR Sessions and business Links
  -> create and verify GRE virtual links
  -> propagate LS objects -> TED -> SPF -> Zebra -> FIB
```

普通节点只需本节点配置和 Bootstrap 候选，不需要配置所有对端。群数量不限定为两个；测量和策略可以触发新群创建、代表选择及运行中的群调整。Session Established、测量 Link up 和业务 GRE 可达分别表示不同状态，不能互相替代。

## 4. 拓扑、前缀与路径计算

第一组通过进程内公开 Topology 接口提供本机 Node/Membership、Link、测量和本地接口信息；外部或测试输入可使用 Local Fact IPC。第二组管理 Membership、Link、Node Prefix、Group Prefix 四类对象，通过原生同步与增量传播维护 LSDB，并提交不可变 TED generation。SPF 消费该视图，计算 local、intra-group、inter-group 或 unreachable 结果。

业务 Link 的本地 `ifindex` 和地址应来自实际 GRE 接口。SPF 的有效下一跳交给安装适配层，由第三组 ZAPI 后端提交给匹配 Zebra，再进入 Linux FIB；不能用“已学习前缀”或“SPF 有条目”代替实际安装与转发验收。

当前 BGP 前缀输入链为：

```text
site BGP-R -> local bgpd
  -> vtysh BGP RIB JSON -> Prefix Provider filtering
  -> /run/midr/prefix.sock -> midrd Prefix transactions
  -> Node Prefix / Group Prefix -> TED / SPF
```

Provider 默认仅导出符合有效、选中或 multipath 条件且 `AS_PATH` 长度不超过 1 的路径；无法解析路径时不导出。参数为 `--max-as-path-length`，持久环境变量为 `MIDR_PREFIX_MAX_AS_PATH_LENGTH`。该过滤只约束输入 MIDR 的前缀，不删除 bgpd 自身的路由。策略和计数规则见 [AS_PATH 过滤验收](FITI部署测试/05-AS_PATH过滤修复与部署结果-20261007.md)。

本地 `connected`、`static` 或 `network` 前缀只有被配置引入本机 BGP RIB 并通过 Provider 策略，或由另一个明确配置的 Prefix Provider 提供时，才成为 MIDR 输入。当前 BGP Provider 不直接扫描全部内核 connected/static 路由。SPF 标为 `local` 表示本节点发布该前缀的出口，不表示该网段直接连接在 MIDR 接口上；远端业务可经 GRE 到这个出口后，再经本站 BGP-R 到达终端。

## 5. 数据面与路由隔离

MIDR 业务路由在 Linux 中以 `proto 199` 标记。`via fe80::... dev mgre-*` 是 GRE 虚接口上的 Overlay 下一跳；其真实对端 locator 通过 `show midr virtual-links` 的 `outer local -> remote` 查询。SPF 路径度量、Zebra 管理距离和 Linux 路由 metric 是不同字段，不能用表中固定的内核 metric 20 推断 SPF 代价相同；当前 MIDR 默认管理距离为 115。

FITI 的 `setup-midr-underlay.py` 在服务启动前建立 IPv6 表 1990，并将本机 locator 发出的指定 MIDR TCP、UDP 和 GRE 外层流量送往本站网关。main 表承担业务路由。该隔离防止业务汇总前缀覆盖 transport locator 后，控制和 GRE 外层再次进入自身业务隧道。表 1990 与业务路由来源 `proto 199` 含义不同；locator 不能兼作应用 UDP 业务源地址。

普通终端目前不会仅因 MIDR 已安装路由而自动进入 Overlay。完成这项接入需要将 MIDR 路由受控发布给本站 BGP-R，并处理来源标记、next-hop、发布与撤销、防回灌和转发验证。当前 `lib/route_types.txt` 中 MIDR 类型的 `redist=0`，该反向发布未开放。Transport 隔离和 BGP/MIDR 双向发布的防回灌属于不同机制。

## 6. 策略与验收边界

`avoid-tier1` 属于第一组候选邻接准入：根据 underlay 探测和 IP-to-AS/Tier1 数据，拒绝明确观察到 Tier1 的新建连候选。它不直接重写 SPF metric，也不为 underlay 创建备用路径。打开开关会保留已有 Established Session；当前 FITI 部署关闭该策略，完整部署验收需由第一组负责，第二组核对拓扑传播与 SPF，第三组验证业务转发。无响应跳点或未映射 ASN 不构成已证明经过 Tier1 的证据。

12 节点顺序冷启动、自动发现、分群、同群/跨群 SPF、GRE、前缀安装与 MIDR 节点发起的真实业务已完成当前场景验收。滚动更新后的自动恢复入群、普通终端自动接入及其防回灌、avoid-tier1 在 FITI 的效果、分群优化和差异化链路条件实验尚未完成对应验收。具体结果以日期化日志为准，不把动态拓扑快照作为固定设计条件。

## 7. 部署与设计资料入口

| 文档 | 适用内容 |
|---|---|
| [00 公共前置与版本部署](FITI部署测试/00-公共前置与版本部署.md) | 匹配版本、发布包、进程、持久配置、vtysh 和运维命令 |
| [03 整网验收](FITI部署测试/03-多节点生产式自动发现与整网验收.md) | 固定角色、自动分群、阶段部署和集中验收 |
| [04 单节点运维](FITI部署测试/04-单节点部署运维与自动化手册.md) | systemd、配置加载、安装与回滚 |
| [第一组迁入 midrd](midr-group1-midrd-integration.md) | Topology/Session 接线和节点目录；文中旧分支状态按其记录日期理解 |
| [第三组输出接口](midr-third-group-output-handoff.md) | TED/SPF generation、下一跳及安装适配契约 |
| [路径计算接口规范](MIDR-TED路径计算接口规范.md) | 路径计算与 Zebra backend 的公开约定 |
| [AS_PATH 发布验收](FITI部署测试/05-AS_PATH过滤修复与部署结果-20261007.md) | 最新正式发布包、过滤策略和 12 节点验收证据 |
| [部署汇报与录屏台本](FITI部署测试/08-MIDR部署汇报与录屏台本.md) | 逐跳 FIB、GRE 对端映射、underlay 分段探测和业务演示 |

早期 `bgpd` 内嵌设计、SAFI/MP_REACH 传播说明及迁移前实施交接保留作历史资料。当前独立 `midrd` 的运行依赖、原生帧格式和正式代码入口以本文及现行分支为准。
