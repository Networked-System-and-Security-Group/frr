# MIDR 虚链路运行时实测证据（T2）

> 本文只记录**本轮**（2026-09-29，远端 dell-PowerEdge-R750 + 容器 `frr-ubuntu24-ymy`）亲眼观察到的原始输出。
> 上一轮文档中的 `106/112 PASS` 与本轮无关，**未复用**（countersign §1 已声明那些计数对应修正前源码）。
> 所有 ifindex、地址、PASS/FAIL 计数均来自本文引用的原始日志。

## 0. 本轮结论速览

| 目标 | 结论 |
| --- | --- |
| 源码同步 | 本地 HEAD `027bde5b` 以 `git archive` 冻结传输，容器树 3056 个源文件 sha256 **逐个相等**；容器内多出的 168 个文件没有一个是 HEAD 内容（166 个构建产物 + 2 个旧残留） |
| 构建 | 容器内 `bootstrap.sh` + `configure` + `make -j112` 退出码 **0**；顶层构建 0 warning（`-Werror`），midrd 侧 138 行 warning 全部来自 `lib/mgmt_msg_native.h`（既有、`-Wno-error`） |
| ZAPI 编号门禁 | 4 种模式全部 **PASS**，5 个新消息追加在 147–151，基线成员序号未变 |
| 组件套件（非特权） | `programs_passed=30 programs_failed=0 fixtures=passed`（`COMPONENT_RESULT=0`） |
| midrd 组件套件 | `make BUILD_DIR=/tmp/midrd-build all test` 退出码 0，27 条 PASS |
| GRE 双容器连通性 | `=== summary: PASS=58 FAIL=0 ===`（含 gre / ip6gre / 幂等 / 拆除 / 真实内核 EEXIST） |
| Stage E 跨 BGP-only underlay | `=== stage E summary: PASS=34 FAIL=0 ===` |
| ZAPI/FIB smoke（补充） | `=== summary: PASS=26 FAIL=0 ===` / `third-group ZAPI/FIB smoke: PASS` |
| 内核级证据 | `ip -d link show` 确认 `gre` / `ip6gre`；overlay 地址为 **`192.168.100.1/30`（host 地址）**，`192.168.100.0/30` 命中数 **0**；IFF_UP 存在；`ip route get` 命中虚接口；拆除后设备与地址均清理 |
| 失败项 | ① 出厂 `midr-component-tests.txt` 与 `tests/bgpd/subdir.am` 注册集不一致 → `run-midr-component-suite.sh` 直接 exit 2；② 以 **root** 运行该套件时 `test_midr_sequence` 断言失败（EACCES 期望值只在非特权下成立） |

本轮脚本自带计数的汇总：**PASS=179，FAIL=3**（明细见 §3.8；其中 2 个 FAIL 属于“以 root 运行”的环境偏差，1 个 FAIL 是 manifest 不一致的真实门禁失败）。
§3.8 列出的 8 条独立运行中，唯一“在跑用例之前就失败”的是出厂清单那一条（`run-midr-component-suite.sh` 的 manifest 一致性检查）。

---

## 1. 被测版本与同步

### 1.1 本地版本

```text
$ cd /Users/yangmengyu/githubdocuments/frr
$ git rev-parse HEAD
027bde5bc8ad0a6ac4d8afb3b2286014a6641ce1
$ git rev-parse --abbrev-ref HEAD
feat/midr-three-way-integration
$ git log -1 --format=%cI
2026-09-29T22:01:57+08:00
$ git status --porcelain=v1 | wc -l
0
$ git log --oneline -3
027bde5bc8 doc: record this round, the EEXIST limitation and the authorization surface
eb9dd38eba midr-test: add the real-kernel EEXIST case, a gate self-test and move the e2e tool
00887b3ffe zebra: decode the label on address unset and drop a stale comment
```

工作树干净（0 条 `status` 记录），即 **worktree == HEAD**。

### 1.2 同步方式（冻结传输 + 内容比对，未使用 `rsync --delete`）

远端有两份副本：

```text
$ ssh ... yangmy@101.6.30.220 'ls -la /home/yangmy/frr /home/yangmy/frr-midrd3'
/home/yangmy/frr         # 旧导出，含 in-source 构建残留（config.h/aclocal.m4/autom4te.cache，部分 root 属主）
/home/yangmy/frr-midrd3  # 本轮目标：无 .git 的源码导出（含 macOS ._* 残渣）
```

容器内真正用于**构建**的树是容器的 `/home/frr/frr-midrd3`（容器只挂载了 `/lib/modules`，
见 `docker inspect frr-ubuntu24-ymy --format '{{json .Mounts}}'` →
`[{"Type":"bind","Source":"/lib/modules","Destination":"/lib/modules",...}]`），因此源码必须**复制进容器**。
本轮不走 `rsync --delete`，改成可校验的冻结包：

```text
$ git archive --format=tar --prefix=frrsrc/ HEAD -o /tmp/midr-src-<sha>.tar && gzip -9
$ ls -la /tmp/midr-src-027bde5bc8ad0a6ac4d8afb3b2286014a6641ce1.tar.gz
-rw-r--r-- 13921794  /tmp/midr-src-027bde5bc8ad0a6ac4d8afb3b2286014a6641ce1.tar.gz
$ shasum -a 256 ...
ee6e680ec7dc54aa05f01406d56801f719bca8cfc0ee8151ab45da928b07eaa5
$ tar -tzf ... | wc -l
12198
```

```text
$ scp -i ~/.ssh/frr -P 50022 /tmp/midr-src-*.tar.gz yangmy@101.6.30.220:/tmp/
$ ssh ... 'sha256sum /tmp/midr-src-*.tar.gz'
ee6e680ec7dc54aa05f01406d56801f719bca8cfc0ee8151ab45da928b07eaa5   # 与本地一致
$ ssh ... 'echo <pw> | sudo -S docker cp /tmp/midr-src-*.tar.gz frr-ubuntu24-ymy:/tmp/'
$ ssh ... 'echo <pw> | sudo -S docker exec frr-ubuntu24-ymy sha256sum /tmp/midr-src-*.tar.gz'
ee6e680ec7dc54aa05f01406d56801f719bca8cfc0ee8151ab45da928b07eaa5   # 与本地一致
$ ssh ... 'echo <pw> | sudo -S docker exec frr-ubuntu24-ymy bash -lc \
    "tar -xzf /tmp/midr-src-*.tar.gz --strip-components=1 -C /home/frr/frr-midrd3"'
```

三处 sha256 相同（mac / 宿主 `/tmp` / 容器 `/tmp`），因此“被测源码版本”可自证。

### 1.3 同步后的内容比对（不是看时间戳，而是逐文件哈希）

```text
容器：find . -type f \( -name '*.c' -o -name '*.h' -o -name '*.sh' -o -name '*.py' -o -name '*.am' \) \
        -not -name '._*' -not -path '*/.deps/*' -not -path '*/.libs/*' -print0 | xargs -0 sha256sum
本地：对 git archive 解包后的同一集合做 sha256sum，join 后逐条比对
```

| 指标 | 结果 |
| --- | --- |
| 共同文件数 | 3056 |
| 内容不一致（同路径哈希不同） | **0** |
| 本地有、容器缺 | **0** |
| 容器多出（非 HEAD 内容） | 168 个 = **80** 个 `*_clippy.c` + **52** 个 `yang/*.yang.c` + **4** 个 `*.pb-c.{c,h}` + **32** 个其它（`config.h`、`lib/command_{lex,parse}.{c,h}`、`lib/defun_lex.c`、`lib/config_paths.h`、`lib/route_types.h`、`lib/version.h`、`lib/vtysh_daemons.h`、`vtysh/vtysh_cmd*.c` 共 9 个、`tools/*.sh` 3 个、`pkgsrc/*.sh` 8 个，以及 **2 个旧同步残留** `bgpd/midr_trace_exec.{c,h}`）。前 166 个都是构建生成物；那 2 个残留存在于历史提交 `89168d2cc7` 但**不在 HEAD**，容器内 mtime 为 Sep 21 05:56，本轮 tar 不会产生它们，构建日志对它 **0** 次引用 |

（`tests/topotests/**/{customize.py,oper.py}` 有 6 个是 git 符号链接（`120000`），
两边 `find -type f` 都不列，故不算缺失。）

关键文件哈希（本地与容器一致）：

| 文件 | sha256 |
| --- | --- |
| `midr-test/midr-gre-connectivity-test.sh` | `487ee932e332f0ca10d17b9710de5a1fbac5c1a59005b849d84d801dd05ced5c` |
| `midr-test/r7-dp-stagee-bgp-underlay.sh` | `a5ab02112125d062df778f6da4cd3c9d10ca83e6b618d3f2b487a7f1192a07b8` |
| `midr-test/check-zapi-numbering.sh` | `795edd28fd7b4974e274eddadddb86c7dee6e73efbbc1535cb6578cd601e949a` |
| `midr-test/run-midr-component-suite.sh` | `bc98d7d5ad42ba558ca2d6381e0a3f856e98dcde8a3c77859831eeaf7a0ef9f7` |
| `midrd/midr-virtual-link.c` | `35e0eaa0906af921b0a728b584d416d064bd94452034607a967b24aea4f0538f` |
| `lib/zclient.h` | `edd0323379ae313e9ffe57c29a13bc5b85f310bc7433073850c154cee9966af0` |
| `zebra/zapi_msg.c` | `6f9e522fa2c2ae00f8f7fa72730e3ec7ad9a9d328d457e3698936ddf346ff86f` |

远端被测路径：容器 `frr-ubuntu24-ymy` 内 `/home/frr/frr-midrd3`。

### 1.4 无源码改动声明

本轮**未修改任何源码**，也没有改动仓库内的任何脚本。唯一被我修改的是**我自己的临时执行脚本**
（`/tmp/t2-build2.sh`：把构建从默认用户改成 `-u root`；`/tmp/t2-suite2.sh`、`/tmp/t2-suite3.sh`：
给 `run-midr-component-suite.sh` 传入第二个参数 `MANIFEST`；另有 `/tmp/t2-zapi.sh`、
`/tmp/t2-kernel-evidence.sh` 两个取证脚本）。这些脚本不在仓库内，不是被测物；
仓库工作树上的写入只有“解包 HEAD 内容”这一步。

---

## 2. 环境与构建

### 2.1 宿主与容器

```text
$ ssh -i ~/.ssh/frr -p 50022 yangmy@101.6.30.220 'uname -a'
Linux dell-PowerEdge-R750 5.15.0-139-generic #149~20.04.1-Ubuntu SMP Wed Apr 16 08:29:56 UTC 2025 x86_64 GNU/Linux
$ ssh ... 'echo <pw> | sudo -S docker inspect frr-ubuntu24-ymy --format "{{.Image}} {{.Config.Image}} {{.Config.User}} {{.Created}}"'
sha256:aabcd4ee389c26e43385496cd3cac4422d9fe2bb485660cf7fe44f12afda600f frr-ubuntu24-ymy:latest frr:frr 2026-09-06T01:57:07.331859883Z
$ ssh ... 'echo <pw> | sudo -S docker exec -u root frr-ubuntu24-ymy bash -lc \
    "cat /etc/os-release|head -2; uname -r; nproc; gcc --version|head -1; git --version"'
PRETTY_NAME="Ubuntu 24.04.4 LTS" / VERSION_ID="24.04"
5.15.0-139-generic
112
gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
git version 2.43.0
```

| 项 | 值 |
| --- | --- |
| 宿主 | dell-PowerEdge-R750，Ubuntu 20.04，内核 `5.15.0-139-generic` |
| 构建容器 | `frr-ubuntu24-ymy`（镜像 `frr-ubuntu24-ymy:latest`，id `aabcd4ee389c`），默认 exec 用户是 `frr`(uid 1010)，**不是 root** |
| 容器内 OS / 编译器 | Ubuntu 24.04.4 LTS / gcc 13.3.0 |
| CPU | `nproc=112` |
| 构建树 | `/home/frr/frr-midrd3`（同时是各 `r7-dp-*.sh`、`midr-gre-connectivity-test.sh` 默认引用的路径） |

与 `docker/ubuntu24-ci/README.md` 的关系：该 README 给的是**镜像构建 / 容器启动 / 顶层 topotest /
`make check`** 的命令（`docker build -t frr-ubuntu24:latest -f docker/ubuntu-ci/Dockerfile .`、
`docker run -d --init --privileged --name frr-ubuntu24 ...`、`docker exec frr-ubuntu24 bash -c 'cd ~/frr ; make check'`）。
本环境的镜像与容器**已按该 README 部署完毕**（容器已 Up 3 周），因此本轮**没有重建镜像**，
而是在既有容器内按仓库自身文档（`midr-test/README.md` “编译”节、`doc/midr-doc/dp-doc/dp-02-build-and-test.md` §3/§4）
构建源码树：`bootstrap.sh` → `configure` → `make -j112`，再加 `midrd/` 独立组件构建与测试。
`make check`（顶层 automake 测试）与 `tests/topotests` **本轮未运行**，理由见 §5。

### 2.2 构建命令与结果

**第一次（按容器默认用户 `frr`，失败——环境问题，非源码问题）**

```text
$ docker exec frr-ubuntu24-ymy bash /tmp/t2-build.sh      # 未加 -u root
bootstrap_exit=0
configure_exit=0
(CDPATH=... && cd . && /bin/bash '/home/frr/frr-midrd3/m4/ac/missing' autoheader)
...
touch config.h.in
touch: cannot touch 'config.h.in': Permission denied
make: *** [Makefile:7717: config.h.in] Error 1
make_exit=2
```

原因：`/home/frr/frr-midrd3/config.h.in`、`config.h`、`Makefile`、`configure` 等生成物属主是 `root`
（上一次带 `-u root` 的构建留下的），默认用户 `frr` 无写权限。日志：容器 `/tmp/t2-build.log`。

**第二次（`-u root`，成功）**

```text
$ docker exec -u root frr-ubuntu24-ymy bash -lc "grep -n '_exit=' /tmp/t2-build-root.log"
1257:make_exit=0
1285:make_tests_exit=0
18549:midrd_test_exit=0
18560:T2_BUILD_ROOT_DONE
```

| 步骤 | 命令（容器内，cwd `/home/frr/frr-midrd3`） | 退出码 |
| --- | --- | --- |
| autotools | `sh bootstrap.sh` | 0（第一次运行，见上） |
| configure | `./configure`（无附加参数，与树内既有 `ac_cs_config=''` 一致） | 0 |
| 顶层构建 | `make -j112` | **0** |
| 组件测试二进制与守护进程 | `make -j112 tests/bgpd/test_midr_{input,resync,sequence,cost,ls_object,codec,attr,safi,packet,rib,scope,owned,prefix,sync,lsdb,ted,instance,scale,zebra,spf,spf_install,spf_pipeline} tests/bgpd/test_capability zebra/zebra bgpd/bgpd midrd/midrd` | **0** |
| midrd 数据面组件 | `cd midrd && make -j112 BUILD_DIR=/tmp/midrd-build all test` | **0** |

告警统计（`/tmp/t2-build-root.log`，18560 行）：

```text
$ grep -c 'warning:' /tmp/t2-build-root.log
138
$ grep 'warning:' /tmp/t2-build-root.log | grep -v 'mgmt_msg_native.h' | wc -l
0
$ grep -o 'mgmt_msg_native.h:[0-9]*' /tmp/t2-build-root.log | sort -u | wc -l
23
```

- 顶层 `make`（`-Werror`）段（日志第 8–1257 行）**0 条 warning**；
- 138 条全部出现在**第 1286 行之后**的 midrd 组件构建段（该段显式带 `-Wno-error`），内容全是既有的
  `lib/mgmt_msg_native.h:NNN:31: warning: declaration does not declare anything`（涉及 23 个行号），与第三组改动无关。

产物（`ls -la`，时间为本次构建 `Sep 29 14:34–14:35`）：

```text
zebra/zebra                     libtool 包装脚本 6272 B
zebra/.libs/zebra               7962816 B   Sep 29 14:34
bgpd/.libs/bgpd                 18125488 B  Sep 29 14:34
lib/.libs/libfrr.so.0.0.0       7687824 B   Sep 29 14:34
midrd/midrd                     libtool 包装脚本 6221 B
/tmp/midrd-build/midrd-virtual-link-test    267856 B
/tmp/midrd-build/midrd-gre-registry-test    235104 B
/tmp/midrd-build/midrd-dp-test              256824 B
/tmp/midrd-build/midrd-gre-tool             235168 B
/tmp/midrd-build/midrd-dp-e2e-tool          239360 B
```

---

## 3. 逐项实测结果

### 3.1 ZAPI 编号门禁 `midr-test/check-zapi-numbering.sh`

该门禁是**纯 git/源码比较**（`git show <baseline>:lib/zclient.h` vs 工作树文件），而容器里的构建树是
**无 `.git` 的导出树**。为了在容器内真跑门禁，本轮用 `git bundle --all` 把本地对象库搬进容器并克隆
（bundle 189 410 838 B，本地 `git count-objects -vH` 显示 `size-pack: 199.74 MiB`）：

```text
$ scp -i ~/.ssh/frr -P 50022 /tmp/frr-full.bundle yangmy@101.6.30.220:/tmp/     # real 15.5 s
$ echo <pw> | sudo -S docker cp /tmp/frr-full.bundle frr-ubuntu24-ymy:/tmp/
$ docker exec -u root frr-ubuntu24-ymy bash /tmp/t2-zapi.sh
clone_HEAD=027bde5bc8ad0a6ac4d8afb3b2286014a6641ce1
clone_branch=feat/midr-three-way-integration
clone_status_lines=0
baseline_1adb4c92d0_present=yes
sync_ref_present=no
=== gate input file identity: clone vs build tree ===
IDENTICAL lib/zclient.h edd0323379ae313e9ffe57c29a13bc5b85f310bc7433073850c154cee9966af0
IDENTICAL lib/log.c 2ddd1226fd9d260bcc827ba01283dc8072a71342c379a766496885f69fccb0bc
IDENTICAL midr-test/check-zapi-numbering.sh 795edd28fd7b4974e274eddadddb86c7dee6e73efbbc1535cb6578cd601e949a
```

| 运行 | 命令 | 关键原始输出 | 结论 |
| --- | --- | --- | --- |
| R1 | `cd /home/frr/frr-zapi-git && ./midr-test/check-zapi-numbering.sh` | `note: 147 reference enum members; 139 tolerated pre-existing positional gap(s)` / `check-zapi-numbering: PASS (baseline 1adb4c92d0 vs working tree)` | **PASS**（`R1_exit=0`） |
| R2 | 对**真实构建树**跑：`cd /home/frr/frr-midrd3 && GIT_DIR=/home/frr/frr-zapi-git/.git GIT_WORK_TREE=/home/frr/frr-midrd3 ./midr-test/check-zapi-numbering.sh` | `check-zapi-numbering: PASS (baseline 1adb4c92d0 vs working tree)` | **PASS**（`R2_exit=0`） |
| R3 | `./midr-test/check-zapi-numbering.sh --self-test` | `check-zapi-numbering: self-test PASS (clean HEAD passes, swapped ZEBRA_GRE_ADD/ZEBRA_GRE_DELETE fails)` | **PASS**（门禁“有牙齿”） |
| R4 | `./midr-test/check-zapi-numbering.sh WORKTREE` | `check-zapi-numbering: PASS (baseline WORKTREE vs working tree)` | **PASS** |

枚举尾部（`--dump HEAD | tail -8`，首列为枚举内 0 基下标）：

```text
144	ZEBRA_TC_FILTER_DELETE
145	ZEBRA_OPAQUE_NOTIFY
146	ZEBRA_SRV6_SID_NOTIFY
147	ZEBRA_GRE_ADD
148	ZEBRA_GRE_DELETE
149	ZEBRA_INTERFACE_ADDRESS_SET
150	ZEBRA_INTERFACE_ADDRESS_UNSET
151	ZEBRA_INTERFACE_SET_ADMIN_UP
```

即 5 个新消息**只追加在尾部**（147–151），基线成员序号不变。计数：**PASS 4 / FAIL 0**。
限制：`origin/fix/midrd-integration-hardening` 未取到，脚本自报
`note: origin/fix/midrd-integration-hardening not fetched; shared-baseline sync check skipped`
—— 共享分支同步检查这一段本轮**未覆盖**。

### 3.2 非特权组件套件 `midr-test/run-midr-component-suite.sh`

**3.2.1 按仓库出厂清单运行 → 门禁失败（原始输出）**

```text
$ docker exec -u root frr-ubuntu24-ymy bash -lc \
    "cd /home/frr/frr-midrd3 && bash midr-test/run-midr-component-suite.sh /home/frr/frr-midrd3"
### T2 component suite 2026-09-29T14:36:22Z
### user=root uid=0 root=/home/frr/frr-midrd3
--- /tmp/tmp.E56L5X04s7/registered	2026-09-29 14:36:22.164448215 +0000
+++ /tmp/tmp.E56L5X04s7/manifest-sorted	2026-09-29 14:36:22.164448215 +0000
@@ -1,11 +1,9 @@
 test_capability
-test_midr_admission
 test_midr_attr
 test_midr_codec
 test_midr_cost
 test_midr_input
 test_midr_instance
-test_midr_ip2asn_update
 test_midr_ls_object
 test_midr_lsdb
 test_midr_owned
@@ -22,9 +20,4 @@
 test_midr_spf_pipeline
 test_midr_sync
 test_midr_ted
-test_midr_tier1
-test_midr_tier1_list
-test_midr_trace_engine
-test_midr_trace_scheduler
-test_midr_trace_udp
 test_midr_zebra
ERROR: component manifest does not match registered non-privileged MIDR tests
suite_exit=2
```

（上面是完整原始输出；`registered` 一侧比 `manifest-sorted` 多出 **7** 项：
`test_midr_admission`、`test_midr_ip2asn_update`、`test_midr_tier1`、`test_midr_tier1_list`、
`test_midr_trace_engine`、`test_midr_trace_scheduler`、`test_midr_trace_udp`。）

源码级原因：`tests/bgpd/subdir.am` 注册了 `test_midr_admission`（第 356 行）与
`test_midr_ip2asn_update`（第 311 行），但 `midr-test/midr-component-tests.txt` 未列出，
套件开头的 manifest↔注册集一致性检查直接 `die`，**一个程序都没跑**。
这是本轮发现的真实门禁缺陷（与虚链路无关，但会让“全量组件门禁”在 HEAD 上跑不起来）。

**3.2.2 用 `subdir.am` 推导出的清单重跑（非特权用户 `frr`，即门禁的预期身份）**

```text
$ sed -n 's|^check_PROGRAMS += tests/bgpd/\(test_midr_[A-Za-z0-9_]*\)$|\1|p' tests/bgpd/subdir.am \
    | grep -v -E '^(test_midr_ted_fixture|test_midr_zebra_e2e)$' > /tmp/t2-manifest.txt
$ sed -n 's|^check_PROGRAMS += tests/bgpd/\(test_capability\)$|\1|p' tests/bgpd/subdir.am >> /tmp/t2-manifest.txt
$ docker exec -u frr frr-ubuntu24-ymy bash /tmp/t2-suite3.sh
### user=frr uid=1010 gid=92 root=/home/frr/frr-midrd3
COMPONENT_SUMMARY programs_passed=30 programs_failed=0 fixtures=passed
COMPONENT_RESULT=0
suite3_exit=0
```

30 个组件程序 + 1 条 fixture 的 `RESULT` 行（原始，共 31 行，全部 `rc=0`）：

```text
RESULT	test_capability	0	failures: 0
RESULT	test_midr_admission	0	MIDR admission tests passed
RESULT	test_midr_attr	0	MIDR attribute tests passed
RESULT	test_midr_codec	0	MIDR codec tests passed
RESULT	test_midr_cost	0	MIDR cost tests passed
RESULT	test_midr_input	0	MIDR input validation tests passed
RESULT	test_midr_instance	0	MIDR instance/canonical tests passed
RESULT	test_midr_ip2asn_update	0	failures: 0
RESULT	test_midr_ls_object	0	MIDR LS object tests passed
RESULT	test_midr_lsdb	0	MIDR LSDB and production TED tests passed
RESULT	test_midr_owned	0	MIDR owned object tests passed
RESULT	test_midr_packet	0	MIDR packet tests passed
RESULT	test_midr_prefix	0	MIDR prefix contributor tests passed
RESULT	test_midr_resync	0	MIDR resynchronization tests passed
RESULT	test_midr_rib	0	MIDR RIB tests passed
RESULT	test_midr_safi	0	MIDR SAFI tests passed
RESULT	test_midr_scale	0	MIDR scale evidence collected
RESULT	test_midr_scope	0	MIDR scope tests passed
RESULT	test_midr_sequence	0	MIDR sequence tests passed
RESULT	test_midr_spf	0	MIDR hierarchical SPF tests passed
RESULT	test_midr_spf_install	0
RESULT	test_midr_spf_pipeline	0	MIDR SPF pipeline tests passed
RESULT	test_midr_sync	0	MIDR sync tests passed
RESULT	test_midr_ted	0	MIDR TED snapshot tests passed
RESULT	test_midr_tier1	0	failures: 0
RESULT	test_midr_tier1_list	0	MIDR Tier-1 list tests passed
RESULT	test_midr_trace_engine	0	MIDR engine tests passed
RESULT	test_midr_trace_scheduler	0	MIDR scheduler tests passed
RESULT	test_midr_trace_udp	0	MIDR UDP ancillary tests passed
RESULT	test_midr_zebra	0	=== 0 test(s) FAILED ===
RESULT	test_midr_ted_fixtures(py)	0	run-ted-fixtures.py
```

计数：**programs 30 PASS / 0 FAIL，TED fixtures PASS**。fixture 明细（原始，12 条全 PASS）：

```text
PASS @M1-TED-001: same-group directed router view
PASS @M1-TED-002: cross-group intra/egress classification
PASS @M1-TED-003: directed Link has no implicit reverse
PASS @M1-TED-004: multiple egress Links aggregate by minimum cost
PASS @M1-TED-005: one prefix maps to multiple target groups
PASS @M1-TED-006: unreachable target has no fabricated group edge
PASS @M1-TED-007: unknown YAML key rejection
PASS @M1-TED-008: duplicate Link rejection
PASS @M1-TED-009: invalid Link cost rejection
PASS @M1-TED-010: unknown Link endpoint rejection
PASS @M1-TED-011: mixed Link endpoint family rejection
PASS @M1-TED-012: duplicate YAML mapping key rejection
```

**3.2.3 同一套件以 root 运行 → 1 个测试失败（环境偏差，必须记录）**

```text
RESULT	test_midr_sequence	134	timeout: the monitored command dumped core
  | 2026/09/29 14:38:10 tests/bgpd/test_midr_sequence.c:282: test_frr_store_persistence():
    assertion (midr_sequence_allocator_init(&allocator, node_id, &midr_sequence_frr_store_ops, NULL) == -EACCES) failed
FAIL: C fixture runner not found: /home/frr/frr-midrd3/tests/bgpd/test_midr_ted_fixture
COMPONENT_SUMMARY programs_passed=29 programs_failed=1 fixtures=failed
```

两点说明（本轮可复现的事实，不是推测）：

- `test_midr_sequence` 该断言**期望 `-EACCES`**（期望“无权写 FRR store”），以 root 运行时前提不成立 → 断言失败；
  换回非特权 `frr` 后同一条用例 PASS（见 3.2.2）。
- `fixtures=failed` 是因为当时 `tests/bgpd/test_midr_ted_fixture` **尚未构建**（它不在这份清单里）；
  随后 `make -j112 tests/bgpd/test_midr_ted_fixture` 构建成功，3.2.2 的 fixtures 即为 PASS。

### 3.3 midrd 数据面组件套件（`midrd/Makefile` 的 `test` 目标）

```text
$ docker exec -u root frr-ubuntu24-ymy bash -lc \
    "cd /home/frr/frr-midrd3/midrd && make -j112 BUILD_DIR=/tmp/midrd-build all test"
midrd_test_exit=0
```

日志（`/tmp/t2-build-root.log`）中出现的 PASS 行，共 27 条：

```text
midrd R6-A contract: PASS            midr-core-test: PASS
midr-prefix-provider-test: PASS      midrd-wire-test: PASS
midrd-transport-test: PASS           midrd-transport-budget-test: PASS
midrd session tests: PASS            midrd-consumer-test: PASS
midrd-spf-test: PASS                 midrd-engine-test: PASS
midrd-prefix-ipc-test: PASS          midrd-owned-test: PASS
midrd-prefix-transaction-test: PASS  midrd-scope-test: PASS
midrd-cost-test: PASS                midrd-local-ipc-test: PASS
midrd-local-transaction-test: PASS   midrd-lsdb-test: PASS
midrd-ted-test: PASS                 midrd-interface-test: PASS
midrd-spf-install-test: PASS         midrd-dp-test: PASS
midrd-dp-gre-registry-test: PASS     midrd-virtual-link-test: PASS
midrd-sync-test: PASS                midrd-scale-test: PASS
standalone libfrr boundary scan: PASS
```

计数：**27 PASS / 0 FAIL**（`make` 退出码 0）。

### 3.4 三个数据面测试二进制（单独运行，原始尾行）

```text
$ LD_LIBRARY_PATH=/home/frr/frr-midrd3/lib/.libs /tmp/midrd-build/midrd-virtual-link-test
midrd-virtual-link-test: PASS                 →  midrd-virtual-link-test_exit=0
$ LD_LIBRARY_PATH=... /tmp/midrd-build/midrd-gre-registry-test
midrd-dp-gre-registry-test: PASS              →  midrd-gre-registry-test_exit=0
$ LD_LIBRARY_PATH=... /tmp/midrd-build/midrd-dp-test
midrd-dp-test: PASS                           →  midrd-dp-test_exit=0
```

用例覆盖面（对 `midr-test/*.c` 的 `main()` 逐一核对）：
`virtual-link-test` 调用 `test_case01..test_case15` + `test_case16a/16b/17/18/19` + `test_alloc_failure`
（共 **21** 个用例，函数名即 §9.1 的检查项：缺 outer endpoint、地址族不一致、overlay 非法、GRE 创建失败、
地址配置失败、置 UP 失败、READY 前超时、重复 add 幂等、同名不同 endpoint 拒绝、rebind、删除重建、
设备丢失与 ifindex 变化、自主设备丢失、事件顺序与 overlay 载荷、失败保留 ifindex、空名重建、
查询路径不通知、丢失后无残留簿记、自主 GRE 通知失败、内存分配失败）；
`gre-registry-test` **3** 个用例；`dp-backend-test` **11** 个用例。
这三个程序用 `assert()` 判定（失败即 abort/非 0），没有内部 PASS 计数器，因此只能给出“进程 PASS + 退出码 0”。

### 3.5 GRE 双容器连通性 `midr-test/midr-gre-connectivity-test.sh`

```text
$ echo <pw> | sudo -S docker cp frr-ubuntu24-ymy:/home/frr/frr-midrd3/midr-test/midr-gre-connectivity-test.sh /tmp/t2-gre-test.sh
$ sha256sum /tmp/t2-gre-test.sh
487ee932e332f0ca10d17b9710de5a1fbac5c1a59005b849d84d801dd05ced5c      # == 本地 HEAD 版本
$ echo <pw> | sudo -S bash /tmp/t2-gre-test.sh        # 在 docker 宿主 root 上跑，node1/node2 双容器
...
=== summary: PASS=58 FAIL=0 ===
```

关键原始行（节选）：

```text
[PASS] zebra running on node1      [PASS] zebra running on node2
[PASS] underlay IPv4 10.1.1.11 -> 10.1.1.12
==== A. IPv4 GRE tunnel (gre) + IPv4 overlay via virtual-link API ====
[PASS] node1 gre1 state=READY via API (ifindex=75)
[PASS] node1 gre1 returned ifindex 75 matches kernel 75
[PASS] node1 gre1 READY overlay_local=192.168.100.1 overlay_remote=192.168.100.2 plen=30 event=ready
[PASS] node1 gre1 is a gre device
[PASS] node1 gre1 overlay 192.168.100.1/30 configured
[PASS] IPv4 connectivity node1 -> node2 over gre1 / (reverse)
==== B. IPv6 overlay over the IPv4 GRE tunnel (gre2) ====
[PASS] node1 gre2 overlay fd00:100::1/64 configured
[PASS] IPv6 connectivity node1 -> node2 over gre2 / (reverse)
==== C. IPv6 GRE tunnel (ip6gre) + IPv6 overlay ====
[PASS] node1 gre6 is an ip6gre device
[PASS] node1 gre6 overlay fd00:200::1/64 configured
[PASS] IPv6 connectivity node1 -> node2 over gre6 / (reverse)
==== D. establishment-status API (idempotent re-add) ====
[PASS] node1 gre1 re-add state=READY ifindex=75
==== E. Teardown ====
[PASS] node1 gre1 removed / gre2 removed / gre6 removed
[PASS] node2 gre1 removed / gre2 removed / gre6 removed
[PASS] node1 gre1 overlay address cleaned / node2 gre1 overlay address cleaned
==== F. Real-kernel EEXIST: a live foreign tunnel on the same outer pair ====
[PASS] F.2 same outer pair refused (err=110 DEVICE_CONFIRM timeout; the EEXIST guard is process-local)
[PASS] F.7 kernel GRE device count unchanged by the refusal (2 devices)
[PASS] F.3 refusal created no device on node1
[PASS] F.4 foreign device untouched (ifindex 78, addresses 0)
[PASS] F.5 control: after the foreign tunnel is gone the same request reaches READY
[PASS] F.6 vlx removed by teardown
```

计数：**PASS=58 / FAIL=0**（脚本自报）。注意 F.2 的实际结果是 **err=110**（DEVICE_CONFIRM 超时），
脚本显式接受 17 或 110 两种拒绝形态；也就是说“另一进程创建的设备无法被本进程注册表识别”这一
**已知限制**在本轮被真实复现并记录，而不是静默通过。

### 3.6 Stage E 跨 BGP-only underlay `midr-test/r7-dp-stagee-bgp-underlay.sh`

```text
$ echo <pw> | sudo -S docker cp frr-ubuntu24-ymy:/home/frr/frr-midrd3/midr-test/r7-dp-stagee-bgp-underlay.sh /tmp/t2-stagee.sh
$ sha256sum /tmp/t2-stagee.sh
a5ab02112125d062df778f6da4cd3c9d10ca83e6b618d3f2b487a7f1192a07b8      # == 本地 HEAD 版本
$ echo <pw> | sudo -S bash /tmp/t2-stagee.sh
=== stage E summary: PASS=34 FAIL=0 ===
```

关键原始行（节选；E1 专门反证“必须靠 BGP 而不是直连路由”）：

```text
--- midra ip route show ---
10.20.1.0/24 dev eth0 proto kernel scope link src 10.20.1.1
10.20.2.0/24 nhid 6 via 10.20.1.254 dev eth0 proto bgp metric 20
[PASS] E1.1 midra has 10.20.2.0/24 via 10.20.1.254 proto bgp
[PASS] E1.2 no connected route to 10.20.2.0/24 on midra
[PASS] E1.3 ip route get 10.20.2.1 on midra resolves via 10.20.1.254 (BGP nexthop)
[PASS] E1.4 midra -> 10.20.2.1 ping works through the BGP-only underlay
[PASS] E2.1 midra state=READY via API (ifindex=8)
[PASS] E2.3 midra API ifindex 8 == kernel 8
[PASS] E2.4 midrb API ifindex 8 == kernel 8
[PASS] E3.3 midra carries exactly 192.168.100.1/30
[PASS] E3.4 midrb carries exactly 192.168.100.2/30
[PASS] E4.1 midra -> 192.168.100.2 over gre1 / E4.2 反向
[PASS] E5.1 midra route get 192.168.100.2 hits dev gre1
[PASS] E6.1 midra installed a FIB route keyed by gre1 (ifindex 8)
[PASS] E6.2 FIB entry present (via 192.168.100.2 dev gre1)
[PASS] E6.3 dataplane lookup for 198.51.100.1 resolves to gre1
[PASS] E7.1 midra gre1 removed by teardown / E7.1 midrb gre1 removed by teardown
[PASS] E7.2 rebuild after teardown -> READY on both ends (A=9 B=9)
[PASS] E7.3 overlay ping again after rebuild
[PASS] E7.4 after zebra restart re-establish -> READY, ifindex 10 == kernel 10
[PASS] E7.5 overlay ping after zebra restart
[PASS] E7.6 kernel device deleted -> recreated, READY, ifindex 10
```

计数：**PASS=34 / FAIL=0**。该脚本自建 `midra/midrb/midrrtr` 三个容器与两条互不相通的 bridge；
本轮跑完后已按脚本提示清理（`docker rm -f midra midrb midrrtr; docker network rm midr-stagee-a midr-stagee-b`）。

### 3.7 第三组 ZAPI/FIB smoke（补充，`midr-test/r7-dp-fib-smoke.sh`，容器内 root）

```text
$ docker exec -d -u root frr-ubuntu24-ymy bash -c \
    "cd /home/frr/frr-midrd3 && nohup bash midr-test/r7-dp-fib-smoke.sh > /tmp/t2-fibsmoke.log 2>&1 &"
$ grep -cE '\[PASS\]' /tmp/t2-fibsmoke.log ; grep -cE '\[FAIL\]' /tmp/t2-fibsmoke.log
26
0
=== summary: PASS=26 FAIL=0 ===
third-group ZAPI/FIB smoke: PASS
```

覆盖 A–E 五段（facade 直连 add/delete、TED→SPF→adapter→FIB + zebra 重启重放 + 关机撤销、
ECMP/UCMP/IPv6 形状、去重与替换、zebra 宕机期间的延迟批失败与重连收敛）。计数：**PASS=26 / FAIL=0**。

### 3.8 本轮计数汇总

| # | 项目 | 命令入口 | 计数 |
| --- | --- | --- | --- |
| 1 | ZAPI 编号门禁（4 种模式） | `midr-test/check-zapi-numbering.sh`（R1/R2/R3/R4） | **PASS 4 / FAIL 0** |
| 2 | 组件套件（出厂清单） | `midr-test/run-midr-component-suite.sh <root>` | **FAIL（exit 2，0 个程序被跑）** |
| 3 | 组件套件（推导清单，非特权 frr） | `run-midr-component-suite.sh <root> /tmp/t2-manifest.txt` | **programs 30 PASS / 0 FAIL；fixtures PASS** |
| 4 | 组件套件（推导清单，root） | 同上，`-u root` | **programs 29 PASS / 1 FAIL；fixtures FAIL** |
| 5 | midrd 组件套件 | `cd midrd && make BUILD_DIR=/tmp/midrd-build all test` | **PASS 27 / FAIL 0** |
| 6 | GRE 双容器连通性 | `midr-test/midr-gre-connectivity-test.sh` | **PASS 58 / FAIL 0** |
| 7 | Stage E 跨 BGP-only underlay | `midr-test/r7-dp-stagee-bgp-underlay.sh` | **PASS 34 / FAIL 0** |
| 8 | ZAPI/FIB smoke | `midr-test/r7-dp-fib-smoke.sh` | **PASS 26 / FAIL 0** |

脚本自带计数合计：**PASS=179，FAIL=3**（= 项目 2 的一次门禁失败 + 项目 4 的 1 个程序失败 + 1 个 fixtures 失败）。
项目 6 的 `ifindex=75`、项目 7 的 `ifindex=8/9/10`、§4 的 `ifindex=80/81` 都是**本轮**真实返回值，
数值不同只是因为容器/轮次的接口创建顺序不同，不是跨轮复制。

---

## 4. 内核级证据（不只看 PASS 计数）

脚本：`/tmp/t2-kernel-evidence.sh`（在 docker 宿主 root 上跑，操作 `node1`/`node2` 两个容器；
用的是 §3.5 那次测试留在两节点 `/opt/midr-dp` 的**本次构建产物**）：

```text
=== artifact identity ===
8c86bae2428c5918506b92a248fd58ceacbd66c20ead451f4f35e76d9f0723c0  /opt/midr-dp/midrd-gre-tool
279923337baeb953c830a72221608c66b1c2943e12b034daffd3e1ae61123d61  /opt/midr-dp/zebra
-rwxr-xr-x 1 root root 235168 Sep 29 14:35 /opt/midr-dp/midrd-gre-tool
0                              # ldd | grep -c 'not found'
```

设备**只通过虚链路 API 创建**（`vlink-setup --overlay-local/--overlay-remote/--overlay-prefix`），
全程没有执行 `ip addr add` / `ip link set ... up`。API 返回：

```text
--- node1 API output ---
    [notify] gre1: state=device_up ifindex=80 iftype=0 err=0 overlay_local=192.168.100.1 overlay_remote=192.168.100.2 overlay_plen=30 overlay_ready=0 event=device_up
    [notify] gre1: state=configuring ifindex=80 iftype=0 err=0 overlay_local=192.168.100.1 overlay_remote=192.168.100.2 overlay_plen=30 overlay_ready=0 event=address_set
    [notify] gre1: state=ready ifindex=80 iftype=8 err=0 overlay_local=192.168.100.1 overlay_remote=192.168.100.2 overlay_plen=30 overlay_ready=1 event=ready
MIDR_VLINK name=gre1 state=ready ifindex=80 iftype=8 err=0 overlay_local=192.168.100.1 overlay_remote=192.168.100.2 overlay_plen=30 overlay_ready=1 event=none
```

即 READY 之前存在有序三条通知 `DEVICE_UP → ADDRESS_SET → READY`，`overlay_ready` 只在 `ready` 事件上变 1。
（`iftype=8` 是虚链路的内部枚举；内核 `type` 见下，是 `778`。）

### K1 设备类型（`ip -d link show`）

```text
=== K1: node1 ip -d link show gre1 (device type) ===
80: gre1@NONE: <POINTOPOINT,UP,LOWER_UP> mtu 1400 qdisc noqueue state UNKNOWN mode DEFAULT group default qlen 1000
    link/gre 10.1.1.11 peer 10.1.1.12 promiscuity 0 minmtu 0 maxmtu 0
    gre remote 10.1.1.12 local 10.1.1.11 ttl inherit addrgenmode eui64 numtxqueues 1 ...
```

→ 内核链路类型是 **`gre`**（IPv4 GRE），outer local/remote = 10.1.1.11/10.1.1.12。

```text
=== K7: node1 ip -d link show gre6 (ip6gre) ===
81: gre6@NONE: <POINTOPOINT,UP,LOWER_UP> mtu 1400 qdisc noqueue state UNKNOWN mode DEFAULT group default qlen 1000
    link/gre6 fd00:1::11 peer fd00:1::12 permaddr 8a7d:b451:65fd:: promiscuity 0 minmtu 0 maxmtu 0
    ip6gre remote fd00:1::12 local fd00:1::11 hoplimit inherit encaplimit 0 tclass 0x00 flowlabel 0x00000 ...
```

→ 第二个设备是 **`ip6gre`**（`link/gre6` + `ip6gre remote/local`）。

### K2 ifindex 与内核 type

```text
=== K2: node1 /sys/class/net/gre1/ifindex + iftype ===
80
778
80: gre1@NONE: <POINTOPOINT,UP,LOWER_UP> mtu 1400 ...
```

→ `/sys/class/net/gre1/ifindex` = **80**（与 API 返回的 `ifindex=80` 一致）；
`/sys/class/net/gre1/type` = **778**（ARPHRD_IPGRE）。gre6 的 ifindex 见 K7 的 `81:`。

### K3 接口状态（IFF_UP）

```text
=== K3: node1 IFF_UP / operstate ===
unknown
80: gre1@NONE: <POINTOPOINT,UP,LOWER_UP> mtu 1400 ...
```

→ `operstate=unknown` 是点对点隧道的正常值；关键是 flags 里带 **`UP`**（IFF_UP）与 `LOWER_UP`。
Stage E 的 `E3.2 <node> gre1 is UP` 是同一判定（`ip link show gre1 | grep -q UP`）。

### K4 overlay 地址：必须是 host 地址而不是网络地址（验证 countersign C4）

```text
=== K4: node1 overlay address (host address, NOT the network address) ===
80: gre1    inet 192.168.100.1/30 brd 192.168.100.3 scope global gre1       valid_lft forever preferred_lft forever
80: gre1    inet6 fe80::a01:10b/64 scope link                               valid_lft forever preferred_lft forever
0            ← ip -4 addr show dev gre1 | grep -c '192.168.100.0/30'
```

```text
=== K8: node1 gre6 overlay address + route get ===
81: gre6    inet6 fd00:100::1/64 scope global       valid_lft forever preferred_lft forever
fd00:100::2 from :: dev gre6 proto kernel src fd00:100::1 metric 256 pref medium
```

→ 内核里装的是 **`192.168.100.1/30`**（host 地址），**`192.168.100.0/30` 命中数为 0**。
这与 countersign C4 描述的缺陷（旧实现把主机位清零、装成 `192.168.100.0/30`）形成直接反证。
源码侧一致：`zebra/zapi_msg.c:4262 zebra_midr_ipaddr_to_prefix()` 直接把地址逐字节复制
（`p->u.prefix4 = ia->ipaddr_v4;`），文件里 `grep -n apply_mask zebra/zapi_msg.c` **无任何命中**。
Stage E 同样独立证实两端分别是 `192.168.100.1/30` / `192.168.100.2/30`（`E3.3`/`E3.4`）。

### K5 `ip route get <overlay_remote>` 命中虚接口

```text
=== K5: node1 ip route get overlay remote ===
192.168.100.2 dev gre1 src 192.168.100.1 uid 0
    cache
```

```text
--- midra ip route get 192.168.100.2 ---（Stage E）
192.168.100.2 dev gre1 src 192.168.100.1 uid 0
```

→ 选路确实走 `gre1`，源地址是 overlay host 地址。

### K6 overlay 双向 ping

```text
=== K6: overlay ping node1 -> node2 and node2 -> node1 ===
PING 192.168.100.2 ... 3 packets transmitted, 3 received, 0% packet loss, time 2031ms      ping_exit=0
PING 192.168.100.1 ... 3 packets transmitted, 3 received, 0% packet loss, time 2043ms      ping_exit=0
```

```text
=== K9: overlay ping6 both directions ===（fd00:100::1 <-> fd00:100::2 over ip6gre）
3 packets transmitted, 3 received, 0% packet loss      ping6_exit=0  （两个方向都是）
```

### K10/K11 拆除后清理

```text
=== teardown through the API ===
node1 teardown gre1 exit=0      node2 teardown gre1 exit=0
node1 teardown gre6 exit=0      node2 teardown gre6 exit=0
=== K10: post-teardown state (devices and addresses must be gone) ===
Device "gre1" does not exist.  rc=1
Device "gre6" does not exist.  rc=1
Device "gre1" does not exist.  rc=1        （node2 同样）
Device "gre6" does not exist.  rc=1
2: gre0@NONE: <NOARP> mtu 1476 qdisc noop state DOWN mode DEFAULT group default qlen 1000\    link/gre 0.0.0.0 brd 0.0.0.0
no_gre_devices_left
=== K11: overlay address must no longer be anywhere in node1 ===
0        ← ip -o -4 addr show | grep -c '192.168.100.1'
0        ← ip -o -6 addr show | grep -c 'fd00:100::1'
```

→ 设备（gre1/gre6）与 overlay 地址都被清掉了；`ip -o link show type gre` 剩下的唯一一条是
**内核自带的 `gre0`**（`<NOARP> state DOWN`，任何 Linux 都有），不是我们创建的。脚本 F.7 也核对过
“拒绝请求前后内核 GRE 设备数不变（2 台）”，与此一致。

（K12 想抓 zebra 日志里的接口事件，但本轮 zebra 的 `log file /tmp/zebra.log` 未落盘
—— zebra 输出走的是容器 stdout，已在原始日志里；该项**未取得**，不影响上面的结论。）

---

## 5. 失败与未验证项（含原因）

### 5.1 本轮真实失败（可复现）

| 项 | 现象（原始） | 归因 |
| --- | --- | --- |
| 出厂 manifest 门禁 | `run-midr-component-suite.sh <root>` → `suite_exit=2`，`registered`/`manifest-sorted` diff 显示 7 项缺失（`test_midr_admission`、`test_midr_ip2asn_update`、`test_midr_tier1`、`test_midr_tier1_list`、`test_midr_trace_{engine,scheduler,udp}`） | **真实门禁缺陷**：`midr-test/midr-component-tests.txt` 与 `tests/bgpd/subdir.am` 不同步（后者第 311/356 行等）。与虚链路逻辑无关，但会让全量组件门禁在 HEAD 上跑不起来 |
| 组件套件以 root 运行 | `test_midr_sequence rc=134`，`test_midr_sequence.c:282` 断言 `... == -EACCES` 失败 | **运行身份问题**：该用例假定非特权（无权限写 FRR store）。换成 `-u frr` 后同用例 PASS（3.2.2） |
| 组件套件 fixtures | `FAIL: C fixture runner not found: .../tests/bgpd/test_midr_ted_fixture` | 该二进制当时未构建；构建后 fixtures PASS |
| 首次构建（默认用户 `frr`） | `touch: cannot touch 'config.h.in': Permission denied` / `make: *** [Makefile:7717: config.h.in] Error 1` | **权限问题**：树内生成物属主为 root。加 `-u root` 后 `make_exit=0` |

### 5.2 未能验证（含原因，未做推测）

1. **顶层 `make check` / `tests/topotests` 全量 topotest**：未运行。原因：本轮目标是 MIDR 虚链路/GRE 数据面；
   全量 topotest 需要数小时且有大量与本分支无关的用例，不属于本次验收范围。
2. **`midr-test/run-midr-component-gate.sh worktree|image`、`run-midr-zebra-e2e-gate.sh`**：未运行。
   原因：脚本默认基线镜像 `frr-midr-p6:f9beedd0d653` 在本宿主的镜像列表里**不存在**
   （`docker images | grep -iE 'midr|p6'` 无命中，共 29 个镜像），worktree 模式需要的工具链镜像同样缺失。
   本轮用**同一套件脚本 + 推导清单**在既有容器内等价执行（3.2.2），但"外层门禁脚本本身"未被验证。
3. **共享基线同步检查**：`check-zapi-numbering.sh` 自报
   `origin/fix/midrd-integration-hardening not fetched; shared-baseline sync check skipped`，
   即该分支的“同名 subject 对齐”检查本轮**未覆盖**（本地仓库没有该 ref）。
4. **MIDR Session 是否真的承载在 GRE 虚链路上**（需求文档 §8、§10 第 9/10 条）。
   未验证。原因：需要第一组的 Session 调用与第二组的 LS 扩散栈；Stage E 只证明了
   `route get`/FIB 出口是 `gre1`，`E7.4/E7.5` 只证明重建后 overlay ping 通，
   **没有**跑 `midr_session_connect()` 也没有验证 HELLO/Established 的源地址。
   建议由第一组/第二组联合台子补。
5. **IPv6 underlay（fd00:1::/64）跨越 BGP-only underlay**：未运行。
   原因：`r7-dp-stagee-bgp-underlay.sh` 的 underlay 与 BGP 会话全部是 IPv4；本轮的 ip6gre 用例
   （GRE 测试 C 段、§4 的 K7–K9）都在**同一 bridge** 上，不是 BGP-only。
6. **containerlab 类台子**（`r7-containerlab-smoke.sh`、15 节点 `backbone-lab`、10 节点 `cl-test`）：
   未运行。原因：需要 containerlab 与相应镜像/台子，本轮未申请到这些资源。
7. **READY 的“overlay endpoint 可达性”负例**：未实测。
   静态阅读（`midrd/midr-virtual-link.c:335` `midr_virtual_link_overlay_reachable()`）可知
   READY 的确认是 `connected_lookup_prefix_exact(ifp, overlay_local/prefix) && if_is_up(ifp)`，
   即**本地接口视图精确匹配 + IFF_UP**，不含主动探测；我没有构造“本地地址已配但对端不可达”的场景来验证
   READY 是否会因此被拒绝。只有正例（可 ping 通）在本轮被验证。
8. **countersign C1–C3、C5–C7**：未逐条实测；本轮只覆盖 C4（overlay host 地址未被清零）。
9. **30 个组件程序内部用例计数**：不可得。原因：这些程序只打印一行 summary（例如
   `test_capability` 的 `failures: 0`），没有逐用例计数字段；`midrd/*` 测试用 `assert()`，失败即 abort。
10. **K12 zebra 日志接口事件**：未取得（`/tmp/zebra.log` 未落盘；zebra 输出走容器 stdout，已在原始日志中）。
11. **双容器 GRE 的 F.2 期望形态 `err=17`（EEXIST）**：本轮实际得到 `err=110`（DEVICE_CONFIRM 超时），
    脚本把两者都判为 PASS。即“另一进程创建的同 endpoint 设备”这一路径**以 110 的形态**被复现，
    EEXIST(17) 形态在本轮**未被观察到**。

---

## 6. 与 `midr-virtual-link-third-group-implementation.md` §9.2 / §9.3 / §10 的逐条对照

图例：**已证明** = 本轮有原始输出直接支撑；**部分** = 只证明了其中一部分；**未证明** = 本轮无证据（原因见 §5）。

### 6.1 §9.2 GRE/IP 隧道测试（6 项检查）

| §9.2 要求 | 本轮证据 | 判定 |
| --- | --- | --- |
| 设备类型正确（gre/ip6gre） | §4 K1 `link/gre 10.1.1.11 peer 10.1.1.12` + `gre remote/local`；K7 `link/gre6` + `ip6gre remote/local`；GRE 测试 A/B/C 段 `node1 gre1 is a gre device` / `gre6 is an ip6gre device` | **已证明** |
| 设备状态为 READY | GRE 测试 `node1 gre1 state=READY via API (ifindex=75)`；Stage E `E2.1/E2.2 … state=READY via API` | **已证明** |
| overlay 地址已配置 | §4 K4 `inet 192.168.100.1/30`、K8 `inet6 fd00:100::1/64`；GRE 测试 `overlay 192.168.100.1/30 configured`；Stage E `E3.3/E3.4` | **已证明** |
| overlay 双向 ping 成功 | §4 K6（v4 双向 0% loss）、K9（v6 双向 0% loss）；GRE 测试 A/B/C 六条 connectivity PASS；Stage E `E4.1/E4.2` | **已证明** |
| 返回 ifindex 有效 | §4 K2 API `ifindex=80` == `/sys/class/net/gre1/ifindex` 80；GRE 测试 `returned ifindex 75 matches kernel 75`；Stage E `E2.3/E2.4`（8==8）；E7.4（10==10） | **已证明** |
| 删除后设备和地址均清理 | §4 K10/K11（设备 `does not exist. rc=1`，地址 grep 计数 0）；GRE 测试 E 段 8 条 PASS；Stage E `E7.1` | **已证明** |

§9.2 的“**不再依赖外部手工地址配置**”（即把 `ip addr add` / `ip link set ... up` 换成第三组 API）：
本轮的内核证据脚本**只在 API 之后读内核**，对 gre1/gre6 没有任何 `ip addr add`/`ip link set` 调用
（脚本全文见 §7；手动命令只出现在 underlay 的 `eth0` 上，以及 Stage E 的 FIB 业务路由）。
判定：**已证明**。

### 6.2 §9.3 跨 BGP-only 区域验收（7 项）

| §9.3 要求 | 本轮证据（Stage E，`PASS=34 FAIL=0`） | 判定 |
| --- | --- | --- |
| 1. outer endpoint 经 underlay 可达 | `E1.1` BGP 路由 `10.20.2.0/24 via 10.20.1.254 proto bgp`；`E1.2` 无直连路由；`E1.3` `route get 10.20.2.1 … via 10.20.1.254`；`E1.4` ping 通 | **已证明** |
| 2. 两端 GRE 设备都进入 READY | `E2.1 midra state=READY`、`E2.2 midrb state=READY` | **已证明** |
| 3. overlay 地址和接口状态正确 | `E3.3 midra carries exactly 192.168.100.1/30`、`E3.4 midrb …/30`、`E3.2 … is UP` | **已证明** |
| 4. `route get overlay-remote` 选择虚接口 | `E5.1 midra route get 192.168.100.2 hits dev gre1`（原始输出 `192.168.100.2 dev gre1 src 192.168.100.1`） | **已证明** |
| 5. 返回的 ifindex 可交给第一组/第二组 | `E2.3/E2.4` API==内核；K2 同；API 返回与内核一致即“可交接”的运行时前提 | **部分**（第一组/第二组侧的实际消费未跑，见 §5.2 第 4 项） |
| 6. Zebra/FIB 能用该 ifindex 安装业务路由 | `E6.1` 安装 `198.51.100.0/24 via 192.168.100.2 dev gre1`；`E6.2` FIB 条目存在；`E6.3` `ip -j route get 198.51.100.1` 解析到 gre1；另 §3.7 ZAPI/FIB smoke `PASS=26 FAIL=0` | **已证明** |
| 7. 删除、Zebra 重连、设备重建后状态可恢复 | `E7.1` 两端删除干净；`E7.2` 重建 READY（A=9 B=9）；`E7.3` 重建后 ping 通；`E7.4` zebra 重启后 READY（10==10）；`E7.5` 重启后 ping 通；`E7.6` 内核设备被外部删除后重建 READY | **已证明** |

§9.3 尾部那句“第三组不需要决定 MIDR 节点是否建邻；第一组负责调用 Session，并由联合测试验证最终
MIDR Session/EOR/SPF/FIB 闭环”——本轮**没有**跑第一组 Session，故该闭环（Session 走 GRE）**未证明**（§5.2 第 4 项）。


### 6.3 §10 完成标准（10 条）

| §10 完成标准 | 本轮证据 | 判定 |
| --- | --- | --- |
| 1. 提供可被第一组直接调用的高层虚链路 API | 运行时确实通过该 API 完成建链（`vlink-setup`/`teardown`）；“第一组直接调用”属接口/契约层面（T1/T3） | **部分** |
| 2. API 不要求第一组执行 shell 或 raw netlink | §4 全部操作经 API；内核证据脚本对 gre1/gre6 无 `ip addr add`/`ip link set` | **已证明** |
| 3. API 完成 GRE 创建、overlay 地址配置和接口 UP | §4 K1（创建）、K4（地址）、K3（IFF_UP）；Stage E E2/E3 | **已证明** |
| 4. READY 明确表示设备、地址和本地数据面配置完成 | READY 前有 `device_up → address_set → ready` 三条有序通知，`overlay_ready` 仅在 ready 为 1；三者与 K1/K3/K4 的内核状态一致；且 READY 的判定条件是 `connected_lookup_prefix_exact()+if_is_up()`，与 K4 的 `192.168.100.1/30` 精确匹配互为印证 | **已证明** |
| 5. 能返回稳定有效的虚接口 ifindex | §4 K2、GRE 测试 D 段幂等重加返回同一 75、Stage E E2.3/E2.4 | **已证明** |
| 6. 支持删除、故障通知、Zebra 重连和重建 | 删除/重连/重建：E7.1–E7.6；故障通知：组件测试 case12/13/19（`midrd-virtual-link-test: PASS`）与 GRE 测试 F 段 | **已证明**（故障通知为组件级，非内核级） |
| 7. 组件测试覆盖失败回滚、幂等和状态转换 | §3.4 21 个用例（含 `gre_create_failure`/`address_set_failure`/`admin_up_failure`/`device_confirm_timeout`/`idempotent_add`/`delete_rebuild`/`device_loss_and_ifindex_change`）+ §3.2 30 程序 0 失败 | **已证明** |
| 8. GRE/IP6GRE 测试不再依赖外部手工地址配置 | 同 §9.2 末条 | **已证明** |
| 9. 跨 BGP-only underlay 的联合验收能够证明实际出口为 GRE 虚接口 | Stage E E1–E7（含 `route get`/FIB 出口为 gre1）；但“MIDR 业务流量”上限是 FIB 层，未含 MIDR Session | **部分**（设备/路由层面已证明；Session 层未证明） |
| 10. 与第一组明确 READY、FAILED、DOWN、ifindex 和 overlay 地址的接口语义 | 运行时确实产生 `state=ready/device_up/configuring`、`event=ready/device_up/address_set`、`err=110` 拒绝、`overlay_*` 三件套（见 §3.5、§4）；“与第一组明确”属文档/契约层面 | **部分**（运行时语义已观测，跨组确认未在本轮） |

### 6.4 本轮未覆盖的需求条目

- §6 的“overlay endpoint 可达性检查失败时不得报 READY”：静态看 READY 用的是接口视图精确前缀 + IFF_UP
  （`midrd/midr-virtual-link.c:335`），**不是**主动探测；本轮没做对端不可达的负例，故该条**未实测**。
- §8“Session 实际选路”：见 §5.2 第 4 项，**未验证**。
- countersign C5（`overlay_remote` 必须与 `overlay_local` 同前缀）本轮只在全部正例中隐式成立，
  没有专门构造跨前缀负例 → **未单独验证**。



---

## 7. 复现步骤（第三方照着做即可）

约定：

```text
SSH="ssh -i ~/.ssh/frr -p 50022 -o StrictHostKeyChecking=no yangmy@101.6.30.220"
SUDO='echo thu325325 | sudo -S'
DEX="echo thu325325 | sudo -S docker exec -u root frr-ubuntu24-ymy"
```

### 7.1 同步被测源码

```bash
SHA=$(git -C /Users/yangmengyu/githubdocuments/frr rev-parse HEAD)   # 027bde5bc8ad0a6ac4d8afb3b2286014a6641ce1
git -C /Users/yangmengyu/githubdocuments/frr archive --format=tar --prefix=frrsrc/ HEAD -o /tmp/midr-src-$SHA.tar
gzip -9 -f /tmp/midr-src-$SHA.tar && shasum -a 256 /tmp/midr-src-$SHA.tar.gz
scp -i ~/.ssh/frr -P 50022 /tmp/midr-src-$SHA.tar.gz yangmy@101.6.30.220:/tmp/
$SSH "$SUDO sha256sum /tmp/midr-src-$SHA.tar.gz"                     # 必须与本地一致
$SSH "echo thu325325 | sudo -S docker cp /tmp/midr-src-$SHA.tar.gz frr-ubuntu24-ymy:/tmp/"
$SSH "$DEX bash -lc 'sha256sum /tmp/midr-src-$SHA.tar.gz'"           # 必须与本地一致
$SSH "$DEX bash -lc 'tar -xzf /tmp/midr-src-$SHA.tar.gz --strip-components=1 -C /home/frr/frr-midrd3'"
```

（本环境用冻结包而不是 `rsync --delete`：容器内的树含 build 产物，`--delete` 会误删 `config.h`、
`Makefile` 等生成物。内容一致性由 §1.3 的逐文件 sha256 比对保证。）

### 7.2 构建（**必须 `-u root`**；容器默认用户是 `frr`）

```bash
$SSH "$DEX bash -lc 'cd /home/frr/frr-midrd3 && sh bootstrap.sh && ./configure && make -j112'"
MAP=$(grep -v '^#' /Users/yangmengyu/githubdocuments/frr/midr-test/midr-component-tests.txt | sed 's|^|tests/bgpd/|' | tr '\n' ' ')
$SSH "$DEX bash -lc 'cd /home/frr/frr-midrd3 && make -j112 $MAP zebra/zebra bgpd/bgpd midrd/midrd'"
$SSH "$DEX bash -lc 'cd /home/frr/frr-midrd3/midrd && make -j112 BUILD_DIR=/tmp/midrd-build all test'"
$SSH "$DEX bash -lc 'cd /home/frr/frr-midrd3 && make -j112 tests/bgpd/test_midr_ted_fixture'"   # fixtures 需要
```

### 7.3 ZAPI 编号门禁（容器树无 `.git`，先把对象库搬进去）

```bash
git -C /Users/yangmengyu/githubdocuments/frr bundle create /tmp/frr-full.bundle --all
scp -i ~/.ssh/frr -P 50022 /tmp/frr-full.bundle yangmy@101.6.30.220:/tmp/
$SSH "echo thu325325 | sudo -S docker cp /tmp/frr-full.bundle frr-ubuntu24-ymy:/tmp/"
$SSH "$DEX bash -lc '
  git clone -q /tmp/frr-full.bundle /home/frr/frr-zapi-git && cd /home/frr/frr-zapi-git &&
  ./midr-test/check-zapi-numbering.sh &&                       # R1
  ./midr-test/check-zapi-numbering.sh --self-test &&           # R3
  ./midr-test/check-zapi-numbering.sh WORKTREE &&              # R4
  cd /home/frr/frr-midrd3 && GIT_DIR=/home/frr/frr-zapi-git/.git GIT_WORK_TREE=/home/frr/frr-midrd3 \
      ./midr-test/check-zapi-numbering.sh'                     # R2（对真实构建树）
```

### 7.4 组件套件（非特权；用 `subdir.am` 推导清单）

```bash
$SSH "$DEX bash -lc 'cd /home/frr/frr-midrd3 &&
  sed -n \"s|^check_PROGRAMS += tests/bgpd/\\(test_midr_[A-Za-z0-9_]*\\)$|\\1|p\" tests/bgpd/subdir.am \
    | grep -v -E \"^(test_midr_ted_fixture|test_midr_zebra_e2e)$\" > /tmp/t2-manifest.txt &&
  sed -n \"s|^check_PROGRAMS += tests/bgpd/\\(test_capability\\)$|\\1|p\" tests/bgpd/subdir.am >> /tmp/t2-manifest.txt &&
  sort -u /tmp/t2-manifest.txt -o /tmp/t2-manifest.txt'"

# 注意：不要用 -u root（会让 test_midr_sequence 的 EACCES 断言失败）
$SSH "echo thu325325 | sudo -S docker exec -u frr frr-ubuntu24-ymy bash -lc \
   'cd /home/frr/frr-midrd3 && bash midr-test/run-midr-component-suite.sh /home/frr/frr-midrd3 /tmp/t2-manifest.txt'"
# 期望：COMPONENT_SUMMARY programs_passed=30 programs_failed=0 fixtures=passed / COMPONENT_RESULT=0
```

若想复现 §3.2.1 的失败，把第二个参数去掉即可（`suite_exit=2`）。

### 7.5 双容器 GRE 连通性（在 docker **宿主** root 上）

```bash
$SSH "$SUDO docker cp frr-ubuntu24-ymy:/home/frr/frr-midrd3/midr-test/midr-gre-connectivity-test.sh /tmp/t2-gre-test.sh"
$SSH "$SUDO bash /tmp/t2-gre-test.sh"       # 期望：=== summary: PASS=58 FAIL=0 ===
```

### 7.6 Stage E（跨 BGP-only underlay，在宿主 root 上）

```bash
$SSH "$SUDO docker cp frr-ubuntu24-ymy:/home/frr/frr-midrd3/midr-test/r7-dp-stagee-bgp-underlay.sh /tmp/t2-stagee.sh"
$SSH "$SUDO bash /tmp/t2-stagee.sh"         # 期望：=== stage E summary: PASS=34 FAIL=0 ===
$SSH "$SUDO bash -c 'docker rm -f midra midrb midrrtr; docker network rm midr-stagee-a midr-stagee-b'"   # 脚本不会自动清理
```


### 7.7 内核级证据（最短可复现序列，宿主 root；`node1/node2` 需已跑过 §7.5 以取得 `/opt/midr-dp`）

```bash
# 起 zebra（两节点）
$SSH "$SUDO docker exec -u root node1 bash -c 'pkill -9 zebra; printf \"hostname n1\nlog file /tmp/zebra.log\n\" > /tmp/zebra.conf;
  LD_LIBRARY_PATH=/opt/midr-dp/lib /opt/midr-dp/zebra -u root -g root -f /tmp/zebra.conf -i /tmp/zebra.pid \
      -z /tmp/zserv_midr.api --vty_socket /tmp -d'"
$SSH "$SUDO docker exec -u root node2 bash -c 'pkill -9 zebra; printf \"hostname n2\nlog file /tmp/zebra.log\n\" > /tmp/zebra.conf;
  LD_LIBRARY_PATH=/opt/midr-dp/lib /opt/midr-dp/zebra -u root -g root -f /tmp/zebra.conf -i /tmp/zebra.pid \
      -z /tmp/zserv_midr.api --vty_socket /tmp -d'"
sleep 3

# 只用虚链路 API 建链（不要 ip addr add / ip link set）
$SSH "$SUDO docker exec -u root node1 bash -c 'LD_LIBRARY_PATH=/opt/midr-dp/lib /opt/midr-dp/midrd-gre-tool \
  vlink-setup --sock /tmp/zserv_midr.api --name gre1 --local 10.1.1.11 --remote 10.1.1.12 --mtu 1400 --wait-ms 8000 \
  --overlay-local 192.168.100.1 --overlay-remote 192.168.100.2 --overlay-prefix 30'"
$SSH "$SUDO docker exec -u root node2 bash -c 'LD_LIBRARY_PATH=/opt/midr-dp/lib /opt/midr-dp/midrd-gre-tool \
  vlink-setup --sock /tmp/zserv_midr.api --name gre1 --local 10.1.1.12 --remote 10.1.1.11 --mtu 1400 --wait-ms 8000 \
  --overlay-local 192.168.100.2 --overlay-remote 192.168.100.1 --overlay-prefix 30'"

# 内核证据
$SSH "$SUDO docker exec -u root node1 bash -c 'ip -d link show gre1'"
$SSH "$SUDO docker exec -u root node1 bash -c 'cat /sys/class/net/gre1/ifindex; cat /sys/class/net/gre1/type'"
$SSH "$SUDO docker exec -u root node1 bash -c 'ip -o addr show dev gre1; ip -4 addr show dev gre1 | grep -c \"192.168.100.0/30\"'"
$SSH "$SUDO docker exec -u root node1 bash -c 'ip route get 192.168.100.2'"
$SSH "$SUDO docker exec -u root node1 bash -c 'ping -c 3 192.168.100.2'"
$SSH "$SUDO docker exec -u root node2 bash -c 'ping -c 3 192.168.100.1'"      # 双向

# 拆除与清理核对
$SSH "$SUDO docker exec -u root node1 bash -c 'LD_LIBRARY_PATH=/opt/midr-dp/lib /opt/midr-dp/midrd-gre-tool \
  teardown --sock /tmp/zserv_midr.api --name gre1'"
$SSH "$SUDO docker exec -u root node2 bash -c 'LD_LIBRARY_PATH=/opt/midr-dp/lib /opt/midr-dp/midrd-gre-tool \
  teardown --sock /tmp/zserv_midr.api --name gre1'"
$SSH "$SUDO docker exec -u root node1 bash -c 'ip -d link show gre1; ip -o addr show dev gre1; \
  ip -o -4 addr show | grep -c 192.168.100.1'"
```

ip6gre 版本：`--local/--remote` 换成 `fd00:1::11/fd00:1::12`、`--overlay-*` 换成
`fd00:100::1/fd00:100::2 --overlay-prefix 64`，并先在两个节点 `eth0` 上各自
`ip addr add fd00:1::1{1,2}/64 dev eth0 nodad`。

### 7.8 ZAPI/FIB smoke（容器内 root）

```bash
$SSH "$DEX bash -lc 'cd /home/frr/frr-midrd3 && bash midr-test/r7-dp-fib-smoke.sh'"
# 期望：=== summary: PASS=26 FAIL=0 === / third-group ZAPI/FIB smoke: PASS
```

### 7.9 本轮原始日志（可复核）

| 日志 | 位置 | 内容 |
| --- | --- | --- |
| `/tmp/t2-build.log` | 容器 | 第一次（非 root）构建失败的完整输出 |
| `/tmp/t2-build-root.log` | 容器 | root 构建 + 组件二进制 + midrd `make test`（18560 行） |
| `/tmp/t2-zapi.log` | 容器 | ZAPI 门禁 4 种模式的输出 |
| `/tmp/t2-suite.log` | 容器 | 出厂清单套件失败 + 三个二进制单独运行 |
| `/tmp/t2-suite2.log` | 容器 | root 运行推导清单套件（29/1） |
| `/tmp/t2-suite3.log` | 容器 | 非特权运行推导清单套件（30/0 + fixtures PASS） |
| `/tmp/t2-fibsmoke.log` | 容器 | ZAPI/FIB smoke（PASS=26 FAIL=0） |
| `/tmp/t2-gre.log` | 宿主 | 双容器 GRE（PASS=58 FAIL=0） |
| `/tmp/t2-kernel.log` | 宿主 | 内核级证据 K1–K12 |
| `/tmp/t2-stagee.log` | 宿主 | Stage E（PASS=34 FAIL=0） |

本轮取证后环境状态：容器内多出 `/home/frr/frr-zapi-git`（bundle 克隆）与 `/tmp/frr-full.bundle`；
`midra/midrb/midrrtr` 与两条 stage E bridge 已清理；`node1`/`node2` 的 zebra 已停止；
`/opt/midr-dp`（本次构建产物）保留，供第三方复核。
