# FITI MIDR 部署编排（12 节点）

本目录提供 FITI 12 台 MIDR 路由器的批量检查、发布、安装和分阶段上线脚本。脚本在 NetArchLab90 上运行，使用 `inventory-12.csv` 连接目标节点，使用 `nodes-12.csv` 决定 MIDR 身份、角色和启动配置。

## 文件职责

以下文件均指 **90 管理机上的 `midr-test/fiti-deploy/` 编排目录**。它们不会因为目标节点已经安装 MIDR 而自动出现，也不会因为科研目录同步而自动上传到 90；在执行批量脚本前，必须先把整套目录放到 90 的 FRR 源码树中。

| 文件 | 用途 |
|---|---|
| `inventory-12.csv` | 管理入口清单。包含 SSH 地址、端口、用户、transport-address 和显式网关字段。 |
| `nodes-12.csv` | 当前 12 节点的正式配置清单，部署脚本按此选择节点。 |
| `node-configs/*.env` | 单节点运行环境变量。部署时会把 `MIDR_RELEASE_DIR` 改成当前发布目录。 |
| `node-configs/*.commands` | 单节点持久 VTY 配置，由 `midrd-vty.py` 在启动后加载。 |
| `node-configs/*.bgpd.conf` | 可选的节点 BGP 配置；TEIN-MIDR 使用明确配置，TEIN-B1 文件供双端邻居准备。 |
| `collect-midr-state.py` | 从 90 集中采集 12 台节点的协议、服务和 FIB，生成 summary.tsv 和原始 JSON。 |
| `deploy-node-from-90.sh` | 检查或安装一台节点。 |
| `deploy-batch-from-90.sh` | 按 bootstrap、representatives、members 三个阶段串行调用单节点脚本。 |
| `install-node-local.sh` | 在目标节点执行校验、备份、切换和启动。 |
| `setup-midr-underlay.py` | 每次 midrd 启动前配置 transport 专用 IPv6 表 1990，保护控制和 GRE 外层出口。 |
| `install-ssh-keys.sh` | 批量安装 90 的 SSH 公钥。 |
| `collect-underlay.sh` | 只读采集 underlay 和 FRR 状态。 |
| `check-transport-matrix.sh` | 按本机 locator 和 TCP 5871 查询到全部 transport-address 的 Linux 路由；不代替端到端建连验证。 |
| `install-midr-default-gateways.sh` | 按地址清单生成或检查 MIDR 的默认网关配置。 |
| `midr-*.service`、`midrd.service` | 目标节点上的 systemd 服务模板。 |

`inventory-12.csv` 是管理入口数据，`nodes-12.csv` 是 MIDR 部署数据；两者不要互相替换。网关列用于 TEIN-MIDR 这类非 `::2` 结尾的 locator。脚本使用 SSH 公钥认证，密码不写入清单或命令行。

目标节点不需要预先拥有这套编排目录。`deploy-node-from-90.sh` 会从 90 读取 `node-configs/`、启动脚本和 systemd 模板，临时复制到目标节点，再由 `install-node-local.sh` 安装到 `/etc/midr`、`/usr/local/libexec/midr` 和 `/etc/systemd/system/`。目标节点上的实际运行文件位置见 00 文档的目录表。

在 90 上执行批量部署前，先确认编排目录完整：

```bash
cd /home/guest/yhy/frr
find midr-test/fiti-deploy -maxdepth 2 -type f | sort

test -f midr-test/fiti-deploy/inventory-12.csv
test -f midr-test/fiti-deploy/nodes-12.csv
test -f midr-test/fiti-deploy/deploy-node-from-90.sh
test -f midr-test/fiti-deploy/deploy-batch-from-90.sh
test -f midr-test/fiti-deploy/install-node-local.sh
test -f midr-test/fiti-deploy/install-ssh-keys.sh
test -f midr-test/fiti-deploy/collect-underlay.sh
test -f midr-test/fiti-deploy/check-transport-matrix.sh
test -f midr-test/fiti-deploy/midr-zebra.service
test -f midr-test/fiti-deploy/midrd.service
test -f midr-test/fiti-deploy/midr-bgpd.service
test -f midr-test/fiti-deploy/node-configs/MIDR_fudan.env
test -f midr-test/fiti-deploy/node-configs/MIDR_fudan.commands
```

任一 `test` 失败，都先同步完整的 `fiti-deploy/` 目录，再继续部署。

## 节点分组

当前固定顺序来自 `nodes-12.csv`：

| 阶段 | 节点 |
|---|---|
| bootstrap | `MIDR-R3_wh`、`MIDR-R3_zhz` |
| representatives | `MIDR_fudan`（Group 2）、`MIDR_buaa`（Group 1） |
| members | WLQ、KMN、QDA、R1_wh、R2_wh、R1_zhz、R2_zhz、TEIN-MIDR_pku，共 8 台 |

边缘节点的 `edge_bgp` 为 `yes`，启动匹配版本的 `zebra + bgpd + midrd + Prefix Provider`；内部节点和专职 bootstrap 为 `no`，启动 `zebra + midrd`。普通成员自由分群，两个初始群代表不限制最终群组数量。`vtysh` 和 `midrd-vty.py` 都是客户端，不是常驻 daemon。

## 发布包要求

发布包必须是一个完整目录，至少包含：

```text
midrd             zebra             bgpd
vtysh             run-midrd.sh      midrd-vty.py
lib/              SHA256SUMS
midr-bgp-prefix-provider.py
```

`lib/` 中应包含运行这些 ELF 文件所需的非系统共享库。包名必须以 `midr-` 开头，例如 `/tmp/midr-<commit>-<utc>`。`SHA256SUMS` 必须覆盖发布包内的二进制、脚本和 `lib/` 共享库。00 文档给出构建和校验命令。

部署前必须在 90 当前 shell 中设置 `RELEASE_ID` 和 `RELEASE_SRC`。构建完成后可这样设置：

```bash
export RELEASE_ID="$(git -C /home/guest/yhy/frr rev-parse --short=12 HEAD)-$(date -u +%Y%m%dT%H%M%SZ)"
export RELEASE_SRC="/tmp/midr-$RELEASE_ID"
printf 'RELEASE_SRC=%s\n' "$RELEASE_SRC"
test -f "$RELEASE_SRC/RELEASE_ID"
test -f "$RELEASE_SRC/SHA256SUMS"
```

如果发布包已经存在，直接填写实际目录名，例如：

```bash
export RELEASE_SRC=/tmp/midr-5228cff9b3be-20261001T154159Z
export RELEASE_ID="$(cat "$RELEASE_SRC/RELEASE_ID")"
printf 'RELEASE_SRC=%s\nRELEASE_ID=%s\n' "$RELEASE_SRC" "$RELEASE_ID"
test -f "$RELEASE_SRC/SHA256SUMS"
```

重新登录 90 后需要重新执行上述设置；部署脚本读取的是 `RELEASE_SRC`，并据此把同一个发布包复制到各目标节点。

## 使用前提

在 90 上执行：

```bash
cd /home/guest/yhy/frr
chmod 0755 midr-test/fiti-deploy/*.sh
for f in midr-test/fiti-deploy/*.sh; do bash -n "$f" || exit 1; done
```

确认 90 可以使用公钥登录每台节点：

```bash
bash midr-test/fiti-deploy/install-ssh-keys.sh \
  --inventory midr-test/fiti-deploy/inventory-12.csv \
  --key "$HOME/.ssh/id_ed25519.pub" \
  --shared-password
```

脚本会先逐台验证公钥登录。若所有节点的公钥已经安装并可用，执行过程中不会提示密码；只有发现某台节点仍需要安装公钥时，才提示一次共享密码。

只读检查不修改目标节点：

```bash
RUN_ROOT="$PWD/fiti-underlay-$(date -u +%Y%m%dT%H%M%SZ)"
bash midr-test/fiti-deploy/collect-underlay.sh \
  --inventory midr-test/fiti-deploy/inventory-12.csv \
  --output "$RUN_ROOT"

MATRIX_ROOT="$PWD/fiti-transport-matrix-$(date -u +%Y%m%dT%H%M%SZ)"
bash midr-test/fiti-deploy/check-transport-matrix.sh \
  --inventory midr-test/fiti-deploy/inventory-12.csv \
  --output "$MATRIX_ROOT"
```

只有当 12 台节点都能登录、transport-address 在本站接口上存在、默认网关可用，并且 transport 矩阵中的 TCP 5871 路由查询成功时，才进入发布阶段。脚本使用本机 locator 作为源地址，已部署节点会命中 underlay 表 1990；发布前没有隔离规则时使用已有默认网关。路由查询只验证本机 FIB，端到端可达性仍需核对 Session 和双向探测。ICMP 被过滤时，以 SSH、TCP 5871 和 daemon 日志为准。

## 单节点检查和安装

`--mode check` 会上传临时文件并执行目标机只读校验，不停止现有服务；`--mode apply` 会备份现状，停止旧 FRR/MIDR，安装新发布和 systemd 服务，然后启动匹配版本。

`deploy-node-from-90.sh` 是 90 上的入口编排脚本；`install-node-local.sh` 是通过临时 staging 目录复制到目标节点后执行的一次性安装器，不是常驻进程。`apply` 会依次备份、停止旧进程、安装新版本、按 zebra → 边缘 bgpd → midrd 顺序启动并验收；失败时恢复备份。若目标节点已有同名但不完整的发布目录，安装器会在停止服务前拒绝切换，避免继续使用缺少 `lib/` 或二进制不匹配的目录。

```bash
# 沿用前面已经设置并校验过的 RELEASE_SRC；重新登录 90 后先按上面的示例恢复它。
test -f "$RELEASE_SRC/SHA256SUMS"

bash midr-test/fiti-deploy/deploy-node-from-90.sh \
  --node MIDR_fudan --mode check

bash midr-test/fiti-deploy/deploy-node-from-90.sh \
  --node MIDR_fudan --mode apply
```

目标机安装内容：

```text
/opt/midr-releases/midr-${RELEASE_ID}  完整发布包
/opt/midr-current                    当前版本符号链接
/etc/midr/node.env                    节点运行参数
/etc/midr/node.commands               持久 VTY 配置
/etc/midr/zebra.conf                  匹配 Zebra 配置
/etc/midr/bgpd.conf                   边缘节点 BGP 配置
/var/lib/midr/<node-id>.seq           MIDR sequence 持久文件
/var/log/midr/                        daemon 日志
/run/midr/                            PID、VTY 和 ZAPI socket
```

目标机安装器会备份 `/etc/frr`、Netplan、`/etc/midr`、systemd 状态、旧进程和路由到 `/var/lib/midr/deploy-backups/`。启动校验失败时恢复备份的系统 FRR 和服务文件；旧发布目录不会删除。

## 分阶段批量安装

默认遇到第一个失败节点就停止，先查看日志再继续。`--continue` 只在确认失败节点不影响后续节点时使用。

```bash
# 沿用前面已经设置并校验过的 RELEASE_SRC。
test -f "$RELEASE_SRC/SHA256SUMS"

bash midr-test/fiti-deploy/deploy-batch-from-90.sh \
  --phase bootstrap --mode check
bash midr-test/fiti-deploy/deploy-batch-from-90.sh \
  --phase bootstrap --mode apply

bash midr-test/fiti-deploy/deploy-batch-from-90.sh \
  --phase representatives --mode check
bash midr-test/fiti-deploy/deploy-batch-from-90.sh \
  --phase representatives --mode apply

bash midr-test/fiti-deploy/deploy-batch-from-90.sh \
  --phase members --mode check
bash midr-test/fiti-deploy/deploy-batch-from-90.sh \
  --phase members --mode apply
```

每次执行的日志保存在 `fiti-deploy-<UTC时间>/<phase>/`。每个阶段完成后，在下一阶段开始前采集一次 MIDR VTY 状态和 underlay 路由矩阵。

## 目标机服务

正式部署由 systemd 管理，启动顺序为：

```text
midr-zebra.service -> midr-bgpd.service（仅 edge_bgp=yes）
                     -> midrd.service -> ExecStartPost 加载 node.commands
                     -> midr-bgp-prefix-provider.service（仅边缘）
```

常用操作：

```bash
systemctl status midr-zebra.service midr-bgpd.service midrd.service
systemctl restart midrd.service
systemctl stop midrd.service
systemctl start midrd.service
systemctl restart midr-zebra.service
systemctl restart midr-bgpd.service       # 仅边缘节点
journalctl -u midrd.service -n 100 --no-pager
```

`midrd` 重启时会重新读取 `/etc/midr/node.commands`，并继续使用 `/var/lib/midr/<node-id>.seq`。不应手工执行旧的 `nohup --no-zebra` 方式，也不应把 `--peer`、`--link` 或 `--link-addr` 加到正式 systemd 启动参数中。

## 回滚

安装器失败会自动回滚。需要人工回滚时，在目标机查找最近备份：

```bash
ls -1dt /var/lib/midr/deploy-backups/* | head
cat /var/lib/midr/last-deploy-backup
```

回滚必须恢复同一版本的 `midrd`、`zebra`、`bgpd` 和 `lib/`，不能只替换一个二进制。恢复后按 systemd 服务顺序启动，并重新执行 `collect-underlay.sh`、Session、GRE 和 FIB 验收。

## 当前验证范围

已完成的本地验证：

```bash
for f in midr-test/fiti-deploy/*.sh; do bash -n "$f" || exit 1; done
bash midr-test/fiti-deploy/deploy-node-from-90.sh --help
bash midr-test/fiti-deploy/deploy-batch-from-90.sh --help
```

这些命令只证明脚本语法和参数入口正确。实际 90 远端部署必须在发布包准备完成、SSH 公钥可用、underlay 门禁通过后执行；本地脚本检查不会替代现场验收。

## 集中检查

TEIN 边缘 BGP 的双端配置见 [03 文档](../../doc/midr-doc/FITI部署测试/03-多节点生产式自动发现与整网验收.md)。所有阶段部署完成后，在 90 执行：

```bash
cd /home/guest/yhy/frr
OBSERVE_ROOT="$PWD/midr-acceptance/runs/$(date -u +%Y%m%dT%H%M%SZ)"
python3 midr-test/fiti-deploy/collect-midr-state.py \
  --nodes midr-test/fiti-deploy/nodes-12.csv --output "$OBSERVE_ROOT"
column -t -s "$(printf '\t')" "$OBSERVE_ROOT/summary.tsv"
```

`collection=OK` 只表示命令采集成功。Bootstrap 检查群号 0、能力 Bootstrap、骨干 Established 以及无 bgpd/Provider；10 台业务节点另外检查 Link、GRE、SPF、FIB 和实际业务包路径。
