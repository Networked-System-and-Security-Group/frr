#!/bin/bash
# teardown.sh — Kill bgpd processes and remove isolation-test namespaces.
set -e

NODES=(a d e f)

echo "[iso-teardown] Killing bgpd instances..."
for node in "${NODES[@]}"; do
    pidfile="/tmp/bgpd-iso-${node}.pid"
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
    ip netns exec "ns-iso-$node" pkill -f bgpd 2>/dev/null || true
done

echo "[iso-teardown] Removing namespaces..."
for node in "${NODES[@]}"; do
    ip netns del "ns-iso-$node" 2>/dev/null || true
done
ip netns del ns-iso-hub 2>/dev/null || true

rm -rf /tmp/midr-iso-vty
echo "[iso-teardown] Done. (logs left in place — remove logs/*.log manually if desired)"
