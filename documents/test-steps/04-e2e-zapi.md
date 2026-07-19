# 测试 04：E2E ZAPI 端到端路由测试

## 测试目的

验证 MIDR DP 模块通过 ZAPI 通道连接运行中的 zebra daemon，下发路由并验证内核 FIB 中的安装与删除。覆盖 SPF 路由和 Dual-Instance SPF+TE SRv6 两种场景。

## 测试代码文件

| 文件 | 说明 |
|------|------|
| `tests/bgpd/test_midr_zebra_e2e.c` | E2E ZAPI 测试程序 |
| `bgpd/bgp_midr_zebra.c` | MIDR DP 核心实现 |
| `bgpd/bgp_midr_zebra.h` | MIDR DP 公共头文件 |

## 测试场景

### Test A: SPF 单路由
- 连接 zebra → init MIDR DP → `midr_zebra_route_add(SPF)` → flush → 验证 FIB → 删除 → 验证清除

### Test B: Dual-Instance SPF+TE SRv6
- 安装 SPF route (instance=0) → 安装 TE SRv6 route (instance=1) → 验证双实例共存 → 删除 TE → 验证 SPF 存活

## 前置条件

- 容器内 zebra daemon 已运行（`/usr/lib/frr/zebra -d -u frr -g frr`）
- zebra socket 就绪（`/var/run/frr/zserv.api`）
- `dummy0` 虚接口已创建，配置 IPv4 + IPv6 地址和静态邻居
- bgpd 已用最新代码编译


## 执行环境

```bash
# 从本地 mac SSH 到服务器
ssh -i ~/.ssh/frr yangmy@101.6.30.220 -p 50022

# 进入容器（以 root 运行）
sudo docker exec -u 0 frr-ubuntu24-ymy bash
cd /home/frr/frr
```

## 编译命令（共享库链接）

```bash
cd /home/frr/frr

gcc -std=gnu11 -Wall -Wextra -g -O0 -include config.h \
  -I lib -I bgpd -I . \
  $(pkg-config --cflags libyang 2>/dev/null) \
  -o /tmp/test_e2e tests/bgpd/test_midr_zebra_e2e.c bgpd/bgp_midr_zebra.o \
  -L lib/.libs -lfrr \
  -lcap -lcrypt -ljson-c -lrt -lpthread -lsqlite3 -lresolv -ldl -lm -lfl -lyang
```

## 执行命令

```bash
# 清理旧二进制
rm -f /tmp/test_e2e

# 启动 zebra
echo -e "bgpd=yes\nzebra=yes" > /etc/frr/daemons
pkill -9 zebra 2>/dev/null || true
sleep 1
/usr/lib/frr/zebra -d -u frr -g frr 2>&1 &
sleep 3

# 检查 zebra 状态
pgrep zebra && echo "zebra running"
ls -la /var/run/frr/zserv.api

# 运行测试（需要 LD_LIBRARY_PATH 指向 libfrr.so）
LD_LIBRARY_PATH=/home/frr/frr/lib/.libs /tmp/test_e2e /var/run/frr/zserv.api
```

## 实际测试输出

```
=== MIDR E2E Route Installation Test (Dual-Instance) ===
zebra socket: /var/run/frr/zserv.api

[Step 1] Setup dummy0 with IPv4=192.0.2.1 IPv6=2001:db8::1/64
[Step 2] Connect to zebra              → Connected to zebra OK
[Step 3] midr_zebra_init               → OK

--- Test A: Pure IP SPF (instance=0) ---
[A.1] midr_zebra_route_add(10.254.1.0/24 via 192.0.2.1, instance=SPF)
[A.2] midr_zebra_route_flush
[A.3] Verify 10.254.1.0/24 in kernel FIB
  OK: route 10.254.1.0/24 in FIB
[A.4] Delete SPF route
  OK: route 10.254.1.0/24 removed from FIB

--- Test B: Dual-Instance SPF+TE SRv6 ---
[B.1] Install SPF route (instance=0)   → OK: SPF route installed
[B.2] Install TE SRv6 route (instance=1)
  OK: SRv6 route installed (nexthop=2001:db8::2 if=dummy0)
[B.3] Delete TE SRv6
[B.4] SPF survives TE delete           → OK: SPF survives TE delete

=== ALL CHECKS PASSED ===
```


## 正确构造 SRv6 路径的核心代码

```c
// Step 1: dummy0 上配置 IPv6 地址和邻居
run_cmd("ip -6 addr add 2001:db8::1/64 dev dummy0");
run_cmd("ip -6 neigh add 2001:db8::2 lladdr 00:11:22:33:44:55 dev dummy0");

// Test B.2: SRv6 路由下发
inet_pton(AF_INET6, "2001:db8::2", &paths_srv6[0].nexthop.ipv6);
paths_srv6[0].ifindex = if_nametoindex("dummy0");  // 必须显式设置！
```

## 数据流路径

```
midr_zebra_route_add()                    ← CP → DP 接口
  → zclient_route_send(ROUTE_ADD)         ← ZAPI 通道
  → zebra daemon                          ← nexthop 可达性检查（需要 ifindex + 同网段）
  → zebra2proto() → RTPROT_BGP_MIDR = 199
  → netlink (rt_netlink.c)
  → kernel FIB                            ← ip -6 route show proto midr 可见
  → 验证: ALL CHECKS PASSED               ← ✅（含 SRv6）
```

## 结论

E2E ZAPI 测试全部通过（含 SRv6）。SPF 路由和 TE SRv6 路由的安装、内核 FIB 验证、删除、Dual-Instance 共存与回退均正常。

## 自动化脚本

```bash
scp -i ~/.ssh/frr -P 50022 tests/bgpd/scripts/test04_e2e_zapi.sh yangmy@101.6.30.220:/tmp/
ssh -i ~/.ssh/frr yangmy@101.6.30.220 -p 50022 "bash /tmp/test04_e2e_zapi.sh"
```

脚本路径: `tests/bgpd/scripts/test04_e2e_zapi.sh`
