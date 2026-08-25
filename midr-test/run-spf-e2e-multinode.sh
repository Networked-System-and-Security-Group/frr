#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Privileged MIDR multi-node end-to-end tests:
# topology/prefix input -> MIDR propagation -> LSDB/TED -> SPF runtime ->
# group-3 Zebra adapter -> ZAPI -> zebra -> Linux FIB -> packet forwarding.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BGPD_BIN="${MIDR_BGPD:-$ROOT_DIR/bgpd/bgpd}"
ZEBRA_BIN="${MIDR_ZEBRA:-$ROOT_DIR/zebra/zebra}"
SCENARIO="${1:-line}"
RUN_DIR="${MIDR_SPF_E2E_RUN_DIR:-$ROOT_DIR/midr-test/run/spf-e2e/$SCENARIO}"
WAIT_STEPS="${MIDR_SPF_E2E_WAIT_STEPS:-160}"
NS_TAG="${MIDR_SPF_E2E_NS_TAG:-$$}"
KEEP_NAMESPACES="${MIDR_SPF_E2E_KEEP:-0}"
PASS_COUNT=0

declare -A NS=()
declare -A ROUTER_ID=()
declare -A TEST_PREFIX=()
declare -A PEERS=()
declare -A GROUP_ID=()

case "$SCENARIO" in
line)
	NODES=(r1 r2 r3)
	LINK_KEYS=(12 23)
	SOURCE_NODE=r1
	DEST_NODE=r3
	SOURCE_IP=10.101.0.1
	DEST_IP=10.103.0.1
	EXPECTED_LINK_COUNT=4
	ROUTER_ID=([r1]="10.0.0.1" [r2]="10.0.0.2" [r3]="10.0.0.3")
	TEST_PREFIX=([r1]="$SOURCE_IP/32" [r3]="$DEST_IP/32")
	PEERS=([r1]="10.12.0.2" [r2]="10.12.0.1 10.23.0.2" [r3]="10.23.0.1")
	GROUP_ID=([r1]=10 [r2]=10 [r3]=10)
	;;
complex)
	NODES=(r1 r2 r3 r4 r5)
	LINK_KEYS=(12 13 24 34 45)
	SOURCE_NODE=r1
	DEST_NODE=r5
	SOURCE_IP=10.101.0.1
	DEST_IP=10.105.0.1
	EXPECTED_LINK_COUNT=10
	ROUTER_ID=([r1]="10.0.0.1" [r2]="10.0.0.2" [r3]="10.0.0.3" \
		[r4]="10.0.0.4" [r5]="10.0.0.5")
	TEST_PREFIX=([r1]="$SOURCE_IP/32" [r5]="$DEST_IP/32")
	PEERS=([r1]="10.12.0.2 10.13.0.2" [r2]="10.12.0.1 10.24.0.2" \
		[r3]="10.13.0.1 10.34.0.2" [r4]="10.24.0.1 10.34.0.1 10.45.0.2" \
		[r5]="10.45.0.1")
	GROUP_ID=([r1]=10 [r2]=10 [r3]=10 [r4]=10 [r5]=10)
	;;
cross-group)
	NODES=(r1 r2 r3 r4 r5)
	LINK_KEYS=(12 13 24 34 45)
	SOURCE_NODE=r1
	DEST_NODE=r5
	SOURCE_IP=10.101.0.1
	DEST_IP=10.105.0.1
	EXPECTED_LINK_COUNT=10
	ROUTER_ID=([r1]="10.0.0.1" [r2]="10.0.0.2" [r3]="10.0.0.3" \
		[r4]="10.0.0.4" [r5]="10.0.0.5")
	TEST_PREFIX=([r1]="$SOURCE_IP/32" [r5]="$DEST_IP/32")
	PEERS=([r1]="10.12.0.2 10.13.0.2" [r2]="10.12.0.1 10.24.0.2" \
		[r3]="10.13.0.1 10.34.0.2" [r4]="10.24.0.1 10.34.0.1 10.45.0.2" \
		[r5]="10.45.0.1")
	GROUP_ID=([r1]=10 [r2]=10 [r3]=10 [r4]=20 [r5]=30)
	;;
-h | --help | help)
	printf 'Usage: %s <line|complex|cross-group>\n' "$0"
	exit 0
	;;
*)
	printf 'Unknown scenario: %s (expected line, complex, or cross-group)\n' \
		"$SCENARIO" >&2
	exit 2
	;;
esac

for node in "${NODES[@]}"; do
	NS[$node]="midr-spf-${NS_TAG}-$node"
done

pass()
{
	PASS_COUNT=$((PASS_COUNT + 1))
	printf 'PASS: %s\n' "$*"
}

fail()
{
	printf 'FAIL: %s\n' "$*" >&2
	return 1
}

require_environment()
{
	local command

	if [[ "$(uname -s)" != "Linux" ]]; then
		fail "Linux is required"
	fi
	if ((EUID != 0)); then
		fail "run this test as root inside a privileged Ubuntu container"
	fi
	for command in ip sysctl ping grep sed tail seq python3 unshare mount; do
		if ! command -v "$command" >/dev/null; then
			fail "required command is unavailable: $command"
		fi
	done
	for command in "$BGPD_BIN" "$ZEBRA_BIN"; do
		if [[ ! -x "$command" ]]; then
			fail "FRR binary is unavailable: $command"
		fi
	done
}

cleanup()
{
	local key
	local node
	local pid

	for node in "${NODES[@]}"; do
		while read -r pid; do
			[[ -n "$pid" ]] && kill "$pid" 2>/dev/null || true
		done < <(ip netns pids "${NS[$node]}" 2>/dev/null || true)
	done
	for node in "${NODES[@]}"; do
		ip netns del "${NS[$node]}" 2>/dev/null || true
	done
	for key in "${LINK_KEYS[@]}"; do
		ip link del "m${key}a${NS_TAG}" 2>/dev/null || true
	done
}

dump_diagnostics()
{
	local node

	printf '\n=== MIDR SPF E2E diagnostics ===\n' >&2
	for node in "${NODES[@]}"; do
		printf '\n--- %s control plane ---\n' "$node" >&2
		if [[ -S "$RUN_DIR/$node/bgpd-vty/bgpd.vty" ]]; then
			vty "$node" -c "show running-config" -c "show bgp summary" \
				-c "show midr sync" -c "show midr rib summary" \
				-c "show midr lsdb summary" -c "show midr ted summary" \
				-c "show midr owned" -c "show midr prefix summary" >&2 || true
		fi
		printf '%s\n' "--- $node FIB ---" >&2
		if ip netns pids "${NS[$node]}" >/dev/null 2>&1; then
			ip -n "${NS[$node]}" -4 route show >&2 || true
		fi
		printf '%s\n' "--- $node bgpd log ---" >&2
		tail -80 "$RUN_DIR/$node/bgpd.log" >&2 || true
		printf '%s\n' "--- $node zebra log ---" >&2
		tail -80 "$RUN_DIR/$node/zebra.log" >&2 || true
	done
}

on_exit()
{
	local status=$?

	if ((status != 0)); then
		dump_diagnostics
	fi
	if [[ "$KEEP_NAMESPACES" == "1" ]]; then
		printf 'MIDR_SPF_E2E_KEEP=1: preserving namespaces and logs in %s\n' "$RUN_DIR" >&2
	else
		cleanup
	fi
	exit "$status"
}
trap on_exit EXIT

prepare_run_dir()
{
	local node

	if [[ -z "$RUN_DIR" || "$RUN_DIR" == "/" ]]; then
		fail "unsafe run directory: $RUN_DIR"
	fi
	case "$RUN_DIR" in
	"$ROOT_DIR"/midr-test/run/* | /tmp/*) ;;
	*) fail "run directory must be under midr-test/run or /tmp: $RUN_DIR" ;;
	esac
	rm -rf -- "$RUN_DIR"
	mkdir -p "$RUN_DIR"
	for node in "${NODES[@]}"; do
		mkdir -p "$RUN_DIR/$node/bgpd-vty" "$RUN_DIR/$node/zebra-vty" \
			"$RUN_DIR/$node/libstate"
	done
	mkdir -p /usr/local/var/lib/frr /usr/local/var/run/frr
}

create_veth_link()
{
	local left_node="$1"
	local right_node="$2"
	local key="$3"
	local left_address="$4"
	local right_address="$5"
	local left_interface="${left_node}${right_node}"
	local right_interface="${right_node}${left_node}"
	local left_veth="m${key}a${NS_TAG}"
	local right_veth="m${key}b${NS_TAG}"

	ip link add "$left_veth" type veth peer name "$right_veth"
	ip link set "$left_veth" netns "${NS[$left_node]}"
	ip link set "$right_veth" netns "${NS[$right_node]}"
	ip -n "${NS[$left_node]}" link set "$left_veth" name "$left_interface"
	ip -n "${NS[$right_node]}" link set "$right_veth" name "$right_interface"
	ip -n "${NS[$left_node]}" address add "$left_address/30" dev "$left_interface"
	ip -n "${NS[$right_node]}" address add "$right_address/30" dev "$right_interface"
	ip -n "${NS[$left_node]}" link set "$left_interface" up
	ip -n "${NS[$right_node]}" link set "$right_interface" up
}

create_namespaces()
{
	local node

	for node in "${NODES[@]}"; do
		ip netns add "${NS[$node]}"
		ip -n "${NS[$node]}" link set lo up
		ip -n "${NS[$node]}" address add "${ROUTER_ID[$node]}/32" dev lo
	done
	ip -n "${NS[$SOURCE_NODE]}" address add "${TEST_PREFIX[$SOURCE_NODE]}" dev lo
	ip -n "${NS[$DEST_NODE]}" address add "${TEST_PREFIX[$DEST_NODE]}" dev lo

	create_veth_link r1 r2 12 10.12.0.1 10.12.0.2
	if [[ "$SCENARIO" == "line" ]]; then
		create_veth_link r2 r3 23 10.23.0.1 10.23.0.2
	else
		create_veth_link r1 r3 13 10.13.0.1 10.13.0.2
		create_veth_link r2 r4 24 10.24.0.1 10.24.0.2
		create_veth_link r3 r4 34 10.34.0.1 10.34.0.2
		create_veth_link r4 r5 45 10.45.0.1 10.45.0.2
	fi

	for node in "${NODES[@]}"; do
		ip netns exec "${NS[$node]}" sysctl -q -w net.ipv4.ip_forward=1
		ip netns exec "${NS[$node]}" sysctl -q -w net.ipv4.conf.all.rp_filter=0
		ip netns exec "${NS[$node]}" sysctl -q -w net.ipv4.conf.default.rp_filter=0
	done
	pass "${#NODES[@]} isolated router namespaces created for $SCENARIO topology"
}

write_bgpd_config()
{
	local node="$1"
	local peer
	local config="$RUN_DIR/$node/bgpd.conf"

	{
		printf 'hostname %s\n' "$node"
		printf 'password zebra\n'
		printf 'route-map BLOCK-IPV4 deny 10\n'
		printf 'router bgp 65000\n'
		printf ' bgp router-id %s\n' "${ROUTER_ID[$node]}"
		for peer in ${PEERS[$node]}; do
			printf ' neighbor %s remote-as 65000\n' "$peer"
		done
		printf ' address-family ipv4 unicast\n'
		for peer in ${PEERS[$node]}; do
			printf '  neighbor %s route-map BLOCK-IPV4 in\n' "$peer"
			printf '  neighbor %s route-map BLOCK-IPV4 out\n' "$peer"
		done
		printf ' exit-address-family\n'
	} >"$config"
}

start_daemons()
{
	local node
	local zsock

	for node in "${NODES[@]}"; do
		write_bgpd_config "$node"
		zsock="$RUN_DIR/$node/zserv.api"
		ip netns exec "${NS[$node]}" \
			env LD_LIBRARY_PATH="$ROOT_DIR/lib/.libs" \
			"$ZEBRA_BIN" -f /dev/null -i "$RUN_DIR/$node/zebra.pid" \
			-z "$zsock" --vty_socket "$RUN_DIR/$node/zebra-vty" \
			-u root -g root --limit-fds 10000 --log stdout \
			>"$RUN_DIR/$node/zebra.log" 2>&1 &
	done
	for node in "${NODES[@]}"; do
		wait_for "$node zebra socket" test -S "$RUN_DIR/$node/zserv.api"
	done

	for node in "${NODES[@]}"; do
		ip netns exec "${NS[$node]}" unshare -m --propagation private \
			bash -c 'mount --bind "$1" /usr/local/var/lib/frr && shift && exec "$@"' \
			bash "$RUN_DIR/$node/libstate" \
			env LD_LIBRARY_PATH="$ROOT_DIR/lib/.libs" \
			"$BGPD_BIN" -S -p 179 -f "$RUN_DIR/$node/bgpd.conf" \
			-i "$RUN_DIR/$node/bgpd.pid" -z "$RUN_DIR/$node/zserv.api" \
			--vty_socket "$RUN_DIR/$node/bgpd-vty" --limit-fds 10000 \
			--log stdout >"$RUN_DIR/$node/bgpd.log" 2>&1 &
	done
	for node in "${NODES[@]}"; do
		wait_for "$node bgpd VTY socket" test -S "$RUN_DIR/$node/bgpd-vty/bgpd.vty"
	done
	pass "one bgpd and one zebra started per node"
}

vty()
{
	local node="$1"
	shift
	local socket="$RUN_DIR/$node/bgpd-vty/bgpd.vty"

	python3 - "$socket" "$@" <<'PY'
import socket
import sys

path = sys.argv[1]
args = sys.argv[2:]
commands = []
index = 0
while index < len(args):
    if args[index] != "-c" or index + 1 >= len(args):
        raise SystemExit("usage: vty NODE -c COMMAND [-c COMMAND ...]")
    commands.append(args[index + 1])
    index += 2

commands.insert(0, "enable")

client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
client.connect(path)
result = 0
for command in commands:
    client.sendall(command.encode("utf-8") + b"\0")
    response = bytearray()
    while True:
        chunk = client.recv(65536)
        if not chunk:
            raise SystemExit("VTY socket closed before command response")
        response.extend(chunk)
        marker = response.find(b"\0\0\0")
        if marker >= 0 and len(response) >= marker + 4:
            sys.stdout.buffer.write(response[:marker])
            result = max(result, response[marker + 3])
            break
client.close()
# FRR command result values are not POSIX exit codes.  The surrounding test
# validates every state-changing command through an explicit observable wait.
raise SystemExit(0)
PY
}

wait_for()
{
	local description="$1"
	shift
	local step

	for step in $(seq 1 "$WAIT_STEPS"); do
		if "$@"; then
			return 0
		fi
		sleep 0.25
	done
	fail "timeout waiting for $description"
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

wait_field()
{
	local node="$1"
	local command="$2"
	local expected="$3"

	wait_for "$node: $expected" output_contains "$node" "$command" "$expected"
}

activate_midr_sessions()
{
	local node
	local peer
	local output

	for node in "${NODES[@]}"; do
		for peer in ${PEERS[$node]}; do
			output="$(vty "$node" -c \
				"midr peer session $peer remote-as 65000 midr-link-state")"
			if grep -F "% MIDR peer session" <<<"$output" >/dev/null; then
				printf '%s\n' "$output" >&2
				fail "$node failed to activate MIDR session with $peer"
			fi
		done
	done
	for node in "${NODES[@]}"; do
		for peer in ${PEERS[$node]}; do
			wait_field "$node" "show bgp neighbors $peer" \
				"Address Family MIDR Link-State: advertised and received"
		done
	done
	pass "all adjacent MIDR BGP sessions established"
}

inject_membership()
{
	local node="$1"

	vty "$node" -c \
		"midr topology node upsert ${ROUTER_ID[$node]} group ${GROUP_ID[$node]} transport ${ROUTER_ID[$node]} version 1" \
		>/dev/null
}

inject_link()
{
	local node="$1"
	local remote_id="$2"
	local link_id="$3"
	local local_address="$4"
	local remote_address="$5"
	local interface="$6"
	local version="$7"
	local ifindex

	ifindex="$(ip -n "${NS[$node]}" -o link show "$interface" | sed -E 's/^([0-9]+):.*/\1/')"
	vty "$node" -c \
		"midr topology link upsert ${ROUTER_ID[$node]} $remote_id id $link_id local-address $local_address remote-address $remote_address rtt-us 1000 loss-ppm 100 available-bandwidth-kbps 100000 version $version ifindex $ifindex" \
		>/dev/null
}

verify_ted_topology()
{
	local node="$1"
	local node_count
	local intra_count
	local egress_count
	local group_edge_count

	if [[ "$SCENARIO" != "cross-group" ]]; then
		node_count="${#NODES[@]}"
		intra_count="$EXPECTED_LINK_COUNT"
		egress_count=0
		group_edge_count=0
	else
		case "${GROUP_ID[$node]}" in
		10)
			node_count=3
			intra_count=4
			egress_count=2
			;;
		20)
			node_count=1
			intra_count=0
			egress_count=3
			;;
		30)
			node_count=1
			intra_count=0
			egress_count=1
			;;
		esac
		group_edge_count=4
	fi

	wait_field "$node" "show midr ted summary" "state:                 READY"
	wait_field "$node" "show midr ted summary" "local group:           ${GROUP_ID[$node]}"
	wait_field "$node" "show midr ted summary" "nodes:                 $node_count"
	wait_field "$node" "show midr ted summary" "intra links:           $intra_count"
	wait_field "$node" "show midr ted summary" "egress links:          $egress_count"
	wait_field "$node" "show midr ted summary" "group edges:           $group_edge_count"
}

inject_topology()
{
	local node
	local node_count="${#NODES[@]}"

	for node in "${NODES[@]}"; do
		inject_membership "$node"
	done
	for node in "${NODES[@]}"; do
		wait_field "$node" "show midr sync" "state:              READY"
		wait_field "$node" "show midr rib summary" \
			"identities:        $node_count/65536"
	done

	inject_link r1 "${ROUTER_ID[r2]}" 12 10.12.0.1 10.12.0.2 r1r2 1
	inject_link r2 "${ROUTER_ID[r1]}" 21 10.12.0.2 10.12.0.1 r2r1 1
	if [[ "$SCENARIO" == "line" ]]; then
		inject_link r2 "${ROUTER_ID[r3]}" 23 10.23.0.1 10.23.0.2 r2r3 1
		inject_link r3 "${ROUTER_ID[r2]}" 32 10.23.0.2 10.23.0.1 r3r2 1
	else
		inject_link r1 "${ROUTER_ID[r3]}" 13 10.13.0.1 10.13.0.2 r1r3 1
		inject_link r3 "${ROUTER_ID[r1]}" 31 10.13.0.2 10.13.0.1 r3r1 1
		inject_link r2 "${ROUTER_ID[r4]}" 24 10.24.0.1 10.24.0.2 r2r4 1
		inject_link r4 "${ROUTER_ID[r2]}" 42 10.24.0.2 10.24.0.1 r4r2 1
		inject_link r3 "${ROUTER_ID[r4]}" 34 10.34.0.1 10.34.0.2 r3r4 1
		inject_link r4 "${ROUTER_ID[r3]}" 43 10.34.0.2 10.34.0.1 r4r3 1
		inject_link r4 "${ROUTER_ID[r5]}" 45 10.45.0.1 10.45.0.2 r4r5 1
		inject_link r5 "${ROUTER_ID[r4]}" 54 10.45.0.2 10.45.0.1 r5r4 1
	fi

	for node in "${NODES[@]}"; do
		verify_ted_topology "$node"
	done
	if [[ "$SCENARIO" == "cross-group" ]]; then
		pass "yhy propagation produced group-local TEDs and four directed Group edges"
	else
		pass "yhy propagation produced a READY $node_count-node TED on every node"
	fi
}

configure_prefix()
{
	local node="$1"
	local prefix="$2"

	vty "$node" -c "configure terminal" \
		-c "route-map EXPORT-MIDR permit 10" -c "exit" >/dev/null
	vty "$node" -c "configure terminal" -c "router bgp 65000" \
		-c "no bgp network import-check" -c "midr group-prefix takeover-delay-ms 0" \
		-c "address-family ipv4 unicast" \
		-c "midr prefix-export route-map EXPORT-MIDR" \
		-c "network $prefix" >/dev/null
}

withdraw_prefix()
{
	local node="$1"
	local prefix="$2"

	vty "$node" -c "configure terminal" -c "router bgp 65000" \
		-c "address-family ipv4 unicast" -c "no network $prefix" >/dev/null
}

route_matches()
{
	local node="$1"
	local prefix="$2"
	local nexthop="$3"
	local interface="$4"
	local output

	output="$(ip -n "${NS[$node]}" -4 route show "$prefix" proto 199 2>/dev/null || true)"
	grep -F "via $nexthop" <<<"$output" >/dev/null &&
		grep -F "dev $interface" <<<"$output" >/dev/null
}

route_absent()
{
	local node="$1"
	local prefix="$2"

	[[ -z "$(ip -n "${NS[$node]}" -4 route show "$prefix" proto 199 2>/dev/null)" ]]
}

route_excludes()
{
	local node="$1"
	local prefix="$2"
	local nexthop="$3"
	local output

	output="$(ip -n "${NS[$node]}" -4 route show "$prefix" proto 199 2>/dev/null || true)"
	[[ -n "$output" ]] && ! grep -F "via $nexthop" <<<"$output" >/dev/null
}

ping_end_to_end()
{
	ip netns exec "${NS[$SOURCE_NODE]}" ping -I "$SOURCE_IP" -c 3 -W 1 \
		"$DEST_IP" >/dev/null
}

ping_fails()
{
	! ip netns exec "${NS[$SOURCE_NODE]}" ping -I "$SOURCE_IP" -c 1 -W 1 \
		"$DEST_IP" >/dev/null 2>&1
}

originate_prefixes()
{
	local expected_node_prefixes
	local node

	configure_prefix "$SOURCE_NODE" "${TEST_PREFIX[$SOURCE_NODE]}"
	configure_prefix "$DEST_NODE" "${TEST_PREFIX[$DEST_NODE]}"
	wait_field "$SOURCE_NODE" "show midr prefix summary" "contributors:      1"
	wait_field "$DEST_NODE" "show midr prefix summary" "contributors:      1"
	for node in "${NODES[@]}"; do
		if [[ "$SCENARIO" == "cross-group" ]]; then
			case "${GROUP_ID[$node]}" in
			10 | 30) expected_node_prefixes=1 ;;
			20) expected_node_prefixes=0 ;;
			esac
		else
			expected_node_prefixes=2
		fi
		wait_field "$node" "show midr ted summary" \
			"node-prefixes:         $expected_node_prefixes"
		if [[ "$SCENARIO" == "cross-group" ]]; then
			wait_field "$node" "show midr ted summary" \
				"prefix-groups:         2"
		fi
	done
	if [[ "$SCENARIO" == "cross-group" ]]; then
		pass "Node Prefixes stayed group-local while two Group Prefix mappings propagated globally"
	else
		pass "node Prefix objects propagated into every TED"
	fi
}

verify_initial_forwarding()
{
	if [[ "$SCENARIO" == "line" ]]; then
		wait_for "r1 MIDR route to r3" route_matches r1 "${TEST_PREFIX[r3]}" \
			10.12.0.2 r1r2
		wait_for "r2 MIDR route to r3" route_matches r2 "${TEST_PREFIX[r3]}" \
			10.23.0.2 r2r3
		wait_for "r3 MIDR route to r1" route_matches r3 "${TEST_PREFIX[r1]}" \
			10.23.0.1 r3r2
		pass "SPF routes reached all three Linux FIBs through group-3 ZAPI"
		wait_for "two-hop r1-to-r3 packet forwarding" ping_end_to_end
		pass "packets traverse r1 -> r2 -> r3 using MIDR routes"
		return
	fi

	wait_for "r1 ECMP route to r5 through r2" route_matches r1 \
		"${TEST_PREFIX[r5]}" 10.12.0.2 r1r2
	wait_for "r1 ECMP route to r5 through r3" route_matches r1 \
		"${TEST_PREFIX[r5]}" 10.13.0.2 r1r3
	wait_for "r2 route to r5 through r4" route_matches r2 \
		"${TEST_PREFIX[r5]}" 10.24.0.2 r2r4
	wait_for "r3 route to r5 through r4" route_matches r3 \
		"${TEST_PREFIX[r5]}" 10.34.0.2 r3r4
	wait_for "r4 route to r5" route_matches r4 \
		"${TEST_PREFIX[r5]}" 10.45.0.2 r4r5
	wait_for "r5 reverse route to r1 through r4" route_matches r5 \
		"${TEST_PREFIX[r1]}" 10.45.0.1 r5r4
	if [[ "$SCENARIO" == "cross-group" ]]; then
		wait_for "r4 cross-group route to r1 through r2" route_matches r4 \
			"${TEST_PREFIX[r1]}" 10.24.0.1 r4r2
		wait_for "r4 cross-group route to r1 through r3" route_matches r4 \
			"${TEST_PREFIX[r1]}" 10.34.0.1 r4r3
		pass "Group SPF selected Group 20 and installed equal-cost physical exits"
	else
		pass "SPF installed two equal-cost r1 first-hops and downstream proto-199 routes"
	fi
	wait_for "three-hop r1-to-r5 packet forwarding" ping_end_to_end
	pass "packets cross the five-node ECMP topology using MIDR routes"
}

verify_prefix_withdraw_and_restore()
{
	withdraw_prefix "$DEST_NODE" "${TEST_PREFIX[$DEST_NODE]}"
	if [[ "$SCENARIO" == "cross-group" ]]; then
		wait_field "$SOURCE_NODE" "show midr ted summary" "prefix-groups:         1"
	else
		wait_field "$SOURCE_NODE" "show midr ted summary" "node-prefixes:         1"
	fi
	wait_for "$SOURCE_NODE FIB withdrawal after Prefix withdrawal" route_absent \
		"$SOURCE_NODE" "${TEST_PREFIX[$DEST_NODE]}"
	wait_for "forwarding failure after Prefix withdrawal" ping_fails
	pass "Prefix withdrawal propagated through TED/SPF and removed the FIB route"

	configure_prefix "$DEST_NODE" "${TEST_PREFIX[$DEST_NODE]}"
	if [[ "$SCENARIO" == "cross-group" ]]; then
		wait_field "$SOURCE_NODE" "show midr ted summary" "prefix-groups:         2"
	else
		wait_field "$SOURCE_NODE" "show midr ted summary" "node-prefixes:         2"
	fi
	wait_for "$SOURCE_NODE FIB restoration after Prefix re-origination" \
		route_matches "$SOURCE_NODE" "${TEST_PREFIX[$DEST_NODE]}" 10.12.0.2 r1r2
	if [[ "$SCENARIO" != "line" ]]; then
		wait_for "r1 second ECMP next-hop restoration" route_matches r1 \
			"${TEST_PREFIX[r5]}" 10.13.0.2 r1r3
	fi
	wait_for "forwarding restoration after Prefix re-origination" ping_end_to_end
	pass "Prefix re-origination restored SPF forwarding"
}

verify_line_link_withdraw_and_restore()
{
	vty r1 -c \
		"midr topology link withdraw ${ROUTER_ID[r1]} ${ROUTER_ID[r2]} id 12 version 2" \
		>/dev/null
	wait_field r1 "show midr ted summary" "intra links:           3"
	wait_for "r1 FIB withdrawal after Link withdrawal" route_absent r1 "${TEST_PREFIX[r3]}"
	wait_for "forwarding failure after Link withdrawal" ping_fails
	pass "directed Link withdrawal made the SPF destination unreachable"

	inject_link r1 "${ROUTER_ID[r2]}" 12 10.12.0.1 10.12.0.2 r1r2 3
	wait_field r1 "show midr ted summary" "intra links:           4"
	wait_for "r1 FIB restoration after Link re-origination" \
		route_matches r1 "${TEST_PREFIX[r3]}" 10.12.0.2 r1r2
	wait_for "forwarding restoration after Link re-origination" ping_end_to_end
	pass "Link re-origination restored TED, SPF, FIB and packet forwarding"
}

verify_complex_link_withdraw_and_restore()
{
	local first_label="intra links:           "
	local first_withdraw_count=9
	local first_restore_count=10
	local second_label="intra links:           "
	local second_withdraw_count=9
	local second_restore_count=10

	if [[ "$SCENARIO" == "cross-group" ]]; then
		first_withdraw_count=3
		first_restore_count=4
		second_label="egress links:          "
		second_withdraw_count=1
		second_restore_count=2
	fi

	vty r1 -c \
		"midr topology link withdraw ${ROUTER_ID[r1]} ${ROUTER_ID[r2]} id 12 version 2" \
		>/dev/null
	wait_field r1 "show midr ted summary" \
		"$first_label$first_withdraw_count"
	wait_for "r1 failover to lower ECMP branch" route_matches r1 \
		"${TEST_PREFIX[r5]}" 10.13.0.2 r1r3
	wait_for "r1 removal of failed upper branch" route_excludes r1 \
		"${TEST_PREFIX[r5]}" 10.12.0.2
	wait_for "forwarding after upper-branch failure" ping_end_to_end
	pass "r1->r2 withdrawal converged to the r1->r3->r4->r5 branch"

	inject_link r1 "${ROUTER_ID[r2]}" 12 10.12.0.1 10.12.0.2 r1r2 3
	wait_field r1 "show midr ted summary" \
		"$first_label$first_restore_count"
	wait_for "upper ECMP branch restoration" route_matches r1 \
		"${TEST_PREFIX[r5]}" 10.12.0.2 r1r2
	wait_for "lower ECMP branch retained" route_matches r1 \
		"${TEST_PREFIX[r5]}" 10.13.0.2 r1r3
	wait_for "forwarding after upper-branch restoration" ping_end_to_end
	pass "r1->r2 restoration recreated the two-way ECMP route"

	vty r3 -c \
		"midr topology link withdraw ${ROUTER_ID[r3]} ${ROUTER_ID[r4]} id 34 version 2" \
		>/dev/null
	wait_field r1 "show midr ted summary" \
		"$second_label$second_withdraw_count"
	wait_for "r1 failover to upper ECMP branch" route_matches r1 \
		"${TEST_PREFIX[r5]}" 10.12.0.2 r1r2
	wait_for "r1 removal of failed lower branch" route_excludes r1 \
		"${TEST_PREFIX[r5]}" 10.13.0.2
	wait_for "forwarding after lower-branch failure" ping_end_to_end
	pass "r3->r4 withdrawal converged to the r1->r2->r4->r5 branch"

	inject_link r3 "${ROUTER_ID[r4]}" 34 10.34.0.1 10.34.0.2 r3r4 3
	wait_field r1 "show midr ted summary" \
		"$second_label$second_restore_count"
	wait_for "upper ECMP branch retained after restoration" route_matches r1 \
		"${TEST_PREFIX[r5]}" 10.12.0.2 r1r2
	wait_for "lower ECMP branch restoration" route_matches r1 \
		"${TEST_PREFIX[r5]}" 10.13.0.2 r1r3
	wait_for "forwarding after lower-branch restoration" ping_end_to_end
	pass "r3->r4 restoration recreated the two-way ECMP route"
}

check_logs()
{
	local node
	local pattern='assertion .* failed|Segmentation fault|AddressSanitizer|core dumped'

	for node in "${NODES[@]}"; do
		if grep -Ei "$pattern" "$RUN_DIR/$node/bgpd.log" "$RUN_DIR/$node/zebra.log" \
			>/dev/null; then
			fail "fatal daemon output detected for $node"
		fi
	done
	pass "daemon logs contain no fatal failure pattern"
}

main()
{
	require_environment
	prepare_run_dir
	create_namespaces
	start_daemons
	activate_midr_sessions
	inject_topology
	originate_prefixes
	verify_initial_forwarding
	verify_prefix_withdraw_and_restore
	if [[ "$SCENARIO" == "line" ]]; then
		verify_line_link_withdraw_and_restore
	else
		verify_complex_link_withdraw_and_restore
	fi
	check_logs
	printf '\nPASS: MIDR %s multi-node SPF end-to-end test (%d checks)\n' \
		"$SCENARIO" "$PASS_COUNT"
}

main "$@"
