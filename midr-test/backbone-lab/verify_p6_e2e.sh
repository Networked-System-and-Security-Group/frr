#!/usr/bin/env bash
# P6 end-to-end acceptance: Group 1 -> MIDR -> TED -> SPF -> Zebra/FIB.
set -uo pipefail

LAB_PREFIX=${MIDR_LAB_PREFIX:-clab-midr-backbone-p6}
[ -n "$LAB_PREFIX" ] || LAB_PREFIX=clab-midr-backbone-p6
MEMBERS="r1 r2 m1a m1b m2a z1 z2"
ALL_NODES="b1 b2 b3 b4 b5 t1 t2 t3 $MEMBERS"
SERVICE_PREFIXES="198.18.0.111/32 198.18.0.112/32 198.18.0.113/32 198.18.0.121/32 198.18.0.122/32 198.18.0.191/32 198.18.0.192/32"
PASS=0
FAIL=0

pass() { printf 'PASS: %s\n' "$1"; PASS=$((PASS + 1)); }
fail() { printf 'FAIL: %s\n' "$1"; FAIL=$((FAIL + 1)); }
container() { printf '%s-%s' "$LAB_PREFIX" "$1"; }
vty() { docker exec "$(container "$1")" vtysh -c "$2" 2>/dev/null; }
field() { awk -F: -v key="$2" '$1 ~ key {gsub(/[[:space:]]/, "", $2); print $2; exit}' <<<"$1"; }
running() { [ "$(docker inspect -f '{{.State.Running}}' "$(container "$1")" 2>/dev/null)" = true ]; }

service_prefix()
{
	case "$1" in
		r1) printf '198.18.0.111/32' ;;
		m1a) printf '198.18.0.112/32' ;;
		m1b) printf '198.18.0.113/32' ;;
		r2) printf '198.18.0.121/32' ;;
		m2a) printf '198.18.0.122/32' ;;
		z1) printf '198.18.0.191/32' ;;
		z2) printf '198.18.0.192/32' ;;
		*) return 1 ;;
	esac
}

member_router_id()
{
	case "$1" in
		r1) printf '10.0.0.111' ;;
		m1a) printf '10.0.0.112' ;;
		m1b) printf '10.0.0.113' ;;
		r2) printf '10.0.0.121' ;;
		m2a) printf '10.0.0.122' ;;
		z1) printf '10.0.0.191' ;;
		z2) printf '10.0.0.192' ;;
		*) return 1 ;;
	esac
}

membership_sequence()
{
	local node="$1" origin="$2"

	vty "$node" 'show midr rib paths' | awk -v origin="$origin" '
		/object=MEMBERSHIP/ && index($0, "origin=" origin " ") {
			for (i = 1; i <= NF; i++)
				if ($i ~ /^sequence=/) {
					split($i, value, "=")
					print value[2]
					exit
				}
		}
	'
}

check_membership_sequence()
{
	local owner="$1" origin expected actual node
	local deadline=$((SECONDS + 30))
	local converged

	origin="$(member_router_id "$owner")" || {
		fail "$owner has no configured router ID"; return
	}
	while [ "$SECONDS" -lt "$deadline" ]; do
		expected="$(membership_sequence "$owner" "$origin")"
		converged=yes
		if [ -z "$expected" ]; then
			converged=no
		else
			for node in $MEMBERS; do
				actual="$(membership_sequence "$node" "$origin")"
				if [ "$actual" != "$expected" ]; then
					converged=no
					break
				fi
			done
		fi
		if [ "$converged" = yes ]; then
			pass "$owner Membership sequence $expected converged on all MIDR nodes"
			return
		fi
		sleep 1
	done
	fail "$owner latest Membership sequence did not converge on all MIDR nodes"
}

spf_has_route()
{
	local routes="$1" prefix="$2"

	awk '
		$1 == "route" && /reachable=yes/ && /local=no/ && /nexthops=[1-9]/ {
			for (i = 1; i <= NF; i++)
				if ($i == "prefix=" prefix)
					found = 1
		}
		END { exit found ? 0 : 1 }
	' prefix="$prefix" <<<"$routes"
}

check_node()
{
	local node="$1" spf routes local_prefix prefix zebra fib count checked=0
	spf="$(vty "$node" 'show midr spf summary')"
	routes="$(vty "$node" 'show midr spf routes')"
	if ! grep -qE '^  state:[[:space:]]+READY$' <<<"$spf"; then
		fail "$node SPF is not READY"; return
	fi
	if ! grep -qE '^  recompute pending:[[:space:]]+no$' <<<"$spf" ||
		! grep -qE '^  last error:[[:space:]]+0$' <<<"$spf"; then
		fail "$node SPF still has pending work or an error"; return
	fi
	count="$(field "$spf" '^  routes')"
	if [ -z "$count" ] || [ "$count" -le 0 ] 2>/dev/null; then
		fail "$node SPF has no route result"; return
	fi
	local_prefix="$(service_prefix "$node")" || {
		fail "$node has no configured service prefix"; return
	}
	for prefix in $SERVICE_PREFIXES; do
		[ "$prefix" = "$local_prefix" ] && continue
		if ! spf_has_route "$routes" "$prefix"; then
			fail "$node has no reachable non-local SPF route for $prefix"; return
		fi
		zebra="$(vty "$node" "show ip route $prefix")"
		if ! grep -q 'Known via "midr"' <<<"$zebra" ||
			! grep -q 'Status: Installed' <<<"$zebra" ||
			! grep -qE '(^|[[:space:]])\*[[:space:]]+[0-9A-Fa-f:.]+' <<<"$zebra"; then
			fail "$node SPF route $prefix is not installed/selected in Zebra"
			printf '%s\n' "$zebra" | sed -n '1,24p'
			return
		fi
		fib="$(docker exec "$(container "$node")" ip -4 route show "$prefix" proto 199 2>/dev/null)"
		if [ -z "$fib" ]; then
			fail "$node SPF route $prefix is absent from Linux proto 199 FIB"; return
		fi
		checked=$((checked + 1))
	done
	[ "$checked" -eq 6 ] || {
		fail "$node checked $checked remote service prefixes instead of 6"; return
	}
	pass "$node: SPF READY -> Zebra installed -> Linux proto 199 for 6 remote service prefixes"
}

for node in $ALL_NODES; do
	running "$node" || fail "$node container is not running"
done
if [ "$FAIL" -ne 0 ]; then
	printf 'P6 result: %d passed, %d failed\n' "$PASS" "$FAIL"; exit 1
fi
for node in $MEMBERS; do check_node "$node"; done
for node in $MEMBERS; do
	check_membership_sequence "$node"
done
printf 'P6 result: %d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
