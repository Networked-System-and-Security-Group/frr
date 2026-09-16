#!/usr/bin/env bash
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BIN=${MIDRD_BIN:-"$ROOT/build/midrd"}
SCALE_BIN=${MIDRD_SCALE_BIN:-"$ROOT/build/midrd-scale-test"}
IMAGE=${MIDRD_IMAGE:-frr-midr-p6:f9beedd0d653}
RUN_ROOT=${MIDRD_HARDENING_RUN_ROOT:-}
ACTIVE_TOPO=

[[ -x "$BIN" ]] || {
	echo "missing midrd binary: $BIN" >&2
	exit 2
}
[[ -x "$SCALE_BIN" ]] || {
	echo "missing midrd scale binary: $SCALE_BIN" >&2
	exit 2
}
command -v containerlab >/dev/null 2>&1 || {
	echo 'containerlab is required for the R7 hardening smoke' >&2
	exit 2
}
command -v docker >/dev/null 2>&1 || {
	echo 'docker is required for the R7 hardening smoke' >&2
	exit 2
}

make_run_dir() {
	local name=$1

	if [[ -n "$RUN_ROOT" ]]; then
		mkdir -p "$RUN_ROOT/$name"
		printf '%s\n' "$RUN_ROOT/$name"
	else
		mktemp -d "${TMPDIR:-/tmp}/midrd-r7-hardening-${name}.XXXXXX"
	fi
}

destroy_lab() {
	local topo=$1

	containerlab destroy --topo "$topo" --cleanup >/dev/null 2>&1 || true
}

cleanup_active() {
	if [[ -n "$ACTIVE_TOPO" ]]; then
		destroy_lab "$ACTIVE_TOPO"
	fi
}
trap cleanup_active EXIT

wait_log_count() {
	local container=$1 pattern=$2 expected=$3 attempts=${4:-200}
	local count i

	for i in $(seq 1 "$attempts"); do
		count=$(docker exec "$container" sh -lc \
			"grep -Ec '$pattern' /tmp/midrd.log 2>/dev/null || true")
		if [[ "$count" -ge "$expected" ]]; then
			return 0
		fi
		sleep 0.1
	done
	return 1
}

collect_logs() {
	local lab=$1 run=$2
	local node

	for node in a b c; do
		docker exec "clab-${lab}-${node}" cat /tmp/midrd.log \
			>"$run/$node.log" 2>&1 || true
	done
}

fail_case() {
	local lab=$1 run=$2 message=$3
	local node

	collect_logs "$lab" "$run"
	echo "$message (logs: $run)" >&2
	for node in a b c; do
		echo "--- $node" >&2
		tail -60 "$run/$node.log" >&2 || true
	done
	return 0
}

run_triangle() {
	local run topo lab

	run=$(make_run_dir triangle)
	topo="$run/topology.clab.yaml"
	lab="midr-r7-hard-triangle-$$"
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
        - ip addr add 10.78.1.1/30 dev eth1
        - ip addr add 10.78.3.1/30 dev eth2
        - >-
          sh -lc 'exec stdbuf -oL -eL /usr/local/bin/midrd --node-id 201
          --listen 0.0.0.0:33101 --peer 10.78.1.2:33102
          --peer 10.78.3.2:33103 --group 1 --prefix 10.20.1.1/32
          --link 202:5 --link 203:20 --takeover-delay 1500
          --lifetime 6000 --runtime 8 >/tmp/midrd.log 2>&1 &'
    b:
      kind: linux
      image: $IMAGE
      binds:
        - $BIN:/usr/local/bin/midrd:ro
      cmd: sleep infinity
      exec:
        - ip addr add 10.78.1.2/30 dev eth1
        - ip addr add 10.78.2.1/30 dev eth2
        - >-
          sh -lc 'exec stdbuf -oL -eL /usr/local/bin/midrd --node-id 202
          --listen 0.0.0.0:33102 --peer 10.78.1.1:33101
          --peer 10.78.2.2:33103 --group 1 --prefix 10.20.2.2/32
          --link 201:5 --link 203:7 --takeover-delay 1500
          --lifetime 6000 --runtime 8 >/tmp/midrd.log 2>&1 &'
    c:
      kind: linux
      image: $IMAGE
      binds:
        - $BIN:/usr/local/bin/midrd:ro
      cmd: sleep infinity
      exec:
        - ip addr add 10.78.2.2/30 dev eth1
        - ip addr add 10.78.3.2/30 dev eth2
        - >-
          sh -lc 'exec stdbuf -oL -eL /usr/local/bin/midrd --node-id 203
          --listen 0.0.0.0:33103 --peer 10.78.2.1:33102
          --peer 10.78.3.1:33101 --group 1 --prefix 10.20.3.3/32
          --link 202:7 --link 201:20 --takeover-delay 1500
          --lifetime 6000 --runtime 8 >/tmp/midrd.log 2>&1 &'
  links:
    - endpoints: ["a:eth1", "b:eth1"]
    - endpoints: ["b:eth2", "c:eth1"]
    - endpoints: ["a:eth2", "c:eth2"]
EOF
	containerlab deploy --topo "$topo" --reconfigure >/dev/null
	ACTIVE_TOPO=$topo
	if ! wait_log_count "clab-${lab}-a" \
		'route originator=203 metric=12 reachable=1' 1 160 ||
	   ! wait_log_count "clab-${lab}-c" \
		'route originator=201 metric=12 reachable=1' 1 160; then
		fail_case "$lab" "$run" 'triangle did not prefer the 5+7 path'
		return 1
	fi
	collect_logs "$lab" "$run"
	grep -q 'spf generation=.* routes=3' "$run/a.log"
	grep -q 'spf generation=.* routes=3' "$run/b.log"
	grep -q 'spf generation=.* routes=3' "$run/c.log"
	destroy_lab "$topo"
	ACTIVE_TOPO=
	echo "r7 hardening triangle: PASS (logs: $run)"
}

run_partition_recovery() {
	local run topo lab a b c before_b before_c after_b after_c
	local before_close_b before_close_c before_low_a before_low_c

	run=$(make_run_dir partition-recovery)
	topo="$run/topology.clab.yaml"
	lab="midr-r7-hard-partition-$$"
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
        - ip addr add 10.79.1.1/30 dev eth1
        - >-
          sh -lc 'exec stdbuf -oL -eL /usr/local/bin/midrd --node-id 211
          --listen 10.79.1.1:33201 --peer 10.79.1.2:33202
          --group 1 --prefix 10.21.1.1/32 --link 212:5
          --takeover-delay 1000 --lifetime 10000 --hold-time 2000
          --runtime 14 >/tmp/midrd.log 2>&1 &'
    b:
      kind: linux
      image: $IMAGE
      binds:
        - $BIN:/usr/local/bin/midrd:ro
      cmd: sleep infinity
      exec:
        - ip addr add 10.79.1.2/30 dev eth1
        - ip addr add 10.79.2.1/30 dev eth2
        - >-
          sh -lc 'exec stdbuf -oL -eL /usr/local/bin/midrd --node-id 212
          --listen 0.0.0.0:33202 --peer 10.79.1.1:33201
          --peer 10.79.2.2:33203 --group 1 --prefix 10.21.2.2/32
          --link 211:5 --link 213:7 --takeover-delay 1000
          --lifetime 10000 --hold-time 2000 --runtime 14
          >/tmp/midrd.log 2>&1 &'
    c:
      kind: linux
      image: $IMAGE
      binds:
        - $BIN:/usr/local/bin/midrd:ro
      cmd: sleep infinity
      exec:
        - ip addr add 10.79.2.2/30 dev eth1
        - >-
          sh -lc 'exec stdbuf -oL -eL /usr/local/bin/midrd --node-id 213
          --listen 10.79.2.2:33203 --peer 10.79.2.1:33202
          --group 1 --prefix 10.21.3.3/32 --link 212:7
          --takeover-delay 1000 --lifetime 10000 --hold-time 2000
          --runtime 14 >/tmp/midrd.log 2>&1 &'
  links:
    - endpoints: ["a:eth1", "b:eth1"]
    - endpoints: ["b:eth2", "c:eth1"]
EOF
	containerlab deploy --topo "$topo" --reconfigure >/dev/null
	ACTIVE_TOPO=$topo
	a="clab-${lab}-a"
	b="clab-${lab}-b"
	c="clab-${lab}-c"
	if ! wait_log_count "$a" 'route originator=213 metric=12 reachable=1' 1 140 ||
	   ! wait_log_count "$c" 'route originator=211 metric=12 reachable=1' 1 140; then
		fail_case "$lab" "$run" 'partition baseline did not converge'
		return 1
	fi
	before_b=$(docker exec "$b" sh -lc \
		"grep -c 'sync type=6' /tmp/midrd.log 2>/dev/null || true")
	before_c=$(docker exec "$c" sh -lc \
		"grep -c 'sync type=6' /tmp/midrd.log 2>/dev/null || true")
	before_close_b=$(docker exec "$b" sh -lc \
		"grep -c 'closed family=' /tmp/midrd.log 2>/dev/null || true")
	before_close_c=$(docker exec "$c" sh -lc \
		"grep -c 'closed family=' /tmp/midrd.log 2>/dev/null || true")
	before_low_a=$(docker exec "$a" sh -lc \
		"grep -Ec 'spf generation=.* routes=[012]$' /tmp/midrd.log 2>/dev/null || true")
	before_low_c=$(docker exec "$c" sh -lc \
		"grep -Ec 'spf generation=.* routes=[012]$' /tmp/midrd.log 2>/dev/null || true")
	containerlab tools netem set --topo "$topo" --node "$b" \
		--interface eth2 --loss 100 >/dev/null
	containerlab tools netem set --topo "$topo" --node "$c" \
		--interface eth1 --loss 100 >/dev/null
	sleep 2.5
	if ! wait_log_count "$b" 'closed family=' "$((before_close_b + 1))" 30 ||
	   ! wait_log_count "$c" 'closed family=' "$((before_close_c + 1))" 30; then
		fail_case "$lab" "$run" 'partition did not cross the configured hold timer'
		return 1
	fi
	if [[ $(docker exec "$a" sh -lc \
		"grep -Ec 'spf generation=.* routes=[012]$' /tmp/midrd.log 2>/dev/null || true") \
		-ne "$before_low_a" ]] ||
	   [[ $(docker exec "$c" sh -lc \
		"grep -Ec 'spf generation=.* routes=[012]$' /tmp/midrd.log 2>/dev/null || true") \
		-ne "$before_low_c" ]]; then
		fail_case "$lab" "$run" 'partition replaced the old consistent route view'
		return 1
	fi
	containerlab tools netem reset --topo "$topo" --node "$b" \
		--interface eth2 >/dev/null
	containerlab tools netem reset --topo "$topo" --node "$c" \
		--interface eth1 >/dev/null
	after_b=$((before_b + 1))
	after_c=$((before_c + 1))
	if ! wait_log_count "$b" 'sync type=6' "$after_b" 100 ||
	   ! wait_log_count "$c" 'sync type=6' "$after_c" 100; then
		fail_case "$lab" "$run" 'partition recovery did not complete snapshot/EoR'
		return 1
	fi
	collect_logs "$lab" "$run"
	grep -q 'route originator=213 metric=12 reachable=1' "$run/a.log"
	grep -q 'route originator=211 metric=12 reachable=1' "$run/c.log"
	destroy_lab "$topo"
	ACTIVE_TOPO=
	echo "r7 hardening partition recovery: PASS (logs: $run)"
}

run_ipv6_partition_recovery() {
	local run topo lab a b c before_b before_c after_b after_c
	local before_close_b before_close_c before_low_a before_low_c

	run=$(make_run_dir ipv6-partition-recovery)
	topo="$run/topology.clab.yaml"
	lab="midr-r7-hard-ipv6-partition-$$"
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
        - ip -6 addr add 2001:db8:79:1::1/64 nodad dev eth1
        - >-
          sh -lc 'exec stdbuf -oL -eL /usr/local/bin/midrd --node-id 231
          --listen [2001:db8:79:1::1]:33501
          --peer [2001:db8:79:1::2]:33502 --group 1
          --prefix 2001:db8:21::1/128 --link 232:5
          --takeover-delay 1000 --lifetime 10000 --hold-time 2000
          --runtime 14 >/tmp/midrd.log 2>&1 &'
    b:
      kind: linux
      image: $IMAGE
      binds:
        - $BIN:/usr/local/bin/midrd:ro
      cmd: sleep infinity
      exec:
        - ip -6 addr add 2001:db8:79:1::2/64 nodad dev eth1
        - ip -6 addr add 2001:db8:79:2::1/64 nodad dev eth2
        - >-
          sh -lc 'exec stdbuf -oL -eL /usr/local/bin/midrd --node-id 232
          --listen [::]:33502 --peer [2001:db8:79:1::1]:33501
          --peer [2001:db8:79:2::2]:33503 --group 1
          --prefix 2001:db8:22::2/128 --link 231:5 --link 233:7
          --takeover-delay 1000 --lifetime 10000 --hold-time 2000
          --runtime 14 >/tmp/midrd.log 2>&1 &'
    c:
      kind: linux
      image: $IMAGE
      binds:
        - $BIN:/usr/local/bin/midrd:ro
      cmd: sleep infinity
      exec:
        - ip -6 addr add 2001:db8:79:2::2/64 nodad dev eth1
        - >-
          sh -lc 'exec stdbuf -oL -eL /usr/local/bin/midrd --node-id 233
          --listen [2001:db8:79:2::2]:33503
          --peer [2001:db8:79:2::1]:33502 --group 1
          --prefix 2001:db8:23::3/128 --link 232:7
          --takeover-delay 1000 --lifetime 10000 --hold-time 2000
          --runtime 14 >/tmp/midrd.log 2>&1 &'
  links:
    - endpoints: ["a:eth1", "b:eth1"]
    - endpoints: ["b:eth2", "c:eth1"]
EOF
	containerlab deploy --topo "$topo" --reconfigure >/dev/null
	ACTIVE_TOPO=$topo
	a="clab-${lab}-a"
	b="clab-${lab}-b"
	c="clab-${lab}-c"
	if ! wait_log_count "$a" 'route originator=233 metric=12 reachable=1' 1 140 ||
	   ! wait_log_count "$c" 'route originator=231 metric=12 reachable=1' 1 140; then
		fail_case "$lab" "$run" 'IPv6 partition baseline did not converge'
		return 1
	fi
	before_b=$(docker exec "$b" sh -lc \
		"grep -c 'sync type=6' /tmp/midrd.log 2>/dev/null || true")
	before_c=$(docker exec "$c" sh -lc \
		"grep -c 'sync type=6' /tmp/midrd.log 2>/dev/null || true")
	before_close_b=$(docker exec "$b" sh -lc \
		"grep -c 'closed family=' /tmp/midrd.log 2>/dev/null || true")
	before_close_c=$(docker exec "$c" sh -lc \
		"grep -c 'closed family=' /tmp/midrd.log 2>/dev/null || true")
	before_low_a=$(docker exec "$a" sh -lc \
		"grep -Ec 'spf generation=.* routes=[012]$' /tmp/midrd.log 2>/dev/null || true")
	before_low_c=$(docker exec "$c" sh -lc \
		"grep -Ec 'spf generation=.* routes=[012]$' /tmp/midrd.log 2>/dev/null || true")
	containerlab tools netem set --topo "$topo" --node "$b" \
		--interface eth2 --loss 100 >/dev/null
	containerlab tools netem set --topo "$topo" --node "$c" \
		--interface eth1 --loss 100 >/dev/null
	sleep 2.5
	if ! wait_log_count "$b" 'closed family=' "$((before_close_b + 1))" 30 ||
	   ! wait_log_count "$c" 'closed family=' "$((before_close_c + 1))" 30; then
		fail_case "$lab" "$run" 'IPv6 partition did not cross the configured hold timer'
		return 1
	fi
	if [[ $(docker exec "$a" sh -lc \
		"grep -Ec 'spf generation=.* routes=[012]$' /tmp/midrd.log 2>/dev/null || true") \
		-ne "$before_low_a" ]] ||
	   [[ $(docker exec "$c" sh -lc \
		"grep -Ec 'spf generation=.* routes=[012]$' /tmp/midrd.log 2>/dev/null || true") \
		-ne "$before_low_c" ]]; then
		fail_case "$lab" "$run" 'IPv6 partition replaced the old consistent route view'
		return 1
	fi
	containerlab tools netem reset --topo "$topo" --node "$b" \
		--interface eth2 >/dev/null
	containerlab tools netem reset --topo "$topo" --node "$c" \
		--interface eth1 >/dev/null
	after_b=$((before_b + 1))
	after_c=$((before_c + 1))
	if ! wait_log_count "$b" 'sync type=6' "$after_b" 100 ||
	   ! wait_log_count "$c" 'sync type=6' "$after_c" 100; then
		fail_case "$lab" "$run" 'IPv6 partition recovery did not complete snapshot/EoR'
		return 1
	fi
	collect_logs "$lab" "$run"
	grep -q 'route originator=233 metric=12 reachable=1' "$run/a.log"
	grep -q 'route originator=231 metric=12 reachable=1' "$run/c.log"
	destroy_lab "$topo"
	ACTIVE_TOPO=
	echo "r7 hardening IPv6 partition recovery: PASS (logs: $run)"
}

run_refresh_soak() {
	local run topo lab a b c before_low_a before_low_b before_low_c
	local before_refresh_c refresh_c

	# R=L/3 and the two-hop path each reserve B=1000 ms.  L=6000 ms leaves
	# processing margin after R+2B while remaining short enough for the test.
	run=$(make_run_dir refresh-soak)
	topo="$run/topology.clab.yaml"
	lab="midr-r7-hard-refresh-$$"
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
        - ip addr add 10.81.1.1/30 dev eth1
        - >-
          sh -lc 'exec stdbuf -oL -eL /usr/local/bin/midrd --node-id 301
          --listen 10.81.1.1:33601 --peer 10.81.1.2:33602
          --group 1 --prefix 10.23.1.1/32 --link 302:5
          --takeover-delay 1000 --lifetime 6000 --runtime 18
          >/tmp/midrd.log 2>&1 &'
    b:
      kind: linux
      image: $IMAGE
      binds:
        - $BIN:/usr/local/bin/midrd:ro
      cmd: sleep infinity
      exec:
        - ip addr add 10.81.1.2/30 dev eth1
        - ip addr add 10.81.2.1/30 dev eth2
        - >-
          sh -lc 'exec stdbuf -oL -eL /usr/local/bin/midrd --node-id 302
          --listen 0.0.0.0:33602 --peer 10.81.1.1:33601
          --peer 10.81.2.2:33603 --group 1 --prefix 10.23.2.2/32
          --link 301:5 --link 303:7 --takeover-delay 1000
          --lifetime 6000 --runtime 18 >/tmp/midrd.log 2>&1 &'
    c:
      kind: linux
      image: $IMAGE
      binds:
        - $BIN:/usr/local/bin/midrd:ro
      cmd: sleep infinity
      exec:
        - ip addr add 10.81.2.2/30 dev eth1
        - >-
          sh -lc 'exec stdbuf -oL -eL /usr/local/bin/midrd --node-id 303
          --listen 10.81.2.2:33603 --peer 10.81.2.1:33602
          --group 1 --prefix 10.23.3.3/32 --link 302:7
          --takeover-delay 1000 --lifetime 6000 --runtime 18
          >/tmp/midrd.log 2>&1 &'
  links:
    - endpoints: ["a:eth1", "b:eth1"]
    - endpoints: ["b:eth2", "c:eth1"]
EOF
	containerlab deploy --topo "$topo" --reconfigure >/dev/null
	ACTIVE_TOPO=$topo
	a="clab-${lab}-a"
	b="clab-${lab}-b"
	c="clab-${lab}-c"
	if ! wait_log_count "$a" 'route originator=303 metric=12 reachable=1' 1 140 ||
	   ! wait_log_count "$c" 'route originator=301 metric=12 reachable=1' 1 140; then
		fail_case "$lab" "$run" 'refresh soak baseline did not converge'
		return 1
	fi
	before_low_a=$(docker exec "$a" sh -lc \
		"grep -Ec 'spf generation=.* routes=[012]$' /tmp/midrd.log 2>/dev/null || true")
	before_low_b=$(docker exec "$b" sh -lc \
		"grep -Ec 'spf generation=.* routes=[012]$' /tmp/midrd.log 2>/dev/null || true")
	before_low_c=$(docker exec "$c" sh -lc \
		"grep -Ec 'spf generation=.* routes=[012]$' /tmp/midrd.log 2>/dev/null || true")
	before_refresh_c=$(docker exec "$c" sh -lc \
		"grep -Ec 'event state=1 type=3 originator=301 ' /tmp/midrd.log 2>/dev/null || true")
	refresh_c=$((before_refresh_c + 4))
	if ! wait_log_count "$c" 'event state=1 type=3 originator=301 ' \
		"$refresh_c" 100; then
		fail_case "$lab" "$run" 'owner refresh did not reach the transit/edge peer'
		return 1
	fi
	sleep 1
	if [[ $(docker exec "$a" sh -lc \
		"grep -Ec 'spf generation=.* routes=[012]$' /tmp/midrd.log 2>/dev/null || true") \
		-ne "$before_low_a" ]] ||
	   [[ $(docker exec "$b" sh -lc \
		"grep -Ec 'spf generation=.* routes=[012]$' /tmp/midrd.log 2>/dev/null || true") \
		-ne "$before_low_b" ]] ||
	   [[ $(docker exec "$c" sh -lc \
		"grep -Ec 'spf generation=.* routes=[012]$' /tmp/midrd.log 2>/dev/null || true") \
		-ne "$before_low_c" ]]; then
		fail_case "$lab" "$run" 'refresh soak produced a route-count regression'
		return 1
	fi
	collect_logs "$lab" "$run"
	grep -q 'spf generation=.* routes=3' "$run/a.log"
	grep -q 'spf generation=.* routes=3' "$run/b.log"
	grep -q 'spf generation=.* routes=3' "$run/c.log"
	destroy_lab "$topo"
	ACTIVE_TOPO=
	echo "r7 hardening refresh soak: PASS (logs: $run)"
}

run_object_scale() {
	local run

	run=$(make_run_dir object-scale)
	MIDRD_SCALE_OBJECTS=4096 MIDRD_SCALE_CYCLES=2 "$SCALE_BIN" \
		>"$run/scale.tsv"
	grep -q '^SCALE[[:space:]]\+fill[[:space:]]\+4096[[:space:]]\+1' \
		"$run/scale.tsv"
	grep -q '^SCALE[[:space:]]\+expired[[:space:]]\+4096[[:space:]]\+1[[:space:]]\+0[[:space:]]\+4096[[:space:]]\+4096' \
		"$run/scale.tsv"
	grep -q '^SCALE[[:space:]]\+reclaimed[[:space:]]\+4096[[:space:]]\+1[[:space:]]\+0[[:space:]]\+0' \
		"$run/scale.tsv"
	grep -q '^midrd-scale-test: PASS$' "$run/scale.tsv"
	echo "r7 hardening object scale: PASS (evidence: $run/scale.tsv)"
}

run_neighbor_scale() {
	local run topo lab center expected peers=8 center_args node_args
	local node container pid routes rss hwm fds baseline

	run=$(make_run_dir neighbor-scale)
	topo="$run/topology.clab.yaml"
	lab="midr-r7-hard-neighbors-$$"
	center_args="--node-id 400 --listen 0.0.0.0:33700 --group 1 --prefix 10.24.0.1/32"
	for node in $(seq 1 "$peers"); do
		center_args+=" --peer 10.82.${node}.2:$((33700 + node)) --link $((400 + node)):$((10 + node))"
	done
	{
		printf 'name: %s\ntopology:\n  nodes:\n' "$lab"
		printf '    n0:\n      kind: linux\n      image: %s\n' "$IMAGE"
		printf '      binds:\n        - %s:/usr/local/bin/midrd:ro\n' "$BIN"
		printf '      cmd: sleep infinity\n      exec:\n'
		for node in $(seq 1 "$peers"); do
			printf '        - ip addr add 10.82.%s.1/30 dev eth%s\n' "$node" "$node"
		done
		printf "        - >-\n          sh -lc 'exec stdbuf -oL -eL /usr/local/bin/midrd %s --takeover-delay 1000 --lifetime 12000 --runtime 30 --pidfile /tmp/midrd.pid >/tmp/midrd.log 2>&1 &'\n" "$center_args"
		for node in $(seq 1 "$peers"); do
			node_args="--node-id $((400 + node)) --listen 10.82.${node}.2:$((33700 + node)) --peer 10.82.${node}.1:33700 --group 1 --prefix 10.24.${node}.1/32 --link 400:$((10 + node))"
			printf '    n%s:\n      kind: linux\n      image: %s\n' "$node" "$IMAGE"
			printf '      binds:\n        - %s:/usr/local/bin/midrd:ro\n' "$BIN"
			printf "      cmd: sleep infinity\n      exec:\n        - ip addr add 10.82.%s.2/30 dev eth1\n        - >-\n          sh -lc 'exec stdbuf -oL -eL /usr/local/bin/midrd %s --takeover-delay 1000 --lifetime 12000 --runtime 30 --pidfile /tmp/midrd.pid >/tmp/midrd.log 2>&1 &'\n" "$node" "$node_args"
		done
		printf '  links:\n'
		for node in $(seq 1 "$peers"); do
			printf '    - endpoints: ["n0:eth%s", "n%s:eth1"]\n' "$node" "$node"
		done
	} >"$topo"
	containerlab deploy --topo "$topo" --reconfigure >/dev/null
	ACTIVE_TOPO=$topo
	center="clab-${lab}-n0"
	expected=$((peers + 1))
	if ! wait_log_count "$center" 'established family=4' "$peers" 240; then
		collect_neighbor_logs "$lab" "$run" "$peers"
		echo "neighbor scale did not establish $peers sessions (logs: $run)" >&2
		return 1
	fi
	for node in $(seq 0 "$peers"); do
		container="clab-${lab}-n${node}"
		if ! wait_log_count "$container" "spf generation=.* routes=${expected}" 1 240; then
			collect_neighbor_logs "$lab" "$run" "$peers"
			echo "neighbor scale node n${node} did not converge to ${expected} routes" >&2
			return 1
		fi
	done
	printf 'node\tpeers\troutes\trss_kb\thwm_kb\tfds\n' >"$run/metrics.tsv"
	for node in $(seq 0 "$peers"); do
		container="clab-${lab}-n${node}"
		baseline=$(docker exec "$container" sh -lc 'wc -l </tmp/midrd.log')
		printf '%s\n' "$baseline" >"$run/n${node}.baseline"
	done
	# Cross two owner refresh periods after the complete nine-node view exists.
	sleep 9
	for node in $(seq 0 "$peers"); do
		container="clab-${lab}-n${node}"
		baseline=$(cat "$run/n${node}.baseline")
		if docker exec "$container" sh -lc \
			"tail -n +$((baseline + 1)) /tmp/midrd.log | grep -E 'tx-dropped|closed family=|spf generation=.* routes=' | grep -Ev 'spf generation=.* routes=${expected}$'" \
			>"$run/n${node}.regression"; then
			collect_neighbor_logs "$lab" "$run" "$peers"
			echo "neighbor scale node n${node} regressed or lost a session after convergence" >&2
			return 1
		fi
		pid=$(docker exec "$container" cat /tmp/midrd.pid)
		routes=$(docker exec "$container" sh -lc \
			"grep -E 'spf generation=.* routes=' /tmp/midrd.log | tail -1 | sed -E 's/.*routes=([0-9]+).*/\\1/'")
		rss=$(docker exec "$container" awk '/VmRSS:/ {print $2}' "/proc/$pid/status")
		hwm=$(docker exec "$container" awk '/VmHWM:/ {print $2}' "/proc/$pid/status")
		fds=$(docker exec -u 0 "$container" sh -lc "ls /proc/$pid/fd | wc -l")
		if ! [[ "$fds" =~ ^[0-9]+$ && "$fds" -gt 0 ]]; then
			collect_neighbor_logs "$lab" "$run" "$peers"
			echo "neighbor scale node n${node} returned an invalid fd count: $fds" >&2
			return 1
		fi
		printf 'n%s\t%s\t%s\t%s\t%s\t%s\n' "$node" \
			"$([[ $node -eq 0 ]] && echo "$peers" || echo 1)" \
			"$routes" "$rss" "$hwm" "$fds" >>"$run/metrics.tsv"
	done
	collect_neighbor_logs "$lab" "$run" "$peers"
	destroy_lab "$topo"
	ACTIVE_TOPO=
	echo "r7 hardening neighbor scale: PASS (evidence: $run)"
}

collect_neighbor_logs() {
	local lab=$1 run=$2 peers=$3 node

	for node in $(seq 0 "$peers"); do
		docker exec "clab-${lab}-n${node}" cat /tmp/midrd.log \
			>"$run/n${node}.log" 2>&1 || true
	done
}

run_owner_expiry() {
	local run topo lab a b c owner_pid before_floor
	local before_withdraw_b before_withdraw_c after_withdraw_b after_withdraw_c

	run=$(make_run_dir owner-expiry)
	topo="$run/topology.clab.yaml"
	lab="midr-r7-hard-expiry-$$"
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
        - ip addr add 10.80.1.1/30 dev eth1
        - >-
          sh -lc 'exec stdbuf -oL -eL /usr/local/bin/midrd --node-id 221
          --listen 10.80.1.1:33301 --peer 10.80.1.2:33302
          --group 1 --prefix 10.22.1.1/32 --link 222:5
          --takeover-delay 1000 --lifetime 3000 --runtime 20
          --pidfile /tmp/midrd.pid >/tmp/midrd.log 2>&1 &'
    b:
      kind: linux
      image: $IMAGE
      binds:
        - $BIN:/usr/local/bin/midrd:ro
      cmd: sleep infinity
      exec:
        - ip addr add 10.80.1.2/30 dev eth1
        - ip addr add 10.80.2.1/30 dev eth2
        - >-
          sh -lc 'exec stdbuf -oL -eL /usr/local/bin/midrd --node-id 222
          --listen 0.0.0.0:33302 --peer 10.80.1.1:33301
          --peer 10.80.2.2:33303 --group 1 --prefix 10.22.2.2/32
          --link 221:5 --link 223:7 --takeover-delay 1000
          --lifetime 3000 --runtime 9 >/tmp/midrd.log 2>&1 &'
    c:
      kind: linux
      image: $IMAGE
      binds:
        - $BIN:/usr/local/bin/midrd:ro
      cmd: sleep infinity
      exec:
        - ip addr add 10.80.2.2/30 dev eth1
        - >-
          sh -lc 'exec stdbuf -oL -eL /usr/local/bin/midrd --node-id 223
          --listen 10.80.2.2:33303 --peer 10.80.2.1:33302
          --group 1 --prefix 10.22.3.3/32 --link 222:7
          --takeover-delay 1000 --lifetime 3000 --runtime 9
          >/tmp/midrd.log 2>&1 &'
  links:
    - endpoints: ["a:eth1", "b:eth1"]
    - endpoints: ["b:eth2", "c:eth1"]
EOF
	containerlab deploy --topo "$topo" --reconfigure >/dev/null
	ACTIVE_TOPO=$topo
	a="clab-${lab}-a"
	b="clab-${lab}-b"
	c="clab-${lab}-c"
	if ! wait_log_count "$c" 'route originator=221 metric=12 reachable=1' 1 120; then
		fail_case "$lab" "$run" 'expiry baseline did not converge'
		return 1
	fi
	before_floor=$(docker exec "$c" sh -lc \
		"grep -Ec 'spf generation=.* routes=2' /tmp/midrd.log 2>/dev/null || true")
	before_withdraw_b=$(docker exec "$b" sh -lc \
		"grep -Ec 'event state=2 .*originator=221' /tmp/midrd.log 2>/dev/null || true")
	before_withdraw_c=$(docker exec "$c" sh -lc \
		"grep -Ec 'event state=2 .*originator=221' /tmp/midrd.log 2>/dev/null || true")
	owner_pid=$(docker exec "$a" cat /tmp/midrd.pid)
	docker exec -u 0 "$a" kill -KILL "$owner_pid"
	if ! wait_log_count "$c" 'spf generation=.* routes=2' \
		"$((before_floor + 1))" 80; then
		fail_case "$lab" "$run" 'remote owner objects did not expire into floor'
		return 1
	fi
	collect_logs "$lab" "$run"
	after_withdraw_b=$(grep -Ec 'event state=2 .*originator=221' "$run/b.log" || true)
	after_withdraw_c=$(grep -Ec 'event state=2 .*originator=221' "$run/c.log" || true)
	if [[ "$after_withdraw_b" -ne "$before_withdraw_b" ||
	      "$after_withdraw_c" -ne "$before_withdraw_c" ]] ||
	   grep -Eq 'shutdown=|final-objects=' "$run/a.log"; then
		echo 'owner loss followed a withdraw/shutdown path instead of natural expiry' >&2
		return 1
	fi
	grep -q 'spf generation=.* routes=2' "$run/b.log"
	grep -q 'spf generation=.* routes=2' "$run/c.log"
	destroy_lab "$topo"
	ACTIVE_TOPO=
	echo "r7 hardening owner expiry: PASS (logs: $run)"
}

case ${1:-all} in
triangle)
	run_triangle
	;;
partition-recovery)
	run_partition_recovery
	;;
ipv6-partition-recovery)
	run_ipv6_partition_recovery
	;;
refresh-soak)
	run_refresh_soak
	;;
object-scale)
	run_object_scale
	;;
neighbor-scale)
	run_neighbor_scale
	;;
owner-expiry)
	run_owner_expiry
	;;
all)
	run_triangle
	run_partition_recovery
	run_ipv6_partition_recovery
	run_refresh_soak
	run_object_scale
	run_neighbor_scale
	run_owner_expiry
	;;
*)
	echo 'usage: r7-hardening-smoke.sh [all|triangle|partition-recovery|ipv6-partition-recovery|refresh-soak|object-scale|neighbor-scale|owner-expiry]' >&2
	exit 2
	;;
esac
echo "r7 hardening smoke ${1:-all}: PASS"
