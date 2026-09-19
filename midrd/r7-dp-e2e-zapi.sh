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
		--prefix 10.0.1.1/32 --link 102:5 --takeover-delay 500 \
		--lifetime 3000 "${extra[@]}" "${rt[@]}" \
		--pidfile "$run/a.pid" >"$run/a.log" 2>&1 &
	A_PID=$!
	"$MIDRD_BIN" --node-id 102 --listen "127.0.0.1:$((base + 1))" \
		--peer "127.0.0.1:$base" --peer "127.0.0.1:$((base + 2))" \
		--group 1 --prefix 10.0.2.2/32 --link 101:5 --link 103:7 \
		--takeover-delay 500 --lifetime 3000 "${extra[@]}" "${rt[@]}" \
		--pidfile "$run/b.pid" >"$run/b.log" 2>&1 &
	B_PID=$!
	"$MIDRD_BIN" --node-id 103 --listen "127.0.0.1:$((base + 2))" \
		--peer "127.0.0.1:$((base + 1))" --group 1 \
		--prefix 10.0.3.3/32 --link 102:7 --takeover-delay 500 \
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
fib_has() { ip route show proto "$PROTO" | grep -F -- "$1" >/dev/null 2>&1; }
fib_line_has_nh() {
	ip route show proto "$PROTO" | grep -F -- "$1" | grep -F -- "$2" \
		>/dev/null 2>&1
}
fib_empty() { [[ -z "$(ip route show proto "$PROTO")" ]]; }
fib_gone() { ! fib_has "$1"; }
fib_has_both() { fib_has 10.0.1.1 && fib_has 10.0.2.2; }

dump_logs() {
	local f

	for f in "$@"; do
		echo "--- $f" >&2
		tail -40 "$f" >&2 2>/dev/null || true
	done
}

rm -rf "$RUN"
mkdir -p "$RUN"

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
for p in 10.0.1.1 10.0.2.2 10.0.3.3; do
	if fib_line_has_nh "$p" 127.0.0.1; then
		ok "FIB has $p via 127.0.0.1"
	else
		bad "FIB missing $p via 127.0.0.1"
	fi
done

hdr "3. kernel lookup resolves through proto $PROTO"
for p in 10.0.2.2 10.0.3.3; do
	route_get=$(ip route get "$p" 2>&1 || true)

	echo "$route_get"
	if echo "$route_get" | grep -Fq 127.0.0.1; then
		ok "ip route get $p uses 127.0.0.1"
	else
		bad "ip route get $p did not resolve via 127.0.0.1"
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
	ok "node a replayed after zebra reconnect"
else
	bad "node a did not replay after zebra reconnect"
fi
if wait_until 20 fib_has_both; then
	ok "FIB repopulated with live prefixes after zebra restart"
else
	bad "FIB not repopulated after zebra restart"
	echo "$(fib_all)"
fi

hdr "6. stopping all midrd empties the proto-$PROTO FIB"
kill -TERM "$A_PID" "$B_PID" 2>/dev/null || true
wait "$A_PID" 2>/dev/null || true
wait "$B_PID" 2>/dev/null || true
A_PID= B_PID= PIDS=
if wait_until 20 fib_empty; then
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
if fib_empty; then
	ok "control run left the proto-$PROTO FIB empty"
else
	bad "control run installed routes into the FIB"
	echo "$(fib_all)"
fi

echo
echo "=== summary: PASS=$PASS FAIL=$FAIL (logs: $RUN) ==="
[[ "$FAIL" -eq 0 ]]
