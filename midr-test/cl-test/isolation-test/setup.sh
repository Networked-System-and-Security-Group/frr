#!/bin/bash
# setup.sh — Create network namespaces and tc-netem link emulation for the
# isolation/RECONNECT test. Star topology via ns-iso-hub, 4 nodes:
#   a — bootstrap only (no group of its own; just serves a rep directory
#       listing d and e). Stays up for the whole test.
#   d — singleton group rep, best-ranked (lower delay) -> f's JOIN target.
#   e — singleton group rep, second-ranked -> f's sole ANCHOR candidate.
#   f — the joining node. After the initial join it has exactly two
#       established sessions: d (its group) and e (its anchor) — nothing
#       else. Killing d and e together (done by run_test.sh, not here)
#       leaves f with zero established sessions while a is still reachable,
#       reproducing the isolation scenario.
# Uses its own namespace prefix (ns-iso-*) and subnet (10.30.x.x) so it never
# collides with the main cl-test or growth-test topologies.
set -e

NODES=(a d e f)

echo "[iso-setup] Removing any previous namespaces..."
for node in "${NODES[@]}"; do
    ip netns del "ns-iso-$node" 2>/dev/null || true
done
ip netns del ns-iso-hub 2>/dev/null || true

echo "[iso-setup] Creating namespaces..."
ip netns add ns-iso-hub
for node in "${NODES[@]}"; do
    ip netns add "ns-iso-$node"
done

ip -n ns-iso-hub link set lo up
ip netns exec ns-iso-hub sysctl -qw net.ipv4.ip_forward=1

declare -A HUBIP=( [a]=10.30.1.1  [d]=10.30.2.1  [e]=10.30.3.1  [f]=10.30.4.1 )
declare -A NODEIP=( [a]=10.30.1.2 [d]=10.30.2.2 [e]=10.30.3.2 [f]=10.30.4.2 )
declare -A TRANSPORT=( [a]=10.0.201.1 [d]=10.0.202.1 [e]=10.0.203.1 [f]=10.0.204.1 )
# d ranks ahead of e (lower delay) so REP_PROBE_DONE picks d as the primary
# (JOIN) target and e as the sole runner-up (ANCHOR) candidate. a and f carry
# no delay — a is never probed as a link target (it's not in anyone's rep
# directory as a candidate, only serves one), and f is the measuring node.
declare -A DELAY=( [a]=0ms [d]=2ms [e]=3ms [f]=0ms )

for node in "${NODES[@]}"; do
    hv="v-iso-${node}-h"
    nv="v-iso-${node}-n"
    h_ip="${HUBIP[$node]}"
    n_ip="${NODEIP[$node]}"
    transport="${TRANSPORT[$node]}"
    delay="${DELAY[$node]}"

    ip -n ns-iso-hub link add "$hv" type veth peer name "$nv"
    ip -n ns-iso-hub link set "$nv" netns "ns-iso-$node"

    ip -n ns-iso-hub  addr add "${h_ip}/30"  dev "$hv"
    ip -n ns-iso-hub  link set "$hv" up

    ip -n "ns-iso-$node" addr add "${n_ip}/30" dev "$nv"
    ip -n "ns-iso-$node" link set "$nv" up
    ip -n "ns-iso-$node" link set lo up
    ip -n "ns-iso-$node" addr add "${transport}/32" dev lo
    ip -n "ns-iso-$node" route add default via "$h_ip"
    ip -n ns-iso-hub route add "${transport}/32" via "$n_ip"

    if [[ "$delay" != "0ms" ]]; then
        ip netns exec ns-iso-hub tc qdisc add dev "$hv" root netem delay "$delay"
    fi

    printf "  %-4s  node=%-14s  hub=%-14s  hub→node delay=%s\n" \
        "$node" "${n_ip}/30" "${h_ip}/30" "$delay"
done

echo "[iso-setup] Done. Hub routes:"
ip netns exec ns-iso-hub ip route show
