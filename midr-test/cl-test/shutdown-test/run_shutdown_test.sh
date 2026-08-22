#!/bin/bash
# run_shutdown_test.sh —— 退网（`midr shutdown`）本体判据 1/2/4/5/6/7。
#
# 前置：run_test.sh 已跑完、newnode 已 JOIN 群 1 并完成 ANCHOR（各节点 bgpd 仍在
# 各自 netns 里跑着）。本脚本**不启停 bgpd**，只经 vty 下命令 + 读日志判据。
#
# 判据 3（静态邻居/underlay 无损）在 test_static_intact.sh —— newnode 压根没有
# 静态邻居，判据 3 必须找一台有静态邻居的节点（g2b）来验。
# 判据 8（join 中止后锚点定时器不迟到）在 test_anchor_abort.sh —— 要另起一轮、
# 卡在锚点热身窗口里中止，跟本脚本的"已落定"前提互斥。
#
# 判据全部**旧败新过**地写：旧二进制上跑必须红（4/5/6 三条是核心旧败），新的必须绿。
#
# 用法：sudo ./run_shutdown_test.sh
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CL_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$CL_DIR/../.." && pwd)"
VTYSH="$REPO_ROOT/vtysh/.libs/vtysh"
LOG="$CL_DIR/logs/bgpd-newnode.log"
VTY="/tmp/midr-cl-vty/newnode"

# 对端一：普通群 1 成员，自动建的 SAME_GROUP 边（验判据 1 的"对端即时删我方"）
PEER_AUTO_VTY="/tmp/midr-cl-vty/g1b"
PEER_AUTO_LOG="$CL_DIR/logs/bgpd-g1b.log"
PEER_AUTO_TRANSPORT="10.10.12.2"
# 对端二：群 2 成员，本轮把它做成**双向 MANUAL**（验判据 1 的"对端 MANUAL 半边保留 + α warn"
# 与判据 2 的"本端 MANUAL 也拆"）
PEER_MAN_VTY="/tmp/midr-cl-vty/g2b"
PEER_MAN_LOG="$CL_DIR/logs/bgpd-g2b.log"
PEER_MAN_TRANSPORT="10.10.22.2"
PEER_MAN_ASN="65022"

SELF_TRANSPORT="10.10.99.2"
SELF_RID="10.0.99.1"
SELF_ASN="65099"

PASS=0; FAIL=0
pass() { echo "  ✓ $1"; PASS=$((PASS+1)); }
fail() { echo "  ✗ $1"; FAIL=$((FAIL+1)); }
die()  { echo "[shutdown-test] 前置不满足：$1"; exit 2; }

vty()  { "$VTYSH" --vty_socket "$VTY" "$@" 2>/dev/null; }
vtyp() { "$VTYSH" --vty_socket "$1" "${@:2}" 2>/dev/null; }
conf() { vty -c "configure terminal" -c "router bgp $SELF_ASN" -c "$1"; }

[[ -x "$VTYSH" ]] || die "$VTYSH 不存在（先 make vtysh/vtysh）"
[[ -S "$VTY/bgpd.vty" ]] || die "没有 newnode 的 vty socket —— 先跑 run_test.sh 并保持进程在"
[[ -f "$LOG" ]] || die "$LOG 不存在"
grep -q "MIDR CL: MEMBER_PROBE_DONE → JOIN 群" "$LOG" || die "newnode 还没 JOIN —— 先把 run_test.sh 跑完"

echo "=============================================================="
echo " 退网本体判据（cl-test / newnode）"
echo "=============================================================="

# ---------------------------------------------------------------- Phase A
# 造一条**双向手配**的 MANUAL 边：newnode <-> g2b。
# 两端都敲 `midr session`，台账两侧才都记 MANUAL（MANUAL 粘性会把既有的
# CL_ANCHOR 原因改写成 MANUAL，这正是"运维点名接管一条既有边"的真实场景）。
echo ""
echo "[Phase A] 手配 MANUAL 边 newnode <-> g2b"
conf "midr session $PEER_MAN_TRANSPORT remote-as $PEER_MAN_ASN" >/dev/null
vtyp "$PEER_MAN_VTY" -c "configure terminal" -c "router bgp $PEER_MAN_ASN" \
     -c "midr session $SELF_TRANSPORT remote-as $SELF_ASN" >/dev/null
sleep 5
vty -c "show midr neighbors" | grep -q "$PEER_MAN_TRANSPORT" \
    && echo "  MANUAL 边已就位（newnode 侧可见 $PEER_MAN_TRANSPORT）" \
    || echo "  ⚠ newnode 侧没看到 $PEER_MAN_TRANSPORT，继续（判据里再核）"

# ---------------------------------------------------------------- Phase B 基线
echo ""
echo "[Phase B] 采退网前基线"
BEFORE_NODES=$(vty -c "show midr nodes" | grep -c "^10\." || true)
BEFORE_SELF_GROUP=$(vty -c "show midr self" | grep -iE "group|群" | head -1)
BEFORE_PEERS=$(vty -c "show midr neighbors" | grep -c "Established" || true)
PEER_AUTO_SEES_ME=$(vtyp "$PEER_AUTO_VTY" -c "show midr nodes" | grep -c "$SELF_RID" || true)
echo "  节点表条目=$BEFORE_NODES  Established 的 MIDR 边=$BEFORE_PEERS"
echo "  自身群号行：$BEFORE_SELF_GROUP"
echo "  g1b 看得见我方条目=$PEER_AUTO_SEES_ME"
[[ "$BEFORE_NODES" -gt 0 ]] || die "退网前节点表就是空的，基线不成立"

LINES_SELF=$(wc -l < "$LOG")
LINES_AUTO=$(wc -l < "$PEER_AUTO_LOG")
LINES_MAN=$(wc -l < "$PEER_MAN_LOG")

# ---------------------------------------------------------------- Phase C 退网
echo ""
echo "[Phase C] midr shutdown"
conf "midr shutdown"
sleep 5

new_self() { tail -n +"$LINES_SELF" "$LOG"; }
new_auto() { tail -n +"$LINES_AUTO" "$PEER_AUTO_LOG"; }
new_man()  { tail -n +"$LINES_MAN"  "$PEER_MAN_LOG"; }

# ---------------------------------------------------------------- 判据 2
echo ""
echo "[判据 2] 本端会话拆净（自动 + 锚点 + MANUAL 全拆，2026-08-21 拍板口径）"
LEFT=$(vty -c "show midr neighbors" | grep -c "Established" || true)
[[ "$LEFT" -eq 0 ]] && pass "本端已无 Established 的 MIDR 边（退网前 $BEFORE_PEERS 条）" \
                    || fail "本端仍有 $LEFT 条 Established 的 MIDR 边"
new_self | grep -q "MIDR 退网：拆除运维手配会话 $PEER_MAN_TRANSPORT（台账 MANUAL）" \
    && pass "MANUAL 边随退网拆除且给出重敲提醒（本端）" \
    || fail "没看到本端拆 MANUAL 边的提醒行"

# ---------------------------------------------------------------- 判据 1
echo ""
echo "[判据 1] 对端即时删我方 + 拆其自动半边；对端 MANUAL 半边保留 + α warn"
AFTER_AUTO_SEES=$(vtyp "$PEER_AUTO_VTY" -c "show midr nodes" | grep -c "$SELF_RID" || true)
[[ "$AFTER_AUTO_SEES" -eq 0 ]] && pass "g1b 节点表里我方条目已消失（即时，非等 15s expire）" \
                               || fail "g1b 仍有我方条目（$AFTER_AUTO_SEES 条）"
vtyp "$PEER_AUTO_VTY" -c "show bgp summary" | grep -q "$SELF_TRANSPORT" \
    && fail "g1b 仍挂着到我方 transport 的会话" \
    || pass "g1b 为我方建的自动会话已拆"
new_man | grep -q "跳过自动拆除——要拆请用 no midr session" \
    && pass "g2b 的 MANUAL 半边被 α 守卫豁免（只 warn 不拆）" \
    || fail "g2b 没出现 α 豁免 warn"

# ---------------------------------------------------------------- 判据 6
echo ""
echo "[判据 6] 退网全程对 CL 零噪声"
CL_NOISE=$(new_self | grep -c "MIDR CL: NODE_CHANGE — 分群影响评估（stub）" || true)
[[ "$CL_NOISE" -eq 0 ]] && pass "退网期间 CL 收到 0 条 NODE_CHANGE（旧码此处会喷一串）" \
                        || fail "退网期间 CL 收到 $CL_NOISE 条 NODE_CHANGE"
SUPPRESS=$(new_self | grep -c "MIDR 退网：抑制 I-3 递交" || true)
[[ "$SUPPRESS" -gt 0 ]] && pass "I-3 守卫有正样本：抑制 $SUPPRESS 次递交" \
                        || fail "没抓到 I-3 抑制日志（守卫没生效 or 没触发）"

# ---------------------------------------------------------------- 判据 7
echo ""
echo "[判据 7] 上报线：node LEAVE + link withdraw；记住 version 供重入后比"
new_self | grep -q "MIDR facts: node 撤销上报" \
    && pass "发出了 node 撤销上报（LEAVE）" || fail "没看到 node 撤销上报"
WD_COUNT=$(new_self | grep -c "MIDR facts: link 撤销上报" || true)
V_BEFORE=$(new_self | grep "MIDR facts: link 撤销上报" | grep -oE "version=[0-9]+" \
           | grep -oE "[0-9]+" | sort -n | tail -1)
V_BEFORE=${V_BEFORE:-0}
[[ "$WD_COUNT" -gt 0 ]] && pass "发出了 $WD_COUNT 条 link 撤销上报（最大 version=$V_BEFORE）" \
                        || fail "没看到 link 撤销上报"

# ---------------------------------------------------------------- 判据 4
echo ""
echo "[判据 4] 节点表清空且 15s 后仍空（旧码此处必被对端 keepalive 灌回）"
N0=$(vty -c "show midr nodes" | grep -c "^10\." || true)
[[ "$N0" -eq 0 ]] && pass "退网后节点表立即为空（退网前 $BEFORE_NODES 条）" \
                  || fail "退网后节点表还有 $N0 条"
echo "  等 20s（对端 keepalive 周期 5s、expire 15s）再查一次..."
sleep 20
N1=$(vty -c "show midr nodes" | grep -c "^10\." || true)
[[ "$N1" -eq 0 ]] && pass "20s 后节点表仍为空（收包守卫挡住了回灌）" \
                  || fail "20s 后节点表被灌回 $N1 条（收包守卫失效）"
DROPPED=$(new_self | grep -c "MIDR 退网：丢弃收到的远端 Node NLRI" || true)
[[ "$DROPPED" -gt 0 ]] && pass "收包守卫有正样本：丢弃了 $DROPPED 份远端 NLRI" \
                       || fail "没抓到收包守卫日志（没人给我发 NLRI？守卫没生效？）"

# ---------------------------------------------------------------- 判据 5 上半
echo ""
echo "[判据 5 上半] 群号回落配置值（newnode 配的是 group-id 0）"
SELF_AFTER=$(vty -c "show midr self")
echo "$SELF_AFTER" | sed -n '1,12p' | sed 's/^/    | /'
GID_AFTER=$(echo "$SELF_AFTER" | awk -F: '/^Group-ID/{gsub(/ /,"",$2); print $2}')
SHUT_FLAG=$(echo "$SELF_AFTER" | awk -F: '/^Shutdown/{gsub(/ /,"",$2); print $2}')
[[ "$GID_AFTER" == "0" ]] && pass "群号已回落到配置值 0（JOIN 来的群 1 已清）" \
                          || fail "群号没回落，现在是 $GID_AFTER（旧码保留 JOIN 来的群号）"
[[ "$SHUT_FLAG" == "yes" ]] && pass "show midr self 的 Shutdown 位 = yes" \
                            || fail "Shutdown 位不是 yes（是 $SHUT_FLAG）"

# ---------------------------------------------------------------- 判据 5 下半
echo ""
echo "[判据 5 下半] no midr shutdown → 重走 join"
LINES_SELF2=$(wc -l < "$LOG")
conf "no midr shutdown"
echo "  等待重入落定（REP_LIST_REQ → 60s 热身 ×2 → JOIN），最多 180s..."
ok_join=0
for i in $(seq 1 36); do
    if tail -n +"$LINES_SELF2" "$LOG" | grep -q "MIDR CL: MEMBER_PROBE_DONE → JOIN 群"; then
        ok_join=1; echo "  重入 JOIN 于约 $((i*5))s 落定"; break
    fi
    sleep 5
done
tail -n +"$LINES_SELF2" "$LOG" | grep -q "MIDR JOIN: sent REP_LIST_REQ to bootstrap" \
    && pass "重入按新节点流程发出 REP_LIST_REQ" || fail "重入没发 REP_LIST_REQ"
[[ "$ok_join" -eq 1 ]] && pass "重入后重新 JOIN 落定" || fail "180s 内没重新 JOIN"
N2=$(vty -c "show midr nodes" | grep -c "^10\." || true)
[[ "$N2" -gt 0 ]] && pass "重入后节点表重新长回 $N2 条" || fail "重入后节点表仍空"

# ---------------------------------------------------------------- 判据 7 下半
echo ""
echo "[判据 7 下半] 重入后重报的 version 严格大于退网前（墓碑口径反证）"
LINES_SELF3=$(wc -l < "$LOG")
conf "midr shutdown"
sleep 5
V_AFTER=$(tail -n +"$LINES_SELF3" "$LOG" | grep "MIDR facts: link 撤销上报" \
          | grep -oE "version=[0-9]+" | grep -oE "[0-9]+" | sort -n | tail -1)
V_AFTER=${V_AFTER:-0}
if [[ "$V_AFTER" -gt "$V_BEFORE" ]]; then
    pass "第二次退网的 link version=$V_AFTER > 第一次的 $V_BEFORE（version 就地延续，没从 1 重起）"
else
    fail "version 没有严格增长：第一次 $V_BEFORE，第二次 $V_AFTER"
fi
conf "no midr shutdown" >/dev/null

echo ""
echo "=============================================================="
echo " 小结：通过 $PASS 条，失败 $FAIL 条"
echo "=============================================================="
[[ "$FAIL" -eq 0 ]]
