#!/bin/bash
# teardown.sh — Kill bgpd processes and remove growth-test namespaces.
set -e

NODES=(r j1 j2 j3)

echo "[growth-teardown] Killing bgpd instances..."
for node in "${NODES[@]}"; do
    pidfile="/tmp/bgpd-gr-${node}.pid"
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

for node in "${NODES[@]}"; do
    ip netns exec "ns-gr-$node" pkill -f bgpd 2>/dev/null || true
done

echo "[growth-teardown] Removing namespaces..."
for node in "${NODES[@]}"; do
    ip netns del "ns-gr-$node" 2>/dev/null || true
done
ip netns del ns-gr-hub 2>/dev/null || true

rm -rf /tmp/midr-gr-vty
echo "[growth-teardown] Done. (logs left in place — remove logs/*.log manually if desired)"
