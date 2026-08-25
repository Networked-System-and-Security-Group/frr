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

# Force-kill anything still lingering in node namespaces.
#
# ⚠ 别写成 `ip netns exec ns-$node pkill -f bgpd`（2026-08-15 实撞）：
# `ip netns exec` **只切换网络命名空间、不切换 PID 命名空间**，于是 pkill 看到的是
# 宿主的整张进程表，会把**所有 docker 容器里的 bgpd 一并杀光**——两套 clab lab
# （midr-backbone 15 台 + midr-join 12 台）当场全灭，且悄无声息。
# 正解 = `ip netns pids` 只列该 netns 内的进程，逐个 kill。
for node in "${NODES[@]}"; do
    for pid in $(ip netns pids "ns-$node" 2>/dev/null); do
        kill -9 "$pid" 2>/dev/null || true
    done
done

echo "[teardown] Removing namespaces..."
for node in "${NODES[@]}"; do
    ip netns del "ns-$node" 2>/dev/null || true
done
ip netns del ns-hub 2>/dev/null || true

rm -rf /tmp/midr-cl-vty
echo "[teardown] Done."
