#!/usr/bin/env bash
# Create the 10-node CL topology for one address family.

set -Eeuo pipefail

NODES=(g1a g1b g1c g1d g1e g2a g2b g3a g3b newnode)
ADDRESS_FAMILY="ipv4"

if [[ "${1:-}" == "--address-family" ]]; then
    ADDRESS_FAMILY="${2:-}"
fi
if [[ "$ADDRESS_FAMILY" != "ipv4" && "$ADDRESS_FAMILY" != "ipv6" ]]; then
    echo "Usage: $0 [--address-family ipv4|ipv6]" >&2
    exit 2
fi

echo "[setup] Removing any previous namespaces..."
for node in "${NODES[@]}"; do
    ip netns del "ns-$node" 2>/dev/null || true
done
ip netns del ns-hub 2>/dev/null || true

echo "[setup] Creating $ADDRESS_FAMILY namespaces..."
ip netns add ns-hub
for node in "${NODES[@]}"; do
    ip netns add "ns-$node"
done

ip -n ns-hub link set lo up

declare -A HUBIP NODEIP LOIP DELAY
if [[ "$ADDRESS_FAMILY" == "ipv4" ]]; then
    ip netns exec ns-hub sysctl -qw net.ipv4.ip_forward=1
    HUBIP=( [g1a]=10.10.11.1 [g1b]=10.10.12.1 [g1c]=10.10.13.1
            [g1d]=10.10.14.1 [g1e]=10.10.15.1
            [g2a]=10.10.21.1 [g2b]=10.10.22.1
            [g3a]=10.10.31.1 [g3b]=10.10.32.1
            [newnode]=10.10.99.1 )
    NODEIP=( [g1a]=10.10.11.2 [g1b]=10.10.12.2 [g1c]=10.10.13.2
             [g1d]=10.10.14.2 [g1e]=10.10.15.2
             [g2a]=10.10.21.2 [g2b]=10.10.22.2
             [g3a]=10.10.31.2 [g3b]=10.10.32.2
             [newnode]=10.10.99.2 )
    LOIP=( [g1a]=172.31.11.1 [g1b]=172.31.12.1 [g1c]=172.31.13.1
           [g1d]=172.31.14.1 [g1e]=172.31.15.1
           [g2a]=172.31.21.1 [g2b]=172.31.22.1
           [g3a]=172.31.31.1 [g3b]=172.31.32.1
           [newnode]=172.31.99.1 )
    LINK_PREFIX=30
    LOOPBACK_PREFIX=32
    IP_MODE=()
else
    ip netns exec ns-hub sysctl -qw net.ipv6.conf.all.forwarding=1
    HUBIP=( [g1a]=fd10:10:11::1 [g1b]=fd10:10:12::1 [g1c]=fd10:10:13::1
            [g1d]=fd10:10:14::1 [g1e]=fd10:10:15::1
            [g2a]=fd10:10:21::1 [g2b]=fd10:10:22::1
            [g3a]=fd10:10:31::1 [g3b]=fd10:10:32::1
            [newnode]=fd10:10:99::1 )
    NODEIP=( [g1a]=fd10:10:11::2 [g1b]=fd10:10:12::2 [g1c]=fd10:10:13::2
             [g1d]=fd10:10:14::2 [g1e]=fd10:10:15::2
             [g2a]=fd10:10:21::2 [g2b]=fd10:10:22::2
             [g3a]=fd10:10:31::2 [g3b]=fd10:10:32::2
             [newnode]=fd10:10:99::2 )
    LOIP=( [g1a]=fd00:0:11::1 [g1b]=fd00:0:12::1 [g1c]=fd00:0:13::1
           [g1d]=fd00:0:14::1 [g1e]=fd00:0:15::1
           [g2a]=fd00:0:21::1 [g2b]=fd00:0:22::1
           [g3a]=fd00:0:31::1 [g3b]=fd00:0:32::1
           [newnode]=fd00:0:99::1 )
    LINK_PREFIX=64
    LOOPBACK_PREFIX=128
    IP_MODE=(-6)
fi

DELAY=( [g1a]=3ms [g1b]=3ms [g1c]=3ms [g1d]=3ms [g1e]=3ms
        [g2a]=50ms [g2b]=50ms [g3a]=6ms [g3b]=6ms [newnode]=0ms )

for node in "${NODES[@]}"; do
    hv="v-${node}-h"
    nv="v-${node}-n"
    h_ip="${HUBIP[$node]}"
    n_ip="${NODEIP[$node]}"
    lo_ip="${LOIP[$node]}"
    delay="${DELAY[$node]}"

    ip -n ns-hub link add "$hv" type veth peer name "$nv"
    ip -n ns-hub link set "$nv" netns "ns-$node"
    ip -n ns-hub "${IP_MODE[@]}" addr add "${h_ip}/${LINK_PREFIX}" dev "$hv"
    ip -n ns-hub link set "$hv" up

    ip -n "ns-$node" "${IP_MODE[@]}" addr add \
        "${n_ip}/${LINK_PREFIX}" dev "$nv"
    ip -n "ns-$node" link set "$nv" up
    ip -n "ns-$node" link set lo up
    ip -n "ns-$node" "${IP_MODE[@]}" addr add \
        "${lo_ip}/${LOOPBACK_PREFIX}" dev lo
    ip -n "ns-$node" "${IP_MODE[@]}" route add default via "$h_ip"
    ip netns exec ns-hub ip "${IP_MODE[@]}" route add \
        "${lo_ip}/${LOOPBACK_PREFIX}" via "$n_ip"

    if [[ "$delay" != "0ms" ]]; then
        ip netns exec ns-hub tc qdisc add dev "$hv" root netem delay "$delay"
    fi

    printf "  %-8s node=%-24s hub=%-24s delay=%s\n" \
        "$node" "${n_ip}/${LINK_PREFIX}" "${h_ip}/${LINK_PREFIX}" "$delay"
done

echo "[setup] Done. Hub routes:"
ip netns exec ns-hub ip "${IP_MODE[@]}" route show
