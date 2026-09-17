#!/usr/bin/env bash
# Acceptance checks for the midrd variant of the 15-node backbone lab.
#
# Group 1 (discovery, measurement, clustering, Tier1 admission) and group 2
# (link-state flooding, TED, SPF) run inside midrd; bgpd only carries the
# eBGP underlay.  Checked here:
#   1. every node runs the expected daemons and the underlay is up;
#   2. the underlay forwards between every pair of MIDR transports;
#   3. groups form as configured (bootstraps, reps) or chosen by CL (z2);
#   4. overlay sessions are native midrd sessions, full mesh inside each
#      group, a backbone mesh between bootstraps and the manual r1-r2 edge;
#   5. the node directory group 1 floods matches every node's table;
#   6. group 2 accepted the Node/Link facts and SPF reaches every member of
#      the same group; inter-group routes are reported for information.
# The Tier1 admission scenario (z1) is checked by check_tier1_admission.sh.
#
# Read-only.

set -uo pipefail

LAB_PREFIX="${MIDR_LAB_PREFIX:-clab-midr-backbone-midrd}"
FAMILY="${MIDR_LAB_FAMILY:-ipv4}"
WAIT_SECONDS="${MIDR_DEMO_WAIT_SECONDS:-600}"
POLL_SECONDS="${MIDR_DEMO_POLL_SECONDS:-10}"

TRANSIT="t1 t2 t3"
BOOTSTRAPS="b1 b2 b3 b4 b5"
MEMBERS="r1 r2 m1a m1b m2a z1 z2"
ALL_NODES="$TRANSIT $BOOTSTRAPS $MEMBERS"
MIDR_NODES="$BOOTSTRAPS $MEMBERS"

declare -A NUM=([b1]=101 [b2]=102 [b3]=103 [b4]=104 [b5]=105
		[t1]=201 [t2]=202 [t3]=203
		[r1]=111 [m1a]=112 [m1b]=113 [r2]=121 [m2a]=122
		[z1]=191 [z2]=192)
declare -A GROUP=([r1]=1 [m1a]=1 [m1b]=1 [z2]=1 [r2]=2 [m2a]=2 [z1]=2)

case "$FAMILY" in
	ipv4)
		loc() { printf '10.99.0.%s' "${NUM[$1]}"; }
		svc() { printf '198.18.0.%s/32' "${NUM[$1]}"; }
		PING=(ping -4)
		UNDERLAY_AF=ipv4Unicast
		;;
	ipv6)
		loc() { printf 'fd00:99::%s' "${NUM[$1]}"; }
		svc() { printf 'fd00:18::%s/128' "${NUM[$1]}"; }
		PING=(ping -6)
		UNDERLAY_AF=ipv6Unicast
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
# MIDR commands go to midrd only; bgpd still carries the old implementation.
# midrd runs as root without privilege separation, so its vty socket is
# root-only.
mvty() { docker exec -u root "$(container "$1")" vtysh -d midrd -c "$2" 2>/dev/null; }
bvty() { docker exec "$(container "$1")" vtysh -d bgpd -c "$2" 2>/dev/null; }
rexec() { local node=$1; shift; docker exec -u root "$(container "$node")" "$@" 2>/dev/null; }
midrd_log() { rexec "$1" cat /etc/frr/logs/midrd.log; }

field()
{
	printf '%s\n' "$1" | awk -F: -v key="$2" '$1 ~ key {sub(/^[^:]*: */, ""); print; exit}'
}

# Prints "<state> <connectionsEstablished> <origin>" for one session.
session()
{
	mvty "$1" 'show midr neighbors json' | python3 -c '
import json, sys
try:
    peer = json.load(sys.stdin).get("neighbors", {}).get(sys.argv[1])
except ValueError:
    peer = None
if not peer:
    print("absent 0 -")
else:
    print(peer["state"], peer["connectionsEstablished"], peer["origin"])
' "$2"
}

established_count()
{
	mvty "$1" 'show midr neighbors json' | python3 -c '
import json, sys
try:
    print(json.load(sys.stdin).get("establishedCount", 0))
except ValueError:
    print(0)
'
}

member_ready()
{
	local node=$1 self group2 gid

	self="$(mvty "$node" 'show midr self')" || return 1
	group2="$(mvty "$node" 'show midr group2')" || return 1
	gid="$(field "$self" '^Group-ID')"
	[ "${gid:-0}" != 0 ] || return 1
	[ "$(established_count "$node")" -gt 0 ] || return 1
	printf '%s\n' "$group2" | grep -q 'reported=1 pending=0' || return 1
}

lab_ready()
{
	local node

	for node in $ALL_NODES; do
		[ "$(docker inspect -f '{{.State.Running}}' "$(container "$node")" 2>/dev/null)" = true ] ||
			return 1
	done
	for node in $MEMBERS; do
		member_ready "$node" || return 1
	done
	for node in $BOOTSTRAPS; do
		[ "$(established_count "$node")" -gt 0 ] || return 1
	done
}

command -v docker >/dev/null 2>&1 || { echo 'docker is unavailable'; exit 2; }

deadline=$(( $(date +%s) + WAIT_SECONDS ))
printf 'Waiting up to %ss for the midrd lab (%s) to converge...\n' "$WAIT_SECONDS" "$FAMILY"
until lab_ready; do
	if [ "$(date +%s)" -ge "$deadline" ]; then
		echo 'Lab did not converge before the timeout; checking anyway.'
		break
	fi
	sleep "$POLL_SECONDS"
done
# Let the same-group mesh and the SPF settle after the last join.
sleep "${MIDR_DEMO_SETTLE_SECONDS:-30}"

########################################################################
# 1. Daemons and underlay
########################################################################
bad=
for node in $ALL_NODES; do
	running="$(rexec "$node" pgrep -x midrd)"
	case " $MIDR_NODES " in
		*" $node "*) [ -n "$running" ] || bad="$bad $node(no-midrd)" ;;
		*) [ -z "$running" ] || bad="$bad $node(unexpected-midrd)" ;;
	esac
done
[ -z "$bad" ] && pass 'midrd runs on the 12 MIDR nodes and not on the transit routers' ||
	fail "unexpected midrd process state:$bad"

bad=
for node in $ALL_NODES; do
	bvty "$node" 'show bgp summary json' | python3 -c '
import json, sys
peers = json.load(sys.stdin).get(sys.argv[1], {}).get("peers", {})
ok = peers and all(p.get("state") == "Established" for p in peers.values())
sys.exit(0 if ok else 1)
' "$UNDERLAY_AF" || bad="$bad $node"
done
[ -z "$bad" ] && pass 'every underlay eBGP session is Established on all 15 nodes' ||
	fail "underlay BGP is not fully Established on:$bad"

bad=
for node in $MIDR_NODES; do
	n="$(bvty "$node" 'show bgp summary json' | python3 -c '
import json, sys
data = json.load(sys.stdin)
print(sum(len(af.get("peers", {})) for af in data.values() if isinstance(af, dict)))
')"
	[ "$n" = 1 ] || bad="$bad $node($n)"
done
[ -z "$bad" ] && pass 'bgpd on MIDR nodes has only its underlay neighbor (no BGP overlay sessions)' ||
	fail "bgpd carries unexpected neighbors on:$bad"

########################################################################
# 2. The underlay forwards between every pair of MIDR transports
########################################################################
failed_pairs=
pairs=0
for src in $MIDR_NODES; do
	for dst in $MIDR_NODES; do
		[ "$src" = "$dst" ] && continue
		pairs=$((pairs + 1))
		rexec "$src" "${PING[@]}" -n -c 2 -i 0.2 -W 2 -I "$(loc "$src")" "$(loc "$dst")" |
			grep -q ' 0% packet loss' || failed_pairs="$failed_pairs $src->$dst"
	done
done
[ -z "$failed_pairs" ] && pass "all $pairs transport pairs of the 12 MIDR nodes forward end to end" ||
	fail "transport pairs without full delivery:$failed_pairs"

########################################################################
# 3. Groups and roles
########################################################################
for node in $BOOTSTRAPS; do
	self="$(mvty "$node" 'show midr self')"
	[ "$(field "$self" '^Group-ID')" = 0 ] &&
		printf '%s\n' "$self" | grep -q '^Capabilities *:.*Bootstrap' &&
		pass "$node is a group-0 bootstrap" ||
		fail "$node is not a group-0 bootstrap: $(printf '%s' "$self" | tr '\n' ' ')"
done
for node in $MEMBERS; do
	self="$(mvty "$node" 'show midr self')"
	gid="$(field "$self" '^Group-ID')"
	[ "$gid" = "${GROUP[$node]}" ] &&
		pass "$node is in group $gid" ||
		fail "$node is in group '${gid:-?}', expected ${GROUP[$node]}"
	[ "$(field "$self" '^Active locator')" = "$(loc "$node")" ] ||
		fail "$node active locator is not $(loc "$node")"
done
for node in r1 r2; do
	mvty "$node" 'show midr self' | grep -q '^Capabilities *:.*GroupRep' &&
		pass "$node is its group's representative" ||
		fail "$node is not a representative"
done
z2_log="$(midrd_log z2)"
recommend="$(printf '%s\n' "$z2_log" | grep 'REP_PROBE_DONE → RECOMMEND' | tail -n 1)"
printf '%s\n' "$recommend" | grep -q '群 1 代表' &&
	pass "zero-config z2 joined by performance: ${recommend#*MIDR CL: }" ||
	fail "z2 has no CL recommendation for group 1: ${recommend:-none}"

########################################################################
# 4. Native overlay sessions
########################################################################
bad=
for a in $MEMBERS; do
	for b in $MEMBERS; do
		[ "$a" = "$b" ] || [ "${GROUP[$a]}" != "${GROUP[$b]}" ] && continue
		read -r state count origin <<<"$(session "$a" "$(loc "$b")")"
		[ "$state" = Established ] || bad="$bad $a->$b($state,$origin)"
	done
done
[ -z "$bad" ] && pass 'each group is a full mesh of Established midrd sessions' ||
	fail "missing same-group sessions:$bad"

bad=
for a in $BOOTSTRAPS; do
	for b in $BOOTSTRAPS; do
		[ "$a" = "$b" ] && continue
		read -r state count origin <<<"$(session "$a" "$(loc "$b")")"
		[ "$state" = Established ] && [ "$origin" = MANUAL ] ||
			bad="$bad $a->$b($state,$origin)"
	done
done
[ -z "$bad" ] && pass 'the five bootstraps form an Established MANUAL backbone mesh' ||
	fail "backbone mesh incomplete:$bad"

read -r state count origin <<<"$(session r1 "$(loc r2)")"
read -r state2 count2 origin2 <<<"$(session r2 "$(loc r1)")"
[ "$state" = Established ] && [ "$state2" = Established ] &&
	pass "the manual r1-r2 edge is Established ($origin/$origin2)" ||
	fail "r1-r2 edge is $state/$state2"

for node in r1 r2; do
	attach="$(mvty "$node" 'show midr neighbors json' | python3 -c '
import json, sys
peers = json.load(sys.stdin).get("neighbors", {})
print(sum(1 for p in peers.values()
          if p["origin"] == "ATTACH" and p["state"] == "Established"))
')"
	[ "${attach:-0}" -ge 1 ] &&
		pass "$node is attached to $attach bootstrap(s)" ||
		fail "$node has no Established ATTACH session"
done

bad=
for node in $MIDR_NODES; do
	midrd_log "$node" | grep -q 'session requested via midr_session_request' ||
		bad="$bad $node"
done
[ -z "$bad" ] && pass 'all MIDR nodes request their sessions through midr_session_request' ||
	fail "no midr_session_request log on:$bad"

bad=
for node in $MIDR_NODES; do
	mvty "$node" 'show midr neighbors' | awk '$3 == "Established" {print $1}' |
		while read -r addr; do
			case "$addr" in
				"$(loc "$node" | sed 's/[0-9a-f]*$//')"*) ;;
				*) echo "$addr" ;;
			esac
		done | grep -q . && bad="$bad $node"
done
[ -z "$bad" ] && pass "every Established session uses a $FAMILY transport address" ||
	fail "sessions on unexpected addresses:$bad"

########################################################################
# 5. Node directory
########################################################################
for node in $MEMBERS; do
	remote="$(mvty "$node" 'show midr group2-remote')"
	printf '%s\n' "$remote" | grep -q '两侧一致' &&
		pass "$node node table matches the flooded node directory" ||
		fail "$node node table differs from the directory: $(printf '%s' "$remote" | tr '\n' ' ' | cut -c1-300)"
done
bad=
for node in $MEMBERS; do
	dir="$(mvty "$node" 'show midr directory')"
	for other in $MEMBERS; do
		[ "$other" = "$node" ] && continue
		printf '%s\n' "$dir" | awk -v loc="$(loc "$other")" -v g="${GROUP[$other]}" \
			'$4 == loc && $2 == g && $0 !~ /withdrawn/ {found = 1} END {exit !found}' ||
			bad="$bad $node:$other"
	done
done
[ -z "$bad" ] && pass 'every member learned all six other members (group and locator) from the directory' ||
	fail "directory entries missing or wrong:$bad"

########################################################################
# 6. Group 2 facts and SPF
########################################################################
for node in $MEMBERS; do
	group2="$(mvty "$node" 'show midr group2')"
	printf '%s\n' "$group2" | grep -q 'node *: valid=1 reported=1 pending=0' &&
		printf '%s\n' "$group2" | grep -qE 'link *: 条目 [1-9][0-9]*（reported [1-9][0-9]* / pending 0）' &&
		pass "$node Node and Link facts were accepted by group 2" ||
		fail "$node facts not accepted: $(printf '%s' "$group2" | grep -E 'node|link' | tr '\n' ' ')"
done
for node in $BOOTSTRAPS; do
	mvty "$node" 'show midr group2' | grep -q 'reported=0' &&
		pass "$node (group 0) does not report itself to group 2" ||
		fail "$node reported a group-0 node"
done

for node in $MEMBERS; do
	spf="$(mvty "$node" 'show midr spf')"
	missing=
	for other in $MEMBERS; do
		[ "${GROUP[$other]}" = "${GROUP[$node]}" ] || continue
		if [ "$other" = "$node" ]; then
			scope=local
		else
			scope=intra
		fi
		printf '%s\n' "$spf" | grep -qE "^  $(svc "$other") origin [0-9.]+ $scope " ||
			missing="$missing $other"
	done
	[ -z "$missing" ] &&
		pass "$node SPF reaches every member of group ${GROUP[$node]}" ||
		fail "$node SPF lacks routes for:$missing"
	info "$node $(printf '%s\n' "$spf" | grep '^Summary')"
done

printf 'midrd lab result (%s): %d passed, %d failed\n' "$FAMILY" "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
