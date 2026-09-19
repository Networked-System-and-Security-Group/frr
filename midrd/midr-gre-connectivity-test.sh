#!/usr/bin/env bash
# MIDR GRE virtual-link connectivity test between two containers.
#
# Topology:
#   node1 (10.1.1.11)  <==== docker bridge ====>  node2 (10.1.1.12)
#            \________ GRE virtual link (created via the MIDR API) ______/
#
# Verifies, through the midrd GRE API helper (gre-link-tool), that:
#   A. an IPv4 GRE ("gre") tunnel carries IPv4 overlay traffic
#   B. the same tunnel also carries IPv6 overlay traffic
#   C. an IPv6 GRE ("ip6gre") tunnel carries IPv6 overlay traffic
#   D. the establishment-status API reports UP with a stable ifindex
#   E. teardown removes the interfaces again
#
# Run as root on the docker host: node1/node2 must already exist on a shared
# bridge and run frr-ubuntu24-ymy:init.  The artifacts (tool, zebra, libs) are
# copied out of the build container frr-ubuntu24-ymy into /opt/midr-dp/ inside
# both nodes so the old /opt/midr assets stay untouched.
set -euo pipefail

NODE1=${MIDRD_GRE_NODE1:-node1}
NODE2=${MIDRD_GRE_NODE2:-node2}
BUILD=${MIDRD_BUILD_CONTAINER:-frr-ubuntu24-ymy}
U1=10.1.1.11
U2=10.1.1.12
U6_1=fd00:1::11
U6_2=fd00:1::12
SOCK=/tmp/zserv_midr.api
LD=/opt/midr-dp/lib
LINK=/opt/midr-dp/midrd-gre-tool
ZEBRA=/opt/midr-dp/zebra
BUILD_TOOL=/tmp/midrd-build-harness/midrd-gre-tool
BUILD_ZEBRA=/home/frr/frr-midrd3/zebra/.libs/zebra
BUILD_LIB=/home/frr/frr-midrd3/lib/.libs
STAGE=

PASS=0
FAIL=0

ok()  { echo "[PASS] $*"; PASS=$((PASS + 1)); }
bad() { echo "[FAIL] $*"; FAIL=$((FAIL + 1)); }
hdr() { echo; echo "==== $* ===="; }

ex1()  { docker exec -u root "$NODE1" bash -c "$1"; }
ex2()  { docker exec -u root "$NODE2" bash -c "$1"; }
link1() { docker exec -u root "$NODE1" bash -c "LD_LIBRARY_PATH=$LD $LINK $*"; }
link2() { docker exec -u root "$NODE2" bash -c "LD_LIBRARY_PATH=$LD $LINK $*"; }

for c in "$NODE1" "$NODE2" "$BUILD"; do
	docker inspect "$c" >/dev/null 2>&1 || {
		echo "missing container: $c" >&2
		exit 2
	}
done
command -v docker >/dev/null 2>&1 || { echo 'docker is required' >&2; exit 2; }

# fetch_lib <soname>: stage the build-container copy of a shared library.
fetch_lib() {
	local name=$1 path real

	path=$(docker exec "$BUILD" bash -lc \
		"export LD_LIBRARY_PATH=$BUILD_LIB; ldd $BUILD_TOOL $BUILD_ZEBRA 2>/dev/null" \
		| awk -v lib="$name" '$1 == lib { print $3; exit }')
	[[ -n "$path" ]] || return 1
	real=$(docker exec "$BUILD" readlink -f "$path")
	[[ -n "$real" ]] || return 1
	docker cp "$BUILD:$real" "$STAGE/lib/$name" >/dev/null
}

install_artifacts() {
	docker cp "$BUILD:$BUILD_TOOL" "$STAGE/midrd-gre-tool" >/dev/null
	docker cp "$BUILD:$BUILD_ZEBRA" "$STAGE/zebra" >/dev/null
	fetch_lib libfrr.so.0 || {
		echo "unable to stage libfrr.so.0 from $BUILD" >&2
		exit 2
	}
	fetch_lib libunwind.so.8 || true

	local n

	for n in "$NODE1" "$NODE2"; do
		docker exec -u root "$n" mkdir -p /opt/midr-dp/lib
		docker cp "$STAGE/midrd-gre-tool" "$n:/opt/midr-dp/midrd-gre-tool" >/dev/null
		docker cp "$STAGE/zebra" "$n:/opt/midr-dp/zebra" >/dev/null
		docker cp "$STAGE/lib/." "$n:/opt/midr-dp/lib/" >/dev/null
		docker exec -u root "$n" bash -c \
			"chmod 0755 /opt/midr-dp/midrd-gre-tool /opt/midr-dp/zebra; \
			 ln -sf libfrr.so.0 /opt/midr-dp/lib/libfrr.so"
	done
}

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

	out=$(docker exec -u root "$1" bash -c "ip -d link show '$2' 2>/dev/null" || true)
	if echo "$out" | grep -q 'ip6gre'; then
		echo ip6gre
	elif echo "$out" | grep -q 'gre'; then
		echo gre
	fi
}

ping4_ok() { docker exec -u root "$1" ping -c 3 -W 2 "$2" >/dev/null 2>&1; }
ping6_ok() { docker exec -u root "$1" ping6 -c 3 -W 2 "$2" >/dev/null 2>&1; }

state_of() { echo "$1" | grep -E '^MIDR_GRE ' | grep -oE 'state=[a-z]+' | head -1 | cut -d= -f2; }
idx_of()   { echo "$1" | grep -E '^MIDR_GRE ' | grep -oE 'ifindex=[0-9]+' | head -1 | cut -d= -f2; }

cleanup() {
	if docker exec -u root "$NODE1" bash -c "test -S $SOCK" 2>/dev/null; then
		link1 teardown --sock $SOCK --name gre1 >/dev/null 2>&1 || true
		link1 teardown --sock $SOCK --name gre6 >/dev/null 2>&1 || true
	fi
	if docker exec -u root "$NODE2" bash -c "test -S $SOCK" 2>/dev/null; then
		link2 teardown --sock $SOCK --name gre1 >/dev/null 2>&1 || true
		link2 teardown --sock $SOCK --name gre6 >/dev/null 2>&1 || true
	fi
	ex1 "ip link del gre1 2>/dev/null; ip link del gre6 2>/dev/null; true"
	ex2 "ip link del gre1 2>/dev/null; ip link del gre6 2>/dev/null; true"
	zebra_stop "$NODE1"
	zebra_stop "$NODE2"
	if [[ -n "$STAGE" && -d "$STAGE" ]]; then
		rm -rf "$STAGE"
	fi
}
trap cleanup EXIT

echo "=== MIDR GRE two-container connectivity test ==="
echo "node1=$U1  node2=$U2  build=$BUILD"
echo "artifacts: $LINK + $ZEBRA + $LD (from $BUILD)"

STAGE=$(mktemp -d "${TMPDIR:-/tmp}/midr-gre-art.XXXXXX")
mkdir -p "$STAGE/lib"
install_artifacts
ok "artifacts staged into /opt/midr-dp on both nodes"

cleanup
zebra_start "$NODE1"
zebra_start "$NODE2"

if ping4_ok "$NODE1" "$U2"; then ok "underlay IPv4 $U1 -> $U2"; else bad "underlay IPv4 unreachable"; fi

ex1 "sysctl -w net.ipv6.conf.eth0.disable_ipv6=0 >/dev/null 2>&1 || true
     ip addr add $U6_1/64 dev eth0 nodad 2>/dev/null || true"
ex2 "sysctl -w net.ipv6.conf.eth0.disable_ipv6=0 >/dev/null 2>&1 || true
     ip addr add $U6_2/64 dev eth0 nodad 2>/dev/null || true"
sleep 2
if ping6_ok "$NODE1" "$U6_2"; then
	ok "underlay IPv6 $U6_1 -> $U6_2"
else
	bad "underlay IPv6 unreachable (ip6gre test will fail)"
fi

hdr "A. IPv4 GRE tunnel (gre) + IPv4 overlay"
R1=$(link1 setup --sock $SOCK --name gre1 --local $U1 --remote $U2 --mtu 1400 || true)
R2=$(link2 setup --sock $SOCK --name gre1 --local $U2 --remote $U1 --mtu 1400 || true)
echo "$R1"; echo "$R2"

if [[ "$(state_of "$R1")" == up ]]; then
	ok "node1 gre1 established (ifindex $(idx_of "$R1"))"
else
	bad "node1 gre1 not established"
fi
if [[ "$(state_of "$R2")" == up ]]; then
	ok "node2 gre1 established (ifindex $(idx_of "$R2"))"
else
	bad "node2 gre1 not established"
fi
[[ "$(iface_kind "$NODE1" gre1)" == gre ]] && ok "node1 gre1 is a gre device" || bad "node1 gre1 kind wrong"
[[ "$(iface_kind "$NODE2" gre1)" == gre ]] && ok "node2 gre1 is a gre device" || bad "node2 gre1 kind wrong"

ex1 "ip addr add 192.168.100.1/30 dev gre1 2>/dev/null || true; ip link set gre1 up"
ex2 "ip addr add 192.168.100.2/30 dev gre1 2>/dev/null || true; ip link set gre1 up"
sleep 2
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
if ping6_ok "$NODE2" fd00:100::1; then ok "IPv6 connectivity node2 -> node1 over gre1"
				     else bad "IPv6 connectivity (reverse) over gre1 FAILED"; fi

hdr "C. IPv6 GRE tunnel (ip6gre) + IPv6 overlay"
R3=$(link1 setup --sock $SOCK --name gre6 --local $U6_1 --remote $U6_2 --mtu 1400 || true)
R4=$(link2 setup --sock $SOCK --name gre6 --local $U6_2 --remote $U6_1 --mtu 1400 || true)
echo "$R3"; echo "$R4"

if [[ "$(state_of "$R3")" == up ]]; then
	ok "node1 gre6 established (ifindex $(idx_of "$R3"))"
else
	bad "node1 gre6 not established"
fi
if [[ "$(state_of "$R4")" == up ]]; then
	ok "node2 gre6 established (ifindex $(idx_of "$R4"))"
else
	bad "node2 gre6 not established"
fi
[[ "$(iface_kind "$NODE1" gre6)" == ip6gre ]] && ok "node1 gre6 is an ip6gre device" || bad "node1 gre6 kind wrong"
[[ "$(iface_kind "$NODE2" gre6)" == ip6gre ]] && ok "node2 gre6 is an ip6gre device" || bad "node2 gre6 kind wrong"

ex1 "ip addr add fd00:200::1/64 dev gre6 2>/dev/null || true; ip link set gre6 up"
ex2 "ip addr add fd00:200::2/64 dev gre6 2>/dev/null || true; ip link set gre6 up"
sleep 2
if ping6_ok "$NODE1" fd00:200::2; then ok "IPv6 connectivity node1 -> node2 over gre6"
				     else bad "IPv6 connectivity over gre6 FAILED"; fi
if ping6_ok "$NODE2" fd00:200::1; then ok "IPv6 connectivity node2 -> node1 over gre6"
				     else bad "IPv6 connectivity (reverse) over gre6 FAILED"; fi

hdr "D. establishment-status API (idempotent re-add)"
R5=$(link1 setup --sock $SOCK --name gre1 --local $U1 --remote $U2 --mtu 1400 || true)
echo "$R5"
if [[ "$(state_of "$R5")" == up && "$(idx_of "$R5")" == "$(idx_of "$R1")" ]]; then
	ok "node1 gre1 re-query state=up ifindex=$(idx_of "$R5")"
else
	bad "node1 gre1 re-query inconsistent (state $(state_of "$R5"))"
fi

hdr "E. Teardown"
T1=$(link1 teardown --sock $SOCK --name gre1 || true); echo "$T1"
T2=$(link2 teardown --sock $SOCK --name gre1 || true); echo "$T2"
T3=$(link1 teardown --sock $SOCK --name gre6 || true); echo "$T3"
T4=$(link2 teardown --sock $SOCK --name gre6 || true); echo "$T4"
sleep 2
if iface_exists "$NODE1" gre1; then bad "node1 gre1 still present"; else ok "node1 gre1 removed"; fi
if iface_exists "$NODE2" gre1; then bad "node2 gre1 still present"; else ok "node2 gre1 removed"; fi
if iface_exists "$NODE1" gre6; then bad "node1 gre6 still present"; else ok "node1 gre6 removed"; fi
if iface_exists "$NODE2" gre6; then bad "node2 gre6 still present"; else ok "node2 gre6 removed"; fi

echo
echo "=== summary: PASS=$PASS FAIL=$FAIL ==="
[[ "$FAIL" -eq 0 ]]
