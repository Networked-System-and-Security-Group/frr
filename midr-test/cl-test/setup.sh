#!/bin/bash
# setup.sh — Create network namespaces and tc-netem link emulation for CL test.
# Topology: star via ns-hub (L3 router), 10 nodes in separate namespaces.
# Link delay is added on hub's EGRESS toward each node (hub→node direction).
set -e

NODES=(g1a g1b g1c g1d g1e g2a g2b g3a g3b newnode)

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
# Delay = one-way delay on hub→node egress interface, applied on EVERY node's
# own hub leg (including between two nodes of the same group — their mutual
# RTT is the SUM of both their own delays, not just one newnode-facing leg).
#   Group 1 (g1a-g1e): 3 ms → RTT from newnode ≈ 3 ms; intra-group (g1x<->g1y)
#                             RTT ≈ 6 ms — both well below 20 ms.        — best, chosen by RECOMMEND
#   Group 3 (g3a-g3b): 6 ms → RTT from newnode ≈ 6 ms; intra-group RTT ≈ 12 ms
#                             — both below 20 ms with margin (NOT 10 ms: that
#                             gives a 20 ms intra-group RTT, right on the
#                             threshold boundary — g3a/g3b would flap their
#                             own PERIODIC_SYNC stay judgement).          — 2nd best, 1st anchor group
#   Group 2 (g2a-g2b): 50 ms → RTT from newnode ≈ 50 ms; intra-group RTT ≈
#                             100 ms — deliberately bad both ways.        — 3rd best, 2nd anchor group
#   newnode:            0 ms → no added delay
# Groups 2 and 3 exist so REP_PROBE_DONE's top-3 ranking has two real runner-up
# candidates to exercise the anchor-connection feature (doc/change-reply.md B2/疑2):
# after RECOMMEND picks group 1, NDS also requests member lists from groups 3 and
# 2 (in that rank order) and CL picks the best 2 nodes in each to anchor-connect.

declare -A HUBIP
declare -A NODEIP
declare -A LOIP
declare -A DELAY
HUBIP=( [g1a]=10.10.11.1  [g1b]=10.10.12.1  [g1c]=10.10.13.1
        [g1d]=10.10.14.1  [g1e]=10.10.15.1
        [g2a]=10.10.21.1  [g2b]=10.10.22.1
        [g3a]=10.10.31.1  [g3b]=10.10.32.1
        [newnode]=10.10.99.1 )
NODEIP=( [g1a]=10.10.11.2  [g1b]=10.10.12.2  [g1c]=10.10.13.2
         [g1d]=10.10.14.2  [g1e]=10.10.15.2
         [g2a]=10.10.21.2  [g2b]=10.10.22.2
         [g3a]=10.10.31.2  [g3b]=10.10.32.2
         [newnode]=10.10.99.2 )
# transport 专用 loopback（= 各节点 router-id）。必须与上面的链路地址分开：
# 静态邻居用链路地址、MIDR overlay 用 transport，同址会让 overlay 撞⑦归属守卫
# （"transport 地址上有一条运维会话，MIDR 不整形运维配置"）而永远建不起来。
LOIP=( [g1a]=10.0.11.1  [g1b]=10.0.12.1  [g1c]=10.0.13.1
       [g1d]=10.0.14.1  [g1e]=10.0.15.1
       [g2a]=10.0.21.1  [g2b]=10.0.22.1
       [g3a]=10.0.31.1  [g3b]=10.0.32.1
       [newnode]=10.0.99.1 )
DELAY=( [g1a]=3ms [g1b]=3ms [g1c]=3ms [g1d]=3ms [g1e]=3ms
        [g2a]=50ms [g2b]=50ms
        [g3a]=6ms [g3b]=6ms
        [newnode]=0ms )

for node in "${NODES[@]}"; do
    hv="v-${node}-h"   # hub-side veth
    nv="v-${node}-n"   # node-side veth
    h_ip="${HUBIP[$node]}"
    n_ip="${NODEIP[$node]}"
    lo_ip="${LOIP[$node]}"
    delay="${DELAY[$node]}"

    # Create veth pair in hub ns, move node end to node ns
    ip -n ns-hub link add "$hv" type veth peer name "$nv"
    ip -n ns-hub link set "$nv" netns "ns-$node"

    ip -n ns-hub  addr add "${h_ip}/30"  dev "$hv"
    ip -n ns-hub  link set "$hv" up

    ip -n "ns-$node" addr add "${n_ip}/30" dev "$nv"
    ip -n "ns-$node" link set "$nv" up
    ip -n "ns-$node" link set lo up
    ip -n "ns-$node" addr add "${lo_ip}/32" dev lo
    ip -n "ns-$node" route add default via "$h_ip"

    # hub 侧回程：到该节点 loopback 走它的链路地址。节点之间靠默认路由 → hub
    # → 这条路由互访 transport。netem 仍加在同一条 veth 上，故延迟照样生效。
    ip netns exec ns-hub ip route add "${lo_ip}/32" via "$n_ip"

    # Apply one-way delay on hub egress toward this node
    if [[ "$delay" != "0ms" ]]; then
        ip netns exec ns-hub tc qdisc add dev "$hv" root netem delay "$delay"
    fi

    printf "  %-8s  node=%-14s  hub=%-14s  hub→node delay=%s\n" \
        "$node" "${n_ip}/30" "${h_ip}/30" "$delay"
done

echo "[setup] Done. Hub routes:"
ip netns exec ns-hub ip route show
