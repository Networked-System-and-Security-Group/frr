#!/bin/bash
# run_test.sh — Main CL integration test.
#
# Topology: 10 nodes in separate Linux network namespaces, all connected via
# an ns-hub L3 router.  tc-netem adds one-way delay on hub→node interfaces:
#   Group 1 (g1a-g1e): 3 ms  → PM long-term RTT ≈  3 ms  (< 20 ms threshold) — best, chosen
#   Group 3 (g3a-g3b): 10 ms → PM long-term RTT ≈ 10 ms  (< 20 ms threshold) — 2nd, 1st anchor group
#   Group 2 (g2a-g2b): 50 ms → PM long-term RTT ≈ 50 ms  (> 20 ms threshold) — 3rd, 2nd anchor group
#
# Expected result: newnode JOINs group 1 after ≈125 seconds, and — as a side
# effect of the same RECOMMEND event (doc/change-reply.md B2/疑2) — anchor-
# connects to the best 2 nodes in each of the two runner-up groups (3 and 2),
# completing a few seconds after JOIN.
# Timing breakdown (MIDR_JOIN_PROBE_WAIT_SECS = 60):
#   t=0    All nodes start; newnode sends REP_LIST_REQ immediately.
#   t≈5    BGP-LS sessions establish; g1a builds full group-1 member list and
#          learns g2a/g3a (cross-group BGP-LS neighbors) for its rep directory.
#   t≈3    g1a replies to REP_LIST_REQ (or retry at t≈3 if g1a not ready yet).
#   t≈63   REP_PROBE_DONE fires (60 s EWMA warm-up): ranks g1a(≈2.9ms) <
#          g3a(≈9.7ms) < g2a(≈48ms) → CL RECOMMEND group 1, anchor_reps=[g3a,g2a].
#   t≈63   NDS sends MEMBER_LIST_REQ to g1a (main) and to g3a/g2a (anchors).
#   t≈123  MEMBER_PROBE_DONE fires: 5 group-1 members × RTT < 20 ms → CL JOIN group 1.
#   t≈123  ANCHOR_PROBE_DONE fires (~same time, own independent timer): CL
#          picks the best 2 nodes in group 3 and the best 2 in group 2 → I-7
#          ANCHOR → NDS connects to all 4.
#
# Usage: sudo ./run_test.sh [--no-setup] [--timeout SECS]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BGPD="$REPO_ROOT/bgpd/.libs/bgpd"
TESTDIR="$SCRIPT_DIR"
TIMEOUT=150   # seconds to wait for JOIN decision
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

# ---- 3. Start group nodes first (groups 1, 2, 3) ----------------------------
NODES=(g1a g1b g1c g1d g1e g2a g2b g3a g3b)
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

# ---- 5b. Wait a bit more for the anchor-connection side effect --------------
# ANCHOR_PROBE_DONE runs on its own independent timer (restarted whenever
# either runner-up group's member list arrives), so it can land a few seconds
# after JOIN. Give it up to 30 s before declaring it missing.
if [[ $elapsed -lt $TIMEOUT ]]; then
    echo "[run_test] Waiting up to 30s for the ANCHOR decision (group-3/group-2 anchor connections)..."
    anchor_elapsed=0
    while [[ $anchor_elapsed -lt 30 ]]; do
        if grep -q "MIDR I-7：ANCHOR " "$LOG" 2>/dev/null; then
            echo "[run_test] ✓ ANCHOR decision detected at t≈$((elapsed + anchor_elapsed))s!"
            break
        fi
        sleep 3
        anchor_elapsed=$((anchor_elapsed + 3))
    done
    if [[ $anchor_elapsed -ge 30 ]]; then
        echo "[run_test] ✗ No ANCHOR decision seen within 30s of JOIN — check logs."
    fi
fi

# ---- 6. Show result summary -------------------------------------------------
echo ""
bash "$TESTDIR/check_result.sh"
