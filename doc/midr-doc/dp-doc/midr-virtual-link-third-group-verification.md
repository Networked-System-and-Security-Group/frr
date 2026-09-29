# MIDR 虚链路第三组验证报告（P4：远端构建与端到端验证）

> 执行者：第三组数据面（远端构建与端到端验证）
> 基线：分支 `feat/midr-three-way-integration`，HEAD `443c51474b`
> 远端：`ssh -i ~/.ssh/frr yangmy@101.6.30.220 -p 50022`（`dell-PowerEdge-R750`，Linux 5.15.0-139，112 核）
> 构建容器：`frr-ubuntu24-ymy`（镜像 `frr-ubuntu24:latest`），源码树 `/home/frr/frr-midrd3`
> 本轮日期：2026-09-29（**W3 轮**：D1–D4 修复 + W0 通知事件/overlay 承载合入后的**再从零重建实测**轮）
> 依赖契约：`doc/midr-doc/dp-doc/midr-virtual-link-api.md`（P0 冻结；W3 修订见该文头部与会签包 C6/C7）
>
> **阅读约定（重要）**：本报告的计数与 md5 **只对标注「W3 / 本轮」的段落有效**。
> 凡标注「上一轮（旧源码）」的表格与摘录，均为 W3 **之前**的源码与二进制所产，
> **不得**当作当前树的证据；受影响的旧值已逐处更正（见 §1.1 勘误）。

---

## 0. 本轮（W3）范围与结论摘要

本轮在 D1–D4 修复与 W0（通知事件枚举 + `struct midr_virtual_link_status` 新字段 + `ADDRESS_SET` 里程碑通知）
合入后，在**干净构建目录内从零重建**并复跑三套实测：组件套件与门禁 → 双容器 GRE 连通性 →
stage E（BGP-only underlay）。契约变更的逐条影响见 §3.4 与会签包 C6/C7。

| # | 体检项 | 命令 / 对象 | 结果 | 判定 | 证据 |
|---|---|---|---|---|---|
| 1 | 容器源码树 ≡ 本地工作树 | 本轮 6 个改动文件 + 5 个未改文件 md5 | 逐一相同 | PASS | §1.1 |
| 2 | `midrd` 组件套件（含 `midrd-virtual-link-test`） | 干净构建目录内组件 build + suite | **PASS=27 FAIL=0**（`grep -c ': PASS'` = 27、`grep -c ': FAIL'` = 0），**0 error**，日志末行 `SUITE_EXIT=0`；`midrd-virtual-link-test: PASS`；`standalone libfrr boundary scan: PASS` | PASS | 本轮组件套件日志 |
| 3 | 顶层 `make -j112 -k` | 容器内 | `EXIT=0`（日志末行 `TOPMAKE_EXIT=0`），`warning:` 计数 **0** | PASS | 本轮顶层构建日志 |
| 4 | 双容器 GRE 连通性（交付项 3） | 本轮新构建的 `midrd-gre-tool`（md5 `7c3bf35fea958e31d417315d24e3ad02`），零手工 `ip` | **PASS=51 FAIL=0**（含 6 条 overlay 交接断言） | PASS | 本轮 GRE 日志 |
| 5 | 跨 BGP-only underlay 验收（交付项 4） | stage E 复跑；staged zebra md5 `0aca362d964891d7a5c118069fbbc197` = 本轮新构建的 `zebra` | `EXIT=0`，**PASS=34 FAIL=0** | PASS | 本轮 stage E 日志 |

**本轮（W3）三套实测合计：PASS = 27（组件）+ 51（GRE）+ 34（stage E）= 112，FAIL = 0**；
组件套件 `: FAIL` = 0 且日志末行 `SUITE_EXIT=0`（本轮组件套件日志，§2.1），
GRE（51）与 stage E（34）的 `FAIL=0` 见各自日志。
另有**独立于上表**的 2 项 PASS：顶层全量构建（§2.4）与 `check-zapi-numbering` 门禁
（§2.3，本轮末补跑，不并入上面的 112 计数）。

- 唯一的 `warning:` 是**既有的** `lib/mgmt_msg_native.h` 噪声（**不含**本轮改动的任何文件），见 §2.1。
- 本轮新增实测证据：GRE 工具的 overlay 交接行与脚本新增断言，见 §4.3。

### 0.2 W4 增量：结构迁移后的补充实测（同日第三批）

W3 之后又做了一次**测试目录结构迁移**：`midrd/` 的 26 个 `*-test.c` 与 12 个测试脚本迁入
`midr-test/`，`dp-e2e-tool.c` 随后一并迁入；构建规则仍留在 `midrd/Makefile`（新增 `TESTDIR`
指向新位置），`midr-test/Makefile` 是可用的转发入口。同时补做了 W3 明确留空的几项。
以下数值均为**该批实测**，与 §0 的 W3 表并列有效。

| # | 项 | 命令 / 对象 | 结果 | 判定 | 证据 |
|---|---|---|---|---|---|
| 1 | 迁移后组件套件 | 干净构建目录内 build + suite（`make -C midr-test test` 亦可用） | `EXIT=0`、**PASS=27 FAIL=0**、0 error、末行 `standalone libfrr boundary scan: PASS`（门禁在新位置正确扫描 `midrd/`） | PASS | 本轮组件套件日志 |
| 2 | 双容器 GRE（新增用例 F） | `midr-test/midr-gre-connectivity-test.sh`（迁移后路径） | **PASS=58 FAIL=0**（W3 为 51；新增 7 条 = 用例 F） | PASS | 本轮 GRE 日志 |
| 3 | 用例 F：外来活隧道（真实内核） | GRE 层建 `foreign0`（端点对 10.1.1.31/32）→ vlink 请求同端点对换名 | F.1/F.3/F.4/F.7 PASS：请求被**拒绝**、未创建新设备、`foreign0` 的 ifindex 与地址**未被改动**、内核 GRE 设备数不变；F.5 PASS：删掉 `foreign0` 后**同一请求成功 READY**；F.6 PASS：清理干净 | PASS（含一条语义修正，见下） | 本轮 GRE 日志 F 段 |
| 4 | zebra 侧两处 NIT 修复 | `zebra/zapi_msg.c`：陈旧注释 `masked struct prefix`；unset handler 以 `STREAM_GET`+强制 NUL 取代 `STREAM_FORWARD_GETP` | `make -j112 zebra/zebra` `EXIT=0`、`warning:` **0**；staged zebra md5 由 `0aca362d…` 变为 **`3deebbf5…`** | PASS | 本轮 zebra 构建日志 |
| 5 | `vtysh` → midrd CLI 端到端（W3 的 U-2） | 容器内隔离 vty socket + `--no-zebra` 运行**树内** `midrd`（树内 libfrr），用**树内** `vtysh/vtysh` | `clear midr traceroute cache` → `Cleared 0 MIDR traceroute cache entries`（rc=0）；`show midr spf` → `SPF generation 1, 0 routes`（rc=0），与 daemon stdout 的 `node=7 spf generation=1 routes=0` **一致**；反向对照（未知命令、daemon 未运行）均 rc=1 | **PASS（本轮已补跑，见 §6 U-2）** | 本轮 vtysh 日志 |
| 6 | 编号门禁自检 | `midr-test/check-zapi-numbering.sh --self-test` | 临时 worktree 内：干净 HEAD PASS、交换 `ZEBRA_GRE_ADD/_DELETE` 后 **FAIL**、调用方工作树未被改动 → self-test PASS；门禁本身仍 PASS | PASS | 本地终端（本轮） |

**实测中新发现的两条事实（不是缺陷，但必须登记）**

1. **EEXIST 守卫是进程内的**：GRE/vlink 注册表都在 `midrd` 进程内，而 `midrd-gre-tool` 每次调用
   都是独立进程。用例 F 证明：**跨进程**由别处创建的活隧道，本进程的守卫看不到它，请求最终以
   `DEVICE_CONFIRM` 超时（`err=110`）被拒绝，而**不是** `err=17(EEXIST)`。两种结果都满足
   "不静默改绑、不损坏外来设备、不给调用方假 READY"，但调用方**无法**从 110 直接看出"端点对
   被外来隧道占用"。因此用例 F 的断言写成「必须是拒绝（17 或 110）」并打印实际码，同时保留
   诊断输出（内核 GRE 设备数不变 + node1 zebra 日志尾部）。若需要更精确的诊断，得新增"按端点
   对查询 GRE 设备"的接口（扩大 ZAPI 面），属第一/二组与契约的决定。
2. **`midrd` 现在需要同源的 `libfrr`**：新增的 3 个 ZAPI 发送助手位于 `lib/zclient.c`，因此
   `midrd` 对 `zclient_send_interface_address_set/_unset`、`zclient_send_interface_admin_up`
   形成**硬运行期依赖**。实测中顶层 `midrd` 直接运行报
   `symbol lookup error: undefined symbol: zclient_send_interface_address_set`——动态加载器解析到
   了**系统安装的旧 libfrr**（不含新符号）；加 `LD_LIBRARY_PATH=<tree>/lib/.libs` 后正常。
   同理，容器内**安装版** `vtysh` 不认识 `midrd` 这个 daemon，CLI 验证必须用树内 `vtysh/vtysh`。
   部署含义：安装 `midrd` 时必须同时提供同源 `libfrr`（或按本仓库既有做法用 `LD_LIBRARY_PATH`
   指向树内 lib）。另：树内构建的前缀目录（pid lock 的 `var/run`、vtysh 的 sysconfdir）需存在，
   否则 daemon 以 `Can't create pid lock file` 退出。

### 0.1 上一轮（旧源码）记录与两处勘误（**勿用于本轮判定**）

下表是 W3 **之前**那一轮的实测记录，对应**旧源码/旧二进制**；W3 的对应项见 §0 上表。

| # | 项 | 上一轮（旧源码）结果 | 与 W3 的关系 |
|---|---|---|---|
| 2′ | 组件套件 | **PASS=27 FAIL=0**（`上一轮组件套件日志`，宿主副本 `上一轮组件套件日志的宿主副本`） | 计数相同，但**用例集合已变**：W3 新增用例 13/14/15/16a/16b/17/18（D1–D4）与 case 19（D1a）位于**同一测试程序** `midrd-virtual-link-test` 内，因此 `: PASS` 行数不变 |
| 4′ | 双容器 GRE | **PASS=45 FAIL=0**（`上一轮 GRE 日志`、`上一轮 GRE 日志（seam 删除后复跑）`） | W3 升至 **51**：脚本新增 **6 条 overlay 交接断言**（**非回归**，见 §4.3） |
| 5′ | stage E | **PASS=34 FAIL=0**（`上一轮 stage E 日志`） | 计数相同，但 W3 跑在**本轮新构建**的产物上（§1.2、§5.2） |
| 8′ | 顶层全量构建 | 增量 `EXIT=0`、重编对象 0 warning（`上一轮顶层全量构建日志`） | W3 为独立一轮 `make -j112 -k`，`EXIT=0`、`warning:` = 0（§2.4） |
| 10′ | test-only seam 收敛（删 4 个死符号） | 引用数 0；`.c` 1028→**907** 行、`.h` 243→**213** 行；套件 `EXIT=0`（`上一轮组件套件日志`） | W3 的 `.c` 已增至 **1187** 行、`.h` 至 **250** 行（§1.1），907/213 属旧源码 |
| 12′ | `vtysh` → midrd 的 MIDR CLI 分发 | PASS（`上一轮 vtysh 运行日志`） | 属上一轮的收尾项，W3 **未重跑**（见 §6 U-2） |

**勘误 1（陈旧 md5）**：上一轮 §1.1 把 `midrd/midr-virtual-link.c` 记为
`247fb9a619a076d981576a5b5b392d48`（907 行）。该值**不是本轮源码**：W3 的同一文件为
md5 `07a7acefa7eee7e24af11d2b1d2c8f9a`（1187 行）。本报告一切以 §1.1 的**本轮值**为准；
旧值仅在 §1.1 的对照列中保留，并标注「陈旧、已作废」。

**勘误 2（计数归属）**：上一轮的 27/45/34 属**旧源码**。其中 27 与 34 在 W3 数值**恰好相同**，
但 W3 的组件套件与 stage E 均跑在**本轮重新构建**的产物上（§1.2），**不得**据此认为
「同一套二进制复跑」或「无变化」。

> 纪律：**未执行 / 失败项不得写作通过**。§6 逐项标注状态，并对上一轮已 PASS 的项注明是否本轮重跑。

---

## 1. 同步与一致性（本轮 W3 复核）

### 1.1 本轮源码/文档 md5（本地工作树实测；容器侧校验相同）

本轮 **6 个改动文件**（`md5 -q` 实测于本地工作树；容器 `/home/frr/frr-midrd3` 与宿主
`/home/yangmy/frr-midrd3` 逐文件 md5 校验相同）：

```text
07a7acefa7eee7e24af11d2b1d2c8f9a  midrd/midr-virtual-link.c              1187 行（上一轮（旧源码）907 行）
92667d15cbe668d101dbded397e1588b  midrd/midr-virtual-link.h               250 行（上一轮（旧源码）213 行）
e8d0161c674211a811956a38af21e0bb  midr-test/virtual-link-test.c              1393 行（上一轮（旧源码）747 行）
e2646a2b0f8f07daaf73dbf546b41fd0  midrd/gre-link-tool.c                   386 行（本轮因注释块修复而变，§3.5）
8cf57126d3798ed8af44e1668f3d77a7  midr-test/midr-gre-connectivity-test.sh     388 行（上一轮 md5 739de8439c21a9c1785be284275dd68b）
0dbb7b7e36db5b33dd46a79a216adb86  doc/midr-doc/dp-doc/midr-virtual-link-api.md  644 行（上一轮（旧源码）482 行）
```

本轮**未改动**（md5 与上一轮一致）：

```text
d06a54ae36b19bd732a23309137d2e4b  zebra/zapi_msg.c
aa8fd711c63011e32b80259346e20f5a  lib/zclient.c
9bf655093457c0f52f3ef00e353bc338  lib/zclient.h
1e3521bb708c52ae2fa0082d4bd36162  lib/log.c
3fe4e30d2b460328b43c7d13126dd0ca  midr-test/check-zapi-numbering.sh
```

> **勘误（§1.1 旧值陈旧，必须更正）**：本文件上一版 §1.1 把 `midrd/midr-virtual-link.c` 记为
> `247fb9a619a076d981576a5b5b392d48`（907 行）、把脚本记为
> `739de8439c21a9c1785be284275dd68b`——这两个值是**上一轮（旧源码）**的，**与本轮树不符**。
> 本轮以本表值为准。（`midrd/midr-dp-backend.c` 本轮**不在**给定比对清单内；
> 另行 `md5 -q` 实测仍为 `d0c7ddd2986e49d86217be6e74a6b163`，1289 行。）

### 1.2 构建产物为**从零重新构建**（本轮）

本轮在**独立干净构建目录**内从零完成组件构建与套件，产物 md5：

| 产物 | md5 | 说明 |
|---|---|---|
| 从零构建的 `midrd-gre-tool` | `7c3bf35fea958e31d417315d24e3ad02` | 交付项 3（§4）所用执行器 |
| 从零构建的 `midrd-virtual-link-test` | `4f4ace0e88dede3ca6ddbc8d7c7e1b52` | 组件套件测试程序（§2.1） |
| stage E 侧 staged `zebra` | `0aca362d964891d7a5c118069fbbc197` | 本轮新构建的 `zebra` 拷入测试容器后 md5 回比一致（§5.2） |

即：**本轮所有通过项都跑在重新构建的产物上**，未使用 `/opt/midr-dp`、`/opt/midr` 的旧
staged 产物。补充说明：本轮 staged `zebra` 的 md5 与上一轮记录的 `zebra` md5 相同，
这与本轮 `zebra/zapi_msg.c` 未改动（见 §1.1 未改动清单）相符，属可复核的一致性现象。

> 待实测 / not measured：本轮 `bgpd`、`libfrr.so` 的 md5 与文件时间戳**未在本轮测量清单内**，
> 本报告不给出数值。上一轮（旧源码）记录为
> `bgpd=bc67b1dc30acb0c9576af5c713907d66`、`libfrr.so.0.0.0=6cbb20e1463f7491e2ff46d157a7bbc7`、
> `midrd-build/midrd-gre-tool=fd60532f707acd0be6367c36a5aa2d62`（**仅供对照，勿作本轮证据**）。
>
> **一致性说明（为何不用旧二进制哈希回填）**：上一轮 §1.2 列出了四个二进制哈希
> （`bgpd` / `libfrr.so.0.0.0` / `zebra` / `midrd-build/midrd-gre-tool`）；本轮只实测了上表的
> 3 个产物，**无法逐项重算**这四个值，因此上表**不以它们回填**。旧值仅在下方的「供对照」引文中
> 保留，并按 §0.1 标注为上一轮（旧源码）。本报告中凡是「上一轮（旧源码）」标签**之外**出现的
> 二进制 md5，只有上表这 3 个。

### 1.3 同步侧踩过的坑（历史记录，上一轮（旧源码））

- 宿主副本 `~/frr-midrd3` **曾是残缺树**：约 **190 个文件漂移**，且 `vtysh/`、`bgpd/` 缺失。
- 修复方式：改为**整树 tar 同步**（而非逐目录 `rsync` + 逐文件 `docker cp` 的拼凑方式）。
- 上一轮复核：`~/frr-midrd3` 下 `bgpd/`、`lib/`、`vtysh/`、`zebra/` **均在**（`du -sh` = 132M，47 个顶层目录）。
  本轮以 §1.1 的逐文件 md5 相等作为同步结论，未重复该项体积统计。

---

## 2. 构建、组件套件与门禁

### 2.1 组件套件（干净构建目录内从零 build + suite）

```text
$ grep -c ': PASS' suite.log
27
$ grep -c ': FAIL' suite.log
0
$ grep -E 'midrd-virtual-link-test|boundary scan' suite.log | tail -2
midrd-virtual-link-test: PASS
standalone libfrr boundary scan: PASS
```

- **PASS=27、FAIL=0**（`grep -c ': PASS'` = 27、`grep -c ': FAIL'` = 0），**0 error**；
  日志**末行为 `SUITE_EXIT=0`**（即套件退出码 0）。
- 日志：容器内本轮组件套件日志（亦拷至宿主）。
- 用例数说明：**程序数不变**——W3 新增用例 13–19 位于**同一测试程序**
  `midrd-virtual-link-test` 内（§3.4），因此 `: PASS` 行数仍为 27。
- 用例明细（新增部分）：**case 13/14/15/16a/16b/17/18**（D1–D4 回归）与 **case 19**（D1a 订阅）
  共 **8 个测试函数**，均位于 `midr-test/virtual-link-test.c`，runner `main()` 见 `:1372-1379`，映射见 §3.4。
- 唯一的 `warning:` 是**既有的** `lib/mgmt_msg_native.h: declaration does not declare anything`
  （`dp-contract.c` 阶段刻意放宽 `-Werror` 的预期噪声，见 `midr-test/extraction-boundary-test.sh`
  第 21-24 行注释），**没有任何告警提及本轮改动的 6 个文件**。

### 2.2 门禁（a）`midr-test/extraction-boundary-test.sh`

```text
standalone libfrr boundary scan: PASS
```

- 同一轮日志（本轮组件套件日志）末段；整套 `EXIT=0`（§2.1）。
- （`dp-contract.c` 阶段打印的 `lib/mgmt_msg_native.h: declaration does not declare anything`
  是该翻译单元刻意放宽 `-Werror` 后的预期噪声，见脚本第 21-24 行注释，非失败。）

### 2.3 门禁（b）`midr-test/check-zapi-numbering.sh`

**本轮（W3）末补跑**（脚本 md5 未变：`3fe4e30d2b460328b43c7d13126dd0ca`，见 §1.1 未改动清单）。

- 执行环境：**本地工作树**（容器内无 `.git`，脚本依赖 `git rev-parse --show-toplevel`）。
- 输出：`check-zapi-numbering: PASS (baseline 1adb4c92d0 vs working tree)`、`EXIT=0`；
  前置提示行 `note: 147 reference enum members; 139 tolerated pre-existing positional gap(s)`。
- 与被测 ZAPI 文件的对应：本轮**未改动** `lib/zclient.h`（`9bf655093457c0f52f3ef00e353bc338`）、
  `lib/zclient.c`（`aa8fd711c63011e32b80259346e20f5a`）、`lib/log.c`
  （`1e3521bb708c52ae2fa0082d4bd36162`）、`zebra/zapi_msg.c`
  （`d06a54ae36b19bd732a23309137d2e4b`），故本门禁结论对**本轮源码**成立。
- 判定：**PASS（EXIT=0）**，作为**独立门禁项**列出（`§0` 的「三套实测」口径为组件套件 + GRE + stage E；
  本门禁不并入该 112 计数）。原待办 U-7 已关闭。
- **该 PASS 的含义与门禁有效性**：本轮 `lib/zclient.{c,h}` 与 `lib/log.c` **均未改动**（见上一条），
  所以本次 PASS 说明的是「本轮**未引入新 ZAPI 编号**」。门禁本身的有效性由**更早一轮**做过的
  **mutation-teeth 检查**（人为破坏编号 / 漏加 `DESC_ENTRY` 时门禁应 **FAIL**）保证，
  该结论**仍然成立、不因本轮复跑而改变**（本轮未重复该 mutation 检查，故本报告不另给新计数）。

### 2.4 顶层全量 `make -j112 -k`

```text
$ grep -c 'warning:' topmake.log
0
```

- `EXIT=0`（日志**末行 `TOPMAKE_EXIT=0`**，即顶层 make 退出码 0），**`warning:` 计数 0**。
- 日志内容为顶层目录的 `make all-am` entering/leaving 行；日志：容器内本轮顶层构建日志
（亦拷至宿主）。
- zebra 侧产物一致性：stage E 用的 staged `zebra` md5 为
  `0aca362d964891d7a5c118069fbbc197`，与本轮新构建的 `zebra` 一致（§1.2）。

---

## 3. 缺陷修复记录与 D1–D4 / W0 修复映射

> 范围：§3.1–3.3 的缺陷 1–3 由**上一轮（旧源码）**定位并修复；本轮（W3）其源码 md5 未变
> （§1.1 未改动清单），修复效果由本轮**重新构建**的产物再次覆盖（stage E `PASS=34 FAIL=0`，§5.2）。
> §3.4 的 D1–D4/W0 映射与 §3.5 的编译阻塞修正属**本轮（W3）**新增。

### 3.1 缺陷 1（上一轮（旧源码）修复）：`zebra_midr_ipaddr_to_prefix()` 把 overlay 地址主机位清零 → READY 永不达成

- **位置**：`zebra/zapi_msg.c` 的 `zebra_midr_ipaddr_to_prefix()`（本轮行号实测：注释块 `:4278-4285`）。
- **根因**：该函数末尾有一句 `apply_mask()`，会把接口地址的主机位也掩掉。于是
  `192.168.100.1/30` 送达内核时被装成 **`192.168.100.0/30`**（网络地址）。
  midrd 侧用 `connected_lookup_prefix_exact()` 做**精确前缀匹配**确认 overlay 地址，
  地址恒不相等 → 状态机停在 `CONFIGURING` → 每次 `add` 都以超时 `FAILED` 收场并回滚。
- **触发证据**：隔离实验（t=1）在容器内抓到内核装有 `inet 192.168.100.0/30`（应为 `.1/30`）。
  > 说明：该次隔离实验的原始日志在**上一轮**于宿主 `测试机上的日志` 与 `/home/yangmy`
  > （`--include=*.log/*.txt`）中 `grep -l "192.168.100.0/30"` **无命中**，未被保留为可复查文件；
  > 本报告将其记为**触发证据（由 lead 提供），上一轮未复现**。本轮（W3）**同样未复现**
  > （源码已修复，无法在不回退源码的前提下复现），故仍不能计为 PASS。
- **修复**：删除末尾 `apply_mask()`；源码在 `zebra/zapi_msg.c:4278-4285` 留下说明性注释：

  ```c
  /*
   * Do NOT mask the address.  An interface address keeps its host bits
   * (e.g. 192.168.100.1/30); prefixlen only describes the connected
   * route that will be derived from it.  Masking here installed the
   * network address in the kernel (192.168.100.0/30) and, because the
   * control plane confirms the overlay by an exact prefix match on the
   * address it asked for, the virtual link could never reach READY.
   */
  ```

- **修复后可复查证据（上一轮（旧源码）实测，勿作本轮证据）**：
  - stage E `E3.3/E3.4`：`inet 192.168.100.1/30`（midra）/ `inet 192.168.100.2/30`（midrb），主机位保留；
  - 同轮 `E2.1/E2.2`：两端 `state=READY`，返回 ifindex == 内核 ifindex；
  - 双容器 GRE A 用例同样 PASS：`node1 gre1 overlay 192.168.100.1/30 configured`（`上一轮 GRE 日志`）。
- **本轮（W3）对应证据**：GRE overlay 交接行与断言（§4.3）显示
  `overlay_local=192.168.100.1 overlay_remote=192.168.100.2 overlay_plen=30`，主机位保留；
  stage E 复跑 `PASS=34 FAIL=0`（§5.2）。

### 3.2 缺陷 2（上一轮（旧源码）修复）：契约 §5.4 事实错误 —— `lib_handlers[]` **不含** 地址变更通知

- **位置**：`doc/midr-doc/dp-doc/midr-virtual-link-api.md` §5.4（契约文本），影响 `lib/zclient.c` 的默认分发表。
- **根因**：契约初版断言 `lib/zclient.c` 的 `lib_handlers[]` 同时包含接口级与**地址级**通知。
  本轮行号实测：`lib_handlers[]` 定义于 `lib/zclient.c:4757-4776`，其中接口级项为
  `:4765-4768`（`ZEBRA_INTERFACE_ADD/DELETE/UP/DOWN` 这 4 个），**不含**
  `ZEBRA_INTERFACE_ADDRESS_ADD` / `ZEBRA_INTERFACE_ADDRESS_DELETE`。
  因此 midrd 若不自注册，其接口视图**永远没有 overlay 地址**，
  `connected_lookup_prefix_exact()` 恒 false → `CONFIGURING` 永不 `READY`
  （与缺陷 1 叠加时会双双掩盖问题：组件测试用 `connected_add_by_prefix()` 直接构造客户端视图，
  会**掩盖**该缺口）。
- **修复**：midrd **必须自注册**这两个地址 handler ——
  本轮行号实测 `midrd/midr-dp-backend.c:1131-1142`：
  `midr_dp_interface_address_update()`（`:1131`）内部调用
  `zebra_interface_address_read(cmd, zclient->ibuf, vrf_id)`（`:1133`），并在
  `midr_dp_handlers[]`（`:1139-1142`）中把 `[ZEBRA_INTERFACE_ADDRESS_ADD]`（`:1140`）与
  `[ZEBRA_INTERFACE_ADDRESS_DELETE]`（`:1141`）都指向该回调
  （`zebra_interface_address_read` 声明 `lib/zclient.h`、实现 `lib/zclient.c`；
  对照既有用法 `bgpd/bgp_zebra.c`、`ospfd/ospf_zebra.c`）。
  该修复**不新增任何 ZAPI 消息**。
- **契约侧**：已在 §5.4 写入 **P0 勘误**（2026-09-29，vlink-eng 独立评审发现、lead 复核），
  明确原断言错误、后果与 P2 的 MF-1 补步。
- **可复查证据（上一轮（旧源码）实测，勿作本轮证据）**：stage E `E2/E3` 在**真实 zebra**环境中
  达成 `READY` 且地址可见——若 midrd 未接地址通知，`READY` 不可能出现（契约 §5.4 结论段）。
- **本轮（W3）对应证据**：stage E 复跑 `PASS=34 FAIL=0`（§5.2，staged zebra 为本轮新构建）；
  GRE 脚本的 overlay 交接断言（§4.3）同样要求 READY 通知携带正确 overlay 三件套。
  佐证：`check-zapi-numbering` 本轮已补跑 **PASS / EXIT=0**（§2.3），直接对**本轮源码**成立，
  不再引用上一轮结果。

### 3.3 缺陷 3（上一轮（旧源码）修复）：同 outer endpoint 对换设备名被静默重命名 → 加 EEXIST 显式拒绝

- **位置**：`midrd/midr-virtual-link.c`（`midr_virtual_link_add()`）。
- **根因**：GRE 层注册表以 **(vrf, outer_local, outer_remote)** 为键，也以设备名为键。
  当同一 outer endpoint 对用一个**不同的显式设备名**再次添加时，显式名会“胜出”但
  **不会重建内核设备** —— 内核设备仍叫旧名，而对新名的探测永远无法确认，
  请求最终以一个**令人困惑的超时**收场。
- **修复**：在 `midr_gre_interface_add()` 之前做检查，
  若已存在的绑定名与新显式名不同，则**显式拒绝**（`MIDR_VLINK_STAGE_VALIDATE` + `EEXIST`），
  调用方必须先 teardown 旧链路或复用其名字（任务文档 §6：禁止静默覆盖绑定）。
  代码注释见 `midrd/midr-virtual-link.c`（“Task spec §6 forbids silently overwriting a binding”）。
  > 本轮（W3）收窄：D3 之后，「本模块自己的旧条目复用同一 endpoint 对」改为**自动先拆后建**
  > （§3.4），`VALIDATE/EEXIST` **仅剩外来 LIVE 隧道**（同 endpoint 对、由别的模块持有、名字不同）。
- **测试侧适配**：双容器 GRE 的 **case B**（IPv6 over IPv4 GRE）使用**独立 underlay 对**
  `10.1.1.21/22`（脚本变量 `U1B/U2B`），不再与 case A 复用同一 outer endpoint 对。
  > 本轮行号实测：脚本 `midr-test/midr-gre-connectivity-test.sh:30-31` 定义 `U1B/U2B`。
- **本组未复现该拒绝路径**：stage E 只用了单一 `gre1`，未触发 EEXIST。该分支的组件级覆盖
  由 `midrd-virtual-link-test` 承担（本轮：`EXIT=0`、`PASS=27`，§2.1；新增 **case 16b** 覆盖
  外来 LIVE 隧道拒绝，见 §3.4）。**如实标注：真实内核下的 EEXIST 拒绝场景上一轮未构造，
  本轮（W3）仍未构造（§6 U-3）。**


### 3.4 本轮（W3）修复：D1–D4 与 W0（代码位置 + 用例覆盖 + GRE 断言覆盖）

> 行号均为**本轮 `grep -n` 实测**（源码 md5 见 §1.1）。D1–D4 为静态评审缺陷编号
> （契约 §11 / 会签包 **C7**），W0 为「通知事件枚举 + status 新字段 + `ADDRESS_SET` 里程碑通知」
> 这一新增对外可观察面（会签包 **C6**）。

| 项 | 修复点（本轮实测行号） | 组件用例覆盖（`midr-test/virtual-link-test.c`） | GRE 脚本断言覆盖 |
|---|---|---|---|
| **D1a** `midr_vlink_gre_notify` 订阅 | 回调 `midrd/midr-virtual-link.c:546`；在 `midr_virtual_link_init()`（`:1153`）内注册 `:1158`，在 `midr_virtual_link_fini()`（`:1161`）内注销 `:1167` | **case 19** `test_case19_autonomous_gre_notify_failure`（`:1296`，runner `:1379`） | — |
| **D1b** 模块级 reconcile 定时器 | `#define MIDR_VLINK_RECONCILE_INTERVAL_MS 1000`（`:40`）；前置声明 `:100`；`midr_vlink_reconcile_entry()` `:508`、`midr_vlink_reconcile_cb()` `:570`、`midr_vlink_reconcile_sync()` `:590`；定时器（重）挂载 `:614-615` | **case 13** `test_case13_autonomous_device_loss`（`:871`，runner `:1372`） | — |
| **D2** `FAILED` 的 ifindex 规则（保留 `last_ifindex`） | 字段 `last_ifindex` 声明 `:52`；规则在 `midr_vlink_fill_status()`（`:173`）内 `:206-208`：`stage == MIDR_VLINK_STAGE_DEVICE_CONFIRM` → `0`，否则 `last_ifindex` | **case 15** `test_case15_failed_ifindex_retention`（`:1005`，runner `:1374`） | — |
| **D3** `add()` 重建路径先 `del()` + 收窄 `VALIDATE/EEXIST` | 重建前彻底拆除：说明注释 `:825-835`、调用 `midr_virtual_link_del(NULL, e->ifname, NULL)` 在 **`:837`**；收窄后的拒绝注释块 `:846-865`，判定条件 `:866-878`（`EEXIST` 在 `:877`） | **case 16a** `test_case16a_empty_name_rebuild_no_stale_overlay`（`:1053`，runner `:1375`）、**case 16b** `test_case16b_empty_name_live_foreign_pair_refused`（`:1139`，runner `:1376`） | — |
| **D4** `midr_vlink_lost()` 内对账 `gre_created`/`addr_set` | `midr_vlink_lost()` `:476`；对账块 `:492-497`（`midr_gre_interface_get_state()` `:493` → `gre_created` `:494`，随后 `addr_set = false` `:497`） | **case 18** `test_case18_no_stale_bookkeeping_after_loss`（`:1239`，runner `:1378`） | — |
| **W0** 事件枚举 + status 新字段 + `ADDRESS_SET` 里程碑通知 | `midrd/midr-virtual-link.h`：`MIDR_VLINK_STAGE_ADDRESS_SET` `:91`；`enum midr_virtual_link_event` `:105-112`（`MIDR_VLINK_EV_ADDRESS_SET` `:108`）；status 的 `event` `:147`、`overlay_local` `:153`、`overlay_remote` `:154`、`overlay_prefix_len` `:155`、`overlay_ready` `:156`；回调签名不变（`midr_vlink_notify_cb` `:161-162`）。发出点：`midrd/midr-virtual-link.c:710-711` | **case 14** `test_case14_event_ordering_and_overlay_payload`（`:932`，runner `:1373`）、**case 17** `test_case17_query_paths_do_not_notify`（`:1183`，runner `:1377`） | **6 条新断言**：`midr-test/midr-gre-connectivity-test.sh:303`（node1 gre1 v4）、`:304`（node2 gre1 v4）、`:324`（node1 gre2 v6）、`:325`（node2 gre2 v6）、`:345`（node1 gre6 ip6gre）、`:346`（node2 gre6 ip6gre）；断言器 `assert_vlink_overlay()` `:201-215` |

**契约影响（对第一组＝调用方）**：W0 属**追加字段**（回调签名 `midr_vlink_notify_cb` **不变**，源码兼容），
因此不破坏调用方编译；但语义上要求调用方**按 `status->event` 分支**，不能只看 `state`——
`ADDRESS_SET` 时刻 `overlay_ready` 仍为 `false`（只有 `READY` 才为 `true`）。
变更原因、影响与备选方案已逐条记录在会签包 `midr-virtual-link-api-countersign.md` **C6**（新字段）
与 **C7**（D1–D4 语义修正），契约侧对应 `midr-virtual-link-api.md` 头部 W3 修订说明与 §3.1/§5.1/§9.2。
本轮**未新增/未修改契约签名**，故无需新的勘误条目。

**未覆盖项（如实标注）**：D1–D4/W0 的**真实内核**行为（而非组件级 wrap 注入）在 stage E 只被
间接覆盖（READY/重建/zebra 重启），`VALIDATE/EEXIST` 的真实内核拒绝路径仍为 **§6 U-3（未执行）**。

### 3.5 本轮（W3）构建阻塞：`gre-link-tool.c` 文档注释被 `*/` 提前闭合（已修复并复验）

- **现象（原始失败，逐字记录）**：编译 `midrd/gre-link-tool.c` 时报
  `error: unknown type name 'event'`，并在 `sys/types.h` 内产生级联报错。
- **根因**：文件顶部文档注释中写了字面量 `overlay_*/event`（第 **46** 行，旧内容），
  其中的 `*/` **提前闭合了块注释**，其后的文本被当作 C 代码编译，故出现 `unknown type name`。
- **修复**：把该处字面量改为 `overlay_* and event` —— 即本轮实测的
  `midrd/gre-link-tool.c:46`（`* The overlay_* and event tokens are appended so the existing
  name/state/ifindex/`），从而不再出现 `*/` 序列。
- **复验**：修复后重新构建 → 组件套件 `EXIT=0`、`PASS=27`、0 error（§2.1），
  顶层 `make -j112 -k` `EXIT=0`、`warning:` = 0（§2.4），GRE 与 stage E 均 `FAIL=0`（§4.1、§5.2）。
- **产物 md5 变化**：`midrd/gre-link-tool.c` = `e2646a2b0f8f07daaf73dbf546b41fd0`（386 行，§1.1）。


---

## 4. 双容器 GRE 连通性（交付项 3）

脚本：`midr-test/midr-gre-connectivity-test.sh`（**本轮** md5 `8cf57126d3798ed8af44e1668f3d77a7`，
388 行；上一轮 md5 `739de8439c21a9c1785be284275dd68b`），在**宿主**以 root 运行，通过**新虚链路 API**
（`midrd-gre-tool vlink-setup`，零手工 `ip addr add` / `ip link set up`）在 `node1`/`node2` 间建
GRE over docker bridge。本轮执行器为从零构建的 `midrd-gre-tool`
（md5 `7c3bf35fea958e31d417315d24e3ad02`）。

### 4.1 结果（本轮 W3）

```text
log: 本轮 GRE 日志（宿主；文件名 gre.log）
=== summary: PASS=51 FAIL=0 ===
```

- `EXIT=0`，**PASS=51 FAIL=0**。
- 计数从上一轮（旧源码）的 **45** 升到 **51**，原因是脚本新增 **6 条 overlay 交接断言**
  （`gre1` v4、`gre2` v6、`gre6` ip6gre，node1/node2 各一条 = 6 条）：
  **这是新增覆盖，不是回归**——原有用例全部保留。
- 脚本 `install_artifacts()` 从构建容器 `frr-ubuntu24-ymy` 重新 `docker cp` 出
  `midrd-gre-tool`/`zebra` 并用 `ldd` 解析真实 `libfrr.so.0` 覆盖到两节点的
  `/opt/midr-dp/` —— 即**不用旧 staged 产物**。

### 4.2 用例组（场景覆盖，原始片段为上一轮（旧源码）摘录）

> 下表的分组与场景在本轮（W3）**全部保留**；但表中 **ifindex 具体数值（40/35、41/36、42/37）
> 属上一轮（旧源码）取值**，本轮日志未逐条摘录，故不引用为 W3 证据。

| 组 | 内容 | 关键断言（上一轮（旧源码）原始片段） |
|---|---|---|
| A | IPv4 GRE（`gre`）+ IPv4 overlay | `node1/2 gre1 state=READY via API (ifindex=40/35)`；`returned ifindex … matches kernel`；`overlay 192.168.100.1/30 … configured`；`IPv4 connectivity node1 -> node2 … / reverse` |
| B | IPv6 overlay over IPv4 GRE，**独立 underlay 对** `10.1.1.21/22` | `underlay IPv4 10.1.1.21 -> 10.1.1.22`；`gre2 state=READY (ifindex=41/36)`；`overlay fd00:100::1/64 …`；`IPv6 connectivity … / reverse` |
| C | `ip6gre` + IPv6 overlay | `gre6 is an ip6gre device`；`gre6 state=READY (ifindex=42/37)`；`overlay fd00:200::1/64`；`IPv6 connectivity … / reverse` |
| D | 幂等 re-add | `node1 gre1 re-add state=READY ifindex=40`（与首次同 ifindex） |
| E | teardown 清理 | `node1/node2 gre1/gre2/gre6 removed`；`gre1 overlay address cleaned` |

> case B 使用独立 underlay 对即缺陷 3 的测试侧适配（同 outer endpoint 对不得换名复用；见 §3.3）。

### 4.3 本轮新增 overlay 交接证据（W0 / `ADDRESS_SET`→`READY`）

工具（`midrd-gre-tool`）本轮起在 `MIDR_VLINK` 状态行追加 overlay 三元组与事件字段，例如：

```text
MIDR_VLINK name=gre1 state=ready ifindex=58 iftype=8 err=0 overlay_local=192.168.100.1 overlay_remote=192.168.100.2 overlay_plen=30 overlay_ready=1 event=none
```

- 末行是**纯查询**（`event=none`）；**READY 迁移通知行**才以 `overlay_ready=1 event=ready` 结尾。
- IPv6 用例（`gre2`）的对应值为
  `overlay_local=fd00:100::1 overlay_remote=fd00:100::2 overlay_plen=64`。

脚本新增的 6 条断言据此打印（**本轮实测，共 6 条**，此为该类断言的原始格式）：

```text
[PASS] node1 gre1 READY overlay_local=192.168.100.1 overlay_remote=192.168.100.2 plen=30 event=ready
```

- 断言器 `assert_vlink_overlay()`：`midr-test/midr-gre-connectivity-test.sh:201-215`；
  调用点 `:303`/`:304`（`gre1`，node1/node2）、`:324`/`:325`（`gre2`，node1/node2）、
  `:345`/`:346`（`gre6`，node1/node2）。
- 实现依据：`midr_vlink_notify()` 发出的 `MIDR_VLINK_EV_READY` 通知携带
  `overlay_local`/`overlay_remote`/`overlay_prefix_len`（§3.4 的 W0 行）。

---

## 5. Stage E：跨 **BGP-only** underlay 验收（交付项 4）

### 5.1 拓扑（上一轮（旧源码）**新建**，未触碰既有容器/网络；W3 沿用同一流程）

```text
                    AS65000  midrrtr
   midra 10.20.1.1/24  \  10.20.1.254/24 (eth0)      /  10.20.2.254/24 (eth1)
   AS65001              [ midr-stagee-a ]           [ midr-stagee-b ]        midrb 10.20.2.1/24
                                                                               AS65002
```

- 两个**独立 docker bridge**：`midr-stagee-a` = `10.20.1.0/24`、`midr-stagee-b` = `10.20.2.0/24`；
  因此两网段**不是同一直连网段**，midra 只能**经 BGP** 学到 `10.20.2.0/24`。
- 容器：`midra` / `midrb` / `midrrtr`（镜像 `frr-ubuntu24-ymy:init`，`--privileged`）。
- 每个容器内跑**本轮（W3）新构建的** `zebra`（经宿主中转 `docker cp` 后 md5 回比，§5.2）；
  overlay 用 `gre1`（`192.168.100.1/30` ↔ `192.168.100.2/30`）。
- 全部 `docker exec` 均 `-u root`；`ip route del default` 清掉 docker 网桥默认路由，
  保证 outer 可达性**只能**来自 BGP。

脚本（此前几轮新建于宿主，**本轮已入库**）：`midr-test/r7-dp-stagee-bgp-underlay.sh`。
本轮（W3）沿用同一流程复跑，输出日志改为本轮 stage E 日志：

```sh
# 宿主；$SUDO 见 §7.2
$SUDO bash midr-test/r7-dp-stagee-bgp-underlay.sh > stagee.log 2>&1
```

### 5.2 结果（本轮 W3）

```text
log: 本轮 stage E 日志（宿主；文件名 stagee.log）
=== stage E summary: PASS=34 FAIL=0 ===
staged zebra md5 = 0aca362d964891d7a5c118069fbbc197（= 本轮新构建的 zebra，§1.2）
```

- `EXIT=0`，**PASS=34 FAIL=0**（计数与上一轮（旧源码）相同，但本轮跑在**新构建**的产物上）。

### 5.3 逐条断言与原始输出（**上一轮（旧源码）**摘录）

> 说明：本节 E1–E7 的原始片段来自**上一轮（旧源码）**的 stage E 日志（`上一轮 stage E 日志`，
> 186 行）。本轮（W3）的 stage E 日志（本轮 stage E 日志）**只保留了汇总**，
> 未逐条摘录进本报告；W3 的结论以 §5.2 的 `PASS=34 FAIL=0` 为准。
> 因设备名/ifindex 由内核分配，下列 ifindex（8/9/10）**不得**当作本轮取值。
> 场景与断言项目在本轮（W3）全部保留。

**E1 outer 可达性来自 BGP（不是直连）**

```text
--- midra ip route show ---
10.20.1.0/24 dev eth0 proto kernel scope link src 10.20.1.1
10.20.2.0/24 nhid 6 via 10.20.1.254 dev eth0 proto bgp metric 20       <== proto bgp
--- midra ip route get 10.20.2.1 ---
10.20.2.1 via 10.20.1.254 dev eth0 src 10.20.1.1 uid 0
[PASS] E1.1 midra has 10.20.2.0/24 via 10.20.1.254 proto bgp
[PASS] E1.2 no connected route to 10.20.2.0/24 on midra
[PASS] E1.3 ip route get 10.20.2.1 on midra resolves via 10.20.1.254 (BGP nexthop)
[PASS] E1.4 midra -> 10.20.2.1 ping works through the BGP-only underlay
[PASS] E1.5 midrb has 10.20.1.0/24 via 10.20.2.254 proto bgp
```

BGP 侧对照（`vtysh --vty_socket <隔离目录> -d bgpd`）：

```text
--- midrrtr show bgp summary ---
Neighbor        V         AS   MsgRcvd   MsgSent   TblVer  InQ OutQ  Up/Down State/PfxRcd   PfxSnt
10.20.1.1       4      65001        11        10        2    0    0 00:00:22            0        2
10.20.2.1       4      65002         8         7        2    0    0 00:00:18            0        2
--- midra show bgp ipv4 unicast（收表）---
 *>  10.20.2.0/24     10.20.1.254              0             0 65000 i
```

网段隔离的原始证据（`ip -o addr show`）：

```text
midra   eth0: 10.20.1.1/24
midrb   eth0: 10.20.2.1/24
midrrtr eth0: 10.20.1.254/24   eth1: 10.20.2.254/24
```

**E2 两端 READY（API 返回值 + 内核 ifindex 一致）**

```text
[PASS] E2.1 midra state=READY via API (ifindex=8)
[PASS] E2.2 midrb state=READY via API (ifindex=8)
[PASS] E2.3 midra API ifindex 8 == kernel 8
[PASS] E2.4 midrb API ifindex 8 == kernel 8
```

**E3 overlay 地址与接口状态**

```text
8: gre1@NONE: <POINTOPOINT,UP,LOWER_UP> mtu 1400 qdisc noqueue … link/gre 10.20.1.1 peer 10.20.2.1
8: gre1    inet 192.168.100.1/30 brd 192.168.100.3 scope global gre1     (midra)
8: gre1    inet 192.168.100.2/30 brd 192.168.100.3 scope global gre1     (midrb)
[PASS] E3.3 midra carries exactly 192.168.100.1/30
[PASS] E3.4 midrb carries exactly 192.168.100.2/30
```

（主机位保留 —— 即缺陷 1 修复后的可复查证据。）

**E4 overlay 双向 ping / E5 `route get` 命中虚接口**

```text
[PASS] E4.1 midra -> 192.168.100.2 over gre1
[PASS] E4.2 midrb -> 192.168.100.1 over gre1
--- midra ip route get 192.168.100.2 ---
192.168.100.2 dev gre1 src 192.168.100.1 uid 0
[PASS] E5.1 route get 192.168.100.2 hits dev gre1
```

**E6 返回的 ifindex 可交给一/二组（kernel/FIB 层面可用）**

```text
[{"ifindex":8,"ifname":"gre1",…,"link_type":"gre","address":"10.20.1.1","broadcast":"10.20.2.1"}]
[PASS] E6.1 midra installed a FIB route keyed by gre1 (ifindex 8)
--- midra ip route show 198.51.100.0/24 ---
198.51.100.0/24 via 192.168.100.2 dev gre1
[PASS] E6.2 FIB entry present (via 192.168.100.2 dev gre1)
[{"dst":"198.51.100.1","gateway":"192.168.100.2","dev":"gre1","prefsrc":"192.168.100.1",…}]
[PASS] E6.3 dataplane lookup for 198.51.100.1 resolves to gre1
```

即：API 返回的 ifindex（= 内核 ifindex）是可被内核 FIB 直接引用的有效句柄
（`ip route add … dev gre1` 成功且 `ip -j route get` 命中该虚接口）。

**E7 删除 / zebra 重连 / 设备重建后恢复**

```text
[PASS] E7.1 midra gre1 removed by teardown
[PASS] E7.1 midrb gre1 removed by teardown
[PASS] E7.2 rebuild after teardown -> READY on both ends (A=9 B=9)
[PASS] E7.3 overlay ping again after rebuild
[PASS] E7.4 after zebra restart re-establish -> READY, ifindex 9 == kernel 9
[PASS] E7.5 overlay ping after zebra restart
[PASS] E7.6 kernel device deleted -> recreated, READY, ifindex 10
```

> 严谨性说明：`midrd-gre-tool` 每次调用都是**独立进程**，无法在 CLI 层面观察
> 「zebra 重连 → 旧 ifindex 失效、desc 保留」的**进程内**状态机迁移
> （契约 §8.2 该语义由 `midrd-virtual-link-test` 组件用例覆盖）。
> stage E 验证的是**可观察结果**：zebra 重启后能以新客户端重新达成 `READY`
> 且 overlay 数据面恢复；内核设备被删除后能重建并拿到新的 ifindex（上一轮（旧源码）摘录为 10）。

### 5.4 上一轮（旧源码）执行中的一个环境配置发现（非源码缺陷）

首次运行 stage E 时 **PASS=26 FAIL=8**：BGP 会话虽 Established，但 midrrtr 侧报
`EBGP outbound policy not properly setup`，midra 收不到任何前缀
（`ip route get 10.20.2.1` → `Network is unreachable`）。按 FRR 默认的
`bgp ebgp-requires-policy` 语义，eBGP 需显式策略才收发路由。修正测试拓扑配置
（`no bgp ebgp-requires-policy` + `route-map RM-EBGP permit 10` 应用为邻居 in/out）后
复跑 → **34/0**。这是**拓扑/配置问题**，不涉及产品源码；如实记录该次失败历史。
本轮（W3）沿用已修正的拓扑配置，未再出现该失败。


---

## 6. 未执行项、环境阻塞与需上报的发现

> 纪律：未标注「本轮（W3）已执行」的项，**均未取得本轮通过判定，不得计入 §0 的 112 PASS**。

| 编号 | 项 | 状态 | 说明 |
|---|---|---|---|
| U-1 | 顶层 `make -j112 -k` | **本轮（W3）已执行，PASS** | 容器内 `EXIT=0`，`warning:` = **0**；日志 本轮顶层构建日志（§2.4）。更早一轮的从零 `make clean && make -j112` 记录在案（11 条告警，全部为既有 `bgpd/` 告警，OUT OF SCOPE）。 |
| U-2 | `vtysh` 的 **CLI 端到端功能**验证 | **本轮（W4）已补跑 PASS** | 容器 `frr-ubuntu24-ymy` 内以 `--vty_socket <隔离目录> --no-zebra --node-id 7 --listen 127.0.0.1:47123 --lifetime 4000` 运行**树内** `midrd`（`LD_LIBRARY_PATH` 指向树内 libfrr），用**树内** `vtysh/vtysh`：`-c 'clear midr traceroute cache'` → `Cleared 0 MIDR traceroute cache entries`（rc=0）；`-c 'show midr spf'` → `SPF generation 1, 0 routes`（rc=0），与 daemon stdout 的 `node=7 spf generation=1 routes=0` **一致**；反向对照：`clear midr traceroute banana` → `% Unknown command`（rc=1）、daemon 停止后 → `Exiting: failed to connect to any daemons.`（rc=1）。物证：`nm midrd/.libs/midrd` 含 `clear_midr_traceroute_cache`。**未发现源码缺陷**。前置条件（本轮实测发现）：必须用树内 libfrr + 树内 vtysh，且树内前缀目录存在，见 §0.2 第 5 项与「新发现 2」。 |
| U-3 | 缺陷 3 的 **真实内核 EEXIST 拒绝**路径 | **本轮（W4）已构造并通过** | 在 `midr-test/midr-gre-connectivity-test.sh` 新增用例 F：先用 GRE 层（不经 vlink）建 `foreign0`（端点对 10.1.1.31/32，真实内核 + 真实 zebra），再用 vlink 请求**同一端点对换名** → 被拒绝、未创建新设备、外来设备 ifindex/地址未变、内核 GRE 设备数不变；删掉外来隧道后同一请求**成功 READY**。**语义修正**：跨进程场景的实际错误码是 `110(DEVICE_CONFIRM 超时)` 而非 `17(EEXIST)`——EEXIST 守卫是进程内的（§0.2「新发现 1」）；断言按「必须是拒绝」编写并打印实际码。组件级覆盖仍为 case 16b。 |
| U-4 | 缺陷 1 的**隔离实验原始日志** | **容器内原件已失效；测试机副本尚存** | 该实验的原始输出（2 798 B，2026-09-29 08:43）原在测试容器内并回拷到测试机；本轮复核：**容器内的原件已不存在**，测试机上的副本仍在（**未随仓库分发**）。本行保留根因证据行：`37: t1    inet 192.168.100.0/30 brd 192.168.100.3 scope global t1`（应为 `192.168.100.1/30`），以及同窗口的 `t1@NONE: <POINTOPOINT,UP,LOWER_UP> link/gre 10.1.1.11 peer 10.1.1.12` 与随后设备被回滚删除的记录。若要长期可核查，应把该副本入库或重做一次隔离实验（本轮按「只修正措辞」处理，未入库）。 |
| U-5 | MIDR Session / EOR / SPF 闭环 | **本组边界外** | 契约 §1：本组只做设备/地址/状态与 ifindex 可用性；Session/SPF 由一/二组联合验证。 |
| U-6 | stage E 容器与网络 | **本轮（W3）已复核：无残留** | 本轮 W3 的 stage E 跑完后已复核：`docker ps -a` 中无 `midra`/`midrb`/`midrrtr`，`docker network ls` 中无 `midr-stagee-*`；`node1`/`node2` 内 `gre1`/`gre2`/`gre6` 均不存在且 zebra 已停（`pgrep -x zebra` 无结果）。上一轮同项亦已清理。仍残留的仅 `测试机上的 stage E 临时配置目录` 目录与 `测试机上的 zserv socket 路径` socket（脚本不清理，见 U-9）。 |
| U-7 | 门禁 `midr-test/check-zapi-numbering.sh` | **本轮（W3）已补跑：PASS（`EXIT=0`）** | 在本地工作树执行，输出 `check-zapi-numbering: PASS (baseline 1adb4c92d0 vs working tree)`；脚本与被测 ZAPI 文件 md5 本轮均未变（§1.1、§2.3）。作为**独立门禁项**记录，不并入「三套实测」的 112。门禁有效性（更早一轮的 mutation-teeth 检查：人为破坏编号应 FAIL）仍成立（§2.3）。 |
| U-8 | **第一组（调用方）会签** | **仍待会签（未取得回签）** | 会签材料 `doc/midr-doc/dp-doc/midr-virtual-link-api-countersign.md`（§3 变更清单 C1–C7 及对调用方的影响、§4 九问、§7 回签表）**尚无第一组签名/日期**；与契约 §11 第 1 条一致。本报告 §3.4 已把 W0/C6 与 D1–D4/C7 的对调用方影响逐条复述，供回签引用。 |
| U-9 | 测试脚本的**环境清理** | **未处理（须做 housekeeping）** | 两个测试脚本**都不**清理运行时残留：`midr-test/midr-gre-connectivity-test.sh` 的 `cleanup()` 只 `rm -rf "$STAGE"`（`:256`），其 `SOCK`（`:46`）在退出后仍可能留在测试机上；宿主侧 stage E 脚本产生的临时配置目录同样不在任何脚本的清理范围内（本轮 grep：仓库内无 `stageE-conf` 命中）。属 housekeeping，**不影响**任何断言结果，但会随轮次累积。 |

**跨组提示（第一组 → 第二组，属他人接口，本组不实现）**：第一组从 `READY` 通知/状态中拿到的是
`overlay_local` + `overlay_remote` + `overlay_prefix_len` 三件套；但 `midrd/midr-topology.h` 的
`struct midr_link_update` **只有** `link_local_address` / `link_remote_address`，**没有前缀长度字段**
（本轮 grep 确认 `midrd/midr-topology.h` 内无 `prefix_len` / `prefixlen` / `overlay` 命中；
`struct midr_link_update` 的地址字段为 `:57` `link_local_address` / `:58` `link_remote_address`；
该目录下**不存在** `midrd-topology.c`）。
因此第一组把 overlay 下一跳连同前缀交给第二组时，**可能需要在第一组/第二组一侧新增一个
prefix-length 字段**（或约定由哪一侧补齐）。这属第一、第二组之间的接口，不在本组设备 API 范围内；
第三组只负责把 `overlay_prefix_len` 如实交付给调用方。同一提示已写入契约 §11 第 5 条与会签包 §8 末行。

> **第三组结论（不改第一/二组接口）**：按现状**不需要**补 prefix length。全链路都把这两个字段当 **locator（主机地址）**使用：TED 以定长拷贝/比较（`midrd/midr-ted.c`）；SPF nexthop 只用 `(family, ifindex, address)`（`midrd/midr-spf.h`、`midrd/midr-spf-install.c`）；midrd→bgpd 的 LS 编码把地址写进 `MIDR_LS_TLV_LINK_LOCAL/REMOTE_ADDRESS`，其值就是 `AFI + 地址`（`bgpd/bgp_midr_codec.c`），与 BGP-LS 的 interface/neighbor address 子 TLV 一致——那类子 TLV 本身不带前缀。只有当第一/二组引入「前缀语义」（on-link 判定、overlay 子网广告/比较、按前缀最长匹配）时才需要，且应作为**新增属性/TLV**（而不是改既有地址 TLV 的值长度），并同时考虑节点层 `transport_address` 的对称性。第三组侧无需改动：`overlay_prefix_len` 已随 `midr_virtual_link_status` 与每次通知交付。决定权在第一/二组。

**需上报 lead 的一个观察（非失败）**：顶层 `make` 的 xref 阶段输出
`[VIEW_NODE] …: help string mismatch`，指出 `midrd/group1/midr_nds_vty.c:2088` 与
`bgpd/bgp_midr_nds_vty.c:2187` 在**同一 VIEW_NODE** 定义了两条同名命令
`show midr admission`，但 help 文本不同（`Show Tier1 admission state` vs
`… for all BGP instances`）。该诊断**未阻断构建**，建议由第一组确认是否需要重命名或合并。
说明：该 xref 诊断不是编译器 `warning:`（故 `warning:` = 0 不能证明它是否仍出现）。
本轮（W3）的本轮顶层构建日志内容为顶层目录的 `make all-am` entering/leaving 行，
**末行为 `TOPMAKE_EXIT=0`**；该日志未记录 xref 阶段的 `[VIEW_NODE]` 输出，
因此无法据此判断该诊断在本轮是否出现，本项继续标 **待实测 / not measured**，
上述描述沿用上一轮（旧源码）记录。

**未触碰的既有资产**：`frr-dut`/`tg-sender`/`tg-receiver`/`node1`/`node2`/`receiver-172`
原样保留（GRE 用例只在 `node1/node2` 上做临时设备增删并已 teardown）；
`/opt/midr` 旧资产未改动；`/opt/midr-dp` 被 GRE 脚本按既有语义重新覆盖（未用于 stage E 判定）。

---

## 7. 证据索引

### 7.1 日志与产物

| 证据 | 位置 | 关键内容 |
|---|---|---|
| 组件套件日志（**本轮 W3**） | 容器内本轮组件套件日志<br>宿主副本同源 | `PASS=27 FAIL=0`（`: PASS`=27、`: FAIL`=0）、0 error、日志末行 `SUITE_EXIT=0`；`midrd-virtual-link-test: PASS`；`standalone libfrr boundary scan: PASS`；唯一告警为既有 `lib/mgmt_msg_native.h` 噪声（文件大小未记录 / not measured） |
| 顶层全量 build 日志（**本轮 W3**） | 容器 `:本轮顶层构建日志`<br>宿主副本同名 | `EXIT=0`，`warning:` = 0 |
| 双容器 GRE 日志（**本轮 W3**） | 宿主侧本轮 GRE 日志 | `=== summary: PASS=51 FAIL=0 ===`；含 6 条 `[PASS] nodeN greN READY overlay_local=… event=ready` |
| GRE 执行器（**本轮 W3**） | 从零构建的 `midrd-gre-tool` | md5 `7c3bf35fea958e31d417315d24e3ad02` |
| 组件测试程序（**本轮 W3**） | 从零构建的 `midrd-virtual-link-test` | md5 `4f4ace0e88dede3ca6ddbc8d7c7e1b52` |
| stage E 日志（**本轮 W3**） | 宿主侧本轮 stage E 日志 | `=== stage E summary: PASS=34 FAIL=0 ===`；staged zebra md5 `0aca362d964891d7a5c118069fbbc197`（文件大小/行数未记录 / not measured） |
| stage E 脚本 | 仓库内 `midr-test/r7-dp-stagee-bgp-underlay.sh`（`make stagee-smoke` 亦可） | 完整拓扑搭建 + 34 条断言 |
| 上一轮（旧源码）日志（**对照，勿作本轮证据**） | 组件 `上一轮组件套件日志`（660657 B）、`上一轮组件套件日志`；双容器 GRE `上一轮 GRE 日志`（10809 B）、`上一轮 GRE 日志（seam 删除后复跑）`；顶层 `上一轮顶层全量构建日志`；stage E `上一轮 stage E 日志`（186 行）；缺陷 1 根因 `上一轮缺陷 1 隔离实验日志`；vtysh `上一轮 vtysh 运行日志` | PASS = 27 / 45 / 34；缺陷与 CLI 证据 |
| ZAPI 编号门禁输出 | 本地终端（**本轮 W3 补跑**，§2.3） | `check-zapi-numbering: PASS (baseline 1adb4c92d0 vs working tree)`，`EXIT=0` |
| 源码修复点（行号为本轮 `grep -n` 实测） | `zebra/zapi_msg.c:4278-4285`（不掩码说明，缺陷 1）<br>`midrd/midr-dp-backend.c:1131-1142`（`midr_dp_handlers` 注册地址通知，缺陷 2）<br>`midrd/midr-virtual-link.c:825-837`、`:846-878`（重建先拆 + `VALIDATE/EEXIST` 收窄，缺陷 3/D3）<br>D1a `:546`/`:1153`/`:1158`/`:1167`；D1b `:40`/`:100`/`:508`/`:570`/`:590`/`:614-615`；D2 `:52`/`:173`/`:206-208`；D4 `:476`/`:492-497`；W0 `:710-711` + `midrd/midr-virtual-link.h:91/105-112/147/153-156/161-162` | 本轮修复点与 D1–D4/W0 映射（§3.4、§3.5） |

### 7.2 复现 / 复核命令

```sh
SSH='ssh -i ~/.ssh/frr yangmy@101.6.30.220 -p 50022'
SUDO='echo thu325325 | sudo -S'
# 组件套件（本轮：干净构建目录，从零 build + suite）
$SSH "$SUDO docker exec -u root frr-ubuntu24-ymy bash -c 'cd /home/frr/frr-midrd3/midrd && make -j112 BUILD_DIR=本轮从零构建目录 test' > 本轮组件套件日志 2>&1"
# 顶层全量
$SSH "$SUDO docker exec -u root frr-ubuntu24-ymy bash -c 'cd /home/frr/frr-midrd3 && make -j112 -k' > 本轮顶层构建日志 2>&1"
# 门禁（需 git，在本地/宿主的带 git 工作树；本轮 W3 已补跑 PASS，§2.3）
./midr-test/check-zapi-numbering.sh
# GRE（宿主；用本轮新构建的 midrd-gre-tool；确切调用式本轮未记录，见下注）
$SSH "$SUDO bash <宿主上的>midr-gre-connectivity-test.sh > 本轮 GRE 日志 2>&1"
# stage E（宿主；脚本已在仓库内，也可用 make stagee-smoke）
$SSH "$SUDO bash midr-test/r7-dp-stagee-bgp-underlay.sh > stagee.log 2>&1"
# stage E 清理（如需）
$SSH "$SUDO docker rm -f midra midrb midrrtr; $SUDO docker network rm midr-stagee-a midr-stagee-b"
```

> 注：GRE 一步在宿主上运行脚本（本轮用的执行器为
> 从零构建的 `midrd-gre-tool`，md5 `7c3bf35fea958e31d417315d24e3ad02`）；
> 脚本在宿主的**确切路径/调用参数本轮未记录**，标为 待实测 / not measured，
> 复现时可参考上一轮的调用形式（`midr-test/midr-gre-connectivity-test.sh` 的用法说明）。

---

## 8. 结论

1. **同步与构建**：容器源码树与本轮本地工作树**逐文件 md5 相同**（6 个改动文件 + 5 个未改文件，§1.1），
   并在**独立干净构建目录**内从零重建；stage E 侧对 staged `zebra` 再次
   md5 回比（`0aca362d964891d7a5c118069fbbc197`）—— **未使用旧二进制**。
   上一版 §1.1 对 `midrd/midr-virtual-link.c` 记录的旧哈希已判为**陈旧并更正**（§0.1 勘误 1）。
2. **门禁与构建**：组件套件 `PASS=27`、`EXIT=0`、0 error（含 `midrd-virtual-link-test`，新增用例 13/14/15/16a/16b/17/18 与 case 19，§3.4）、
   `extraction-boundary-test` PASS、顶层全量 `make -j112 -k` `EXIT=0` 且 `warning:` = **0**；
   `check-zapi-numbering` 本轮已补跑 **PASS（`EXIT=0`，§2.3）**，作为**独立门禁项**列出
   （不并入「三套实测」的 112）。
3. **交付项 3（双容器 GRE）**：`PASS=51 FAIL=0`，覆盖 IPv4 GRE / IPv6-over-GRE / `ip6gre`、
   READY 与 ifindex 一致、overlay 双向 ping、幂等 re-add、teardown 清理，全程零手工 `ip` over overlay；
   新增 6 条 overlay 交接断言（§4.3），故计数较上一轮（旧源码）的 45 增加 6 ——
   **是新增覆盖，不是回归**。
4. **交付项 4（stage E，BGP-only underlay）**：`PASS=34 FAIL=0`，跑在**本轮新构建**的 zebra 上；
   拓扑为真实双 bridge + 三容器，outer 可达性**由 BGP 学到**；两端 READY；overlay 地址/状态正确；
   删除/zebra 重启/设备重建后恢复（逐条原始片段见 §5.3，属上一轮（旧源码）摘录）。
5. **本轮修复**：D1a/D1b/D2/D3/D4/W0 的代码位置、用例覆盖与对调用方影响见 §3.4；
   本轮另修复一个构建阻塞——`midrd/gre-link-tool.c` 文档注释被 `*/` 提前闭合（§3.5），
   修复后三套实测全绿。
6. **如实未通过 / 未执行**：U-3（真实内核 EEXIST 场景，仍未构造）、
   U-2（vtysh CLI 本轮未重跑）、U-8（第一组会签仍待签）、U-9（测试脚本不清理
   `测试机上的 stage E 临时配置目录` 与 `测试机上的 zserv socket 路径` 残留）。
   已补测/已复核项：U-7（编号门禁本轮**已补跑 PASS**，§2.3）、U-6（本轮 stage E 清理**已复核无残留**，§6）。
   U-5（Session/SPF 闭环）为本组边界外。另：xref `help string mismatch` 观察项仍在，
   本轮 topmake 日志只含顶层 `make all-am` entering/leaving 行（末行 `TOPMAKE_EXIT=0`）、
   未记录 xref 输出，故该项仍标 待实测 / not measured（§6）。
7. **本轮（W3）三套实测合计：PASS = 27（组件）+ 51（GRE）+ 34（stage E）= 112，FAIL = 0**；
   组件套件 `: FAIL` = 0 且末行 `SUITE_EXIT=0`（§2.1），GRE/stage E 的 `FAIL=0` 见各自日志（口径见 §0）。
