#!/bin/bash
# test_anchor_abort.sh —— 判据 8：join 中止后，锚点热身定时器不再迟到触发。
#
# 这是配套一（anchor 残留清理 helper）的反证实验，要卡在一个很窄的窗口里：
#   t≈63s  RECOMMEND 之后，次优群成员表到达 → 武装 t_anchor_probe_done（60s 热身）
#          日志："MIDR 锚点：收到次优群 N 成员列表，ANCHOR_PROBE_DONE 将在 60 秒后触发"
#   t≈123s 定时器到点 → 日志："MIDR 锚点：ANCHOR_PROBE_DONE 定时器触发，通知 CL"
# 在这两点之间中止 join，旧码不 cancel 定时器 → 到点照样拿**过期的备选群**触发
# ANCHOR_PROBE_DONE；新码在中止路径上清掉上下文 + cancel，定时器不该再响。
#
# 中止手段特意选 `no midr bootstrap`（清空候选）而不是 `midr shutdown`：
# shutdown 另有 notify_cl 静默守卫，会把"定时器响了但 CL 没收到"也算成通过，
# 测不出锚点清理本身。清空候选这条路上没有那道守卫，判据只落在配套一身上。
#
# 本脚本会**重启 newnode 的 bgpd**（需要一轮全新的 join 才有热身窗口），其余节点不动。
#
# 用法：sudo ./test_anchor_abort.sh
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CL_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$CL_DIR/../.." && pwd)"
BGPD="$REPO_ROOT/bgpd/.libs/bgpd"
VTYSH="$REPO_ROOT/vtysh/.libs/vtysh"
VTY="/tmp/midr-cl-vty/newnode"
LOG="$CL_DIR/logs/bgpd-newnode.log"
ASN="65099"

PASS=0; FAIL=0
pass() { echo "  ✓ $1"; PASS=$((PASS+1)); }
fail() { echo "  ✗ $1"; FAIL=$((FAIL+1)); }

cd "$CL_DIR"   # bgpd 的 conf 里日志路径是相对 cl-test 目录写的

echo "=============================================================="
echo " 判据 8：join 中止后锚点定时器不迟到（配套一反证）"
echo "=============================================================="

# ---- 重启 newnode，拿一轮全新的 join --------------------------------------
if [[ -f /tmp/bgpd-cl-newnode.pid ]]; then
    kill -9 "$(cat /tmp/bgpd-cl-newnode.pid)" 2>/dev/null || true
    rm -f /tmp/bgpd-cl-newnode.pid
fi
sleep 1
# 日志截断：cl-test 的 conf 是 append 模式，不截断会 grep 到上一轮的行（脚手架坑之二）
: > "$LOG"
mkdir -p /tmp/midr-cl-vty/newnode
ip netns exec ns-newnode "$BGPD" -f "$CL_DIR/configs/bgpd-newnode.conf" -Z -S \
    -i /tmp/bgpd-cl-newnode.pid --vty_socket /tmp/midr-cl-vty/newnode \
    --log-level debug &
echo "  newnode 已重启（pid $!），等待锚点热身定时器武装..."

# ---- 等"定时器武装"那一行 ---------------------------------------------------
armed=0
for i in $(seq 1 40); do   # 最多 200s
    if grep -q "ANCHOR_PROBE_DONE 将在" "$LOG" 2>/dev/null; then
        armed=1
        echo "  t≈$((i*5))s：锚点定时器已武装 —— $(grep -m1 "ANCHOR_PROBE_DONE 将在" "$LOG" | sed 's/.*MIDR/MIDR/')"
        break
    fi
    sleep 5
done

if [[ "$armed" -eq 0 ]]; then
    echo "  ✗ 200s 内没等到锚点定时器武装 —— 判据 8 拿不到正样本，本条不算通过"
    echo "    （沉默 ≠ 通过：没有武装过的定时器，'没触发'证明不了任何事）"
    exit 3
fi

# ---- 中止 join --------------------------------------------------------------
MARK=$(wc -l < "$LOG")
echo ""
echo "[中止] no midr bootstrap（清空候选 = 中止加入）"
"$VTYSH" --vty_socket "$VTY" -c "configure terminal" -c "router bgp $ASN" \
         -c "no midr bootstrap" >/dev/null 2>&1

tail -n +"$MARK" "$LOG" | grep -q "MIDR 锚点：清理评估上下文" \
    && pass "中止时清了锚点评估上下文（helper 有正样本）" \
    || fail "没看到锚点清理日志（配套一没挂上，或没有残留可清）"

# ---- 等过定时器本该到点的时刻 ------------------------------------------------
echo "  等 90s（覆盖 60s 热身窗）看定时器还响不响..."
sleep 90

if tail -n +"$MARK" "$LOG" | grep -q "ANCHOR_PROBE_DONE 定时器触发"; then
    fail "定时器仍然到点触发了 —— 中止后拿过期备选群通知了 CL（旧码行为）"
else
    pass "定时器没有再触发（迟到的 ANCHOR_PROBE_DONE 被掐掉）"
fi

if tail -n +"$MARK" "$LOG" | grep -q "MIDR CL: ANCHOR_PROBE_DONE"; then
    fail "CL 仍收到了 ANCHOR_PROBE_DONE"
else
    pass "CL 全程没收到 ANCHOR_PROBE_DONE"
fi

echo ""
echo " 小结：通过 $PASS 条，失败 $FAIL 条"
[[ "$FAIL" -eq 0 ]]
