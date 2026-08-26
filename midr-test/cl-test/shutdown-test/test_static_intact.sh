#!/bin/bash
# test_static_intact.sh —— 判据 3（静态邻居与 underlay 无损）+ 判据 5 的"手配群号"分支。
#
# 为什么不在 newnode 上验：newnode 的 conf 里压根没有静态 BGP 邻居（它整个身家都是
# join 来的），"只退 MIDR 不动 BGP"这条判据在它身上没有被测对象。g2b 有：
#   neighbor 10.10.21.2 remote-as 65021 + address-family link-state 里 activate
# 而且它 `midr group-id 2` 是手配群号，正好一并验判据 5 的"手配群号者以配置群号落定"。
#
# 用法：sudo ./test_static_intact.sh
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CL_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$CL_DIR/../.." && pwd)"
VTYSH="$REPO_ROOT/vtysh/.libs/vtysh"
VTY="/tmp/midr-cl-vty/g2b"
LOG="$CL_DIR/logs/bgpd-g2b.log"
ASN="65022"
STATIC_PEER="10.10.21.2"   # g2a，frr.conf 里手写的静态邻居（载 BGP-LS）

PASS=0; FAIL=0
pass() { echo "  ✓ $1"; PASS=$((PASS+1)); }
fail() { echo "  ✗ $1"; FAIL=$((FAIL+1)); }
die()  { echo "[static-intact] 前置不满足：$1"; exit 2; }

vty()  { "$VTYSH" --vty_socket "$VTY" "$@" 2>/dev/null; }
conf() { vty -c "configure terminal" -c "router bgp $ASN" -c "$1"; }

[[ -S "$VTY/bgpd.vty" ]] || die "没有 g2b 的 vty socket"

echo "=============================================================="
echo " 判据 3 / 判据 5（手配群号分支）—— 在 g2b 上验"
echo "=============================================================="

# 静态会话的 Up/Down 列（uptime）——退网不该让它清零重连
before_line=$(vty -c "show bgp summary" | grep "^$STATIC_PEER" || true)
before_up=$(echo "$before_line" | awk '{print $(NF-2)}')
echo "  退网前静态会话行：$before_line"
[[ -n "$before_line" ]] || die "g2b 的静态邻居 $STATIC_PEER 现在就不在 summary 里"
echo "$before_line" | grep -qE "Idle|Active|Connect" && die "静态会话现在就没建起来，基线不成立"

# underlay 连通性基线
ip netns exec ns-g2b ping -c 2 -W 2 "$STATIC_PEER" >/dev/null 2>&1 \
    && echo "  退网前 ping $STATIC_PEER 通" || die "退网前 ping 就不通"

LINES=$(wc -l < "$LOG")
echo ""
echo "[执行] g2b: midr shutdown"
conf "midr shutdown"
sleep 8

after_line=$(vty -c "show bgp summary" | grep "^$STATIC_PEER" || true)
after_up=$(echo "$after_line" | awk '{print $(NF-2)}')
echo "  退网后静态会话行：$after_line"

# 判据 3-①：静态会话还在、且没有被拆掉重连（uptime 不回退到几秒）
if [[ -z "$after_line" ]]; then
    fail "静态邻居 $STATIC_PEER 从 summary 里消失了——退网动了 underlay"
elif echo "$after_line" | grep -qE "Idle|Active|Connect"; then
    fail "静态会话掉线了（$after_up）——退网动了 underlay"
else
    pass "静态会话仍在且未掉线（退网前 uptime=$before_up，退网后=$after_up）"
fi

# 判据 3-②：underlay 转发面无损
if ip netns exec ns-g2b ping -c 3 -W 2 "$STATIC_PEER" >/dev/null 2>&1; then
    pass "退网后 ping $STATIC_PEER 仍通（underlay 无损）"
else
    fail "退网后 ping 不通——退网砸了 underlay"
fi

# 判据 3-③：拆的只该是 MIDR 自建的边，静态邻居不该出现在拆除日志里
if tail -n +"$LINES" "$LOG" | grep "MIDR 退网：拆除" | grep -q "$STATIC_PEER"; then
    fail "退网日志里出现了拆静态邻居 $STATIC_PEER 的记录"
else
    pass "退网没碰静态邻居（拆除记录里没有 $STATIC_PEER）"
fi

# 判据 5（手配群号分支）：g2b 配的是 group-id 2，退网后应回落到 2 而不是 0
echo ""
echo "[判据 5 手配群号分支] g2b 配了 midr group-id 2"
self_out=$(vty -c "show midr self")
echo "$self_out" | sed -n '1,12p' | sed 's/^/    | /'
gid=$(echo "$self_out" | awk -F: '/^Group-ID/{gsub(/ /,"",$2); print $2}')
[[ "$gid" == "2" ]] && pass "群号落在配置值 2（手配的群号退网后保留）" \
                    || fail "群号不是配置值 2，而是 $gid"

echo ""
echo "[收尾] g2b: no midr shutdown"
conf "no midr shutdown" >/dev/null

echo ""
echo " 小结：通过 $PASS 条，失败 $FAIL 条"
[[ "$FAIL" -eq 0 ]]
