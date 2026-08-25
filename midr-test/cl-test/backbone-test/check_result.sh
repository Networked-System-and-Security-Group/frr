#!/bin/bash
# check_result.sh — Parse z1/z2's logs and verify the migrated backbone
# topology produces the expected clustering outcome: both zero-config nodes
# JOIN group 1 (r1's fast access link) and ANCHOR-connect to group 2 (the
# only runner-up -- this topology has no group 3).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
VTYSH="$REPO_ROOT/vtysh/.libs/vtysh"
LOGDIR="$SCRIPT_DIR/logs"

echo "=== MIDR Backbone-Topology-Migration Test Result Summary ==="
echo ""

FAILED=0

check_node() {
    local node="$1"
    local log="$LOGDIR/bgpd-${node}.log"
    echo "--- $node ---"
    if [[ ! -f "$log" ]]; then
        echo "  ERROR: $log not found."
        FAILED=1
        return
    fi

    echo "  REP/MEMBER/ANCHOR decisions:"
    grep -E "MIDR CL: REP_PROBE_DONE|MIDR CL: MEMBER_PROBE_DONE|MIDR I-7：ANCHOR " "$log" 2>/dev/null | tail -6 | sed 's/^/    /'

    join_line=$(grep "MIDR CL: MEMBER_PROBE_DONE → JOIN 群" "$log" 2>/dev/null | tail -1)
    if [[ -n "$join_line" ]]; then
        joined_gid=$(echo "$join_line" | grep -oP "JOIN 群 \K[0-9]+")
        if [[ "$joined_gid" == "1" ]]; then
            echo "  PASS: $node JOINed group 1 (r1)"
        else
            echo "  FAIL: $node JOINed group $joined_gid instead of group 1 -- see the decision line above for which rep it actually recommended"
            FAILED=1
        fi
    elif grep -q "MIDR CL:.*CREATE 新群" "$log" 2>/dev/null; then
        echo "  FAIL: $node fell back to CREATE instead of JOINing group 1"
        FAILED=1
    else
        echo "  FAIL: no JOIN/CREATE decision found for $node"
        FAILED=1
    fi

    if grep -q "MIDR I-7：ANCHOR " "$log" 2>/dev/null; then
        established=$("$VTYSH" --vty_socket "/tmp/midr-bb-vty/$node" \
            -c 'show midr neighbors' 2>/dev/null \
            | awk '/Established/ && /CL_ANCHOR/' | wc -l)
        if [[ "$established" -gt 0 ]]; then
            echo "  PASS: $node has $established Established anchor session(s)"
        else
            echo "  FAIL: $node reached ANCHOR but no anchor session is Established"
            FAILED=1
        fi
    else
        echo "  FAIL: $node has no ANCHOR decision"
        FAILED=1
    fi
    echo ""
}

check_node z1
check_node z2

echo "--- Bootstrap mesh sanity (b1's view) ---"
if [[ -f "$LOGDIR/bgpd-b1.log" ]]; then
    grep -c "Established" "$LOGDIR/bgpd-b1.log" >/dev/null 2>&1 || true
    grep -E "MIDR ctrl: REP_LIST_REQ from|midr_ctrl: REP_LIST_REQ from" "$LOGDIR/bgpd-b1.log" 2>/dev/null | tail -4 | sed 's/^/  /'
fi

echo ""
if [[ "$FAILED" -eq 0 ]]; then
    echo "Final verdict: PASS -- migrated backbone topology reproduces the expected group-1-wins / group-2-anchors outcome."
else
    echo "Final verdict: FAIL -- see above."
fi

echo ""
echo "Logs are in: $LOGDIR/"

exit "$FAILED"
