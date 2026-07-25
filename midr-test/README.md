# MIDR 接口骨架测试

本目录用于测试 MIDR 第一组输入接口、Local Fact 版本处理和基础 session wrapper。测试以 terminal VTY 模式启动 `bgpd`，不连接 zebra，也不监听真实 BGP 端口。

## 目录结构

```text
midr-test/
  bgpd.conf              # 测试用 bgpd 配置
  run.sh                 # 场景运行与断言脚本
  expect/                # 固定字符串预期结果
  vty/                   # 场景输入的 VTY 命令
  run/                   # pid、socket 和日志，不纳入 Git
```

`expect/*.expect` 中的普通非空行表示日志必须包含该固定字符串；以 `!` 开头的行表示日志不得包含其后的固定字符串；以 `#` 开头的行是注释。

## 编译

```bash
cd ~/yhy/frr
make -j$(nproc) bgpd/bgpd tests/bgpd/test_midr_input
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
make -j$(nproc) bgpd/bgpd tests/bgpd/test_midr_input
```

## 运行

运行 C 输入校验单元测试：

```bash
./tests/bgpd/test_midr_input
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
show midr events
exit
```

`show midr topology nodes/links` 只显示 active Local Fact，`show midr topology tombstones` 只显示已撤销对象的 key 和最后 input version。`show midr events` 中 `enqueued` 表示成功入队事件数，`processed` 表示已处理事件数，`ignored-old` 表示被版本规则忽略的事件数。
