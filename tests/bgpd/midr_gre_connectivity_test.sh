#!/usr/bin/env bash
# MIDR GRE virtual link connectivity test between two containers.
#
# Topology:
#   node1 (10.1.1.11)  <==== docker bridge (test-net) ====>  node2 (10.1.1.12)
#            \________ GRE virtual link (created via MIDR API) ______/
#
# Verifies, using the MIDR control-plane API (test_midr_gre_link), that:
#   A. an IPv4 GRE ("gre") tunnel is established and carries IPv4 traffic
#   B. the same tunnel also carries IPv6 overlay traffic
#   C. an IPv6 GRE ("ip6gre") tunnel is established and carries IPv6 traffic
#   D. the establishment-status API reports UP with a valid ifindex
#   E. teardown removes the interfaces again
#
# Run as root on the docker host (the script calls docker directly).

set -u

NODE1=node1
NODE2=node2
U1=10.1.1.11
U2=10.1.1.12
U6_1=fd00:1::11
U6_2=fd00:1::12
SOCK=/tmp/zserv_midr.api
LD=/opt/midr/lib
LINK=/opt/midr/test_midr_gre_link
ZEBRA=/opt/midr/zebra

PASS=0
FAIL=0

ok()   { echo "[PASS] $*"; PASS=$((PASS + 1)); }
bad()  { echo "[FAIL] $*"; FAIL=$((FAIL + 1)); }
hdr()  { echo; echo "==== $* ===="; }

ex1()  { docker exec -u root "$NODE1" bash -c "$1"; }
ex2()  { docker exec -u root "$NODE2" bash -c "$1"; }
link1() { docker exec -u root "$NODE1" bash -c "LD_LIBRARY_PATH=$LD $LINK $*"; }
link2() { docker exec -u root "$NODE2" bash -c "LD_LIBRARY_PATH=$LD $LINK $*"; }

zebra_start() {
	local n=$1

	docker exec -u root "$n" bash -c "
		pkill -9 zebra 2>/dev/null
		sleep 1
		usermod -aG frrvty root 2>/dev/null || true
		printf 'hostname %s\n' '$n' > /tmp/zebra.conf
		printf 'log file /tmp/zebra.log\n' >> /tmp/zebra.conf
		printf 'debug zebra kernel\n' >> /tmp/zebra.conf
		LD_LIBRARY_PATH=$LD $ZEBRA -u root -g root -f /tmp/zebra.conf \
			-i /tmp/zebra.pid -z $SOCK --vty_socket /tmp -d
	"
	sleep 3
	if docker exec -u root "$n" bash -c "test -S $SOCK"; then
		ok "zebra running on $n"
	else
		bad "zebra failed to start on $n"
		docker exec -u root "$n" bash -c "tail -5 /tmp/zebra.log 2>/dev/null; true"
	fi
}

zebra_stop() { docker exec -u root "$1" bash -c "pkill -9 zebra 2>/dev/null; true"; }

iface_exists() { docker exec -u root "$1" bash -c "ip link show '$2' >/dev/null 2>&1"; }

iface_kind() {
	local out

	out=$(docker exec -u root "$1" bash -c "ip -d link show '$2' 2>/dev/null")
	if echo "$out" | grep -q 'ip6gre'; then
		echo ip6gre
	elif echo "$out" | grep -q 'gre'; then
		echo gre
	fi
}

ping4_ok() { docker exec -u root "$1" ping -c 3 -W 2 "$2" >/dev/null 2>&1; }
ping6_ok() { docker exec -u root "$1" ping6 -c 3 -W 2 "$2" >/dev/null 2>&1; }

state_of() { echo "$1" | grep -oE 'state=[a-z]+' | head -1 | cut -d= -f2; }
idx_of()   { echo "$1" | grep -oE 'ifindex=[0-9]+' | head -1 | cut -d= -f2; }

cleanup() {
	link1 teardown --sock $SOCK --name gre1 >/dev/null 2>&1 || true
	link2 teardown --sock $SOCK --name gre1 >/dev/null 2>&1 || true
	link1 teardown --sock $SOCK --name gre6 >/dev/null 2>&1 || true
	link2 teardown --sock $SOCK --name gre6 >/dev/null 2>&1 || true
	ex1 "ip link del gre1 2>/dev/null; ip link del gre6 2>/dev/null; true"
	ex2 "ip link del gre1 2>/dev/null; ip link del gre6 2>/dev/null; true"
}

trap 'cleanup; zebra_stop $NODE1; zebra_stop $NODE2' EXIT

echo "=== MIDR GRE two-container connectivity test ==="
echo "node1=$U1  node2=$U2"

cleanup
zebra_start "$NODE1"
zebra_start "$NODE2"

if ping4_ok "$NODE1" "$U2"; then ok "underlay IPv4 $U1 -> $U2"; else bad "underlay IPv4 unreachable"; fi

ex1 "sysctl -w net.ipv6.conf.eth0.disable_ipv6=0 >/dev/null 2>&1 || true
     ip addr add $U6_1/64 dev eth0 nodad 2>/dev/null || true"
ex2 "sysctl -w net.ipv6.conf.eth0.disable_ipv6=0 >/dev/null 2>&1 || true
     ip addr add $U6_2/64 dev eth0 nodad 2>/dev/null || true"
sleep 2
if ping6_ok "$NODE1" "$U6_2"; then ok "underlay IPv6 $U6_1 -> $U6_2"
			       else bad "underlay IPv6 unreachable (ip6gre test will fail)"; fi

hdr "A. IPv4 GRE tunnel (gre) + IPv4 overlay"
R1=$(link1 setup --sock $SOCK --name gre1 --local $U1 --remote $U2 --ip 192.168.100.1/30 --mtu 1400)
R2=$(link2 setup --sock $SOCK --name gre1 --local $U2 --remote $U1 --ip 192.168.100.2/30 --mtu 1400)
echo "$R1"; echo "$R2"

[ "$(state_of "$R1")" = up ] && ok "node1 gre1 established (ifindex $(idx_of "$R1"))" \
			    || bad "node1 gre1 not established"
[ "$(state_of "$R2")" = up ] && ok "node2 gre1 established (ifindex $(idx_of "$R2"))" \
			    || bad "node2 gre1 not established"
[ "$(iface_kind "$NODE1" gre1)" = gre ] && ok "node1 gre1 is a gre device" || bad "node1 gre1 kind wrong"
[ "$(iface_kind "$NODE2" gre1)" = gre ] && ok "node2 gre1 is a gre device" || bad "node2 gre1 kind wrong"

if ping4_ok "$NODE1" 192.168.100.2; then ok "IPv4 connectivity node1 -> node2 over gre1"
				       else bad "IPv4 connectivity over gre1 FAILED"; fi
if ping4_ok "$NODE2" 192.168.100.1; then ok "IPv4 connectivity node2 -> node1 over gre1"
				       else bad "IPv4 connectivity (reverse) over gre1 FAILED"; fi

hdr "B. IPv6 overlay over the IPv4 GRE tunnel"
ex1 "ip addr add fd00:100::1/64 dev gre1 2>/dev/null || true; ip link set gre1 up"
ex2 "ip addr add fd00:100::2/64 dev gre1 2>/dev/null || true; ip link set gre1 up"
sleep 2
if ping6_ok "$NODE1" fd00:100::2; then ok "IPv6 connectivity node1 -> node2 over gre1"
				     else bad "IPv6 connectivity over gre1 FAILED"; fi

hdr "C. IPv6 GRE tunnel (ip6gre) + IPv6 overlay"
R3=$(link1 setup --sock $SOCK --name gre6 --local $U6_1 --remote $U6_2 --ip6 fd00:200::1/64 --mtu 1400)
R4=$(link2 setup --sock $SOCK --name gre6 --local $U6_2 --remote $U6_1 --ip6 fd00:200::2/64 --mtu 1400)
echo "$R3"; echo "$R4"

[ "$(state_of "$R3")" = up ] && ok "node1 gre6 established (ifindex $(idx_of "$R3"))" \
			    || bad "node1 gre6 not established"
[ "$(state_of "$R4")" = up ] && ok "node2 gre6 established (ifindex $(idx_of "$R4"))" \
			    || bad "node2 gre6 not established"
[ "$(iface_kind "$NODE1" gre6)" = ip6gre ] && ok "node1 gre6 is an ip6gre device" || bad "node1 gre6 kind wrong"
[ "$(iface_kind "$NODE2" gre6)" = ip6gre ] && ok "node2 gre6 is an ip6gre device" || bad "node2 gre6 kind wrong"

if ping6_ok "$NODE1" fd00:200::2; then ok "IPv6 connectivity node1 -> node2 over gre6"
				     else bad "IPv6 connectivity over gre6 FAILED"; fi
if ping6_ok "$NODE2" fd00:200::1; then ok "IPv6 connectivity node2 -> node1 over gre6"
				     else bad "IPv6 connectivity (reverse) over gre6 FAILED"; fi

hdr "D. Teardown"
T1=$(link1 teardown --sock $SOCK --name gre1); echo "$T1"
T2=$(link2 teardown --sock $SOCK --name gre1); echo "$T2"
T3=$(link1 teardown --sock $SOCK --name gre6); echo "$T3"
T4=$(link2 teardown --sock $SOCK --name gre6); echo "$T4"
sleep 2
iface_exists "$NODE1" gre1 && bad "node1 gre1 still present" || ok "node1 gre1 removed"
iface_exists "$NODE2" gre1 && bad "node2 gre1 still present" || ok "node2 gre1 removed"
iface_exists "$NODE1" gre6 && bad "node1 gre6 still present" || ok "node1 gre6 removed"
iface_exists "$NODE2" gre6 && bad "node2 gre6 still present" || ok "node2 gre6 removed"

echo
echo "=== summary: PASS=$PASS FAIL=$FAIL ==="
[ "$FAIL" -eq 0 ]
