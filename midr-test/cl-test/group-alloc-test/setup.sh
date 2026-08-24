#!/bin/bash
# setup.sh — netns topology for the group-id-allocation race test.
#
# Minimal star topology, hub = b2:
#
#         b1 --- b2 --- b3
#                 |  \
#                 x    y
#
# b1/b2/b3 are bootstraps (full pairwise mesh via `midr session`/`midr
# bootstrap`, BGP-LS sessions relayed through b2 the same way it relays IP
# for everyone -- a session doesn't need a *direct* link, just a route).
# x/y are zero-config joiners with no static group-id.
#
# This test is about the CREATE-race protocol/timing, not RTT-based
# clustering quality, so all four links get the same small uniform
# delay/bandwidth -- no differentiation needed.
#
# b1 is deliberately given the numerically smallest router-id (10.0.5.1) so
# it's the one midr_nds_pick_group_allocator() elects as the group-id
# allocator on every node (see CLAUDE.md's group-id collision writeup) --
# run_test.sh's "repair" scenario exploits this by simply not starting b1,
# forcing every GROUP_ALLOC_REQ to fail over to the local-estimate fallback
# without needing any artificial packet loss.

set -e

echo "[ga-setup] Creating namespaces..."
NODES=(b1 b2 b3 x y)
for n in "${NODES[@]}"; do
    ip netns add "ns-ga-$n" 2>/dev/null || { ip netns del "ns-ga-$n"; ip netns add "ns-ga-$n"; }
    ip -n "ns-ga-$n" link set lo up
done

echo "[ga-setup] Enabling ip_forward on the hub (b2)..."
ip netns exec ns-ga-b2 sysctl -qw net.ipv4.ip_forward=1

declare -A LOOPBACK=( [b1]=10.99.5.1 [b2]=10.99.5.2 [b3]=10.99.5.3 [x]=10.99.5.11 [y]=10.99.5.12 )
for n in "${NODES[@]}"; do
    ip -n "ns-ga-$n" addr add "${LOOPBACK[$n]}/32" dev lo
done

echo "[ga-setup] Wiring 4 point-to-point links (hub = b2)..."
# spoke sub delay bandwidth
L1=(b1 0  2ms 100mbit)
L2=(b3 4  2ms 100mbit)
L3=(x  8  2ms 100mbit)
L4=(y  12 2ms 100mbit)

for i in 1 2 3 4; do
    eval "link=(\"\${L$i[@]}\")"
    spoke="${link[0]}"; sub="${link[1]}"; delay="${link[2]}"; bw="${link[3]}"
    hv="vga${sub}h"; sv="vga${sub}s"
    hip="10.10.55.$((sub+1))"; sip="10.10.55.$((sub+2))"

    ip -n ns-ga-b2 link add "$hv" type veth peer name "$sv" netns "ns-ga-$spoke"
    ip -n ns-ga-b2 addr add "${hip}/30" dev "$hv"
    ip -n ns-ga-b2 link set "$hv" up
    ip -n "ns-ga-$spoke" addr add "${sip}/30" dev "$sv"
    ip -n "ns-ga-$spoke" link set "$sv" up

    ip netns exec ns-ga-b2 tc qdisc add dev "$hv" root netem delay "$delay" rate "$bw"
    ip netns exec "ns-ga-$spoke" tc qdisc add dev "$sv" root netem delay "$delay" rate "$bw"

    printf "  b2 -- %-3s  %-15s  %-15s  delay=%s bw=%s\n" "$spoke" "${hip}/30" "${sip}/30" "$delay" "$bw"
done

echo "[ga-setup] Routing: spokes -> default via b2..."
declare -A UPLINK_VIA=( [b1]=10.10.55.1 [b3]=10.10.55.5 [x]=10.10.55.9 [y]=10.10.55.13 )
for spoke in b1 b3 x y; do
    ip -n "ns-ga-$spoke" route add default via "${UPLINK_VIA[$spoke]}"
done

echo "[ga-setup] Routing: b2 -> each spoke's loopback via its link address..."
# A spoke's loopback (10.99.5.x) is not reachable just because the /30 link
# subnet is directly connected -- same bug class documented in
# backbone-test/setup.sh. b2 relays for everyone, so it needs an explicit
# /32 per spoke.
ip -n ns-ga-b2 route add 10.99.5.1/32  via 10.10.55.2   # b1
ip -n ns-ga-b2 route add 10.99.5.3/32  via 10.10.55.6   # b3
ip -n ns-ga-b2 route add 10.99.5.11/32 via 10.10.55.10  # x
ip -n ns-ga-b2 route add 10.99.5.12/32 via 10.10.55.14  # y

echo "[ga-setup] Done."
