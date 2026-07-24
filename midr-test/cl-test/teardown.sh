#!/bin/bash
# teardown.sh — Kill bgpd processes and remove all test namespaces.
set -e

NODES=(g1a g1b g1c g1d g1e g2a g2b g3a g3b newnode)

echo "[teardown] Killing bgpd instances..."
for node in "${NODES[@]}"; do
    pidfile="/tmp/bgpd-cl-${node}.pid"
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

# Force-kill anything still lingering in node namespaces
for node in "${NODES[@]}"; do
    ip netns exec "ns-$node" pkill -f bgpd 2>/dev/null || true
done

echo "[teardown] Removing namespaces..."
for node in "${NODES[@]}"; do
    ip netns del "ns-$node" 2>/dev/null || true
done
ip netns del ns-hub 2>/dev/null || true

rm -rf /tmp/midr-cl-vty
echo "[teardown] Done."

rm logs/*.log