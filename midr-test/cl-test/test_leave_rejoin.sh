#!/bin/bash
# test_leave_rejoin.sh — Exercise the B1-Q1 steady-state LEAVE + auto-rejoin path.
#
# Prerequisite: run_test.sh has already been run against this same topology and
# newnode has JOINed group 1 (bgpd instances for all nodes are still running in
# their namespaces — this script does not start or stop bgpd).
#
# What it does:
#   1. Degrades newnode's own hub link (tc netem delay on v-newnode-h) enough
#      to push every group's RTT — including group 1's — well past the 20 ms
#      join/stay threshold.
#   2. Waits for PERIODIC_SYNC's steady-state LEAVE judgement
#      (cl_handle_periodic_sync in bgp_midr_cl.c) to fire once the
#      group_settled_at grace window has elapsed and the degraded links have
#      had time to drag the long-term EWMA above threshold.
#   3. Confirms NDS executed LEAVE (group cleared) and automatically restarted
#      the join flow via the bootstrap candidate list.
#
# This does not wait for a second full JOIN/CREATE outcome (the link is still
# degraded, so there's nothing good to join) — it only validates that LEAVE
# fires and the rejoin attempt is actually initiated.
#
# Usage: sudo ./test_leave_rejoin.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOG="$SCRIPT_DIR/logs/bgpd-newnode.log"
VETH="v-newnode-h"          # hub-side veth toward newnode, see setup.sh
DEGRADED_DELAY="200ms"      # one-way; RTT to every group jumps well above 20ms
# Grace period (MIDR_JOIN_PROBE_WAIT_SECS) + up to one PERIODIC_SYNC tick
# (MIDR_PERIODIC_SYNC_INTERVAL) + margin for EWMA to actually cross the
# threshold and for scheduling jitter.
WAIT_TIMEOUT=150

fail() { echo "[test_leave_rejoin] ✗ $1"; exit 1; }
pass() { echo "[test_leave_rejoin] ✓ $1"; }

[[ -f "$LOG" ]] || fail "$LOG not found — run run_test.sh first"
grep -q "MIDR CL: MEMBER_PROBE_DONE → JOIN 群" "$LOG" || \
    fail "newnode hasn't JOINed a group yet — run run_test.sh to completion first"
ip netns exec ns-hub ip link show "$VETH" >/dev/null 2>&1 || \
    fail "$VETH not found in ns-hub — is the topology from setup.sh still up?"

restore_link() {
    ip netns exec ns-hub tc qdisc del dev "$VETH" root 2>/dev/null || true
}
trap restore_link EXIT

echo "[test_leave_rejoin] Degrading $VETH to ${DEGRADED_DELAY} one-way delay..."
ip netns exec ns-hub tc qdisc replace dev "$VETH" root netem delay "$DEGRADED_DELAY"
pass "link degraded — every group's RTT from newnode is now well above the 20ms threshold"

lines_before=$(wc -l < "$LOG")

echo "[test_leave_rejoin] Waiting up to ${WAIT_TIMEOUT}s for the steady-state LEAVE judgement..."
elapsed=0
leave_seen=0
while [[ $elapsed -lt $WAIT_TIMEOUT ]]; do
    if tail -n +"$lines_before" "$LOG" | grep -q "MIDR CL: PERIODIC_SYNC.*LEAVE"; then
        leave_seen=1
        break
    fi
    printf "\r  elapsed: %3ds / %ds" "$elapsed" "$WAIT_TIMEOUT"
    sleep 5
    elapsed=$((elapsed + 5))
done
echo ""

[[ $leave_seen -eq 1 ]] || fail "no 'PERIODIC_SYNC ... LEAVE' decision within ${WAIT_TIMEOUT}s — check logs / thresholds"
pass "CL emitted LEAVE at t≈${elapsed}s after degrading the link"

if tail -n +"$lines_before" "$LOG" | grep -q "MIDR I-7：LEAVE 群 .*，自动重新加入"; then
    pass "NDS executed LEAVE and restarted the join flow via the bootstrap candidate list"
elif tail -n +"$lines_before" "$LOG" | grep -q "MIDR I-7：LEAVE 群 .*，无引导候选可用"; then
    fail "NDS executed LEAVE but had no bootstrap candidate to rejoin through (check 'midr bootstrap' config / §8.32 candidate list)"
else
    fail "no 'MIDR I-7：LEAVE' NDS execution line found after the CL decision — check midr_nds_on_cluster_decision"
fi

if tail -n +"$lines_before" "$LOG" | grep -q "MIDR JOIN: sent REP_LIST_REQ to bootstrap"; then
    pass "rejoin attempt confirmed — a fresh REP_LIST_REQ was sent to the bootstrap"
else
    fail "expected a fresh REP_LIST_REQ after rejoin was triggered, found none"
fi

echo ""
echo "[test_leave_rejoin] All checks passed. Restoring $VETH to its original (no delay) state..."
