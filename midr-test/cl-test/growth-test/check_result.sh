#!/bin/bash
# check_result.sh — Parse growth-test logs and verify each joiner JOINed
# group 1 under an adaptive (not flat-5) threshold.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOGDIR="$SCRIPT_DIR/logs"

echo "=== MIDR Organic Growth Test Result Summary ==="
echo ""

FAILED=0

for node in j1 j2 j3; do
    log="$LOGDIR/bgpd-${node}.log"
    echo "--- $node ---"
    if [[ ! -f "$log" ]]; then
        echo "  ERROR: $log not found."
        FAILED=1
        continue
    fi

    join_line=$(grep "MIDR CL: MEMBER_PROBE_DONE → JOIN 群" "$log" 2>/dev/null | tail -1 || true)
    create_line=$(grep "MIDR CL:.*CREATE 新群" "$log" 2>/dev/null | tail -1 || true)

    if [[ -n "$join_line" ]]; then
        echo "  PASS: $join_line"
    elif [[ -n "$create_line" ]]; then
        # 别直接归咎阈值封顶：拿不到代表目录同样走 CREATE，且更常见。
        # 先看 create_line 是哪一种——"无可用群代表"= 目录空，与阈值无关。
        echo "  FAIL: $node fell back to CREATE instead of JOIN"
        echo "        $create_line"
        if [[ "$create_line" == *"无可用群代表"* ]]; then
            echo "        ↳ 目录空，不是阈值问题：查引导有没有学到代表（show midr reps）"
        else
            echo "        ↳ 探到了代表但没入群：查好链路数与 min(5, 已知成员数) 阈值"
        fi
        FAILED=1
    else
        echo "  FAIL: no JOIN or CREATE decision found — $node never reached MEMBER_PROBE_DONE"
        FAILED=1
    fi
done

echo ""
echo "--- Expected progression (good_links/join_threshold, known members) ---"
echo "  j1: 1/1, 认识 1 个成员  (group had only r)"
echo "  j2: 2/2, 认识 2 个成员  (group had r, j1)"
echo "  j3: 3/3, 认识 3 个成员  (group had r, j1, j2)"
echo "  All three thresholds are below the old flat MIDR_CL_MIN_GOOD_LINKS=5 floor,"
echo "  which would have forced CREATE at every step before the fix."

echo ""
if [[ "$FAILED" -eq 0 ]]; then
    echo "Final verdict: PASS — group 1 grew organically from 1 to 4 members via sequential JOINs."
else
    echo "Final verdict: FAIL — see above."
fi

echo ""
echo "Logs are in: $LOGDIR/"
