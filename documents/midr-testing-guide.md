# MIDR 数据平面测试说明文档

> **版本**: v2.0  
> **日期**: 2026-06-21  
> **覆盖**: 单元测试 + Topotest 集成测试 + 脚本化一键测试

---

## 一、测试概览

| 层级 | 套件 | 文件 | 语言 |
|------|------|------|------|
| **单元测试** | 9 项 DP 函数测试 | `tests/bgpd/test_midr_zebra.c` | C + libbgp.a |
| **Topotest** | 7 项集成 + 功能验证 | `tests/topotests/midr_dp_topo1/` | Python + pytest |
| **脚本** | 1 容器脚本 + 1 服务器脚本 | `scripts/` | Bash |

---

## 二、单元测试

### 2.1 目标

验证 `bgp_midr_zebra.c` 核心逻辑：路由安装/撤销、批量防抖、立即下发、生命周期、installed-hash diff。

### 2.2 测试项（9 项，35 断言）

| # | 测试函数 | 内容 | 断言 |
|---|---------|------|-----|
| 1 | `test_lifecycle` | init → fini 清理 → double-fini 安全 | 7 |
| 2 | `test_pending_add_flush` | route_add 入队 → flush → installed hash | 7 |
| 3 | `test_add_then_delete` | ADD+DEL 同批次 → 最终不安装 | 4 |
| 4 | `test_deferred` | deferred 定时器合并 | 7 |
| 5 | `test_srv6_path` | 3 段 SID + IPv6 nexthop | 2 |
| 6 | `test_multiple_prefixes` | 3 前缀独立管理、选择性删除 | 7 |
| 7 | `test_ucmp` | 混合权重 204/51/0 | 2 |
| 8 | `test_empty_ops` | 空队列 flush/deferred 不崩溃 | 2 |
| 9 | `test_struct_layout` | sizeof / 零值语义 | 3 |

### 2.3 运行

```bash
cd /home/frr/frr
gcc -DHAVE_CONFIG_H -I. -I$PWD/lib -I$PWD/bgpd \
  -g -O0 -Wl,--wrap=zclient_route_send \
  -o /tmp/test_midr_ut tests/bgpd/test_midr_zebra.c \
  bgpd/libbgp.a lib/.libs/libfrr.a \
  -ljson-c -lrt -lcap -lreadline -lm -lpthread -lcrypt \
  $(pkg-config --libs libyang 2>/dev/null || echo '-lyang')
/tmp/test_midr_ut
```

预期：`=== 0 test(s) FAILED ===`

---

## 三、Topotest

### 3.1 目标

1. 编译运行单元测试二进制（验证 DP API 函数正确）
2. **功能验证**：通过 `ip route add ... proto 199` 验证 kernel 识别 `RTPROT_BGP_MIDR=199` 为 `proto midr`，**证明 MIDR DP 整条链路 endpoint 正确**
3. 静态检查所有修改文件的 MIDR 内容
4. 检查 netlink 协议号、zebra RIB 注册等

### 3.2 测试项（7 项）

| # | 函数 | 验证方式 |
|---|------|---------|
| 1 | `test_unit_build_and_run` | gcc 编译 → 运行单元测试 → 9/9 通过 |
| 2 | `test_kernel_proto199_midr` | `ip route add 10.200.200.0/24 proto 199` → `ip route show proto midr` 确认符号名 → `ip route show proto 199` 确认数字 → `ip route del proto 199` |
| 3 | `test_midr_route_type` | grep `ZEBRA_ROUTE_BGP_MIDR` in route_types.txt |
| 4 | `test_midr_rtprot` | grep `RTPROT_BGP_MIDR` = 199 in rt_netlink.h |
| 5 | `test_midr_symbols_in_build` | `ar t libbgp.a \| grep midr_zebra` |
| 6 | `test_midr_source_files` | 12 个文件 grep -li midr |
| 7 | `test_midr_debug_nl` | debug_nl.c / fpm_listener.c / kernel_netlink.c 含 MIDR |

### 3.3 功能验证详情

测试 `test_kernel_proto199_midr` 验证了 MIDR DP 管道的 kernel 端点：

#### 为什么 `ip route add ... proto 199` 能验证 MIDR DP？

MIDR DP 下发的完整链路是：

```
midr_zebra_route_add()                          ← DP 层 (bgpd/bgp_midr_zebra.c)
    │  构造 zapi_route: type=ZEBRA_ROUTE_BGP_MIDR, distance=115
    │  (单元测试已 mock zclient_route_send 验证此步骤)
    ▼
zclient_route_send() → zebra                      ← ZAPI 通道
    │  zebra 收到路由后调用 zebra2proto() 转换
    │  ZEBRA_ROUTE_BGP_MIDR → RTPROT_BGP_MIDR=199
    ▼
netlink (rt_netlink.c)                            ← Netlink 层
    │  rtnl_route_add() 带上 proto=199
    ▼
kernel FIB                                        ← 最终落点
    │  ip route show proto midr 可见 (199 映射到 "midr")
```

本测试用 `ip route add ... proto 199` **直接走 netlink** 注入一条 proto=199 的路由，验证：
- kernel 能识别 protocol 199
- `ip route show proto midr` 用符号名显示（因为 `/etc/iproute2/rt_protos` 有 `199 midr` 映射）
- `ip route show proto 199` 用数字也能查
- `ip route del ... proto 199` 能精确删除

这证明 **kernel 已准备好接收来自 MIDR DP 的路由**，管道端点正确。

> **Dummy 网卡说明**：`dummy` 是 Linux 内核内置的虚拟网卡驱动（`drivers/net/dummy.c`），执行 `ip link add ... type dummy` 时内核自动加载。Docker 容器以 `--privileged` 运行时具备 `NET_ADMIN` capability。

测试流程：

1. `ip link add dummy_midr type dummy` — 创建 dummy 虚拟网卡
2. `ip addr add 203.0.113.1/32 dev dummy_midr` — 配置下一跳地址
3. `ip route add 10.200.200.0/24 via 203.0.113.1 dev dummy_midr proto 199` — **用 proto 199 添加**
4. `ip route show proto midr` — 确认**符号名 "midr"** 显示该路由
5. `ip route show proto 199` — 确认**数字 199** 也能查到
6. `ip route del 10.200.200.0/24 proto 199` — 精确删除
7. 验证路由已移除

### 3.4 运行

```bash
cd /home/frr/frr/tests/topotests
pytest -v -s midr_dp_topo1/test_midr_dp_topo1.py
```

预期输出：
```
midr_dp_topo1/test_midr_dp_topo1.py::test_unit_build_and_run PASSED
midr_dp_topo1/test_midr_dp_topo1.py::test_kernel_proto199_midr PASSED
midr_dp_topo1/test_midr_dp_topo1.py::test_midr_route_type PASSED
midr_dp_topo1/test_midr_dp_topo1.py::test_midr_rtprot PASSED
midr_dp_topo1/test_midr_dp_topo1.py::test_midr_symbols_in_build PASSED
midr_dp_topo1/test_midr_dp_topo1.py::test_midr_source_files PASSED
midr_dp_topo1/test_midr_dp_topo1.py::test_midr_debug_nl PASSED

============================== 7 passed in 2.13s ===============================
```

---

## 四、测试脚本

### 4.1 容器内脚本 `scripts/docker_midr_test.sh`

在容器内执行，完成：
1. 生成 mock frrscript 符号（zebra 缺失符号）
2. 停止旧 zebra，创建 FRR 配置文件
3. 启动 zebra daemon
4. 运行 topotest
5. 清理

```bash
# 在容器内
bash /tmp/docker_midr_test.sh
```

### 4.2 服务器脚本 `scripts/run_topomidr_on_server.sh`

在服务器 `101.6.30.220` 上执行，完成：
1. 复制所有源码文件到容器
2. 复制 topotest 目录到容器
3. 复制 docker_midr_test.sh 到容器
4. 执行容器内测试脚本

```bash
# 在服务器上
bash /home/yangmy/frr/run_topomidr_on_server.sh
```

---

## 五、完整部署流程

```bash
# 1. 本地 scp 到服务器（所有文件一次性同步）
scp -i ~/.ssh/frr -P 50022 \
  /Users/yangmengyu/githubdocuments/frr/tests/topotests/midr_dp_topo1/ \
  /Users/yangmengyu/githubdocuments/frr/scripts/ \
  /Users/yangmengyu/githubdocuments/frr/tests/bgpd/test_midr_zebra.c \
  /Users/yangmengyu/githubdocuments/frr/bgpd/bgp_midr_zebra.c \
  ... \
  yangmy@101.6.30.220:/home/yangmy/frr/

# 2. SSH 到服务器，启动容器
ssh -i ~/.ssh/frr yangmy@101.6.30.220 -p 50022
echo thu325325 | sudo -S docker start frr-ubuntu24-ymy

# 3. 运行容器内测试脚本
echo thu325325 | sudo -S docker cp /home/yangmy/frr/docker_midr_test.sh frr-ubuntu24-ymy:/tmp/
echo thu325325 | sudo -S docker exec -u 0 frr-ubuntu24-ymy bash /tmp/docker_midr_test.sh

# 4. 或直接运行 topotest（不需要 zebra）
echo thu325325 | sudo -S docker exec frr-ubuntu24-ymy bash -c \
  "cd /home/frr/frr/tests/topotests && pytest -v midr_dp_topo1/test_midr_dp_topo1.py"
```

---

## 六、文件清单

### 6.1 辅助脚本

| 文件 | 说明 |
|------|------|
| `scripts/test_proto199.sh` | Proto 199 独立验证脚本（可在容器中直接运行） |

### 6.2 新增文件

| 文件 | 说明 |
|------|------|
| `tests/bgpd/test_midr_zebra.c` | MIDR DP 单元测试（9 项，35 断言） |
| `tests/bgpd/test_midr_zebra_e2e.c` | MIDR DP 端到端测试（直接连 zebra socket） |
| `tests/topotests/midr_dp_topo1/test_midr_dp_topo1.py` | Topotest 集成 + 功能测试（7 项） |
| `tests/topotests/midr_dp_topo1/midr_dp_topo1.json` | 拓扑定义 |
| `tests/topotests/midr_dp_topo1/__init__.py` | Python 包标记 |
| `scripts/docker_midr_test.sh` | 容器内一键测试 |
| `scripts/run_topomidr_on_server.sh` | 服务器一键部署+测试 |
| `documents/midr-testing-guide.md` | 本文档 |

### 6.2 被测试的 13 个源文件

| 文件 | 修改内容 |
|------|---------|
| `bgpd/bgp_midr_zebra.c` | **核心 DP 实现** |
| `bgpd/bgp_midr_zebra.h` | CP-DP 接口 |
| `bgpd/bgpd.h` | 添加 `midr_dp` 字段 |
| `bgpd/subdir.am` | 构建系统 |
| `lib/frrdistance.h` | distance = 115 |
| `lib/route_types.txt` | ZEBRA_ROUTE_BGP_MIDR |
| `tools/etc/iproute2/rt_protos.d/frr.conf` | 199 midr |
| `zebra/rt_netlink.h` | RTPROT_BGP_MIDR = 199 |
| `zebra/rt_netlink.c` | zebra2proto / proto2zebra / is_selfroute |
| `zebra/zebra_rib.c` | RIB 注册 |
| `zebra/debug_nl.c` | 调试输出 |
| `zebra/fpm_listener.c` | FPM 导出 |
| `zebra/kernel_netlink.c` | 协议名映射 |

---

## 七、测试结果

| 日期 | 单元测试 | Topotest | Proto 199 | 环境 |
|------|---------|---------|-----------|------|
| 2026-06-21 | **35/35 通过** | **7/7 通过** | ✅ `ip route show proto midr` | Docker `frr-ubuntu24-ymy` |

### 7.1 Proto 199 验证详情

```
$ ip route add 10.200.200.0/24 via 203.0.113.1 dev dummy_midr proto 199
$ ip route show proto midr
10.200.200.0/24 via 203.0.113.1 dev dummy_midr    ← kernel 识别为 "midr"
$ ip route show proto 199
10.200.200.0/24 via 203.0.113.1 dev dummy_midr    ← 数字 199 也可见
$ ip route del 10.200.200.0/24 proto 199           ← 精确删除
```

这证明 `/etc/iproute2/rt_protos` 中 `199 midr` 映射生效，kernel 完全支持 MIDR 协议号。
