# MIDR 输入、TED 与协议对象测试

本目录用于测试 MIDR 第一组输入接口、Local Fact、Snapshot/Resync、基础 session wrapper、不可变 TED snapshot、LS Object、sequence、cost、wire codec、MIDR SAFI RIB、owned object、LSDB、生产 TED，以及多节点传播、Prefix Reachability、scope、withdraw、代表接管、EoR 和 Route Refresh。单节点 VTY 测试以 terminal 模式启动 `bgpd`，不连接 zebra，也不监听真实 BGP 端口；多节点测试使用 rootless user/network namespace 建立真实 BGP session。测试 Provider、TED fixture 和 fuzz corpus均不进入生产 `bgpd` 数据路径。

## 目录结构

```text
midr-test/
  bgpd.conf              # 测试用 bgpd 配置
  run.sh                 # 场景运行与断言脚本
  run-resync-scenarios.sh
                          # M2 Gherkin/组件测试映射入口
  run-ted-fixtures.sh    # TED Gherkin/fixture 断言入口
  run-codec-fuzz.sh      # M3 codec libFuzzer 入口
  run-m3-mutation.sh     # M3 关键规则定向变异测试
  run-m3-coverage.sh     # M3 核心文件覆盖率门禁
  run-m4-scenarios.sh    # M4 RIB/LSDB Gherkin 组件测试
  run-m5-multinode.sh    # M5 rootless 多节点传播测试
  run-m5-scenarios.py    # M5 Gherkin ID 与自动断言映射
  run-spf-e2e-multinode.sh
                          # privileged 多节点 TED→SPF→Zebra→FIB 测试
  run-m6-m7-scenarios.py # M6/M7 Gherkin ID 与自动断言映射
  check-command-reference.py
                          # 配置手册与实际 VTY 语法一致性检查
  ted-fixture-normalize.py
                          # 严格 YAML schema 到规范化 JSON
  expect/                # 固定字符串预期结果
  vty/                   # 场景输入的 VTY 命令
  features/              # M1/M2/M4 Gherkin 场景及 ID 映射
  fuzz/corpus/           # M3 golden 和 malformed 十六进制种子
  path-fixtures/         # 有效、非法及 expected TED fixture
  run/                   # pid、socket 和日志，不纳入 Git
```

`expect/*.expect` 中的普通非空行表示日志必须包含该固定字符串；以 `!` 开头的行表示日志不得包含其后的固定字符串；以 `#` 开头的行是注释。

## 编译

```bash
cd ~/yhy/frr
make -j$(nproc) \
  bgpd/bgpd \
  tests/bgpd/test_midr_input \
  tests/bgpd/test_midr_resync \
  tests/bgpd/test_midr_ted \
  tests/bgpd/test_midr_ted_fixture \
  tests/bgpd/test_midr_ls_object \
  tests/bgpd/test_midr_sequence \
  tests/bgpd/test_midr_cost \
  tests/bgpd/test_midr_codec \
  tests/bgpd/test_midr_safi \
  tests/bgpd/test_midr_attr \
  tests/bgpd/test_midr_rib \
  tests/bgpd/test_midr_owned \
  tests/bgpd/test_midr_lsdb \
  tests/bgpd/test_midr_prefix \
  tests/bgpd/test_midr_packet \
  tests/bgpd/test_midr_scope \
  tests/bgpd/test_midr_sync
```

干净环境或构建清单发生变化时，先执行：

```bash
./bootstrap.sh
./configure --prefix=/usr \
  --sysconfdir=/etc \
  --localstatedir=/var \
  --sbindir=/usr/lib/frr \
  --enable-vtysh \
  --enable-multipath=256 \
  --enable-user=frr \
  --enable-group=frr \
  --enable-vty-group=frrvty \
  --with-pkg-git-version
make -j$(nproc) \
  bgpd/bgpd \
  tests/bgpd/test_midr_input \
  tests/bgpd/test_midr_resync \
  tests/bgpd/test_midr_ted \
  tests/bgpd/test_midr_ted_fixture \
  tests/bgpd/test_midr_ls_object \
  tests/bgpd/test_midr_sequence \
  tests/bgpd/test_midr_cost \
  tests/bgpd/test_midr_codec \
  tests/bgpd/test_midr_safi \
  tests/bgpd/test_midr_attr \
  tests/bgpd/test_midr_rib \
  tests/bgpd/test_midr_owned \
  tests/bgpd/test_midr_lsdb \
  tests/bgpd/test_midr_prefix \
  tests/bgpd/test_midr_packet \
  tests/bgpd/test_midr_scope \
  tests/bgpd/test_midr_sync
```

## 运行

运行 C 输入校验单元测试：

```bash
./tests/bgpd/test_midr_input
```

运行 Local Fact、Snapshot/Resync 和 Router-ID 生命周期组件测试：

```bash
./tests/bgpd/test_midr_resync
./midr-test/run-resync-scenarios.sh all
```

单个 M2 Gherkin 场景可按 ID 运行：

```bash
./midr-test/run-resync-scenarios.sh M2-INPUT-004
```

运行不可变 snapshot、generation、consumer 和 builder 单元测试：

```bash
./tests/bgpd/test_midr_ted
```

运行 M3 协议对象核心单元测试：

```bash
./tests/bgpd/test_midr_ls_object
./tests/bgpd/test_midr_sequence
./tests/bgpd/test_midr_cost
./tests/bgpd/test_midr_codec
```

运行 M3 codec fuzz、定向变异和覆盖率门禁：

```bash
MIDR_FUZZ_TIME=60 ./midr-test/run-codec-fuzz.sh
./midr-test/run-m3-mutation.sh
./midr-test/run-m3-coverage.sh
```

Fuzzer 使用 Clang 的 ASAN/UBSAN 和 LeakSanitizer；变异测试要求 10 个定向 mutant 全部被现有单元测试杀死；覆盖率只统计 `bgp_midr_ls.c`、`bgp_midr_sequence.c`、`bgp_midr_cost.c` 和 `bgp_midr_codec.c`，门禁为 line 90%、branch 80%。覆盖率脚本需要 `gcovr`。

运行 M4 SAFI、Attribute、RIB、owned object、LSDB 与生产 TED 测试：

```bash
./tests/bgpd/test_midr_safi
./tests/bgpd/test_midr_attr
./tests/bgpd/test_midr_rib
./tests/bgpd/test_midr_owned
./tests/bgpd/test_midr_lsdb
./midr-test/run-m4-scenarios.sh all
```

M4 的 selection、LSDB 事务和 owned-object 生命周期属于核心逻辑。阶段结束时先验证需求场景、故障回滚、定向 mutation 和 Sanitizer，再进行一次覆盖缺口分析。核心新增代码聚合覆盖率硬门禁为 line 85%、branch 70%，单个核心文件最低为 line 75%、branch 60%；line 90%、branch 80% 保留为目标值，不为纯防御性短路、不可注入的 FRR glue 或重复语义分支反复补充低价值测试。未达到目标值但达到硬门禁时，必须同时满足关键需求场景均有自动断言、关键算法 mutation 或等价故障注入通过、Sanitizer 无报告且审查无未解决 P0/P1。

运行 M5 wire、scope 和 EoR 组件测试：

```bash
./tests/bgpd/test_midr_packet
./tests/bgpd/test_midr_scope
./tests/bgpd/test_midr_sync
```

运行全部 M5 rootless 多节点传播场景：

```bash
./midr-test/run-m5-multinode.sh all
python3 ./midr-test/run-m5-scenarios.py
```

固定场景覆盖三节点线形传播、逐跳 `MP_UNREACH`、intra-group/global scope、三角拓扑 alternate path 与防环、EoR timeout/迟到恢复，以及 Route Refresh。M5 的阶段门禁以这些协议行为、完整 M0-M5 回归、Clang、ASAN/UBSAN/LeakSanitizer 和 codec fuzz 为主；覆盖率只用于定位明显缺失的关键分支，不设置为了达到单一百分比而反复补测的关闭门槛。

运行 SPF 端到端测试：

```bash
make -j$(nproc) bgpd/bgpd zebra/zebra
sudo ./midr-test/run-spf-e2e-multinode.sh line
sudo ./midr-test/run-spf-e2e-multinode.sh complex
sudo ./midr-test/run-spf-e2e-multinode.sh cross-group
```

该测试需要 Ubuntu 24.04 privileged 容器或等价的 root Linux 环境，并要求安装
`iproute2` 和 `iputils-ping`。每个节点使用独立 network namespace，并运行一套真实
`bgpd + zebra`。`line` 场景验证 r1→r2→r3 两跳路径；`complex` 场景构造
r1→{r2,r3}→r4→r5 五节点等价双路径，验证两个 `proto 199` ECMP first-hop、两条
分支分别故障时的无损收敛以及恢复后的 ECMP 重建。`cross-group` 使用相同物理拓扑，
但将 r1/r2/r3、r4、r5 分别放入 Group 10、20、30，验证 `egress_links`、
`group_edges`、`prefix_groups`、Group SPF 和本群物理出口选择。三个场景都从拓扑和
Prefix 输入开始，贯穿 MIDR BGP 传播、LSDB/TED、SPF、第三组 ZAPI、Zebra、Linux
FIB 和真实报文转发，并验证 Prefix 撤销/恢复。运行日志保存在
`midr-test/run/spf-e2e/<scenario>/`。

运行 M6 Prefix Contributor、Node/Group Prefix、代表接管，以及 M7 TED parity/Consumer 场景：

```bash
./tests/bgpd/test_midr_prefix
python3 ./midr-test/run-m6-m7-scenarios.py
python3 ./midr-test/check-command-reference.py
```

单个 Gherkin 场景可按 ID 运行：

```bash
python3 ./midr-test/run-m6-m7-scenarios.py M6-PFX-004
python3 ./midr-test/run-m6-m7-scenarios.py M7-TED-002
```

M6/M7 的 Gherkin 只映射跨模块和外部可观察行为。PFX-01 的字段级 eligibility、sequence 和异常输入由 `test_midr_prefix` 直接断言；真实 Node Prefix、Group Prefix、逐跳撤销和代表接管由 rootless 多节点场景断言；Real/Mock parity 和 public Consumer 生命周期由 `test_midr_lsdb` 断言。

运行全部 M1 TED YAML/Gherkin 场景：

```bash
./midr-test/run-ted-fixtures.sh all
```

运行单个 M1 场景：

```bash
./midr-test/run-ted-fixtures.sh M1-TED-004
```

运行全部 VTY 场景：

```bash
./midr-test/run.sh all
```

运行单个场景或列出场景：

```bash
./midr-test/run.sh smoke
./midr-test/run.sh list
```

固定场景包括：

```text
smoke          IPv4 Node 和完整 Link upsert
ipv6           IPv6 transport 和 Link endpoint
version        旧 version 过滤
node-withdraw  Node active 状态与 tombstone 分离
link-withdraw  未知 Link withdraw 后旧 upsert 不复活
ownership      本地 Router-ID ownership 校验
invalid-link   地址族、measurement 和 uint64 输入校验
peer-session   基础 session request/release wrapper
ted-not-ready  M1 生产 TED 保持 NOT_READY / generation 0
sync-status    输入状态、队列上限和 Provider 状态
eor-config     EoR timeout 配置、持久化输出和默认值恢复
router-id-restart
               Router-ID 变化时清理旧 identity，并将新输入置于 Resync Barrier 后
prefix-config  Prefix policy、external source 和代表接管延迟配置
```

每个场景的完整输出保存在 `midr-test/run/<scenario>.log`。缺少必要输出、出现禁止输出、命令无法解析、进程崩溃或超时都会使脚本返回非零。普通用户运行时出现 `/var/lib/frr` 或 `/var/run/frr` permission warning 不作为失败。

## 手工调试

```bash
./midr-test/run.sh interactive
```

交互模式下可直接输入：

```text
enable
midr topology node upsert 1.1.1.1 group 100 transport 2001:db8::1 version 1
midr topology link upsert 1.1.1.1 2.2.2.2 id 10 local-address 2001:db8::1 remote-address 2001:db8::2 rtt-us 1200 loss-ppm 10 available-bandwidth-kbps 900 version 1 ifindex 0 seqno 1 timestamp-ms 1000
show midr topology nodes
show midr topology links
show midr topology tombstones
show midr topology sync
show midr events
show midr rib summary
show midr owned
show midr lsdb summary
show midr ted summary
exit
```

`show midr topology nodes/links` 只显示 active Local Fact，`show midr topology tombstones` 只显示已撤销对象的 key 和最后 input version。`show midr topology sync` 显示输入状态、Provider、队列和 Resync 诊断；`show midr events` 显示事件接收、处理、拒绝和丢弃计数。

## 第一组与第二组 10 节点联合测试

10 节点 CL 测试按 bootstrap、群代表、现有成员和 newnode 的顺序启动，验证 newnode 的 JOIN 决策和跨群 Anchor Session 建立：

```bash
cd midr-test/cl-test
sudo ./run_test.sh
```

测试会在 `midr-test/cl-test/artifacts/<run-id>/` 保存各阶段的只读状态，并将完整终端输出同步写入 `console.log`。JOIN 和 ANCHOR 决策后，脚本还会等待 4 条 Anchor 会话全部 Established，并等待 newnode Membership/LSDB/TED 与 g1b 所见 newnode Membership 连续两次满足稳定条件；成功时生成 `after-anchor-sessions/` 和 `post-convergence/`，超时则生成对应的 `*-timeout/` 诊断快照。每个节点的文件包含第一组视图、第二组 Local Fact、owned object、全部 MIDR RIB path、LSDB object 和完整 TED snapshot；`events/` 保存第一组上报及第二组传播相关日志摘录，`logs/` 保存最终完整 bgpd 日志。状态采集与第二组稳定条件不改变原 JOIN/ANCHOR 判定，单条诊断命令失败会记录退出码但不会改变原测试结果。

台子使用 `bgpd -Z`、不连接 zebra，因此 `show midr neighbors` 在会话尚未 Established 时可能显示 `（no route）`：该标记只检查 BGP NHT/RIB，在此环境中不能反映 namespace 的内核默认路由。底层可达性应结合 PM reply 和最终会话状态判断。

测试仍在运行时可手工追加一次快照：

```bash
sudo MIDR_CAPTURE_RUN_ID=<run-id> ./capture_state.sh manual
```

新增详细命令为：

```text
show midr rib paths
show midr lsdb objects
show midr ted detail
```

## 第一组与第二组只读验收

从当前合栈工作树构建镜像并部署独立的 15 节点台子：

```bash
sudo -E ./midr-test/backbone-lab/run_group1_group2_lab.sh all
```

脚本默认使用实验名 `midr-backbone-yhy`，运行文件保存在仓库同级的 `midr-lab-runs/midr-backbone-yhy/`，不会自动销毁或替换任何已有容器。发现同名实验时会直接退出。`preflight` 只检查环境，`check` 复跑已经部署台子的验收并保存第二组证据，`capture` 不等待收敛而立即采集当前状态；可通过 `MIDR_LAB_NAME`、`MIDR_LAB_IMAGE` 和 `MIDR_LAB_RUN_ROOT` 使用其他独立名称和目录。不要执行 `backbone-lab/gen-backbone-configs.py`，该生成器仍保留旧工作目录，仅应维护并使用仓库内已经审查的配置和拓扑。

15 节点 `midr-backbone` containerlab 已按阶段启动并收敛后运行：

```bash
./midr-test/backbone-group2/run_group1_group2_demo_check.sh
```

脚本只读取容器和 VTY 状态，不执行退网、重启、iptables 或配置修改。默认最多等待 600 秒，验证第一组 Node/Link 上报、强 Snapshot Provider、第二组输入队列、owned objects、MIDR RIB、LSDB、TED、group 0 引导静默和 remote view 对账。可通过 `MIDR_DEMO_WAIT_SECONDS`、`MIDR_DEMO_POLL_SECONDS` 和 `MIDR_LAB_PREFIX` 调整等待时间、轮询间隔和容器名前缀。

推荐通过统一入口复跑验收和证据采集：

```bash
./midr-test/backbone-lab/run_group1_group2_lab.sh check
```

证据保存在 `midr-lab-runs/<lab-name>/evidence/<timestamp>/post-convergence/`。`summary.md` 提供适合审查和录屏的紧凑表格，`summary.tsv` 保存同一份机器可读计数，`semantic-digests.tsv` 记录规范化对象集合，`verification.log` 保存跨层自动断言；`nodes/<node>/` 分别保存第一组接口、Local Fact、owned object、全部 RIB path、LSDB object、TED detail、完整 FRR 日志和 MIDR 日志摘录。验证包括 Provider/Local Fact/owned 数量一致，RIB selected 与 LSDB object 一致，全节点 Membership 和 Prefix-to-Group 收敛一致，以及 LSDB Membership、Link、Node Prefix、Group Prefix 与 TED 六类数组的逐项对应。验证失败时仍会保留同一时刻的全部采集结果。
