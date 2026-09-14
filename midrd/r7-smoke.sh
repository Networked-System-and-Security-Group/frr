#!/usr/bin/env bash
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BIN=${MIDRD_BIN:-"$ROOT/build/midrd"}
if [[ ! -x "$BIN" ]]; then
	echo "missing midrd binary: $BIN" >&2
	exit 2
fi

run_cluster() {
	local name=$1 family=$2 base_port=$3 owner_runtime=$4 peer_runtime=$5
	local expected_a=$6 expected_b=$7 expected_c=$8
	local node_a=$9 node_b=${10} node_c=${11}
	local prefix_a=${12} prefix_b=${13} prefix_c=${14}
	local run a_listen b_listen c_listen a_peer b_peer c_peer
	local a b c

	run=$(mktemp -d "${TMPDIR:-/tmp}/midrd-r7-${name}.XXXXXX")
	if [[ "$family" == 4 ]]; then
		a_listen="127.0.0.1:${base_port}"
		b_listen="127.0.0.1:$((base_port + 1))"
		c_listen="127.0.0.1:$((base_port + 2))"
	else
		a_listen="[::1]:${base_port}"
		b_listen="[::1]:$((base_port + 1))"
		c_listen="[::1]:$((base_port + 2))"
	fi
	a_peer=$b_listen
	b_peer=$a_listen
	c_peer=$b_listen

	"$BIN" --node-id "$node_a" --listen "$a_listen" --peer "$a_peer" \
		--prefix "$prefix_a" --lifetime 900 --runtime "$owner_runtime" \
		>"$run/a.log" 2>&1 & a=$!
	"$BIN" --node-id "$node_b" --listen "$b_listen" --peer "$b_peer" \
		--peer "$c_listen" --prefix "$prefix_b" --lifetime 900 \
		--runtime "$peer_runtime" >"$run/b.log" 2>&1 & b=$!
	"$BIN" --node-id "$node_c" --listen "$c_listen" --peer "$c_peer" \
		--prefix "$prefix_c" --lifetime 900 --runtime "$peer_runtime" \
		>"$run/c.log" 2>&1 & c=$!

	wait "$a"
	wait "$b"
	wait "$c"

	grep -q "final-objects=$expected_a" "$run/a.log"
	grep -q "final-objects=$expected_b" "$run/b.log"
	grep -q "final-objects=$expected_c" "$run/c.log"
	grep -q 'sync type=6' "$run/b.log"
	grep -q 'sync type=6' "$run/c.log"
	grep -q 'type=2' "$run/b.log"
	grep -q 'type=2' "$run/c.log"
	if [[ "$expected_b" != 3 || "$expected_c" != 3 ]]; then
		grep -q 'event state=2' "$run/b.log"
		grep -q 'event state=2' "$run/c.log"
	fi
	echo "r7 smoke $name: PASS (logs: $run)"
}

run_cluster ipv4-convergence 4 31001 4 4 3 3 3 \
	101 102 103 10.0.1.1/32 10.0.2.2/32 10.0.3.3/32
run_cluster ipv6-convergence 6 32001 4 4 3 3 3 \
	111 112 113 2001:db8:11::1/128 2001:db8:12::1/128 2001:db8:13::1/128
run_cluster ipv4-expiry 4 31101 1 3 3 2 2 \
	121 122 123 10.1.1.1/32 10.1.2.2/32 10.1.3.3/32

echo 'r7 smoke: PASS'
