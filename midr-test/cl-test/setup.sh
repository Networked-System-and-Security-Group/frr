#!/bin/bash
# setup.sh — Create network namespaces and tc-netem link emulation for CL test.
# Topology: star via ns-hub (L3 router), 8 nodes in separate namespaces.
# Link delay is added on hub's EGRESS toward each node (hub→node direction).
set -e

NODES=(g1a g1b g1c g1d g1e g2a g2b newnode)

echo "[setup] Removing any previous namespaces..."
for node in "${NODES[@]}"; do
    ip netns del "ns-$node" 2>/dev/null || true
done
ip netns del ns-hub 2>/dev/null || true

echo "[setup] Creating namespaces..."
ip netns add ns-hub
for node in "${NODES[@]}"; do
    ip netns add "ns-$node"
done

ip -n ns-hub link set lo up
ip netns exec ns-hub sysctl -qw net.ipv4.ip_forward=1

# /30 subnets: each node gets hub.x.1 (hub side) / hub.x.2 (node side)
# Delay = one-way delay on hub→node egress interface (controls RTT from newnode)
#   Group 1 (g1a-g1e): 3 ms  → RTT from newnode ≈  3 ms  (well below 20 ms threshold)
#   Group 2 (g2a-g2b): 50 ms → RTT from newnode ≈ 50 ms  (well above 20 ms threshold)
#   newnode:            0 ms  → no added delay

declare -A HUBIP
declare -A NODEIP
declare -A DELAY
HUBIP=( [g1a]=10.10.11.1  [g1b]=10.10.12.1  [g1c]=10.10.13.1
        [g1d]=10.10.14.1  [g1e]=10.10.15.1
        [g2a]=10.10.21.1  [g2b]=10.10.22.1
        [newnode]=10.10.99.1 )
NODEIP=( [g1a]=10.10.11.2  [g1b]=10.10.12.2  [g1c]=10.10.13.2
         [g1d]=10.10.14.2  [g1e]=10.10.15.2
         [g2a]=10.10.21.2  [g2b]=10.10.22.2
         [newnode]=10.10.99.2 )
DELAY=( [g1a]=3ms [g1b]=3ms [g1c]=3ms [g1d]=3ms [g1e]=3ms
        [g2a]=50ms [g2b]=50ms [newnode]=0ms )

for node in "${NODES[@]}"; do
    hv="v-${node}-h"   # hub-side veth
    nv="v-${node}-n"   # node-side veth
    h_ip="${HUBIP[$node]}"
    n_ip="${NODEIP[$node]}"
    delay="${DELAY[$node]}"

    # Create veth pair in hub ns, move node end to node ns
    ip -n ns-hub link add "$hv" type veth peer name "$nv"
    ip -n ns-hub link set "$nv" netns "ns-$node"

    ip -n ns-hub  addr add "${h_ip}/30"  dev "$hv"
    ip -n ns-hub  link set "$hv" up

    ip -n "ns-$node" addr add "${n_ip}/30" dev "$nv"
    ip -n "ns-$node" link set "$nv" up
    ip -n "ns-$node" link set lo up
    ip -n "ns-$node" route add default via "$h_ip"

    # Apply one-way delay on hub egress toward this node
    if [[ "$delay" != "0ms" ]]; then
        ip netns exec ns-hub tc qdisc add dev "$hv" root netem delay "$delay"
    fi

    printf "  %-8s  node=%-14s  hub=%-14s  hub→node delay=%s\n" \
        "$node" "${n_ip}/30" "${h_ip}/30" "$delay"
done

echo "[setup] Done. Hub routes:"
ip netns exec ns-hub ip route show
