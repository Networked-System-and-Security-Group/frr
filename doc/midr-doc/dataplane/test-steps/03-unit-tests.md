# 测试 03：单元测试

## 测试目的

验证 `bgp_midr_zebra.c` 内部逻辑正确性，使用 mock `zclient_route_send` 绕开真实 zebra 连接，测试所有 DP API 函数。

## 测试代码文件

| 文件 | 说明 |
|------|------|
| `tests/bgpd/test_midr_zebra.c` | 单元测试（10 项测试，46 断言） |

## 测试内容（10 项）

| # | 测试函数 | 断言数 | 验证内容 |
|---|---------|:------:|---------|
| 1 | `test_lifecycle` | 7 | init → fini → double-fini 安全 |
| 2 | `test_add_flush_diff` | 7 | 入队 → flush → installed hash 变更 |
| 3 | `test_add_then_delete` | 4 | ADD+DEL 同批次不安装 |
| 4 | `test_deferred` | 7 | 定时器合并与取消 |
| 5 | `test_srv6_path` | 2 | TE 实例 SRv6 路径 |
| 6 | `test_multiple_prefixes` | 7 | 多前缀独立管理与选择性删除 |
| 7 | `test_ucmp` | 2 | 混合权重 |
| 8 | `test_empty_ops` | 2 | 空队列安全 |
| 9 | `test_struct_layout` | 5 | sizeof / 零值语义 / instance 默认值 |
| 10 | `test_dual_instance` | 4 | SPF+TE 同前缀共存 |

## 前置条件

- 已进入 Docker 容器: `sudo docker exec -it frr-ubuntu24-ymy bash`
- FRR 构建系统已完成 `./configure && make`
- bgpd 已编译: `make bgpd/bgpd -j$(nproc)`
- 容器内有 `gcc`、`libyang-dev`、`libsqlite3-dev`

## 执行环境

```bash
# 从本地 mac SSH 到服务器
ssh -i ~/.ssh/frr yangmy@101.6.30.220 -p 50022

# 进入容器
sudo docker exec -it frr-ubuntu24-ymy bash
echo "thu325325" | sudo -S bash
cd /home/frr/frr
```

## 编译命令（共享库链接）

```bash
cd /home/frr/frr

gcc -std=gnu11 -Wall -Wextra -g -O0 -include config.h \
  -I lib -I bgpd -I . \
  $(pkg-config --cflags libyang 2>/dev/null) \
  -o /tmp/test_midr_zebra tests/bgpd/test_midr_zebra.c bgpd/bgp_midr_zebra.o \
  -Wl,--wrap=zclient_route_send \
  -L lib/.libs -lfrr \
  -lcap -lcrypt -ljson-c -lrt -lpthread -lsqlite3 -lresolv -ldl -lm -lfl -lyang
```

> **关键**: 使用共享库 `-L lib/.libs -lfrr` 链接，而不是 `-Wl,--whole-archive bgpd/libbgp.a`。后者会拉入 rfapi/skiplist/ringbuf 等无关模块导致链接失败。

## 执行命令

```bash
/tmp/test_midr_zebra
```

## 实际测试输出（2026-07-08 v4.0）

```
=== MIDR Data-Plane Unit Tests ===

[Test 1] init/fini lifecycle
  OK: fini on NULL dp safe
  OK: midr_dp allocated
  OK: pending_ops list created
  OK: installed hash created
  OK: pending_ops empty initially
  OK: flush_pending=false initially
  OK: midr_dp NULL after fini

[Test 2] add/flush/diff
  OK: 1 pending after add
  OK: nil args ignored
  OK: queue empty after flush
  OK: installed hash has prefix
  OK: reinstall preserves entry
  OK: entry still exists after change
  OK: gone after delete

[Test 3] add-then-delete
  OK: ADD+DEL queued
  OK: queue empty after flush
  OK: not installed after ADD->DEL
  OK: delete non-existent safe

[Test 4] deferred timer coalescing
  OK: flush_pending=true after deferred
  OK: deferred timer armed
  OK: coalesced (still true)
  OK: flush_pending reset
  OK: timer cancelled by flush
  OK: queue drained
  OK: empty flush safe

[Test 5] SRv6 path
  OK: SRv6 queued
  OK: SRv6 installed under TE instance

[Test 6] multiple prefixes
  OK: 3 pending ops
  OK: p1 / p2 / p3
  OK: p1 still / p2 gone / p3 still

[Test 7] UCMP weights
  OK: UCMP queued / installed

[Test 8] empty operations
  OK: flush on empty safe / deferred on empty safe

[Test 9] struct layout
  OK: midr_path sizeof > 0
  OK: midr_path_result has room for SID list
  OK: zero-inited sid_count=0 (pure IP)
  OK: zero-inited path_count=0
  OK: zero-inited instance=0 (SPF default)

[Test 10] dual-instance SPF+TE coexist
  OK: SPF installed at instance=0
  OK: TE absent before add
  OK: SPF still installed alongside TE
  OK: TE installed at instance=1

=== 0 test(s) FAILED ===
```

## 结论

全部 10 项/46 断言通过。MIDR DP 模块内部逻辑（init/fini 生命周期、路由队列管理、add/flush/diff 操作、定时器合并、SRv6 路径、多前缀管理、UCMP 权重、空操作安全、结构体布局、Dual-Instance 共存）均验证正确。

**编译策略**: 共享库链接 `bgp_midr_zebra.o + libfrr.so` 成功绕过 `--whole-archive` 引入的 rfapi/skiplist 依赖。

## 自动化脚本

```bash
scp -i ~/.ssh/frr -P 50022 tests/bgpd/scripts/test03_unit_tests.sh yangmy@101.6.30.220:/tmp/
ssh -i ~/.ssh/frr yangmy@101.6.30.220 -p 50022 "bash /tmp/test03_unit_tests.sh"
```

脚本路径: `tests/bgpd/scripts/test03_unit_tests.sh`
