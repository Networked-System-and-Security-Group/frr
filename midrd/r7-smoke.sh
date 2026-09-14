#!/usr/bin/env bash
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BIN=${MIDRD_BIN:-"$ROOT/build/midrd"}
IMAGE=${MIDRD_IMAGE:-frr-midr-p6:f9beedd0d653}

if [[ ! -x "$BIN" ]]; then
	echo "missing midrd binary: $BIN (run: make -C midrd test)" >&2
	exit 2
fi
command -v containerlab >/dev/null 2>&1 || {
	echo 'containerlab is required for the R7 smoke' >&2
	exit 2
}

run_cluster() {
	local name=$1 family=$2 base_port=$3 owner_runtime=$4 peer_runtime=$5
	local expected_a=$6 expected_b=$7 expected_c=$8
	local node_a=$9 node_b=${10} node_c=${11}
	local prefix_a=${12} prefix_b=${13} prefix_c=${14}
	local run topo lab a_addr b_a_addr b_c_addr c_addr
	local a_listen b_listen c_listen a_peer b_peer b_to_c_peer c_peer
	local n container all i

	run=$(mktemp -d "${TMPDIR:-/tmp}/midrd-r7-${name}.XXXXXX")
	topo="$run/topology.clab.yaml"
	lab="midr-r7-${name}"
	if [[ "$family" == 4 ]]; then
		a_addr=10.77.1.1/30
		b_a_addr=10.77.1.2/30
		b_c_addr=10.77.2.1/30
		c_addr=10.77.2.2/30
		a_listen=10.77.1.1:$base_port
		b_listen=0.0.0.0:$((base_port + 1))
		c_listen=10.77.2.2:$((base_port + 2))
		a_peer=10.77.1.2:$((base_port + 1))
		b_peer=10.77.1.1:$base_port
		b_to_c_peer=10.77.2.2:$((base_port + 2))
		c_peer=10.77.2.1:$((base_port + 1))
	else
		a_addr='2001:db8:77:1::1/64 nodad'
		b_a_addr='2001:db8:77:1::2/64 nodad'
		b_c_addr='2001:db8:77:2::1/64 nodad'
		c_addr='2001:db8:77:2::2/64 nodad'
		a_listen="[2001:db8:77:1::1]:$base_port"
		b_listen="[::]:$((base_port + 1))"
		c_listen="[2001:db8:77:2::2]:$((base_port + 2))"
		a_peer="[2001:db8:77:1::2]:$((base_port + 1))"
		b_peer="[2001:db8:77:1::1]:$base_port"
		b_to_c_peer="[2001:db8:77:2::2]:$((base_port + 2))"
		c_peer="[2001:db8:77:2::1]:$((base_port + 1))"
	fi

	cat >"$topo" <<EOF
name: $lab
topology:
  nodes:
    a:
      kind: linux
      image: $IMAGE
      binds:
        - $BIN:/usr/local/bin/midrd:ro
      cmd: sleep infinity
      exec:
        - ip addr add $a_addr dev eth1
        - >-
          sh -lc 'exec /usr/local/bin/midrd --node-id $node_a --listen $a_listen
          --peer $a_peer --prefix $prefix_a --lifetime 900
          --runtime $owner_runtime >/tmp/midrd.log 2>&1 &'
    b:
      kind: linux
      image: $IMAGE
      binds:
        - $BIN:/usr/local/bin/midrd:ro
      cmd: sleep infinity
      exec:
        - ip addr add $b_a_addr dev eth1
        - ip addr add $b_c_addr dev eth2
        - >-
          sh -lc 'exec /usr/local/bin/midrd --node-id $node_b --listen $b_listen
          --peer $b_peer --peer $b_to_c_peer --prefix $prefix_b --lifetime 900
          --runtime $peer_runtime >/tmp/midrd.log 2>&1 &'
    c:
      kind: linux
      image: $IMAGE
      binds:
        - $BIN:/usr/local/bin/midrd:ro
      cmd: sleep infinity
      exec:
        - ip addr add $c_addr dev eth1
        - >-
          sh -lc 'exec /usr/local/bin/midrd --node-id $node_c --listen $c_listen
          --peer $c_peer --prefix $prefix_c --lifetime 900
          --runtime $peer_runtime >/tmp/midrd.log 2>&1 &'
  links:
    - endpoints: ["a:eth1", "b:eth1"]
    - endpoints: ["b:eth2", "c:eth1"]
EOF

	cleanup_case() {
		containerlab destroy --topo "$topo" --cleanup >/dev/null 2>&1 || true
	}
	trap cleanup_case RETURN
	containerlab deploy --topo "$topo" --reconfigure >/dev/null
	for i in $(seq 1 120); do
		all=1
		for n in a b c; do
			container="clab-${lab}-${n}"
			if ! docker exec "$container" grep -q 'midrd node=.* final-objects=' /tmp/midrd.log 2>/dev/null; then
				all=0
				break
			fi
		done
		[[ "$all" == 1 ]] && break
		sleep 0.1
	done
	if [[ "$all" != 1 ]]; then
		echo "timeout waiting for $name" >&2
		for n in a b c; do
			docker exec "clab-${lab}-${n}" tail -40 /tmp/midrd.log 2>&1 || true
		done
		return 1
	fi
	for n in a b c; do
		docker exec "clab-${lab}-${n}" cat /tmp/midrd.log >"$run/$n.log"
	done
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
	echo "r7 containerlab smoke $name: PASS (logs: $run)"
}

run_cluster ipv4-convergence 4 31001 4 4 3 3 3 \
	101 102 103 10.0.1.1/32 10.0.2.2/32 10.0.3.3/32
run_cluster ipv6-convergence 6 32001 4 4 3 3 3 \
	111 112 113 2001:db8:11::1/128 2001:db8:12::1/128 2001:db8:13::1/128
run_cluster ipv4-expiry 4 31101 1 3 3 2 2 \
	121 122 123 10.1.1.1/32 10.1.2.2/32 10.1.3.3/32

echo 'r7 containerlab smoke: PASS'
