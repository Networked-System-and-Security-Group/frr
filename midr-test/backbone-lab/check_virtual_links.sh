#!/usr/bin/env bash
# Virtual-link acceptance for the midrd backbone lab (group 1 asks group 3
# for one GRE tunnel per reported MIDR Link).  Run after check_midrd_lab.sh.
#   1. the underlay cannot carry the service prefixes: no transit router has
#      a route to them, so traffic between them can only use the tunnels;
#   2. every READY tunnel answers on its fe80 overlay addresses, and service
#      prefixes of the same group ping each other out of a GRE device whose
#      TX counter grows (IPv6; IPv4 prefixes are not carried by fe80 Links
#      and are reported for information);
#   3. deleting a GRE device withdraws its Link and moves the route off it;
#      the tunnel comes back with a new ifindex, the same link_id and a
#      higher version, and traffic flows again;
#   4. a peer that stops answering on the overlay gets its Link withdrawn
#      while the tunnel stays; the Link returns once it answers again.
#
# Sections 3 and 4 change the lab (delete a device, add an ip6tables rule)
# and put it back.

# No pipefail: see check_midrd_lab.sh.
set -u

LAB_PREFIX="${MIDR_LAB_PREFIX:-clab-midr-backbone-midrd}"
FAMILY="${MIDR_LAB_FAMILY:-ipv4}"

TRANSIT="t1 t2 t3"
MEMBERS="r1 r2 m1a m1b m2a z1 z2"
declare -A NUM=([r1]=111 [m1a]=112 [m1b]=113 [r2]=121 [m2a]=122
		[z1]=191 [z2]=192)
declare -A GROUP=([r1]=1 [m1a]=1 [m1b]=1 [z2]=1 [r2]=2 [m2a]=2 [z1]=2)

case "$FAMILY" in
	ipv4)
		loc() { printf '10.99.0.%s' "${NUM[$1]}"; }
		svc() { printf '198.18.0.%s' "${NUM[$1]}"; }
		IPR=(ip -4 route)
		;;
	ipv6)
		loc() { printf 'fd00:99::%s' "${NUM[$1]}"; }
		svc() { printf 'fd00:18::%s' "${NUM[$1]}"; }
		IPR=(ip -6 route)
		;;
	*)
		printf 'MIDR_LAB_FAMILY must be ipv4 or ipv6\n' >&2
		exit 2
		;;
esac

PASS=0
FAIL=0

pass() { printf 'PASS: %s\n' "$1"; PASS=$((PASS + 1)); }
fail() { printf 'FAIL: %s\n' "$1"; FAIL=$((FAIL + 1)); }
info() { printf 'INFO: %s\n' "$1"; }
container() { printf '%s-%s' "$LAB_PREFIX" "$1"; }
mvty() { docker exec -u root "$(container "$1")" vtysh -d midrd -c "$2" 2>/dev/null; }
rexec() { local node=$1; shift; docker exec -u root "$(container "$node")" "$@" 2>/dev/null; }

# "name node state ifindex overlay-state overlay-remote outer-remote" per
# tunnel, from `show midr virtual-links`.
tunnels()
{
	mvty "$1" 'show midr virtual-links' | awk '
		$1 ~ /^mgre-/ { name = $1; node = $3; state = $4; ifx = $6; reach = $8 }
		$1 == "outer" { outer = $4 }
		$1 == "overlay" { print name, node, state, ifx, reach, $4, outer }'
}

# The tunnel on $1 whose outer remote is $2's locator: "name node ifindex".
tunnel_to()
{
	tunnels "$1" | awk -v o="$(loc "$2")" '$7 == o { print $1, $2, $4 }'
}

# "version ifindex link_id" of the Link from $1 to node id $2 in the snapshot
# handed to group 2; empty when it is not reported.
link_fact()
{
	mvty "$1" 'show midr group2-snapshot' | awk -v n="$2" '
		$1 == "->" { cur = ($2 == n); id = $3; sub(/link_id=/, "", id) }
		cur && /version=/ { v = $NF; sub(/version=/, "", v) }
		cur && $1 == "addr" { print v, $6, id; cur = 0 }'
}

tx_packets()
{
	rexec "$1" ip -s link show dev "$2" | awk '/TX:/ { getline; print $2; exit }'
}

route_dev()
{
	rexec "$1" "${IPR[@]}" get "$2" | grep -o ' dev [^ ]*' | head -1 | cut -c6-
}

wait_for()
{
	local seconds=$1 i
	shift
	for ((i = 0; i < seconds * 2; i++)); do
		"$@" && return 0
		sleep 0.5
	done
	return 1
}

########################################################################
# 1. Only the tunnels can carry the service prefixes
########################################################################
bad=
for t in $TRANSIT; do
	for node in $MEMBERS; do
		rexec "$t" "${IPR[@]}" show | grep -q "^$(svc "$node")[ /]" &&
			bad="$bad $t:$(svc "$node")"
	done
done
[ -z "$bad" ] &&
	pass 'no transit router has a route to a service prefix (the underlay cannot carry them)' ||
	fail "the underlay carries service prefixes:$bad"

########################################################################
# 2. Traffic over the tunnels
########################################################################
for node in $MEMBERS; do
	bad=
	count=0
	while read -r name _ state _ reach remote _; do
		[ "$state" = READY ] || continue
		count=$((count + 1))
		rexec "$node" ping -6 -c 2 -W 2 "$remote%$name" >/dev/null ||
			bad="$bad $name"
	done < <(tunnels "$node")
	[ "$count" -gt 0 ] && [ -z "$bad" ] &&
		pass "$node: all $count tunnels answer on their fe80 overlay addresses" ||
		fail "$node: overlay ping failed on:$bad ($count READY tunnels)"
done

for node in $MEMBERS; do
	for other in $MEMBERS; do
		[ "$other" != "$node" ] && [ "${GROUP[$other]}" = "${GROUP[$node]}" ] || continue
		src="$(svc "$node")"
		dst="$(svc "$other")"
		dev="$(route_dev "$node" "$dst")"
		if [ "${dev#mgre-}" = "$dev" ]; then
			if [ "$FAMILY" = ipv6 ]; then
				fail "$node -> $dst is not routed over GRE (dev ${dev:-none})"
			else
				info "$node -> $dst is not routed over GRE (fe80 Links carry IPv6 only)"
			fi
			continue
		fi
		before="$(tx_packets "$node" "$dev")"
		if rexec "$node" ping -c 3 -W 2 -I "$src" "$dst" >/dev/null; then
			after="$(tx_packets "$node" "$dev")"
			[ $((after - before)) -ge 3 ] &&
				pass "$node $src -> $dst answered, sent over $dev (+$((after - before)) packets)" ||
				fail "$node $src -> $dst answered but $dev sent only $((after - before)) packets"
		else
			fail "$node $src -> $dst over $dev did not answer"
		fi
	done
done

########################################################################
# 3. A deleted GRE device is withdrawn and rebuilt (m1a -> r1)
########################################################################
read -r name rid old_ifx < <(tunnel_to m1a r1)
read -r old_ver _ _ < <(link_fact m1a "$rid")
if [ -z "${name:-}" ] || [ -z "${old_ver:-}" ]; then
	fail "m1a has no reported tunnel to r1 to test recovery with"
else
	info "m1a -> r1 before: $name ifindex $old_ifx, Link version $old_ver"
	link_gone() { [ -z "$(link_fact m1a "$rid")" ]; }
	rexec m1a ip link del "$name"
	if wait_for 10 link_gone; then
		dev="$(route_dev m1a "$(svc r1)")"
		pass "deleting $name withdrew the m1a -> r1 Link"
		[ "$dev" != "$name" ] &&
			pass "m1a no longer routes $(svc r1) over the deleted device (now ${dev:-none})" ||
			fail "m1a still routes $(svc r1) over the deleted $name"
	else
		fail "deleting $name did not withdraw the m1a -> r1 Link"
	fi
	link_back()
	{
		local v x id
		read -r v x id < <(link_fact m1a "$rid")
		[ -n "${v:-}" ] && [ "$v" -gt "$old_ver" ] && [ "$x" != "$old_ifx" ]
	}
	if wait_for 40 link_back; then
		read -r new_ver new_ifx link_id < <(link_fact m1a "$rid")
		read -r _ _ tun_ifx < <(tunnel_to m1a r1)
		# A withdraw and a new upsert each take a version.
		[ "$new_ver" -ge $((old_ver + 2)) ] &&
			pass "$name came back: ifindex $old_ifx -> $new_ifx, version $old_ver -> $new_ver (withdraw + upsert)" ||
			fail "$name came back but version only moved $old_ver -> $new_ver"
		[ "$link_id" = 0 ] && [ "$new_ifx" = "$tun_ifx" ] &&
			pass 'the recovered Link keeps link_id 0 and carries the new tunnel ifindex' ||
			fail "recovered Link: link_id $link_id, ifindex $new_ifx, tunnel ifindex $tun_ifx"
		if [ "$FAMILY" = ipv6 ]; then
			rexec m1a ping -c 3 -W 2 -I "$(svc m1a)" "$(svc r1)" >/dev/null &&
				pass "traffic m1a -> r1 flows again after the rebuild" ||
				fail "traffic m1a -> r1 does not flow after the rebuild"
		fi
	else
		fail "$name was not rebuilt and re-reported within 40 s"
	fi
fi

########################################################################
# 4. A silent peer loses its Link, the tunnel stays (m1a -> r1)
########################################################################
read -r name rid _ < <(tunnel_to m1a r1)
read -r peer_name _ _ < <(tunnel_to r1 m1a)
if [ -z "${name:-}" ] || [ -z "${peer_name:-}" ]; then
	fail "no m1a <-> r1 tunnel pair to test overlay reachability with"
else
	rule=(OUTPUT -o "$peer_name" -p icmpv6 --icmpv6-type echo-reply -j DROP)
	trap 'rexec r1 ip6tables -D "${rule[@]}"' EXIT
	read -r old_ver _ _ < <(link_fact m1a "$rid")
	rexec r1 ip6tables -I "${rule[@]}"
	info "r1 drops echo replies out of $peer_name"
	unreachable()
	{
		tunnels m1a | awk -v n="$name" '$1 == n && $5 == "unreachable"' | grep -q . &&
			[ -z "$(link_fact m1a "$rid")" ]
	}
	if wait_for 40 unreachable; then
		pass "m1a withdrew the Link to r1 once r1 stopped answering on the overlay"
		ip_link="$(rexec m1a ip -o link show dev "$name")"
		[ -n "$ip_link" ] &&
			pass "the tunnel $name itself was kept" ||
			fail "the tunnel $name was deleted"
	else
		fail "m1a kept the Link to r1 although r1 does not answer on the overlay"
	fi
	rexec r1 ip6tables -D "${rule[@]}"
	trap - EXIT
	answered_again()
	{
		local v
		read -r v _ _ < <(link_fact m1a "$rid")
		[ -n "${v:-}" ] && [ "$v" -gt "${old_ver:-0}" ]
	}
	wait_for 20 answered_again &&
		pass "the Link to r1 returned with a higher version once r1 answered again" ||
		fail "the Link to r1 did not return after r1 answered again"
fi

printf 'virtual-link result (%s): %d passed, %d failed\n' "$FAMILY" "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
