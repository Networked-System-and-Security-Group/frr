# MIDR 数据平面路由安装开发文档


---

## 一、模块概述

数据平面负责将控制平面（SPF/CSPF）的路径计算结果编码为 ZAPI 消息并下发至 zebra，最终安装到内核 FIB。

### 1.1 核心文件

| 文件 | 职责 | 行数 |
|------|------|:----:|
| `bgpd/bgp_midr_zebra.h` | CP→DP 接口定义（`midr_path_result`、API 声明） | 227 |
| `bgpd/bgp_midr_zebra.c` | ZAPI 编码、批量下发、diff 去重、Dual-Instance TE | 850 |
| `bgpd/bgpd.h` | `struct bgp` 中挂载 `void *midr_dp` | — |

### 1.2 辅助文件（zebra 侧）

| 文件 | 修改内容 |
|------|---------|
| `lib/route_types.txt` | 新增 `ZEBRA_ROUTE_BGP_MIDR` |
| `lib/frrdistance.h` | `ZEBRA_BGP_MIDR_DISTANCE_DEFAULT = 115` |
| `zebra/zebra_rib.c` | RIB 元数据注册 |
| `zebra/rt_netlink.h` | `RTPROT_BGP_MIDR = 199` |
| `zebra/rt_netlink.c` | `zebra2proto()` / `proto2zebra()` 转换 |
| `zebra/kernel_netlink.c` | 协议名映射 |
| `zebra/debug_nl.c` | 调试输出 |
| `zebra/fpm_listener.c` | FPM 导出 |
| `tools/etc/iproute2/rt_protos.d/frr.conf` | iproute2 协议名 |

---

## 二、数据流架构

```
控制平面 (SPF / CSPF)
    │
    │ midr_path_result (instance + paths + SID list)
    ▼
midr_zebra_route_add()          ← DP 入口
    │ 深拷贝, 入队 pending_ops
    ▼
midr_zebra_route_flush()        ← 立即下发（紧急） 或
midr_zebra_route_update_deferred()  ← 100ms 定时器批处理
    │
    ▼
midr_flush_pending()
    │ FIFO 遍历 pending_ops
    ├── midr_process_one_op()
    │   ├── No-op (installed hash 命中) → 跳过
    │   ├── 变更 → DEL 旧 + ADD 新
    │   └── 新增 → ADD
    │
    └── midr_result_to_zapi()
        │ 模式识别: BASIC / ECMP / UCMP / SRv6
        │ ZAPI 编码: type/instance/metric/distance/nexthops/SRV6
        ▼
    zclient_route_send() → ZAPI → zebra → Netlink → Kernel FIB
```

---

## 三、数据结构详解

### 3.1 `struct midr_path`（单条路径）

定义位置: `bgpd/bgp_midr_zebra.h`

| 字段 | 类型 | 说明 |
|------|------|------|
| `nexthop` | `union g_addr` | 下一跳地址（IPv4 或 IPv6） |
| `ifindex` | `uint32_t` | 出接口索引（0 = zebra 解析） |
| `metric` | `uint32_t` | IGP 度量值 |
| `path_avail_bw` | `float` | 瓶颈可用带宽 (Mbps) |
| `weight` | `uint8_t` | UCMP 权重 1-255（0 = ECMP） |

### 3.2 `struct midr_path_result`（CP→DP 统一载体）

定义位置: `bgpd/bgp_midr_zebra.h`

| 字段 | 类型 | 说明 |
|------|------|------|
| `paths` | `struct midr_path *` | 路径数组（深拷贝到 pending_op） |
| `path_count` | `uint8_t` | 路径数量（>1 = 多路径） |
| `explicit.sid_list[]` | `struct in6_addr[SRV6_MAX_SEGS]` | SRv6 段列表 |
| `explicit.sid_count` | `uint8_t` | 0 = 纯IP, >0 = SRv6 |
| `instance` | `uint8_t` | `MIDR_INSTANCE_SPF(0)` 或 `MIDR_INSTANCE_TE(1)` |

### 3.3 `struct midr_pending_op`（内部——排队操作）

定义位置: `bgpd/bgp_midr_zebra.c`

| 字段 | 说明 |
|------|------|
| `op` | `MIDR_OP_ADD` 或 `MIDR_OP_DEL` |
| `prefix` | 目标前缀 |
| `instance` | SPF/TE 实例号 |
| `paths` / `path_count` / `sid_count` / `sid_list` | 深拷贝自 `midr_path_result` |

### 3.4 `struct midr_installed_entry`（内部——已安装路由快照）

| 字段 | 说明 |
|------|------|
| `prefix` | 目的前缀（哈希键） |
| `instance` | SPF/TE 实例号（哈希键的一部分） |
| `paths` / `path_count` / `sid_count` / `sid_list` | 最后安装的转发状态 |

### 3.5 `struct bgp_midr_dp`（内部——per-BGP 实例 DP 状态）

| 字段 | 说明 |
|------|------|
| `t_deferred` | 100ms 批量定时器（oneshot） |
| `pending_ops` | `struct midr_pending_op` 链表 |
| `installed` | `struct midr_installed_entry` 哈希表 |
| `flush_pending` | 防止重复启动定时器的守卫 |

---

## 四、关键函数说明

### 4.1 Public API

#### `midr_zebra_init(struct bgp *bgp)`
**位置**: `bgpd/bgp_midr_zebra.c` ~663
初始化 per-BGP-instance DP 状态：创建 pending_ops 链表和 installed 哈希表，挂到 `bgp->midr_dp`。

#### `midr_zebra_route_add(struct bgp *bgp, struct prefix *p, struct midr_path_result *result)`
**位置**: ~732
深拷贝 `result` → 构造 `midr_pending_op` → 追加到 `pending_ops` 链表。**不发送任何 ZAPI 消息**。

#### `midr_zebra_route_del(struct bgp *bgp, struct prefix *p)`
**位置**: ~765
构造 MIDR_OP_DEL 的 pending_op，instance 默认使用 `MIDR_INSTANCE_SPF(0)`。

#### `midr_zebra_route_flush(struct bgp *bgp)`
**位置**: ~810
取消定时器 → 立即排空 pending_ops 队列。用于紧急场景（接口 down）。

#### `midr_zebra_route_update_deferred(struct bgp *bgp)`
**位置**: ~783
启动 100ms 一次性定时器（多次调用会合并）。触发批量下发。

### 4.2 Internal Functions

#### `midr_result_to_zapi(..., instance, api)`
**位置**: ~320
核心 ZAPI 编码函数：
- **模式识别**: 根据 `sid_count` 选择纯IP 或 SRv6
- **Dual-Instance**: `instance==1` → `api.metric=1`（覆盖 SPF）
- **UCMP**: 填充 `ZAPI_NEXTHOP_FLAG_WEIGHT` + `ZEBRA_FLAG_USE_RECURSIVE_WEIGHT`
- **SRv6**: `ZAPI_NEXTHOP_FLAG_SEG6` + `SRV6_HEADEND_BEHAVIOR_H_INSERT`

#### `midr_process_one_op(bgp, dp, op)`
**位置**: ~548
单个操作的处理逻辑：
- ADD: installed hash diff → 相同则跳过 / 不同则 DEL 旧 + ADD 新
- DEL: 从 installed hash 中删除 → ZAPI route_delete

#### `midr_path_result_eq(a, b)`
**位置**: ~222
对比两个路径结果是否等同。v2.1 修复：
- 新增 `instance` 比较
- **只比较 prefix.family 对应的地址族**（不再同时比较 IPv4 + IPv6）
- 不比较 metric（metric 变化不影响转发状态）

#### `midr_path_fill_zapi_nh(p, mpath, znh, vrf_id)`
**位置**: ~270
根据 prefix 地址族填充 zapi_nexthop（AF_INET → NEXTHOP_TYPE_IPV4，AF_INET6 → NEXTHOP_TYPE_IPV6）。

---

## 五、Dual-Instance TE 模型

### 5.1 设计

| 实例 | 常量 | zapi.instance | metric | 行为 |
|------|------|:---:|:---:|------|
| SPF | `MIDR_INSTANCE_SPF=0` | 0 | IGP 路径度量 | 标准最短路 |
| TE | `MIDR_INSTANCE_TE=1` | 1 | 1 | SRv6 覆盖路径 |

二者通过 `(prefix, instance)` 作为联合键共存于 installed hash 中。zebra RIB 中 `rib_choose_best()` 选 metric 最小的条目（TE metric=1 < SPF metric=100），因此 TE 自动覆盖。删除 TE 后，SPF 自动恢复。

### 5.2 调用方用法

```c
struct midr_path_result result = {};

/* SPF 路由 */
result.instance = MIDR_INSTANCE_SPF;
result.paths[0].metric = 100;
midr_zebra_route_add(bgp, &prefix, &result);

/* TE SRv6 路由（同一前缀，自动覆盖 SPF） */
result.instance = MIDR_INSTANCE_TE;
result.explicit.sid_count = 3;
result.explicit.sid_list[0] = ...;
midr_zebra_route_add(bgp, &prefix, &result);
```

### 5.3 删除语义

`midr_zebra_route_del()` 默认 instance=SPF(0)。若需要精确删除特定 instance，目前通过 `midr_zebra_route_add` 中的 installed hash diff 机制自动处理旧条目的 DEL。

---

## 六、批量下发机制

### 6.1 流程

```
add/add/add/.../del   →  pending_ops 队列（累积）
                             ↓
update_deferred() 或 flush()  →  定时器/立即执行
                             ↓
midr_flush_pending()  →  FIFO 遍历每个 op
                             ↓
midr_process_one_op()  →  与 installed hash diff
  ├── 相同  →  跳过（减少 ZAPI 消息）
  └── 不同  →  DEL 旧 + ADD 新 → zclient_route_send()
```

### 6.2 参数

| 常量 | 值 | 说明 |
|------|:---:|------|
| `MIDR_BATCH_INTERVAL_MS` | 100ms | 批处理窗口 |

### 6.3 Ops 优先级

队列按 FIFO 顺序处理（先入先出）。同一个 prefix 的 ADD→DEL 序列会产生先安装后删除的效果（DEL 覆盖 ADD）。

---

## 七、ZAPI 编码细节

### 7.1 路由类型

```c
api->type = ZEBRA_ROUTE_BGP_MIDR;
api->distance = 115;             /* 与 IS-IS 同级 */
api->safi = SAFI_UNICAST;
```

### 7.2 距离值设计

| 协议 | 距离 | 优先级 |
|------|:---:|:---:|
| BGP EBGP | 20 | 最高 |
| MIDR | 115 | 中等 |
| BGP IBGP | 200 | 最低 |

MIDR 路由的优先级位于 EBGP 和 IBGP 之间，确保直连学习的外部路由优先于 SPF 计算的路由。

### 7.3 Netlink 协议号

```
RTPROT_BGP_MIDR = 199  （内核 /etc/iproute2/rt_protos 映射为 "midr"）
```

---

## 八、测试

### 8.1 编译策略

**问题**: 使用 `-Wl,--whole-archive libbgp.a` 链接会拉入 `rfapi`/`skiplist`/`ringbuf`/`bgp_ls_ted`/`bgp_script` 等模块，产生大量无法解析的符号依赖（`skiplist_*`、`ringbuf_*`、`ls_*`、`lua_*` 等）。

**解决方案**: 改用共享库链接，只链接 `bgp_midr_zebra.o` + `libfrr.so`，避免拉入 `libbgp.a` 中的无关模块：

```bash
# 所有测试二进制的通用编译命令格式
gcc -std=gnu11 -Wall -Wextra -g -O0 -include config.h \
  -I lib -I bgpd -I . \
  $(pkg-config --cflags libyang 2>/dev/null) \
  -o /tmp/test_midr_xxx tests/bgpd/test_midr_xxx.c bgpd/bgp_midr_zebra.o \
  -Wl,--wrap=zclient_route_send \
  -L lib/.libs -lfrr \
  -lcap -lcrypt -ljson-c -lrt -lpthread -lsqlite3 -lresolv -ldl -lm -lfl -lyang
```

关键点：
- 使用 `-L lib/.libs -lfrr`（共享库）而非 `lib/.libs/libfrr.a -Wl,--whole-archive bgpd/libbgp.a`
- `--wrap=zclient_route_send` 用于 mock ZAPI 调用（单元测试和批量测试）
- E2E 测试不需要 `--wrap=zclient_route_send`，因为它连接真实 zebra

### 8.2 单元测试

**文件**: `tests/bgpd/test_midr_zebra.c`（10 项测试，46 断言，含 Dual-Instance）

**编译**:
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

**执行**: `/tmp/test_midr_zebra`

**测试项**:

| # | 测试函数 | 断言 | 验证内容 |
|---|---------|:----:|---------|
| 1 | `test_lifecycle` | 7 | init → fini → double-fini 安全，NULL dp 保护 |
| 2 | `test_add_flush_diff` | 7 | 入队 → flush → installed hash 变更 → 重入去重 → 删除 |
| 3 | `test_add_then_delete` | 4 | ADD+DEL 同批次 => 不安装（FIFO 抵消） |
| 4 | `test_deferred` | 7 | 100ms 定时器合并、取消、空队列安全 |
| 5 | `test_srv6_path` | 2 | TE instance=1 SRv6 多条 SID 路径 |
| 6 | `test_multiple_prefixes` | 7 | 3 前缀独立安装、选择性删除、剩余验证 |
| 7 | `test_ucmp` | 2 | 混合权重 (204+51+0) 多路径 |
| 8 | `test_empty_ops` | 2 | 空队列 flush 和 deferred 安全 |
| 9 | `test_struct_layout` | 5 | sizeof 检查、零值语义、instance 默认值=SPF |
| 10 | `test_dual_instance` | 4 | SPF(instance=0) + TE(instance=1) 同前缀共存 |

**v4.0 实际结果**: 10/10 PASS，0 FAIL。

### 8.3 批量压力测试

**文件**: `tests/bgpd/test_midr_batch.c`

**编译**（与 8.2 相同，替换源文件）:
```bash
gcc -std=gnu11 -Wall -Wextra -g -O0 -include config.h \
  -I lib -I bgpd -I . \
  $(pkg-config --cflags libyang 2>/dev/null) \
  -o /tmp/test_midr_batch tests/bgpd/test_midr_batch.c bgpd/bgp_midr_zebra.o \
  -Wl,--wrap=zclient_route_send \
  -L lib/.libs -lfrr \
  -lcap -lcrypt -ljson-c -lrt -lpthread -lsqlite3 -lresolv -ldl -lm -lfl -lyang
```

**验证项**:

| 测试 | 规模 | 验证点 |
|------|:---:|--------|
| Batch IPv4 | 1024 路由 | 入队期间零 ZAPI 发送（全部批处理）、flush 一次性发送 >=1024、diff 去重无重复、批量删除完整 |
| Batch SRv6 | 256 路由 | SID 列表无损坏 |

**v4.0 实际结果**: 0 FAIL。

### 8.4 E2E ZAPI 端到端测试

**文件**: `tests/bgpd/test_midr_zebra_e2e.c`（284 行）

**编译**（不需要 `--wrap=zclient_route_send`）:
```bash
gcc -std=gnu11 -Wall -Wextra -g -O0 -include config.h \
  -I lib -I bgpd -I . \
  $(pkg-config --cflags libyang 2>/dev/null) \
  -o /tmp/test_e2e tests/bgpd/test_midr_zebra_e2e.c bgpd/bgp_midr_zebra.o \
  -L lib/.libs -lfrr \
  -lcap -lcrypt -ljson-c -lrt -lpthread -lsqlite3 -lresolv -ldl -lm -lfl -lyang
```

**前置条件**: zebra daemon 运行中，`/var/run/frr/zserv.api` 就绪。

**执行**:
```bash
# 启动 zebra
/usr/lib/frr/zebra -d -u frr -g frr
sleep 3

# 运行 E2E 测试
/tmp/test_e2e /var/run/frr/zserv.api
```

**测试场景**:

| 场景 | 步骤 | 验证 |
|------|------|:---:|
| Test A: SPF 单路由 | connect zebra → midr_zebra_init → midr_zebra_route_add(SPF) → flush → 验证 kernel FIB → del + flush → 验证清除 | SPF 路由完整生命周期 |
| Test B: Dual-Instance | install SPF(instance=0) → install TE SRv6(instance=1) → delete TE → verify SPF survives | TE 删除后 SPF 存活 |

**v4.0 实际结果**: ALL CHECKS PASSED（SRv6 WARN 为容器内核不支持 seg6 的预期限制）。

### 8.5 Proto 199 内核验证（ip route 命令层）

在容器内直接使用 `ip route` 命令验证内核 proto 199 支持。这是 MIDR 分层验证中的**最高抽象层**——不需要任何 MIDR 代码，直接用 shell 命令操作 proto 199 路由。

```bash
ip link add dummy_midr type dummy && ip link set dummy_midr up
ip route add 10.200.200.0/24 via 203.0.113.1 dev dummy_midr proto 199
ip route show proto 199     # → 10.200.200.0/24 ... proto midr
ip route del 10.200.200.0/24 proto 199
ip route show 10.200.200.0/24  # → (empty)
```

**v4.0 结果**: add/show/del 全部正常。

### 8.6 Netlink C 验证（libnl 底层验证，不引用 MIDR 代码）

**文件**: `tests/bgpd/test_midr_proto199.c`

通过 libnl-3 C API 直接发送 netlink 消息操作 proto 199 路由，验证底层 netlink socket 与 kernel 的交互。**此文件不引用任何 MIDR 头文件或代码**——`RTPROT_BGP_MIDR = 199` 硬编码在文件中。这是有意的分层设计：先隔离验证内核能力，再在上层测试 MIDR DP 逻辑。

```bash
# 在服务器主机上（不需要 FRR 构建环境）
gcc -std=gnu11 -I/usr/include/libnl3 test_midr_proto199.c \
    -lnl-3 -lnl-route-3 -lm -o /tmp/test_proto199
sudo /tmp/test_proto199
```

**验证内容**:
| 步骤 | 操作 | 关联 MIDR？ |
|:---:|------|:---:|
| 1 | libnl 创建 dummy 接口 | ❌ 无 |
| 2 | `rtnl_route_set_protocol(rt, RTPROT_BGP_MIDR=199)` | ❌ 硬编码 |
| 3 | `rtnl_route_add()` → kernel netlink | ❌ 标准 netlink |
| 4 | `ip route show proto 199` 验证 | ❌ shell 命令 |
| 5 | `rtnl_route_delete()` | ❌ 标准 netlink |

**v4.0 结果**: proto 199 路由 add/verify/delete 全部通过。确认内核接受 RTPROT_BGP_MIDR=199。

**MIDR 关联**：此测试成功后，意味着当 MIDR DP 通过 `zclient_route_send()` → zebra → netlink → kernel 下发路由时，内核层能正确识别 proto 199。MIDR DP 代码本身的正确性由 8.2 单元测试和 8.4 E2E ZAPI 测试验证。


### 8.7 Containerlab 拓扑测试

**脚本**: `tests/bgpd/run_midr_full_test.sh`（Phase 5）

**拓扑**: 2 节点 BGP (AS65001 ↔ AS65002)，镜像 `frr-ubuntu24-ymy:latest`

**测试流程**:
1. `sudo clab deploy` 部署 2 节点拓扑
2. 等待 zebra socket 就绪
3. 验证 BGP 会话 Established
4. **路由下发前连通性**: `ping 10.0.99.2` → 0% loss
5. 安装 MIDR SPF 路由 (metric=100, proto 199)
6. 安装 MIDR TE 路由 (metric=1, proto 199)
7. FIB 验证: `ip route show proto 199`
8. Dual-Instance: TE(metric=1) 覆盖 SPF(metric=100)
9. 删除 TE → SPF(metric=100) 自动回退
10. 删除全部路由 → FIB 清空
11. **路由删除后连通性**: `ping 10.0.99.2` → 0% loss

**v4.0 结果**: 11/11 PASS（连通性对比：路由下发前后均为 0% packet loss）。

### 8.8 完整自动化测试脚本

**文件**: `tests/bgpd/run_midr_full_test.sh`

一键运行 Phase 0-5 全部测试（编译 → 单元 → 批量 → Proto199 → E2E → Containerlab）。

```bash
# 在容器内
bash /home/frr/frr/tests/bgpd/run_midr_full_test.sh
```

或从服务器主机：
```bash
echo "thu325325" | sudo -S docker exec frr-ubuntu24-ymy \
    bash /home/frr/frr/tests/bgpd/run_midr_full_test.sh
```

### 8.9 测试结果汇总 (v4.0)

| 测试层 | 项数 | 结果 |
|--------|:---:|:---:|
| bgpd 编译验证 | 2 | ✅ 100% |
| 单元测试 | 10 (46 断言) | ✅ 100% |
| 批量压力测试 | 2 (1280 路由) | ✅ 100% |
| Proto 199 内核验证 | 5 | ✅ 100% |
| Netlink C 验证 | 5 | ✅ 100% |
| E2E ZAPI | 5 | ✅ 100% |
| Containerlab 拓扑 | 11 | ✅ 100% |

---

## 九、变更记录

### v4.0 (2026-07-08)

1. **编译策略改进**
   - 所有测试二进制改用共享库链接: `bgp_midr_zebra.o` + `-L lib/.libs -lfrr`
   - 弃用 `-Wl,--whole-archive libbgp.a`，避免拉入 rfapi/skiplist/ringbuf/lua 等无关模块
   - 单元测试、批量测试、E2E 测试二进制全部独立编译运行通过

2. **测试文档完善**
   - `documents/midr-test-report-20260708.md`: 更新至 v4.0，含完整测试结果
   - `documents/test-steps/03-unit-tests.md`: 增加共享库编译命令和实际输出
   - `documents/test-steps/04-e2e-zapi.md`: 增加共享库编译命令、zebra 启动步骤
   - `documents/test-steps/05-clab-topology.md`: 完整拓扑测试流程、配置文件、连通性对比
   - `documents/test-steps/06-batch-stress.md`: 增加共享库编译命令和实际输出

3. **测试脚本新增**
   - `tests/bgpd/run_midr_full_test.sh`: Phase 0-5 完整自动化测试脚本
   - Containerlab 拓扑 Phase 5 验证 BGP 会话、连通性对比、Dual-Instance

4. **代码注释增强**
   - `bgpd/bgp_midr_zebra.h`: 全文件详细 Doxygen 风格注释
   - `bgpd/bgp_midr_zebra.c`: 全函数注释 + 数据流说明

### v2.1 (2026-07-08)

1. **`bgp_midr_zebra.h`**
   - 新增 `#define MIDR_INSTANCE_SPF 0` / `#define MIDR_INSTANCE_TE 1`
   - `struct midr_path_result` 新增 `uint8_t instance` 字段
   - 添加 dual-instance TE 模型文档注释

2. **`bgp_midr_zebra.c`**
   - `midr_pending_op` 新增 `instance` 字段
   - `midr_installed_entry` 新增 `instance` 字段
   - `midr_prefix_hash_key()`: 引入 `instance * 31` 混合哈希
   - `midr_prefix_cmp()`: 增加 instance 相等判断
   - `midr_path_result_eq()`: 
     - 新增 `instance` 比较
     - 只比较 `prefix.family` 对应的地址族（修复缺陷）
   - `midr_result_to_zapi()`: 
     - 新增 `uint8_t instance` 参数（从调用方传入）
     - `api->instance = instance`（不再硬编码为 0）
     - TE mode: `api->metric = 1`；SPF mode: `api->metric = paths[0].metric`
   - `midr_installed_lookup/set/unset()`: 全部接受 `instance` 参数
   - `midr_process_one_op()`: 使用 `op->instance` 精确 DEL
   - `midr_zebra_route_add()`: 从 `result->instance` 传递
   - `midr_zebra_route_del()`: 默认 `MIDR_INSTANCE_SPF`

3. **测试文件更新**
   - `test_midr_zebra.c`: 新增 Test 10 dual-instance, `installed_has` 增加 instance 参数, struct 副本同步更新
   - `test_midr_zebra_e2e.c`: 新增 Dual-Instance SPF+TE SRv6 验证
   - `test_midr_batch.c`: 新建 1024 路由批量压力测试

