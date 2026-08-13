#!/bin/bash
# test_anchor_established.sh — Validate the PEER_REQUEST cross-group guard fix
# (bgp_midr_ctrl.c: removed the "target_group != mi->local_group_id" filter on
# the receive side).
#
# What was wrong before the fix: newnode's ANCHOR decision looked like it
# connected to all 4 candidates ("尝试建连 4" in check_result.sh), but that
# only means midr_ctrl_connect() was *called* on this side — the receiver in
# each anchor group (g2a/g2b/g3a/g3b) was silently dropping the PEER_REQUEST
# because target_group (newnode's own group, 1) never equals the receiver's
# own group (2 or 3), so every session sat in Active forever. "Attempted
# connect" and "actually Established" are different things, and the old test
# only checked the former.
#
# This script checks the real thing: BGP FSM state, from BOTH ends of each
# of the 4 anchor sessions, via `show midr neighbors` (which reports
# peer->connection->status, not just a log line).
#
# Prerequisite: run_test.sh has already run to completion (ANCHOR decision
# fired) and all bgpd instances are still running in their namespaces.
#
# Usage: sudo ./test_anchor_established.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
VTYSH="$REPO_ROOT/vtysh/vtysh"
LOG="$SCRIPT_DIR/logs/bgpd-newnode.log"
VTYDIR="/tmp/midr-cl-vty"

fail() { echo "[test_anchor_established] ✗ $1"; FAILED=1; }
pass() { echo "[test_anchor_established] ✓ $1"; }
FAILED=0

# A bare `vtysh` resolves to whatever's in PATH — on a machine with a system
# FRR package installed, that binary has no MIDR commands at all and can't
# even find its own config, so it fails before it gets anywhere near
# --vty_socket. Use the repo-built one explicitly.
[[ -x "$VTYSH" ]] || { echo "[test_anchor_established] $VTYSH not found or not executable — build it first (make vtysh/vtysh)"; exit 1; }

[[ -f "$LOG" ]] || { echo "[test_anchor_established] $LOG not found — run run_test.sh first"; exit 1; }
grep -q "MIDR I-7：ANCHOR " "$LOG" || {
    echo "[test_anchor_established] no ANCHOR decision in newnode log yet — run run_test.sh to completion first"
    exit 1
}

vty() { "$VTYSH" --vty_socket "$1" -c "show midr neighbors"; }

# node -> (transport addr, ASN)
declare -A TRANSPORT=( [g2a]=10.10.21.2 [g2b]=10.10.22.2 [g3a]=10.10.31.2 [g3b]=10.10.32.2 )
NEWNODE_TRANSPORT=10.10.99.2

echo "[test_anchor_established] --- Checking from newnode's side (4 anchor sessions) ---"
newnode_out="$(vty "$VTYDIR/newnode")"
for node in g2a g2b g3a g3b; do
    ip="${TRANSPORT[$node]}"
    line=$(echo "$newnode_out" | grep -F "$ip" || true)
    if [[ -z "$line" ]]; then
        fail "newnode: $node ($ip) not present in 'show midr neighbors' at all"
    elif echo "$line" | grep -q "Established"; then
        pass "newnode: $node ($ip) is Established"
    else
        fail "newnode: $node ($ip) present but NOT Established — line: $line"
    fi
done

echo ""
echo "[test_anchor_established] --- Checking from each anchor node's side (reverse direction) ---"
for node in g2a g2b g3a g3b; do
    vtysock="$VTYDIR/$node"
    if [[ ! -S "$vtysock/bgpd.vty" ]]; then
        fail "$node: no vty socket at $vtysock — is it still running?"
        continue
    fi
    line=$(vty "$vtysock" | grep -F "$NEWNODE_TRANSPORT" || true)
    if [[ -z "$line" ]]; then
        fail "$node: newnode ($NEWNODE_TRANSPORT) not present in its 'show midr neighbors'"
    elif echo "$line" | grep -q "Established"; then
        pass "$node: sees newnode ($NEWNODE_TRANSPORT) as Established"
    else
        fail "$node: sees newnode but NOT Established — line: $line"
    fi
done

echo ""
echo "[test_anchor_established] --- Checking for the old regression signature in anchor logs ---"
for node in g2a g2b g3a g3b; do
    nlog="$SCRIPT_DIR/logs/bgpd-${node}.log"
    [[ -f "$nlog" ]] || continue
    if grep -qE "ignoring PEER_REQUEST|target group [0-9]+, ours [0-9]+" "$nlog"; then
        fail "$node log still shows the old cross-group PEER_REQUEST rejection — guard regressed"
    fi
done
pass "no old rejection signature found in any anchor node log"

echo ""
if [[ "$FAILED" -eq 0 ]]; then
    echo "[test_anchor_established] All checks passed — all 4 anchor sessions are genuinely Established on both ends."
    exit 0
else
    echo "[test_anchor_established] One or more checks FAILED — see above."
    exit 1
fi
