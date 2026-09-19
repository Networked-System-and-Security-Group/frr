# 测试 01：bgpd 编译验证

## 测试目的

验证 `bgp_midr_zebra.{c,h}` 代码可以正常编译。

## 测试代码文件

| 文件 | 说明 |
|------|------|
| `bgpd/bgp_midr_zebra.c` | MIDR DP 核心实现 (866 行) |
| `bgpd/bgp_midr_zebra.h` | MIDR DP 公共头文件 (230 行) |
| `bgpd/subdir.am` | 构建系统文件，声明 `bgp_midr_zebra.c` 为编译源 |

## 前置条件

- 已进入 Docker 容器: `sudo docker exec -it frr-ubuntu24-ymy bash`
- FRR autotools 构建系统已配置（`./configure` 已执行）
- 源文件已复制到 `/home/frr/frr/bgpd/`

## 执行环境

```bash
# 从本地 mac SSH 到服务器
ssh -i ~/.ssh/frr yangmy@101.6.30.220 -p 50022

# 进入容器
sudo docker exec -it frr-ubuntu24-ymy bash
echo "thu325325" | sudo -S chown -R frr:frr /home/frr/frr/bgpd/
cd /home/frr/frr
```

## 编译命令

```bash
# 清理并编译 bgpd
rm -rf bgpd/.libs/bgpd bgpd/.libs/lt-bgpd 2>/dev/null || true
make bgpd/bgpd -j$(nproc)
```

## 验证步骤

| 步骤 | 命令 | 预期结果 |
|:---:|------|---------|
| 1 | `make bgpd/bgpd -j$(nproc) 2>&1` | `CC bgpd/bgp_midr_zebra.o` → `AR bgpd/libbgp.a` → `CCLD bgpd/bgpd` |
| 2 | `ls -la bgpd/.libs/bgpd` | 二进制文件存在，大小约 14MB |
| 3 | `ls -la bgpd/bgp_midr_zebra.o` | 目标文件存在，约 167KB |
| 4 | `nm bgpd/bgp_midr_zebra.o \| grep ' T '` | 6 个 MIDR DP API 符号 |
| 5 | `ar t bgpd/libbgp.a \| grep midr_zebra` | `bgp_midr_zebra.o` 在静态库中 |
| 6 | `nm bgpd/libbgp.a \| grep midr_zebra` | MIDR API 符号在静态库中可以找到 |

## 实际编译输出

```
$ make bgpd/bgpd -j$(nproc)
  CC       bgpd/bgp_midr_zebra.o          ← 新代码编译通过
  AR       bgpd/libbgp.a                  ← 静态库打包
  CCLD     bgpd/bgpd                      ← 二进制链接成功
Exit code: 0

$ ls -la bgpd/.libs/bgpd
-rwxr-xr-x 1 frr frr 14854792 Jul  8 07:20 bgpd/.libs/bgpd

$ ls -la bgpd/bgp_midr_zebra.o
-rw-r--r-- 1 frr frr 167688 Jul  8 07:20 bgpd/bgp_midr_zebra.o
```

## 符号检查结果

```
$ nm bgpd/bgp_midr_zebra.o | grep ' T '
0000000000000c00 T midr_zebra_fini
0000000000000b80 T midr_zebra_init
0000000000000cf0 T midr_zebra_route_add
0000000000000de0 T midr_zebra_route_del
0000000000000ee0 T midr_zebra_route_flush
0000000000000e70 T midr_zebra_route_update_deferred
```

全部 6 个公共 DP API 符号正确导出，编译零错误零警告。

## 结论

bgpd 编译成功。`bgp_midr_zebra.o` 正确生成并链接到 libbgp.a。

## 自动化脚本

```bash
# 从本地 mac 执行
scp -i ~/.ssh/frr -P 50022 tests/bgpd/scripts/test01_bgpd_build.sh yangmy@101.6.30.220:/tmp/
ssh -i ~/.ssh/frr yangmy@101.6.30.220 -p 50022 "bash /tmp/test01_bgpd_build.sh"
```

脚本路径: `tests/bgpd/scripts/test01_bgpd_build.sh`
