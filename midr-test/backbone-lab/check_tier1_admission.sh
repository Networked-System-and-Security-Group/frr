#!/usr/bin/env bash
# Tier1 admission checks for the 15-node backbone lab.
#
# Scenario (see configs-backbone/z1 and midr-backbone.clab.yaml):
#   - z1 loads an IP2ASN snapshot mapping every t1 address to AS 64500 and a
#     Tier1 list containing only AS 64500, and enables `midr avoid-tier1`.
#   - t2<->t3 carries 4 ms of netem delay each way, so from z1 the targets
#     behind t1 (r1, m1a) measure faster than those behind t2 (r2, m1b).
# Expected:
#   - z1 screens every candidate with traceroute before CL decides; CL ranks
#     r1 (group 1) first by performance, skips it, and recommends group 2;
#   - z1's admission refuses r1 (Tier1 observed); since r1 is the only
#     group-1 node its member list offers, CL selects no group-1 anchor;
#   - z1 never establishes a MIDR session with r1 or m1a (both directions);
#   - z1 ends up Established with r2 and m2a (group 2), and these sessions
#     were created through the session API: midr_peer_session_request() in
#     bgpd, midr_session_request() in midrd (MIDR_LAB_STACK=midrd).
#
# Read-only; run after the base acceptance check has converged.

# No pipefail: pipelines here feed large logs into `grep -q`, which exits
# at the first match and makes the writer fail with SIGPIPE.
set -u

LAB_PREFIX="${MIDR_LAB_PREFIX:-clab-midr-backbone}"
LOG_ROOT="${MIDR_LAB_LOG_ROOT:-}"
WAIT_SECONDS="${MIDR_TIER1_WAIT_SECONDS:-300}"
POLL_SECONDS="${MIDR_TIER1_POLL_SECONDS:-10}"
OBSERVE_SECONDS="${MIDR_TIER1_OBSERVE_SECONDS:-120}"

FAMILY="${MIDR_LAB_FAMILY:-ipv4}"
STACK="${MIDR_LAB_STACK:-bgpd}"

# Transport locator of node number N, and t1's address on the t1-t3 link.
case "$FAMILY" in
	ipv4) loc() { printf '10.99.0.%s' "$1"; }; T1_LINK=10.10.3.1; PING=(ping -4) ;;
	ipv6) loc() { printf 'fd00:99::%s' "$1"; }; T1_LINK=fd00:10:3::1; PING=(ping -6) ;;
	*) printf 'MIDR_LAB_FAMILY must be ipv4 or ipv6\n' >&2; exit 2 ;;
esac
LOC_PREFIX="$(loc '')"

Z1_TRANSPORT=$(loc 191)
BLOCKED_TRANSPORTS="$(loc 111) $(loc 112)" # r1 m1a, behind t1
SCREENED_TRANSPORTS="$(loc 111)"           # candidates z1 actually traced
BLOCKED_NODES="r1 m1a"
SLOW_TRANSPORTS="$(loc 113) $(loc 121)"    # m1b r2, behind t2

PASS=0
FAIL=0

pass() { printf 'PASS: %s\n' "$1"; PASS=$((PASS + 1)); }
fail() { printf 'FAIL: %s\n' "$1"; FAIL=$((FAIL + 1)); }
info() { printf 'INFO: %s\n' "$1"; }
container() { printf '%s-%s' "$LAB_PREFIX" "$1"; }
case "$STACK" in
	bgpd)
		VTYSH=(vtysh)
		MIDR_LOG=/etc/frr/logs/frr.log
		SESSION_LOG='midr_peer_session_request for [0-9./]* %s AS'
		;;
	midrd)
		# MIDR runs in midrd; bgpd only carries the underlay.
		VTYSH=(vtysh -d midrd)
		MIDR_LOG=/etc/frr/logs/midrd.log
		SESSION_LOG='midr_session_(connect|request) for [0-9./]* %s \\('
		;;
	*) printf 'MIDR_LAB_STACK must be bgpd or midrd\n' >&2; exit 2 ;;
esac
# midrd's vty socket is root-only (no privilege separation).
vty() { docker exec -u root "$(container "$1")" "${VTYSH[@]}" -c "$2" 2>/dev/null; }
node_log() { docker exec -u root "$(container "$1")" cat "$MIDR_LOG" 2>/dev/null; }
session_log() { printf "$SESSION_LOG" "$1"; }

# Prints "<state> <connectionsEstablished>" or "absent 0".
peer_state()
{
	if [ "$STACK" = midrd ]; then
		vty "$1" 'show midr neighbors json' | python3 -c '
import json, sys
try:
    peer = json.load(sys.stdin).get("neighbors", {}).get(sys.argv[1])
except ValueError:
    peer = None
print("absent 0" if not peer else
      "%s %s" % (peer["state"], peer["connectionsEstablished"]))
' "$2"
		return
	fi
	vty "$1" "show bgp neighbors $2 json" | python3 -c '
import json, sys
try:
    data = json.load(sys.stdin)
except ValueError:
    data = {}
peer = data.get(sys.argv[1])
if not isinstance(peer, dict):
    print("absent 0")
else:
    print(peer.get("bgpState", "?"), peer.get("connectionsEstablished", 0))
' "$2"
}

# Average RTT in microseconds from z1's transport to a target transport.
ping_rtt_us()
{
	docker exec -u root "$(container z1)" "${PING[@]}" -n -q -c 10 -i 0.2 -I "$Z1_TRANSPORT" "$1" 2>/dev/null |
		awk -F/ '/^rtt|^round-trip/ {printf "%d\n", $5 * 1000}'
}

# Established MIDR overlay peers on z1 (transport addresses).
z1_established_overlays()
{
	if [ "$STACK" = midrd ]; then
		vty z1 'show midr neighbors json' | python3 -c '
import json, sys
try:
    peers = json.load(sys.stdin).get("neighbors", {})
except ValueError:
    peers = {}
for addr, peer in peers.items():
    if peer.get("state") == "Established":
        print(addr)
'
		return
	fi
	vty z1 'show bgp neighbors json' | python3 -c '
import json, sys
try:
    data = json.load(sys.stdin)
except ValueError:
    data = {}
for addr, peer in data.items():
    if not isinstance(peer, dict) or not addr.startswith(sys.argv[1]):
        continue
    if peer.get("bgpState") == "Established":
        print(addr)
' "$LOC_PREFIX"
}

########################################################################
# 1. Scenario prerequisites
########################################################################
tier1_list="$(vty z1 'show midr tier1 list')"
printf '%s\n' "$tier1_list" | grep -q '64500' &&
	pass 'z1 loaded the Tier1 list containing AS 64500' ||
	fail "z1 did not load the Tier1 list: $tier1_list"

lookup="$(vty z1 "show midr ip2asn $T1_LINK")"
printf '%s\n' "$lookup" | grep -q '64500' &&
	pass "z1 maps t1 ($T1_LINK) to AS 64500" ||
	fail "z1 IP2ASN lookup for t1 is wrong: $lookup"

admission="$(vty z1 'show midr admission')"
printf '%s\n' "$admission" | grep -q 'avoid-tier1: enabled' &&
	pass 'z1 has avoid-tier1 enabled' || fail 'z1 avoid-tier1 is not enabled'
for node in r1 z2; do
	vty "$node" 'show midr admission' | grep -q 'avoid-tier1: disabled' &&
		pass "$node keeps avoid-tier1 disabled (control)" ||
		fail "$node unexpectedly enables avoid-tier1"
done

for spec in t2:eth2 t3:eth1; do
	node=${spec%%:*}
	dev=${spec#*:}
	docker exec -u root "$(container "$node")" tc qdisc show dev "$dev" | grep -q 'netem.*delay' &&
		pass "$node $dev carries the netem delay" ||
		fail "$node $dev has no netem delay"
done

trace="$(vty z1 "show midr traceroute $(loc 111) refresh")"
info "z1 traceroute towards r1 (diagnostic, auto source): $(printf '%s' "$trace" | tr '\n' ' ' | cut -c1-300)"

########################################################################
# 2. The blocked targets really are the best-performing ones
########################################################################
best_blocked=
worst_blocked=0
for t in $BLOCKED_TRANSPORTS; do
	rtt=$(ping_rtt_us "$t")
	info "z1 -> $t ping avg ${rtt:-?} us (behind t1 / AS 64500)"
	[ -n "$rtt" ] || { fail "z1 cannot ping $t from its transport"; continue; }
	[ -z "$best_blocked" ] || [ "$rtt" -lt "$best_blocked" ] && best_blocked=$rtt
	[ "$rtt" -gt "$worst_blocked" ] && worst_blocked=$rtt
done
best_slow=
for t in $SLOW_TRANSPORTS; do
	rtt=$(ping_rtt_us "$t")
	info "z1 -> $t ping avg ${rtt:-?} us (behind t2)"
	[ -n "$rtt" ] || { fail "z1 cannot ping $t from its transport"; continue; }
	[ -z "$best_slow" ] || [ "$rtt" -lt "$best_slow" ] && best_slow=$rtt
done
if [ -n "$best_slow" ] && [ "$worst_blocked" -gt 0 ] && [ "$worst_blocked" -lt "$best_slow" ]; then
	pass "r1/m1a (<= ${worst_blocked} us) are faster than r2/m1b (>= ${best_slow} us) from z1"
else
	fail "delay setup did not make r1/m1a the fastest (blocked worst=${worst_blocked} slow best=${best_slow:-?})"
fi

########################################################################
# 3. CL decided with the path verdict: skip r1's group, join another one
########################################################################
z1_log="$(node_log z1)"
skipped="$(printf '%s\n' "$z1_log" | grep 'REP_PROBE_DONE → 跳过第 1 名群 1 代表 10.0.0.111' | head -n 1)"
recommend="$(printf '%s\n' "$z1_log" | grep 'REP_PROBE_DONE → RECOMMEND' | head -n 1)"
[ -n "$skipped" ] &&
	pass 'z1 CL ranked r1 (group 1) first by performance and skipped it for Tier1' ||
	fail 'z1 CL has no log of skipping the top-ranked r1'
printf '%s\n' "$recommend" | grep -q '群 2 代表 10.0.0.121' &&
	pass "z1 CL recommended group 2 instead: ${recommend#*MIDR CL: }" ||
	fail "z1 CL did not recommend group 2: ${recommend:-no RECOMMEND log}"

gid="$(vty z1 'show midr self' | awk -F: '/^Group-ID/ {gsub(/ /, "", $2); print $2; exit}')"
[ "$gid" = 2 ] && pass 'z1 settled in group 2, not the best-performing but forbidden group 1' ||
	fail "z1 is in group '${gid:-?}', expected 2"

########################################################################
# 4. Blocked targets: admission verdict and no session in either direction
########################################################################
# Screening entries expire after 10 minutes without re-evaluation, so the
# refusal log is the durable record; the table is shown for reference.
deadline=$(( $(date +%s) + WAIT_SECONDS ))
until node_log z1 | grep -qF "MIDR admission: refusing $(loc 111): Tier1 observed"; do
	[ "$(date +%s)" -ge "$deadline" ] && break
	sleep "$POLL_SECONDS"
done
z1_log="$(node_log z1)"
for t in $SCREENED_TRANSPORTS; do
	printf '%s\n' "$z1_log" | grep -qF "MIDR admission: refusing $t: Tier1 observed" &&
		pass "z1 admission refused $t: Tier1 observed on its path" ||
		fail "z1 admission never refused $t"
done
printf '%s\n' "$z1_log" | grep 'MIDR admission: refusing' | sed 's/^/INFO: /'
printf '\n----- z1 show midr admission -----\n'
vty z1 'show midr admission'
printf '\n'

# Sample for a while: a blocked edge must never reach Established.
end=$(( $(date +%s) + OBSERVE_SECONDS ))
bad=
while :; do
	for t in $BLOCKED_TRANSPORTS; do
		read -r state count <<<"$(peer_state z1 "$t")"
		if [ "$state" = Established ] || [ "${count:-0}" -gt 0 ]; then
			bad="$bad z1->$t($state,$count)"
		fi
	done
	for node in $BLOCKED_NODES; do
		read -r state count <<<"$(peer_state "$node" "$Z1_TRANSPORT")"
		if [ "$state" = Established ] || [ "${count:-0}" -gt 0 ]; then
			bad="$bad $node->z1($state,$count)"
		fi
	done
	[ "$(date +%s)" -ge "$end" ] && break
	sleep "$POLL_SECONDS"
done
if [ -z "$bad" ]; then
	pass "no z1<->r1/m1a MIDR session was ever established during ${OBSERVE_SECONDS}s of sampling"
else
	fail "a blocked MIDR session was established:$bad"
fi
for t in $BLOCKED_TRANSPORTS; do
	printf '%s\n' "$z1_log" | grep -qE "$(session_log "$t")" &&
		fail "z1 requested a session towards blocked $t" ||
		pass "z1 never requested a session towards blocked $t"
done

# r1 and m1a stay healthy with the rest of the lab.
for node in $BLOCKED_NODES; do
	vty "$node" 'show midr neighbors' | grep -q 'Established' &&
		pass "$node still has Established MIDR sessions with other nodes" ||
		fail "$node has no Established MIDR session"
done

########################################################################
# 5. Allowed sessions were created through the session API
########################################################################
anchor="$(printf '%s\n' "$(node_log z1)" | grep 'ANCHOR_PROBE_DONE → 群 1/' | tail -n 1)"
printf '%s\n' "$anchor" | grep -q '共选出 0 个锚点候选' &&
	pass 'z1 CL selected no group-1 anchor (its only candidate r1 is blocked)' ||
	fail "z1 CL anchor selection for group 1 is unexpected: ${anchor:-no ANCHOR log}"

# Group 2 members.
EXPECTED_TRANSPORTS="$(loc 121) $(loc 122)"
deadline=$(( $(date +%s) + WAIT_SECONDS ))
while :; do
	established="$(z1_established_overlays)"
	missing=
	for t in $EXPECTED_TRANSPORTS; do
		printf '%s\n' "$established" | grep -qx "$t" || missing="$missing $t"
	done
	[ -z "$missing" ] && break
	[ "$(date +%s)" -ge "$deadline" ] && break
	sleep "$POLL_SECONDS"
done
[ -z "$missing" ] &&
	pass "z1 is Established with r2 and m2a (group 2)" ||
	fail "z1 is missing expected Established sessions:$missing"

z1_log="$(node_log z1)"
established="$(z1_established_overlays)"
[ -n "$established" ] &&
	pass "z1 has Established MIDR sessions: $(printf '%s' "$established" | tr '\n' ' ')" ||
	fail 'z1 has no Established MIDR session'
via_api=0
for t in $established; do
	case " $BLOCKED_TRANSPORTS " in
		*" $t "*) fail "z1 is Established with blocked $t" ;;
	esac
	if printf '%s\n' "$z1_log" | grep -qE "$(session_log "$t")"; then
		via_api=$((via_api + 1))
		if [ "$STACK" = midrd ]; then
			pass "z1 session to $t was requested via midr_session_connect (native midrd session)"
		else
			vty z1 "show bgp neighbors $t" | grep -qi 'midr' &&
				pass "z1 session to $t was requested via midr_peer_session_request and carries MIDR-LS" ||
				fail "z1 session to $t does not show the MIDR-LS address family"
		fi
	else
		info "z1 session to $t was created by the remote side (passive) or pre-existed"
	fi
done
[ "$via_api" -gt 0 ] &&
	pass "$via_api Established z1 session(s) were created through the session API" ||
	fail 'no Established z1 session was created through the session API'

for node in r2 m2a; do
	docker exec -u root "$(container "$node")" grep -qE \
		'session requested via midr_(peer_session_request|session_(connect|request))' \
		"$MIDR_LOG" 2>/dev/null &&
		pass "$node also creates sessions through the session API" ||
		fail "$node has no session API log"
done

printf '\n----- z1 show midr links -----\n'
vty z1 'show midr links'
printf '\n----- z1 show midr self -----\n'
vty z1 'show midr self'
printf '\n----- z1 show midr neighbors -----\n'
vty z1 'show midr neighbors'
[ -n "$LOG_ROOT" ] && printf '\nNode logs: %s\n' "$LOG_ROOT"

printf 'Tier1 admission result: %d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
