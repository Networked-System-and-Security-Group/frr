#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AF_TEST_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BGPD="$REPO_ROOT/bgpd/bgpd"
VTYSH="$REPO_ROOT/vtysh/vtysh"
PYTHON_BIN="${PYTHON_BIN:-python3}"

SCENARIO="${1:-}"
if [[ "$SCENARIO" == "-h" || "$SCENARIO" == "--help" ]]; then
    SCENARIO=""
elif [[ -n "$SCENARIO" ]]; then
    shift
fi
ADDRESS_FAMILY="ipv6"

usage() {
    sed -n '/^Usage:/,/^$/p' <<'EOF'
Usage: sudo -E ./midr-test/af-test/nds/run_test.sh SCENARIO [OPTIONS]
  SCENARIO                  vty or control-smoke
  --address-family FAMILY  ipv4 or ipv6 (default: ipv6)
  -h, --help               Show this help

EOF
}

while (( $# > 0 )); do
    case "$1" in
        --address-family)
            ADDRESS_FAMILY="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ -z "$SCENARIO" ]]; then
    usage
    exit 0
fi

case "$SCENARIO" in
    vty|control-smoke)
        ;;
    *)
        echo "Unsupported scenario: ${SCENARIO:-<empty>}" >&2
        usage >&2
        exit 2
        ;;
esac

case "$ADDRESS_FAMILY" in
    ipv4)
        FAMILY_LABEL="IPv4"
        NAMESPACE="midr-af-nds4"
        LOCAL_TRANSPORT="10.99.1.1"
        REMOTE_TRANSPORT="10.99.1.2"
        FOREIGN_TRANSPORT="fd00:99:1::2"
        PREFIX_LENGTH="32"
        ADDR_FLAGS=()
        CAPTURE_FILTER="ip and port 5859"
        OTHER_CAPTURE_FILTER="ip6 and port 5859"
        ;;
    ipv6)
        FAMILY_LABEL="IPv6"
        NAMESPACE="midr-af-nds6"
        LOCAL_TRANSPORT="fd00:99:1::1"
        REMOTE_TRANSPORT="fd00:99:1::2"
        FOREIGN_TRANSPORT="10.99.1.2"
        PREFIX_LENGTH="128"
        ADDR_FLAGS=(nodad)
        CAPTURE_FILTER="ip6 and port 5859"
        OTHER_CAPTURE_FILTER="ip and port 5859"
        ;;
    *)
        echo "Unsupported address family: $ADDRESS_FAMILY" >&2
        exit 2
        ;;
esac

if (( EUID != 0 )); then
    echo "Run this script with sudo -E." >&2
    exit 1
fi
for command in ip ss tcpdump "$PYTHON_BIN"; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done
[[ -x "$BGPD" ]] || {
    echo "Missing bgpd binary: $BGPD" >&2
    exit 1
}
[[ -x "$VTYSH" ]] || {
    echo "Missing vtysh binary: $VTYSH" >&2
    exit 1
}

RUN_ID="${MIDR_AF_RUN_ID:-$(date -u +%Y%m%d-%H%M%S)-$$}"
ARTIFACT_DIR="$AF_TEST_DIR/artifacts/$RUN_ID/$ADDRESS_FAMILY/$SCENARIO"
VTY_DIR="$ARTIFACT_DIR/vty"
VTY_RESTART_DIR="$ARTIFACT_DIR/vty-restart"
VTY_CONFIG_DIR="$ARTIFACT_DIR/vty-config"
mkdir -p "$ARTIFACT_DIR" "$VTY_DIR" "$VTY_RESTART_DIR" \
    "$VTY_CONFIG_DIR"
touch "$VTY_CONFIG_DIR/vtysh.conf" "$VTY_CONFIG_DIR/frr.conf"
exec > >(tee "$ARTIFACT_DIR/test.log") 2>&1

BGPD_PID=""
TCPDUMP_PID=""
CLEANED_UP=0

namespace_exists() {
    ip netns list | grep -Eq "^$1([[:space:]]|$)"
}

stop_pid() {
    local pid="$1"
    local attempt

    [[ -n "$pid" ]] || return 0
    kill -0 "$pid" 2>/dev/null || return 0
    kill -TERM "$pid" 2>/dev/null || true
    for attempt in {1..30}; do
        if ! kill -0 "$pid" 2>/dev/null; then
            wait "$pid" 2>/dev/null || true
            return 0
        fi
        sleep 0.1
    done
    kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

delete_namespace() {
    local namespace="$1"
    local pid
    local -a pids=()

    namespace_exists "$namespace" || return 0
    mapfile -t pids < <(ip netns pids "$namespace")
    for pid in "${pids[@]}"; do
        kill -TERM "$pid" 2>/dev/null || true
    done
    sleep 0.3
    mapfile -t pids < <(ip netns pids "$namespace")
    for pid in "${pids[@]}"; do
        kill -KILL "$pid" 2>/dev/null || true
    done
    ip netns delete "$namespace"
}

cleanup() {
    local status=$?

    (( CLEANED_UP == 0 )) || return "$status"
    CLEANED_UP=1
    trap - EXIT INT TERM
    stop_pid "$TCPDUMP_PID"
    stop_pid "$BGPD_PID"
    delete_namespace "$NAMESPACE"
    if [[ -n "${SUDO_UID:-}" && -n "${SUDO_GID:-}" ]]; then
        chown -R "$SUDO_UID:$SUDO_GID" "$ARTIFACT_DIR" 2>/dev/null || true
    fi
    exit "$status"
}
trap cleanup EXIT INT TERM

wait_for_file() {
    local path="$1"
    local timeout="$2"
    local elapsed

    for (( elapsed = 0; elapsed < timeout; elapsed++ )); do
        [[ -e "$path" ]] && return 0
        sleep 1
    done
    echo "Timed out waiting for $path" >&2
    return 1
}

wait_for_pattern() {
    local path="$1"
    local pattern="$2"
    local timeout="$3"
    local elapsed

    for (( elapsed = 0; elapsed < timeout; elapsed++ )); do
        grep -Eq "$pattern" "$path" 2>/dev/null && return 0
        sleep 1
    done
    echo "Timed out waiting for '$pattern' in $path" >&2
    return 1
}

run_vty() {
    local socket_dir="$1"
    shift
    ip netns exec "$NAMESPACE" "$VTYSH" \
        --config_dir "$VTY_CONFIG_DIR" --vty_socket "$socket_dir" "$@"
}

assert_contains() {
    local path="$1"
    local value="$2"
    local message="$3"

    if grep -Fq -- "$value" "$path"; then
        echo "PASS: $message"
    else
        echo "FAIL: $message" >&2
        return 1
    fi
}

assert_not_contains() {
    local path="$1"
    local value="$2"
    local message="$3"

    if grep -Fq -- "$value" "$path"; then
        echo "FAIL: $message" >&2
        return 1
    fi
    echo "PASS: $message"
}

start_bgpd() {
    local config="$1"
    local socket_dir="$2"
    local console_log="$3"

    ip netns exec "$NAMESPACE" "$BGPD" -f "$config" -Z -S \
        -i "$ARTIFACT_DIR/bgpd.pid" --vty_socket "$socket_dir" \
        --db_file "$ARTIFACT_DIR/bgpd.db" \
        --log-level debug >"$console_log" 2>&1 &
    BGPD_PID=$!
    wait_for_file "$socket_dir/bgpd.vty" 20
    wait_for_pattern "$ARTIFACT_DIR/bgpd.log" \
        "UDP channel ready on $LOCAL_TRANSPORT:5859" 20
    wait_for_pattern "$ARTIFACT_DIR/bgpd.log" \
        "TCP list-exchange channel ready on $LOCAL_TRANSPORT:5859" 20
}

echo "[nds-$SCENARIO] address family: $ADDRESS_FAMILY"
echo "[nds-$SCENARIO] artifacts: $ARTIFACT_DIR"
delete_namespace "$NAMESPACE"
ip netns add "$NAMESPACE"
if [[ "$ADDRESS_FAMILY" == "ipv4" ]]; then
    ip netns exec "$NAMESPACE" sysctl -qw net.ipv6.conf.all.disable_ipv6=1
else
    ip netns exec "$NAMESPACE" sysctl -qw net.ipv6.conf.all.disable_ipv6=0
fi
ip -n "$NAMESPACE" link set lo up
ip -n "$NAMESPACE" addr add "$LOCAL_TRANSPORT/$PREFIX_LENGTH" dev lo \
    "${ADDR_FLAGS[@]}"
if [[ "$SCENARIO" == "control-smoke" ]]; then
    ip -n "$NAMESPACE" addr add "$REMOTE_TRANSPORT/$PREFIX_LENGTH" dev lo \
        "${ADDR_FLAGS[@]}"
fi

cat >"$ARTIFACT_DIR/bgpd.conf" <<EOF
frr version 10.0
hostname nds-node
log file $ARTIFACT_DIR/bgpd.log debugging
log timestamp precision 3
debug bgp midr

router bgp 65101
  bgp router-id 10.255.1.1
  no bgp ebgp-requires-policy
  midr group-id 11
  midr transport-address $LOCAL_TRANSPORT
!
EOF

start_bgpd "$ARTIFACT_DIR/bgpd.conf" "$VTY_DIR" \
    "$ARTIFACT_DIR/bgpd.console.log"
run_vty "$VTY_DIR" -c 'show midr self' >"$ARTIFACT_DIR/show-self.txt"
ip netns exec "$NAMESPACE" ss -l -n -t -u -p \
    >"$ARTIFACT_DIR/sockets.txt"
ip -n "$NAMESPACE" addr show >"$ARTIFACT_DIR/addresses.txt"
if [[ "$ADDRESS_FAMILY" == "ipv4" ]]; then
    ip -n "$NAMESPACE" -6 addr show scope global \
        >"$ARTIFACT_DIR/foreign-global-addresses.txt"
else
    ip -n "$NAMESPACE" -4 addr show scope global \
        >"$ARTIFACT_DIR/foreign-global-addresses.txt"
fi

assert_contains "$ARTIFACT_DIR/show-self.txt" \
    "Address family    : $FAMILY_LABEL" "show reports the selected family"
assert_contains "$ARTIFACT_DIR/show-self.txt" \
    "Active locator    : $LOCAL_TRANSPORT" "the transport is active"
CONTROL_SOCKET_COUNT="$(grep -F "$LOCAL_TRANSPORT" \
    "$ARTIFACT_DIR/sockets.txt" | grep -c '5859' || true)"
if (( CONTROL_SOCKET_COUNT < 2 )); then
    echo "FAIL: expected exact-address TCP and UDP Control sockets" >&2
    exit 1
fi
echo "PASS: Control TCP and UDP use the exact transport"
assert_not_contains "$ARTIFACT_DIR/sockets.txt" "0.0.0.0:5859" \
    "Control does not bind the IPv4 wildcard"
assert_not_contains "$ARTIFACT_DIR/sockets.txt" "[::]:5859" \
    "Control does not bind the IPv6 wildcard"
if [[ -s "$ARTIFACT_DIR/foreign-global-addresses.txt" ]]; then
    echo "FAIL: the namespace contains a foreign-family global address" >&2
    exit 1
fi
echo "PASS: the namespace has no foreign-family global address"
assert_not_contains "$ARTIFACT_DIR/addresses.txt" "10.255.1.1" \
    "the BGP Identifier is not configured on an interface"

if [[ "$SCENARIO" == "vty" ]]; then
    run_vty "$VTY_DIR" -c 'configure terminal' -c 'router bgp 65101' \
        -c "midr bootstrap $REMOTE_TRANSPORT remote-as 65102 router-id 10.255.1.2" \
        -c "midr session $REMOTE_TRANSPORT remote-as 65102" \
        >"$ARTIFACT_DIR/configure-valid.txt"
    touch "$ARTIFACT_DIR/family-guard.txt"
    for foreign_command in \
        "midr transport-address $FOREIGN_TRANSPORT" \
        "midr bootstrap $FOREIGN_TRANSPORT remote-as 65102 router-id 10.255.1.2" \
        "midr session $FOREIGN_TRANSPORT remote-as 65102"; do
        run_vty "$VTY_DIR" -c 'configure terminal' \
            -c 'router bgp 65101' -c "$foreign_command" \
            >>"$ARTIFACT_DIR/family-guard.txt" 2>&1 || true
    done
    run_vty "$VTY_DIR" -c 'show running-config' \
        >"$ARTIFACT_DIR/running-config.txt"
    run_vty "$VTY_DIR" -c 'show midr self' \
        >"$ARTIFACT_DIR/show-self-after-guard.txt"

    assert_contains "$ARTIFACT_DIR/running-config.txt" \
        "midr transport-address $LOCAL_TRANSPORT" \
        "running config preserves the local transport"
    assert_contains "$ARTIFACT_DIR/running-config.txt" \
        "midr bootstrap $REMOTE_TRANSPORT remote-as 65102 router-id 10.255.1.2" \
        "running config preserves the bootstrap locator"
    assert_contains "$ARTIFACT_DIR/running-config.txt" \
        "midr session $REMOTE_TRANSPORT remote-as 65102" \
        "running config preserves the manual session"
    assert_contains "$ARTIFACT_DIR/family-guard.txt" \
        "locator family differs from local transport" \
        "cross-family locator changes are rejected"
    assert_not_contains "$ARTIFACT_DIR/running-config.txt" \
        "$FOREIGN_TRANSPORT" "rejected locators do not enter configuration"
    assert_contains "$ARTIFACT_DIR/show-self-after-guard.txt" \
        "Address family    : $FAMILY_LABEL" \
        "the instance family remains unchanged"

    awk '/^frr version / { copy = 1 } copy { print }' \
        "$ARTIFACT_DIR/running-config.txt" >"$ARTIFACT_DIR/restored.conf"
    stop_pid "$BGPD_PID"
    BGPD_PID=""
    start_bgpd "$ARTIFACT_DIR/restored.conf" "$VTY_RESTART_DIR" \
        "$ARTIFACT_DIR/bgpd-restart.console.log"
    run_vty "$VTY_RESTART_DIR" -c 'show running-config' \
        >"$ARTIFACT_DIR/running-config-restored.txt"
    run_vty "$VTY_RESTART_DIR" -c 'show midr self' \
        >"$ARTIFACT_DIR/show-self-restored.txt"
    assert_contains "$ARTIFACT_DIR/show-self-restored.txt" \
        "Address family    : $FAMILY_LABEL" \
        "the selected family survives restart"
    assert_contains "$ARTIFACT_DIR/show-self-restored.txt" \
        "Configured locator: $LOCAL_TRANSPORT" \
        "the transport survives restart"
    assert_contains "$ARTIFACT_DIR/running-config-restored.txt" \
        "midr bootstrap $REMOTE_TRANSPORT remote-as 65102 router-id 10.255.1.2" \
        "the bootstrap entry survives restart"
    assert_contains "$ARTIFACT_DIR/running-config-restored.txt" \
        "midr session $REMOTE_TRANSPORT remote-as 65102" \
        "the manual session survives restart"
else
    ip netns exec "$NAMESPACE" tcpdump -U -i lo -s 0 -w \
        "$ARTIFACT_DIR/control.pcap" 'port 5859' \
        >"$ARTIFACT_DIR/tcpdump.log" 2>&1 &
    TCPDUMP_PID=$!
    sleep 1
    ip netns exec "$NAMESPACE" "$PYTHON_BIN" \
        "$SCRIPT_DIR/inject_control.py" \
        --address-family "$ADDRESS_FAMILY" \
        --source "$REMOTE_TRANSPORT" --destination "$LOCAL_TRANSPORT"
    sleep 1
    stop_pid "$TCPDUMP_PID"
    TCPDUMP_PID=""

    assert_contains "$ARTIFACT_DIR/bgpd.log" \
        "ANNOUNCE from $REMOTE_TRANSPORT" \
        "a valid version-4 UDP request is decoded"
    assert_contains "$ARTIFACT_DIR/bgpd.log" \
        "invalid UDP request from $REMOTE_TRANSPORT" \
        "invalid UDP frames are rejected"
    assert_contains "$ARTIFACT_DIR/bgpd.log" \
        "ignoring REP_LIST_REQ from $REMOTE_TRANSPORT" \
        "a valid version-4 TCP request is decoded"
    assert_contains "$ARTIFACT_DIR/bgpd.log" "bad frame_len 1" \
        "invalid TCP frame lengths are rejected"

    SELECTED_COUNT="$(tcpdump -nr "$ARTIFACT_DIR/control.pcap" \
        "$CAPTURE_FILTER" 2>/dev/null | wc -l)"
    OTHER_COUNT="$(tcpdump -nr "$ARTIFACT_DIR/control.pcap" \
        "$OTHER_CAPTURE_FILTER" 2>/dev/null | wc -l)"
    if (( SELECTED_COUNT < 4 )); then
        echo "FAIL: capture contains only $SELECTED_COUNT selected-family packets" >&2
        exit 1
    fi
    echo "PASS: capture contains $SELECTED_COUNT $ADDRESS_FAMILY Control packets"
    if (( OTHER_COUNT != 0 )); then
        echo "FAIL: capture contains $OTHER_COUNT cross-family packets" >&2
        exit 1
    fi
    echo "PASS: capture contains no cross-family Control packets"
fi

echo "[nds-$SCENARIO] PASS"
echo "[nds-$SCENARIO] results: $ARTIFACT_DIR"
