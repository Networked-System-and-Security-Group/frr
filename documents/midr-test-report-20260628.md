# MIDR 数据平面测试报告

> **日期**: 2026-06-28  
> **环境**: Docker `frr-ubuntu24-ymy` 


---

## 一、测试概览

| 测试层级 | 测试项 | 通过 | 失败 | 通过率 |
|---------|-------|:---:|:---:|:------:|
| **单元测试** | 9 项 (42 断言) | 9 | 0 | **100%** |
| **Topotest 集成验证** | 7 项 | 7 | 0 | **100%** |
| **Proto 199 内核验证** | 4 步 (add/show/del/verify) | 4 | 0 | **100%** |
| **Netlink C 验证** | 7 步 | 7 | 0 | **100%** |
| **E2E 端到端测试** | 8 步 | 8 | 0 | **100%** |

---

## 二、测试架构与各层级目的

```
┌──────────────────────────────────────────────────┐
│  第一层: 单元测试 (test_midr_zebra.c)              │
│  目的: 验证 MIDR DP 内部逻辑                       │
│  方式: mock zclient_route_send, 不依赖外部进程      │
│  范围: bgp_midr_zebra.c 的 API 函数逻辑             │
├──────────────────────────────────────────────────┤
│  第二层: Topotest 集成验证 (test_midr_dp_topo1.py) │
│  目的: 验证 MIDR 构建集成 + kernel 层功能           │
│  方式: 编译运行单元测试 + ip route 操作 proto 199    │
│  范围: 构建系统、源文件完整性、proto 199 内核功能     │
├──────────────────────────────────────────────────┤
│  第三层: Proto 199 内核验证                         │
│  目的: 验证 kernel 识别 proto 199 → "midr"         │
│  方式: ip route 命令 (模拟用户/系统管理员视角)        │
│  范围: kernel FIB 的协议号识别                      │
├──────────────────────────────────────────────────┤
│  第四层: Netlink C 级验证 (test_midr_proto199.c)   │
│  目的: 验证 libnl C API 能操作 proto 199 路由      │
│  方式: libnl-3 直接发送 netlink 消息                │
│  范围: 底层 netlink socket 与 kernel 的交互          │
├──────────────────────────────────────────────────┤
│  第五层: E2E 端到端测试 (test_midr_zebra_e2e.c)   │
│  目的: 验证 MIDR 路由在实际运行的 zebra 中安装与删除   │
│  方式: 连接运行中的 zebra daemon，通过 ZAPI 通道下发路由   │  
│  范围: zebra daemon 连接、ZAPI 通道、路由安装与删除     │
└─────────────────────────────────────────────────────┘
```

### Proto 199 内核验证 vs Netlink C 验证

| 对比维度 | Proto 199 内核验证 | Netlink C 验证 |
|---------|-------------------|---------------|
| 调用方式 | `ip route` CLI 命令 | libnl C API (`rtnl_route_*`) |
| 抽象层级 | 最高层 (shell 命令) | 最底层 (netlink socket) |
| 测试代码 | Python subprocess | C 语言 + libnl3 |
| 关键依赖 | iproute2 | libnl-3-dev |

两者验证 kernel 对 proto 199 的支持，但视角不同：前者是**功能端到端验证**，后者是**底层 C API 兼容性验证**。


---

## 三、单元测试结果

### 测试文件

`tests/bgpd/test_midr_zebra.c` — 9 项测试，42 断言。编译链接需要加 `-llua5.3`。

### 输出摘要

```
=== MIDR Data-Plane Unit Tests ===
[Test 1] init/fini lifecycle                   7/7  OK
[Test 2] add/flush/diff                        7/7  OK
[Test 3] add-then-delete                       4/4  OK
[Test 4] deferred timer coalescing             7/7  OK
[Test 5] SRv6 path                             2/2  OK
[Test 6] multiple prefixes                     7/7  OK
[Test 7] UCMP weights                          2/2  OK
[Test 8] empty operations                      2/2  OK
[Test 9] struct layout                         4/4  OK
=== 0 test(s) FAILED ===
```

### 逐项分析

| # | 测试函数 | 断言 | 验证内容 |
|---|---------|:----:|---------|
| 1 | `test_lifecycle` | 7 | init → fini → double-fini 安全 |
| 2 | `test_add_flush_diff` | 7 | 入队 → flush → installed hash 变更 |
| 3 | `test_add_then_delete` | 4 | ADD+DEL 同批次不安装 |
| 4 | `test_deferred` | 7 | 定时器合并与取消 |
| 5 | `test_srv6_path` | 2 | 多段 SID + IPv6 nexthop |
| 6 | `test_multiple_prefixes` | 7 | 多前缀独立管理与选择性删除 |
| 7 | `test_ucmp` | 2 | 混合权重 (204/51/0) |
| 8 | `test_empty_ops` | 2 | 空队列安全 |
| 9 | `test_struct_layout` | 4 | sizeof / 零值语义 |

---

## 四、Topotest 集成验证结果

### 测试文件

`tests/topotests/midr_dp_topo1/test_midr_dp_topo1.py` — 7 项测试。其中 `test_kernel_proto199_midr` 通过 `ip route` 命令验证 proto 199 路由功能。

### 输出

```
============================= 7 passed in 1.93s ===============================
```

### 逐项分析

| # | 测试函数 | 验证内容 |
|---|---------|---------|
| 1 | `test_unit_build_and_run` | 编译 + 运行单元测试通过 |
| 2 | `test_kernel_proto199_midr` | `ip route add/show/del proto 199` |
| 3 | `test_midr_route_type` | ZEBRA_ROUTE_BGP_MIDR 在 route_types.txt |
| 4 | `test_midr_rtprot` | RTPROT_BGP_MIDR = 199 |
| 5 | `test_midr_symbols_in_build` | bgp_midr_zebra.o 在 libbgp.a |
| 6 | `test_midr_source_files` | 12 个文件含 MIDR 内容 |
| 7 | `test_midr_debug_nl` | debug/fpm/kernel 含 MIDR |

---

## 五、Proto 199 内核验证

```bash
$ ip route add 10.200.200.0/24 via 203.0.113.1 dev dummy_midr proto 199
$ ip route show proto midr      → 10.200.200.0/24 via 203.0.113.1 dev dummy_midr
$ ip route show proto 199       → 10.200.200.0/24 via 203.0.113.1 dev dummy_midr
$ ip route del 10.200.200.0/24 proto 199
$ ip route show 10.200.200.0/24 → (empty)
```

系统配置：`/etc/iproute2/rt_protos` 中有 `199  midr`。

---

## 六、Netlink C 级验证

### 测试文件

`tests/bgpd/test_midr_proto199.c`

### 输出

```
=== ALL CHECKS PASSED (proto 199 = midr verified) ===
```

通过 libnl-3 库直接构造 netlink 消息操作 proto 199 路由，添加和删除均成功。

---

## 七、E2E 端到端测试结果

### 测试文件

`tests/bgpd/test_midr_zebra_e2e.c` — 连接运行中的 zebra daemon，通过 ZAPI 通道下发 MIDR 路由并验证 kernel FIB。

### 输出

```
=== MIDR E2E Route Installation Test ===

[Step 1] Setup dummy0 with 192.0.2.1
[Step 2] Connect to zebra                    OK
[Step 3] midr_zebra_init                     OK
[Step 4] midr_zebra_route_add                OK
[Step 5] midr_zebra_route_flush              OK
[Step 6] Check kernel FIB                    OK: route installed
[Step 7] midr_zebra_route_del + flush        OK
[Step 8] Check kernel FIB after delete       OK: route removed

=== ALL CHECKS PASSED ===
```

### 验证的完整链路

```
midr_zebra_route_add()                    ← DP API
    → zclient_route_send() (ZAPI)         ← ZAPI 通道
    → zebra daemon                        ← 实际运行的 zebra
    → zebra2proto() → RTPROT_BGP_MIDR=199
    → netlink (rt_netlink.c)
    → kernel FIB                           ← ip route show 可见
```

---

## 八、总结

| 测试类别 | 通过率 | 状态 |
|---------|:------:|:----:|
| 单元测试 (9 项, 42 断言) | **100%** | ✅ |
| Topotest 集成验证 (7 项) | **100%** | ✅ |
| Proto 199 内核验证 | **100%** | ✅ |
| Netlink C 级验证 (7 步) | **100%** | ✅ |
| E2E 端到端测试 (8 步) | **100%** | ✅ |

