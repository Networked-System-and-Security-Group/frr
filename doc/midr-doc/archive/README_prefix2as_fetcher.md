# Prefix2AS/CAIDA 快照抓取与格式化说明

## 数据源

推荐直接使用 CAIDA 发布的 RouteViews Prefix-to-AS 映射数据，而不是自己解析 RouteViews MRT RIB：

- IPv4: `https://publicdata.caida.org/datasets/routing/routeviews-prefix2as/`
- IPv6: `https://publicdata.caida.org/datasets/routing/routeviews6-prefix2as/`
- 每个目录下都有 `pfx2as-creation.log`，可用于发现新增快照。

`pfx2as-creation.log` 的核心字段是：

```text
seqnum generated_timestamp relative_path
```

脚本默认按最大 `seqnum` 选择最新快照，也可以用 `--date YYYYMMDD` 固定某一天。

## 输出格式

脚本会同时生成两种规范化文件：

```text
# prefix/asn
1.1.1.0/24 13335
2001:4860::/32 15169
```

```text
# CAIDA prefix2as style
1.1.1.0 24 13335
2001:4860:: 32 15169
```

CAIDA 原始 ASN 字段可能是 MOAS/AS-set 简化形式，例如：

```text
13335_15169
```

MVP 规则：只保留第一个前导十进制 ASN，即 `13335`。

## 使用方式

从本工作区运行：

```powershell
python .\outputs\fetch_caida_prefix2as.py --output-dir .\data\prefix2as
```

只抓 IPv4：

```powershell
python .\outputs\fetch_caida_prefix2as.py --families ipv4 --output-dir .\data\prefix2as
```

抓指定日期：

```powershell
python .\outputs\fetch_caida_prefix2as.py --date 20260625 --output-dir .\data\prefix2as
```

只解析 creation log，不下载快照：

```powershell
python .\outputs\fetch_caida_prefix2as.py --dry-run
```

## 目录结构

```text
data/prefix2as/
  raw/
    ipv4/2026/06/routeviews-rv2-20260625-1200.pfx2as.gz
    ipv6/2026/06/routeviews-rv6-20260625-1200.pfx2as.gz
  snapshots/
    ipv4-20260625-seq...__ipv6-20260625-seq.../
      prefix_asn.txt
      caida_style.txt
      manifest.json
  latest/
    prefix_asn.txt
    caida_style.txt
    manifest.json
```

`latest/` 是最新成功生成快照的拷贝，业务侧 IP-to-ASN 查询可以稳定读取这里。

## 自动化建议

Windows Task Scheduler 可每天执行：

```powershell
python C:\path\to\fetch_caida_prefix2as.py --output-dir C:\path\to\data\prefix2as
```

Linux/macOS cron 可每天执行：

```cron
15 3 * * * /usr/bin/python3 /path/to/fetch_caida_prefix2as.py --output-dir /var/lib/prefix2as
```

建议业务服务只读取 `latest/`。脚本会先生成完整快照，再逐文件临时写入并替换 `latest/` 下的规范化文件，避免服务读到半成品文件。

## 后续 IP-to-ASN 查询建议

读取 `latest/prefix_asn.txt` 后构建最长前缀匹配索引：

- Python MVP 可用 `pytricia` 或 `py-radix`。
- Go 可用 `netip.Prefix` 配合 Patricia trie/radix tree。
- 如果不引入依赖，需分别维护 IPv4/IPv6 按 prefix length 分桶的哈希表，从 `/32` 到 `/0` 或 `/128` 到 `/0` 递减查找。

查询必须使用最长前缀匹配，而不能用第一条命中。
