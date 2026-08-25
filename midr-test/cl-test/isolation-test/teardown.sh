#!/bin/bash
# teardown.sh — Kill bgpd processes and remove isolation-test namespaces.
set -e

NODES=(a d e f)

echo "[iso-teardown] Killing bgpd instances..."
for node in f; do
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

for node in a d e; do
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

# ⚠ 别写成 `ip netns exec ns-iso-$node pkill -f bgpd`（2026-08-15 在主 cl-test 的
# teardown.sh 上实撞过）：`ip netns exec` **只切换网络命名空间、不切换 PID 命名
# 空间**，pkill 看到的是**宿主整张进程表**，会把所有 docker 容器里的 bgpd 一并
# 杀光（我方两套 clab 台子当场全灭，且悄无声息）。正解 = `ip netns pids`。
for node in "${NODES[@]}"; do
    for pid in $(ip netns pids "ns-iso-$node" 2>/dev/null); do
        kill -9 "$pid" 2>/dev/null || true
    done
done

echo "[iso-teardown] Removing namespaces..."
for node in "${NODES[@]}"; do
    ip netns del "ns-iso-$node" 2>/dev/null || true
done
ip netns del ns-iso-hub 2>/dev/null || true

rm -rf /tmp/midr-iso-vty
echo "[iso-teardown] Done. (logs left in place — remove logs/*.log manually if desired)"
