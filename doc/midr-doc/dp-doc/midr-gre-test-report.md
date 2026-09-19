# MIDR GRE 虚拟链路接口测试报告

> 被测接口：`bgpd/bgp_midr_gre.h`
> 测试脚本：`tests/bgpd/midr_gre_connectivity_test.sh`
> 辅助程序：`tests/bgpd/test_midr_gre_link.c`
> 接口说明：`doc/midr-doc/midr-gre-interface-api.md`

## 1. 测试目标

验证 MIDR 控制面创建的 GRE 虚拟链路接口在**两个真实容器之间**能够：

1. 通过接口成功创建（且接口 API 能 **返回是否建立成功**）；
2. 承载 **IPv4** 业务并连通；
3. 承载 **IPv6** 业务并连通；
4. 在 **IPv6 承载（ip6gre）** 场景下同样建立并连通；
5. 能够被正确消除。

---

## 2. 测试环境

| 项 | 值 |
|----|----|
| 宿主 | `dell-PowerEdge-R750`，Ubuntu 5.15 内核 |
| 被测容器 | `node1`、`node2`（镜像 `frr-ubuntu24-ymy:init`） |
| 构建容器 | `frr-ubuntu24-ymy`（FRR 源码 `/home/frr/frr`） |
| FRR 版本 | `10.7.0-dev`（含本分支 MIDR/GRE 改动） |
| 网络 | 同一 docker bridge（`test-net`，10.1.1.0/24） |

### 2.1 拓扑

```
        node1                                            node2
   10.1.1.11/24  ────────── docker bridge ──────────  10.1.1.12/24
        │                                                  │
        └────────── GRE 虚拟链路（MIDR API 创建） ──────────┘
             gre1 : 192.168.100.1/30  <->  192.168.100.2/30     (IPv4 overlay)
             gre1 : fd00:100::1/64    <->  fd00:100::2/64       (IPv6 over IPv4)
             gre6 : fd00:200::1/64    <->  fd00:200::2/64       (IPv6 over ip6gre)
```

### 2.2 部署步骤

```sh
# 1) 在构建容器内打包运行时（zebra + libfrr + 测试客户端）
docker exec -u root frr-ubuntu24-ymy bash -c '
  rm -rf /tmp/midr_rt && mkdir -p /tmp/midr_rt/lib
  cp /home/frr/frr/zebra/.libs/zebra /tmp/midr_rt/zebra
  cp -a /home/frr/frr/lib/.libs/libfrr.so.0.0.0 /tmp/midr_rt/lib/
  ln -sf libfrr.so.0.0.0 /tmp/midr_rt/lib/libfrr.so.0
  ln -sf libfrr.so.0 /tmp/midr_rt/lib/libfrr.so
  cp /tmp/test_midr_gre_link /tmp/midr_rt/
  tar czf /tmp/midr_runtime.tgz -C /tmp/midr_rt .'

# 2) 分发到 node1 / node2 的 /opt/midr
docker cp frr-ubuntu24-ymy:/tmp/midr_runtime.tgz /tmp/
for n in node1 node2; do
  docker cp /tmp/midr_runtime.tgz $n:/tmp/
  docker exec -u root $n bash -c 'rm -rf /opt/midr && mkdir -p /opt/midr &&
      tar xzf /tmp/midr_runtime.tgz -C /opt/midr && chmod +x /opt/midr/*'
done

# 3) 运行全部连通性用例
sudo bash tests/bgpd/midr_gre_connectivity_test.sh
```

> 环境注意事项（脚本已自动处理）：
> - zebra 以 `-u root -g root` 运行，需先把 root 加入 `frrvty` 组；
> - 容器 eth0 默认 `net.ipv6.conf.eth0.disable_ipv6=1`，IPv6 承载用例前需置 0。

---

## 3. 结果汇总

```
=== summary: PASS=21 FAIL=0 ===
```

| 编号 | 用例 | 结果 |
|------|------|:----:|
| A | IPv4 GRE（`gre`）建立 + IPv4 overlay 双向连通 | ✅ |
| B | 同一 GRE 隧道承载 IPv6 overlay 连通 | ✅ |
| C | IPv6 GRE（`ip6gre`）建立 + IPv6 overlay 双向连通 | ✅ |
| D | 四个接口全部消除 | ✅ |
| - | 建立状态 API 返回 `state=up` 且 `ifindex` 有效 | ✅ |
| - | underlay IPv4 / IPv6 可达性 | ✅ |

---

## 4. 详细测试记录（实测输出）

### 4.0 环境与建链

```
[PASS] zebra running on node1
[PASS] zebra running on node2
[PASS] underlay IPv4 10.1.1.11 -> 10.1.1.12
[PASS] underlay IPv6 fd00:1::11 -> fd00:1::12
```

### 4.A IPv4 GRE 隧道 + IPv4 overlay

```
==== A. IPv4 GRE tunnel (gre) + IPv4 overlay ====
MIDR_GRE name=gre1 state=up ifindex=14 err=0      <-- node1（API 返回建立成功）
MIDR_GRE name=gre1 state=up ifindex=12 err=0      <-- node2
[PASS] node1 gre1 established (ifindex 14)
[PASS] node2 gre1 established (ifindex 12)
[PASS] node1 gre1 is a gre device
[PASS] node2 gre1 is a gre device
[PASS] IPv4 connectivity node1 -> node2 over gre1
[PASS] IPv4 connectivity node2 -> node1 over gre1
```

说明：`state=up` 由 `midr_gre_interface_wait_up()` 返回，`ifindex` 为内核真实
索引，证明接口**确已建立**（而非仅请求已发送）。

### 4.B IPv6 overlay over IPv4 GRE

```
==== B. IPv6 overlay over the IPv4 GRE tunnel ====
[PASS] IPv6 connectivity node1 -> node2 over gre1
```

gre1 上同时配置 `fd00:100::1/64` / `fd00:100::2/64`，`ping6` 双向可达。

### 4.C IPv6 GRE（ip6gre）+ IPv6 overlay

```
==== C. IPv6 GRE tunnel (ip6gre) + IPv6 overlay ====
MIDR_GRE name=gre6 state=up ifindex=15 err=0
MIDR_GRE name=gre6 state=up ifindex=13 err=0
[PASS] node1 gre6 established (ifindex 15)
[PASS] node2 gre6 established (ifindex 13)
[PASS] node1 gre6 is an ip6gre device
[PASS] node2 gre6 is an ip6gre device
[PASS] IPv6 connectivity node1 -> node2 over gre6
[PASS] IPv6 connectivity node2 -> node1 over gre6
```

内核视角（`ip -d link show`）：
```
gre6@NONE: <POINTOPOINT,UP,LOWER_UP> ... link/gre6 fd00:1::11 peer fd00:1::12
    ip6gre remote fd00:1::12 local fd00:1::11 hoplimit inherit ...
```

### 4.D 消除

```
==== D. Teardown ====
MIDR_GRE name=gre1 state=down ifindex=0 err=0
MIDR_GRE name=gre1 state=down ifindex=0 err=0
MIDR_GRE name=gre6 state=down ifindex=0 err=0
MIDR_GRE name=gre6 state=down ifindex=0 err=0
[PASS] node1 gre1 removed
[PASS] node2 gre1 removed
[PASS] node1 gre6 removed
[PASS] node2 gre6 removed
```

---

## 5. 测试过程中发现并解决的问题

| # | 现象 | 原因 | 处理 |
|---|------|------|------|
| 1 | node1/node2 上 zebra 启动失败：`user(root) is not part of vty group specified(frrvty)` | 容器内 root 不在 `frrvty` 组 | 脚本中 `usermod -aG frrvty root` |
| 2 | ip6gre 用例不通，`ping6` 报 Network unreachable | 容器 eth0 `disable_ipv6=1` | 脚本中先 `sysctl -w net.ipv6.conf.eth0.disable_ipv6=0` |
| 3 | 用"一次性客户端"删除接口时请求丢失，接口仍在 | 客户端发出 `ZEBRA_GRE_DELETE` 后立即退出，zebra 在同一次读中同时读到数据与 EOF，丢弃了请求 | 辅助程序在发送后保持 zclient 存活约 1s（`pump_events(1000)`），并在文档中作为注意事项说明 |
| 4 | 接口 kind 判定误判 ip6gre | `ip -d link` 输出中先出现 `link/gre6` | 先匹配 `ip6gre` 再匹配 `gre` |

> 第 3 条是通用工程问题：长时间运行的 bgpd 不受影响；一次性/短命客户端需注意。

---

## 6. 复现方法

```sh
# 前提：node1/node2 已部署 /opt/midr（见 2.2）
sudo bash midr_gre_connectivity_test.sh
# 期望最后一行
=== summary: PASS=21 FAIL=0 ===
```

`test_midr_gre_link` 也可单独使用：

```sh
# 建立
LD_LIBRARY_PATH=/opt/midr/lib /opt/midr/test_midr_gre_link setup \
    --sock /tmp/zserv_midr.api --name gre1 \
    --local 10.1.1.11 --remote 10.1.1.12 \
    --ip 192.168.100.1/30 --ip6 fd00:100::1/64 --mtu 1400
# 输出：MIDR_GRE name=gre1 state=up ifindex=14 err=0

# 消除
LD_LIBRARY_PATH=/opt/midr/lib /opt/midr/test_midr_gre_link teardown \
    --sock /tmp/zserv_midr.api --name gre1
```

---

## 7. 结论与建议

**结论**：GRE 接口在两个真实容器间完成了 IPv4 与 IPv6 的实际
连通性验证（21/21 通过），并且接口能够**返回虚拟链路是否建立成功**
（`state` + `ifindex` + 回调）。

**后续建议**：

1. 在真实 FRR 部署中，创建后还需由 CP 负责**拉起接口并配置地址/覆盖路由**
   （本组接口不含该职责）。
2. 如需二层 GRETAP（`gretap`/`ip6gretap`），可在此框架上扩展 kind 选择。
3. 建议将 `tests/bgpd/test_midr_gre_e2e.c` 与
   `midr_gre_connectivity_test.sh` 纳入 CI（后者需 docker 与 NET_ADMIN）。

