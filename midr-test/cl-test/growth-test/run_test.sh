#!/bin/bash
# run_test.sh — Organic group growth test (validates the MEMBER_PROBE_DONE
# JOIN-threshold cap fix in bgp_midr_cl.c).
#
# Before the fix, cl_handle_member_probe_done() required an uncapped,
# absolute good_links >= MIDR_CL_MIN_GOOD_LINKS (5) to JOIN. Group 1 here
# starts with only its rep (r) — 1 member — so under the OLD code every
# joiner would see far fewer than 5 known members, fail the bar, and CREATE
# its own singleton group instead of ever joining group 1. This is the exact
# bug shape: a group smaller than 5 can never gain a first member.
#
# With the fix (threshold = min(MIDR_CL_MIN_GOOD_LINKS, total known members),
# plus total_known > 0), joiners started one at a time should JOIN group 1
# even though the group has only 1, then 2, then 3 members at each step —
# demonstrating the threshold adapting as the group actually grows:
#   j1 sees total_known=1 (just r)        -> needs good_links >= 1 -> JOIN
#   j2 sees total_known=2 (r, j1)         -> needs good_links >= 2 -> JOIN
#   j3 sees total_known=3 (r, j1, j2)     -> needs good_links >= 3 -> JOIN
#
# j2/j3 are only started after the previous joiner's JOIN decision has
# actually fired — starting them earlier wouldn't exercise the growing
# total_known count, since r would not yet know about the previous joiner.
#
# Usage: sudo ./run_test.sh [--no-setup] [--timeout SECS]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BGPD="$REPO_ROOT/bgpd/.libs/bgpd"
TESTDIR="$SCRIPT_DIR"
TIMEOUT=150   # seconds to wait for each joiner's JOIN decision
DO_SETUP=1

cd "$TESTDIR"

for arg in "$@"; do
    case "$arg" in
        --no-setup) DO_SETUP=0 ;;
        --timeout)  shift; TIMEOUT="$1" ;;
    esac
done

trap 'echo "[growth-run_test] Interrupted."; exit 1' INT TERM

echo "[growth-run_test] Checking for stale bgpd instances from a previous run..."
shopt -s nullglob
for pidfile in /tmp/bgpd-gr-*.pid; do
    pid=$(cat "$pidfile" 2>/dev/null || true)
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
        echo "  killing stale $(basename "$pidfile") (pid $pid)"
        kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$pidfile"
done
shopt -u nullglob
sleep 1

if [[ "$DO_SETUP" -eq 1 ]]; then
    echo "[growth-run_test] Setting up network namespaces..."
    bash "$TESTDIR/setup.sh"
fi

mkdir -p "$TESTDIR/logs"
rm -rf /tmp/midr-gr-vty && mkdir -p /tmp/midr-gr-vty

start_node() {
    local node="$1"
    mkdir -p "/tmp/midr-gr-vty/$node"
    ip netns exec "ns-gr-$node" "$BGPD" \
        -f "$TESTDIR/configs/bgpd-${node}.conf" \
        -Z -S \
        -i "/tmp/bgpd-gr-${node}.pid" \
        --vty_socket "/tmp/midr-gr-vty/$node" \
        --log-level debug &
    echo "  started bgpd for $node (bg pid $!)"
}

wait_for_join() {
    local node="$1"
    local log="$TESTDIR/logs/bgpd-${node}.log"
    local elapsed=0
    echo "[growth-run_test] Waiting up to ${TIMEOUT}s for $node's JOIN decision..."
    while [[ $elapsed -lt $TIMEOUT ]]; do
        if [[ -f "$log" ]] && grep -q "MIDR CL: MEMBER_PROBE_DONE → JOIN 群" "$log" 2>/dev/null; then
            echo ""
            echo "[growth-run_test] ✓ $node JOIN detected at t=${elapsed}s!"
            return 0
        fi
        printf "\r  elapsed: %3ds / %ds" "$elapsed" "$TIMEOUT"
        sleep 5
        elapsed=$((elapsed + 5))
    done
    echo ""
    echo "[growth-run_test] ✗ $node timed out — no JOIN decision in ${TIMEOUT}s."
    return 1
}

echo "[growth-run_test] Starting r (group-1 rep, 1 member so far)..."
start_node r
sleep 5

echo "[growth-run_test] Starting j1 (group 1 has 1 known member: r)..."
start_node j1
wait_for_join j1 || true

echo "[growth-run_test] Waiting 10s for j1's group change to reach r via BGP-LS..."
sleep 10

echo "[growth-run_test] Starting j2 (group 1 should now have 2 known members: r, j1)..."
start_node j2
wait_for_join j2 || true

echo "[growth-run_test] Waiting 10s for j2's group change to reach r via BGP-LS..."
sleep 10

echo "[growth-run_test] Starting j3 (group 1 should now have 3 known members: r, j1, j2)..."
start_node j3
wait_for_join j3 || true

# Logs were written as root; open them up so a non-root reader can inspect
# results afterward without an extra manual chmod step.
chmod -R a+rX "$TESTDIR/logs" 2>/dev/null || true

echo ""
bash "$TESTDIR/check_result.sh"
