#!/bin/bash
# run_test.sh — Main CL integration test.
#
# Topology: 8 nodes in separate Linux network namespaces, all connected via an
# ns-hub L3 router.  tc-netem adds one-way delay on hub→node interfaces:
#   Group 1 (g1a-g1e): 3 ms  → PM long-term RTT ≈  3 ms  (< 20 ms threshold)
#   Group 2 (g2a-g2b): 50 ms → PM long-term RTT ≈ 50 ms  (> 20 ms threshold)
#
# Expected result: newnode JOINs group 1 after ≈60 seconds.
# Timing breakdown:
#   t=0    All nodes start; newnode sends REP_LIST_REQ immediately.
#   t≈5    BGP-LS sessions establish; g1a builds full group-1 member list.
#   t≈3    g1a replies to REP_LIST_REQ (or retry at t≈3 if g1a not ready yet).
#   t≈23   REP_PROBE_DONE fires (20 s EWMA warm-up):
#             g1a long-term RTT ≈ 1.9 ms  < g2a ≈ 32 ms → CL RECOMMEND group 1
#   t≈23   NDS sends MEMBER_LIST_REQ; g1a replies with 5 members (g1a-g1e).
#   t≈43   MEMBER_PROBE_DONE fires (20 s EWMA warm-up):
#             5 members × RTT < 20 ms → CL JOIN group 1
#
# Usage: sudo ./run_test.sh [--no-setup] [--timeout SECS]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BGPD="$REPO_ROOT/bgpd/.libs/bgpd"
TESTDIR="$SCRIPT_DIR"
TIMEOUT=90   # seconds to wait for JOIN decision
DO_SETUP=1

# bgpd config files use log paths relative to TESTDIR (e.g. "logs/bgpd-g1a.log"),
# so bgpd must be launched with TESTDIR as its cwd.
cd "$TESTDIR"

for arg in "$@"; do
    case "$arg" in
        --no-setup) DO_SETUP=0 ;;
        --timeout)  shift; TIMEOUT="$1" ;;
    esac
done

trap 'echo "[run_test] Interrupted."; exit 1' INT TERM

# ---- 1. Network setup -------------------------------------------------------
if [[ "$DO_SETUP" -eq 1 ]]; then
    echo "[run_test] Setting up network namespaces..."
    bash "$TESTDIR/setup.sh"
fi

# ---- 2. Create log and VTY dirs ---------------------------------------------
mkdir -p "$TESTDIR/logs"
rm -rf /tmp/midr-cl-vty && mkdir -p /tmp/midr-cl-vty

# ---- 3. Start group nodes first (g1a, g1b, g1c, g1d, g1e, g2a, g2b) --------
NODES=(g1a g1b g1c g1d g1e g2a g2b)
echo "[run_test] Starting group nodes..."
for node in "${NODES[@]}"; do
    mkdir -p "/tmp/midr-cl-vty/$node"
    ip netns exec "ns-$node" "$BGPD" \
        -f "$TESTDIR/configs/bgpd-${node}.conf" \
        -Z -S \
        -i "/tmp/bgpd-cl-${node}.pid" \
        --vty_socket "/tmp/midr-cl-vty/$node" \
        --log-level debug &
    echo "  started bgpd for $node (bg pid $!)"
done

echo "[run_test] Waiting 15 s for group BGP-LS sessions to establish..."
sleep 15

# ---- 4. Start newnode (bootstrap command in config fires immediately) --------
echo "[run_test] Starting newnode (join flow will begin automatically)..."
mkdir -p /tmp/midr-cl-vty/newnode
ip netns exec ns-newnode "$BGPD" \
    -f "$TESTDIR/configs/bgpd-newnode.conf" \
    -Z -S \
    -i /tmp/bgpd-cl-newnode.pid \
    --vty_socket /tmp/midr-cl-vty/newnode \
    --log-level debug &
echo "  started newnode bgpd (bg pid $!)"

# ---- 5. Wait for JOIN decision in newnode's log -----------------------------
LOG="$TESTDIR/logs/bgpd-newnode.log"
echo "[run_test] Waiting up to ${TIMEOUT}s for 'JOIN' decision in newnode log..."
elapsed=0
while [[ $elapsed -lt $TIMEOUT ]]; do
    if [[ -f "$LOG" ]] && grep -q "MIDR CL: MEMBER_PROBE_DONE → JOIN 群" "$LOG" 2>/dev/null; then
        echo ""
        echo "[run_test] ✓ JOIN decision detected at t=${elapsed}s!"
        break
    fi
    printf "\r  elapsed: %3ds / %ds" "$elapsed" "$TIMEOUT"
    sleep 5
    elapsed=$((elapsed + 5))
done
echo ""

if [[ $elapsed -ge $TIMEOUT ]]; then
    echo "[run_test] ✗ Timed out — no JOIN decision in ${TIMEOUT}s."
fi

# ---- 6. Show result summary -------------------------------------------------
echo ""
bash "$TESTDIR/check_result.sh"
