#!/bin/bash
# check_result.sh — Parse bgpd logs and print a summary of the CL test outcome.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOGDIR="$SCRIPT_DIR/logs"

echo "=== MIDR CL Test Result Summary ==="
echo ""

newnode_log="$LOGDIR/bgpd-newnode.log"

if [[ ! -f "$newnode_log" ]]; then
    echo "  ERROR: $newnode_log not found."
    exit 1
fi

# Key log lines to extract
echo "--- REP probe phase ---"
grep -E "REP_LIST_REQ|REP_LIST_RESP|I-1.*群代表|REP_PROBE_DONE|RECOMMEND" "$newnode_log" 2>/dev/null | tail -10 || true

echo ""
echo "--- MEMBER probe phase ---"
grep -E "MEMBER_LIST_REQ|MEMBER_LIST_RESP|I-1.*成员|MEMBER_PROBE_DONE" "$newnode_log" 2>/dev/null | tail -10 || true

echo ""
echo "--- CL decisions ---"
grep -E "MIDR CL:|JOIN|CREATE|RECOMMEND" "$newnode_log" 2>/dev/null | tail -15 || true

echo ""
echo "--- Anchor-connection phase (group-3 and group-2 runner-up groups) ---"
grep -E "锚点|ANCHOR" "$newnode_log" 2>/dev/null | tail -20 || true

echo ""
echo "--- PM I-5 metric samples (last 5 per node) ---"
for node in g1a g2a g1b g1c g1d g1e; do
    echo "  Node $node:"
    grep "MIDR PM I-5" "$newnode_log" 2>/dev/null | grep -oP "(10\.10\.[0-9]+\.2)" | sort -u | while read -r ip; do
        grep "MIDR PM I-5" "$newnode_log" | grep "$ip" | tail -3 | sed 's/^/    /'
    done || true
done
grep "MIDR PM I-5" "$newnode_log" 2>/dev/null | tail -10 | sed 's/^/  /' || true

echo ""
echo "--- Final verdict ---"
if grep -qE "MIDR CL: MEMBER_PROBE_DONE → JOIN 群|JOIN group" "$newnode_log" 2>/dev/null; then
    group=$(grep -oP "JOIN 群 \K[0-9]+" "$newnode_log" 2>/dev/null | tail -1 || echo "?")
    echo "  PASS: newnode JOIN group $group  ✓"
elif grep -qE "MIDR CL:.*CREATE 新群" "$newnode_log" 2>/dev/null; then
    group=$(grep -oP "CREATE 新群 \K[0-9]+" "$newnode_log" 2>/dev/null | tail -1 || echo "?")
    echo "  INFO: newnode CREATE new group $group (EWMA not converged yet or < 5 good links)"
else
    echo "  PENDING: no CL decision yet (increase --timeout or check logs)"
fi

anchor_line=$(grep "MIDR I-7：ANCHOR " "$newnode_log" 2>/dev/null | tail -1 || true)
if [[ -n "$anchor_line" ]]; then
    connected=$(echo "$anchor_line" | grep -oP "尝试建连 \K[0-9]+" || echo "?")
    if [[ "$connected" == "4" ]]; then
        echo "  PASS: anchor connections established to all 4 candidates (2 in group 3, 2 in group 2)  ✓"
    else
        echo "  INFO: anchor decision processed but only connected $connected/4 candidates — check logs"
    fi
else
    echo "  PENDING: no ANCHOR decision yet (increase --timeout or check logs)"
fi

echo ""
echo "Logs are in: $LOGDIR/"
