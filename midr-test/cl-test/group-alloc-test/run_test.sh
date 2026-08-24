#!/bin/bash
# run_test.sh — one-click validation for the group-id-allocation collision
# fix (see CLAUDE.md: "Fixed: two simultaneous joiners could create
# same-numbered, disjoint groups").
#
# Two scenarios, run sequentially by default:
#
#   prevent — b1/b2/b3 all up. x and y (zero-config joiners, no static
#     group-id) are started at the exact same instant into an otherwise
#     empty network, so both independently hit "no rep exists yet" and
#     CREATE a new group. Expected: layer 1 (asking b1, the
#     deterministically-elected allocator, for an authoritative id) hands
#     them different numbers -- no collision, layer 2 never has to act.
#
#   repair — b1 (the elected allocator, smallest router-id) is deliberately
#     NOT started. Every GROUP_ALLOC_REQ x/y send times out and falls back
#     to the local estimate, which is identical for both in this empty
#     network -- a real collision happens (both self-appoint rep of the
#     same group). Expected: once x and y become mutually visible via
#     ordinary NLRI propagation through b2/b3, layer 2 detects it and y
#     (the larger router-id) yields and rejoins; x is untouched.
#
# The repair scenario's full resolution needs two more EWMA warm-up windows
# after the collision (RECONNECT restarts the whole join flow from
# scratch), so budget several minutes for it -- this is inherent to the
# system's timing (MIDR_JOIN_PROBE_WAIT_SECS), not this test being slow for
# no reason.
#
# Usage: sudo ./run_test.sh [--scenario prevent|repair|both]   (default: both)
# Each scenario always tears down and re-sets-up its own fresh namespaces.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BGPD="$REPO_ROOT/bgpd/.libs/bgpd"
TESTDIR="$SCRIPT_DIR"
SCENARIO="both"

cd "$TESTDIR"

for arg in "$@"; do
    case "$arg" in
        --scenario) ;; # next token (prevent|repair|both) matched on its own below
        prevent|repair|both) SCENARIO="$arg" ;;
    esac
done

trap 'echo "[ga-run_test] Interrupted."; exit 1' INT TERM

mkdir -p "$TESTDIR/logs"

start_node() {
    local node="$1"
    mkdir -p "/tmp/midr-ga-vty/$node"
    ip netns exec "ns-ga-$node" "$BGPD" \
        -f "$TESTDIR/configs/bgpd-${node}.conf" \
        -Z -S \
        -i "/tmp/bgpd-ga-${node}.pid" \
        --vty_socket "/tmp/midr-ga-vty/$node" \
        --log-level debug &
    echo "  started bgpd for $node (bg pid $!)"
}

# Same since_line pattern as backbone-test/run_test.sh -- teardown doesn't
# delete logs, and FRR's `log file` appends rather than truncates, so an
# unscoped grep can false-positive on a leftover line from an earlier
# scenario/run.
wait_for_log() {
    local node="$1" pattern="$2" timeout="$3" label="$4" since_line="$5"
    local log="$TESTDIR/logs/bgpd-${node}.log"
    local elapsed=0
    while [[ $elapsed -lt $timeout ]]; do
        if [[ -f "$log" ]] && tail -n "+${since_line}" "$log" | grep -q "$pattern" 2>/dev/null; then
            echo ""
            echo "[ga-run_test] ✓ $node: $label detected at t≈${elapsed}s!"
            return 0
        fi
        printf "\r  elapsed: %3ds / %ds" "$elapsed" "$timeout"
        sleep 5
        elapsed=$((elapsed + 5))
    done
    echo ""
    echo "[ga-run_test] ✗ $node: $label not seen within ${timeout}s."
    return 1
}

log_lines_now() {
    local node="$1" log="$TESTDIR/logs/bgpd-${node}.log"
    [[ -f "$log" ]] && wc -l < "$log" || echo 0
}

kill_stale() {
    shopt -s nullglob
    for pidfile in /tmp/bgpd-ga-*.pid; do
        pid=$(cat "$pidfile" 2>/dev/null || true)
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            echo "  killing stale $(basename "$pidfile") (pid $pid)"
            kill -9 "$pid" 2>/dev/null || true
        fi
        rm -f "$pidfile"
    done
    shopt -u nullglob
}

run_prevent() {
    echo ""
    echo "########################################################"
    echo "# Scenario: prevent (layer 1 -- all 3 bootstraps up)"
    echo "########################################################"

    kill_stale; sleep 1
    bash "$TESTDIR/setup.sh"
    rm -f "$TESTDIR/logs"/*.log
    rm -rf /tmp/midr-ga-vty && mkdir -p /tmp/midr-ga-vty

    echo "[ga-run_test] Starting b1, b2, b3..."
    for b in b1 b2 b3; do start_node "$b"; done
    echo "[ga-run_test] Waiting 15s for the bootstrap mesh to establish..."
    sleep 15

    echo "[ga-run_test] Starting x and y at the same instant..."
    x_marker=$(log_lines_now x); y_marker=$(log_lines_now y)
    start_node x
    start_node y

    wait_for_log x "MIDR I-7：CREATE 落定群" 100 "CREATE settle" "$x_marker" || true
    wait_for_log y "MIDR I-7：CREATE 落定群" 100 "CREATE settle" "$y_marker" || true

    chmod -R a+rX "$TESTDIR/logs" 2>/dev/null || true

    echo ""
    local rc=0
    bash "$TESTDIR/check_result.sh" prevent || rc=$?

    bash "$TESTDIR/teardown.sh"
    return $rc
}

run_repair() {
    echo ""
    echo "########################################################"
    echo "# Scenario: repair (layer 2 -- b1 deliberately NOT started,"
    echo "#           forcing layer 1's fallback path)"
    echo "########################################################"

    kill_stale; sleep 1
    bash "$TESTDIR/setup.sh"
    rm -f "$TESTDIR/logs"/*.log
    rm -rf /tmp/midr-ga-vty && mkdir -p /tmp/midr-ga-vty

    echo "[ga-run_test] Starting b2, b3 only (b1 stays down)..."
    for b in b2 b3; do start_node "$b"; done
    echo "[ga-run_test] Waiting 15s for the (partial) bootstrap mesh to establish..."
    sleep 15

    echo "[ga-run_test] Starting x and y at the same instant..."
    x_marker=$(log_lines_now x); y_marker=$(log_lines_now y)
    start_node x
    start_node y

    echo "[ga-run_test] Waiting for both to hit their first CREATE settle"
    echo "              (~60s EWMA warm-up + up to 15s GROUP_ALLOC retry/fallback)..."
    wait_for_log x "MIDR I-7：CREATE 落定群" 100 "first CREATE settle" "$x_marker" || true
    wait_for_log y "MIDR I-7：CREATE 落定群" 100 "first CREATE settle" "$y_marker" || true

    echo "[ga-run_test] Waiting for layer 2 to detect the collision (y should yield)..."
    wait_for_log y "MIDR CL: GROUP_ID_COLLISION" 90 "collision detected" "$y_marker" || true

    # Marker taken from the GROUP_ID_COLLISION line itself, not a separate
    # log_lines_now (wc -l) call taken right after -- that raced behind the
    # file state wait_for_log's own grep had *just* successfully read from
    # (observed directly: log_lines_now returned a line count smaller than
    # where GROUP_ID_COLLISION actually was, so the next wait's since_line
    # landed *before* it -- back before even the original, colliding CREATE
    # settle line -- and immediately false-matched on that stale line
    # instead of waiting for the real post-RECONNECT one). Deriving the
    # marker from the same successful match sidesteps whatever caused that
    # gap entirely: if grep -n found this line, the file on disk
    # unambiguously contains it.
    y_post_reconnect_marker=$(grep -n "MIDR CL: GROUP_ID_COLLISION" "$TESTDIR/logs/bgpd-y.log" | tail -1 | cut -d: -f1)
    y_post_reconnect_marker=$((y_post_reconnect_marker + 1))
    echo "[ga-run_test] Waiting for y to finish rejoining (RECONNECT restarts the full"
    echo "              join flow -- another full REP_PROBE_DONE + possibly"
    echo "              MEMBER_PROBE_DONE warm-up cycle, budget ~250s)..."
    wait_for_log y "MIDR CL: MEMBER_PROBE_DONE → JOIN 群\|MIDR I-7：CREATE 落定群" 250 "final resolution" "$y_post_reconnect_marker" || true

    chmod -R a+rX "$TESTDIR/logs" 2>/dev/null || true

    echo ""
    local rc=0
    bash "$TESTDIR/check_result.sh" repair || rc=$?

    bash "$TESTDIR/teardown.sh"
    return $rc
}

overall_rc=0

if [[ "$SCENARIO" == "prevent" || "$SCENARIO" == "both" ]]; then
    run_prevent || overall_rc=1
fi

if [[ "$SCENARIO" == "repair" || "$SCENARIO" == "both" ]]; then
    run_repair || overall_rc=1
fi

echo ""
echo "########################################################"
if [[ "$overall_rc" -eq 0 ]]; then
    echo "# Overall: PASS"
else
    echo "# Overall: FAIL -- see per-scenario verdicts above"
fi
echo "########################################################"

exit "$overall_rc"
