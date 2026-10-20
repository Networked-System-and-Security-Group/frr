# MIDR 发布与批量部署工具

本目录用于构建 MIDR 发布包，并把它按角色分阶段部署到一组 MIDR 节点。所有命令都在**管理机**上执行：管理机要有已配置好的 FRR 构建树，并能用 SSH 公钥登录全部节点。目标节点上的进程由 systemd 管理。

完整说明见 [MIDR 发布与部署手册](../../doc/midr-doc/deployment/MIDR发布与部署手册.md)。本文只给出最短流程和文件说明。

## 快速开始

以下命令在仓库根目录执行，使用默认 profile `profiles/fiti`（FITI 12 节点环境）。

```bash
# 1. 构建发布包：编译 midrd、zebra、bgpd、vtysh，组装到 /tmp/midr-<commit>-<UTC>
tools/midr-deploy/build-release.sh --tag mytag
export RELEASE_SRC=/tmp/midr-<上一步输出的目录名>

# 2. 管理机到各节点的公钥登录与 underlay 只读检查
tools/midr-deploy/install-ssh-keys.sh --shared-password
tools/midr-deploy/collect-underlay.sh
tools/midr-deploy/check-transport-matrix.sh

# 3. 三个阶段依次执行：先 check（不停服务），再 apply
for phase in bootstrap representatives members; do
    tools/midr-deploy/deploy-batch.sh --phase "$phase" --mode check
done
tools/midr-deploy/deploy-batch.sh --phase bootstrap --mode apply
tools/midr-deploy/deploy-batch.sh --phase representatives --mode apply
tools/midr-deploy/deploy-batch.sh --phase members --mode apply

# 4. 集中采集与验收
python3 tools/midr-deploy/collect-midr-state.py --output "midr-state-$(date -u +%Y%m%dT%H%M%SZ)"
```

换一个环境时，复制 `profiles/fiti` 为新目录并修改其中的文件，然后在每条命令后加 `--profile DIR`，或设置 `export MIDR_PROFILE=DIR`。

## 文件说明

| 路径 | 作用 |
|---|---|
| `build-release.sh` | 编译并组装发布包，生成 `RELEASE_ID`、`GIT_COMMIT`、`SHA256SUMS`，并做依赖与校验和自检 |
| `deploy-node.sh` | 检查（check）或安装（apply）一台节点 |
| `deploy-batch.sh` | 按 bootstrap → representatives → members 阶段调用 `deploy-node.sh`；apply 时每台非 bootstrap 节点入群后才处理下一台 |
| `install-node-local.sh` | 由 `deploy-node.sh` 复制到目标节点执行的一次性安装器：校验、备份、切换、启动，失败时回滚 |
| `collect-midr-state.py` | 并行采集各节点的服务、MIDR 状态和 FIB，生成 `summary.tsv` 和原始 JSON |
| `collect-underlay.sh` | 只读采集 underlay 地址、路由和现有 FRR 状态 |
| `check-transport-matrix.sh` | 按 TCP 5871 查询每台节点到全部 locator 的本机路由 |
| `install-ssh-keys.sh` | 把管理机公钥装到各节点，并逐台验证公钥登录 |
| `lib/profile.sh` | 各脚本共用的 profile 读取函数 |
| `node/` | 安装到目标节点的文件：systemd 单元、启动脚本、`setup-midr-underlay.py`、Prefix Provider、`midrd-vty.py`、日志轮转和 needrestart 配置 |
| `profiles/fiti/` | 默认 profile，见其中的 [README](profiles/fiti/README.md) |
| `tests/` | Prefix Provider 策略的单元测试：`python3 tools/midr-deploy/tests/test_bgp_prefix_provider.py -v` |

## Profile 格式

一个 profile 目录描述一个部署环境：

```text
nodes.csv            每行一台节点
node-configs/
  <name>.env         安装为目标机 /etc/midr/node.env
  <name>.commands    安装为目标机 /etc/midr/node.commands
  <name>.bgpd.conf   可选，边缘节点的 bgpd 配置
profile.env          可选，环境级默认值
```

`nodes.csv` 按表头名取列，列顺序不限。表头之后以 `#` 开头的行和空行会被忽略。

| 列 | 含义 |
|---|---|
| `name` | 节点名，也是 `node-configs/` 中的文件名前缀 |
| `ssh_host`、`ssh_port`、`ssh_user` | 管理机登录该节点的 SSH 入口 |
| `node_id` | MIDR 节点编号，全网唯一 |
| `locator` | transport-address |
| `gateway` | 本站 underlay 网关 |
| `edge_bgp` | `yes` 时运行 bgpd 和 Prefix Provider |
| `role` | `bootstrap`、`group-rep` 或 `member`，决定所属部署阶段 |
| `group` | 群号；bootstrap 为 `0`，普通成员为 `auto` |

`profile.env` 中可设置的变量：

| 变量 | 默认值 | 含义 |
|---|---|---|
| `MIDR_UNDERLAY_INTERFACE` | `ens5` | transport 出口接口，写入各节点的 `node.env` |
| `MIDR_PROTECTED_NETPLAN` | 空 | 安装前后必须保持不变的 `/etc/netplan/` 文件名，空格分隔 |
| `MIDR_SSH_EXTRA` | 空 | 追加给 ssh/scp 的选项，例如跳板机 `-o ProxyJump=...` |

## 发布包格式

```text
midr-<sha12>[-tag]-<UTC>/
  midrd  zebra  bgpd  vtysh        真实 ELF（不是 libtool 包装脚本）
  run-midrd.sh                     设置 LD_LIBRARY_PATH 后启动 midrd
  midrd-vty.py                     midrd 的 Python VTY 客户端
  midr-bgp-prefix-provider.py      边缘节点的 BGP Prefix Provider
  lib/libfrr.so.0  lib/libmlag_pb.so.0  lib/libyang.so.3
  RELEASE_ID  GIT_COMMIT  SHA256SUMS
```

目录名必须以 `midr-` 开头。目标机把它安装到 `/opt/midr-releases/<目录名>`，再把 `/opt/midr-current` 指向它。
