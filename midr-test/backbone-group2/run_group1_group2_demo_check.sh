#!/usr/bin/env bash
# Read-only acceptance checks for the 15-node group1+group2 containerlab.

set -uo pipefail

LAB_PREFIX="${MIDR_LAB_PREFIX:-clab-midr-backbone}"
WAIT_SECONDS="${MIDR_DEMO_WAIT_SECONDS:-600}"
POLL_SECONDS="${MIDR_DEMO_POLL_SECONDS:-10}"
MEMBERS="r1 r2 m1a m1b m2a z1 z2"
BOOTSTRAPS="b1 b2 b3 b4 b5"
TRANSIT="t1 t2 t3"
ALL_NODES="$TRANSIT $BOOTSTRAPS $MEMBERS"

PASS=0
FAIL=0

pass() { printf 'PASS: %s\n' "$1"; PASS=$((PASS + 1)); }
fail() { printf 'FAIL: %s\n' "$1"; FAIL=$((FAIL + 1)); }
container() { printf '%s-%s' "$LAB_PREFIX" "$1"; }
vty() { docker exec "$(container "$1")" vtysh -c "$2" 2>/dev/null; }

number_field()
{
	printf '%s\n' "$1" | awk -F: -v key="$2" '$1 ~ key {gsub(/ /, "", $2); print $2; exit}'
}

container_running()
{
	[ "$(docker inspect -f '{{.State.Running}}' "$(container "$1")" 2>/dev/null)" = true ]
}

node_ready()
{
	local node="$1" self neighbors group2 sync rib lsdb ted gid selected usable

	self="$(vty "$node" 'show midr self')" || return 1
	neighbors="$(vty "$node" 'show midr neighbors')" || return 1
	group2="$(vty "$node" 'show midr group2')" || return 1
	sync="$(vty "$node" 'show midr topology sync')" || return 1
	rib="$(vty "$node" 'show midr rib summary')" || return 1
	lsdb="$(vty "$node" 'show midr lsdb summary')" || return 1
	ted="$(vty "$node" 'show midr ted summary')" || return 1
	gid="$(number_field "$self" '^Group-ID')"
	selected="$(number_field "$rib" '^  selected')"
	usable="$(number_field "$lsdb" '^  usable')"
	[ -n "$gid" ] && [ "$gid" != 0 ] || return 1
	printf '%s\n' "$neighbors" | grep -q 'Established' || return 1
	printf '%s\n' "$group2" | grep -q 'reported=1 pending=0' || return 1
	printf '%s\n' "$group2" | grep -q '回调注册.*已注册' || return 1
	printf '%s\n' "$sync" | grep -q 'state:.*NORMAL' || return 1
	printf '%s\n' "$sync" | grep -q 'provider:.*available' || return 1
	[ "${selected:-0}" -gt 0 ] || return 1
	[ "${usable:-0}" -gt 0 ] || return 1
	printf '%s\n' "$ted" | grep -q 'state:.*READY' || return 1
}

lab_ready()
{
	local node

	for node in $ALL_NODES; do
		container_running "$node" || return 1
	done
	for node in $MEMBERS; do
		node_ready "$node" || return 1
	done
	return 0
}

if ! command -v docker >/dev/null 2>&1; then
	echo 'docker is unavailable'
	exit 2
fi

deadline=$(( $(date +%s) + WAIT_SECONDS ))
printf 'Waiting up to %ss for the group1+group2 lab to converge...\n' "$WAIT_SECONDS"
until lab_ready; do
	if [ "$(date +%s)" -ge "$deadline" ]; then
		echo 'Lab did not reach the stable acceptance state before timeout.'
		exit 3
	fi
	sleep "$POLL_SECONDS"
done

pass 'all 15 containers are running and all MIDR members are ready'

for node in $MEMBERS; do
	group2="$(vty "$node" 'show midr group2')"
	sync="$(vty "$node" 'show midr topology sync')"
	events="$(vty "$node" 'show midr events')"
	rib="$(vty "$node" 'show midr rib summary')"
	lsdb="$(vty "$node" 'show midr lsdb summary')"
	ted="$(vty "$node" 'show midr ted summary')"

	printf '%s\n' "$group2" | grep -q 'pending=0' &&
		pass "$node has no pending node report" || fail "$node has a pending node report"
	printf '%s\n' "$sync" | grep -q 'normal queue:.*0/4096' &&
		printf '%s\n' "$sync" | grep -q 'resync queue:.*0/4096' &&
		pass "$node input queues are empty" || fail "$node input queue is not empty"
	printf '%s\n' "$events" | grep -q 'rejected-full:  *0' &&
		pass "$node has no full-queue rejection" || fail "$node rejected an event because its queue was full"
	selected="$(number_field "$rib" '^  selected')"
	[ "${selected:-0}" -gt 0 ] && pass "$node has selected MIDR RIB paths" ||
		fail "$node has no selected MIDR RIB path"
	usable="$(number_field "$lsdb" '^  usable')"
	[ "${usable:-0}" -gt 0 ] && pass "$node has usable LSDB objects" ||
		fail "$node has no usable LSDB object"
	printf '%s\n' "$ted" | grep -q 'state:.*READY' &&
		pass "$node TED is READY" || fail "$node TED is not READY"
done

for node in $BOOTSTRAPS; do
	group2="$(vty "$node" 'show midr group2')"
	owned="$(vty "$node" 'show midr owned')"
	printf '%s\n' "$group2" | grep -q 'reported=0' &&
		pass "$node suppresses its group-0 node report" ||
		fail "$node reported a group-0 node"
	if printf '%s\n' "$owned" | grep -qE '^  (memberships|links|node prefixes|group prefixes): +[1-9]'; then
		fail "$node originated an owned object while in group 0"
	else
		pass "$node has no owned object while in group 0"
	fi
done

for node in r1 r2 m1a m1b m2a z1 z2; do
	remote="$(vty "$node" 'show midr group2-remote')"
	printf '%s\n' "$remote" | grep -q '两侧一致' &&
		pass "$node remote view matches the first-group node table" ||
		fail "$node remote view differs from the first-group node table"
done

printf 'Result: %d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
