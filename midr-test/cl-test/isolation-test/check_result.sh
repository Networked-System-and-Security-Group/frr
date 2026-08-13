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

echo "--- Phase 1: initial join (f -> d) + anchor (f -> e) ---"
check "MIDR CL: MEMBER_PROBE_DONE → JOIN 群" "f JOINed d's group"
check "MIDR I-7：ANCHOR " "f ANCHOR-connected to e"
grep -E "MIDR CL: REP_PROBE_DONE|MIDR CL: MEMBER_PROBE_DONE|MIDR I-7：ANCHOR " "$LOG" | tail -6 | sed 's/^/    /'

echo ""
echo "--- Phase 2: isolation detection after d and e are killed ---"
check "MIDR: 连续.*次探测 0 个已建立会话" "NDS debounce counted sessions down to zero"
check "MIDR CL: ISOLATED" "CL received the ISOLATED trigger and emitted RECONNECT"
check "MIDR I-7：RECONNECT 群.*因失联触发，自动重新加入" "NDS executed RECONNECT and restarted the join flow"
grep -E "MIDR: .*已建立会话|MIDR CL: ISOLATED|MIDR I-7：RECONNECT" "$LOG" | tail -8 | sed 's/^/    /'

echo ""
echo "--- Phase 3: recovery attempt against the still-alive bootstrap (a) ---"
# Two REP_LIST_REQ lines are expected in the full log: one at t=0, one after
# recovery. This just shows the last one (should be the post-recovery one).
grep "MIDR JOIN: sent REP_LIST_REQ to bootstrap" "$LOG" | tail -2 | sed 's/^/    /'
req_count=$(grep -c "MIDR JOIN: sent REP_LIST_REQ to bootstrap" "$LOG" 2>/dev/null || echo 0)
if [[ "$req_count" -ge 2 ]]; then
    echo "  PASS: bootstrap was re-contacted after isolation ($req_count total REP_LIST_REQ attempts, expected >= 2)"
else
    echo "  FAIL: only $req_count REP_LIST_REQ attempt(s) seen -- expected at least 2 (initial + post-recovery)"
    FAILED=1
fi

echo ""
echo "--- Phase 4: fallback outcome (d/e both dead, nothing live to join) ---"
if grep -q "CREATE 新群" "$LOG" 2>/dev/null; then
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
