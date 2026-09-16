#!/usr/bin/env bash
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BIN=${MIDRD_BIN:-"$ROOT/build/midrd"}
BASE_PORT=${MIDRD_SMOKE_BASE_PORT:-44100}
RUNTIME=${MIDRD_SMOKE_RUNTIME:-4}
PIDS=

[[ -x "$BIN" ]] || {
	echo "missing midrd binary: $BIN" >&2
	exit 2
}

cleanup() {
	for pid in $PIDS; do
		kill "$pid" 2>/dev/null || true
	done
}
trap cleanup EXIT

run_cluster() {
	local name=$1 family=$2 base=$3
	local run a_listen b_listen c_listen a_peer b_peer b_to_c c_peer
	local prefix_a prefix_b prefix_c a b c sa sb sc failed=0

	run=$(mktemp -d "${TMPDIR:-/tmp}/midrd-native-${name}.XXXXXX")
	if [[ "$family" == 4 ]]; then
		a_listen="127.0.0.1:$((base + 1))"
		b_listen="127.0.0.1:$((base + 2))"
		c_listen="127.0.0.1:$((base + 3))"
		a_peer=$b_listen
		b_peer=$a_listen
		b_to_c=$c_listen
		c_peer=$b_listen
		prefix_a=10.0.1.1/32
		prefix_b=10.0.2.2/32
		prefix_c=10.0.3.3/32
	else
		a_listen="[::1]:$((base + 1))"
		b_listen="[::1]:$((base + 2))"
		c_listen="[::1]:$((base + 3))"
		a_peer=$b_listen
		b_peer=$a_listen
		b_to_c=$c_listen
		c_peer=$b_listen
		prefix_a=2001:db8:11::1/128
		prefix_b=2001:db8:12::1/128
		prefix_c=2001:db8:13::1/128
	fi

	"$BIN" --node-id 101 --listen "$a_listen" --peer "$a_peer" \
		--group 1 --prefix "$prefix_a" --link 102:5 \
		--takeover-delay 500 --lifetime 3000 --runtime "$RUNTIME" \
		>"$run/a.log" 2>&1 &
	a=$!
	"$BIN" --node-id 102 --listen "$b_listen" --peer "$b_peer" \
		--peer "$b_to_c" --group 1 --prefix "$prefix_b" \
		--link 101:5 --link 103:7 --takeover-delay 500 --lifetime 3000 \
		--runtime "$RUNTIME" >"$run/b.log" 2>&1 &
	b=$!
	"$BIN" --node-id 103 --listen "$c_listen" --peer "$c_peer" \
		--group 1 --prefix "$prefix_c" --link 102:7 \
		--takeover-delay 500 --lifetime 3000 --runtime "$RUNTIME" \
		>"$run/c.log" 2>&1 &
	c=$!
	PIDS="$a $b $c"

	set +e
	wait "$a"; sa=$?
	wait "$b"; sb=$?
	wait "$c"; sc=$?
	set -e
	PIDS=
	if [[ "$sa" -ne 0 || "$sb" -ne 0 || "$sc" -ne 0 ]]; then
		failed=1
	fi
	for node in a b c; do
		grep -q 'spf generation=.* routes=3' "$run/$node.log" || failed=1
		grep -q 'sync type=6' "$run/$node.log" || failed=1
		grep -Eq 'shutdown=(COMPLETE|DEGRADED)' "$run/$node.log" || failed=1
		grep -q 'final-objects=' "$run/$node.log" || failed=1
	done
	if [[ "$failed" -ne 0 ]]; then
		echo "r7 native smoke $name: FAIL (status $sa/$sb/$sc, logs: $run)" >&2
		for node in a b c; do
			echo "--- $node" >&2
			tail -40 "$run/$node.log" >&2
		done
		return 1
	fi
	echo "r7 native smoke $name: PASS (logs: $run)"
}

run_cluster ipv4 4 "$BASE_PORT"
run_cluster ipv6 6 "$((BASE_PORT + 100))"
echo 'r7 native dual-stack smoke: PASS'
