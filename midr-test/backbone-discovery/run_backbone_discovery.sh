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
M2A=clab-midr-backbone-m2a      # Group 2 member
Z1=clab-midr-backbone-z1        # 无配置群号、靠引导 join 的节点（判据 1/2/9 主角）
R2=clab-midr-backbone-r2        # 群 2 代表（判据 4/5 的锚点边一端）
LOG=/etc/frr/logs/frr.log

Z1_TRANSPORT=10.99.0.191; Z1_RID=10.0.0.191
M1A_TRANSPORT=10.99.0.112; M1A_RID=10.0.0.112

PASS=0; FAIL=0; NOSAMPLE=0
Z1_SHUTDOWN=0
FAULT_RULES=0
pass()  { echo "  ✓ $1"; PASS=$((PASS+1)); }
fail()  { echo "  ✗ $1"; FAIL=$((FAIL+1)); }
nosam() { echo "  ⚠ 没造出场景：$1"; NOSAMPLE=$((NOSAMPLE+1)); }

v()        { docker exec "$1" vtysh -c "$2" 2>/dev/null; }
# Apply a BGP-scoped MIDR command.
conf()     { docker exec "$1" vtysh -c "configure terminal" -c "router bgp $2" -c "$3" 2>/dev/null; }
loglines() { docker exec "$1" sh -c "wc -l < $LOG" 2>/dev/null | tr -d ' '; }
logtail()  { docker exec "$1" sh -c "tail -n +$(( $2 + 1 )) $LOG" 2>/dev/null; }
dbgon()    { docker exec "$1" vtysh -c "configure terminal" \
                -c "debug bgp midr" -c "debug bgp midr discovery" >/dev/null 2>&1; }
groupid()  { v "$1" "show midr self" | awk -F: '/^Group-ID/{gsub(/ /,"",$2);print $2}'; }
retry_cleanup_complete() {
    local text="$1" transport="$2" rid="$3" half_line cleanup_line
    half_line=$(printf '%s\n' "$text" | grep -n \
        "拆除 $transport 的半边会话.*PEER_REQUEST 重传 5 次无回应" \
        | tail -1 | cut -d: -f1)
    cleanup_line=$(printf '%s\n' "$text" | grep -n \
        "边注销：$rid/32" | tail -1 | cut -d: -f1)
    [ -n "$half_line" ] && [ -n "$cleanup_line" ] && \
        [ "$cleanup_line" -gt "$half_line" ]
}

remove_fault_rule() {
    local container="$1" source="$2" protocol="$3" port="$4"
    while docker exec -u root "$container" iptables -C INPUT -s "$source" \
        -p "$protocol" --dport "$port" -j DROP >/dev/null 2>&1; do
        docker exec -u root "$container" iptables -D INPUT -s "$source" \
            -p "$protocol" --dport "$port" -j DROP >/dev/null 2>&1 || break
    done
}

clear_fault_rules() {
    remove_fault_rule "$M1A" 10.99.0.113 tcp 179
    remove_fault_rule "$M1A" 10.99.0.113 udp 5859
    remove_fault_rule "$M1B" 10.99.0.112 tcp 179
    remove_fault_rule "$M1B" 10.99.0.112 udp 5859
    FAULT_RULES=0
}

restore_test_state() {
    local rc=$?
    trap - EXIT INT TERM
    if [ "$FAULT_RULES" -eq 1 ]; then clear_fault_rules; fi
    if [ "$Z1_SHUTDOWN" -eq 1 ]; then
        conf "$Z1" 65191 "no midr shutdown" >/dev/null 2>&1 || true
    fi
    exit "$rc"
}

restart_node() {
    local container="$1" elapsed=0
    timeout 15s docker exec -u root "$container" \
        /usr/lib/frr/frrinit.sh stop >/dev/null 2>&1 || true
    docker exec -u root "$container" sh -c \
        'pkill -9 -x watchfrr 2>/dev/null || true
         pkill -9 -x bgpd 2>/dev/null || true
         pkill -9 -x zebra 2>/dev/null || true
         pkill -9 -x staticd 2>/dev/null || true
         pkill -9 -x mgmtd 2>/dev/null || true
         rm -f /var/run/frr/*.pid /var/run/frr/*.vty' >/dev/null 2>&1 || true
    docker exec -u root "$container" /usr/lib/frr/frrinit.sh start >/dev/null 2>&1 || return 1
    while [ "$elapsed" -lt 45 ]; do
        if docker exec "$container" vtysh -c "show bgp summary" >/dev/null 2>&1; then
            return 0
        fi
        sleep 2
        elapsed=$((elapsed+2))
    done
    return 1
}

trap restore_test_state EXIT
trap 'exit 130' INT TERM

echo "=============================================================="
echo " 发现链专题判据 —— 15 节点骨干台子（真多跳 / 真分离）"
echo "=============================================================="

for c in $R1 $M1A $M1B $Z1 $R2 $M2A; do dbgon $c; done

INITIAL_GROUP=$(groupid "$Z1")
# Complete wire-format PEER_REQUEST frames from the group representatives.
case "$INITIAL_GROUP" in
    1)
        INITIAL_OBSERVERS=($R1 $M1A $M1B clab-midr-backbone-z2)
        TEST_SOURCE=$R1
        TEST_FRAME='\x03\x01\x00\x00\x0a\x00\x00\x6f\x0a\x63\x00\x6f\x00\x00\xfe\x57\x00\x00\x00\x01'
        ;;
    2)
        INITIAL_OBSERVERS=($R2 $M2A)
        TEST_SOURCE=$R2
        TEST_FRAME='\x03\x01\x00\x00\x0a\x00\x00\x79\x0a\x63\x00\x79\x00\x00\xfe\x61\x00\x00\x00\x02'
        ;;
    *) echo "  z1 is not in group 1 or 2 before the discovery test" >&2; exit 1 ;;
esac
INITIAL_PEERS=()
for c in "${INITIAL_OBSERVERS[@]}"; do
    if v "$c" "show midr neighbors" | grep "$Z1_TRANSPORT" | grep -q Established; then
        INITIAL_PEERS+=("$c")
    fi
done
if [ "${#INITIAL_PEERS[@]}" -eq 0 ]; then
    echo "  no group $INITIAL_GROUP member has an Established session to z1" >&2
    exit 1
fi

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
Z1_BASE=$(loglines $Z1)
if docker exec $Z1 vtysh -c "configure terminal" -c "router bgp 65191" \
    -c "midr shutdown" >/dev/null 2>&1; then
    Z1_SHUTDOWN=1
fi
INJECTED=0
for attempt in 1 2 3; do
    if docker exec "$TEST_SOURCE" bash -c \
        'printf "%b" "$1" > "/dev/udp/$2/5859"' \
        _ "$TEST_FRAME" "$Z1_TRANSPORT"; then
        INJECTED=$((INJECTED+1))
    fi
    sleep 1
done
sleep 45
Z1_LOG=$(logtail $Z1 $Z1_BASE)

if echo "$Z1_LOG" | grep -q "退网：丢弃控制通道 UDP 消息 type=1"; then
    if echo "$Z1_LOG" | grep -q "peering back"; then
        fail "判据9-a：z1 退网后仍回配了（应一律丢弃）"
    else
        pass "判据9-a：z1 守卫丢弃 $(echo "$Z1_LOG" | grep -c '退网：丢弃控制通道 UDP 消息 type=1') 条 PEER_REQUEST，无回配"
    fi
elif [ "$INJECTED" -eq 0 ]; then
    nosam "群 $INITIAL_GROUP 代表未能发送测试 PEER_REQUEST"
else
    fail "判据9-a：已发送 $INJECTED 条 PEER_REQUEST，但 z1 退网守卫没有记录丢弃"
fi
STALE_SESSION=""
STALE_NODE=""
for c in "${INITIAL_OBSERVERS[@]}"; do
    if v "$c" "show midr neighbors" | grep "$Z1_TRANSPORT" | grep -q Established; then
        STALE_SESSION="$c"
    fi
    if v "$c" "show midr nodes" | grep -q "$Z1_RID"; then
        STALE_NODE="$c"
    fi
done
if [ -n "$STALE_SESSION" ]; then
    fail "判据9-b：${STALE_SESSION##*-} 与已退网的 z1 竟建成了 Established 会话"
else
    pass "判据9-b：群 $INITIAL_GROUP 的老成员侧无 Established（会话建不成，符合预期）"
fi
if [ -n "$STALE_NODE" ]; then
    fail "判据9-c：45s 后 ${STALE_NODE##*-} 节点表里仍有 z1 条目（未自愈）"
else
    pass "判据9-c：群 $INITIAL_GROUP 的老成员已随 expire 清除 z1 条目，终态干净"
fi

# 恢复 z1
if docker exec $Z1 vtysh -c "configure terminal" -c "router bgp 65191" \
    -c "no midr shutdown" >/dev/null 2>&1; then
    Z1_SHUTDOWN=0
fi

# ---------------------------------------------------------------------------
# 判据 1 + 2：回配置位 + 老成员起探（本批核心）
#   重启 z1 让它重走一遍 join：z1 侧 connect_group 朝老成员发 PEER_REQUEST，
#   老成员（r1/m1a）走回配路把 z1 纳入本群邻居并起探。
#   认机制：不只看"位上了"，要看回配路的日志（peering back）+ PM I-1 起探。
# ---------------------------------------------------------------------------
echo
echo "[判据 1+2] z1 重新入群：老成员经回配置位 + 起探"
declare -A OBS_BASE
OBSERVERS=($R1 $M1A $M1B $R2 $M2A clab-midr-backbone-z2)
for c in "${OBSERVERS[@]}"; do OBS_BASE[$c]=$(loglines "$c"); done
if ! restart_node "$Z1"; then
    echo "  z1 restart failed" >&2
    exit 1
fi
dbgon $Z1
echo "  （z1 已重启，等待 join 完成）"
T0=$(date +%s); REJOIN_GROUP=0
while [ $(( $(date +%s) - T0 )) -le 240 ]; do
    REJOIN_GROUP=$(groupid "$Z1")
    [ "${REJOIN_GROUP:-0}" != 0 ] && break
    sleep 5
done
if [ "${REJOIN_GROUP:-0}" = 0 ]; then
    echo "  z1 did not join any group within 240s" >&2
    exit 1
fi
case "$REJOIN_GROUP" in
    1) GROUP_OBSERVERS=($R1 $M1A $M1B clab-midr-backbone-z2) ;;
    2) GROUP_OBSERVERS=($R2 $M2A) ;;
    *) echo "  z1 joined unexpected group $REJOIN_GROUP" >&2; exit 1 ;;
esac
echo "  z1 joined group $REJOIN_GROUP in $(( $(date +%s) - T0 ))s"
SESSION_NODE=""; T1=$(date +%s)
while [ $(( $(date +%s) - T1 )) -le 30 ]; do
    for c in "${GROUP_OBSERVERS[@]}"; do
        if v "$c" "show midr neighbors" | grep "$Z1_TRANSPORT" | grep -q "Established.*SAME_GROUP"; then
            SESSION_NODE="$c"
            break 2
        fi
    done
    sleep 2
done
RESPONDER=""; PROBER=""; LEAVE_NODE=""; SENT_TOTAL=0; STORM_NODE=""
for c in "${GROUP_OBSERVERS[@]}"; do
    NODE_LOG=$(logtail "$c" "${OBS_BASE[$c]}")
    if [ -z "$RESPONDER" ] && echo "$NODE_LOG" | grep -q "PEER_REQUEST from rid $Z1_RID.*peering back"; then
        RESPONDER="$c"
    fi
    if [ -z "$PROBER" ] && echo "$NODE_LOG" | grep -q "start probing $Z1_RID"; then
        PROBER="$c"
    fi
    if [ -z "$LEAVE_NODE" ] && echo "$NODE_LOG" | grep -q "LEAVE"; then
        LEAVE_NODE="$c"
    fi
    NODE_SENT=$(echo "$NODE_LOG" | grep -c "sent PEER_REQUEST to $Z1_TRANSPORT")
    SENT_TOTAL=$((SENT_TOTAL+NODE_SENT))
    [ "$NODE_SENT" -gt 5 ] && STORM_NODE="$c"
done

# 1) 回配路径确实跑了（是谁干的）
if [ -n "$RESPONDER" ]; then
    pass "判据1-a：${RESPONDER##*-} 收到 z1 的 PEER_REQUEST 并走回配路（peering back）"
else
    nosam "群 $REJOIN_GROUP 的老成员日志里没有 'peering back' —— 回配路没被触发"
fi

# 2) 结果：z1 在 r1 的台账里是 SAME_GROUP，且会话 Established
if [ -n "$SESSION_NODE" ]; then
    pass "判据1-b：${SESSION_NODE##*-} 台账里 z1 记为 SAME_GROUP 且已建会话"
else
    fail "判据1-b：群 $REJOIN_GROUP 的老成员台账里没有 z1 的 SAME_GROUP 条目"
    v "${GROUP_OBSERVERS[0]}" "show midr neighbors" | sed 's/^/      /'
fi

# 3) 老成员对新成员起探（判据 2 的核心：删探测 A 后唯一来源就是回配）
if [ -n "$PROBER" ]; then
    pass "判据2-a：${PROBER##*-} 对 z1 起探（PM I-1 start probing，回配路触发）"
else
    fail "判据2-a：群 $REJOIN_GROUP 的老成员没有对 z1 起探 —— 回配没接上"
fi

# 4) 不误判 LEAVE（散群风险的直接反证）
if [ -n "$LEAVE_NODE" ]; then
    fail "判据2-b：${LEAVE_NODE##*-} 出现 LEAVE 决策 —— 散群风险成真"
else
    pass "判据2-b：群 $REJOIN_GROUP 的老成员无 LEAVE 决策（群未散）"
fi

# ---------------------------------------------------------------------------
# 判据 3：无 nudge 风暴（刹车生效，回配不再回声）
#   计 z1 重启窗口内 r1 收到/发出的 PEER_REQUEST 条数。回配不发 nudge，
#   故每条边每方向应只有 1 发（+ 未及时收敛时的 3s 重传，按 pending 节奏区分）。
# ---------------------------------------------------------------------------
echo
echo "[判据 3] 无 nudge 风暴"
if [ -n "$STORM_NODE" ]; then
    fail "判据3：${STORM_NODE##*-} 朝 z1 发出超过 5 条 PEER_REQUEST —— 疑似回声风暴"
elif [ "$SENT_TOTAL" -eq 0 ]; then
    pass "判据3：老成员未朝 z1 回发 PEER_REQUEST（回配路刹车生效）"
else
    pass "判据3：老成员共发 $SENT_TOTAL 条 PEER_REQUEST，单节点均未超过重传预算 5"
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
# S2-a below covers the same cleanup path by dropping an established overlay
# session while both transports are blocked, then letting fallback create a
# real half-session whose PEER_REQUEST retries must expire.
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
# S2: block both directions, clear the live overlay, and verify retry cleanup
# followed by fallback recovery after the transport is restored.
# ---------------------------------------------------------------------------
echo
echo "[判据 补2] 死锁自愈（两边对挡 179+5859 → 恢复）"
M1A_BASE=$(loglines $M1A)
M1B_BASE=$(loglines $M1B)
clear_fault_rules
FAULT_RULES=1
for r in "$M1A:10.99.0.113" "$M1B:10.99.0.112"; do
    c=${r%%:*}; s=${r##*:}
    docker exec -u root $c iptables -A INPUT -s $s -p tcp --dport 179 -j DROP 2>/dev/null
    docker exec -u root $c iptables -A INPUT -s $s -p udp --dport 5859 -j DROP 2>/dev/null
done
if docker exec -u root $M1A iptables -C INPUT -s 10.99.0.113 \
    -p udp --dport 5859 -j DROP >/dev/null 2>&1; then
    CLEAR_OK=1
    v "$M1A" "clear bgp 10.99.0.113" >/dev/null 2>&1 || CLEAR_OK=0
    v "$M1B" "clear bgp 10.99.0.112" >/dev/null 2>&1 || CLEAR_OK=0
    CLEAN_NODE=""; DEAD_NODE=""; T0=$(date +%s)
    while [ $(( $(date +%s) - T0 )) -le 75 ]; do
        M1A_LOG=$(logtail $M1A $M1A_BASE)
        M1B_LOG=$(logtail $M1B $M1B_BASE)
        if echo "$M1A_LOG" | grep -q "PEER_REQUEST 尝试 5 次无响应.*目标 10.99.0.113"; then
            DEAD_NODE=$M1A
            if retry_cleanup_complete "$M1A_LOG" 10.99.0.113 10.0.0.113; then
                CLEAN_NODE=$M1A; break
            fi
        fi
        if echo "$M1B_LOG" | grep -q "PEER_REQUEST 尝试 5 次无响应.*目标 10.99.0.112"; then
            DEAD_NODE=$M1B
            if retry_cleanup_complete "$M1B_LOG" 10.99.0.112 10.0.0.112; then
                CLEAN_NODE=$M1B; break
            fi
        fi
        sleep 3
    done
    if [ "$CLEAR_OK" -eq 0 ]; then
        nosam "clear bgp 未能清除既有 overlay 会话"
    elif [ -n "$CLEAN_NODE" ]; then
        pass "判据补2-a：${CLEAN_NODE##*-} 重传死心后拆半边、停探并清除邻接"
    elif [ -n "$DEAD_NODE" ]; then
        fail "判据补2-a：${DEAD_NODE##*-} 已重传死心，但半边会话或邻接清理不完整"
    else
        nosam "75s 内没有观察到 m1a/m1b 的 PEER_REQUEST 重传死心"
    fi
    # 放开网络，看兜底能否自愈
    clear_fault_rules
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
    clear_fault_rules
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
