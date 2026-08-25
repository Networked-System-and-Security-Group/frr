#!/bin/bash
# setup.sh — Create network namespaces and tc-netem link emulation for the
# organic-growth test. Star topology via ns-gr-hub, 5 nodes: one dedicated
# bootstrap (b), one rep (r) and three sequential joiners (j1, j2, j3).
# 【我方适配】原版 4 节点里 r 兼任引导+群代表；本仓库 2026-08-13 起引导与群代表
# 角色硬互斥（引导优先，group-id/group-rep 在引导上一律拒绝），故拆出专职引导 b。 Uses its own namespace prefix
# (ns-gr-*) and subnet (10.20.x.x) so it never collides with the main
# ../setup.sh 10-node topology and can be run independently.
set -e

NODES=(b r j1 j2 j3)

echo "[growth-setup] Removing any previous namespaces..."
for node in "${NODES[@]}"; do
    ip netns del "ns-gr-$node" 2>/dev/null || true
done
ip netns del ns-gr-hub 2>/dev/null || true

echo "[growth-setup] Creating namespaces..."
ip netns add ns-gr-hub
for node in "${NODES[@]}"; do
    ip netns add "ns-gr-$node"
done

ip -n ns-gr-hub link set lo up
ip netns exec ns-gr-hub sysctl -qw net.ipv4.ip_forward=1

# All nodes get a uniform 2 ms one-way hub-egress delay, so any pairwise RTT
# (both legs delayed, same as ../setup.sh's model) is ~4 ms — comfortably
# under MIDR_CL_JOIN_RTT_THRESHOLD_US (20 ms). Link quality isn't what this
# test is exercising; group size is.
declare -A HUBIP=( [b]=10.20.5.1  [r]=10.20.1.1  [j1]=10.20.2.1  [j2]=10.20.3.1  [j3]=10.20.4.1 )
declare -A NODEIP=( [b]=10.20.5.2 [r]=10.20.1.2 [j1]=10.20.2.2 [j2]=10.20.3.2 [j3]=10.20.4.2 )
declare -A TRANSPORT=( [b]=10.0.100.1 [r]=10.0.101.1 [j1]=10.0.102.1 [j2]=10.0.103.1 [j3]=10.0.104.1 )

for node in "${NODES[@]}"; do
    hv="v-gr-${node}-h"
    nv="v-gr-${node}-n"
    h_ip="${HUBIP[$node]}"
    n_ip="${NODEIP[$node]}"
    transport="${TRANSPORT[$node]}"

    ip -n ns-gr-hub link add "$hv" type veth peer name "$nv"
    ip -n ns-gr-hub link set "$nv" netns "ns-gr-$node"

    ip -n ns-gr-hub  addr add "${h_ip}/30"  dev "$hv"
    ip -n ns-gr-hub  link set "$hv" up

    ip -n "ns-gr-$node" addr add "${n_ip}/30" dev "$nv"
    ip -n "ns-gr-$node" link set "$nv" up
    ip -n "ns-gr-$node" link set lo up
    ip -n "ns-gr-$node" addr add "${transport}/32" dev lo
    ip -n "ns-gr-$node" route add default via "$h_ip"
    ip -n ns-gr-hub route add "${transport}/32" via "$n_ip"

    ip netns exec ns-gr-hub tc qdisc add dev "$hv" root netem delay 2ms

    printf "  %-4s  node=%-14s  hub=%-14s  hub→node delay=2ms\n" \
        "$node" "${n_ip}/30" "${h_ip}/30"
done

echo "[growth-setup] Done. Hub routes:"
ip netns exec ns-gr-hub ip route show
