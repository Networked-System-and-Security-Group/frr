#!/usr/bin/env bash
# Third-group ZAPI/FIB smoke test (runs inside the build container, root).
#
# Verifies the complete chain
#   facade / SPF adapter -> midrd backend -> zclient -> zebra RIB -> Linux FIB
# against a real zebra, using midrd-dp-e2e-tool.  It deliberately does NOT use
# the MIDR session/LS-flooding layer, so the data-plane migration can be
# accepted independently of group 2.
#
# Cases:
#   A. direct facade add/delete of an IPv4 route with a recursive nexthop
#   B. full TED -> SPF -> adapter -> FIB (serve-ted), zebra-restart replay and
#      shutdown withdraw
#   C. ECMP, UCMP and IPv6 adds and their FIB shape
#   D. dedup (dup4) and replacement (replace4)
#   E. deferred-batch failure on a real zebra: the batch is staged while zebra
#      is down, so it fails after the adapter already committed its desired
#      generation; a fresh desired generation arrives during recovery; the
#      reconnect must converge both the FIB and the installed bookkeeping
set -u

ROOT=${FRR_ROOT:-/home/frr/frr-midrd3}
LIB=$ROOT/lib/.libs
TOOL=${DP_TOOL_BIN:-/tmp/midrd-build/midrd-dp-e2e-tool}
ZEBRA=${ZEBRA_BIN:-$ROOT/zebra/.libs/zebra}
D=${MIDRD_FIB_LOG:-/tmp/midrd-dp-fib}
PROTO=199

V4UNDER=192.168.200.1/24
V4NH=192.168.200.2
V4NH2=192.168.200.3
V6UNDER=fd00:200::1/64
V6NH=fd00:200::2
SPFUNDER=192.0.200.1/24
SPFNH=192.0.200.2
SPFPFX=198.51.100.0/24

SOCK=$D/zserv.api
PASS=0
FAIL=0

ok() { echo "[PASS] $*"; PASS=$((PASS + 1)); }
bad() { echo "[FAIL] $*"; FAIL=$((FAIL + 1)); }
hdr() { echo; echo "==== $* ===="; }
# Extract "name=<number>" from a captured tool status line.
field() { echo "$1" | sed -nE "s/.*$2=([0-9-]+).*/\1/p" | tail -1; }

export LD_LIBRARY_PATH=$LIB

cleanup() {
	pkill -9 -f 'midrd-dp-e2e-tool' 2>/dev/null || true
	if [[ -f $D/zebra.pid ]]; then
		kill -9 "$(cat "$D/zebra.pid")" 2>/dev/null || true
		rm -f "$D/zebra.pid"
	fi
}
trap cleanup EXIT

zebra_start() {
	rm -f "$SOCK"
	"$ZEBRA" -u root -g root -f "$D/zebra.conf" -i "$D/zebra.pid" \
		-z "$SOCK" --vty_socket "$D/vty" -d >>"$D/zebra.out" 2>&1
	local i

	for ((i = 0; i < 80; i++)); do
		[[ -S $SOCK ]] && return 0
		sleep 0.1
	done
	return 1
}

zebra_stop() {
	[[ -f $D/zebra.pid ]] || return 0
	kill "$(cat "$D/zebra.pid")" 2>/dev/null || true
	sleep 1
	kill -9 "$(cat "$D/zebra.pid")" 2>/dev/null || true
	rm -f "$D/zebra.pid"
}

fib() {
	ip route show proto "$PROTO" 2>/dev/null | grep -F -- "$1"
	ip -6 route show proto "$PROTO" 2>/dev/null | grep -F -- "$1"
}
fib_has() { fib "$1" >/dev/null 2>&1; }

# zebra may install a route inline ("via N") or through a nexthop group
# ("nhid N"), so resolve both forms before comparing nexthops.
fib_nhid() { fib "$1" | grep -oE 'nhid [0-9]+' | awk '{print $2}' | head -1; }
fib_nh_list() {
	local pfx=$1 nhid

	nhid=$(fib_nhid "$pfx")
	if [[ -n $nhid ]]; then
		ip nexthop show id "$nhid" 2>/dev/null
		ip nexthop show id "$nhid" 2>/dev/null | grep -oE 'group [0-9,/]+' |
			tr ',' '/' | tr -d ' ' | sed 's|group||' | tr '/' '\n' |
			while read -r id; do
				[[ -n $id ]] && ip nexthop show id "$id" 2>/dev/null
			done
	else
		fib "$pfx"
	fi
}
fib_has_nh() { fib "$1" | grep -qF -- "$2" || fib_nh_list "$1" | grep -qF -- "$2"; }
fib_gone() { ! fib_has "$1"; }
fib_count_nh() { fib_nh_list "$1" | grep -c 'via'; }
nh_weights() { ip nexthop show 2>/dev/null | grep -cE 'group [0-9]+,[0-9]+'; }

wait_fib_nh() {
	local pfx=$1 nh=$2 timeout=${3:-15}
	local i

	for ((i = 0; i < timeout * 4; i++)); do
		fib_has_nh "$pfx" "$nh" && return 0
		sleep 0.25
	done
	return 1
}

wait_fib_gone() {
	local pfx=$1 timeout=${2:-15}
	local i

	for ((i = 0; i < timeout * 4; i++)); do
		fib_gone "$pfx" && return 0
		sleep 0.25
	done
	return 1
}

wait_line() {
	local file=$1 pattern=$2 timeout=${3:-15}
	local i

	for ((i = 0; i < timeout * 10; i++)); do
		grep -q "$pattern" "$file" 2>/dev/null && return 0
		sleep 0.1
	done
	return 1
}

rm -rf "$D"
mkdir -p "$D/vty"
cat >"$D/zebra.conf" <<EOF
hostname midr-dp-fib
log stdout
debug zebra kernel
EOF

# Underlay dummies: the MIDR nexthops are recursive (ifindex 0), so they must
# be resolvable through a connected route to reach the kernel FIB.
ip link add midr-dp0 type dummy 2>/dev/null || true
ip link set midr-dp0 up
ip addr add "$V4UNDER" dev midr-dp0 2>/dev/null || true
ip link add midr-spf0 type dummy 2>/dev/null || true
ip link set midr-spf0 up
ip addr add "$SPFUNDER" dev midr-spf0 2>/dev/null || true
ip link add midr-dp6 type dummy 2>/dev/null || true
ip link set midr-dp6 up
ip addr add "$V6UNDER" dev midr-dp6 nodad 2>/dev/null || true

echo "=== third-group ZAPI/FIB smoke (proto $PROTO) ==="
echo "tool=$TOOL zebra=$ZEBRA logs=$D"

hdr "0. zebra on a private ZAPI socket"
if zebra_start; then
	ok "zebra running on $SOCK"
else
	bad "zebra failed to start"
	tail -5 "$D/zebra.out"
	exit 1
fi

hdr "A. direct facade add/delete of an IPv4 route (recursive nexthop)"
"$TOOL" -s "$SOCK" cycle4 10.10.0.0/24 "$V4NH" 6 >"$D/a.out" 2>&1 &
A_PID=$!
if wait_line "$D/a.out" 'action=cycle4-add' 15; then
	ok "facade add accepted"
else
	bad "facade add did not report"
fi
grep -E 'action=' "$D/a.out" | head -3
if wait_fib_nh 10.10.0.0/24 "$V4NH" 10; then
	ok "FIB: 10.10.0.0/24 via $V4NH (proto $PROTO)"
else
	bad "FIB missing 10.10.0.0/24 via $V4NH"
	fib 10.10.0.0
fi
if ip route get 10.10.0.1 2>/dev/null | grep -q midr-dp0; then
	ok "10.10.0.1 resolves over the midr-dp0 underlay"
else
	bad "10.10.0.1 does not resolve over the underlay"
	ip route get 10.10.0.1 2>&1 | head -2
fi
wait $A_PID 2>/dev/null || true
if wait_fib_gone 10.10.0.0/24 10; then
	ok "client exit withdrew the route"
else
	bad "route survived the tool exit"
	fib 10.10.0.0
fi

hdr "B. TED -> SPF -> adapter -> FIB, zebra restart replay, shutdown withdraw"
"$TOOL" -s "$SOCK" -t 40 serve-ted 40 >"$D/b.out" 2>&1 &
B_PID=$!
if wait_line "$D/b.out" 'action=serve-ted-add' 15; then
	ok "synthetic committed TED applied (SPF -> adapter -> backend)"
else
	bad "serve-ted did not start"
fi
if wait_fib_nh "$SPFPFX" "$SPFNH" 20; then
	ok "FIB: $SPFPFX via $SPFNH (SPF-derived, proto $PROTO)"
else
	bad "SPF-derived route missing from the FIB"
	fib "$SPFPFX"
fi
zebra_stop
if zebra_start; then
	ok "zebra restarted"
else
	bad "zebra restart failed"
fi
if wait_fib_nh "$SPFPFX" "$SPFNH" 25; then
	ok "reconnect replay restored the route after the zebra restart"
else
	bad "replay did not restore the route"
	fib "$SPFPFX"
fi
grep -oE 'action=serve-ted-tick[^ ]*' "$D/b.out" | tail -1
kill -TERM "$B_PID" 2>/dev/null || true
wait "$B_PID" 2>/dev/null || true
if wait_fib_gone "$SPFPFX" 10; then
	ok "SIGTERM withdrew the SPF route (unregister + flush)"
else
	bad "SPF route survived the shutdown"
	fib "$SPFPFX"
fi
grep -E 'installed=' "$D/b.out" | tail -2

hdr "C. ECMP, UCMP and IPv6 shapes"
"$TOOL" -s "$SOCK" -t 6 add4-ecmp 10.20.0.0/24 "$V4NH" "$V4NH2" >"$D/c1.out" 2>&1 &
C1=$!
wait_line "$D/c1.out" 'action=add4-ecmp' 15
if [[ $(fib_count_nh 10.20.0.0/24) == "2" ]]; then
	ok "ECMP: FIB has 2 nexthops for 10.20.0.0/24"
else
	bad "ECMP: expected 2 nexthops, got $(fib_count_nh 10.20.0.0/24)"
	fib 10.20.0.0
fi
wait $C1 2>/dev/null || true

"$TOOL" -s "$SOCK" -t 6 add4-ucmp 10.30.0.0/24 "$V4NH:3" "$V4NH2:1" >"$D/c2.out" 2>&1 &
C2=$!
wait_line "$D/c2.out" 'action=add4-ucmp' 15
if fib 10.30.0.0/24 | grep -q weight || [[ $(nh_weights) -ge 1 ]]; then
	ok "UCMP: weighted nexthop group installed"
else
	bad "UCMP: no weighted nexthop group"
	fib 10.30.0.0
	ip nexthop show 2>&1 | tail -4
fi
wait $C2 2>/dev/null || true

"$TOOL" -s "$SOCK" -t 6 add6 fd00:10::/64 "$V6NH" >"$D/c3.out" 2>&1 &
C3=$!
wait_line "$D/c3.out" 'action=add6' 15
if wait_fib_nh fd00:10::/64 "$V6NH" 10; then
	ok "IPv6: FIB has fd00:10::/64 via $V6NH"
else
	bad "IPv6: route missing from the FIB"
	fib fd00:10::
fi
wait $C3 2>/dev/null || true

hdr "D. dedup and replacement"
"$TOOL" -s "$SOCK" dup4 10.40.0.0/24 "$V4NH" >"$D/d1.out" 2>&1
grep -E 'DP dup4' "$D/d1.out"
first=$(sed -nE 's/.*adds_first=([0-9]+).*/\1/p' "$D/d1.out" | tail -1)
second=$(sed -nE 's/.*adds_second=([0-9]+).*/\1/p' "$D/d1.out" | tail -1)
if [[ -n $first && $first == "$second" ]]; then
	ok "dedup: identical add was not re-sent (adds=$first)"
else
	bad "dedup: the second add was sent ($first -> $second)"
fi

"$TOOL" -s "$SOCK" -t 6 replace4 10.40.0.0/24 "$V4NH" "$V4NH2" >"$D/d2.out" 2>&1 &
D2=$!
wait_line "$D/d2.out" 'action=replace4-new' 15
if wait_fib_nh 10.40.0.0/24 "$V4NH2" 10 &&
	! fib 10.40.0.0/24 | grep -qF "$V4NH "; then
	ok "replace: FIB switched to $V4NH2 (old nexthop gone)"
else
	bad "replace: FIB did not switch nexthop"
	fib 10.40.0.0
fi
wait $D2 2>/dev/null || true

hdr "E. deferred-batch failure on a real zebra + FIB (fail the batch, reconnect, converge)"
"$TOOL" -s "$SOCK" -t 30 interleave-ted 4 >"$D/e.out" 2>&1 &
E_PID=$!
if wait_line "$D/e.out" 'ACTION interleave-ready' 20; then
	ok "seed result applied against the live zebra"
else
	bad "the interleave seed did not complete"
fi
if wait_fib_nh 198.51.100.0/24 "$SPFNH" 15; then
	ok "FIB: seed route 198.51.100.0/24 via $SPFNH"
else
	bad "seed route missing from the FIB"
	fib 198.51.100.0
fi

# Inject the interleave: kill zebra so that the batch staged next is submitted
# while the socket is gone.  update_deferred() still reports success, so the
# adapter advances its desired generation and the deferred batch timer then
# fails -- the exact sequence review section 5 asks to be covered on a real
# zebra/FIB rather than only against the fake zclient.
zebra_stop
if wait_line "$D/e.out" 'ACTION interleave-batch' 45; then
	ok "deferred batch failed after the desired generation was committed"
else
	bad "the injected deferred-batch failure was not observed"
fi
wait_line "$D/e.out" 'ACTION interleave-newdesired' 45

seed_line=$(grep 'DP action=interleave-seed' "$D/e.out" | tail -1)
batch_line=$(grep 'DP action=interleave-batch' "$D/e.out" | tail -1)
new_line=$(grep 'DP action=interleave-newdesired' "$D/e.out" | tail -1)
seed_gen=$(field "$seed_line" desired_gen)
batch_gen=$(field "$batch_line" desired_gen)
if [[ -n $seed_gen && -n $batch_gen && $batch_gen -gt $seed_gen ]]; then
	ok "control plane advanced its desired generation ($seed_gen -> $batch_gen) with the FIB behind"
else
	bad "desired generation did not advance across the failed batch ($seed_gen -> $batch_gen)"
fi
fails=$(field "$batch_line" fails)
pending=$(field "$batch_line" pending)
recovering=$(field "$batch_line" recovering)
# The failing batch carries count-1 adds (the seed prefix is already
# installed), and all of them must survive as retained recovery work.
if [[ ${fails:-0} -ge 1 && ${pending:-0} -ge 2 && ${recovering:-0} == 1 ]]; then
	ok "failed batch retained as recovery work (fails=$fails pending=$pending recovering=$recovering)"
else
	bad "the failed batch was not retained (fails=$fails pending=$pending recovering=$recovering)"
fi
new_pending=$(field "$new_line" pending)
if [[ -n $new_pending && -n $pending && $new_pending -gt $pending ]]; then
	ok "a fresh desired generation during recovery joined the retained set ($pending -> $new_pending)"
else
	bad "the fresh desired update was lost during recovery ($pending -> $new_pending)"
fi

if zebra_start; then
	ok "zebra restarted after the injected failure"
else
	bad "zebra restart failed"
fi
if wait_line "$D/e.out" 'ACTION interleave-converged' 90; then
	ok "data plane converged after the reconnect (batch drained, recovery idle)"
else
	bad "data plane did not converge after the reconnect"
	wait_line "$D/e.out" 'ACTION interleave-timeout' 5
fi

missing=0
for i in 0 1 2 3 4; do
	pfx="198.51.$((100 + i)).0/24"
	wait_fib_nh "$pfx" "$SPFNH" 20 || {
		missing=$((missing + 1))
		fib "$pfx"
	}
done
if [[ $missing -eq 0 ]]; then
	ok "FIB holds all 5 SPF prefixes after the failure/reconnect interleave"
else
	bad "FIB is missing $missing of the 5 expected prefixes"
fi

final_line=$(grep 'DP action=interleave-final' "$D/e.out" | tail -1)
installed=$(field "$final_line" installed)
final_pending=$(field "$final_line" pending)
final_recovering=$(field "$final_line" recovering)
resyncs=$(field "$final_line" resyncs)
replays=$(field "$final_line" replays)
if [[ ${installed:-0} == 5 && ${final_pending:-0} == 0 && ${final_recovering:-0} == 0 ]]; then
	ok "final consistency: installed=5 pending=0 recovering=0 (resyncs=$resyncs replays=$replays)"
else
	bad "final state inconsistent: installed=$installed pending=$final_pending recovering=$final_recovering"
fi
grep -E 'DP action=interleave-' "$D/e.out"

kill -TERM "$E_PID" 2>/dev/null || true
wait "$E_PID" 2>/dev/null || true
if wait_fib_gone 198.51.100.0/24 15; then
	ok "tool exit withdrew the routes after the interleave"
else
	bad "routes survived the tool exit"
fi

echo
echo "=== summary: PASS=$PASS FAIL=$FAIL ==="
if [[ $FAIL -eq 0 ]]; then
	echo "third-group ZAPI/FIB smoke: PASS"
	exit 0
fi
echo "third-group ZAPI/FIB smoke: FAIL"
exit 1
