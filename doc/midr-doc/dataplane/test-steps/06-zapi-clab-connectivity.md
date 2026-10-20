# 测试 06：MIDR ZAPI Containerlab 连通性测试

## 测试目的

在 Containerlab 2 节点拓扑中，**不启动 bgpd**，仅通过手工配置 `ip route` 建立 r1↔r2 基线连通性，然后通过 MIDR ZAPI 安装/删除 proto 199 路由，根据连通性变化（ping）给出测试结果。

下面用真实数据面的连通性变化，逐层验证 MIDR ZAPI 对内核路由表的控制力。

## 核心概念说明

### metric（路由度量值）
Linux 内核 FIB 中到同一目的地的多条路由，**metric 越小优先级越高**。测试中手工基线路由设为 `metric=100`，MIDR ZAPI 二进制在 Blackhole/Stress 模式下安装路由时传入 **`metric=1`**（这是一个刻意的小值，用于覆盖手工路由；实际 BGP-LS SPF 算出的路由 metric 另有其值，不在此测试范围内）。`1 < 100` 所以 MIDR 路由覆盖手工路由。

### TTL（存活时间）
r1 ping r2 的 lo 地址 (`10.100.0.1`) 只经过 eth1 直连链路这一层 L2 转发，未经过路由器跳转（两台容器即使开了 `ip_forward`，对直连目标也不做 IP 层转发），所以 TTL 保持 Linux 默认初始值 64。ping 返回 ttl 值仅用于判断目标主机的协议栈是否回复（有 ttl=可达，无 ttl=不可达），此处不作路由跳数计算。

## 三项测试的设计意图

| 测试 | 在验证什么 | 对应 BGP-LS SPF 场景 |
|------|-----------|---------------------|
| **A. Blackhole** | MIDR 能否**阻断**一条已存在的路径 → 证明 DP 有"撤销/故障注入"能力 | SPF 算出一台节点故障，CP 下发 proto 199 黑洞路由丢弃去往该节点的流量 |
| **B. Redirect** | MIDR 能否用更优 metric **覆盖**手工路由 → 证明 DP 有"优选路径植入"能力 | SPF 算出一条更短路径，CP 下发低 metric 路由替换 BGP 原有路径 |
| **C. Stress** | 在高频 add/del 交替下，MIDR DP→ZAPI→zebra→netlink 管线是否**稳定不崩溃** | BGP-LS 拓扑频繁变更导致 SPF 反复重算，DP 需承受连续路由震荡 |

## 测试代码文件

| 文件 | 说明 |
|------|------|
| `tests/bgpd/test_midr_zapi_clab.c` | Containerlab 连通性测试程序（3 种模式：blackhole/redirect/stress）|
| `tests/bgpd/run_midr_zapi_clab.sh` | 编译 + 部署拓扑 + 执行 3 项测试的一键脚本 |
| `bgpd/bgp_midr_zebra.c` | MIDR DP 核心实现 |
| `bgpd/bgp_midr_zebra.h` | MIDR DP 公共头文件 |

## 拓扑结构（2 节点，无 BGP）

```
r1 ──────────── eth1 ──────────── r2
  eth1: 10.0.99.1/30     eth1: 10.0.99.2/30
                         lo: 10.100.0.1/32 (target)
```

- **基线路由**：r1 上 `ip route add 10.100.0.0/24 via 10.0.99.2 metric 100`
- **MIDR 路由**：r1 通过 ZAPI 安装 proto 199 路由（metric=1），覆盖基线路由

镜像: `frr-ubuntu24-ymy:latest`

## 前置条件

- 服务器 `101.6.30.220:50022` 上 frr-ubuntu24-ymy 容器已就绪
- FRR 构建系统已完成
- Containerlab 已安装（`sudo clab` 可用）

## 执行环境

```bash
# 从本地 mac SSH 到服务器
ssh -i ~/.ssh/frr yangmy@101.6.30.220 -p 50022
```

## 编译命令（在 dev 容器中编译）

```bash
echo "thu325325" | sudo -S docker exec -u 0 frr-ubuntu24-ymy bash -c "
cd /home/frr/frr
make bgpd/bgpd -j\$(nproc)
rm -f /tmp/test_midr_zapi_clab
gcc -std=gnu11 -Wall -Wextra -g -O0 -include config.h \
  -I lib -I bgpd -I . \$(pkg-config --cflags libyang 2>/dev/null) \
  -o /tmp/test_midr_zapi_clab tests/bgpd/test_midr_zapi_clab.c \
  bgpd/bgp_midr_zebra.o \
  -L lib/.libs -lfrr -lcap -lcrypt -ljson-c -lrt -lpthread \
  -lsqlite3 -lresolv -ldl -lm -lfl -lyang
"

# 复制到主机
echo "thu325325" | sudo -S docker cp frr-ubuntu24-ymy:/tmp/test_midr_zapi_clab /tmp/
echo "thu325325" | sudo -S docker cp frr-ubuntu24-ymy:/home/frr/frr/lib/.libs/libfrr.so.0.0.0 /tmp/libfrr.so
```

## 部署 + 测试（一键脚本）

```bash
# 上传脚本到服务器
scp -i ~/.ssh/frr -P 50022 tests/bgpd/run_midr_zapi_clab.sh yangmy@101.6.30.220:/tmp/

# 运行
ssh -i ~/.ssh/frr yangmy@101.6.30.220 -p 50022
echo "thu325325" | sudo -S bash /tmp/run_midr_zapi_clab.sh
```

## 测试详解

### Test A: Blackhole 连通性

| 阶段 | 操作 | 预期 |
|------|------|:---:|
| BEFORE | ping 10.100.0.1 | ✅ 0% loss |
| DURING | MIDR 安装 10.100.0.0/24 via 10.200.0.1 (dead nexthop) | ❌ 100% loss |
| AFTER | MIDR 删除路由 | ✅ 0% loss（BGP 路径恢复）|

### Test B: Redirect（metric 覆盖）

| 阶段 | 操作 | 预期 |
|------|------|:---:|
| BEFORE | TTL 基线 | 正常 TTL |
| DURING | MIDR 安装 metric=1 路由（覆盖手工的 metric=100） | FIB 出现 proto 199，0% loss |
| AFTER | MIDR 删除 | 手工 metric=100 路由恢复，0% loss |

### Test C: Stress（高频增删）

| 阶段 | 操作 | 预期 |
|------|------|:---:|
| 打流 | ping -i 0.2 10.100.0.1 持续 | - |
| 压力 | 20 轮 add/del (300ms 间隔) | 20/20 安装 + 20/20 删除 |
| 结束后 | ping 10.100.0.1 | ✅ 0% loss |

## 预期测试输出

```
============================================
  MIDR ZAPI Clab Connectivity Test
============================================
  Test A: Blackhole
[PASS] A1: BEFORE TTL=64
[PASS] A2: DURING UNREACHABLE (blackhole works)
[PASS] A3: ZAPI binary PASSED
[PASS] A4: AFTER reachable (TTL=64) — restored!
  Test B: Redirect
[PASS] B2: Redirect with 0% loss (MIDR metric=1 routes working)
[PASS] B3: Redirect binary PASSED
[PASS] B4: AFTER reachable (TTL=64) — restored!
  Test C: Stress
[PASS] C2: Stress binary PASSED
[PASS] C3: Connectivity intact after stress
  ------------------------------
  PASS: XX | FAIL: 0
  ALL TESTS PASSED
============================================
```

## 连通性对比总结

| 测试 | 阶段 | 操作 | 预期 |
|:---:|------|------|:---:|
| **A** | BEFORE | ping 10.100.0.1 | ✅ 0% loss |
| | DURING | MIDR 黑洞 (dead nexthop) | ❌ 100% loss |
| | AFTER | MIDR 删除 | ✅ 0% loss |
| **B** | BEFORE | TTL 基线 | TTL=N |
| | DURING | MIDR metric=1 覆盖 | 0% loss, proto 199 可见 |
| | AFTER | MIDR 删除 | TTL=N, 0% loss |
| **C** | DURING | 20轮 add/del | 20/20 成功 |
| | AFTER | ping | ✅ 0% loss |

## 结论

MIDR ZAPI 在 Containerlab 真实拓扑中，能够在控制面正确安装/删除 proto 199 路由，并通过连通性测试验证数据面的真实效果：黑洞阻断 100% 丢包，删除后自动恢复。

## 自动化脚本

```bash
scp -i ~/.ssh/frr -P 50022 tests/bgpd/scripts/test06_zapi_clab.sh yangmy@101.6.30.220:/tmp/
ssh -i ~/.ssh/frr yangmy@101.6.30.220 -p 50022 "bash /tmp/test06_zapi_clab.sh"
```

脚本路径: `tests/bgpd/scripts/test06_zapi_clab.sh`

> **注**: clab 拓扑的 `exec` 钩子直接调用 `/usr/lib/frr/zebra -d -u frr -g frr` 启动 zebra，绕过 `frrinit.sh` 的 watchfrr 依赖，确保 ZAPI socket 在容器启动后自动就绪。