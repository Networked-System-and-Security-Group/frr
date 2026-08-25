#!/bin/bash
# teardown.sh — kill bgpd processes and remove group-alloc-test namespaces.
set -e

NODES=(x y b3 b2 b1)

echo "[ga-teardown] Killing bgpd instances..."
for node in "${NODES[@]}"; do
    pidfile="/tmp/bgpd-ga-${node}.pid"
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
# only switches the network namespace, not the PID namespace, so it can see
# and kill the host's entire process table.
for node in "${NODES[@]}"; do
    for pid in $(ip netns pids "ns-ga-$node" 2>/dev/null); do
        kill -9 "$pid" 2>/dev/null || true
    done
done

echo "[ga-teardown] Removing namespaces..."
for node in "${NODES[@]}"; do
    ip netns del "ns-ga-$node" 2>/dev/null || true
done

rm -rf /tmp/midr-ga-vty
rm -rf /tmp/midr-ga-state
echo "[ga-teardown] Done. (logs left in place)"
