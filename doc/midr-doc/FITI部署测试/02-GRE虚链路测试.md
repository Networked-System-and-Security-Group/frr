# 02 GRE 虚链路独立实验部署与测试

本文用于独立完成 Fudan-Buaa 两节点的 MIDR 数据面实验：建立 Session，自动创建 GRE 虚链路，验证 overlay 地址和双向业务 Ping，并验证删除接口后的自动恢复。本文依赖 00 文档定义的发布包、systemd 服务、持久配置和回滚目录，但不要求先执行 01；需要控制面单独验收时可参考 01。

## 1. 测试拓扑和边界

| 节点 | node-id | transport-address | 预期 overlay IPv4 |
|---|---:|---|---|
| Fudan | 101 | `240a:a00f:100:1::2` | `198.18.101.1` |
| Buaa | 102 | `240a:a00f:200:1::2` | `198.18.102.1` |

正式部署由 `midr-zebra.service`、`midr-bgpd.service`（边缘节点）和 `midrd.service` 管理。Session、Link 和 GRE 接口由配置和 MIDR 状态自动产生，不手工创建 `mgre-*` 接口，不在服务参数中写入固定 `--peer`、`--link` 或 `--link-addr`。

本文的必选通过条件是：

1. 两端 `midrd`、匹配版本 `zebra` 和必要的 `bgpd` active；
2. `show midr virtual-links` 有远端虚链路，状态为 `READY` 且 `overlay reachable`；
3. 两端 overlay IPv4 指定源地址 Ping 双向成功；
4. 删除其中一端虚接口后，MIDR 能重新创建接口并恢复 `READY` 和 Ping。

`proto 199` 只标记由 MIDR/FIB Provider 安装的业务 Prefix。未输入业务 Prefix 时，`ip route show proto 199` 为空是正常现象，不能据此否定 GRE 虚链路；Prefix/FIB 扩展验收见第 8 节。

## 2. 发布和安装

先在 90 上完成 00 文档的发布包构建、SSH 公钥和 underlay 检查，然后执行：

```bash
cd /home/guest/yhy/frr
export RELEASE_SRC="/tmp/midr-$RELEASE_ID"
test -f "$RELEASE_SRC/SHA256SUMS"

bash midr-test/fiti-deploy/deploy-node-from-90.sh \
  --node MIDR_fudan --mode check
bash midr-test/fiti-deploy/deploy-node-from-90.sh \
  --node MIDR_buaa --mode check

bash midr-test/fiti-deploy/deploy-node-from-90.sh \
  --node MIDR_fudan --mode apply
bash midr-test/fiti-deploy/deploy-node-from-90.sh \
  --node MIDR_buaa --mode apply
```


在完成两台节点的 `apply` 后，在 90 上写入本实验专用的两节点持久配置。该配置用于独立实验，与 03 的自动加入配置分别使用。

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

`systemctl restart midrd` 自动加载这份文件；90 的正式 `node-configs/` 保持 12 节点自动发现配置。执行 03 的 `apply` 会重新安装正式配置。


## 3. 控制面和虚链路检查

在两端分别执行：

```bash
systemctl is-active midr-zebra.service midrd.service
systemctl is-active midr-bgpd.service       # 两台节点当前均为边缘节点
test -S /run/midr/zserv.api
test -S /run/midr/midrd.vty
ss -tnp | grep ':5871'

export MIDR_DIR=/opt/midr-current
export LD_LIBRARY_PATH="$MIDR_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
"$MIDR_DIR/vtysh" --vty_socket /run/midr -d midrd \
  -c 'show midr neighbors' \
  -c 'show midr links' \
  -c 'show midr virtual-links'
```

预期 `neighbors` 为 `Established`，`virtual-links` 中有一个指向远端 node-id 的条目，显示 `READY`、`overlay reachable`，并有 `probe seq` 和 `unanswered 0` 或持续为 0 的探测结果。端口和日志证据：

```bash
grep -E 'established remote=|sync type=|closed|failed|error=' \
  /var/log/midr/*-midrd.log | tail -n 100
journalctl -u midrd.service -n 100 --no-pager
```

## 4. 查找自动创建的虚接口

接口名包含按节点身份计算的后缀，不能写死为某个 ifindex 或固定名称。每台节点执行：

```bash
VLINK=$(ip -o link show | awk -F': ' '$2 ~ /^mgre-/{print $2; exit}')
test -n "$VLINK"
printf 'virtual interface: %s\n' "$VLINK"
ip -d link show "$VLINK"
ip link show up
ip -6 addr show dev "$VLINK"
ip -4 addr show dev "$VLINK"
```

`ip -d link show` 应显示 GRE6 的外层本地和远端 transport-address；`ip -4 addr show` 应能看到本节点 overlay IPv4。接口由 midrd 通过匹配 Zebra/ZAPI 管理，不要执行 `ip link add` 代替 MIDR 控制面。

## 5. 双向 overlay Ping

Fudan 执行：

```bash
ping -4 -c 3 -I 198.18.101.1 198.18.102.1
```

Buaa 执行：

```bash
ping -4 -c 3 -I 198.18.102.1 198.18.101.1
```

三次请求均收到回复且丢包率为 0，表示业务报文已经使用 overlay 地址通过 GRE 虚链路转发。可用 `ip route get` 固定检查实际出口：

```bash
# Fudan
ip -4 route get 198.18.102.1 from 198.18.101.1

# Buaa
ip -4 route get 198.18.101.1 from 198.18.102.1
```

输出应显示 `dev mgre-*`，下一跳通常是 overlay 链路的 link-local 地址。

## 6. 虚接口删除与自动恢复

先记录基线：

```bash
export MIDR_DIR=/opt/midr-current
export LD_LIBRARY_PATH="$MIDR_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
"$MIDR_DIR/vtysh" --vty_socket /run/midr -d midrd \
  -c 'show midr virtual-links' \
  -c 'show midr links'
```

在其中一台节点删除自动创建的接口：

```bash
VLINK=$(ip -o link show | awk -F': ' '$2 ~ /^mgre-/{print $2; exit}')
test -n "$VLINK"
ip link del "$VLINK"
```

等待 5 至 30 秒后重复第 3 至第 5 节。通过条件为：新的 `mgre-*` 接口出现，`show midr virtual-links` 恢复 `READY` 和 `overlay reachable`，探测未应答数恢复为 0，双向 overlay Ping 恢复。接口删除测试验证的是数据面故障后的控制面重建能力，不是手工配置接口的方法。

## 7. midrd 重启后的恢复

```bash
systemctl restart midrd.service
systemctl is-active midrd.service
test -S /run/midr/midrd.vty
export MIDR_DIR=/opt/midr-current
export LD_LIBRARY_PATH="$MIDR_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
"$MIDR_DIR/vtysh" --vty_socket /run/midr -d midrd \
  -c 'show midr neighbors' \
  -c 'show midr virtual-links'
```

重启后不重新输入 `midr session`。服务重新读取 `/etc/midr/node.commands`，并继续使用 `/var/lib/midr/<node-id>.seq`。若虚链路未恢复，按顺序检查 ZAPI socket、`systemctl status`、underlay `ip -6 route get`、TCP 5871 和 `journalctl -u midrd.service`。

## 8. Prefix/FIB 扩展验收（可选）

只有部署了 Prefix Provider 或明确配置了业务测试前缀，才执行本节。先确认前缀的归属、源地址和 underlay 不冲突，再在两端检查：

```bash
ip -4 route show proto 199
ip -6 route show proto 199
```

对已批准的远端业务前缀执行：

```bash
ip -4 route get <远端IPv4前缀地址> from <本端IPv4源地址>
ip -6 route get <远端IPv6前缀地址> from <本端IPv6源地址>
```

通过条件是路由实际选择 `mgre-*`，并且双方业务地址的 Ping/业务连接成功。`proto 199` 为空而虚链路和 overlay Ping 正常，只说明当前实验没有安装业务 Prefix，不表示 GRE 失败。

## 9. 停止、回滚和清理

结束实验时使用 systemd：

```bash
systemctl stop midrd.service
systemctl stop midr-bgpd.service       # 边缘节点
systemctl stop midr-zebra.service
```

需要恢复旧版本时，使用 00 文档定义的 `/var/lib/midr/deploy-backups/` 和完整发布目录，同时恢复匹配版本的 `midrd`、`zebra`、`bgpd` 与 `lib/`。不要只替换单个二进制，也不要删除 sequence 文件。
