#!/bin/bash
# run_backbone_discovery.sh —— 发现链专题判据，跑在 **15 节点骨干台子**上。
#
# 被测改动（发现链专题 2026-08-21）：
#   ① 删探测 A（on_node_discovered + discovery_filter）——收到新节点不再无差别起探、
#      同群不再自动建连；
#   ② 回配接棒——midr_ctrl_connect() 的 SAME_GROUP 分支把对端纳入本群邻居
#      （查建条目 + is_adjacent + I-1 起探），notify CL 由调用方按批发；
#   ③ ctrl_connect 重排 + 刹车参数 send_nudge（回配不再回声）；
#   ④ 台账覆盖新规则（MANUAL 覆盖一切，其余先到先得）；
#   ⑤ 死心清位（建连死心后注销邻居身份）。
#
# 判据纪律（退网批教训，本批沿用）：
#   · 认机制不认结果——"条目在/位置上了"旧码也能凑出来，必须连"是谁干的"日志一起认；
#   · 拿不到正样本不算过——没造出场景报 exit 3，不给绿灯。
#
# 用法：sudo ./run_backbone_discovery.sh    （台子须已部署新 bgpd 且收敛）
set -u

R1=clab-midr-backbone-r1        # 群 1 代表（手配群号 1 + GROUP_REP + 挂靠边）
M1A=clab-midr-backbone-m1a      # 群 1 普通成员
M1B=clab-midr-backbone-m1b      # 群 1 普通成员
Z1=clab-midr-backbone-z1        # 无配置群号、靠引导 join 的节点（判据 1/2/9 主角）
R2=clab-midr-backbone-r2        # 群 2 代表（判据 4/5 的锚点边一端）
LOG=/etc/frr/logs/frr.log

Z1_TRANSPORT=10.99.0.191; Z1_RID=10.0.0.191
M1A_TRANSPORT=10.99.0.112; M1A_RID=10.0.0.112

PASS=0; FAIL=0; NOSAMPLE=0
pass()  { echo "  ✓ $1"; PASS=$((PASS+1)); }
fail()  { echo "  ✗ $1"; FAIL=$((FAIL+1)); }
nosam() { echo "  ⚠ 没造出场景：$1"; NOSAMPLE=$((NOSAMPLE+1)); }

v()        { docker exec "$1" vtysh -c "$2" 2>/dev/null; }
# 判据补 2 用它改群号造死心场景。本函数原先缺失（脚本第 276/277 行直接调用了
# 未定义的 conf，报 "command not found" → 改群号没执行 → 判据补2-a 恒 NOSAMPLE）。
# 2026-08-22 轮 4 步 0 回归时发现并补上，写法照抄 backbone-shutdown/ 同名函数。
conf()     { docker exec "$1" vtysh -c "configure terminal" -c "router bgp $2" -c "$3" 2>/dev/null; }
loglines() { docker exec "$1" sh -c "wc -l < $LOG" 2>/dev/null | tr -d ' '; }
logtail()  { docker exec "$1" sh -c "tail -n +$2 $LOG" 2>/dev/null; }
dbgon()    { docker exec "$1" vtysh -c "configure terminal" -c "log file $LOG debugging" \
                -c "debug bgp midr" -c "debug bgp midr discovery" >/dev/null 2>&1; }

echo "=============================================================="
echo " 发现链专题判据 —— 15 节点骨干台子（真多跳 / 真分离）"
echo "=============================================================="

for c in $R1 $M1A $M1B $Z1 $R2; do dbgon $c; done

# ---------------------------------------------------------------------------
# 判据 9（08-21 语义精确化）：退网零实害。
#   收包侧"同群就建连"保留后，残存条目仍会触发 r1 建一次边——但真边建不成：
#   z1 侧退网守卫丢弃全部 PEER_REQUEST、无对应 neighbor 配置故会话永停 Active、
#   PM 来源校验丢探测包。所以判的是"退网侧零反应 + 发起侧的假状态能自愈"。
#   ⚠ 件② 后残存条目来自第二组 LSDB 的传播窗口（不再是我方 NLRI 表），机理见
#   记档 41；现象与判据不变。
# ---------------------------------------------------------------------------
echo
echo "[判据 9] z1 退网：退网侧零反应，发起侧假状态自愈"
R1_BASE=$(loglines $R1); Z1_BASE=$(loglines $Z1)
docker exec $Z1 vtysh -c "configure terminal" -c "router bgp 65191" -c "midr shutdown" >/dev/null 2>&1
sleep 45
R1_LOG=$(logtail $R1 $R1_BASE); Z1_LOG=$(logtail $Z1 $Z1_BASE)

if echo "$R1_LOG" | grep -q "removing peer\|node gone\|MIDR CL: NODE_CHANGE"; then
    # z1 侧：必须零反应（只丢弃、不回配）
    if echo "$Z1_LOG" | grep -q "退网：丢弃控制通道"; then
        if echo "$Z1_LOG" | grep -q "peering back"; then
            fail "判据9-a：z1 退网后仍回配了（应一律丢弃）"
        else
            pass "判据9-a：z1 侧零反应（守卫丢弃 $(echo "$Z1_LOG" | grep -c '退网：丢弃控制通道') 条，无回配）"
        fi
    else
        nosam "z1 侧没收到任何控制消息（没造出'对端来敲门'的场景）"
    fi
    # r1 侧：会话不得 Established；且假状态要自愈
    if v $R1 "show midr neighbors" | grep "$Z1_TRANSPORT" | grep -q Established; then
        fail "判据9-b：r1 与已退网的 z1 竟建成了 Established 会话"
    else
        pass "判据9-b：r1 侧无 Established（会话建不成，符合预期）"
    fi
    if v $R1 "show midr nodes" | grep -q "$Z1_RID"; then
        fail "判据9-c：45s 后 r1 节点表里仍有 z1 条目（未自愈）"
    else
        pass "判据9-c：z1 条目已随 expire 清除，终态干净"
    fi
else
    nosam "r1 日志里没看到处理 z1 离开的痕迹（z1 可能本就不在其视图）"
fi

# 恢复 z1
docker exec $Z1 vtysh -c "configure terminal" -c "router bgp 65191" -c "no midr shutdown" >/dev/null 2>&1

# ---------------------------------------------------------------------------
# 判据 1 + 2：回配置位 + 老成员起探（本批核心）
#   重启 z1 让它重走一遍 join：z1 侧 connect_group 朝老成员发 PEER_REQUEST，
#   老成员（r1/m1a）走回配路把 z1 纳入本群邻居并起探。
#   认机制：不只看"位上了"，要看回配路的日志（peering back）+ PM I-1 起探。
# ---------------------------------------------------------------------------
echo
echo "[判据 1+2] z1 重新入群：老成员经回配置位 + 起探"
R1_BASE=$(loglines $R1); M1A_BASE=$(loglines $M1A)
docker exec -u root $Z1 /usr/lib/frr/frrinit.sh restart >/dev/null 2>&1
echo "  （z1 已重启，等 join 两段热身 ~150s）"
sleep 165
dbgon $Z1

R1_LOG=$(logtail $R1 $R1_BASE)
M1A_LOG=$(logtail $M1A $M1A_BASE)

# 1) 回配路径确实跑了（是谁干的）
if echo "$R1_LOG" | grep -q "PEER_REQUEST from rid $Z1_RID.*peering back"; then
    pass "判据1-a：r1 收到 z1 的 PEER_REQUEST 并走回配路（peering back）"
elif echo "$R1_LOG" | grep -q "peering back"; then
    pass "判据1-a：r1 走了回配路（peering back，未匹配到 z1 rid 字样但路径成立）"
else
    nosam "r1 日志里没有 'peering back' —— 回配路没被触发"
fi

# 2) 结果：z1 在 r1 的台账里是 SAME_GROUP，且会话 Established
if v $R1 "show midr neighbors" | grep "$Z1_TRANSPORT" | grep -q "SAME_GROUP"; then
    pass "判据1-b：r1 台账里 z1 记为 SAME_GROUP 且已建会话"
else
    fail "判据1-b：r1 台账里没有 z1 的 SAME_GROUP 条目"
    v $R1 "show midr neighbors" | sed 's/^/      /'
fi

# 3) 老成员对新成员起探（判据 2 的核心：删探测 A 后唯一来源就是回配）
if echo "$R1_LOG" | grep -q "start probing $Z1_RID"; then
    pass "判据2-a：r1 对 z1 起探（PM I-1 start probing，回配路触发）"
else
    fail "判据2-a：r1 没有对 z1 起探 —— 回配没接上，periodic_sync 将误判 LEAVE"
    echo "$R1_LOG" | grep "start probing" | tail -3 | sed 's/^/      /'
fi

# 4) 不误判 LEAVE（散群风险的直接反证）
if echo "$R1_LOG" | grep -q "LEAVE"; then
    fail "判据2-b：r1 出现 LEAVE 决策 —— 散群风险成真"
    echo "$R1_LOG" | grep "LEAVE" | tail -3 | sed 's/^/      /'
else
    pass "判据2-b：r1 无 LEAVE 决策（群未散）"
fi

# ---------------------------------------------------------------------------
# 判据 3：无 nudge 风暴（刹车生效，回配不再回声）
#   计 z1 重启窗口内 r1 收到/发出的 PEER_REQUEST 条数。回配不发 nudge，
#   故每条边每方向应只有 1 发（+ 未及时收敛时的 3s 重传，按 pending 节奏区分）。
# ---------------------------------------------------------------------------
echo
echo "[判据 3] 无 nudge 风暴"
SENT=$(echo "$R1_LOG" | grep -c "sent PEER_REQUEST to $Z1_TRANSPORT")
if [ "$SENT" -eq 0 ]; then
    pass "判据3：r1 未朝 z1 发 PEER_REQUEST（回配路刹车生效，不回声）"
elif [ "$SENT" -le 5 ]; then
    pass "判据3：r1 朝 z1 发了 $SENT 条 PEER_REQUEST（≤ 重传预算 5，非风暴）"
else
    fail "判据3：r1 朝 z1 发了 $SENT 条 PEER_REQUEST —— 疑似回声风暴"
fi

# ---------------------------------------------------------------------------
# 判据 4：ANCHOR / 挂靠行为与旧码基线一致（重排不伤及无辜）
#   基线（旧码实测）：r1 有 2 条 ATTACH；z1 有 2 条 CL_ANCHOR；r2 有 2 条 ATTACH + 2 条 CL_ANCHOR。
#   两类边都不该带 is_adjacent（它们不是本群邻居）。
# ---------------------------------------------------------------------------
echo
echo "[判据 4] ANCHOR / 挂靠 与基线一致"
R1_ATTACH=$(v $R1 "show midr neighbors" | grep -c "ATTACH")
Z1_ANCHOR=$(v $Z1 "show midr neighbors" | grep -c "CL_ANCHOR")
R2_ATTACH=$(v $R2 "show midr neighbors" | grep -c "ATTACH")

[ "$R1_ATTACH" -ge 2 ] && pass "判据4-a：r1 挂靠边 $R1_ATTACH 条（基线 2）" \
                       || fail "判据4-a：r1 挂靠边只有 $R1_ATTACH 条（基线 2）"
[ "$R2_ATTACH" -ge 2 ] && pass "判据4-b：r2 挂靠边 $R2_ATTACH 条（基线 2）" \
                       || fail "判据4-b：r2 挂靠边只有 $R2_ATTACH 条（基线 2）"
if [ "$Z1_ANCHOR" -ge 1 ]; then
    pass "判据4-c：z1 锚点边 $Z1_ANCHOR 条（基线 2，CL_ANCHOR 原因未被改写）"
else
    nosam "z1 这轮没建出 CL_ANCHOR 边（锚点评估未触发或候选群不足）"
fi

# ---------------------------------------------------------------------------
# 〔判据 6「真死心清位」已删除 —— 2026-08-22 轮 4 步 0 回归时定位并拍板〕
#
# 它自写出起就从未真正验证过，两处独立缺陷：
#   ① **构造假设了按设计不存在的行为**：原构造在 m1a 上 iptables 挡 5859，再用
#      `midr session` 在 z1 上手动建边，指望它发 PEER_REQUEST → 重传耗尽死心。
#      但 `midr session` 按设计**从不发 PEER_REQUEST**——MANUAL 边的约定是
#      **两端各配一次**（对端由运维自己敲，故无需 nudge 通知对方；命令回显的
#      MIDR_SESSION_SYMMETRY_HINT 与记档第 7 条"双端对称执行"即此约定）。
#      实证：`midr_ctrl_send_peer_request` 全树唯一调用点在 `midr_ctrl_connect`
#      的 send_nudge 分支（bgp_midr_ctrl.c:1797），而 midr session 命令
#      （bgp_midr_nds_vty.c）根本不走 midr_ctrl_connect。→ 恒报 NOSAMPLE，
#      而且看起来像"这次环境没凑巧"，极易被解释过去。
#   ② **grep 的日志串不存在**：原查 "MIDR 死心：注销"，实际文案是
#      "MIDR 边注销：…的本群邻居身份已撤"（bgp_midr_nds.c:1055）。即便侥幸造出
#      场景，第二层判据也会命中 fail 分支，报出**假 FAIL**。
#
# 它要验的语义（建连死心 → midr_nds_cleanup_by_transport → 停探 + 清 is_adjacent）
# **已由下方判据补 2-a 完整覆盖**：那条走真实可达的自动建连路径（改群号 → 重收敛
# → midr_ctrl_connect 发 PEER_REQUEST → 被两边对挡 → 重传耗尽死心），2026-08-22
# 已拿到正样本。留一条造不出场景的判据只会每轮产出假 NOSAMPLE、掩盖真问题。
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# 判据 7：直连同群双会话（静态 + overlay 并存）
#   骨干台子上成员多经过境相连；这里只核 overlay 会话确实只载 LS、
#   静态会话确实不载 LS（真分离的直接体现）。
# ---------------------------------------------------------------------------
echo
echo "[判据 7] 静态 / overlay 两层并存且各司其职"
SUM=$(v $R1 "show bgp summary")
if echo "$SUM" | grep -q "10.99.0."; then
    pass "判据7-a：r1 上有 overlay 会话（transport 10.99.0.x 为键）"
else
    fail "判据7-a：r1 上看不到 overlay 会话"
fi
if echo "$SUM" | grep -qE "10\.10\.[0-9]+\.[0-9]+"; then
    pass "判据7-b：r1 上有静态会话（链路地址 10.10.x.x 为键），与 overlay 并存"
else
    nosam "r1 上没有静态会话（该节点可能只有 overlay）"
fi

# ---------------------------------------------------------------------------
# 判据 8：全网无"静态邻居 activate link-state"残留
# ---------------------------------------------------------------------------
echo
# ⚠ 判据写法（08-21 修正）：不能只查 "neighbor .* activate"——overlay 会话是代码
# 动态 activate 的（setup_overlay_peer 必然要激活 LS），也在 running-config 里。
# 真正要查的残留 = **链路地址**（10.10.x.x）的 activate；overlay 用 transport（10.99.0.x）。
echo "[判据 8] 骨干台子无静态 LS activate 残留"
LEFT=0
for c in $R1 $R2 $M1A $M1B $Z1; do
    BAD=$(v $c "show running-config" \
            | awk '/address-family link-state/,/exit-address-family/' \
            | grep -c "neighbor 10\.10\.")
    if [ "$BAD" -gt 0 ]; then
        LEFT=$((LEFT+1)); echo "      $c 有 $BAD 条静态邻居(链路地址) activate"
    fi
done
[ "$LEFT" -eq 0 ] && pass "判据8：5 个受检节点的 LS 段全是 overlay(10.99.0.x)，零静态残留" \
                 || fail "判据8：$LEFT 个节点仍有静态 LS activate"

# ---------------------------------------------------------------------------
# 判据 补1：群内完全互联（本批核心 —— 删探测 A 引入的回归，靠"收包侧同群建连 +
#          periodic_sync 兜底扫描"两条腿修好）。旧败样本：m1a↔m1b 双向零会话。
# ---------------------------------------------------------------------------
echo
echo "[判据 补1] 群 1 五台完全互联"
MISS=0
for pair in "$M1A:10.99.0.113" "$M1B:10.99.0.112" "$M1A:10.99.0.111" "$M1B:10.99.0.111"; do
    c=${pair%%:*}; t=${pair##*:}
    v $c "show midr neighbors" | grep "$t" | grep -q Established \
        || { MISS=$((MISS+1)); echo "      $c 缺 $t"; }
done
[ "$MISS" -eq 0 ] && pass "判据补1：m1a/m1b 互连且各自连着 r1" \
                  || fail "判据补1：缺 $MISS 条群内边"

# ---------------------------------------------------------------------------
# 判据 补2：死锁自愈（两边对挡 → 恢复后 30s 一拍内靠兜底重连）
#   旧败：修复前同样场景 60s 不自愈，只能手动改群号救。
# ---------------------------------------------------------------------------
echo
echo "[判据 补2] 死锁自愈（两边对挡 179+5859 → 恢复）"
M1B_BASE=$(loglines $M1B)
for r in "$M1A:10.99.0.113" "$M1B:10.99.0.112"; do
    c=${r%%:*}; s=${r##*:}
    docker exec -u root $c iptables -A INPUT -s $s -p tcp --dport 179 -j DROP 2>/dev/null
    docker exec -u root $c iptables -A INPUT -s $s -p udp --dport 5859 -j DROP 2>/dev/null
done
if docker exec -u root $M1A iptables -L INPUT -n 2>/dev/null | grep -q 5859; then
    conf $M1B 65113 "midr group-id 2"; sleep 6
    conf $M1B 65113 "midr group-id 1"; sleep 30      # 等死心
    if logtail $M1B $M1B_BASE | grep -q "边注销"; then
        # 本条同时承接原判据 6「真死心清位」的语义（原判据已删，理由见上方留痕注释块）
        pass "判据补2-a：死心后完整清理生效（边注销 = 停探 + 清位）"
    else
        nosam "没等到死心/清理（对挡可能没生效）"
    fi
    # 放开网络，看兜底能否自愈
    docker exec -u root $M1A iptables -F INPUT 2>/dev/null
    docker exec -u root $M1B iptables -F INPUT 2>/dev/null
    T0=$(date +%s); OK=0
    while [ $(( $(date +%s) - T0 )) -le 90 ]; do
        v $M1B "show midr neighbors" | grep "10.99.0.112" | grep -q Established && { OK=1; break; }
        sleep 5
    done
    if [ "$OK" = 1 ]; then
        pass "判据补2-b：网络恢复后 $(( $(date +%s) - T0 ))s 内自愈（兜底扫描重连）"
        logtail $M1B $M1B_BASE | grep "补边兜底" | tail -2 | sed 's/^/      /'
    else
        fail "判据补2-b：90s 未自愈 —— 兜底扫描没起作用"
    fi
else
    nosam "容器里没有 iptables，判据补2 跳过"
fi

# ---------------------------------------------------------------------------
# 判据 补3：兜底不误触发（稳态应为 0；实验期的不算）
# ---------------------------------------------------------------------------
echo
echo "[判据 补3] 稳态无兜底误触发"
sleep 35   # 跨过一拍 periodic_sync
QUIET=0
for c in $R1 $Z1; do
    N0=$(docker exec $c sh -c "grep -c 补边兜底 $LOG" 2>/dev/null)
    sleep 35
    N1=$(docker exec $c sh -c "grep -c 补边兜底 $LOG" 2>/dev/null)
    [ "$N0" = "$N1" ] || { QUIET=1; echo "      $c 稳态仍在兜底（$N0 → $N1）"; }
done
[ "$QUIET" = 0 ] && pass "判据补3：r1/z1 稳态一拍内零兜底触发" \
                 || fail "判据补3：稳态出现兜底触发（疑似判据过宽）"

echo
echo "=============================================================="
echo " 结果：通过 $PASS，失败 $FAIL，没造出场景 $NOSAMPLE"
echo "=============================================================="
if [ "$FAIL" -gt 0 ]; then exit 1; fi
if [ "$NOSAMPLE" -gt 0 ]; then
    echo "⚠ 有判据没拿到正样本，不算通过（沉默 ≠ 通过）"
    exit 3
fi
exit 0
