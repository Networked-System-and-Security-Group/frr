#!/usr/bin/env bash
#
# Stage E - MIDR virtual link across a *BGP-only* underlay (third-group
# acceptance for delivery item 4: prove the virtual link is carried over GRE
# while the outer endpoint is reachable only through BGP).
#
#   midra (IP_A, AS65001) -- net-a -- midrrtr (IP_RA|IP_RB, AS65000)
#   midrb (IP_B, AS65002) -- net-b
#
# net-a and net-b are two distinct docker bridges, so 10.20.1.0/24 and
# 10.20.2.0/24 are NOT the same connected segment: midra can only learn
# 10.20.2.0/24 from its BGP session with midrrtr.  The script then asserts the
# two ends reach READY through the virtual-link API, that the returned ifindex
# is the kernel's, that the overlay really gets used (route get / FIB), and
# that teardown, rebuild, a zebra restart and an externally deleted kernel
# device are all recovered (E1-E7).
#
# WHERE TO RUN: on the docker HOST as root, NOT inside the build container --
# the container has no docker CLI.  The script creates its own containers
# ($A/$B/$R) and bridges ($NA/$NB) and pulls zebra/bgpd/libfrr and
# midrd-gre-tool out of $BUILD.  It does not delete them at the end; the
# summary prints the cleanup command.
#
#   echo <sudo-pw> | sudo -S bash r7-dp-stagee-bgp-underlay.sh
#   MIDRD_GRE_TOOL=<path to a freshly built midrd-gre-tool> sudo -E bash r7-dp-stagee-bgp-underlay.sh
#
# Never touches frr-dut / tg-sender / tg-receiver / node1 / node2 / /opt/midr.
set -uo pipefail

BUILD=${MIDRD_BUILD_CONTAINER:-frr-ubuntu24-ymy}
IMAGE=${MIDRD_STAGEE_IMAGE:-frr-ubuntu24-ymy:init}
A=${MIDRD_STAGEE_NODE_A:-midra}; B=${MIDRD_STAGEE_NODE_B:-midrb}; R=${MIDRD_STAGEE_NODE_R:-midrrtr}
NA=${MIDRD_STAGEE_NET_A:-midr-stagee-a}
NB=${MIDRD_STAGEE_NET_B:-midr-stagee-b}
IPA=10.20.1.1; IPB=10.20.2.1
IPRA=10.20.1.254; IPRB=10.20.2.254
GWA=10.20.1.250; GWB=10.20.2.250
SOCK=/tmp/zserv_midr.api
STAGE=${MIDRD_STAGEE_STAGE:-/opt/midr-stagee}
SRC=${MIDRD_SRC_TREE:-/home/frr/frr-midrd3}
BUILDTOOL=${MIDRD_GRE_TOOL:-/tmp/midrd-build/midrd-gre-tool}
OVL_A=192.168.100.1
OVL_B=192.168.100.2
OVL_PLEN=30
DEV=gre1
WORK=$(mktemp -d "${TMPDIR:-/tmp}/stageE-conf.XXXXXX")

command -v docker >/dev/null 2>&1 || { echo "docker is required (run on the docker host as root)" >&2; exit 2; }
docker inspect "$BUILD" >/dev/null 2>&1 || { echo "missing build container: $BUILD" >&2; exit 2; }

PASS=0; FAIL=0
ok()  { echo "[PASS] $*"; PASS=$((PASS+1)); }
bad() { echo "[FAIL] $*"; FAIL=$((FAIL+1)); }
hdr() { echo; echo "==== $* ===="; }
ex()  { docker exec -u root "$1" bash -c "$2"; }
exq() { docker exec -u root "$1" bash -c "$2" 2>/dev/null || true; }
tool(){ docker exec -u root "$1" bash -c "LD_LIBRARY_PATH=$STAGE/lib $STAGE/midrd-gre-tool $2" 2>&1; }
vs()  { docker exec -u root "$1" /usr/bin/vtysh --vty_socket /tmp -d "$2" -c "$3" 2>&1; }

destroy() {
	local c
	for c in "$A" "$B" "$R"; do
		docker rm -f "$c" >/dev/null 2>&1 || true
	done
	docker network rm "$NA" "$NB" >/dev/null 2>&1 || true
}

echo "=== Stage E: MIDR virtual link over a BGP-only underlay ==="
echo "date=$(date -Is)  build_container=$BUILD  image=$IMAGE"
echo "topology: $A($IPA) -[$NA]- $R($IPRA|$IPRB) -[$NB]- $B($IPB)"

destroy
docker network create --driver bridge --subnet 10.20.1.0/24 --gateway "$GWA" "$NA" >/dev/null || { echo "network create $NA failed" >&2; exit 2; }
docker network create --driver bridge --subnet 10.20.2.0/24 --gateway "$GWB" "$NB" >/dev/null || { echo "network create $NB failed" >&2; exit 2; }
ok "two independent bridges created ($NA=10.20.1.0/24, $NB=10.20.2.0/24)"

docker run -d --name "$A" --hostname "$A" --privileged --network "$NA" --ip "$IPA" "$IMAGE" >/dev/null
docker run -d --name "$B" --hostname "$B" --privileged --network "$NB" --ip "$IPB" "$IMAGE" >/dev/null
docker run -d --name "$R" --hostname "$R" --privileged --network "$NA" --ip "$IPRA" "$IMAGE" >/dev/null
docker network connect --ip "$IPRB" "$NB" "$R" >/dev/null
sleep 3
ok "containers created: $A / $B / $R"

REALLIB=$(docker exec "$BUILD" readlink -f "$SRC/lib/.libs/libfrr.so.0")
[[ -n "$REALLIB" ]] || { echo "cannot locate libfrr.so.0 in $BUILD" >&2; exit 2; }
# docker cp cannot copy container -> container: stage through the host.
cpin() { # cpin <build-path> <target-container> <target-path>
	docker cp "$BUILD:$1" "$WORK/$(basename "$1")" >/dev/null || return 1
	docker cp "$WORK/$(basename "$1")" "$2:$3" >/dev/null || return 1
}
for c in "$A" "$B" "$R"; do
	ex "$c" "mkdir -p $STAGE/lib /etc/frr; : > /etc/frr/vtysh.conf; : > /etc/frr/frr.conf"
	cpin "$SRC/zebra/.libs/zebra" "$c" "$STAGE/zebra" || bad "stage zebra -> $c"
	cpin "$SRC/bgpd/.libs/bgpd" "$c" "$STAGE/bgpd" || bad "stage bgpd -> $c"
	cpin "$REALLIB" "$c" "$STAGE/lib/libfrr.so.0.0.0" || bad "stage libfrr -> $c"
	ex "$c" "chmod 0755 $STAGE/zebra $STAGE/bgpd; cd $STAGE/lib && ln -sf libfrr.so.0.0.0 libfrr.so.0 && ln -sf libfrr.so.0 libfrr.so"
done
for c in "$A" "$B"; do
	cpin "$BUILDTOOL" "$c" "$STAGE/midrd-gre-tool" || bad "stage midrd-gre-tool -> $c"
	ex "$c" "chmod 0755 $STAGE/midrd-gre-tool"
done
ZEBRA_MD5=$(docker exec "$BUILD" md5sum "$SRC/zebra/.libs/zebra" | awk '{print $1}')
STAGED_MD5=$(docker exec "$A" md5sum "$STAGE/zebra" | awk '{print $1}')
BGPD_MD5=$(docker exec "$BUILD" md5sum "$SRC/bgpd/.libs/bgpd" | awk '{print $1}')
STAGED_BGPD=$(docker exec "$A" md5sum "$STAGE/bgpd" | awk '{print $1}')
if [[ "$ZEBRA_MD5" == "$STAGED_MD5" && "$BGPD_MD5" == "$STAGED_BGPD" ]]; then
	ok "fresh zebra+bgpd staged and md5-verified (zebra=$ZEBRA_MD5 bgpd=$BGPD_MD5)"
else
	bad "staged artifact md5 mismatch (zebra $ZEBRA_MD5/$STAGED_MD5 bgpd $BGPD_MD5/$STAGED_BGPD)"
fi

for c in "$A" "$B" "$R"; do
	cat > "$WORK/zebra-$c.conf" <<EOF
hostname $c
log file /tmp/zebra.log
EOF
done
cat > "$WORK/bgpd-$A.conf" <<EOF
hostname $A
log file /tmp/bgpd.log
route-map RM-EBGP permit 10
!
router bgp 65001
 bgp router-id $IPA
 no bgp ebgp-requires-policy
 neighbor $IPRA remote-as 65000
 neighbor $IPRA route-map RM-EBGP in
 neighbor $IPRA route-map RM-EBGP out
!
EOF
cat > "$WORK/bgpd-$B.conf" <<EOF
hostname $B
log file /tmp/bgpd.log
route-map RM-EBGP permit 10
!
router bgp 65002
 bgp router-id $IPB
 no bgp ebgp-requires-policy
 neighbor $IPRB remote-as 65000
 neighbor $IPRB route-map RM-EBGP in
 neighbor $IPRB route-map RM-EBGP out
!
EOF
cat > "$WORK/bgpd-$R.conf" <<EOF
hostname $R
log file /tmp/bgpd.log
route-map RM-EBGP permit 10
!
router bgp 65000
 bgp router-id 10.20.0.254
 no bgp ebgp-requires-policy
 network 10.20.1.0/24
 network 10.20.2.0/24
 neighbor $IPA remote-as 65001
 neighbor $IPA route-map RM-EBGP in
 neighbor $IPA route-map RM-EBGP out
 neighbor $IPB remote-as 65002
 neighbor $IPB route-map RM-EBGP in
 neighbor $IPB route-map RM-EBGP out
!
EOF
for c in "$A" "$B" "$R"; do
	docker cp "$WORK/zebra-$c.conf" "$c:/tmp/zebra.conf" >/dev/null
	docker cp "$WORK/bgpd-$c.conf" "$c:/tmp/bgpd.conf" >/dev/null
done

start_frr() {
	local c=$1
	ex "$c" "pkill -9 zebra 2>/dev/null; pkill -9 bgpd 2>/dev/null; sleep 1;
		usermod -aG frrvty root 2>/dev/null || true
		sysctl -w net.ipv4.ip_forward=1 >/dev/null 2>&1 || true
		sysctl -w net.ipv4.conf.all.rp_filter=0 >/dev/null 2>&1 || true
		sysctl -w net.ipv4.conf.default.rp_filter=0 >/dev/null 2>&1 || true
		LD_LIBRARY_PATH=$STAGE/lib $STAGE/zebra -u root -g root -f /tmp/zebra.conf -i /tmp/zebra.pid -z $SOCK --vty_socket /tmp -d
		LD_LIBRARY_PATH=$STAGE/lib $STAGE/bgpd  -u root -g root -f /tmp/bgpd.conf  -i /tmp/bgpd.pid  -z $SOCK --vty_socket /tmp -d"
}
hdr "0. start zebra+bgpd on all three nodes"
for c in "$R" "$A" "$B"; do start_frr "$c"; done
sleep 6
for c in "$A" "$B" "$R"; do
	if ex "$c" "test -S $SOCK"; then ok "zebra socket present on $c"; else bad "zebra socket missing on $c"; ex "$c" "tail -5 /tmp/zebra.log 2>/dev/null; true"; fi
done

for c in "$A" "$B" "$R"; do ex "$c" "ip route del default 2>/dev/null || true"; done

hdr "E1. outer reachability must come from BGP (not a connected route)"
sleep 8
R_MAIN=$(exq "$A" "ip route show")
echo "--- $A ip route show ---"
echo "$R_MAIN"
echo "--- $A ip route get $IPB ---"
RG=$(exq "$A" "ip route get $IPB")
echo "$RG"

if echo "$R_MAIN" | grep -qE "^10\.20\.2\.0/24 .*via $IPRA .*proto bgp"; then
	ok "E1.1 $A has 10.20.2.0/24 via $IPRA proto bgp"
else
	bad "E1.1 $A lacks a BGP route to 10.20.2.0/24 via $IPRA"
fi
if echo "$R_MAIN" | grep -qE "^10\.20\.2\.0/24 dev "; then
	bad "E1.2 10.20.2.0/24 is a connected route on $A (would be faking reachability)"
else
	ok "E1.2 no connected route to 10.20.2.0/24 on $A"
fi
if echo "$RG" | grep -qE "10\.20\.2\.1 via $IPRA .*dev eth0"; then
	ok "E1.3 ip route get $IPB on $A resolves via $IPRA (BGP nexthop)"
else
	bad "E1.3 ip route get $IPB on $A did not use the BGP nexthop: $RG"
fi
if ex "$A" "ping -c 3 -W 2 $IPB" >/dev/null 2>&1; then
	ok "E1.4 $A -> $IPB ping works through the BGP-only underlay"
else
	bad "E1.4 $A -> $IPB ping failed"
fi
R_MAIN_B=$(exq "$B" "ip route show")
if echo "$R_MAIN_B" | grep -qE "^10\.20\.1\.0/24 .*via $IPRB .*proto bgp"; then
	ok "E1.5 $B has 10.20.1.0/24 via $IPRB proto bgp"
else
	bad "E1.5 $B lacks a BGP route to 10.20.1.0/24 via $IPRB"
fi
echo "--- $R bgp summary (vtysh) ---"
vs "$R" bgpd "show bgp summary" | head -20

hdr "E2. create the virtual link through the new API (both ends)"
RES_A=$(tool "$A" "vlink-setup --sock $SOCK --name $DEV --local $IPA --remote $IPB --mtu 1400 --wait-ms 8000 --overlay-local $OVL_A --overlay-remote $OVL_B --overlay-prefix $OVL_PLEN"); echo "$RES_A"
RES_B=$(tool "$B" "vlink-setup --sock $SOCK --name $DEV --local $IPB --remote $IPA --mtu 1400 --wait-ms 8000 --overlay-local $OVL_B --overlay-remote $OVL_A --overlay-prefix $OVL_PLEN"); echo "$RES_B"
vst()  { echo "$1" | grep -E '^MIDR_VLINK ' | grep -oE 'state=[a-z_]+' | head -1 | cut -d= -f2; }
vidx() { echo "$1" | grep -E '^MIDR_VLINK ' | grep -oE 'ifindex=[0-9]+' | head -1 | cut -d= -f2; }
KIDX_A=$(exq "$A" "cat /sys/class/net/$DEV/ifindex")
KIDX_B=$(exq "$B" "cat /sys/class/net/$DEV/ifindex")
if [[ "$(vst "$RES_A")" == ready ]]; then ok "E2.1 $A state=READY via API (ifindex=$(vidx "$RES_A"))"; else bad "E2.1 $A not READY ($(vst "$RES_A"))"; fi
if [[ "$(vst "$RES_B")" == ready ]]; then ok "E2.2 $B state=READY via API (ifindex=$(vidx "$RES_B"))"; else bad "E2.2 $B not READY ($(vst "$RES_B"))"; fi
if [[ -n "$(vidx "$RES_A")" && "$(vidx "$RES_A")" == "$KIDX_A" ]]; then ok "E2.3 $A API ifindex $(vidx "$RES_A") == kernel $KIDX_A"; else bad "E2.3 $A ifindex mismatch (api=$(vidx "$RES_A") kernel=$KIDX_A)"; fi
if [[ -n "$(vidx "$RES_B")" && "$(vidx "$RES_B")" == "$KIDX_B" ]]; then ok "E2.4 $B API ifindex $(vidx "$RES_B") == kernel $KIDX_B"; else bad "E2.4 $B ifindex mismatch (api=$(vidx "$RES_B") kernel=$KIDX_B)"; fi

hdr "E3. overlay address + interface state"
echo "--- $A: ip -d link show $DEV ---"; exq "$A" "ip -d link show $DEV"
echo "--- $A: ip -o addr show dev $DEV ---"; exq "$A" "ip -o addr show dev $DEV"
echo "--- $B: ip -o addr show dev $DEV ---"; exq "$B" "ip -o addr show dev $DEV"
for n in "$A" "$B"; do
	if ex "$n" "ip -o addr show dev $DEV | grep -q 'inet '"; then ok "E3.1 $n has an IPv4 overlay address on $DEV"; else bad "E3.1 $n missing overlay address"; fi
	if ex "$n" "ip link show $DEV | grep -q 'UP'"; then ok "E3.2 $n $DEV is UP"; else bad "E3.2 $n $DEV is not UP"; fi
done
if ex "$A" "ip -o addr show dev $DEV | grep -qE 'inet $OVL_A/$OVL_PLEN'"; then ok "E3.3 $A carries exactly $OVL_A/$OVL_PLEN"; else bad "E3.3 $A overlay address is not $OVL_A/$OVL_PLEN"; fi
if ex "$B" "ip -o addr show dev $DEV | grep -qE 'inet $OVL_B/$OVL_PLEN'"; then ok "E3.4 $B carries exactly $OVL_B/$OVL_PLEN"; else bad "E3.4 $B overlay address is not $OVL_B/$OVL_PLEN"; fi

hdr "E4. overlay reachability (bidirectional ping)"
if ex "$A" "ping -c 3 -W 2 $OVL_B" >/dev/null 2>&1; then ok "E4.1 $A -> $OVL_B over $DEV"; else bad "E4.1 $A -> $OVL_B failed"; fi
if ex "$B" "ping -c 3 -W 2 $OVL_A" >/dev/null 2>&1; then ok "E4.2 $B -> $OVL_A over $DEV"; else bad "E4.2 $B -> $OVL_A failed"; fi

hdr "E5. ip route get <overlay_remote> must hit the virtual interface"
RG5=$(exq "$A" "ip route get $OVL_B")
echo "--- $A ip route get $OVL_B ---"; echo "$RG5"
if echo "$RG5" | grep -qE "dev $DEV"; then ok "E5.1 $A route get $OVL_B hits dev $DEV"; else bad "E5.1 $A route get $OVL_B does not hit $DEV"; fi

hdr "E6. returned ifindex usable by the FIB / other groups"
IDX=$(vidx "$RES_A")
echo "--- $A: ip -j link show $DEV ---"; exq "$A" "ip -j link show $DEV"
if ex "$A" "ip route add 198.51.100.0/24 via $OVL_B dev $DEV" >/dev/null 2>&1; then
	ok "E6.1 $A installed a FIB route keyed by $DEV (ifindex $IDX)"
else
	bad "E6.1 $A could not install a route via $DEV"
fi
FT=$(exq "$A" "ip route show 198.51.100.0/24")
echo "--- $A ip route show 198.51.100.0/24 ---"; echo "$FT"
if echo "$FT" | grep -qE "via $OVL_B dev $DEV"; then ok "E6.2 FIB entry present (via $OVL_B dev $DEV)"; else bad "E6.2 FIB entry missing"; fi
FJ=$(exq "$A" "ip -j route get 198.51.100.1")
echo "--- $A ip -j route get 198.51.100.1 ---"; echo "$FJ"
if echo "$FJ" | grep -q "\"dev\":\"$DEV\""; then ok "E6.3 dataplane lookup for 198.51.100.1 resolves to $DEV"; else bad "E6.3 dataplane lookup did not resolve to $DEV"; fi
ex "$A" "ip route del 198.51.100.0/24" >/dev/null 2>&1

hdr "E7. teardown / zebra restart / rebuild recovery"
tool "$A" "teardown --sock $SOCK --name $DEV" >/dev/null 2>&1
tool "$B" "teardown --sock $SOCK --name $DEV" >/dev/null 2>&1
sleep 2
for n in "$A" "$B"; do
	if ex "$n" "ip link show $DEV >/dev/null 2>&1"; then bad "E7.1 $n $DEV still present after teardown"; else ok "E7.1 $n $DEV removed by teardown"; fi
done

RES_A2=$(tool "$A" "vlink-setup --sock $SOCK --name $DEV --local $IPA --remote $IPB --mtu 1400 --wait-ms 8000 --overlay-local $OVL_A --overlay-remote $OVL_B --overlay-prefix $OVL_PLEN"); echo "$RES_A2"
RES_B2=$(tool "$B" "vlink-setup --sock $SOCK --name $DEV --local $IPB --remote $IPA --mtu 1400 --wait-ms 8000 --overlay-local $OVL_B --overlay-remote $OVL_A --overlay-prefix $OVL_PLEN"); echo "$RES_B2"
if [[ "$(vst "$RES_A2")" == ready && "$(vst "$RES_B2")" == ready ]]; then ok "E7.2 rebuild after teardown -> READY on both ends (A=$(vidx "$RES_A2") B=$(vidx "$RES_B2"))"; else bad "E7.2 rebuild failed (A=$(vst "$RES_A2") B=$(vst "$RES_B2"))"; fi
if ex "$A" "ping -c 3 -W 2 $OVL_B" >/dev/null 2>&1; then ok "E7.3 overlay ping again after rebuild"; else bad "E7.3 overlay ping failed after rebuild"; fi

ex "$A" "pkill -9 zebra; sleep 2; true"
start_frr "$A"
sleep 6
RES_A3=$(tool "$A" "vlink-setup --sock $SOCK --name $DEV --local $IPA --remote $IPB --mtu 1400 --wait-ms 8000 --overlay-local $OVL_A --overlay-remote $OVL_B --overlay-prefix $OVL_PLEN"); echo "$RES_A3"
KA3=$(exq "$A" "cat /sys/class/net/$DEV/ifindex")
if [[ "$(vst "$RES_A3")" == ready && "$(vidx "$RES_A3")" == "$KA3" ]]; then ok "E7.4 after zebra restart re-establish -> READY, ifindex $(vidx "$RES_A3") == kernel $KA3"; else bad "E7.4 after zebra restart re-establish failed (state=$(vst "$RES_A3") api=$(vidx "$RES_A3") kernel=$KA3)"; fi
if ex "$A" "ping -c 3 -W 2 $OVL_B" >/dev/null 2>&1; then ok "E7.5 overlay ping after zebra restart"; else bad "E7.5 overlay ping failed after zebra restart"; fi

ex "$A" "ip link del $DEV" >/dev/null 2>&1
sleep 1
RES_A4=$(tool "$A" "vlink-setup --sock $SOCK --name $DEV --local $IPA --remote $IPB --mtu 1400 --wait-ms 8000 --overlay-local $OVL_A --overlay-remote $OVL_B --overlay-prefix $OVL_PLEN"); echo "$RES_A4"
KA4=$(exq "$A" "cat /sys/class/net/$DEV/ifindex")
if [[ "$(vst "$RES_A4")" == ready && -n "$KA4" && "$(vidx "$RES_A4")" == "$KA4" ]]; then ok "E7.6 kernel device deleted -> recreated, READY, ifindex $(vidx "$RES_A4")"; else bad "E7.6 device-vanished rebuild failed (state=$(vst "$RES_A4") api=$(vidx "$RES_A4") kernel=$KA4)"; fi

echo
echo "=== stage E summary: PASS=$PASS FAIL=$FAIL ==="
echo "workdir=$WORK"
echo "cleanup: docker rm -f $A $B $R; docker network rm $NA $NB"
exit $([[ "$FAIL" -eq 0 ]] && echo 0 || echo 1)

