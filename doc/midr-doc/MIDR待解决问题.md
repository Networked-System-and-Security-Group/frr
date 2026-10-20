# MIDR 待解决问题

维护起点：2026-10-16。本文是持续更新的问题清单，接替 [项目交接-20261009](handoff/MIDR项目交接-20261009.md) 第 3 节的任务表；交接文档保留为当日快照，不再更新任务状态。协议设计层面的长期 FOLLOW 事项仍记录在科研目录 `03_开发与审查/02_MIDR链路状态传播FOLLOW事项.md`，本文只在需要时引用。

状态取值：`待分析`、`已定位`、`方案待定`、`开发中`、`待验证`、`已解决`。已解决的条目移到文末“已关闭”，保留结论和证据路径。

## 总览

| 编号 | 问题 | 主责 | 状态 | 影响 |
|---|---|---|---|---|
| P-01 | 节点重启后入群卡死、群间不建锚点，overlay 分裂 | 第一组 | 待验证（`02e0905344` 实验回归通过，未上 FITI） | 现网 overlay 自 2026-10-15/16 起失效 |
| P-02 | unattended-upgrades/needrestart 自动重启全部 MIDR 服务 | 部署 | 部署脚本已处置，随下次 apply 生效 | P-01 的现网触发源 |
| P-03 | midrd/zebra 日志无限增长 | 第三组（zebra/mgmtd）、第二组（midrd 输出）、部署 | 现网已缓解（logrotate），根因待修 | 已有节点磁盘写满；zebra 因无 mgmtd 每台每天约 4 GB 错误日志 |
| P-04 | 反向发布的 locator 环路 | 第三组、环境维护方 | 方案待定 | 反向发布上线的前置条件 |
| P-05 | MIDR 路由反向发布到网关 | 第三组 | 开发中 | 普通终端尚不能自动进入 overlay |
| P-06 | 同 sequence 异内容未隔离 | 第二组 | 已定位 | 冲突时按到达顺序生效 |
| P-07 | 未持久化或丢失 sequence 时，重启节点被邻居长期拒绝 | 第二组 | 已定位（待修复） | backbone 实验必现；FITI 已配置持久化，未受影响 |
| P-08 | 重连 snapshot 不补发 WITHDRAWN | 第二组 | 已定位 | 旧对象最多多留一个寿命周期 |
| P-09 | 寿命/刷新/cost 通告间隔与设计文档不一致 | 第二组 | 方案待定 | 文档与实现不符；影响选路实验时延 |
| P-10 | 代表能力位与 Group Prefix 代表核对 | 第一组、第二组 | 待分析 | 09 观测到多 GroupRep |
| P-11 | avoid-tier1 部署与效果验收 | 第一组（二、三组配合） | 待分析 | 现网关闭 |
| P-12 | 差异化链路条件下的分群与选路实验 | 第一组、第二组、环境维护方 | 待分析 | netem 条件未验收 |
| P-13 | 设计文档修订（01/04/顶层） | 第二组 | 方案待定 | 旧文档仍含 bgpd 内嵌语义 |

## P-01 节点重启后入群卡死、群间不建锚点

**结论**：触发条件是 midrd **重启**，与是否更换二进制无关。2026-10-15/16 现网在发布包未变（仍为 `94a3ca3c94ed-aspathfix-20261007T055433Z`）的情况下，仅因服务被逐台重启（见 P-02）就复现；2026-10-07 的“滚动更新”本质上也是部署脚本逐台停止并重启全部 MIDR 服务。顺序冷启动能恢复，是因为它保证每台节点上线时，目录里的群和代表都是已收敛、不会马上消失的。

### 现象

重启后节点进入两种异常稳态，且不会自行恢复（现网已持续约 25 小时）：

- **A 卡在探成员**：`show midr join` 为 `in-progress / probing-members`，`Local group-id 0`，`Target group` 指向一个已不存在、或其成员视图失真的群；`Pending ctrl requests (none)`。
- **B 孤岛群**：节点自建了单成员新群（或是配置群号的固定代表），`idle/done`，但 `Anchor cands (none)`、`Members linked 0`，只有到两台专职 Bootstrap 的会话。

结果：全网 0 条 GRE 虚链路、0 条 proto 199 路由；Bootstrap 目录里只剩 7 个单成员群代表。

2026-10-16 只读采集（`NetArchLab90:/tmp/midr-rolling-20261016/`）：

| 节点 | midrd 重启（UTC） | 状态 | 群 / 目标群 | 关联 |
|---|---|---|---|---|
| MIDR_kmn | 10-15 06:39:19 | A | 目标 6 | 群 6 原代表 R1_zhz 于 20 s 后重启 |
| MIDR-R1_zhz | 10-15 06:39:39 | A | 目标 4 | 群 4 原代表 kmn 于 20 s 前重启 |
| MIDR-R1_wh | 10-15 06:54:21 | A | 目标 3 | 群 3 原代表 wlq 于 31 s 后重启 |
| MIDR_wlq、MIDR_qda、MIDR-R2_wh、MIDR-R2_zhz、TEIN-MIDR_pku | 10-15～10-16 | B | 9 / 11 / 7 / 8 / 10 | 各自建群，无锚点 |
| MIDR_fudan、MIDR_buaa | 10-16 | B | 2 / 1（配置群号） | 无成员、无锚点 |

2026-10-07 部署证据（`/home/guest/yhy/frr/fiti-aspath-fix-20261007T055433Z/all-online/`）：bootstrap → 代表 → 成员三阶段依次重启，成员阶段逐台连续执行、不等待收敛。之后 8 台普通成员全部为 A：kmn、qda、wlq、R1_wh 目标为群 1（北航，代表在线），R1_zhz→5、R2_wh→4、R2_zhz→6、pku→6（这些群的代表正在同一轮里被重启）。

### 机理（2026-10-16 按运行时状态与代码核实）

确定的卡死链条（kmn、R1_zhz、R1_wh 三例均吻合）：

1. 节点重启 → 向 Bootstrap 取代表表 → 对各群代表做 60 s 探测热身（`MIDR_JOIN_PROBE_WAIT_SECS`）→ 选出目标群 G 及其代表 R，向 R 发 `MEMBER_LIST_REQ`。代表表是重启那一刻的快照，热身期间不会更新。
2. 若 R 在这约 60 s 内也重启了，它此时群号为 0（或已另建新群）。R 组装的 `MEMBER_LIST_RESP` 第一条是它自己的真实群号。
3. 接收方 `midr_ctrl_recv_member_list()`（`midrd/group1/midr_ctrl.c` 约 1590～1610 行）先清掉重传，再发现响应群号 ≠ G，判为“过期响应”直接丢弃。
4. 只有群号匹配的响应才会启动 `t_member_probe_done` 定时器（约 1660 行）；该定时器是探成员阶段唯一的出口（之后由 CL 给出 JOIN 或 CREATE）。响应被丢弃后没有任何超时或重评估，节点永久停在 `probing-members`。
5. 双方的节点表里互相保留着对方的旧群号（kmn 记 R1_zhz=6，R1_zhz 记 kmn=4），探测流量持续刷新该条目的 Age，但群号不更新；其余条目自重启后不再更新（Age 1d7h）。

时间吻合：kmn 06:39:19 重启，约 06:40:20 选定群 6，而群 6 代表 R1_zhz 在 06:39:39 重启；R1_zhz 06:39:39 重启，约 06:40:40 选定群 4，群 4 代表 kmn 在 06:39:19 重启；R1_wh 06:54:21 重启，约 06:55:21 选定群 3，群 3 代表 wlq 在 06:54:52 重启。

由此得出触发条件：**一个节点重启后的约 60 s 内，它将要加入的目标群的代表也重启（或群号发生变化）**。顺序冷启动逐台等待收敛，因此不会触发；无间隔的逐台更新或随机自动重启则很容易触发。

补充说明：

1. **探成员阶段没有超时和重评估**。加入流程选定目标群后，向其代表要成员表并等待探测完成；若代表已重启（不再属于该群）或成员视图里有已消失的节点，`MEMBER_LIST_RESP` 被判为“与当前候选群不符，忽略”，或成员探测永远“探不齐”，流程就停在 `PROBING_MEMBERS`。代码明确留有待办：`midrd/group1/midr_nds.c` 约 2271、2404 行（“探不齐异常后续做超时兜底”）。
2. **目标群来自可能过期的目录**。Bootstrap 目录条目有数十秒寿命，代表重启后旧条目仍可被选中；两台互为对方目标群代表的节点在几十秒内先后重启，就会互相卡死（kmn↔R1_zhz）。
3. **群间锚点只在加入评估时产生一次**。锚点候选取自 RECOMMEND 决策的第 2/3 名群（同文件约 2330～2395 行），探测 60 s 后由 ANCHOR 决策建连。自建新群（CREATE，约 2537 行）和配置群号的固定代表都不经过这一步，之后也没有周期性重评估；代码 TODO 写明“现依赖其它节点各自 join 时经目录发现本群”。随机顺序重启时，能为别人搭桥的后加入者恰好卡在 A，于是没有任何锚点。
4. **孤立检测未兜底**。CL 的 ISOLATED 触发要求“群内与锚点会话全部消失”，从未有过群内/锚点会话的孤岛节点（仍有 Bootstrap 会话）未见触发 RECONNECT。此点为现象推断，需调试日志确认。

与 P-07 的关系：FITI 节点均配置了序号持久化，重启后的对象已被 Bootstrap 正常接受，P-01 与序号无关。第一组已在 `feat/midr-group1-midrd` 方向改写入群探测的健壮性（2026-10 反馈），待合入后用本节场景回归。

### 受控复现（2026-10-19，已完全复现）

环境：90 上 containerlab 实验 `midr-rejoin-repro`（文件在 `NetArchLab90:/tmp/midr-repro/`）。12 个节点的名字、角色、node-id 与 FITI 相同，运行 FITI 发布包原样二进制（`94a3ca3c94ed-aspathfix-20261007T055433Z`，zebra+midrd），启动参数相同（含 `--sequence-file`）。节点间按目的地址注入时延，矩阵取自 FITI 10-07 各阶段采集的实测 RTT（66 对）。开启 `debug midr`、`debug midr discovery`。

- 顺序冷启动后的群号与会话数和 FITI 10-07 更新前（`before/summary.tsv`）逐项一致。
- 按 10-07 journal 记录的停止/启动时刻重放部署（`replay-1007.sh`），约 6 分钟后的状态：kmn、qda、r1wh 卡在探测群 1，r2wh→4，r1zhz→5，r2zhz、pku→6，与 10-07 现场一致（仅 wlq 目标由 1 变为 4）。

调试日志给出的完整链条（10-07 “目标群 1、北航在线仍卡住”的答案）：

1. 群 2 代表复旦重启（停机约 11 s）。成员 r1wh、r2wh 的周期评估看到到复旦的链路降级，“群 2 仅 1/2 条好链路”，主动 LEAVE 并重新加入。
2. 重新加入时它们已无 MIDR 会话，节点目录条目约 40 s 后到期；“远端视图撤销 → detach 清理链”删掉了它们**正在探测的代表**及其链路数据（`nodedir: 10x expired`、`stop probing reason=1`）。
3. 60 s 探代表结束（REP_PROBE_DONE）时，视图里没有可用代表，也没有任何非 0 群号，CL 判定“无可用群代表，CREATE 新群 1”（新群号 = 视图内最大群号 + 1 = 1），与北航的群 1 **撞号**，并自任代表（GroupRep）。r1wh、r2wh 各自如此，网络中出现 3 个互不相连的“群 1 代表”。
4. Bootstrap 随即把 106、107 作为“群 1 代表”发给后续加入者。kmn 选群时北航链路降级，r1wh 时延更优，于是选了“群 1 代表 106”，向其发 `MEMBER_LIST_REQ`。
5. r1wh 此时已按部署顺序再次重启，群号 0、无代表位，日志 `ignoring MEMBER_LIST_REQ for group …`。kmn 5 次重传后 `MEMBER_LIST_REQ 尝试 5 次无响应，放弃`，此后没有任何动作，永久停在 `probing-members`。

给第一组的完整说明（实验过程、日志与修改建议）：[重启后入群卡死问题说明-致第一组-20261019](handoff/重启后入群卡死问题说明-致第一组-20261019.md)。

### 缺陷清单（第一组）

| 编号 | 缺陷 | 代码位置 |
|---|---|---|
| D1 | 探成员阶段无超时：成员表请求放弃重传、或响应群号不符被丢弃后，加入流程永久停住 | `midr_ctrl.c` 约 1590～1660 行；`midr_nds.c` 约 2271、2404 行 TODO |
| D2 | 加入期间节点目录到期触发的 detach 清理，会删掉正在探测的代表与链路数据；探代表窗口（60 s）长于目录寿命（约 40 s） | `midr_nds.c` 远端视图撤销路径 |
| D3 | CREATE 的新群号取“视图内最大群号 + 1”，视图刚被清空时得到 1，与现有群撞号，并直接自任代表 | `midr_cl.c` `cl_max_group_id()` 及约 264、439 行 |
| D4 | 代表短暂重启即导致成员按链路降级 LEAVE，放大重启影响 | `midr_cl.c` PERIODIC_SYNC 退群判定 |
| D5 | 群间锚点只在加入评估时产生一次，CREATE 与配置群号代表不建锚点 | `midr_nds.c` 约 2330～2395、2537 行 |

复现实验保留用于第一组修复后的回归：`bash /tmp/midr-repro/ctl.sh status` 查看状态；冷启动与重放步骤见该目录脚本。删除实验：`containerlab destroy -t /tmp/midr-repro/midr-rejoin-repro.clab.yaml`。

### 第一组修复与回归（2026-10-20）

第一组在 `feat/midr-group1-midrd`（`e11d807c52`）修改了 D1～D5：候选群批量评估、成员表无响应/群号不符时 REJOIN（至多 3 轮）、加入期间暂缓远端撤销、新群号同时参考代表目录、一群两代表时按 router-id 让位、退群去抖（3 个周期）、稳态补锚点。该分支除 `midrd/group1/` 与新增测试外，代码树与 `fix/stock-bgpd-10.7-midrd` 相同。

验证（90 worktree `/home/guest/yhy/frr-g1-e11d807`）：

- 全量编译通过；`test_midr_group1`、candidate-fallback 60 项、midrd 组件测试 27 项通过。
- 用该版本替换实验中的 midrd 重放 10-07 重启序列：重放后约 12 分钟，10 台业务节点全部入群，每群一个代表，业务节点 overlay 连通（旧版本永久卡住 7 台）。日志确认去抖、撤销暂缓、REJOIN、补锚点、重复代表让位均生效。

后续收尾（2026-10-20，已推到 GitHub `feat/midr-group1-midrd` = `02e0905344`）：

- `5cc36a7c0c`：正式合并 `fix/stock-bgpd-10.7-midrd`（代码树与 `e11d807` 相同，只补历史）；90 本地分支、`origin/` 与 GitHub 已一致。
- `298115119f`：新增 `midr_join_active()`，REJOIN 刚开始、等待目录期间也暂缓撤销；候选耗尽时不再无限期挡住补做；新增单元测试（修复前该测试失败）。
- `02e0905344`：backbone 实验 `midrd_options` 加 `--sequence-file /etc/frr/logs/midrd.seq`，并重新生成 `configs-midrd`、`configs-midrd-v6`。
- 验证：编译 0 告警；`test_midr_group1`、candidate-fallback 60/60、midrd 组件测试 27 项通过；复现实验重放 10-07 序列，约 12 分钟 10 台业务节点全部入群、每群一个代表。

- `feat/midr-three-way-integration` 已快进到 `02e0905344`（GitHub 与 90 一致）。
- 发布包：`NetArchLab90:/tmp/midr-02e090534468-g1rejoin-20261020T072257Z`（手册第 5 节格式，含 Prefix Provider 与 `lib/` 三个库；ldd、ELF、SHA256SUMS 校验通过），尚未部署到 FITI。

保留的设计改进：

- 新群号竞争（设计层面，非正确性缺陷）：wlq、qda、pku、R2_zhz 在 74 s 内先后 CREATE 了同一个“群 7”（各自的代表目录里还没有新群）。该状态最终收敛：重复代表按 router-id 让位，链路不满足的节点去抖后退群重入，最终群号唯一、每群一个代表、overlay 连通。代价是约 5 分钟额外收敛时间与几轮退群/重入。原因是新群号由“已知最大群号 + 1”的局部视图分配；若要缩短收敛，可改为不依赖局部视图的唯一分配（如由 node-id 派生，或由引导节点分配），作为设计改进跟踪。

### 尚待确认

- ISOLATED 判据的具体条件（孤岛节点为何未触发 RECONNECT）。

### 修复方向（待第一组确认）

- 探成员/探代表阶段加超时：超时或目标群代表不再属于该群时，放弃当前目标、重新拉取目录并重走加入流程。
- 选择目标群时排除刚重启或目录寿命将尽的代表；收到不匹配的 `MEMBER_LIST_RESP` 时直接触发重评估，而不是静默忽略。
- 锚点改为稳态维护：已入群节点（含 CREATE 和配置群号代表）周期性检查群间锚点数量，不足时按目录重新评估。
- 回归场景：两台互为目标群的节点相隔 10～40 s 重启；全网随机顺序重启；固定代表最后重启。

### 现网恢复

按[发布与部署手册](deployment/MIDR发布与部署手册.md)的阶段顺序冷启动可恢复。现网用于汇报，恢复前需用户确认；恢复后若不先处置 P-02，下一次自动升级仍会再次触发。

## P-02 unattended-upgrades/needrestart 自动重启 MIDR 服务

12 台节点的 `apt-daily-upgrade` 在 2026-10-15/16 各自随机时间执行 unattended-upgrades，升级 `libsystemd0`、`systemd` 等库；needrestart 随后自动重启了依赖这些库的服务，包括 `midr-zebra`、`midr-bgpd`、`midrd`、`midr-bgp-prefix-provider`，以及 `systemd-networkd`。期间无人工 SSH 登录，虚机未重启，`NRestarts=0`。证据：复旦 `/var/log/apt/history.log` 06:23:48～06:24:28 的升级记录，以及同一时段 journal 中 midr-* 的 Stopping/Started。

表 1990 规则和默认路由在重启后由 `setup-midr-underlay.py` 重建，已核对完好。

处置选项（均需用户确认后在节点上执行）：

- 在 needrestart 中排除 `midr-*.service`（`$nrconf{override_rc}`），保留安全更新；或
- 关闭 MIDR 节点的自动升级（`apt-daily-upgrade.timer`），改为维护窗口手动升级。

**2026-10-20**：`tools/midr-deploy` 的安装器在节点存在 needrestart 时安装 `/etc/needrestart/conf.d/midr.conf`，排除 `midr*` 服务的自动重启，安全更新照常进行。现网 12 台节点尚未重新 apply，排除项随下次部署生效。

即使处置了 P-02，P-01 仍需代码修复：节点故障、人工重启和滚动发布同样会触发。

## P-03 日志无限增长

现网 `/var/log/midr/` 下 `*-midrd.log` 约 14～16 GB、`*-zebra.log` 约 41 GB；MIDR-R3_wh 根分区已用 85%。midrd 把每个收发帧、TED 事件和对象事件以 `printf` 写入标准输出，再由启动脚本追加到日志文件；zebra 日志同样持续增长；均未配置 logrotate。

处置方向：短期加 logrotate（`copytruncate`）并清理旧日志；中期把 midrd 的逐帧/逐事件输出改为调试开关控制，保留状态变化与错误日志。

**2026-10-20 现网处置（已完成）**

- 处置前：北航、郑州 R3 根分区已满（100%），武汉 R3 为 100%，其余 84%～94%。zebra 日志 51～57 GB，是主要来源。
- 12 台节点：清空 zebra 日志；midrd 日志保留最后 100 MB，压缩到 `/var/log/midr/archive/` 后清空。处置后各节点根分区均为 9%。
- 安装 `/etc/midr/logrotate.conf`：`/var/log/midr/*.log` 超过 256 MB 即以 copytruncate 方式轮转，保留 4 份压缩文件。由 `midr-logrotate.timer` 每 15 分钟执行一次，独立状态文件 `/var/lib/logrotate/midr.status`，不需要重启服务。部署脚本 `tools/midr-deploy` 已同样安装该定时器，新部署的节点自动具备。
- 磁盘写满期间，北航的 midrd 与 zebra 没有重启，序号文件恢复正常增长。

**zebra 刷屏的根因（待修）**

12 台节点的 zebra 自 10-05 首次部署起，每分钟约 17,000 行（每台每天约 4 GB）重复以下错误，全部针对 `ens10`：

```text
ZEBRA: BE-client: msg_conn_send_msg: can't send message on closed connection
ZEBRA: NB_OP_CHANGE: oper_walk_done: ERROR: Error sending notification message for path: /frr-interface:lib/interface[name="ens10"]/state
```

内核中 `ens10` 没有链路事件（`ip monitor link` 3 s 内为 0），不是接口抖动。FRR 10.x 的 zebra 作为 mgmtd 后端客户端推送接口运行态，而发布包与部署未运行 mgmtd，通知持续失败并打印错误。logrotate 只能控制占用，不能消除写入。修复方向：在发布包与部署中加入 mgmtd，或关闭 zebra 对未运行 mgmtd 的运行态通知；需要先在实验环境确认。midrd 逐帧输出（上文“中期”项）仍待处理。

## P-04 反向发布的 locator 环路

把远端站点前缀（如乌鲁木齐 `240a:a00f:300::/48`）发布给本站 BGP-R 后，远端 MIDR locator `240a:a00f:300:1::2` 也落在这个 /48 内。本站 MIDR 发往该 locator 的 GRE 外层和控制报文经表 1990 交给 BGP-R，BGP-R 按 /48 又送回本站 MIDR，形成本站内环路，隧道和控制面中断。表 1990 只约束 MIDR 节点自身，BGP-R 无法区分隧道外层与业务报文。

候选方案：

- **underlay 通告 locator 所在 /64**（推荐短期）：各 BGP-R 增加 `network 240a:a00f:X00:1::/64`（云站点为 `800:1-3::/64`、`810:1-3::/64`），靠最长匹配让 locator 走 underlay。TEIN 的 `f000:737::/64` 已单独通告。BGP-R 发往本站 MIDR 的 iBGP 需过滤该 /64，避免被 Provider 当作本站前缀。需环境维护方同意。
- **locator 改用独立地址块**（推荐长期）：例如 `240a:a00f:f100::/48`，与用户前缀不重叠。
- 部署检查：启用反向发布前，自动核对各 BGP-R 对远端 locator 的最长匹配是否为 underlay /64，不满足则拒绝生成出口配置。

2026-10-16 已只读确认乌鲁木齐 BGP-R（端口 29503）配置可改；复旦 BGP-R（15502）当前密钥不能登录。因 P-01 导致 overlay 失效，未做任何修改。

## P-05 MIDR 路由反向发布到网关

90 分支 `feat/midr-route-export`（基于 `6cdd105f`，未提交、未部署）已完成：

- `lib/route_types.txt`、`lib/log.c`：开放 `redistribute midr`，bgpd/ospf6d/ospfd/isisd 均可使用。
- midrd 安装路由统一带 tag（默认 199，`--route-tag`）。
- Prefix Provider 改用 detail JSON，过滤带 MIDR tag 的路径，防止回灌。
- 验证：FRR 全量编译、midrd 组件测试 27 项、Provider 单元测试 10 项、Docker 端到端 7 项均通过（脚本 `NetArchLab90:/tmp/midr-route-export-e2e/run.sh`）。

剩余：P-04 前置条件；出口 route-map 模板（`match tag`、`no-export`、next-hop、排除默认路由与本站前缀）；部署脚本按站点选择对接协议；随新版本发布；端到端终端业务与撤销验证。限制：不带 instance 的 `redistribute midr` 只取 instance 0（SPF）路由。

## P-06 同 sequence 异内容未隔离

`midr_core_upsert()` 返回 `MIDR_CORE_CONFLICT`，但 midrd 无调用方处理，先到达的内容继续进入 TED。与 01/04 设计的“identity 级隔离”不符。需补隔离状态、诊断计数和回归测试。

## P-07 未持久化或丢失 sequence 时，重启节点的对象被邻居长期拒绝

第一组在 backbone 实验中复现（2026-10 反馈《第二组单节点重启序号持久化问题说明》）：节点重启后 Membership/Link/NodePrefix 全部被存活节点判为 `MIDR_CORE_OLDER`，无法恢复。2026-10-16 核实：

- **结论成立，但只在未传 `--sequence-file` 时发生。** `midr-owned.c` 的 `save_sequence()` 在路径为空时直接返回成功、不落盘，重启后 `last_sequence` 回到 0；midrd 没有默认路径。backbone 实验各节点 `daemons` 中的 `midrd_options` 均未传该参数，因此必然复现。
- **FITI 现网不受此影响。** `start-midrd-service.sh` 传入 `--sequence-file /var/lib/midr/<node-id>.seq`；现网 kmn、R1_wh、R1_zhz 的序号文件为 73 万～97 万且持续递增，重启后未归零；Bootstrap R3_wh 接受了 wlq、qda 重启后的对象，序号与其本地文件一致（如 wlq 682844）。所以 FITI 的入群卡死（P-01）不是序号问题。
- 存活节点的 floor 永久保留（生产代码未开启回收），也没有自源纠正，所以序号一旦回退，邻居会一直拒绝，直到邻居自己重启。不应放松接收端比较。

需要第二组修改：

1. 不允许“未持久化地发布”：未配置路径时使用默认路径（如 `<statedir>/midrd-<node-id>.seq`），无法创建或读取时拒绝启动，而不是静默从 1 开始。
2. 写入加 `fsync` 文件与目录；写失败时不发布，保留原高水位（现有分配逻辑已返回错误，需补测试）。
3. 防文件丢失与首次升级：启动时取 `max(文件值, 基于实时时钟的种子)` 作为起点（例如微秒级 Unix 时间，远大于现有计数），可覆盖文件丢失、旧版本从未保存、重装等情况；时钟回退时仍以文件值为下限。也可回到设计文档的 `boot_epoch<<32 | counter` 方案。
4. 长期：实现自源对象纠正（fight-back），或 floor 安全回收（FOLLOW-11-B）。

## P-08 重连 snapshot 不补发 WITHDRAWN

`send_snapshot()` 只发送 ACTIVE 对象。漏收撤销的邻居在当前 30 s 寿命内保留旧对象。需决定是补发仍在传播期内的 WITHDRAWN，还是在设计文档中明确依赖寿命收敛。

## P-09 参数与设计文档不一致

| 参数 | 设计文档 | 实现 / FITI |
|---|---|---|
| 对象寿命 L | 3,600,000 ms | 默认 6 s；FITI `--lifetime 30000` |
| 刷新间隔 | 300 s | L/3（FITI 10 s） |
| cost 最小通告间隔 | 1 s | 300 s（`MIDR_LINK_COST_MIN_ADVERTISEMENT_INTERVAL_MS`） |

300 s 的 cost 通告间隔会使测量变化最快 5 分钟后才影响选路，P-12 实验需考虑。需决定修改实现还是修订文档。

## P-10 代表能力位与 Group Prefix 代表核对

第一组 NDS 的 `MIDR_CAP_GROUP_REP` 能力位与第二组“群内最小 node_id 发起 Group Prefix”的代表是两套机制。09 观测到群 1 多个节点带 GroupRep 位，北航 join 中 Bootstrap 候选带 `failed` 标记。需在 P-01 修复并恢复现网后重新核对，分别检查能力位与实际 Group Prefix 发起者。

## P-11 avoid-tier1 部署与效果验收

现网关闭。准入实现只作用于新建连，开关不清除已有 Established Session；需核对 IP2ASN/Tier1 数据和新邻接场景。

## P-12 差异化链路条件实验

正式 netem 时延/带宽/丢包条件尚未验收；`fq_codel` 不代表已注入时延。依赖 P-01 恢复，并受 P-09 中 cost 通告间隔影响。

## P-13 设计文档修订

科研目录的 `MIDR 链路状态传播设计文档.md`、`04_MIDR单实例LS对象洪泛算法.md` 仍含 bgpd 内嵌期语义；`01_MIDR链路状态传播设计规范v1.md` 已按 midrd 对齐但待审核。修订清单见 `03_开发与审查/16_midrd设计文档拟修改清单.md`；P-06～P-09 的结论需同步写入。

## 已关闭

暂无。
