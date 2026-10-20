# MIDR 发布与部署手册

本文说明如何从源码构建 MIDR 发布包，并用 `tools/midr-deploy/` 把它分阶段部署到一组节点，以及部署后的运维、验收和回滚。文中的具体地址、端口和节点名取自默认的 FITI profile，仅作示例；换成其他环境时，改用自己的 profile 即可，命令不变。

系统架构见 [MIDR 系统总体设计](../MIDR系统总体设计.md)，FITI 环境的节点表见 [FITI profile](../../../tools/midr-deploy/profiles/fiti/README.md)。

## 1. 适用范围与版本边界

- MIDR 节点运行本仓库构建的 FRR 10.7 `zebra`、`bgpd`、`vtysh` 和独立的 `midrd`，四者必须来自同一次构建。
- underlay 路由器（如 FITI 的 BGP-R、TEIN 路由器）继续使用自己的 FRR 版本，通过标准 BGP 与边缘 MIDR 互通。不要把其他版本的单个 `bgpd` 放进发布包，也不要让节点上的系统 FRR 与发布包的 zebra 同时管理同一张 Linux FIB。
- 目标节点的操作系统按 Ubuntu 22.04 验证。发布包自带 `libfrr`、`libmlag_pb` 和 `libyang`；其余依赖（libcap2、libjson-c5、libpcre2-8、libprotobuf-c1、libunwind8、liblzma5 等）应由系统提供，`check` 模式会用 ldd 检查。

## 2. 角色与进程

| 节点角色 | 常驻进程 | 说明 |
|---|---|---|
| 边缘节点（`edge_bgp=yes`） | zebra、bgpd、midrd、Prefix Provider | 与本站 underlay BGP 路由器交换前缀；Provider 把选中的 BGP 路由交给 midrd |
| 内部节点（`edge_bgp=no`） | zebra、midrd | 只从 MIDR 网络学习前缀 |
| bootstrap（`role=bootstrap`） | zebra、midrd | 群号 0，提供代表目录，不承担业务转发 |

`role` 列的取值 `bootstrap`、`group-rep`、`member` 是 MIDR 协议角色，同时决定节点属于哪个部署阶段。`group-rep` 是预置群的初始代表；`member` 自动选择或创建群，实际群数不受预置群数限制。

`vtysh` 和 `midrd-vty.py` 是客户端，不常驻。

服务启动顺序（systemd 依赖）：

```text
midr-zebra.service
  → midr-bgpd.service（仅边缘）
  → midrd.service：ExecStartPre 建立 transport 隔离，ExecStartPost 加载 node.commands
  → midr-bgp-prefix-provider.service（仅边缘）
```

## 3. 仓库与目录结构

```text
tools/midr-deploy/
  build-release.sh          构建发布包
  deploy-node.sh            单节点 check / apply
  deploy-batch.sh           分阶段批量 check / apply
  install-node-local.sh     在目标节点执行的安装器
  collect-midr-state.py     集中采集
  collect-underlay.sh  check-transport-matrix.sh  install-ssh-keys.sh
  lib/profile.sh            profile 读取
  node/                     安装到节点上的 systemd 单元、启动脚本等
  profiles/fiti/            默认 profile
```

目标节点上的布局：

```text
/opt/midr-releases/midr-<id>/     完整发布目录，安装后不再修改
/opt/midr-current                 指向当前发布目录的符号链接
/etc/midr/node.env                节点身份与服务参数
/etc/midr/node.commands           持久 VTY 配置
/etc/midr/zebra.conf  bgpd.conf   匹配版本的 zebra / bgpd 配置（bgpd 仅边缘）
/etc/midr/logrotate.conf          MIDR 日志轮转配置
/usr/local/libexec/midr/          启动与配置加载脚本
/var/lib/midr/<node-id>.seq       MIDR 序号持久文件，不能删除
/var/lib/midr/deploy-backups/     每次 apply 前的备份
/var/log/midr/                    daemon 日志
/run/midr/                        PID、VTY、ZAPI、Prefix socket
```

## 4. 构建环境

管理机需要一个已配置好的 FRR 构建树，即本仓库的 checkout。依赖按 FRR 官方的 [Ubuntu 22.04 构建说明](../../developer/building-frr-for-ubuntu2204.rst) 安装，其中 libyang 需要 3.x 版本。

当前发布使用的 configure 参数：

```bash
./bootstrap.sh
./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var \
    --sbindir=/usr/lib/frr --enable-vtysh \
    --enable-user=frr --enable-group=frr
```

配置一次即可，之后 `build-release.sh` 会增量编译。

## 5. 构建发布包

```bash
tools/midr-deploy/build-release.sh [--build-dir DIR] [--out-dir DIR] [--tag NAME]
```

| 参数 | 默认值 | 含义 |
|---|---|---|
| `--build-dir` | 脚本所在的仓库 | 已配置的 FRR 构建树 |
| `--out-dir` | `/tmp` | 发布目录的上级目录 |
| `--tag` | 无 | 附加到发布 ID 的短标签，如 `g1rejoin` |
| `--no-make` | — | 不编译，直接打包现有构建结果 |
| `--allow-dirty` | — | 允许构建树中有未提交的已跟踪修改，发布 ID 带 `-dirty` |

脚本依次完成以下工作：

1. 检查构建树没有未提交的已跟踪修改（未跟踪文件不影响）。
2. `make midrd/midrd zebra/zebra bgpd/bgpd vtysh/vtysh`。
3. 从各目录的 `.libs/` 复制真实 ELF（顶层的同名文件是 libtool 包装脚本，不能用），复制 `node/` 下的 `midrd-vty.py` 和 Prefix Provider，生成 `run-midrd.sh`。
4. 复制 `libfrr.so.0`、`libmlag_pb.so.0`，以及 `MIDR_RELEASE_EXTRA_LIBS` 列出的系统库（默认 `libyang.so.3`，路径用 ldd 解析）到 `lib/`。
5. 写 `RELEASE_ID`、`GIT_COMMIT`、`SHA256SUMS`。
6. 自检：用 `LD_LIBRARY_PATH=lib/` 执行 ldd，确认没有缺失的库，且打包的库都从 `lib/` 解析；再校验 `SHA256SUMS`。

输出示例：

```text
release=/tmp/midr-02e090534468-g1rejoin-20261020T082421Z
```

发布 ID 的格式为 `<提交前 12 位>[-tag][-dirty]-<UTC 时间>`。后续部署命令通过环境变量 `RELEASE_SRC` 使用这个目录：

```bash
export RELEASE_SRC=/tmp/midr-02e090534468-g1rejoin-20261020T082421Z
```

目录名必须以 `midr-` 开头。节点上的安装位置由目录名决定，所以 `node.env` 不需要随版本修改。

## 6. Profile 配置

一个 profile 是一个目录，描述一个部署环境。各脚本通过 `--profile DIR` 或环境变量 `MIDR_PROFILE` 选择 profile，都不指定时使用 `tools/midr-deploy/profiles/fiti`。新建环境时，复制 FITI profile 再修改。

### 6.1 nodes.csv

每行一台节点，按表头名取列：

```csv
name,ssh_host,ssh_port,ssh_user,node_id,locator,gateway,edge_bgp,role,group
MIDR_fudan,118.229.43.234,15503,root,101,240a:a00f:100:1::2,240a:a00f:100:1::1,yes,group-rep,2
MIDR-R3_wh,118.229.43.234,13008,root,108,240a:a00f:800:3::2,240a:a00f:800:3::1,no,bootstrap,0
MIDR_wlq,118.229.43.234,29502,root,103,240a:a00f:300:1::2,240a:a00f:300:1::1,yes,member,auto
```

| 列 | 含义 |
|---|---|
| `name` | 节点名，对应 `node-configs/<name>.*` |
| `ssh_host`、`ssh_port`、`ssh_user` | 管理机的 SSH 入口 |
| `node_id` | MIDR 节点编号，全网唯一且不再变更 |
| `locator` | transport-address，必须已配置在节点的 underlay 接口上 |
| `gateway` | 本站 underlay 网关 |
| `edge_bgp` | `yes` 或 `no` |
| `role` | `bootstrap`、`group-rep` 或 `member` |
| `group` | bootstrap 为 `0`，群代表为预置群号，普通成员为 `auto` |

以 `#` 开头的行和空行会被忽略，可用来临时排除某台节点。

### 6.2 node-configs/

**`<name>.env`**：安装为 `/etc/midr/node.env`，systemd 和启动脚本从中读取参数。

```sh
MIDR_RELEASE_DIR=/opt/midr-current
MIDR_NODE_NAME=MIDR_fudan
MIDR_NODE_ID=101
MIDR_LOCATOR=240a:a00f:100:1::2
MIDR_GATEWAY=240a:a00f:100:1::1
MIDR_EDGE_BGP=yes
MIDR_ROLE=group-rep
MIDR_GROUP=2
MIDR_CONFIG=/etc/midr/node.commands
MIDR_VTY_DIR=/run/midr
MIDR_ZSERV_PATH=/run/midr/zserv.api
MIDR_PREFIX_SOCKET=/run/midr/prefix.sock
MIDR_PREFIX_MODE=socket
```

部署时 `MIDR_RELEASE_DIR` 会被替换为本次发布目录，profile 中的 `MIDR_UNDERLAY_INTERFACE`、`MIDR_PROTECTED_NETPLAN` 也会写入。其他可选变量：

| 变量 | 含义 |
|---|---|
| `MIDR_PREFIX_MODE` | 边缘节点为 `socket`（接 Provider），其余为 `none` |
| `MIDR_PREFIX_MAX_AS_PATH_LENGTH` | Provider 导出到 MIDR 的最大 AS_PATH 长度，默认 1；不影响 bgpd 自身的路由表 |
| `MIDR_ROUTE_EXPORT` | `yes` 时把 MIDR 路由发布给本站网关，默认 `no`，见第 14 节 |
| `MIDR_ROUTE_TAG` | midrd 给安装路由打的 tag，默认 199；Provider 和出口配置使用同一个值 |
| `MIDR_PREFIX_EXCLUDE_FILE` | Provider 不导入的前缀列表，默认 `/etc/midr/locators.txt`（全部 locator 前缀） |

**`<name>.commands`**：安装为 `/etc/midr/node.commands`，midrd 每次启动后自动加载。

```text
configure terminal
midr transport-address 240a:a00f:100:1::2
midr group-id 2
midr role group-rep
midr bootstrap 240a:a00f:800:3::2 remote-as 65108 router-id 108.0.0.0
midr bootstrap 240a:a00f:810:3::2 remote-as 65111 router-id 111.0.0.0
end
```

- 普通成员不写 `group-id` 和 `role`，只写 `transport-address` 和 bootstrap 候选。
- bootstrap 写 `midr group-id 0`、`midr role bootstrap`，以及到其他 bootstrap 的条目。
- `remote-as` 是 MIDR overlay 内的编号，与站点 underlay 的 BGP AS 无关。
- 普通节点不需要为其他成员配置 Session，成员 Session 由自动发现建立。

**`<name>.bgpd.conf`**（可选）：边缘节点的 bgpd 配置。安装器按以下顺序取第一个存在的来源：profile 中的该文件、节点上的 `/etc/frr/bgpd.conf`、`/etc/frr/frr.conf`。边缘节点三者都没有时，安装失败。

### 6.3 profile.env

环境级默认值，所有脚本加载 profile 时读入：

| 变量 | 通用默认值 | FITI 取值 | 含义 |
|---|---|---|---|
| `MIDR_UNDERLAY_INTERFACE` | `ens5` | `ens5` | transport 出口接口 |
| `MIDR_PROTECTED_NETPLAN` | 空 | `60-ens4.yaml 61-l3.yaml` | 安装前后必须保持不变的 `/etc/netplan/` 文件 |
| `MIDR_SSH_EXTRA` | 空 | 空 | 追加给 ssh/scp 的选项，如 `-o ProxyJump=jump@host` |
| `MIDR_LOCATOR_PREFIX_LEN` | IPv6 为 64，IPv4 为 24 | 默认 | 由 locator 推出 locator 前缀时使用的长度 |

## 7. 部署前检查

### 7.1 SSH 公钥

管理机必须能用公钥（BatchMode）登录所有节点：

```bash
tools/midr-deploy/install-ssh-keys.sh --shared-password
```

脚本先逐台验证公钥登录，只有登录失败的节点才会提示输入密码并安装公钥。默认使用 `~/.ssh/id_ed25519.pub`，可用 `--key` 另行指定。

### 7.2 underlay

```bash
tools/midr-deploy/collect-underlay.sh
tools/midr-deploy/check-transport-matrix.sh
```

两个脚本都只读，不修改节点。进入部署前应满足：

- 所有节点都能登录；
- locator 已配置在 underlay 接口上，节点有经本站网关的默认路由；
- transport 矩阵中，每台节点到全部 locator 的 TCP 5871 路由查询都成功。

矩阵只检查本机路由表，端到端可达要在部署后由 Session 状态确认。部分站点过滤 ICMP，ping 不通不能单独说明不可达。

MIDR 的首跳依赖 underlay：节点从 bootstrap 拿到代表和成员的 locator 后，直接向这些地址建 TCP Session，bootstrap 不转发流量。所以 underlay 必须能到达所有可能的 locator，只到 bootstrap 可达是不够的。

## 8. 单节点部署

```bash
tools/midr-deploy/deploy-node.sh --node MIDR_fudan --mode check
tools/midr-deploy/deploy-node.sh --node MIDR_fudan --mode apply
```

**check** 在管理机上校验发布包（SHA256、ELF、ldd），把发布包和节点配置暂存到目标机 `/tmp`，由安装器做只读检查：节点配置与角色是否一致、locator 是否在 underlay 接口上、经本站网关的默认路由、受保护的 Netplan 文件是否存在、边缘节点的 BGP 配置来源、发布包校验和与动态库、目标机上同名发布目录是否一致，以及系统 FRR 配置中是否有需要先迁移的静态路由或 IGP。check 不停止任何服务。

**apply** 在 check 的基础上依次执行：

1. 备份 `/etc/frr`（如存在）、受保护的 Netplan 文件、`/etc/midr`、systemd 状态、进程和路由表，备份目录记在 `/var/lib/midr/last-deploy-backup`；
2. 停止旧的 midr-* 服务，停用系统 `frr.service`；
3. 安装发布目录、`/opt/midr-current`、`node.env`、`node.commands`、zebra/bgpd 配置和 systemd 单元；
4. 安装日志轮转定时器 `midr-logrotate.timer`；节点装有 needrestart 时，安装 `/etc/needrestart/conf.d/midr.conf`，避免系统自动升级后重启 midr 服务；
5. 按 zebra → bgpd（边缘）→ midrd → Provider（边缘）的顺序启动并验收。

任何一步失败，安装器自动恢复备份并重新启动原有服务。如果目标机上已有同名但内容不一致的发布目录，安装器会在停服务之前拒绝。

## 9. 分阶段批量部署

```bash
tools/midr-deploy/deploy-batch.sh --phase PHASE --mode check|apply \
    [--profile DIR] [--wait-joined SECS] [--continue]
```

阶段必须按以下顺序执行：

| 阶段 | 节点 | 说明 |
|---|---|---|
| `bootstrap` | `role=bootstrap` | 先上线，提供代表目录 |
| `representatives` | `role=group-rep` | 预置群代表 |
| `members` | `role=member` | 普通成员，自动入群 |

```bash
for phase in bootstrap representatives members; do
    tools/midr-deploy/deploy-batch.sh --phase "$phase" --mode check
done

tools/midr-deploy/deploy-batch.sh --phase bootstrap --mode apply
tools/midr-deploy/deploy-batch.sh --phase representatives --mode apply
tools/midr-deploy/deploy-batch.sh --phase members --mode apply
```

- 同一阶段内按 `nodes.csv` 的顺序逐台执行。
- **等待入群**：apply 模式下，每装完一台非 bootstrap 节点，脚本通过 `show midr join` 等待它报告非 0 的本地群号，再处理下一台，默认最多等 300 秒。代表和成员接连重启、不等收敛时，节点可能选中一个正在重启的群代表，入群过程会长时间无法完成。`--wait-joined 0` 关闭等待；超时按失败处理。
- 默认遇到第一个失败的节点就停止。确认失败节点不影响其余节点时，才使用 `--continue`。
- 日志写在 `$RUN_ROOT/<phase>/` 下，`RUN_ROOT` 默认为当前目录下的 `midr-deploy-<UTC时间>`。

**升级**也使用同样的流程：构建新发布包，设置新的 `RELEASE_SRC`，按三个阶段 apply。不要单独替换某个二进制。

## 10. 日常运维

### 10.1 服务

```bash
systemctl status midr-zebra midr-bgpd midrd midr-bgp-prefix-provider --no-pager
systemctl restart midrd             # 重启后自动重新加载 node.commands
systemctl restart midr-bgpd         # 仅边缘节点
systemctl restart midr-zebra        # 会影响 midrd 的 ZAPI 和 FIB，必要时才做
journalctl -u midrd -n 100 --no-pager
```

修改持久配置：编辑 `/etc/midr/node.commands`，然后 `systemctl restart midrd`。通过 VTY 交互做的修改立即生效，但不会写回 `node.commands`。

### 10.2 查询

```bash
export MIDR_DIR=/opt/midr-current
export LD_LIBRARY_PATH="$MIDR_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

"$MIDR_DIR/vtysh" --vty_socket /run/midr                 # 交互，可查 midrd、zebra、bgpd
"$MIDR_DIR/vtysh" --vty_socket /run/midr -d midrd \
    -c 'show midr self' -c 'show midr join' -c 'show midr neighbors' \
    -c 'show midr links' -c 'show midr spf' -c 'show midr virtual-links'
"$MIDR_DIR/vtysh" --vty_socket /run/midr -d zebra -c 'show ipv6 route'
"$MIDR_DIR/vtysh" --vty_socket /run/midr -d bgpd -c 'show bgp ipv6 unicast summary'   # 仅边缘
ip -6 route show proto 199                                 # MIDR 安装的业务路由
```

必须使用发布包中的 vtysh 和 `/run/midr`，不能用节点上系统 FRR 的 vtysh。`midrd-vty.py --commands-file` 会执行配置命令，可能重新触发入群，不能当查询工具用。

### 10.3 日志与自动升级

- `midr-logrotate.timer` 每 15 分钟检查一次 `/var/log/midr/*.log`，超过 256 MB 的文件以 copytruncate 方式轮转，保留 4 份压缩文件，不需要重启服务。
- needrestart 排除了 `midr*` 服务。系统升级了 midr 依赖的库后，需要在维护窗口按阶段手动重启这些服务。

### 10.4 序号文件

`/var/lib/midr/<node-id>.seq` 保存 MIDR 对象的序号。删除它会导致节点重启后发出的序号低于网络中已有的记录，新状态会被其他节点忽略。迁移或重装节点时要保留这个文件。

## 11. 集中采集与验收

```bash
python3 tools/midr-deploy/collect-midr-state.py \
    --output "midr-state-$(date -u +%Y%m%dT%H%M%SZ)"
column -t -s "$(printf '\t')" midr-state-*/summary.tsv
```

脚本并行登录各节点，采集服务状态、`show midr *` 输出、FIB、表 1990 和日志尾部，写入 `raw/<name>.json` 和汇总表 `summary.tsv`。`collection=OK` 只表示采集命令成功，验收还要按以下各层逐项核对。

| 层次 | 通过条件 |
|---|---|
| 进程与角色 | 边缘节点四个服务 active；内部节点和 bootstrap 没有 bgpd 和 Provider |
| underlay | 带协议和端口的 `ip -6 route get <对端locator> from <本机locator> ipproto tcp dport 5871` 经本站网关、underlay 接口和表 1990 |
| bootstrap | 群号 0，能力包含 Bootstrap，bootstrap 之间的 Session 为 Established |
| 入群 | 每个非 bootstrap 节点 `show midr join` 显示非 0 群号，`show midr directory` 中的成员、群号和能力与预期一致 |
| 拓扑与 SPF | 预期的 Link 为 up；同群前缀在 SPF 中为 local/intra，跨群前缀为 inter，且带下一跳 |
| 虚链路 | 预期的虚链路为 READY 且 overlay reachable |
| FIB 与业务 | 业务前缀以 `proto 199` 安装；`ip -6 route get` 的出口与 SPF 一致；选定的同群和跨群业务地址之间双向 ping 成功 |

注意事项：

- 不带协议和端口的 `ip route get` 查的是业务选路，结果可能指向 GRE，不能据此判断 transport 隔离失败。
- 业务前缀如果同时有更优的 `proto bgp` 路径，那么流量不经过 MIDR，这样的结果不能算作 MIDR 验收通过。
- 只验证了 locator 之间可达，不能代替业务前缀的验收。

## 12. 回滚

安装器失败时会自动回滚。需要人工回滚时，在目标节点上：

```bash
cat /var/lib/midr/last-deploy-backup
ls -1dt /var/lib/midr/deploy-backups/* | head
```

最简单的回滚方式是用上一个发布包重新执行 apply：

```bash
export RELEASE_SRC=/tmp/midr-<上一个发布 ID>
tools/midr-deploy/deploy-node.sh --node <name> --mode apply
```

旧发布目录保留在节点的 `/opt/midr-releases/` 中。手工回滚时必须同时恢复匹配的 midrd、zebra、bgpd 和 `lib/`，保留 `node.commands` 和序号文件，然后按 zebra → bgpd → midrd 的顺序启动，再重新验收。

停止整个网络时，按普通成员、群代表、bootstrap 的顺序停止 midrd，最后停止 midr-zebra。underlay 路由器上的 FRR 不属于停止对象。

## 13. 常见问题

| 现象 | 检查 |
|---|---|
| midrd 启动失败 | `/run/midr/zserv.api` 是否存在（zebra 是否 active）；服务使用的二进制和 `lib/` 是否来自同一发布包 |
| Session 建不起来 | 带协议和端口执行 `ip -6 route get <对端> from <本机locator> ipproto tcp dport 5871`；`ss -tnp \| grep 5871`；`ip -6 rule show` 中优先级 19900～19905 的规则和表 1990 是否存在 |
| 节点一直停在入群过程中 | `show midr join` 的阶段；bootstrap 和群代表是否在线；是否有多台节点同时重启（按阶段部署并等待入群） |
| 虚链路 READY 但业务不通 | `ip -6 route show proto 199`；业务前缀是否被 underlay 的更优路由覆盖；`route get` 的实际出口 |
| ping 不通 | 部分站点过滤 ICMP，改用 SSH、TCP 5871、Session 状态和日志判断 |
| check 报 `FRR includes additional routing configuration` | 节点系统 FRR 的 `/etc/frr/frr.conf` 中有静态路由或 OSPF/IS-IS/RIP 配置，停用系统 FRR 会丢失它们，需先迁移到 Netplan 或 underlay 路由器 |
| check 报 `missing protected netplan file`，或 apply 因 Netplan 文件变化回滚 | `MIDR_PROTECTED_NETPLAN` 中的文件不存在，或在部署期间被修改，需人工核对 |
| 磁盘被日志写满 | `midr-logrotate.timer` 是否 active；zebra 日志大量增长的问题见[待解决问题](../MIDR待解决问题.md) P-03 |

**表 1990**：midrd 启动前，`setup-midr-underlay.py` 根据 `MIDR_LOCATOR`、`MIDR_GATEWAY`、`MIDR_UNDERLAY_INTERFACE` 建立 IPv6 路由表 1990 和优先级 19900～19905 的策略规则，把本机 locator 发出的 MIDR 控制、探测和 GRE 外层报文送到本站网关。这样，即使 MIDR 学到的业务前缀覆盖了 locator，协议报文也不会递归进入隧道。表 1990 与 `proto 199` 是两回事：前者是 underlay 转发表，后者标记 main 表中由 MIDR 安装的业务路由。

## 14. 反向发布 MIDR 路由到网关

反向发布让边缘 MIDR 把从 MIDR 学到的远端前缀发布给本站网关，终端流量因此可以经网关进入 MIDR：

```text
终端 → 本站网关 →（学自 MIDR 的远端前缀）→ 本站 MIDR ==GRE== 远端 MIDR → 远端网关 → 远端终端
```

默认关闭，目前只生成 BGP 的配置，要求边缘 MIDR 与本站网关在同一个 AS（iBGP），FITI 即是如此。

### 14.1 开启

在边缘节点的 `node-configs/<name>.env` 中设置 `MIDR_ROUTE_EXPORT=yes`，再执行 check 和 apply。安装器在 `/etc/midr/bgpd.conf` 末尾追加 `node/midr-export-config.sh` 生成的配置：

- route-map `MIDR-EXPORT` 只放行带 MIDR tag 的路由，并设 `no-export`，网关只在本 AS 内使用它们；默认路由和所有 locator 前缀被拒绝。IPv4、IPv6 都执行 `redistribute midr route-map MIDR-EXPORT`。
- `network <本机 locator>/64`：把本机 locator 所在的前缀作为普通 underlay 路由发给网关，不带 `no-export`，由网关传给其他站点。
- AS 号取自原有的 `router bgp`；配置中没有同 AS 的邻居时，check 即报错。

网关不需要任何改动：来自 MIDR 的路由 AS_PATH 为空，比经 underlay 学到的同一前缀更优。

### 14.2 为什么要通告本机 locator

远端站点的前缀（如 `240a:a00f:300::/48`）通常覆盖远端 MIDR 的 locator（`240a:a00f:300:1::2`）。本站网关从本站 MIDR 学到这个 /48 后，如果没有更具体的路由，会把发往远端 locator 的 GRE 外层和控制报文也送回本站 MIDR，形成环路，Session 和隧道随之中断。每个边缘 MIDR 通告自己的 locator /64 后，各站网关对 locator 的最长匹配都走 underlay。

网关也会把其他站点的 locator /64 发给本站 MIDR。安装器把 profile 中全部 locator 前缀写入 `/etc/midr/locators.txt`，Prefix Provider 依据它不导入这些前缀。

内部节点不导入任何前缀，它们的 locator 只有在被别处导出的前缀覆盖时才需要单独通告。在 FITI 上，武汉、郑州的全部 MIDR 都是内部节点，不需要。

### 14.3 验证

```bash
# 网关：远端站点前缀来自本站 MIDR（带 no-export），远端 locator 走 underlay
vtysh -c 'show bgp ipv6 unicast <远端站点前缀>'
ip -6 route get <远端locator>
# 边缘 MIDR：没有 locator /64 进入 MIDR
ip -6 route show proto 199
```

两站点实验 `midr-test/route-export-lab/test.sh` 验证了这套配置，也演示了撤掉 locator 通告后出现的环路，结果见[待解决问题](../MIDR待解决问题.md) P-04。
