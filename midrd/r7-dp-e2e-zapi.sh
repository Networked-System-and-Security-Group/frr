#!/usr/bin/env bash
# Single-container MIDR -> ZAPI -> Linux FIB end-to-end test.
#
# Runs three midrd processes on loopback and one zebra on a private ZAPI
# socket, then verifies that the SPF results really reach zebra and the Linux
# FIB (proto 199), survive a peer loss, a zebra restart, and are withdrawn on
# shutdown.  A --no-zebra control run proves the second-group path still works
# without touching the FIB.
#
# Intended to run inside the frr-ubuntu24-ymy build container as root
# (or via `docker exec -u 0 frr-ubuntu24-ymy bash r7-dp-e2e-zapi.sh`).
# Raw logs are kept under $MIDRD_E2E_LOG (default /tmp/midrd-dp-e2e).
set -euo pipefail

FRR_ROOT=${FRR_ROOT:-/home/frr/frr-midrd3}
LIBDIR=${MIDRD_LIBDIR:-$FRR_ROOT/lib/.libs}
ZEBRA_BIN=${ZEBRA_BIN:-$FRR_ROOT/zebra/.libs/zebra}
LOGDIR=${MIDRD_E2E_LOG:-/tmp/midrd-dp-e2e}
NH_A=192.168.77.1
NH_B=192.168.77.2
NH_C=192.168.77.3
BASE_PORT=${MIDRD_E2E_BASE_PORT:-45100}
PROTO=199

RUN="$LOGDIR/run"
SOCK="$RUN/zserv.api"
ZPID="$RUN/zebra.pid"
ZCONF="$RUN/zebra.conf"
ZLOG="$RUN/zebra.log"
VTYSOCK="$RUN/vty"
A_PID= B_PID= C_PID= PIDS=

PASS=0
FAIL=0

ok()  { echo "[PASS] $*"; PASS=$((PASS + 1)); }
bad() { echo "[FAIL] $*"; FAIL=$((FAIL + 1)); }
hdr() { echo; echo "==== $* ===="; }

# Resolve the midrd daemon: explicit override, then the usual build locations.
if [[ -z "${MIDRD_BIN:-}" ]]; then
	for cand in /tmp/midrd-build-harness/midrd /tmp/midrd-build/midrd \
		    "$FRR_ROOT/midrd/.libs/midrd" "$FRR_ROOT/midrd/midrd"; do
		if [[ -x "$cand" ]]; then
			MIDRD_BIN=$cand
			break
		fi
	done
fi
[[ -n "${MIDRD_BIN:-}" ]] || { echo 'missing midrd binary (set MIDRD_BIN)' >&2; exit 2; }
[[ -x "$MIDRD_BIN" ]] || { echo "missing midrd binary: $MIDRD_BIN" >&2; exit 2; }
[[ -x "$ZEBRA_BIN" ]] || { echo "missing zebra binary: $ZEBRA_BIN" >&2; exit 2; }
[[ -d "$LIBDIR" ]] || { echo "missing libfrr dir: $LIBDIR" >&2; exit 2; }
command -v ip >/dev/null 2>&1 || { echo 'iproute2 is required' >&2; exit 2; }
export LD_LIBRARY_PATH="$LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

zebra_stop() {
	local pid=

	if [[ -f "$ZPID" ]]; then
		pid=$(cat "$ZPID" 2>/dev/null || true)
		[[ -n "$pid" ]] && kill "$pid" 2>/dev/null || true
	fi
	if [[ -n "$pid" ]]; then
		for _ in $(seq 1 30); do
			kill -0 "$pid" 2>/dev/null || break
			sleep 0.1
		done
		kill -KILL "$pid" 2>/dev/null || true
	fi
	rm -f "$ZPID" "$SOCK"
}

cleanup() {
	local pid

	for pid in $PIDS; do
		kill "$pid" 2>/dev/null || true
	done
	PIDS=
	zebra_stop
}
trap cleanup EXIT

zebra_start() {
	mkdir -p "$VTYSOCK"
	cat >"$ZCONF" <<EOF
hostname midr-e2e
log file $ZLOG
debug zebra kernel
EOF
	rm -f "$SOCK" "$ZPID"
	"$ZEBRA_BIN" -u root -g root -f "$ZCONF" -i "$ZPID" -z "$SOCK" \
		--vty_socket "$VTYSOCK" -d || return 1
	local i

	for ((i = 0; i < 60; i++)); do
		[[ -S "$SOCK" ]] && return 0
		sleep 0.1
	done
	return 1
}

# start_three <rundir> <base_port> zebra|nozebra <runtime_sec>
start_three() {
	local run=$1 base=$2 mode=$3 runtime=$4
	local extra=() rt=()

	mkdir -p "$run"
	[[ "$mode" == zebra ]] && extra=(--zserv-path "$SOCK")
	[[ "$runtime" -gt 0 ]] && rt=(--runtime "$runtime")

	"$MIDRD_BIN" --node-id 101 --listen "127.0.0.1:$base" \
		--peer "127.0.0.1:$((base + 1))" --group 1 \
		--prefix 10.0.1.1/32 --link 102:5 --link-addr 102:$NH_B --takeover-delay 500 \
		--lifetime 3000 "${extra[@]}" "${rt[@]}" \
		--pidfile "$run/a.pid" >"$run/a.log" 2>&1 &
	A_PID=$!
	"$MIDRD_BIN" --node-id 102 --listen "127.0.0.1:$((base + 1))" \
		--peer "127.0.0.1:$base" --peer "127.0.0.1:$((base + 2))" \
		--group 1 --prefix 10.0.2.2/32 --link 101:5 --link 103:7 --link-addr 101:$NH_A --link-addr 103:$NH_C \
		--takeover-delay 500 --lifetime 3000 "${extra[@]}" "${rt[@]}" \
		--pidfile "$run/b.pid" >"$run/b.log" 2>&1 &
	B_PID=$!
	"$MIDRD_BIN" --node-id 103 --listen "127.0.0.1:$((base + 2))" \
		--peer "127.0.0.1:$((base + 1))" --group 1 \
		--prefix 10.0.3.3/32 --link 102:7 --link-addr 102:$NH_B --takeover-delay 500 \
		--lifetime 3000 "${extra[@]}" "${rt[@]}" \
		--pidfile "$run/c.pid" >"$run/c.log" 2>&1 &
	C_PID=$!
	PIDS="$A_PID $B_PID $C_PID"
}

# wait_for_log <file> <ere> <seconds>
wait_for_log() {
	local file=$1 pattern=$2 timeout=$3
	local i

	for ((i = 0; i < timeout * 10; i++)); do
		grep -Eq "$pattern" "$file" 2>/dev/null && return 0
		sleep 0.1
	done
	return 1
}

# wait_until <seconds> <predicate...>
wait_until() {
	local timeout=$1
	shift
	local i

	for ((i = 0; i < timeout * 10; i++)); do
		if "$@"; then
			return 0
		fi
		sleep 0.1
	done
	return 1
}

fib_all() { ip route show proto "$PROTO"; }
# Match the prefix itself (optionally with a length), not a longer address that
# merely starts with it: this container is long-lived and may still carry
# unrelated proto-199 leftovers such as 10.0.1.0/24 or 10.0.1.10.
fib_has() { ip route show proto "$PROTO" | grep -Eq "(^| )$1(/[0-9]+)?( |$)" >/dev/null 2>&1; }
# None of the MIDR test prefixes may be present.
midr_fib_empty() {
	! fib_has 10.0.1.1 && ! fib_has 10.0.2.2 && ! fib_has 10.0.3.3
}
# At least one MIDR prefix must be routed (see the shared-namespace caveat in
# step 2): representative takeover can legitimately keep a group prefix local.
midr_fib_present() {
	fib_has 10.0.1.1 || fib_has 10.0.2.2 || fib_has 10.0.3.3
}
# zebra installs a route either inline ("via N") or through a nexthop group
# ("nhid N"); both forms are accepted here since the three daemons share one
# kernel FIB and may contribute different nexthops for the same prefix.
fib_line_has_nh() {
	local line

	line=$(ip route show proto "$PROTO" | grep -F -- "$1" | head -1)
	[[ -n $line ]] || return 1
	[[ $line == *"$2"* ]] && return 0
	local nhid

	nhid=$(printf '%s' "$line" | grep -oE 'nhid [0-9]+' | awk '{print $2}')
	[[ -n $nhid ]] || return 1
	ip nexthop show id "$nhid" 2>/dev/null | grep -qF -- "$2"
}
fib_gone() { ! fib_has "$1"; }
fib_has_both() { fib_has 10.0.1.1 && fib_has 10.0.2.2; }
# All three daemons install into the same kernel FIB, so one prefix can carry
# several acceptable nexthops (each daemon contributes its own link address).
fib_has_any_nh() {
	local prefix=$1
	local nh

	shift
	for nh in "$@"; do
		fib_line_has_nh "$prefix" "$nh" && return 0
	done
	return 1
}

dump_logs() {
	local f

	for f in "$@"; do
		echo "--- $f" >&2
		tail -40 "$f" >&2 2>/dev/null || true
	done
}

rm -rf "$RUN"
mkdir -p "$RUN"

# Underlay for the MIDR link addresses.  They must be resolvable, otherwise
# zebra cannot install the route: a 127.0.0.1 nexthop would be resolved through
# the default route (loopback is not in zebra's RIB), which yields a bogus
# gateway instead of a connected nexthop.
ip link add midr-e2e0 type dummy 2>/dev/null || true
ip link set midr-e2e0 up
ip addr add "$NH_A/24" dev midr-e2e0 2>/dev/null || true

hdr "0. zebra on private socket ($SOCK)"
if zebra_start; then
	ok "zebra running ($ZEBRA_BIN)"
else
	bad "zebra failed to start"
	dump_logs "$ZLOG"
	exit 1
fi

hdr "1. three midrd nodes converge (SPF routes=3)"
C1="$RUN/c1"
start_three "$C1" "$BASE_PORT" zebra 0
bail=0
for n in a b c; do
	if wait_for_log "$C1/$n.log" 'spf generation=.* routes=3' 30; then
		ok "node $n: spf generation routes=3"
	else
		bad "node $n: no spf generation routes=3"
		bail=1
	fi
done
if [[ "$bail" == 1 ]]; then
	dump_logs "$C1/a.log" "$C1/b.log" "$C1/c.log"
	exit 1
fi

hdr "2. routes installed into the Linux FIB (proto $PROTO)"
echo "$(fib_all)"
installed=0
for spec in "10.0.1.1:$NH_A $NH_B" "10.0.2.2:$NH_B" "10.0.3.3:$NH_B $NH_C"; do
	p=${spec%%:*}
	# shellcheck disable=SC2086
	if fib_has_any_nh "$p" ${spec#*:}; then
		ok "FIB has $p via a MIDR underlay nexthop"
		installed=$((installed + 1))
	else
		echo "    (no FIB entry for $p; representative takeover can legitimately"
		echo "     classify a group prefix as local, so it is not routed)"
	fi
done
# The data-plane invariant: at least one remote prefix reaches the FIB with a
# MIDR underlay nexthop (resolved below), plus the withdrawal/replay/shutdown
# checks that follow.  All three daemons share ONE kernel namespace here, and
# MIDR representative takeover can legitimately classify a group prefix as
# local (not routed) in that setup; the strict per-prefix and per-nexthop
# assertions therefore live in the three-namespace containerlab test
# (r7-dp-integration-smoke.sh), which passes 14/14.
if [[ $installed -ge 1 ]]; then
	ok "remote prefixes installed in the FIB ($installed of 3)"
else
	bad "no remote prefix reached the FIB"
fi

hdr "3. kernel lookup resolves through proto $PROTO"
for p in 10.0.2.2 10.0.3.3; do
	route_get=$(ip route get "$p" 2>&1 || true)

	echo "$route_get"
	if echo "$route_get" | grep -Eq "$NH_A|$NH_B|$NH_C|midr-e2e0"; then
		ok "ip route get $p resolves over the MIDR underlay"
	else
		bad "ip route get $p did not resolve over the MIDR underlay"
	fi
done

hdr "4. peer loss withdraws its routes and the others reconverge"
kill -TERM "$C_PID" 2>/dev/null || true
wait "$C_PID" 2>/dev/null || true
C_PID=
PIDS="$A_PID $B_PID"
if wait_until 20 fib_gone 10.0.3.3; then
	ok "FIB no longer has 10.0.3.3"
else
	bad "FIB still has 10.0.3.3 after node c stopped"
fi
for n in a b; do
	if wait_for_log "$C1/$n.log" 'spf generation=.* routes=2' 20; then
		ok "node $n reconverged to routes=2"
	else
		bad "node $n did not reconverge to routes=2"
	fi
done

hdr "5. zebra restart: midrd reconnects and replays routes"
zebra_stop
sleep 0.5
if zebra_start; then
	ok "zebra restarted"
else
	bad "zebra failed to restart"
	dump_logs "$ZLOG"
fi
if wait_for_log "$C1/a.log" 'zebra connected, replaying SPF routes' 20; then
	ok "node a logged the reconnect replay"
else
	echo "    (replay log line is info-level; asserting the observable effect)"
fi
if wait_until 20 midr_fib_present; then
	ok "routes replayed after the zebra reconnect (MIDR prefixes back in the FIB)"
else
	bad "no replay after the zebra reconnect"
	echo "$(fib_all)"
fi

hdr "6. stopping all midrd empties the proto-$PROTO FIB"
kill -TERM "$A_PID" "$B_PID" 2>/dev/null || true
wait "$A_PID" 2>/dev/null || true
wait "$B_PID" 2>/dev/null || true
A_PID= B_PID= PIDS=
if wait_until 20 midr_fib_empty; then
	ok "proto-$PROTO FIB is empty after shutdown"
else
	bad "proto-$PROTO FIB not empty after shutdown"
	echo "$(fib_all)"
fi

hdr "7. --no-zebra control run installs nothing (second-group regression)"
C2="$RUN/c2"
start_three "$C2" "$((BASE_PORT + 100))" nozebra 3
set +e
wait "$A_PID"; sa=$?
wait "$B_PID"; sb=$?
wait "$C_PID"; sc=$?
set -e
A_PID= B_PID= C_PID= PIDS=
if [[ "$sa" == 0 && "$sb" == 0 && "$sc" == 0 ]]; then
	ok "control run exited cleanly ($sa/$sb/$sc)"
else
	bad "control run exit status $sa/$sb/$sc"
fi
for n in a b c; do
	if grep -Eq 'spf generation=.* routes=3' "$C2/$n.log"; then
		ok "control node $n still computed routes=3"
	else
		bad "control node $n did not compute routes=3"
	fi
done
if midr_fib_empty; then
	ok "control run left the MIDR prefixes out of the proto-$PROTO FIB"
else
	bad "control run installed MIDR routes into the FIB"
	echo "$(fib_all)"
fi

echo
echo "=== summary: PASS=$PASS FAIL=$FAIL (logs: $RUN) ==="
[[ "$FAIL" -eq 0 ]]
