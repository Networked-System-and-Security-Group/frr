# 测试 02：Netlink C 协议 199 验证

## 测试目的

本测试是 MIDR 分层验证中的**底层内核协议号验证**——独立于 MIDR 代码，验证 Linux 内核能通过 libnl-3/netlink 识别 `RTPROT_BGP_MIDR = 199` 协议号。

**为什么不引用 MIDR 头文件？**  
这是有意的设计：先隔离验证"内核是否接受 proto 199"，再在上层测试 MIDR DP 逻辑。如果 E2E 测试失败，可以排除内核层的嫌疑。

**MIDR 关联**：测试成功后，意味着当 MIDR DP 调用 `zclient_route_send()` → zebra → netlink 时，内核能识别 proto 199。MIDR DP 代码的正确性由单元测试（mock zclient）和 E2E ZAPI 测试（真实 zebra 连接）验证。

## 测试代码文件

| 文件 | 说明 |
|------|------|
| `tests/bgpd/test_midr_proto199.c` | 独立验证程序（硬编码 `RTPROT_BGP_MIDR 199`，不引用 MIDR 代码） |

## 前置条件

- 服务器 101.6.30.220 上已安装 `libnl-3-dev`、`libnl-route-3-dev`
- 具有 root 权限（sudo）
- 容器内 `/etc/iproute2/rt_protos` 已配置 `199  midr`

## 执行环境与关联说明

```
测试分层:
  test_midr_proto199.c  ← 本文件（内核层隔离验证）
  不引用 MIDR 代码
  只用 libnl-3 直接发 netlink 消息
  RTPROT_BGP_MIDR=199 硬编码在本文中
      ↓ 验证通过后
  test_midr_zebra_e2e.c  ← 引用 bgp_midr_zebra.h
  真正调用 midr_zebra_route_add()
  通过 ZAPI 连接 zebra → netlink → kernel
```

```bash
# 从本地 mac SSH 到服务器
ssh -i ~/.ssh/frr yangmy@101.6.30.220 -p 50022

# 进入容器
sudo docker exec -it frr-ubuntu24-ymy bash
echo "thu325325" | sudo -S chown -R frr:frr /home/frr/frr/bgpd/
cd /home/frr/frr
```

## 编译命令

在容器内编译：

```bash
cd /home/frr/frr/tests/bgpd

gcc -std=gnu11 -Wall -Wextra -g -O0 \
    -I/usr/include/libnl3 \
    test_midr_proto199.c \
    -lnl-3 -lnl-route-3 -lm \
    -o /tmp/test_midr_proto199
```

## 执行命令

```bash
sudo /tmp/test_midr_proto199
```

## 验证步骤

| 步骤 | 操作 | 命令/预期 | 验证点 |
|:---:|------|----------|--------|
| 1 | 创建 dummy 接口 | `rtnl_link_add()` → ifindex 分配 | libnl 链路层 API 正常 |
| 2 | 构造 proto 199 路由 | `rtnl_route_set_protocol(route, 199)` | RTPROT_BGP_MIDR 设置 |
| 3 | 添加路由到内核 | `rtnl_route_add()` 发送 netlink 消息 | 内核接受 proto 199 |
| 4 | 验证路由可见 | `ip route show 10.200.200.0/24 proto 199` | proto 199 路由在 FIB |
| 5 | 删除路由 | `rtnl_route_delete()` | netlink DELETE 成功 |
| 6 | 验证路由删除 | `ip route show 10.200.200.0/24` → 空 | 内核确认路由已删除 |

## 实际测试输出（2026-07-08）

```
=== MIDR Protocol 199 Netlink Test ===
[Step 1] Create dummy_nl interface
  ifindex = 24
[Step 3] Add 10.200.200.0/24 via 203.0.113.1 proto 199
  Route added via netlink
[Step 4] Verify proto midr visible
  10.200.200.0/24 via 203.0.113.1 dev dummy_nl 
  OK: 10.200.200.0/24 shows as proto midr
[Step 5] Verify proto 199 visible
  OK: 10.200.200.0/24 shows as proto 199
[Step 6] Delete route with proto 199
[Step 7] Verify route removed
  OK: route deleted

=== ALL CHECKS PASSED (proto 199 = midr verified) ===
```

## 结论

libnl-3 C API 通过 netlink socket 成功操作 proto 199 路由。路由添加、内核 FIB 查询验证、路由删除全部正常。底层 netlink 消息交互正确，`RTPROT_BGP_MIDR = 199` 协议号在内核中正确处理。

> **注**: Step 4 (`ip route show proto midr`) 在主机上可能显示为 `proto 199` 而非 `proto midr`，这是因为主机 `/etc/iproute2/rt_protos` 可能缺少 `199  midr` 映射（容器内已配置）。这不影响 netlink 协议的正确性。

## 自动化脚本

```bash
scp -i ~/.ssh/frr -P 50022 tests/bgpd/scripts/test02_proto199_netlink.sh yangmy@101.6.30.220:/tmp/
ssh -i ~/.ssh/frr yangmy@101.6.30.220 -p 50022 "bash /tmp/test02_proto199_netlink.sh"
```

脚本路径: `tests/bgpd/scripts/test02_proto199_netlink.sh`
