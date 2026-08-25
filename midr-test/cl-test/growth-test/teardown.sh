#!/bin/bash
# teardown.sh — Kill bgpd processes and remove growth-test namespaces.
set -e

NODES=(j3 j2 j1 r b)

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

# ⚠ 别写成 `ip netns exec ns-gr-$node pkill -f bgpd`（2026-08-15 在主 cl-test 的
# teardown.sh 上实撞过，本文件是新写的、又踩了一遍）：`ip netns exec` **只切换网络
# 命名空间、不切换 PID 命名空间**，于是 pkill 看到的是**宿主的整张进程表**，会把
# 所有 docker 容器里的 bgpd 一并杀光（我方两套 clab 台子当场全灭，且悄无声息）。
# 正解 = `ip netns pids` 只列该 netns 内的进程，逐个 kill。
for node in "${NODES[@]}"; do
    for pid in $(ip netns pids "ns-gr-$node" 2>/dev/null); do
        kill -9 "$pid" 2>/dev/null || true
    done
done

echo "[growth-teardown] Removing namespaces..."
for node in "${NODES[@]}"; do
    ip netns del "ns-gr-$node" 2>/dev/null || true
done
ip netns del ns-gr-hub 2>/dev/null || true

rm -rf /tmp/midr-gr-vty
rm -rf /tmp/midr-gr-state
echo "[growth-teardown] Done. (logs left in place — remove logs/*.log manually if desired)"
