# dp-02 构建与测试

## 1. 结论

数据面迁移后的构建分三档：FRR 顶层 autotools 构建（权威严格构建）、
`midrd` 独立组件套件（含数据面单测与边界扫描）、以及容器内同步/构建流程。
顶层构建与组件套件均已在本周期验证通过；此外新增第三组 ZAPI/FIB smoke
（`midrd/r7-dp-fib-smoke.sh`），在构建容器内用真实 zebra + 真实内核 FIB 验证完整
ZAPI/FIB 链路，**不依赖第二组会话层**（见第 4.1 节）。

## 2. 范围

- FRR 顶层 `make`（含 `lib/`、`zebra/` 的 GRE 支撑与新 `midrd` 目标）。
- `midrd/Makefile` 的 `test` 目标（25 个程序 + 边界扫描）。
- `midrd/dp-backend-test.c`（包裹 `zclient_route_send`，无需真实 zebra）。
- harness 构建产物 `midrd-gre-tool`（`gre-link-tool.c`）与
  `midrd-dp-e2e-tool`（`dp-e2e-tool.c`）。
- 第三组 ZAPI/FIB smoke `midrd/r7-dp-fib-smoke.sh`（构建容器内 root，真实 zebra +
  真实内核 FIB）。
- 容器内源码同步与构建命令。

## 3. FRR 顶层构建

```sh
# 容器 frr-ubuntu24-ymy，源码树 /home/frr/frr-midrd3
cd /home/frr/frr-midrd3
make clean && make -j112        # 从零重建：EXIT=0；11 行 warning（非 0），明细见 3.1
```

| 项 | 值 |
| --- | --- |
| 命令 | `make clean && make -j112`（从零重建） |
| 结果 | EXIT=0；**11 行 `warning:`（非 0）**，全部为既有 bgpd midr 告警（2 条 DPLANE_OP_GRE_* 已由 T9 消除；T9 前为 13） |
| 日志 | 容器 `frr-ubuntu24-ymy` 的 `/tmp/verify-clean-T10.log`、`/tmp/verify-build-T10.log`（T9 前：`/tmp/verify-build-7ad4671558.log`） |
| 状态 | 已验证（最终 11）；旧「0 警告/错误」结论已更正，见 3.1 |

### 3.1 从零重建的告警实测（最终 11 条，非 0；T9 前 13 条）

`make clean && make -j112` 的最终日志为容器内 `/tmp/verify-build-T10.log`（1199 行，
`build_exit=0`），`grep -c 'warning:'` 得 **11**，且这 11 条**全部**在 `bgpd/` 内
（`grep warning: ... | grep -v bgpd/` 为空）。T9 修 `zebra/dplane_fpm_nl.c` **之前**
的一次从零重建（`/tmp/verify-build-7ad4671558.log`，1205 行）为 **13** 条；多出的
2 条正是 `zebra/dplane_fpm_nl.c:978` 的 `DPLANE_OP_GRE_ADD`/`DPLANE_OP_GRE_DELETE`
`-Wswitch`，T9 把它们补进 `dplane_fpm_nl.c` 的 no-op 分组后归零。

文档早前写下的「0 警告/错误」来自一次**增量构建**：当时源码树基本是最新的，`make`
只重编了 `vtysh/vtysh_cmd.*.o` 并重链 `vtysh`（旧日志 `/tmp/verify-build.log` 仅
10 行 `CC`，没有重编任何 `zebra/`、`bgpd/` 翻译单元），因此这些**潜伏的 `-Wswitch`
缺口根本没有被触发**；只有在 `make clean` 后的完整重建里才会暴露。

**(a) DPLANE 开关告警：T9 前 2 条，T9 后 0 条。** T9 前（`/tmp/verify-build-7ad4671558.log`）：

```
zebra/dplane_fpm_nl.c:978:9: warning: enumeration value 'DPLANE_OP_GRE_ADD' not handled in switch [-Wswitch]
zebra/dplane_fpm_nl.c:978:9: warning: enumeration value 'DPLANE_OP_GRE_DELETE' not handled in switch [-Wswitch]
```

T9 在 `zebra/dplane_fpm_nl.c` 第 1098-1100 行的 no-op 分组（`case DPLANE_OP_GRE_SET:`
旁）补上这两个 case 后，对最终日志
`grep warning: /tmp/verify-build-T10.log | grep dplane` **0 命中**。这与 `e7c78d33a7`
在 `zebra/zebra_dplane.c` / `zebra/zebra_rib.c` 修的是同一类缺陷，T9 补上了漏掉的
这一个文件。

**(b) 既有 bgpd midr 告警 11 条（T9 后剩余的全部告警）**（**pre-existing，明确 OUT OF SCOPE**）：

```
bgpd/bgp_midr_pm.c:156:31: warning: cast from function call of type 'double' to non-matching type 'unsigned int' [-Wbad-function-cast]
bgpd/bgp_midr_pm.c:171:33: warning: cast from function call of type 'double' to non-matching type 'unsigned int' [-Wbad-function-cast]
bgpd/midr_trace_scheduler.c:1682:9: warning: ... [-Wswitch-enum]   （8 条枚举值：MIDR_TRACE_OK / ERR_INVALID / ERR_NO_SNAPSHOT / ERR_UNSUPPORTED / ERR_QUEUE_FULL / ERR_REQUEST_LIMIT / ERR_CANCELED / ERR_SHUTDOWN）
bgpd/bgp_midr_nds.c:3375:13: warning: 'midr_bootstrap_seed_load_cb' defined but not used [-Wunused-function]
```

这 11 条都在 `bgpd/` 内、且都不在本分支的改动集里，属于既存问题。审查依据
`doc/midr-doc/第三组最新midrd分支审查反馈与同步建议-2026-09-20.md` 第 6 节
（「不在本次同步范围内的内容」）明确要求第三组**不**合并第一组的 group1 接线 /
peer discovery / session observer，因此本组**有意不**把这类 bgpd 改动拉进来，
它们**不计入本分支的修复验收**。

顶层 `Makefile.am` 第 138-166 行把 `midrd/midrd` 加入 `sbin_PROGRAMS`，并在
`midrd_midrd_SOURCES` 中列出 `midrd/midr-dp-backend.{c,h}`、
`midrd/midr-gre.{c,h}`（第 157-158 行）；`midrd_midrd_CPPFLAGS = -Imidrd`、
`midrd_midrd_LDADD = lib/libfrr.la $(LIBCAP)`。

## 4. midrd 组件套件

```sh
cd /home/frr/frr-midrd3/midrd
make -j112 BUILD_DIR=/tmp/midrd-build test
```

`midrd/Makefile`：
- `DP_OBJECTS = $(BUILD_DIR)/midr-dp-backend.o $(BUILD_DIR)/midr-gre.o`（第 36 行）；
- `all` 目标包含三个数据面 harness/test 目标：`$(BUILD_DIR)/midrd-dp-test`、
  `$(BUILD_DIR)/midrd-gre-tool`、`$(BUILD_DIR)/midrd-dp-e2e-tool`（第 55-58 行）；
- `test` 目标（第 285-310 行）顺序执行全部组件程序，最后运行
  `./extraction-boundary-test.sh`；
- `midrd` 二进制依赖 `$(CORE_OBJECTS) $(INSTALL_OBJECTS) $(DP_OBJECTS)`
  （第 268 行）；
- `FRR_CFLAGS` 对第三组翻译单元设 `-Wno-error`（第 8-14 行）：libfrr 公开头经
  `zclient.h → vrf.h → vty.h` 拉入匿名结构体成员写法，触发 GCC 默认开启且无
  `-W` 开关的警告；FRR 顶层构建仍是权威严格构建。
- 显式验收目标 `fib-smoke`、`integration-smoke`、`gre-smoke`（第 335/345/349 行，
  `.PHONY` 第 19-20 行）：这三个目标**不属于** `all`/`test`，需 root/网络命名空间/
  真实 zebra，分别包装 `r7-dp-fib-smoke.sh`（容器内 root）、
  `r7-dp-integration-smoke.sh`（docker 宿主 root + containerlab）、
  `midr-gre-connectivity-test.sh`（docker 宿主 root），并通过环境变量
  `DP_TOOL_BIN`/`MIDRD_BIN`/`MIDRD_GRE_TOOL` 传入构建产物路径，例如
  `make -C midrd BUILD_DIR=/tmp/midrd-build fib-smoke`。

| 项 | 值 |
| --- | --- |
| 命令 | `make -j112 BUILD_DIR=/tmp/midrd-build test` |
| 结果 | 25 个程序全部 PASS，含 `midrd-dp-test: PASS`、`standalone libfrr boundary scan: PASS`，EXIT=0 |
| 日志 | 容器 `/tmp/verify-comp.log`、`/tmp/final-comp.log` |
| 状态 | 已验证 |

单独运行数据面单测：

```sh
/tmp/midrd-build/midrd-dp-test      # 逐段执行后打印 midrd-dp-test: PASS
```

`dp-backend-test.c` 用 `--wrap=zclient_route_send` 捕获编码后的
`zapi_route`，覆盖：
- `test_encoding_modes()`：BASIC/ECMP/UCMP/SRv6、双 instance、metric-only
  的 DELETE+ADD、distance、`ZEBRA_FLAG_ALLOW_RECURSION`；
- `test_send_failure_and_abort()`：发送失败不更新 installed hash、abort 清空；
- `test_adapter_pipeline_and_recovery()`：READY 安装、同 generation resync
  no-op、zebra 重连 replay、deferred 失败两步恢复；
- `test_gre_api()`：GRE API 的校验路径（NULL/族不一致/无 zclient/无后端）。

### 4.1 第三组 ZAPI/FIB smoke（`midrd/r7-dp-fib-smoke.sh`）

```sh
# 容器 frr-ubuntu24-ymy 内，root
bash /home/frr/frr-midrd3/midrd/r7-dp-fib-smoke.sh
```

该脚本在构建容器内启动真实、新构建的 zebra 于私有 ZAPI socket，用
`midrd-dp-e2e-tool` 驱动，落到真实内核 FIB；**不使用 MIDR 会话/洪泛层**，因此
数据面可独立于第二组验收。

| 项 | 值 |
| --- | --- |
| 命令 | `bash /home/frr/frr-midrd3/midrd/r7-dp-fib-smoke.sh` |
| 结果 | `=== summary: PASS=15 FAIL=0 ===`，末行 `third-group ZAPI/FIB smoke: PASS`，EXIT=0 |
| 日志 | 容器 `/tmp/fib-smoke.log` |
| 状态 | 已验证 |

覆盖（详见 `dp-03-e2e-and-integration.md` 第 3.1 节）：直接 facade add/delete（递归
nexthop，proto 199 + `ip route get`）；完整 TED → SPF → adapter → backend → ZAPI →
zebra → FIB（含 zebra 重启 replay 与 SIGTERM 撤销）；ECMP/UCMP/IPv6 形状（IPv6 用
`ip -6 route show proto 199` 检查）；去重与替换。

> **FIB 断言须同时解析两种安装形式（group-aware）**：zebra 既可能内联安装
> （路由行含 `via N`），也可能经 nexthop group 安装（路由行含 `nhid N`）。断言
> 必须先取 `nhid`，再用 `ip nexthop show id <nhid>` 展开，并跟随 `group` 列表继续
> 展开成员 nexthop（`midrd/r7-dp-fib-smoke.sh` 第 80-101 行的 `fib_nhid()`/
> `fib_nh_list()`）。只匹配单一形式曾造成多次误报失败。

## 5. 两阶段边界扫描

`midrd/extraction-boundary-test.sh`（60 行）是 `midrd` 组件的独立边界门禁：

```text
阶段 1  源级扫描（第 8-17 行）
        grep -RInE '#include <bgpd|bgp_|zebra/>'  midrd/*.c midrd/*.h   -> 命中即 FAIL
        grep -RInE 'struct (bgp|peer|bgp_path_info)|AFI_BGP|SAFI_MIDR_LS|BGP_(OPEN|UPDATE|FSM|CAPABILITY)'
                                                                         -> 命中即 FAIL

阶段 2a contract.c（第 30-49 行）
        只 include 纯 MIDR 公开头，用严格 -Werror 编译

阶段 2b dp-contract.c（第 51-59 行）
        include midr-dp-backend.h / midr-gre.h / midr-spf-install.h / midr-zebra.h
        用放宽 flag 编译（见下）
```

`dp-contract.c` 放宽 `-Werror` 的原因（脚本第 21-24 行注释）：第三组头经
libfrr `vrf.h → vty.h` 链拉入匿名结构体成员写法，GCC 报 “declaration does
not declare anything”，该警告默认开启且没有 `-W` 开关，故无法在该翻译单元
保留 `-Werror`。**FRR 顶层构建仍是权威严格构建**，其权威性在于编译 FRR
全部翻译单元并暴露 `-Wswitch` 等真实告警，而**不是**「0 警告」；实测
从零重建（`make clean && make -j112`）最终为 11 行 `warning:`（全部为分支外
既有 bgpd 告警，OUT OF SCOPE），见第 3.1 节。

命令与预期末行：

```sh
cd /home/frr/frr-midrd3/midrd && ./extraction-boundary-test.sh
# 期望末行: standalone libfrr boundary scan: PASS
```

## 6. 容器内同步 / 构建流程

| 项 | 值 |
| --- | --- |
| 跳板 | `ssh -i ~/.ssh/frr yangmy@101.6.30.220 -p 50022` |
| 构建容器 | `frr-ubuntu24-ymy` |
| 容器内源码树 | `/home/frr/frr-midrd3` |
| 宿主副本 | `~/frr-midrd3` |
| 同步方式 | `rsync`（宿主副本）+ `docker cp`（进容器） |
| 容器内 uid | 宿主 uid 1010/92 映射进容器 |

典型流程（占位顶层路径 `~/frr-midrd3`，按实际检出调整）：

```sh
# 1) 宿主：把工作区同步到宿主副本
rsync -a --delete /Users/yangmengyu/githubdocuments/frr/midrd/ \
      ~/frr-midrd3/midrd/
rsync -a /Users/yangmengyu/githubdocuments/frr/Makefile.am ~/frr-midrd3/Makefile.am

# 2) 宿主 -> 容器：把源码拷入构建容器
docker cp ~/frr-midrd3/midrd/. frr-ubuntu24-ymy:/home/frr/frr-midrd3/midrd/

# 3) 容器内：顶层构建 + 组件套件
docker exec -u root frr-ubuntu24-ymy bash -lc '
  cd /home/frr/frr-midrd3 && make -j112
  cd /home/frr/frr-midrd3/midrd && make -j112 BUILD_DIR=/tmp/midrd-build test'
```

> 上表为已确认的环境参数；第 3 节顶层构建、第 4 节组件套件、以及
> `dp-03`/`dp-04` 的 GRE 测试均已在本环境实际执行（GRE 测试用
> `docker cp` 把新构建的 zebra、libfrr 与 harness staged 到 node1/node2 的
> `/opt/midr-dp`，保留旧 `/opt/midr` 资产不动）。

## 7. 日志位置

| 日志 | 内容 | 位置 |
| --- | --- | --- |
| `/tmp/verify-clean-T10.log`、`/tmp/verify-build-T10.log` | FRR 顶层从零重建（`make clean && make -j112`）最终输出，含 11 行 `warning:`（T9 前 13 行，见 `/tmp/verify-build-7ad4671558.log`） | 容器 `frr-ubuntu24-ymy` |
| `/tmp/verify-comp.log`、`/tmp/final-comp.log` | `midrd` 组件套件输出 | 容器 `frr-ubuntu24-ymy` |
| `/tmp/midrd-build/` | 独立组件构建目录 | 容器 `frr-ubuntu24-ymy` |
| `/tmp/fib-smoke.log` | 第三组 ZAPI/FIB smoke | 容器 `frr-ubuntu24-ymy` |
| `/tmp/fib-final.log`、`/tmp/midrd-dp-zapi-fib/run/` | group-3 隔离 ZAPI/FIB | 容器 `frr-ubuntu24-ymy` |
| `/tmp/verify-integration.log` | 三节点 containerlab 联合测试 | docker 宿主 |
| `/tmp/gre-test.log` | 双容器 GRE 连通性 | docker 宿主 |

## 8. 证据

| 验证 | 结果 | 证据 |
| --- | --- | --- |
| FRR 顶层构建（从零重建） | EXIT=0；**11 行 `warning:`（非 0）**，全部为既有 bgpd（OUT OF SCOPE）；2 条 DPLANE_OP_GRE_* 已由 T9 消除（T9 前 13） | `/tmp/verify-clean-T10.log`、`/tmp/verify-build-T10.log`（容器 `frr-ubuntu24-ymy`） |
| midrd 组件套件 | 25 程序 PASS，含 `midrd-dp-test: PASS`、`standalone libfrr boundary scan: PASS`，EXIT=0 | `/tmp/verify-comp.log`、`/tmp/final-comp.log`（容器 `frr-ubuntu24-ymy`） |
| 第三组 ZAPI/FIB smoke | `PASS=15 FAIL=0`，`third-group ZAPI/FIB smoke: PASS`，EXIT=0 | `midrd/r7-dp-fib-smoke.sh`；`/tmp/fib-smoke.log`（容器 `frr-ubuntu24-ymy`） |
| FIB 断言（group-aware） | 同时解析内联 `via N` 与 `nhid N` | `midrd/r7-dp-fib-smoke.sh` 第 80-101 行 `fib_nhid()`/`fib_nh_list()` |
| deferred-batch 恢复 | 缺陷已修复；先重试保留批再 resync | `midrd/dp-backend-test.c` `test_adapter_pipeline_and_recovery()` |
| 单测覆盖 | 编码模式、失败 abort、adapter 恢复、GRE 校验路径 | `midrd/dp-backend-test.c` |
| 边界门禁 | 两阶段编译 + 源扫描 | `midrd/extraction-boundary-test.sh` |
