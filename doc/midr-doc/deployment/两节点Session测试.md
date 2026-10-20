# 两节点 Session 测试

本文用于独立验证两台 FITI MIDR 节点的 underlay 可达性、TCP 控制面、MIDR Session、Node directory、基础 Link 和 SPF 控制面状态。GRE 虚链路、MIDR FIB 和业务 Prefix 属于[两节点 GRE 虚链路测试](两节点GRE虚链路测试.md)，不作为本文的通过条件。

## 1. 测试对象和预期

| 节点 | node-id | transport-address | 默认网关 | 固定配置 |
|---|---:|---|---|---|
| Fudan | 101 | `240a:a00f:100:1::2` | `240a:a00f:100:1::1` | 本节两节点 node.commands |
| Buaa | 102 | `240a:a00f:200:1::2` | `240a:a00f:200:1::1` | 本节两节点 node.commands |

两台节点使用同一发布包和正式 systemd 服务。Session 配置写入 `/etc/midr/node.commands`，由 `midrd.service` 在每次启动后自动加载；不在实验步骤中使用 `nohup`、`--peer`、`--link` 或 `--link-addr`。

Session 通过条件是：两端服务 active，目标节点可通过 underlay 到达，TCP 5871 为 `ESTAB`，日志出现 `established remote=` 和 `sync type=6`，`show midr neighbors` 显示远端为 `Established`。`show midr links` 暂时为空不单独判为 Session 失败；Link/GRE 在 GRE 虚链路测试中验收。

## 2. 准备和安装

先按[发布与部署手册](MIDR发布与部署手册.md)构建发布包，并完成 SSH 公钥和 underlay 检查。发布包目录必须是实际存在的目录，例如 `/tmp/midr-5228cff9b3be-20261001T153820Z`；不要照抄不存在的目录名。

```bash
cd <仓库根目录>
export RELEASE_SRC=/tmp/midr-<发布包目录名>
test -f "$RELEASE_SRC/SHA256SUMS"

tools/midr-deploy/deploy-node.sh \
  --node MIDR_fudan --mode check
tools/midr-deploy/deploy-node.sh \
  --node MIDR_buaa --mode check

tools/midr-deploy/deploy-node.sh \
  --node MIDR_fudan --mode apply
tools/midr-deploy/deploy-node.sh \
  --node MIDR_buaa --mode apply
```

`check` 只检查发布包、地址、默认路由、配置和动态库；`apply` 会在目标节点备份旧状态，停止旧 FRR/MIDR，安装匹配的 zebra/bgpd/midrd，写入持久配置并启动 systemd 服务。边缘节点按其 `.env` 中的 `MIDR_EDGE_BGP=yes` 启动 `midr-bgpd.service`；服务清单和配置位置见[手册](MIDR发布与部署手册.md)。


在完成两台节点的 `apply` 后，在管理机上写入本实验专用的两节点持久配置。该配置用于独立实验，与 12 节点的自动加入配置分别使用。

```bash
ssh -p 15503 root@118.229.43.234 'bash -s' <<'FUDAN'
set -eu
cp -a /etc/midr/node.commands /etc/midr/node.commands.before-two-node
cat >/etc/midr/node.commands <<'CONFIG'
configure terminal
midr transport-address 240a:a00f:100:1::2
midr group-id 1
midr session 240a:a00f:200:1::2 remote-as 65102
end
CONFIG
sed -i 's/^MIDR_GROUP=.*/MIDR_GROUP=1/' /etc/midr/node.env
chmod 600 /etc/midr/node.commands
systemctl restart midrd.service
FUDAN

ssh -p 15003 root@118.229.43.234 'bash -s' <<'BUAA'
set -eu
cp -a /etc/midr/node.commands /etc/midr/node.commands.before-two-node
cat >/etc/midr/node.commands <<'CONFIG'
configure terminal
midr transport-address 240a:a00f:200:1::2
midr group-id 1
midr session 240a:a00f:100:1::2 remote-as 65101
end
CONFIG
sed -i 's/^MIDR_GROUP=.*/MIDR_GROUP=1/' /etc/midr/node.env
chmod 600 /etc/midr/node.commands
systemctl restart midrd.service
BUAA
```

`systemctl restart midrd` 自动加载这份文件；profile 中的 `node-configs/` 仍是 12 节点自动发现配置，之后按手册批量 apply 会重新安装正式配置。

## 3. 目标节点基础检查

在 Fudan 和 Buaa 分别执行：

```bash
systemctl is-active midr-zebra.service midrd.service
systemctl is-active midr-bgpd.service       # 本实验两台均为边缘节点
test -S /run/midr/zserv.api
test -S /run/midr/midrd.vty
ss -ltnp | grep ':5871'
readlink -f /opt/midr-current
```

从 Fudan 检查 Buaa，从 Buaa 检查 Fudan：

```bash
ip -6 route get 240a:a00f:200:1::2 from 240a:a00f:100:1::2
ip -6 route get 240a:a00f:100:1::2 from 240a:a00f:200:1::2
```

`route get` 必须给出可用出口；ICMP 被平台过滤时不以 ping 作为唯一门禁。

## 4. VTY 控制面验收

在每台节点使用当前发布包的 `vtysh` 查询 MIDR；统一交互入口和 Zebra/BGP 查询见[手册](MIDR发布与部署手册.md)第 10.2 节：

```bash
export MIDR_DIR=/opt/midr-current
export LD_LIBRARY_PATH="$MIDR_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
"$MIDR_DIR/vtysh" --vty_socket /run/midr -d midrd \
  -c 'show midr self' \
  -c 'show midr join' \
  -c 'show midr neighbors' \
  -c 'show midr nodes' \
  -c 'show midr links' \
  -c 'show midr spf'
```

预期结果：

- `show midr self` 的 locator 和 group-id 与本节点 `.env`/`.commands` 一致；
- `show midr neighbors` 有对端地址，状态为 `Established`；
- `show midr nodes` 至少包含本节点和已同步的对端节点；
- `show midr spf` 能显示 SPF generation，路由数取决于是否已提供 Prefix；
- `show midr links` 在没有完成 Link/数据面条件时可以为空。

## 5. 日志和 TCP 证据

```bash
ss -tnp | grep ':5871'
grep -E 'established remote=|sync type=|closed|failed|error=' \
  /var/log/midr/*-midrd.log | tail -n 100
journalctl -u midrd.service -n 100 --no-pager
```

两端均应看到到对端的 `ESTAB`、`established remote=` 以及同步阶段（通常包含 `sync type=6`）。若没有建立，按顺序检查：`systemctl is-active`、`test -S /run/midr/zserv.api`、`ip -6 route get`、监听端口、防火墙和日志；不要先修改 Session 地址。

## 6. 重启和持久配置

```bash
systemctl restart midrd.service
systemctl is-active midrd.service
test -S /run/midr/midrd.vty
export MIDR_DIR=/opt/midr-current
export LD_LIBRARY_PATH="$MIDR_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
"$MIDR_DIR/vtysh" --vty_socket /run/midr -d midrd \
  -c 'show midr neighbors'
```

重启后服务会重新读取 `/etc/midr/node.commands`，因此不需要再次手工输入 Session。`/run/midr/midrd.vty` 是运行时 socket，重启时会重新创建；`/var/lib/midr/<node-id>.seq` 是持久序列号文件，不得删除。

## 7. 停止和清理

```bash
systemctl stop midrd.service
systemctl stop midr-bgpd.service       # 边缘节点
systemctl stop midr-zebra.service
```

如果只是结束本实验，保留 `/etc/midr/node.commands`、sequence 文件和发布目录，下一次可直接 `systemctl start` 或按[手册](MIDR发布与部署手册.md)的升级流程处理。只有确认要切回原系统 FRR 时，才按手册的回滚步骤恢复 `frr.service`。
