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
# Second underlay pair used by case B.  The GRE layer keys a tunnel by its
# outer endpoint pair as well as by its device name, so a second IPv4 GRE
# tunnel may not reuse the same pair while the first one still exists (the
# virtual-link API rejects that silent rename with EEXIST).
U1B=10.1.1.21
U2B=10.1.1.22
U6_1=fd00:1::11
U6_2=fd00:1::12
# Overlay addressing handed to the virtual-link API (no more manual
# `ip addr add`):  A) IPv4 overlay over IPv4 GRE, B) IPv6 overlay over an
# IPv4 GRE with its own underlay pair (U1B/U2B), C) IPv6 overlay over an
# IPv6 GRE (ip6gre) device.
OV4_1=192.168.100.1
OV4_2=192.168.100.2
OV4_PLEN=30
OV6_1=fd00:100::1
OV6_2=fd00:100::2
OV6_PLEN=64
OV6B_1=fd00:200::1
OV6B_2=fd00:200::2
SOCK=/tmp/zserv_midr.api
LD=/opt/midr-dp/lib
LINK=/opt/midr-dp/midrd-gre-tool
ZEBRA=/opt/midr-dp/zebra
BUILD_TOOL=${MIDRD_GRE_TOOL:-}
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

# Resolve the gre-link-tool inside the build container.  The historical
# harness path is preferred only when it exists; the canonical component
# build dir (/tmp/midrd-build, i.e. the gate-2 BUILD_DIR) is tried first so
# the test exercises the freshly built sources rather than a stale harness
# copy.  Override with MIDRD_GRE_TOOL.
if [[ -z $BUILD_TOOL ]]; then
	for cand in /tmp/midrd-build/midrd-gre-tool /tmp/midrd-build-harness/midrd-gre-tool; do
		if docker exec "$BUILD" test -x "$cand" 2>/dev/null; then
			BUILD_TOOL=$cand
			break
		fi
	done
fi
[[ -n $BUILD_TOOL ]] || {
	echo "no gre-link-tool found in $BUILD (set MIDRD_GRE_TOOL)" >&2
	exit 2
}
docker exec "$BUILD" test -x "$BUILD_TOOL" || {
	echo "gre-link-tool not executable in $BUILD: $BUILD_TOOL" >&2
	exit 2
}
echo "using gre-link-tool: $BUILD_TOOL"

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

# --- virtual-link API helpers (third group) ---------------------------------

# vlink1/vlink2 <name> <outer-local> <outer-remote> <ov-local> <ov-remote> <plen>
vlink1() {
	link1 vlink-setup --sock "$SOCK" --name "$1" --local "$2" --remote "$3" \
		--mtu 1400 --wait-ms 8000 \
		--overlay-local "$4" --overlay-remote "$5" --overlay-prefix "$6"
}
vlink2() {
	link2 vlink-setup --sock "$SOCK" --name "$1" --local "$2" --remote "$3" \
		--mtu 1400 --wait-ms 8000 \
		--overlay-local "$4" --overlay-remote "$5" --overlay-prefix "$6"
}

vlink_state_of()   { echo "$1" | grep -E '^MIDR_VLINK ' | grep -oE 'state=[a-z_]+' | head -1 | cut -d= -f2; }
vlink_idx_of()     { echo "$1" | grep -E '^MIDR_VLINK ' | grep -oE 'ifindex=[0-9]+' | head -1 | cut -d= -f2; }
vlink_iftype_of()  { echo "$1" | grep -E '^MIDR_VLINK ' | grep -oE 'iftype=[0-9]+' | head -1 | cut -d= -f2; }

# The READY transition notification is the only line that carries event=ready:
# the final MIDR_VLINK status line is a pure query and reports event=none.
# `|| true` keeps `set -e`/pipefail from aborting when no READY line exists,
# so the caller can report a [FAIL] instead of dying.
vlink_ready_line_of() { echo "$1" | grep -E 'state=ready .*event=ready' | head -1 || true; }
vlink_ready_field_of() { vlink_ready_line_of "$1" | grep -oE "$2=[^ ]+" | head -1 | cut -d= -f2 || true; }

# assert_vlink_overlay <label> <status-line> <ov-local> <ov-remote> <plen>
# Asserts the READY notification reports the configured overlay triple plus
# event=ready, i.e. the group-1 -> group-2 handoff data is visible.
assert_vlink_overlay() {
	local label=$1 out=$2 ovl=$3 ovr=$4 plen=$5 line
	line=$(vlink_ready_line_of "$out")
	if [[ -n "$line" \
	   && "$(vlink_ready_field_of "$out" overlay_local)" == "$ovl" \
	   && "$(vlink_ready_field_of "$out" overlay_remote)" == "$ovr" \
	   && "$(vlink_ready_field_of "$out" overlay_plen)" == "$plen" \
	   && "$(vlink_ready_field_of "$out" event)" == ready ]]; then
		ok "$label READY overlay_local=$ovl overlay_remote=$ovr plen=$plen event=ready"
	else
		bad "$label READY overlay/event mismatch (${line:-no ready event line})"
	fi
}

# assert_vlink_ready <label> <status-line> <expected-kernel-ifindex>
assert_vlink_ready() {
	local label=$1 out=$2 kidx=$3 vidx
	vidx=$(vlink_idx_of "$out")
	if [[ "$(vlink_state_of "$out")" == ready && -n "$vidx" && "$vidx" -gt 0 ]]; then
		ok "$label state=READY via API (ifindex=$vidx)"
	else
		bad "$label not READY via API ($(vlink_state_of "$out"))"
	fi
	if [[ -n "$vidx" && "$vidx" == "$kidx" && "$kidx" -gt 0 ]]; then
		ok "$label returned ifindex $vidx matches kernel $kidx"
	else
		bad "$label ifindex mismatch (api=${vidx:-none} kernel=${kidx:-none})"
	fi
}

# iface has an address prefix (either family) configured on it?
iface_has_addr() {
	docker exec -u root "$1" bash -c "ip -o addr show dev '$2' 2>/dev/null | grep -q '$3'" 2>/dev/null
}


cleanup() {
	if docker exec -u root "$NODE1" bash -c "test -S $SOCK" 2>/dev/null; then
		link1 teardown --sock $SOCK --name gre1 >/dev/null 2>&1 || true
		link1 teardown --sock $SOCK --name gre2 >/dev/null 2>&1 || true
		link1 teardown --sock $SOCK --name gre6 >/dev/null 2>&1 || true
	fi
	if docker exec -u root "$NODE2" bash -c "test -S $SOCK" 2>/dev/null; then
		link2 teardown --sock $SOCK --name gre1 >/dev/null 2>&1 || true
		link2 teardown --sock $SOCK --name gre2 >/dev/null 2>&1 || true
		link2 teardown --sock $SOCK --name gre6 >/dev/null 2>&1 || true
	fi
	ex1 "ip link del gre1 2>/dev/null; ip link del gre2 2>/dev/null; ip link del gre6 2>/dev/null; true"
	ex2 "ip link del gre1 2>/dev/null; ip link del gre2 2>/dev/null; ip link del gre6 2>/dev/null; true"
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

# Extra IPv4 alias pair for case B's second GRE tunnel (see U1B/U2B).
ex1 "ip addr add $U1B/24 dev eth0 2>/dev/null || true"
ex2 "ip addr add $U2B/24 dev eth0 2>/dev/null || true"
if ping4_ok "$NODE1" "$U2B"; then
	ok "underlay IPv4 $U1B -> $U2B"
else
	bad "underlay IPv4 (case B pair) unreachable"
fi

hdr "A. IPv4 GRE tunnel (gre) + IPv4 overlay via virtual-link API"
R1=$(vlink1 gre1 "$U1" "$U2" "$OV4_1" "$OV4_2" "$OV4_PLEN" || true); echo "$R1"
R2=$(vlink2 gre1 "$U2" "$U1" "$OV4_2" "$OV4_1" "$OV4_PLEN" || true); echo "$R2"
K1=$(ex1 "cat /sys/class/net/gre1/ifindex" 2>/dev/null || true)
K2=$(ex2 "cat /sys/class/net/gre1/ifindex" 2>/dev/null || true)
assert_vlink_ready "node1 gre1" "$R1" "$K1"
assert_vlink_ready "node2 gre1" "$R2" "$K2"
assert_vlink_overlay "node1 gre1" "$R1" "$OV4_1" "$OV4_2" "$OV4_PLEN"
assert_vlink_overlay "node2 gre1" "$R2" "$OV4_2" "$OV4_1" "$OV4_PLEN"
[[ "$(iface_kind "$NODE1" gre1)" == gre ]] && ok "node1 gre1 is a gre device" || bad "node1 gre1 kind wrong"
[[ "$(iface_kind "$NODE2" gre1)" == gre ]] && ok "node2 gre1 is a gre device" || bad "node2 gre1 kind wrong"
if iface_has_addr "$NODE1" gre1 "$OV4_1/$OV4_PLEN"; then ok "node1 gre1 overlay $OV4_1/$OV4_PLEN configured"
						 else bad "node1 gre1 overlay address missing"; fi
if iface_has_addr "$NODE2" gre1 "$OV4_2/$OV4_PLEN"; then ok "node2 gre1 overlay $OV4_2/$OV4_PLEN configured"
						 else bad "node2 gre1 overlay address missing"; fi
sleep 2
if ping4_ok "$NODE1" "$OV4_2"; then ok "IPv4 connectivity node1 -> node2 over gre1"
			      else bad "IPv4 connectivity over gre1 FAILED"; fi
if ping4_ok "$NODE2" "$OV4_1"; then ok "IPv4 connectivity node2 -> node1 over gre1"
			      else bad "IPv4 connectivity (reverse) over gre1 FAILED"; fi

hdr "B. IPv6 overlay over the IPv4 GRE tunnel (gre) via virtual-link API"
R3=$(vlink1 gre2 "$U1B" "$U2B" "$OV6_1" "$OV6_2" "$OV6_PLEN" || true); echo "$R3"
R4=$(vlink2 gre2 "$U2B" "$U1B" "$OV6_2" "$OV6_1" "$OV6_PLEN" || true); echo "$R4"
KB1=$(ex1 "cat /sys/class/net/gre2/ifindex" 2>/dev/null || true)
KB2=$(ex2 "cat /sys/class/net/gre2/ifindex" 2>/dev/null || true)
assert_vlink_ready "node1 gre2" "$R3" "$KB1"
assert_vlink_ready "node2 gre2" "$R4" "$KB2"
assert_vlink_overlay "node1 gre2" "$R3" "$OV6_1" "$OV6_2" "$OV6_PLEN"
assert_vlink_overlay "node2 gre2" "$R4" "$OV6_2" "$OV6_1" "$OV6_PLEN"
[[ "$(iface_kind "$NODE1" gre2)" == gre ]] && ok "node1 gre2 is a gre device" || bad "node1 gre2 kind wrong"
[[ "$(iface_kind "$NODE2" gre2)" == gre ]] && ok "node2 gre2 is a gre device" || bad "node2 gre2 kind wrong"
if iface_has_addr "$NODE1" gre2 "$OV6_1/$OV6_PLEN"; then ok "node1 gre2 overlay $OV6_1/$OV6_PLEN configured"
						 else bad "node1 gre2 overlay address missing"; fi
if iface_has_addr "$NODE2" gre2 "$OV6_2/$OV6_PLEN"; then ok "node2 gre2 overlay $OV6_2/$OV6_PLEN configured"
						 else bad "node2 gre2 overlay address missing"; fi
sleep 2
if ping6_ok "$NODE1" "$OV6_2"; then ok "IPv6 connectivity node1 -> node2 over gre2"
			       else bad "IPv6 connectivity over gre2 FAILED"; fi
if ping6_ok "$NODE2" "$OV6_1"; then ok "IPv6 connectivity node2 -> node1 over gre2"
			       else bad "IPv6 connectivity (reverse) over gre2 FAILED"; fi

hdr "C. IPv6 GRE tunnel (ip6gre) + IPv6 overlay via virtual-link API"
R5=$(vlink1 gre6 "$U6_1" "$U6_2" "$OV6B_1" "$OV6B_2" "$OV6_PLEN" || true); echo "$R5"
R6=$(vlink2 gre6 "$U6_2" "$U6_1" "$OV6B_2" "$OV6B_1" "$OV6_PLEN" || true); echo "$R6"
KC1=$(ex1 "cat /sys/class/net/gre6/ifindex" 2>/dev/null || true)
KC2=$(ex2 "cat /sys/class/net/gre6/ifindex" 2>/dev/null || true)
assert_vlink_ready "node1 gre6" "$R5" "$KC1"
assert_vlink_ready "node2 gre6" "$R6" "$KC2"
assert_vlink_overlay "node1 gre6" "$R5" "$OV6B_1" "$OV6B_2" "$OV6_PLEN"
assert_vlink_overlay "node2 gre6" "$R6" "$OV6B_2" "$OV6B_1" "$OV6_PLEN"
[[ "$(iface_kind "$NODE1" gre6)" == ip6gre ]] && ok "node1 gre6 is an ip6gre device" || bad "node1 gre6 kind wrong"
[[ "$(iface_kind "$NODE2" gre6)" == ip6gre ]] && ok "node2 gre6 is an ip6gre device" || bad "node2 gre6 kind wrong"
if iface_has_addr "$NODE1" gre6 "$OV6B_1/$OV6_PLEN"; then ok "node1 gre6 overlay $OV6B_1/$OV6_PLEN configured"
						  else bad "node1 gre6 overlay address missing"; fi
if iface_has_addr "$NODE2" gre6 "$OV6B_2/$OV6_PLEN"; then ok "node2 gre6 overlay $OV6B_2/$OV6_PLEN configured"
						  else bad "node2 gre6 overlay address missing"; fi
sleep 2
if ping6_ok "$NODE1" "$OV6B_2"; then ok "IPv6 connectivity node1 -> node2 over gre6"
				else bad "IPv6 connectivity over gre6 FAILED"; fi
if ping6_ok "$NODE2" "$OV6B_1"; then ok "IPv6 connectivity node2 -> node1 over gre6"
				else bad "IPv6 connectivity (reverse) over gre6 FAILED"; fi

hdr "D. establishment-status API (idempotent re-add)"
R7=$(vlink1 gre1 "$U1" "$U2" "$OV4_1" "$OV4_2" "$OV4_PLEN" || true); echo "$R7"
if [[ "$(vlink_state_of "$R7")" == ready && "$(vlink_idx_of "$R7")" == "$(vlink_idx_of "$R1")" ]]; then
	ok "node1 gre1 re-add state=READY ifindex=$(vlink_idx_of "$R7")"
else
	bad "node1 gre1 re-add inconsistent (state $(vlink_state_of "$R7"))"
fi

hdr "E. Teardown"
T1=$(link1 teardown --sock $SOCK --name gre1 || true); echo "$T1"
T2=$(link2 teardown --sock $SOCK --name gre1 || true); echo "$T2"
T3=$(link1 teardown --sock $SOCK --name gre2 || true); echo "$T3"
T4=$(link2 teardown --sock $SOCK --name gre2 || true); echo "$T4"
T5=$(link1 teardown --sock $SOCK --name gre6 || true); echo "$T5"
T6=$(link2 teardown --sock $SOCK --name gre6 || true); echo "$T6"
sleep 2
for n in "$NODE1" "$NODE2"; do
	for dev in gre1 gre2 gre6; do
		if iface_exists "$n" "$dev"; then bad "$n $dev still present"; else ok "$n $dev removed"; fi
	done
done
# the overlay addresses must have been cleaned up together with the devices
if iface_has_addr "$NODE1" gre1 "$OV4_1/$OV4_PLEN"; then bad "node1 gre1 overlay address not cleaned"
							   else ok "node1 gre1 overlay address cleaned"; fi
if iface_has_addr "$NODE2" gre1 "$OV4_2/$OV4_PLEN"; then bad "node2 gre1 overlay address not cleaned"
							   else ok "node2 gre1 overlay address cleaned"; fi

echo
echo "=== summary: PASS=$PASS FAIL=$FAIL ==="
[[ "$FAIL" -eq 0 ]]
