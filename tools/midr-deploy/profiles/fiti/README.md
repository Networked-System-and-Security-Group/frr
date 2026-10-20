# FITI profile

FITI 模拟环境的 12 台 MIDR 路由器。它是 `tools/midr-deploy` 的默认 profile。

## 节点与角色

所有节点经 FITI 门户 `118.229.43.234` 的不同端口以 root 登录。

| 节点 | SSH 端口 | node-id | locator | 角色 | 群 | 边缘 BGP |
|---|---:|---:|---|---|---|---|
| MIDR-R3_wh | 13008 | 108 | 240a:a00f:800:3::2 | bootstrap | 0 | 否 |
| MIDR-R3_zhz | 22005 | 111 | 240a:a00f:810:3::2 | bootstrap | 0 | 否 |
| MIDR_fudan | 15503 | 101 | 240a:a00f:100:1::2 | group-rep | 2 | 是 |
| MIDR_buaa | 15003 | 102 | 240a:a00f:200:1::2 | group-rep | 1 | 是 |
| MIDR_wlq | 29502 | 103 | 240a:a00f:300:1::2 | member | auto | 是 |
| MIDR_kmn | 27502 | 104 | 240a:a00f:400:1::2 | member | auto | 是 |
| MIDR_qda | 25502 | 105 | 240a:a00f:500:1::2 | member | auto | 是 |
| MIDR-R1_wh | 13006 | 106 | 240a:a00f:800:1::2 | member | auto | 是 |
| MIDR-R2_wh | 13007 | 107 | 240a:a00f:800:2::2 | member | auto | 否 |
| MIDR-R1_zhz | 22003 | 109 | 240a:a00f:810:1::2 | member | auto | 是 |
| MIDR-R2_zhz | 22004 | 110 | 240a:a00f:810:2::2 | member | auto | 否 |
| TEIN-MIDR_pku | 14004 | 112 | 240a:a00f:f000:737::f037 | member | auto | 是 |

- 边缘节点运行 zebra、bgpd、midrd 和 Prefix Provider，与本站 BGP-R（TEIN-MIDR_pku 为 TEIN-B1_pku）保持 BGP 会话。
- 内部节点（R2_wh、R2_zhz）和 bootstrap（R3_wh、R3_zhz）只运行 zebra 和 midrd。
- 群 1、2 是初始预置群，普通成员自由分群，实际群数不限于 2。
- `node-configs/*.commands` 中的 `remote-as` 65101～65112 是 MIDR overlay 编号，与站点 underlay BGP 的 AS 无关。

每个节点的网关见 `nodes.csv` 的 `gateway` 列。除 TEIN-MIDR_pku 的网关为 `240a:a00f:f000:737::f007` 外，其余都是 locator 末尾 `::2` 换成 `::1`。

## 环境约定

- transport 出口接口为 `ens5`。
- `/etc/netplan/60-ens4.yaml`（带外管理）和 `61-l3.yaml`（三层互联）由平台维护。安装器只检查它们在部署前后不变，不修改它们。
- 节点上原有系统 FRR 8.4.4，安装器会停用 `frr.service`，改由匹配版本的 `midr-*` 服务接管。边缘节点沿用 `/etc/frr` 中已有的站点 BGP 配置；TEIN-MIDR_pku 使用 `node-configs/TEIN-MIDR_pku.bgpd.conf`。
- 部分站点过滤 ICMP，ping 失败不能单独说明不可达。

## 一次性准备

**默认路由**。首次部署前，各节点需要一条经本站网关的默认路由。`install-midr-default-gateways.sh` 把它写进 `61-l3.yaml` 并立即生效。默认 `--dry-run` 只做显示，确认后再用 `--apply`：

```bash
tools/midr-deploy/profiles/fiti/install-midr-default-gateways.sh --dry-run
tools/midr-deploy/profiles/fiti/install-midr-default-gateways.sh --apply
```

**TEIN-B1 邻居**。TEIN-MIDR_pku 的 BGP 对端 TEIN-B1_pku（SSH 端口 14002）不属于 MIDR 节点，需要手工配置对称邻居。TEIN-MIDR_pku 使用 AS `4200000007`、Router-ID `10.0.0.37`。

```bash
scp -P 14002 tools/midr-deploy/profiles/fiti/node-configs/TEIN-B1_pku.bgpd.conf \
    root@118.229.43.234:/tmp/midr-edge-bgp.conf
ssh -n -p 14002 root@118.229.43.234 \
    'vtysh -f /tmp/midr-edge-bgp.conf && vtysh -c "write memory"'
```

## 部署记录

FITI 上历次部署的结果和证据摘要见 [`doc/midr-doc/FITI部署记录/`](../../../../doc/midr-doc/FITI部署记录/)。
