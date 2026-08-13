#!/bin/bash
# run_test.sh — Isolation / RECONNECT test (validates MIDR_TRIGGER_ISOLATED +
# MIDR_DECISION_RECONNECT in bgp_midr.c / bgp_midr_cl.c).
#
# Reproduces the reported scenario in miniature: a bootstrap node (a) that
# stays up the whole time, two singleton-group reps (d, e) that a joining
# node f ends up depending on for *all* of its connectivity, and a kill of
# both d and e partway through -- leaving f with zero established sessions
# even though a is still perfectly reachable. Before this fix, nothing would
# ever notice; f would sit isolated forever.
#
# f's rep directory (served by a) lists only d and e -- d has lower delay so
# REP_PROBE_DONE ranks it as the primary (JOIN) target, e is the sole
# runner-up (ANCHOR) candidate. After the initial join, f's established
# sessions are exactly {d, e}, nothing else -- so killing both is a clean,
# total isolation, not a partial degradation.
#
# Sequence:
#   t=0      a, d, e start. f starts a few seconds later (bootstrap fires
#            immediately).
#   t≈125    f has JOINed d's group and ANCHOR-connected to e.
#   (kill)   d and e are both killed.
#   +≤150s   MIDR_TRIGGER_ISOLATED fires (2 x 30s debounce + margin) and CL
#            emits MIDR_DECISION_RECONNECT.
#   +≤30s    NDS executes RECONNECT: group cleared, fresh REP_LIST_REQ sent
#            to the bootstrap (a, still alive).
#   +≤150s   Since d/e are both dead, the fresh REP_PROBE_DONE finds no live
#            rep and CL falls back to CREATE -- confirming f is genuinely
#            un-stuck, not just that it tried once and gave up.
#
# Usage: sudo ./run_test.sh [--no-setup] [--join-timeout SECS]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BGPD="$REPO_ROOT/bgpd/.libs/bgpd"
TESTDIR="$SCRIPT_DIR"
JOIN_TIMEOUT=150
ISOLATION_TIMEOUT=150
REJOIN_TIMEOUT=30
FALLBACK_TIMEOUT=150
DO_SETUP=1

cd "$TESTDIR"

for arg in "$@"; do
    case "$arg" in
        --no-setup) DO_SETUP=0 ;;
        --join-timeout) shift; JOIN_TIMEOUT="$1" ;;
    esac
done

trap 'echo "[iso-run_test] Interrupted."; exit 1' INT TERM

echo "[iso-run_test] Checking for stale bgpd instances from a previous run..."
shopt -s nullglob
for pidfile in /tmp/bgpd-iso-*.pid; do
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
    echo "[iso-run_test] Setting up network namespaces..."
    bash "$TESTDIR/setup.sh"
fi

mkdir -p "$TESTDIR/logs"
rm -rf /tmp/midr-iso-vty && mkdir -p /tmp/midr-iso-vty

start_node() {
    local node="$1"
    mkdir -p "/tmp/midr-iso-vty/$node"
    ip netns exec "ns-iso-$node" "$BGPD" \
        -f "$TESTDIR/configs/bgpd-${node}.conf" \
        -Z -S \
        -i "/tmp/bgpd-iso-${node}.pid" \
        --vty_socket "/tmp/midr-iso-vty/$node" \
        --log-level debug &
    echo "  started bgpd for $node (bg pid $!)"
}

# Only matches lines added to f's log *after* $since_line -- several of the
# patterns we wait for (REP_LIST_REQ, CREATE) also legitimately appear once
# during the very first join, so an unscoped grep would false-positive on
# that old line instead of proving a fresh one showed up post-recovery.
wait_for_log() {
    local pattern="$1" timeout="$2" label="$3" since_line="$4"
    local log="$TESTDIR/logs/bgpd-f.log"
    local elapsed=0
    while [[ $elapsed -lt $timeout ]]; do
        if [[ -f "$log" ]] && tail -n "+${since_line}" "$log" | grep -q "$pattern" 2>/dev/null; then
            echo ""
            echo "[iso-run_test] ✓ $label detected at t≈${elapsed}s!"
            return 0
        fi
        printf "\r  elapsed: %3ds / %ds" "$elapsed" "$timeout"
        sleep 5
        elapsed=$((elapsed + 5))
    done
    echo ""
    echo "[iso-run_test] ✗ $label not seen within ${timeout}s."
    return 1
}

log_lines_now() {
    local log="$TESTDIR/logs/bgpd-f.log"
    [[ -f "$log" ]] && wc -l < "$log" || echo 0
}

echo "[iso-run_test] Starting a (bootstrap), d and e (singleton reps)..."
start_node a
start_node d
start_node e
sleep 10

echo "[iso-run_test] Starting f (join flow begins automatically)..."
start_node f
sleep 2  # let the log file get created before we start tailing it from line 1

wait_for_log "MIDR CL: MEMBER_PROBE_DONE → JOIN 群" "$JOIN_TIMEOUT" "JOIN" 1 || true
wait_for_log "MIDR I-7：ANCHOR " 30 "ANCHOR" 1 || true

echo "[iso-run_test] Waiting 5s for sessions to settle before killing d and e..."
sleep 5

echo "[iso-run_test] Killing d and e -- f should now have zero established sessions..."
kill_marker=$(log_lines_now)
for node in d e; do
    pidfile="/tmp/bgpd-iso-${node}.pid"
    pid=$(cat "$pidfile" 2>/dev/null || true)
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
        echo "  killed $node (pid $pid)"
    else
        echo "  ✗ no running pid for $node -- was it already dead?"
    fi
done

wait_for_log "MIDR CL: ISOLATED" "$ISOLATION_TIMEOUT" "ISOLATED trigger / RECONNECT decision" "$kill_marker" || true
wait_for_log "MIDR JOIN: sent REP_LIST_REQ to bootstrap" "$REJOIN_TIMEOUT" "fresh REP_LIST_REQ to bootstrap (rejoin attempt)" "$kill_marker" || true
wait_for_log "CREATE 新群" "$FALLBACK_TIMEOUT" "fallback CREATE (d/e both dead, no live rep to join)" "$kill_marker" || true

chmod -R a+rX "$TESTDIR/logs" 2>/dev/null || true

echo ""
bash "$TESTDIR/check_result.sh"
