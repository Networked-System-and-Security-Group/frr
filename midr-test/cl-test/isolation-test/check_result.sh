#!/bin/bash
# check_result.sh — Parse f's log and verify the full isolation -> RECONNECT
# narrative: initial JOIN+ANCHOR (exactly two sessions: d, e), then after d
# and e are killed, ISOLATED fires, CL emits RECONNECT, NDS restarts the join
# flow against the still-alive bootstrap (a), and -- since d/e are both dead
# -- eventually falls back to CREATE, proving f is genuinely un-stuck rather
# than having tried once and given up.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOGDIR="$SCRIPT_DIR/logs"
LOG="$LOGDIR/bgpd-f.log"

echo "=== MIDR Isolation / RECONNECT Test Result Summary ==="
echo ""

if [[ ! -f "$LOG" ]]; then
    echo "  ERROR: $LOG not found."
    exit 1
fi

FAILED=0
check() {
    local pattern="$1" desc="$2"
    if grep -q "$pattern" "$LOG" 2>/dev/null; then
        echo "  PASS: $desc"
    else
        echo "  FAIL: $desc"
        FAILED=1
    fi
}

# kill d/e 时 f 日志的行号，由 run_test.sh 落盘。ISOLATED / 重新 join / fallback
# CREATE 这几条在**首次 join** 时也会合法出现，全文件 grep 会拿旧行报 PASS
# （2026-08-25 实测：CREATE 在 kill 前 3 分钟就发生了，Phase 4 照样报 PASS）。
KILL_MARKER=$(cat "$LOGDIR/.kill_marker" 2>/dev/null || echo "")
after_kill() {
    if [[ -n "$KILL_MARKER" ]]; then
        tail -n "+${KILL_MARKER}" "$LOG" 2>/dev/null
    else
        cat "$LOG" 2>/dev/null   # 没有 marker：退回全文件，但下面会提示不可信
    fi
}
check_after() {
    local pattern="$1" desc="$2"
    if after_kill | grep -q "$pattern" 2>/dev/null; then
        echo "  PASS: $desc"
    else
        echo "  FAIL: $desc"
        FAILED=1
    fi
}
if [[ -z "$KILL_MARKER" ]]; then
    echo "  ⚠ 未找到 logs/.kill_marker，Phase 2-4 退回全文件匹配，结果不可信"
    echo ""
fi

echo "--- Phase 1: initial join (f -> d) + anchor (f -> e) ---"
check "MIDR CL: MEMBER_PROBE_DONE → JOIN 群" "f JOINed d's group"
check "MIDR I-7：ANCHOR " "f ANCHOR-connected to e"
grep -E "MIDR CL: REP_PROBE_DONE|MIDR CL: MEMBER_PROBE_DONE|MIDR I-7：ANCHOR " "$LOG" | tail -6 | sed 's/^/    /'

echo ""
echo "--- Phase 2: isolation detection after d and e are killed ---"
check_after "MIDR: 连续.*次探测 0 个已建立会话" "NDS debounce counted sessions down to zero"
check_after "MIDR CL: ISOLATED" "CL received the ISOLATED trigger and emitted RECONNECT"
check_after "MIDR I-7：RECONNECT 群.*因失联触发，自动重新加入" "NDS executed RECONNECT and restarted the join flow"
grep -E "MIDR: .*已建立会话|MIDR CL: ISOLATED|MIDR I-7：RECONNECT" "$LOG" | tail -8 | sed 's/^/    /'

echo ""
echo "--- Phase 3: recovery attempt against the still-alive bootstrap (a) ---"
# 原判据是"全文件计数 >= 2"，但首次 join 死心时本身就会重试出好几条，
# 光靠计数分不出新旧。改成只数 kill 之后的。
after_kill | grep "MIDR JOIN: sent REP_LIST_REQ to bootstrap" | tail -2 | sed 's/^/    /'
req_count=$(after_kill | grep -c "MIDR JOIN: sent REP_LIST_REQ to bootstrap" 2>/dev/null || echo 0)
if [[ "$req_count" -ge 1 ]]; then
    echo "  PASS: bootstrap was re-contacted after isolation ($req_count REP_LIST_REQ post-kill)"
else
    echo "  FAIL: no REP_LIST_REQ seen after the kill -- recovery never restarted the join flow"
    FAILED=1
fi

echo ""
echo "--- Phase 4: fallback outcome (d/e both dead, nothing live to join) ---"
if after_kill | grep -q "CREATE 新群" 2>/dev/null; then
    echo "  PASS: f fell back to CREATE after re-probing found no live rep -- confirms it's genuinely unstuck"
else
    echo "  PENDING/FAIL: no fallback CREATE seen -- either still probing (increase timeout) or recovery didn't restart the flow"
    FAILED=1
fi

echo ""
if [[ "$FAILED" -eq 0 ]]; then
    echo "Final verdict: PASS -- f detected total isolation, reconnected to the bootstrap, and resumed the join flow."
else
    echo "Final verdict: FAIL -- see above."
fi

echo ""
echo "Logs are in: $LOGDIR/"
