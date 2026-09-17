#!/usr/bin/env bash
# IPv6-only checks for the 15-node backbone lab (MIDR_LAB_FAMILY=ipv6).
#
# Verifies that the lab really is IPv6-only, that every node forwards IPv6,
# that the underlay carries IPv6 transport reachability hop by hop, and that
# the first/second-group control plane (overlay sessions, Node/Link facts,
# Prefix input) runs on IPv6.  The group-3 route installation is reported
# for information only.
#
# Read-only; run after the base acceptance check has converged.

set -uo pipefail

LAB_PREFIX="${MIDR_LAB_PREFIX:-clab-midr-backbone-v6}"
# bgpd: MIDR in bgpd (reference); midrd: MIDR in midrd, bgpd is underlay only.
STACK="${MIDR_LAB_STACK:-bgpd}"

TRANSIT="t1 t2 t3"
BOOTSTRAPS="b1 b2 b3 b4 b5"
MEMBERS="r1 r2 m1a m1b m2a z1 z2"
ALL_NODES="$TRANSIT $BOOTSTRAPS $MEMBERS"
MIDR_NODES="$BOOTSTRAPS $MEMBERS"

declare -A NUM=([b1]=101 [b2]=102 [b3]=103 [b4]=104 [b5]=105
		[t1]=201 [t2]=202 [t3]=203
		[r1]=111 [m1a]=112 [m1b]=113 [r2]=121 [m2a]=122
		[z1]=191 [z2]=192)
# Routers between two nodes on the underlay path (each decrements hop limit).
declare -A ACCESS=([b1]=t1 [b2]=t1 [r1]=t1 [m1a]=t1 [z2]=t1
		   [b3]=t2 [m1b]=t2 [r2]=t2
		   [b4]=t3 [b5]=t3 [m2a]=t3 [z1]=t3)

PASS=0
FAIL=0

pass() { printf 'PASS: %s\n' "$1"; PASS=$((PASS + 1)); }
fail() { printf 'FAIL: %s\n' "$1"; FAIL=$((FAIL + 1)); }
info() { printf 'INFO: %s\n' "$1"; }
container() { printf '%s-%s' "$LAB_PREFIX" "$1"; }
vty() { docker exec "$(container "$1")" vtysh -c "$2" 2>/dev/null; }
if [ "$STACK" = midrd ]; then
	# midrd's vty socket is root-only (no privilege separation).
	mvty() { docker exec -u root "$(container "$1")" vtysh -d midrd -c "$2" 2>/dev/null; }
else
	mvty() { vty "$@"; }
fi
rexec() { local node=$1; shift; docker exec -u root "$(container "$node")" "$@" 2>/dev/null; }
loc() { printf 'fd00:99::%s' "${NUM[$1]}"; }

expected_hops()
{
	local a=${ACCESS[$1]} b=${ACCESS[$2]}

	if [ "$a" = "$b" ]; then
		echo 1
	else
		echo 2
	fi
}

########################################################################
# 1. The lab is IPv6-only and every node forwards IPv6
########################################################################
bad_v4=
bad_fwd=
for node in $ALL_NODES; do
	v4="$(rexec "$node" ip -4 -o addr show scope global)"
	[ -z "$v4" ] || bad_v4="$bad_v4 $node"
	[ "$(rexec "$node" sysctl -n net.ipv6.conf.all.forwarding)" = 1 ] &&
		vty "$node" 'show ipv6 forwarding' | grep -q 'is on' ||
		bad_fwd="$bad_fwd $node"
done
[ -z "$bad_v4" ] && pass 'no node has a global IPv4 address (only 127.0.0.1 remains)' ||
	fail "global IPv4 addresses remain on:$bad_v4"
[ -z "$bad_fwd" ] && pass 'IPv6 forwarding is on in the kernel and zebra on all 15 nodes' ||
	fail "IPv6 forwarding is off on:$bad_fwd"

bad=
for node in $ALL_NODES; do
	summary="$(vty "$node" 'show bgp summary json')"
	printf '%s\n' "$summary" | python3 -c '
import json, sys
data = json.load(sys.stdin)
v4 = data.get("ipv4Unicast", {}).get("peers", {})
v6 = data.get("ipv6Unicast", {}).get("peers", {})
ok = not v4 and v6 and all(p.get("state") == "Established" for p in v6.values())
sys.exit(0 if ok else 1)
' || bad="$bad $node"
done
[ -z "$bad" ] && pass 'underlay BGP runs IPv6 unicast only, all sessions Established' ||
	fail "underlay BGP is not IPv6-only/Established on:$bad"

########################################################################
# 2. The underlay forwards IPv6 between every pair of MIDR transports
########################################################################
failed_pairs=
hop_mismatch=
pairs=0
for src in $MIDR_NODES; do
	for dst in $MIDR_NODES; do
		[ "$src" = "$dst" ] && continue
		pairs=$((pairs + 1))
		out="$(rexec "$src" ping -6 -n -c 2 -i 0.2 -W 2 -I "$(loc "$src")" "$(loc "$dst")")"
		if ! printf '%s\n' "$out" | grep -q ' 0% packet loss'; then
			failed_pairs="$failed_pairs $src->$dst"
			continue
		fi
		ttl="$(printf '%s\n' "$out" | sed -n 's/.*ttl=\([0-9]*\).*/\1/p' | head -n 1)"
		[ "$ttl" = "$((64 - $(expected_hops "$src" "$dst")))" ] ||
			hop_mismatch="$hop_mismatch $src->$dst(ttl=$ttl)"
	done
done
[ -z "$failed_pairs" ] && pass "all $pairs transport pairs of the 12 MIDR nodes reach each other over IPv6" ||
	fail "IPv6 transport pairs without full delivery:$failed_pairs"
[ -z "$hop_mismatch" ] &&
	pass 'every reply hop limit matches the transit routers on its underlay path (forwarded by t1-t3)' ||
	fail "unexpected hop limit (path not through the expected transit routers):$hop_mismatch"

# BGP installs the peer's link-local next hop when it has one.
route="$(rexec z1 ip -6 route get "$(loc r1)" from "$(loc z1)")"
t3_ll="$(rexec t3 ip -6 -o addr show dev eth6 scope link | awk '{sub(/\/.*/, "", $4); print $4}')"
info "z1 -> r1 FIB lookup: $route (t3 eth6 link-local ${t3_ll:-?})"
if printf '%s\n' "$route" | grep -qE "via (fd00:10:31::2|${t3_ll:-none}) dev eth1 "; then
	pass 'z1 forwards to r1 through t3 (eth1 next hop is t3)'
else
	fail 'z1 does not forward to r1 through t3'
fi

########################################################################
# 3. First/second-group control plane runs on IPv6
########################################################################
bad=
for node in $MEMBERS; do
	self="$(mvty "$node" 'show midr self')"
	printf '%s\n' "$self" | grep -q '^Address family *: IPv6' &&
		printf '%s\n' "$self" | grep -q "^Active locator *: $(loc "$node")\$" ||
		bad="$bad $node"
done
[ -z "$bad" ] && pass 'every MIDR member reports an IPv6 transport family and locator' ||
	fail "MIDR self is not IPv6 on:$bad"

bad=
for node in $MIDR_NODES; do
	neighbors="$(mvty "$node" 'show midr neighbors')"
	est="$(printf '%s\n' "$neighbors" | awk '$3 == "Established"')"
	[ -n "$est" ] && ! printf '%s\n' "$est" | awk '{print $1}' | grep -qv ':' ||
		bad="$bad $node"
done
[ -z "$bad" ] && pass 'every MIDR node has Established overlay sessions, all to IPv6 transports' ||
	fail "missing or non-IPv6 overlay sessions on:$bad"

bad=
for node in $MEMBERS; do
	links="$(mvty "$node" 'show midr group2-snapshot')"
	printf '%s\n' "$links" | grep -q 'fd00:99::' &&
		! printf '%s\n' "$links" | grep -qE '\b10\.99\.' || bad="$bad $node"
done
[ -z "$bad" ] && pass 'first-group Node/Link facts handed to group 2 carry IPv6 endpoints only' ||
	fail "group-2 snapshot has missing or IPv4 endpoints on:$bad"

bad=
if [ "$STACK" = midrd ]; then
	# midrd takes its service prefix from --prefix; SPF shows what it used.
	for node in $MEMBERS; do
		spf="$(mvty "$node" 'show midr spf')"
		printf '%s\n' "$spf" | grep -qE '^  fd00:18::' &&
			! printf '%s\n' "$spf" | grep -qE '^  [0-9]+\.[0-9]+\.' ||
			bad="$bad $node"
	done
	[ -z "$bad" ] && pass 'midrd SPF carries IPv6 prefixes only' ||
		fail "midrd SPF has missing or IPv4 prefixes on:$bad"
fi
for node in $([ "$STACK" = midrd ] || echo "$MEMBERS"); do
	prefix="$(vty "$node" 'show midr prefix summary')"
	v4=$(printf '%s\n' "$prefix" | awk -F: '/^  IPv4 contributors/ {gsub(/ /, "", $2); print $2}')
	v6=$(printf '%s\n' "$prefix" | awk -F: '/^  IPv6 contributors/ {gsub(/ /, "", $2); print $2}')
	[ "${v4:-x}" = 0 ] && [ "${v6:-0}" -gt 0 ] || bad="$bad $node(v4=${v4:-?},v6=${v6:-?})"
done
if [ "$STACK" != midrd ]; then
	[ -z "$bad" ] && pass 'group-2 Prefix input sees IPv6 contributors only' ||
		fail "unexpected Prefix contributors:$bad"
fi

########################################################################
# 4. Group-3 IPv6 routes (information only)
########################################################################
for node in $([ "$STACK" = midrd ] || echo "$MEMBERS"); do
	spf="$(vty "$node" 'show midr spf summary')"
	routes="$(vty "$node" 'show midr spf routes')"
	v6_reach=$(printf '%s\n' "$routes" | grep -c 'route afi=2 .*reachable=yes')
	v4_routes=$(printf '%s\n' "$routes" | grep -c 'route afi=1 ')
	counts=$(printf '%s\n' "$spf" | awk -F: '/local\/intra\/inter/ {gsub(/ /, "", $2); print $2}')
	fib=$(rexec "$node" ip -6 route show proto 199 | wc -l)
	info "$node SPF: IPv6 reachable routes=$v6_reach IPv4 routes=$v4_routes local/intra/inter=$counts kernel proto-199 IPv6 routes=$fib"
done
if [ "$STACK" = midrd ]; then
	# midrd does not install routes yet (group 3's zclient work).
	for node in $MEMBERS; do
		info "$node midrd $(mvty "$node" 'show midr spf' | grep '^Summary')"
	done
fi
sample="$(vty m1a "show ipv6 route $(loc r1)/128")"
info "m1a zebra entry for r1 transport: $(printf '%s' "$sample" | tr '\n' ' ' | tr -s ' ')"

printf 'IPv6-only result: %d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
