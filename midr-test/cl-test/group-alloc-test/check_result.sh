#!/bin/bash
# check_result.sh — parse x/y's logs for one scenario ("prevent" or "repair")
# and report PASS/FAIL. Called by run_test.sh after each scenario; also
# runnable by hand afterwards against the same logs/ directory.
#
# Usage: ./check_result.sh prevent|repair

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOGDIR="$SCRIPT_DIR/logs"
SCENARIO="${1:?usage: check_result.sh prevent|repair}"

FAILED=0

create_gid() {
    # 从某节点日志里取它最终落定的 CREATE 群号（可能出现两次：本地估算先
    # 打一行"CREATE 群 N（本地估算）"，落定时再打一行"CREATE 落定群 M"——
    # 后者才是真正生效的号，取最后一次匹配）。
    grep -oP "MIDR I-7：CREATE 落定群 \K[0-9]+" "$LOGDIR/bgpd-$1.log" 2>/dev/null | tail -1
}

echo "=== group-alloc-test: $SCENARIO scenario result ==="
echo ""

if [[ "$SCENARIO" == "prevent" ]]; then
    x_gid=$(create_gid x)
    y_gid=$(create_gid y)
    echo "x settled on group: ${x_gid:-<none found>}"
    echo "y settled on group: ${y_gid:-<none found>}"

    if [[ -z "$x_gid" || -z "$y_gid" ]]; then
        echo "FAIL: one or both nodes never reached a CREATE settle -- check logs/bgpd-{x,y}.log"
        FAILED=1
    elif [[ "$x_gid" == "$y_gid" ]]; then
        echo "FAIL: x and y settled on the SAME group ($x_gid) -- layer 1 (bootstrap-allocated ids) did not prevent the collision"
        FAILED=1
    else
        echo "PASS: x and y settled on different groups ($x_gid vs $y_gid) -- layer 1 worked, no collision"
    fi

    if grep -q "GROUP_ID_COLLISION" "$LOGDIR/bgpd-x.log" "$LOGDIR/bgpd-y.log" 2>/dev/null; then
        echo "FAIL: layer 2 (collision repair) fired even though layer 1 should have prevented any collision -- unexpected"
        FAILED=1
    else
        echo "PASS: layer 2 never fired (as expected -- there was nothing to repair)"
    fi

elif [[ "$SCENARIO" == "repair" ]]; then
    x_first_gid=$(grep -oP "MIDR I-7：CREATE 落定群 \K[0-9]+" "$LOGDIR/bgpd-x.log" 2>/dev/null | head -1)
    y_first_gid=$(grep -oP "MIDR I-7：CREATE 落定群 \K[0-9]+" "$LOGDIR/bgpd-y.log" 2>/dev/null | head -1)
    echo "x's first CREATE settle: ${x_first_gid:-<none found>}"
    echo "y's first CREATE settle: ${y_first_gid:-<none found>}"

    if [[ -z "$x_first_gid" || -z "$y_first_gid" ]]; then
        echo "FAIL: one or both nodes never reached a CREATE settle -- check logs/bgpd-{x,y}.log"
        FAILED=1
    elif [[ "$x_first_gid" != "$y_first_gid" ]]; then
        echo "FAIL: x and y did not collide ($x_first_gid vs $y_first_gid), so the repair path was not exercised"
        FAILED=1
    else
        echo "Collision precondition confirmed: both settled on group $x_first_gid independently."
        echo ""
        echo "Waiting for layer 2 (checked already -- see below)."

        # grep -c already prints 0 on no-match (its exit code is 1, but it
        # still writes output) -- an `|| echo 0` fallback here would just
        # append a spurious second "0" and break the numeric comparisons
        # below.
        y_collision=$(grep -c "GROUP_ID_COLLISION" "$LOGDIR/bgpd-y.log" 2>/dev/null)
        x_collision=$(grep -c "GROUP_ID_COLLISION" "$LOGDIR/bgpd-x.log" 2>/dev/null)
        y_collision=${y_collision:-0}
        x_collision=${x_collision:-0}

        if [[ "$y_collision" -gt 0 && "$x_collision" -eq 0 ]]; then
            echo "PASS: y (larger router-id) detected the collision and yielded; x (smaller router-id) did nothing -- exactly the expected tie-break"
        elif [[ "$y_collision" -eq 0 && "$x_collision" -eq 0 ]]; then
            echo "FAIL: neither node ever logged GROUP_ID_COLLISION -- layer 2 never fired"
            FAILED=1
        else
            echo "FAIL: unexpected collision-detection pattern (x_collision=$x_collision y_collision=$y_collision) -- expected only y"
            FAILED=1
        fi

        y_final_gid=$(grep -oP "MIDR I-7：CREATE 落定群 \K[0-9]+" "$LOGDIR/bgpd-y.log" 2>/dev/null | tail -1)
        y_joined_gid=$(grep -oP "MIDR CL: MEMBER_PROBE_DONE → JOIN 群 \K[0-9]+" "$LOGDIR/bgpd-y.log" 2>/dev/null | tail -1)
        echo ""
        echo "y's final CREATE settle (if any): ${y_final_gid:-<none>}"
        echo "y's final JOIN settle (if any):   ${y_joined_gid:-<none>}"

        if [[ -n "$y_joined_gid" && "$y_joined_gid" == "$x_first_gid" ]]; then
            echo "PASS: y ended up JOINing x's group $x_first_gid directly -- network self-healed into one group"
        elif [[ -n "$y_final_gid" && "$y_final_gid" != "$x_first_gid" ]]; then
            echo "PASS: y re-created a fresh, different group ($y_final_gid, != x's $x_first_gid) after yielding -- no more collision"
        else
            echo "FAIL: y never settled on a resolved state distinct from the collision -- see logs/bgpd-y.log for what actually happened after RECONNECT"
            FAILED=1
        fi
    fi
else
    echo "Unknown scenario '$SCENARIO' (expected prevent|repair)"
    FAILED=1
fi

echo ""
if [[ "$FAILED" -eq 0 ]]; then
    echo "Final verdict ($SCENARIO): PASS"
else
    echo "Final verdict ($SCENARIO): FAIL -- see above"
fi
echo "Logs are in: $LOGDIR/"

exit "$FAILED"
