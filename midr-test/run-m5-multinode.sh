#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BGPD_BIN="${MIDR_BGPD:-$ROOT_DIR/bgpd/bgpd}"
VTYSH_BIN="${MIDR_VTYSH:-$ROOT_DIR/vtysh/vtysh}"
RUN_ROOT="${MIDR_M5_RUN_DIR:-$ROOT_DIR/midr-test/run}"
WAIT_STEPS="${MIDR_M5_WAIT_STEPS:-80}"
SCENARIOS=(
	line
	withdraw
	scope
	triangle
	eor-timeout
	route-refresh
	prefix
	prefix-withdraw
	prefix-takeover
)

usage()
{
	printf 'Usage: %s <%s|all>\n' "$0" "$(IFS='|'; echo "${SCENARIOS[*]}")"
}

run_rootless()
{
	local scenario="$1"

	if ! command -v unshare >/dev/null || ! command -v ip >/dev/null ||
	   ! command -v mount >/dev/null; then
		echo "rootless namespace dependencies are unavailable" >&2
		return 1
	fi
	unshare -Urnm "$0" --inside "$scenario"
}

if [[ "${1:-}" != "--inside" ]]; then
	scenario="${1:-all}"
	if [[ "$scenario" == "all" ]]; then
		for scenario in "${SCENARIOS[@]}"; do
			run_rootless "$scenario"
		done
		echo "PASS: all MIDR M5 rootless multi-node scenarios"
		exit 0
	fi
	if ! printf '%s\n' "${SCENARIOS[@]}" | grep -Fx "$scenario" >/dev/null; then
		usage >&2
		exit 1
	fi
	run_rootless "$scenario"
	exit
fi

scenario="${2:?missing internal scenario}"
RUN_DIR="$RUN_ROOT/m5-$scenario"
declare -A NODE_IP=(
	[r1]=10.0.0.1
	[r2]=10.0.0.2
	[r3]=10.0.0.3
)
declare -A NEIGHBORS
NODES=(r1 r2 r3)

case "$scenario" in
	triangle)
		NEIGHBORS[r1]="r2 r3"
		NEIGHBORS[r2]="r1 r3"
		NEIGHBORS[r3]="r1 r2"
		;;
	eor-timeout)
		NODES=(r1 r2)
		NEIGHBORS[r1]="r2"
		NEIGHBORS[r2]="r1"
		;;
	*)
		NEIGHBORS[r1]="r2"
		NEIGHBORS[r2]="r1 r3"
		NEIGHBORS[r3]="r2"
		;;
esac

rm -rf "$RUN_DIR"
mkdir -p "$RUN_DIR/varlib" "$RUN_DIR/varrun"
mount --bind "$RUN_DIR/varlib" /var/lib
mount --bind "$RUN_DIR/varrun" /var/run

for node in "${NODES[@]}"; do
	mkdir -p "$RUN_DIR/$node/vty" "/var/lib/frr/$node" \
		"/var/run/frr/$node"
	ip addr add "${NODE_IP[$node]}/32" dev lo
done
ip link set lo up

write_config()
{
	local node="$1"
	local peer
	local config="$RUN_DIR/$node.conf"

	{
		printf 'hostname %s\n' "$node"
		printf 'password zebra\n'
		printf 'router bgp 65000\n'
		printf ' bgp router-id %s\n' "${NODE_IP[$node]}"
		for peer in ${NEIGHBORS[$node]}; do
			printf ' neighbor %s remote-as 65000\n' \
				"${NODE_IP[$peer]}"
			printf ' neighbor %s update-source %s\n' \
				"${NODE_IP[$peer]}" "${NODE_IP[$node]}"
		done
	} >"$config"
}

start_node()
{
	local node="$1"

	"$BGPD_BIN" -S -Z -d -N "$node" -p 179 -l "${NODE_IP[$node]}" \
		-f "$RUN_DIR/$node.conf" -i "$RUN_DIR/$node/$node.pid" \
		--vty_socket "$RUN_DIR/$node/vty" --log stdout \
		--limit-fds 10000 >"$RUN_DIR/$node/$node.log" 2>&1
}

vty()
{
	local node="$1"
	shift
	"$VTYSH_BIN" --vty_socket "$RUN_DIR/$node/vty" -d bgpd "$@" \
		2>/dev/null
}

stop_nodes()
{
	local node

	for node in "${NODES[@]}"; do
		if [[ -f "$RUN_DIR/$node/$node.pid" ]]; then
			kill "$(cat "$RUN_DIR/$node/$node.pid")" 2>/dev/null ||
				true
		fi
	done
}
trap stop_nodes EXIT

wait_for()
{
	local description="$1"
	shift
	local step

	for step in $(seq 1 "$WAIT_STEPS"); do
		if "$@"; then
			return 0
		fi
		sleep 0.2
	done
	echo "timeout waiting for $description" >&2
	for debug_node in "${NODES[@]}"; do
		echo "=== $debug_node ===" >&2
		vty "$debug_node" -c "show midr rib summary" \
			-c "show midr lsdb summary" -c "show midr owned" \
			-c "show midr sync" >&2 || true
	done
	return 1
}

output_contains()
{
	local node="$1"
	local command="$2"
	local expected="$3"
	local output

	output="$(vty "$node" -c "$command" || true)"
	grep -F "$expected" <<<"$output" >/dev/null
}

assert_output()
{
	local node="$1"
	local command="$2"
	local expected="$3"
	local output

	output="$(vty "$node" -c "$command")"
	if ! grep -F "$expected" <<<"$output" >/dev/null; then
		printf 'missing "%s" from %s: %s\n%s\n' \
			"$expected" "$node" "$command" "$output" >&2
		return 1
	fi
}

activate_sessions()
{
	local node
	local peer

	for node in "${NODES[@]}"; do
		for peer in ${NEIGHBORS[$node]}; do
			vty "$node" -c \
				"midr peer session ${NODE_IP[$peer]} remote-as 65000 midr-link-state"
		done
	done
	for node in "${NODES[@]}"; do
		for peer in ${NEIGHBORS[$node]}; do
			wait_for "$node to establish MIDR with $peer" \
				output_contains "$node" \
				"show bgp neighbors ${NODE_IP[$peer]}" \
				"Address Family MIDR Link-State: advertised and received"
		done
	done
}

inject_membership()
{
	local node="$1"
	local group="$2"

	vty "$node" -c \
		"midr topology node upsert ${NODE_IP[$node]} group $group transport ${NODE_IP[$node]} version 1"
}

inject_link()
{
	local node="$1"
	local remote="$2"
	local link_id="$3"

	vty "$node" -c \
		"midr topology link upsert ${NODE_IP[$node]} ${NODE_IP[$remote]} id $link_id local-address 198.51.100.$link_id remote-address 198.51.101.$link_id rtt-us 1000 loss-ppm 100 available-bandwidth-kbps 100000 version 1"
}

configure_prefix()
{
	local node="$1"
	local prefix="$2"

	vty "$node" -c "configure terminal" \
		-c "route-map EXPORT-MIDR permit 10" \
		-c "exit"
	vty "$node" -c "configure terminal" \
		-c "router bgp 65000" \
		-c "no bgp network import-check" \
		-c "midr group-prefix takeover-delay-ms 500" \
		-c "address-family ipv4 unicast" \
		-c "midr prefix-export route-map EXPORT-MIDR" \
		-c "midr prefix-export local-source network" \
		-c "network $prefix"
}

withdraw_prefix()
{
	local node="$1"
	local prefix="$2"

	vty "$node" -c "configure terminal" \
		-c "router bgp 65000" \
		-c "address-family ipv4 unicast" \
		-c "no network $prefix"
}

wait_field()
{
	local node="$1"
	local command="$2"
	local expected="$3"

	wait_for "$node: $expected" output_contains "$node" "$command" \
		"$expected"
}

check_logs()
{
	local node
	local pattern='Received signal|assertion .* failed|Segmentation fault|AddressSanitizer|core dumped'

	for node in "${NODES[@]}"; do
		if grep -E "$pattern" "$RUN_DIR/$node/$node.log" >/dev/null; then
			echo "fatal daemon output in $RUN_DIR/$node/$node.log" >&2
			return 1
		fi
	done
}

for node in "${NODES[@]}"; do
	write_config "$node"
	start_node "$node"
done
for node in "${NODES[@]}"; do
	wait_for "$node VTY socket" test -S "$RUN_DIR/$node/vty/bgpd.vty"
done
activate_sessions

case "$scenario" in
	line|withdraw|route-refresh)
		inject_membership r1 10
		inject_membership r2 10
		inject_membership r3 10
		for node in "${NODES[@]}"; do
			wait_field "$node" "show midr rib summary" \
				"identities:        3/65536"
			wait_field "$node" "show midr sync" \
				"state:              READY"
		done
		inject_link r1 r2 1
		wait_field r3 "show midr lsdb summary" "links:            1"
		wait_field r3 "show midr rib summary" \
			"identities:        4/65536"
		if [[ "$scenario" == "withdraw" ]]; then
			vty r1 -c \
				"midr topology link withdraw 10.0.0.1 10.0.0.2 id 1 version 2"
			wait_field r3 "show midr lsdb summary" \
				"links:            0"
			wait_field r3 "show midr rib summary" \
				"identities:        3/65536"
		elif [[ "$scenario" == "route-refresh" ]]; then
			vty r3 -c "clear bgp 10.0.0.2 soft in"
			wait_field r3 "show midr rib summary" \
				"identities:        4/65536"
			assert_output r3 "show midr rib summary" \
				"conflicts:         0"
		fi
		;;
	scope)
		inject_membership r1 10
		inject_membership r2 10
		inject_membership r3 20
		for node in "${NODES[@]}"; do
			wait_field "$node" "show midr rib summary" \
				"identities:        3/65536"
		done
		inject_link r1 r2 1
		inject_link r2 r3 2
		wait_field r1 "show midr lsdb summary" "links:            2"
		wait_field r2 "show midr lsdb summary" "links:            2"
		wait_field r3 "show midr lsdb summary" "links:            1"
		;;
	triangle)
		for node in "${NODES[@]}"; do
			inject_membership "$node" 10
		done
		for node in "${NODES[@]}"; do
			wait_field "$node" "show midr rib summary" \
				"identities:        3/65536"
			wait_field "$node" "show midr rib summary" \
				"paths:             5"
		done
		vty r1 -c \
			"midr peer session 10.0.0.3 release midr-link-state"
		vty r3 -c \
			"midr peer session 10.0.0.1 release midr-link-state"
		wait_field r3 "show midr rib summary" \
			"identities:        3/65536"
		assert_output r3 "show midr rib summary" "conflicts:         0"
		;;
	eor-timeout)
		vty r1 -c "configure terminal" -c "router bgp 65000" \
			-c "midr eor-timeout 1"
		inject_membership r1 10
		wait_field r1 "show midr sync" "state:              READY"
		wait_field r1 "show midr sync" "timed-out peers:    1"
		inject_membership r2 10
		wait_field r1 "show midr sync" "timed-out peers:    0"
		wait_field r2 "show midr sync" "state:              READY"
		;;
	prefix|prefix-withdraw|prefix-takeover)
		for node in "${NODES[@]}"; do
			inject_membership "$node" 10
		done
		for node in "${NODES[@]}"; do
			wait_field "$node" "show midr sync" \
				"state:              READY"
		done
		configure_prefix r1 203.0.113.0/24
		if [[ "$scenario" == "prefix-takeover" ]]; then
			configure_prefix r2 203.0.113.0/24
		fi
		wait_field r1 "show midr prefix summary" \
			"contributors:      1"
		wait_field r3 "show midr ted summary" \
			"prefix-groups:         1"
		if [[ "$scenario" == "prefix-withdraw" ]]; then
			withdraw_prefix r1 203.0.113.0/24
			wait_field r1 "show midr prefix summary" \
				"contributors:      0"
			wait_field r3 "show midr ted summary" \
				"prefix-groups:         0"
		elif [[ "$scenario" == "prefix-takeover" ]]; then
			kill "$(cat "$RUN_DIR/r1/r1.pid")"
			rm -f "$RUN_DIR/r1/r1.pid"
			wait_field r2 "show midr owned" \
				"group prefixes:     1"
			wait_field r2 "show midr owned" \
				"10.0.0.2 (COMMITTED)"
			wait_field r3 "show midr ted summary" \
				"prefix-groups:         1"
		fi
		;;
esac

check_logs
echo "PASS: MIDR M5 rootless $scenario"
