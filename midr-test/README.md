# MIDR 输入、TED 与协议对象测试

本目录用于测试 MIDR 第一组输入接口、Local Fact、Snapshot/Resync、基础 session wrapper、不可变 TED snapshot，以及 LS Object、sequence、cost 和 wire codec。VTY 测试以 terminal 模式启动 `bgpd`，不连接 zebra，也不监听真实 BGP 端口；测试 Provider、TED fixture 和 fuzz corpus 均不进入生产 `bgpd` 数据路径。

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
  ted-fixture-normalize.py
                          # 严格 YAML schema 到规范化 JSON
  expect/                # 固定字符串预期结果
  vty/                   # 场景输入的 VTY 命令
  features/              # M1/M2 Gherkin 场景及 ID 映射
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
  tests/bgpd/test_midr_codec
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
  tests/bgpd/test_midr_codec
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
router-id-restart
               Router-ID 变化时清理旧 identity，并将新输入置于 Resync Barrier 后
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
exit
```

`show midr topology nodes/links` 只显示 active Local Fact，`show midr topology tombstones` 只显示已撤销对象的 key 和最后 input version。`show midr topology sync` 显示输入状态、Provider、队列和 Resync 诊断；`show midr events` 显示事件接收、处理、拒绝和丢弃计数。
