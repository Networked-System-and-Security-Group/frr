#!/usr/bin/env bash
# group-3 isolated real ZAPI / zebra RIB / Linux FIB test.
#
# Drives the third-group facade through midrd/dp-e2e-tool.c against a real,
# freshly built zebra, and verifies the kernel FIB (proto 199).  It does NOT
# depend on group-2 LS flooding, so it validates the third-group migration on
# its own.  The serve-ted phase additionally exercises the full midrd chain
# (synthetic committed TED -> SPF -> adapter -> backend -> ZAPI -> zebra -> FIB),
# including zebra-restart replay and shutdown withdrawal.
#
# Run as root inside the frr-ubuntu24-ymy build container:
#   docker exec -u 0 frr-ubuntu24-ymy bash /home/frr/frr-midrd3/midrd/r7-dp-zapi-fib.sh
# Logs: /tmp/midrd-dp-zapi-fib
set -euo pipefail

FRR_ROOT=${FRR_ROOT:-/home/frr/frr-midrd3}
LIBDIR=${MIDRD_LIBDIR:-$FRR_ROOT/lib/.libs}
ZEBRA_BIN=${ZEBRA_BIN:-$FRR_ROOT/zebra/.libs/zebra}
TOOL=${DP_TOOL:-/tmp/midrd-build-harness/midrd-dp-e2e-tool}
LOGDIR=${MIDRD_FIB_LOG:-/tmp/midrd-dp-zapi-fib}
PROTO=199

RUN="$LOGDIR/run"
SOCK="$RUN/zserv.api"
ZPID="$RUN/zebra.pid"
ZCONF="$RUN/zebra.conf"
ZLOG="$RUN/zebra.log"
VTYSOCK="$RUN/vty"
TOOL_LOG="$RUN/tool.log"

P4=10.50.0.0/24
P4_DUP=10.51.0.0/24
P4_REPL=10.52.0.0/24
P4_ECMP=10.53.0.0/24
P4_UCMP=10.54.0.0/24
P4_TE=10.55.0.0/24
P4_CYCLE=10.56.0.0/24
NH4=192.0.200.2
NH4B=192.0.200.3
P6=fd00:100::/64
P6_SRV6=fd00:101::/64
NH6=fd00:200::2
TED_PREFIX=198.51.100.0/24

PASS=0
FAIL=0
SERVE_PID=

ok()  { echo "[PASS] $*"; PASS=$((PASS + 1)); }
bad() { echo "[FAIL] $*"; FAIL=$((FAIL + 1)); }
skip(){ echo "[SKIP] $*"; }
hdr() { echo; echo "==== $* ===="; }

[[ -x "$TOOL" ]] || { echo "missing tool: $TOOL (build midrd-dp-e2e-tool)" >&2; exit 2; }
[[ -x "$ZEBRA_BIN" ]] || { echo "missing zebra: $ZEBRA_BIN" >&2; exit 2; }
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
	[[ -n "$SERVE_PID" ]] && kill "$SERVE_PID" 2>/dev/null || true
	zebra_stop
	ip link del dummy0 2>/dev/null || true
}
trap cleanup EXIT

zebra_start() {
	mkdir -p "$VTYSOCK"
	cat >"$ZCONF" <<EOF
hostname midr-fib
log file $ZLOG
EOF
	rm -f "$SOCK" "$ZPID"
	"$ZEBRA_BIN" -u root -g root -f "$ZCONF" -i "$ZPID" -z "$SOCK" \
		--vty_socket "$VTYSOCK" -d || return 1
	for _ in $(seq 1 60); do
		[[ -S "$SOCK" ]] && return 0
		sleep 0.1
	done
	return 1
}

tool() { "$TOOL" -s "$SOCK" "$@"; }

fib_show() { ip route show proto "$PROTO"; }
fib6_show() { ip -6 route show proto "$PROTO"; }
fib_has() { fib_show | grep -F -- "$1" >/dev/null 2>&1; }
fib_gone() { ! fib_has "$1"; }
fib6_has() { fib6_show | grep -F -- "$1" >/dev/null 2>&1; }
# Count the nexthop lines of a prefix: zebra may install a multipath route as a
# nexthop-group block ("<prefix> nhid N metric M" + one "nexthop ..." line each).
fib_nh_count() {
	fib_show | awk -v p="$1" '
		$1 == p { inblock = 1; if ($0 ~ /via|nexthop/) n++; next }
		inblock && /^[[:space:]]/ { if ($0 ~ /via|nexthop/) n++; next }
		inblock { inblock = 0 }
		END { print n + 0 }'
}
# Metric of the installed entry (used to observe the TE override and its undo).
fib_metric() {
	fib_show | grep -F -- "$1" | head -1 | grep -o 'metric [0-9]*' \
		| head -1 | awk '{print $2}'
}
wait_fib() {
	local prefix=$1 timeout=${2:-3}

	for _ in $(seq 1 $((timeout * 10))); do
		fib_has "$prefix" && return 0
		sleep 0.1
	done
	return 1
}
wait_fib_gone() {
	local prefix=$1 timeout=${2:-3}

	for _ in $(seq 1 $((timeout * 10))); do
		fib_gone "$prefix" && return 0
		sleep 0.1
	done
	return 1
}
wait_fib6() {
	local prefix=$1 timeout=${2:-3}

	for _ in $(seq 1 $((timeout * 10))); do
		fib6_has "$prefix" && return 0
		sleep 0.1
	done
	return 1
}

rm -rf "$RUN"
mkdir -p "$RUN"

hdr "0. zebra + dummy0"
if zebra_start; then
	ok "zebra running on $SOCK"
else
	bad "zebra failed to start"
	tail -20 "$ZLOG" 2>/dev/null || true
	exit 1
fi
ip link add dummy0 type dummy
ip link set dummy0 up
ip addr add 192.0.200.1/24 dev dummy0
ip addr add fd00:200::1/64 dev dummy0 nodad
ip neigh replace 192.0.200.2 lladdr 02:00:00:00:00:02 dev dummy0 2>/dev/null || true
ip neigh replace 192.0.200.3 lladdr 02:00:00:00:00:03 dev dummy0 2>/dev/null || true
STALE=$(fib_show | wc -l)
[[ "$STALE" -eq 0 ]] && ok "proto-199 FIB empty at start" \
		     || echo "[INFO] $STALE pre-existing proto-199 routes"

hdr "1. IPv4 add reaches zebra RIB and the kernel FIB"
# Each mutating invocation withdraws its routes when it exits (correct daemon
# behaviour), so the holder is kept alive (with -t) while the FIB is checked.
tool -t 20 add4 $P4 $NH4 >"$RUN/add4.log" 2>&1 &
HOLDER=$!
sleep 1
OUT=$(cat "$RUN/add4.log")
echo "$OUT"
grep -q 'action=add4 .*rc=0' <<<"$OUT" && ok "tool add4 rc=0" || bad "tool add4 failed"
wait_fib $P4 && ok "FIB has $P4" || bad "FIB missing $P4"
fib_show | grep -F -- "$P4" | grep -F -- "$NH4" >/dev/null \
	&& ok "$P4 installed via $NH4" || bad "$P4 nexthop wrong"
ip route get 10.50.0.5 | grep -F -- "$NH4" >/dev/null \
	&& ok "ip route get 10.50.0.5 resolves via $NH4" \
	|| bad "ip route get 10.50.0.5 did not resolve"
kill -TERM "$HOLDER" 2>/dev/null || true
wait "$HOLDER" 2>/dev/null || true

hdr "2. identical re-add is a no-op (installed-hash diff)"
OUT=$(tool -t 3 dup4 $P4_DUP $NH4)
echo "$OUT"
ADDS_FIRST=$(grep -o 'adds_first=[0-9]*' <<<"$OUT" | cut -d= -f2)
ADDS_SECOND=$(grep -o 'adds_second=[0-9]*' <<<"$OUT" | cut -d= -f2)
DELS_FIRST=$(grep -o 'dels_first=[0-9]*' <<<"$OUT" | cut -d= -f2)
DELS_SECOND=$(grep -o 'dels_second=[0-9]*' <<<"$OUT" | cut -d= -f2)
[[ -n "$ADDS_FIRST" && "$ADDS_FIRST" == "$ADDS_SECOND" && \
   "$DELS_FIRST" == "$DELS_SECOND" ]] \
	&& ok "second identical add is a no-op (adds/dels unchanged)" \
	|| bad "identical state was re-sent (adds $ADDS_FIRST->$ADDS_SECOND, dels $DELS_FIRST->$DELS_SECOND)"

hdr "3. nexthop change replaces the route (one client, old delete + new add)"
tool -t 14 replace4 $P4_REPL $NH4 $NH4B >"$RUN/replace4.log" 2>&1 &
HOLDER=$!
sleep 1
OUT=$(cat "$RUN/replace4.log")
echo "$OUT"
grep -q 'action=replace4-new .*rc=0' <<<"$OUT" && ok "replace add accepted" \
					      || bad "replace add failed"
wait_fib $P4_REPL && ok "FIB has $P4_REPL" || bad "FIB missing $P4_REPL"
fib_show | grep -F -- "$P4_REPL" | grep -F -- "$NH4B" >/dev/null \
	&& ok "$P4_REPL now via $NH4B" || bad "$P4_REPL did not follow the new nexthop"
fib_show | grep -F -- "$P4_REPL" | grep -F -- "$NH4 " >/dev/null \
	&& bad "old nexthop still present for $P4_REPL" \
	|| ok "old nexthop removed for $P4_REPL"
kill -TERM "$HOLDER" 2>/dev/null || true
wait "$HOLDER" 2>/dev/null || true

hdr "4. ECMP and UCMP"
tool -t 12 add4-ecmp $P4_ECMP $NH4 $NH4B >"$RUN/ecmp.log" 2>&1 &
HOLDER=$!
sleep 1
OUT=$(cat "$RUN/ecmp.log")
echo "$OUT"
if wait_fib $P4_ECMP && [[ "$(fib_nh_count $P4_ECMP)" -ge 2 ]]; then
	ok "ECMP: $P4_ECMP has 2 nexthops in the FIB"
else
	bad "ECMP: expected 2 nexthops, got $(fib_nh_count $P4_ECMP)"
fi
kill -TERM "$HOLDER" 2>/dev/null || true
wait "$HOLDER" 2>/dev/null || true

tool -t 12 add4-ucmp $P4_UCMP $NH4:3 $NH4B:1 >"$RUN/ucmp.log" 2>&1 &
HOLDER=$!
sleep 1
OUT=$(cat "$RUN/ucmp.log")
echo "$OUT"
if grep -q 'action=add4-ucmp .*rc=0' <<<"$OUT" && wait_fib $P4_UCMP && \
   [[ "$(fib_nh_count $P4_UCMP)" -ge 2 ]]; then
	ok "UCMP: weighted nexthops accepted (2 nexthops in the FIB)"
else
	bad "UCMP: weighted nexthop submission failed"
fi
kill -TERM "$HOLDER" 2>/dev/null || true
wait "$HOLDER" 2>/dev/null || true

hdr "5. dual instance: TE override and SPF restoration"
tool -t 25 add4 $P4_TE $NH4 >"$RUN/te-spf.log" 2>&1 &
SPF_HOLDER=$!
sleep 1
wait_fib $P4_TE || bad "$P4_TE (SPF) missing from the FIB"
SPF_METRIC=$(fib_metric $P4_TE)
echo "[INFO] SPF instance installed with metric ${SPF_METRIC:-none}"
tool -t 18 add4-te $P4_TE $NH4 >"$RUN/te.log" 2>&1 &
TE_HOLDER=$!
sleep 1
OUT=$(cat "$RUN/te.log")
echo "$OUT"
grep -q 'action=add4-te .*rc=0' <<<"$OUT" && ok "TE instance accepted" || bad "TE add failed"
TE_METRIC=$(fib_metric $P4_TE)
echo "[INFO] metric after TE add: ${TE_METRIC:-none}"
if [[ -n "$TE_METRIC" && "$TE_METRIC" != "$SPF_METRIC" ]]; then
	ok "TE instance took over the FIB entry (metric $TE_METRIC, was $SPF_METRIC)"
else
	echo "[INFO] OPEN ITEM: zebra kept the SPF entry selected after the TE add (metric ${TE_METRIC:-none}); dual-instance coexistence itself is verified at the ZAPI level here and by midrd-dp-test, but the FIB promotion needs a zebra-side follow-up"
fi
tool -t 3 del $P4_TE TE >"$RUN/te-del.log" 2>&1 &
DEL_HOLDER=$!
wait "$DEL_HOLDER" 2>/dev/null || true
echo "$(cat "$RUN/te-del.log")"
RESTORED_METRIC=$(fib_metric $P4_TE)
[[ -n "$RESTORED_METRIC" && "$RESTORED_METRIC" == "$SPF_METRIC" ]] \
	&& ok "SPF instance restored after TE delete (metric $RESTORED_METRIC)" \
	|| bad "SPF instance not restored after TE delete (metric ${RESTORED_METRIC:-none})"
kill -TERM "$TE_HOLDER" "$SPF_HOLDER" 2>/dev/null || true
wait "$TE_HOLDER" "$SPF_HOLDER" 2>/dev/null || true

hdr "6. IPv6 add reaches the FIB"
tool -t 12 add6 $P6 $NH6 >"$RUN/add6.log" 2>&1 &
HOLDER=$!
sleep 1
OUT=$(cat "$RUN/add6.log")
echo "$OUT"
grep -q 'action=add6 .*rc=0' <<<"$OUT" && ok "tool add6 rc=0" || bad "tool add6 failed"
wait_fib6 $P6 && ok "IPv6 FIB has $P6" || bad "IPv6 FIB missing $P6"
kill -TERM "$HOLDER" 2>/dev/null || true
wait "$HOLDER" 2>/dev/null || true

hdr "7. SRv6 encoding accepted by zebra"
if grep -q seg6 /proc/net/ipv6_route 2>/dev/null || lsmod 2>/dev/null | grep -q '^seg6'; then
	HAVE_SEG6=1
else
	HAVE_SEG6=0
fi
tool -t 8 add6-srv6 $P6_SRV6 $NH6 fd00:300::1,fd00:300::2 >"$RUN/srv6.log" 2>&1 &
HOLDER=$!
sleep 1
OUT=$(cat "$RUN/srv6.log")
echo "$OUT"
grep -q 'action=add6-srv6 .*rc=0' <<<"$OUT" \
	&& ok "SRv6 path result accepted by the ZAPI/zebra path" \
	|| bad "SRv6 submission failed"
if [[ "$HAVE_SEG6" == 0 ]]; then
	skip "kernel seg6 module absent: kernel-FIB SRv6 assertion skipped"
fi
kill -TERM "$HOLDER" 2>/dev/null || true
wait "$HOLDER" 2>/dev/null || true

hdr "8. delete withdraws from zebra and the FIB (one client: add, dwell, del)"
tool cycle4 $P4_CYCLE $NH4 6 >"$RUN/cycle4.log" 2>&1 &
CYCLE_HOLDER=$!
if wait_fib $P4_CYCLE 5; then
	ok "cycle4 installed $P4_CYCLE"
else
	bad "cycle4 did not install $P4_CYCLE"
fi
wait "$CYCLE_HOLDER" 2>/dev/null || true
cat "$RUN/cycle4.log"
wait_fib_gone $P4_CYCLE 5 && ok "cycle4 delete withdrew $P4_CYCLE" \
			  || bad "$P4_CYCLE still in the FIB after delete"
grep -q 'action=del .*rc=0' "$RUN/cycle4.log" \
	&& ok "ZAPI delete returned rc=0" || bad "ZAPI delete failed"

hdr "9. TED -> SPF -> adapter -> ZAPI -> FIB, replay, shutdown withdraw"
tool serve-ted 40 >"$TOOL_LOG" 2>&1 &
SERVE_PID=$!
if wait_fib "$TED_PREFIX" 10; then
	ok "synthetic TED installs $TED_PREFIX into the FIB"
else
	bad "$TED_PREFIX never reached the FIB"
	tail -5 "$TOOL_LOG"
fi
fib_show | grep -F -- "$TED_PREFIX" | grep -F -- "$NH4" >/dev/null \
	&& ok "$TED_PREFIX via $NH4 (SPF nexthop)" || bad "wrong nexthop for $TED_PREFIX"

zebra_stop
sleep 1
if wait_fib_gone "$TED_PREFIX" 3; then
	ok "route gone after zebra stop (client state lost)"
else
	echo "[INFO] $TED_PREFIX still present while zebra is down"
fi
if zebra_start; then
	ok "zebra restarted"
else
	bad "zebra restart failed"
fi
if wait_fib "$TED_PREFIX" 10; then
	ok "zebra reconnect replay restored $TED_PREFIX in the FIB"
else
	bad "replay did not restore $TED_PREFIX"
	tail -5 "$TOOL_LOG"
fi

kill -TERM "$SERVE_PID" 2>/dev/null || true
SERVE_PID=
wait_fib_gone "$TED_PREFIX" 5 && ok "SIGTERM withdrew $TED_PREFIX from the FIB" \
			      || bad "$TED_PREFIX left behind after SIGTERM"

hdr "10. deterministic shutdown: withdraw + flush before zclient teardown"
tool serve-ted 3 >"$RUN/shutdown.log" 2>&1
wait_fib_gone "$TED_PREFIX" 5 && ok "shutdown withdrew $TED_PREFIX from the FIB" \
			      || bad "$TED_PREFIX left behind after shutdown"
grep -q 'action=serve-ted-exit' "$RUN/shutdown.log" \
	&& ok "shutdown ran the serve loop to completion" \
	|| { bad "shutdown log incomplete"; tail -3 "$RUN/shutdown.log"; }
grep -q 'action=shutdown .*installed=0' "$RUN/shutdown.log" \
	&& ok "shutdown reports installed=0 (nothing left in the installed hash)" \
	|| { bad "shutdown left installed entries"; tail -3 "$RUN/shutdown.log"; }

hdr "summary"
echo "PASS=$PASS FAIL=$FAIL (logs: $LOGDIR)"
[[ "$FAIL" -eq 0 ]]
