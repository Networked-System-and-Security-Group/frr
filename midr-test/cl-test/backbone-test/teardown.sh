#!/bin/bash
# teardown.sh — Kill bgpd processes and remove backbone-test namespaces.
set -e

REAL_NODES=(b1 b2 b3 b4 b5 r1 m1a m1b r2 m2a z1 z2)
TRANSIT_NODES=(t1 t2 t3)
ALL_NODES=("${REAL_NODES[@]}" "${TRANSIT_NODES[@]}")

echo "[bb-teardown] Killing bgpd instances..."
for node in z1 z2; do
    pidfile="/tmp/bgpd-bb-${node}.pid"
    if [[ -f "$pidfile" ]]; then
        pid=$(cat "$pidfile" 2>/dev/null || true)
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
            echo "  Killed bgpd for $node (pid $pid)"
        fi
        rm -f "$pidfile"
    fi
done

sleep 1

for node in b1 b2 b3 b4 b5 r1 m1a m1b r2 m2a t1 t2 t3; do
    pidfile="/tmp/bgpd-bb-${node}.pid"
    if [[ -f "$pidfile" ]]; then
        pid=$(cat "$pidfile" 2>/dev/null || true)
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
            echo "  Killed bgpd for $node (pid $pid)"
        fi
        rm -f "$pidfile"
    fi
done

sleep 1

# `ip netns pids`, not `pkill -f bgpd` inside `ip netns exec` -- the latter
# only switches the network namespace, not the PID namespace, so it sees
# and can kill the host's *entire* process table (a real incident on the
# teammate's own containerlab rigs, per their teardown.sh fix elsewhere in
# this repo). Transit nodes never ran bgpd, but harmless to include.
for node in "${ALL_NODES[@]}"; do
    for pid in $(ip netns pids "ns-bb-$node" 2>/dev/null); do
        kill -9 "$pid" 2>/dev/null || true
    done
done

echo "[bb-teardown] Removing namespaces..."
for node in "${ALL_NODES[@]}"; do
    ip netns del "ns-bb-$node" 2>/dev/null || true
done

rm -rf /tmp/midr-bb-vty
echo "[bb-teardown] Done. (logs left in place — remove logs/*.log manually if desired)"
