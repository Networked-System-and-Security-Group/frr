#!/bin/bash
# run_backbone_shutdown.sh —— 退网（`midr shutdown`）本体判据，跑在 **15 节点骨干台子**上。
#
# 为什么用这个台子而不是 cl-test：骨干台子是**真多跳 + 真分离**（t1–t3 过境节点不跑
# MIDR、静态邻居一律不 activate BGP-LS，LS 只走 overlay 多跳会话），而 cl-test 是 10 个
# netns 星形挂一个 hub、且静态邻居上还 activate 着 LS 的旧形态。本批的判据 3
# （"只退 MIDR 不动 BGP"）在这里才有真被测对象。
#
# 被测对象：
#   z1  —— 无配置群号、靠引导 join 进来的节点。判据 1/2/4/5上/6/7 的主角。
#   r1  —— 群 1 代表（GROUP_REP 位 + 挂靠边）+ 手配群号 1。判据 5 的另一半。
#   ⚠ b1–b5（引导/骨干）**不做被测对象**：引导/骨干执行 shutdown 的语义归群代表专题，
#     是决策档 midr-shutdown-semantics §3 明确划的范围外。
#
# 用法：sudo ./run_backbone_shutdown.sh   （台子须已部署且 z1 已入群）
set -u

Z1=clab-midr-backbone-z1
R1=clab-midr-backbone-r1
M2A=clab-midr-backbone-m2a      # 群 2 成员，用来做双向 MANUAL 边的对端
LOG=/etc/frr/logs/frr.log

Z1_TRANSPORT=10.99.0.191; Z1_ASN=65191; Z1_RID=10.0.0.191
M2A_TRANSPORT=10.99.0.122; M2A_ASN=65122
Z1_STATIC=10.10.31.2            # z1 的静态过境邻居（t3），不载 LS

PASS=0; FAIL=0
pass() { echo "  ✓ $1"; PASS=$((PASS+1)); }
fail() { echo "  ✗ $1"; FAIL=$((FAIL+1)); }
die()  { echo "[backbone-shutdown] 前置不满足：$1"; exit 2; }

v()    { docker exec "$1" vtysh -c "$2" 2>/dev/null; }
# 节点表条目数**不含 self**：`show midr nodes` 一直把本机自己那行也列出来
# （退网只清"学来的"条目、有意留 self，见决策 §3 C-1 第 4 点），拿总行数当判据
# 会永远差 1 条、看着像"没清干净"。按 router-id 排掉自己那行。
nodecount() { v "$1" "show midr nodes" | grep -E "^10\." | grep -vc "^$2"; }
conf() { docker exec "$1" vtysh -c "configure terminal" -c "router bgp $2" -c "$3" 2>/dev/null; }
loglines() { docker exec "$1" sh -c "wc -l < $LOG" 2>/dev/null | tr -d ' '; }
logtail()  { docker exec "$1" sh -c "tail -n +$2 $LOG" 2>/dev/null; }

echo "=============================================================="
echo " 退网本体判据 —— 15 节点骨干台子（真多跳 / 真分离）"
echo "=============================================================="

# ---- 前置：把 debug 频道与文件日志级别打开 ---------------------------------
# 判据要 grep 的几条（CL 的 NODE_CHANGE stub、退网守卫的抑制/丢弃、锚点清理）都是
# debug 级；conf 里没开 debug bgp midr，运行时补上。err/warn/info 无条件全进，不受影响。
for c in $Z1 $R1 $M2A; do
    docker exec "$c" vtysh -c "configure terminal" \
        -c "log file $LOG debugging" -c "debug bgp midr" \
        -c "debug bgp midr discovery" >/dev/null 2>&1
done
echo "  已开 debug bgp midr（三个被测容器）"

# ---- 前置核查 ---------------------------------------------------------------
GID0=$(v $Z1 "show midr self" | awk -F: '/^Group-ID/{gsub(/ /,"",$2);print $2}')
[[ "$GID0" =~ ^[1-9] ]] || die "z1 还没入群（Group-ID=$GID0），等它 join 完再跑"
SESS0=$(v $Z1 "show midr neighbors" | grep -c "Established")
[[ "$SESS0" -gt 0 ]] || die "z1 没有任何 Established 的 MIDR 会话，基线不成立"
echo "  z1 基线：群 $GID0，Established MIDR 边 $SESS0 条"

# ---- Phase A：造一条双向手配 MANUAL 边 z1 <-> m2a ---------------------------
echo ""
echo "[Phase A] 手配 MANUAL 边 z1 <-> m2a（两端都敲 midr session，台账两侧才都记 MANUAL）"
conf $Z1  $Z1_ASN  "midr session $M2A_TRANSPORT remote-as $M2A_ASN" >/dev/null
conf $M2A $M2A_ASN "midr session $Z1_TRANSPORT remote-as $Z1_ASN"  >/dev/null
sleep 8
v $Z1 "show midr neighbors" | grep -q "$M2A_TRANSPORT" \
    && echo "  MANUAL 边已就位" || echo "  ⚠ z1 侧暂未见 $M2A_TRANSPORT，继续"

# ---- Phase B：基线快照 -------------------------------------------------------
echo ""
echo "[Phase B] 退网前基线"
NODES0=$(nodecount $Z1 $Z1_RID)
PEERS0=$(v $Z1 "show midr neighbors" | grep -c "Established")
STATIC0=$(v $Z1 "show bgp summary" | grep "^$Z1_STATIC" | awk '{print $(NF-2)}')
M1A_SEES=$(v clab-midr-backbone-m1a "show midr nodes" | grep -c "$Z1_RID")
echo "  节点表=$NODES0  MIDR 边=$PEERS0  静态邻居 uptime=$STATIC0  m1a 看得见 z1=$M1A_SEES"
docker exec $Z1 ping -c2 -W2 10.99.0.112 >/dev/null 2>&1 \
    && echo "  underlay: z1 -> m1a transport ping 通" \
    || die "退网前 ping m1a 就不通，基线不成立"

L_Z1=$(loglines $Z1); L_M2A=$(loglines $M2A)
L_M1A=$(loglines clab-midr-backbone-m1a)

# ---- Phase C：退网 -----------------------------------------------------------
echo ""
echo "[Phase C] z1: midr shutdown"
conf $Z1 $Z1_ASN "midr shutdown"
sleep 6

# ---- 判据 2 ------------------------------------------------------------------
echo ""
echo "[判据 2] 本端 MIDR 会话拆净（自动 + 挂靠 + 锚点 + MANUAL 全拆）"
LEFT=$(v $Z1 "show midr neighbors" | grep -c "Established")
[[ "$LEFT" -eq 0 ]] && pass "z1 已无 Established 的 MIDR 边（退网前 $PEERS0 条）" \
                    || fail "z1 仍有 $LEFT 条 Established 的 MIDR 边"
logtail $Z1 "$L_Z1" | grep -q "MIDR 退网：拆除运维手配会话 $M2A_TRANSPORT（台账 MANUAL）" \
    && pass "MANUAL 边随退网拆除并给出重敲提醒" \
    || fail "没看到拆 MANUAL 边的提醒行"

# ---- 判据 3（本台子的重头戏：真多跳 underlay 无损）---------------------------
echo ""
echo "[判据 3] 静态过境邻居与 underlay 无损（只退 MIDR 不动 BGP）"
STATIC_LINE=$(v $Z1 "show bgp summary" | grep "^$Z1_STATIC")
STATIC1=$(echo "$STATIC_LINE" | awk '{print $(NF-2)}')
if [[ -z "$STATIC_LINE" ]]; then
    fail "静态邻居 $Z1_STATIC 从 summary 里消失了"
elif echo "$STATIC_LINE" | grep -qE "Idle|Active|Connect"; then
    fail "静态邻居掉线（$STATIC1）"
else
    pass "静态过境邻居仍在（uptime $STATIC0 -> $STATIC1，未清零重连）"
fi
if docker exec $Z1 ping -c3 -W2 10.99.0.112 >/dev/null 2>&1; then
    pass "退网后跨过境 ping m1a transport 仍通（underlay 无损）"
else
    fail "退网后 ping 不通 —— 退网砸了 underlay"
fi
logtail $Z1 "$L_Z1" | grep "MIDR 退网：拆除" | grep -q "$Z1_STATIC" \
    && fail "退网日志里出现了拆静态邻居的记录" \
    || pass "退网没碰静态邻居"

# ---- 判据 1 ------------------------------------------------------------------
echo ""
echo "[判据 1] 对端即时删我方 + 拆其自动半边；对端 MANUAL 半边保留 + α warn"
M1A_SEES1=$(v clab-midr-backbone-m1a "show midr nodes" | grep -c "$Z1_RID")
[[ "$M1A_SEES1" -eq 0 ]] && pass "m1a 节点表里 z1 条目已消失（即时，非等 15s expire）" \
                         || fail "m1a 仍有 z1 条目（$M1A_SEES1 条）"
v clab-midr-backbone-m1a "show bgp summary" | grep -q "$Z1_TRANSPORT" \
    && fail "m1a 仍挂着到 z1 的会话" || pass "m1a 为 z1 建的自动会话已拆"
logtail $M2A "$L_M2A" | grep -q "跳过自动拆除——要拆请用 no midr session" \
    && pass "m2a 的 MANUAL 半边被 α 守卫豁免（只 warn 不拆）" \
    || fail "m2a 没出现 α 豁免 warn"

# ---- 判据 6 ------------------------------------------------------------------
echo ""
echo "[判据 6] 退网全程对 CL 零噪声"
NOISE=$(logtail $Z1 "$L_Z1" | grep -c "MIDR CL: NODE_CHANGE — 分群影响评估（stub）")
[[ "$NOISE" -eq 0 ]] && pass "CL 收到 0 条 NODE_CHANGE" || fail "CL 收到 $NOISE 条 NODE_CHANGE"
# I-3 抑制的正样本要等 periodic_sync 那一拍（30s 周期）才拿得到——退网后 6s 就采样
# 必然是空的（08-21 实撞：抑制发生在退网后 18s，脚本早采样 12s，误报成"守卫没生效"）。
# 挪到判据 4 的 20s 等待之后统一统计，见下方 [判据 6 补采]。
LEAVE=$(logtail $Z1 "$L_Z1" | grep -c "MIDR I-7：LEAVE")
[[ "$LEAVE" -eq 0 ]] && pass "退网期间 CL 没有判过 LEAVE（旧码正是靠 CL LEAVE 顺带清群号的）" \
                     || fail "退网期间 CL 判了 $LEAVE 次 LEAVE"

# ---- 判据 5 上半（认机制，不只认结果）---------------------------------------
echo ""
echo "[判据 5 上半] 群号由**退网本身**当场回落到配置值（z1 没配群号 → 落 0）"
SELF=$(v $Z1 "show midr self")
echo "$SELF" | sed -n '1,8p' | sed 's/^/    | /'
GID1=$(echo "$SELF" | awk -F: '/^Group-ID/{gsub(/ /,"",$2);print $2}')
SHUT=$(echo "$SELF" | awk -F: '/^Shutdown/{gsub(/ /,"",$2);print $2}')
[[ "$GID1" == "0" ]] && pass "群号已回落 0（JOIN 来的群 $GID0 已清）" \
                     || fail "群号没回落，现在是 $GID1"
[[ "$SHUT" == "yes" ]] && pass "Shutdown 位 = yes" || fail "Shutdown 位 = $SHUT"
# ⚠ 关键：旧码上群号也可能变 0，但那是 CL 判 LEAVE 顺带清的（几十秒后、伴随一堆
# NODE_CHANGE）。所以这条判据必须连着"退网自己打的那行收尾日志"一起认。
logtail $Z1 "$L_Z1" | grep -q "MIDR 退网：已撤销自身通告" \
    && pass "有退网收尾日志（群号回落是退网本身干的，不是 CL LEAVE 的副作用）" \
    || fail "没有退网收尾日志 —— 群号若变了也是别人干的"

# ---- 判据 7 上半 -------------------------------------------------------------
echo ""
echo "[判据 7 上半] node LEAVE + link withdraw"
logtail $Z1 "$L_Z1" | grep -q "MIDR facts: node 撤销上报" \
    && pass "发出 node 撤销上报（LEAVE）" || fail "没有 node 撤销上报"
WD=$(logtail $Z1 "$L_Z1" | grep -c "MIDR facts: link 撤销上报")
V1=$(logtail $Z1 "$L_Z1" | grep "MIDR facts: link 撤销上报" | grep -oE "version=[0-9]+" \
     | grep -oE "[0-9]+" | sort -n | tail -1); V1=${V1:-0}
[[ "$WD" -gt 0 ]] && pass "发出 $WD 条 link 撤销上报（最大 version=$V1）" || fail "没有 link 撤销上报"

# ---- 判据 4 ------------------------------------------------------------------
echo ""
echo "[判据 4] 节点表清空且 20s 后仍空（旧码此处被对端 keepalive 灌回）"
N0=$(nodecount $Z1 $Z1_RID)
[[ "$N0" -eq 0 ]] && pass "退网后节点表已清空（除 self；退网前 $NODES0 条学来的条目）" || fail "节点表还有 $N0 条"
echo "  等 20s..."
sleep 20
N1=$(nodecount $Z1 $Z1_RID)
[[ "$N1" -eq 0 ]] && pass "20s 后仍为空（收包守卫挡住回灌，self 除外）" || fail "20s 后被灌回 $N1 条"

# ---- 判据 6 补采（等够一拍 periodic_sync 之后）-------------------------------
echo ""
echo "[判据 6 补采] I-3 抑制的正样本（periodic_sync 每 30s 一拍，退网后要等够一拍）"
for _ in $(seq 1 8); do
    SUP=$(logtail $Z1 "$L_Z1" | grep -c "MIDR 退网：抑制 I-3 递交")
    [[ "$SUP" -gt 0 ]] && break
    sleep 5
done
[[ "$SUP" -gt 0 ]] && pass "I-3 守卫有正样本：抑制 $SUP 次递交（periodic_sync 被拦下）" \
                   || fail "等满 40s 仍没抓到 I-3 抑制日志"

# ---- 判据 4 补采：收包守卫的正样本 -------------------------------------------
# ⚠ 为什么要专门造场景：守卫齐备后，退网节点把 MIDR 会话全拆了，而本台子是**真分离**
# （静态邻居不载 LS），于是退网后根本没有任何 BGP-LS 输入源——收包守卫想触发也没得触发
# （08-21 实撞：v2 轮 0 条，v1 轮 188 条，差别正是 v1 没有 ctrl 守卫、会话被回配重建了）。
# 拿正样本必须造一条**退网拆不掉的载 LS 会话**：手工配一条静态多跳邻居并 activate
# link-state。它不是 `midr session` 建的、不带 OVERLAY 标记，退网不碰它——这恰好就是
# 这道守卫真正要防的场景（运维自己在静态邻居上开了 LS，退网后 NLRI 照样往里灌）。
echo ""
echo "[判据 4 补采] 造一条退网也拆不掉的载 LS 静态会话，看收包守卫是否真的丢弃"
DROP0=$(logtail $Z1 "$L_Z1" | grep -c "MIDR 退网：丢弃收到的远端 Node NLRI")
M1A=clab-midr-backbone-m1a; M1A_TRANSPORT=10.99.0.112; M1A_ASN=65112
for spec in "$Z1|$Z1_ASN|$M1A_TRANSPORT|$M1A_ASN" "$M1A|$M1A_ASN|$Z1_TRANSPORT|$Z1_ASN"; do
    IFS='|' read -r cc aa nn na <<< "$spec"
    docker exec "$cc" vtysh -c "configure terminal" -c "router bgp $aa" \
        -c "neighbor $nn remote-as $na" \
        -c "neighbor $nn ebgp-multihop 5" \
        -c "neighbor $nn update-source lo" \
        -c "address-family link-state link-state" \
        -c "neighbor $nn activate" >/dev/null 2>&1
done
echo "  已配 z1 <-> m1a 静态多跳 LS 会话（非 midr session，退网不该拆它），等 40s 看效果"
sleep 40
STATIC_LS=$(v $Z1 "show bgp summary" | grep "^$M1A_TRANSPORT" | grep -cv "Idle\|Active\|Connect")
DROP1=$(logtail $Z1 "$L_Z1" | grep -c "MIDR 退网：丢弃收到的远端 Node NLRI")
[[ "$STATIC_LS" -gt 0 ]] && pass "静态 LS 会话已建立且退网没拆它（运维会话不受退网影响）" \
                         || fail "静态 LS 会话没建起来，判据 4 的正样本无从取得"
if [[ "$DROP1" -gt "$DROP0" ]]; then
    pass "收包守卫有正样本：新丢弃 $((DROP1-DROP0)) 份远端 NLRI（有输入源时确实拦住了）"
else
    fail "有了 LS 输入源却没抓到收包守卫日志（守卫可能没生效）"
fi
N3=$(nodecount $Z1 $Z1_RID)
[[ "$N3" -eq 0 ]] && pass "灌了 40s NLRI 后节点表仍为空（守卫真的挡住了回灌）" \
                  || fail "节点表被灌回 $N3 条"
# 拆掉这条测试用的静态会话，免得污染后续判据
for spec in "$Z1|$Z1_ASN|$M1A_TRANSPORT" "$M1A|$M1A_ASN|$Z1_TRANSPORT"; do
    IFS='|' read -r cc aa nn <<< "$spec"
    docker exec "$cc" vtysh -c "configure terminal" -c "router bgp $aa" \
        -c "no neighbor $nn" >/dev/null 2>&1
done
sleep 3

# ---- 判据 5 下半 -------------------------------------------------------------
echo ""
echo "[判据 5 下半] no midr shutdown → 按新节点流程重走 join"
L_Z1B=$(loglines $Z1)
conf $Z1 $Z1_ASN "no midr shutdown"
echo "  等待重入落定（REP_LIST_REQ → 两段 60s 热身），最多 240s..."
ok=0
for i in $(seq 1 48); do
    if v $Z1 "show midr self" | awk -F: '/^Group-ID/{gsub(/ /,"",$2);print $2}' | grep -qE "^[1-9]"; then
        ok=1; echo "  约 $((i*5))s 后重新落群"; break
    fi
    sleep 5
done
logtail $Z1 "$L_Z1B" | grep -q "REP_LIST_REQ" \
    && pass "重入发出了 REP_LIST_REQ（走新节点入网流程）" || fail "重入没发 REP_LIST_REQ"
[[ "$ok" -eq 1 ]] && pass "重入后重新落群（群 $(v $Z1 'show midr self' | awk -F: '/^Group-ID/{gsub(/ /,"",$2);print $2}')）" \
                  || fail "240s 内没重新落群"
N2=$(nodecount $Z1 $Z1_RID)
[[ "$N2" -gt 0 ]] && pass "重入后节点表长回 $N2 条" || fail "重入后节点表仍空"

# ---- 判据 7 下半（墓碑口径反证）----------------------------------------------
echo ""
echo "[判据 7 下半] 重入后重报的 version 严格大于退网前"
# ⚠ 必须等链路事实**真的重报过**再退网：重入落群只是建了会话，链路指标还要走 PM 热身
# + I-5 去抖才会 upsert（fl->reported 置真）。落群后立刻 shutdown 的话，撤销路径发现
# reported 还是假、直接跳过，一条 withdraw 都发不出来——看着像"墓碑丢了"，实为采样太早
# （08-21 实撞）。这里等到日志里出现 link 上报，最多 120s。
echo "  等链路事实重新上报（最多 120s）..."
L_WAIT=$(loglines $Z1)
for _ in $(seq 1 24); do
    logtail $Z1 "$L_WAIT" | grep -q "MIDR shim: link_upsert" && break
    sleep 5
done
logtail $Z1 "$L_WAIT" | grep -q "MIDR shim: link_upsert" \
    && echo "  链路事实已重报，可以比 version 了" \
    || echo "  ⚠ 120s 内没见 link_upsert，下面的 version 比较可能取不到样本"
L_Z1C=$(loglines $Z1)
conf $Z1 $Z1_ASN "midr shutdown"
sleep 6
WD2=$(logtail $Z1 "$L_Z1C" | grep -c "MIDR facts: link 撤销上报")
V2=$(logtail $Z1 "$L_Z1C" | grep "MIDR facts: link 撤销上报" | grep -oE "version=[0-9]+" \
     | grep -oE "[0-9]+" | sort -n | tail -1); V2=${V2:-0}
# 两种失败要分清：① 第二次压根没发撤销（旧码不 detach，取不到 version，不是 version 回退）；
# ② 真发了但 version 没涨（墓碑没留住、从 1 重起）。
if [[ "$WD2" -eq 0 ]]; then
    fail "第二次退网没发出任何 link 撤销上报（无从比较 version；旧码正是如此——它不 detach）"
elif [[ "$V2" -gt "$V1" ]]; then
    pass "第二次退网 version=$V2 > 第一次 $V1（就地延续，没从 1 重起）"
else
    fail "version 未严格增长：第一次 $V1，第二次 $V2（墓碑没留住）"
fi
conf $Z1 $Z1_ASN "no midr shutdown" >/dev/null

# ---- 判据 5 另一半：r1（群代表 + 手配群号）-----------------------------------
echo ""
echo "[判据 5 另一半] r1：清 GROUP_REP 位 + 手配群号回落到配置值 1"
CAP0=$(v $R1 "show midr self" | awk -F: '/^Capabilities/{print $2}')
L_R1=$(loglines $R1)
conf $R1 65111 "midr shutdown"
sleep 6
SELF_R1=$(v $R1 "show midr self")
echo "$SELF_R1" | sed -n '1,8p' | sed 's/^/    | /'
CAP1=$(echo "$SELF_R1" | awk -F: '/^Capabilities/{print $2}')
GID_R1=$(echo "$SELF_R1" | awk -F: '/^Group-ID/{gsub(/ /,"",$2);print $2}')
echo "$CAP1" | grep -qi "GroupRep" \
    && fail "GROUP_REP 位还在（$CAP0 -> $CAP1）" \
    || pass "GROUP_REP 位已清（$CAP0 -> $CAP1）"
[[ "$GID_R1" == "1" ]] && pass "手配群号保留=1（回落到配置值而非 0）" \
                       || fail "手配群号没保留，现在是 $GID_R1"
conf $R1 65111 "no midr shutdown" >/dev/null

echo ""
echo "=============================================================="
echo " 小结：通过 $PASS 条，失败 $FAIL 条"
echo "=============================================================="
[[ "$FAIL" -eq 0 ]]
