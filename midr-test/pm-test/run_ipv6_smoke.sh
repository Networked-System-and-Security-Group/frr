#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
CC_BIN="${CC:-cc}"

if [[ -z "${PYTHON_BIN:-}" ]]; then
    if [[ -n "${CONDA_PREFIX:-}" && -x "$CONDA_PREFIX/bin/python3" ]]; then
        PYTHON_BIN="$CONDA_PREFIX/bin/python3"
    else
        PYTHON_BIN="python3"
    fi
fi

ADDRESS_FAMILY="ipv6"

while (( $# > 0 )); do
    case "$1" in
        --address-family)
            ADDRESS_FAMILY="${2:-}"
            shift 2
            ;;
        -h|--help)
            echo "Usage: sudo -E ./midr-test/pm-test/run_ipv6_smoke.sh [--address-family ipv4|ipv6|all]"
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 2
            ;;
    esac
done

if (( EUID != 0 )); then
    echo "Run this script with sudo -E." >&2
    exit 1
fi

if [[ "$ADDRESS_FAMILY" == "all" ]]; then
    "$SCRIPT_DIR/run_ipv6_smoke.sh" --address-family ipv4
    "$SCRIPT_DIR/run_ipv6_smoke.sh" --address-family ipv6
    exit 0
fi

case "$ADDRESS_FAMILY" in
    ipv4)
        NS_A="midr-pms4-a"
        NS_B="midr-pms4-b"
        IF_A="pms4-a"
        IF_B="pms4-b"
        LINK_A="10.10.1.1"
        LINK_B="10.10.1.2"
        LINK_PREFIX=30
        TRANSPORT_A="10.99.0.1"
        TRANSPORT_B="10.99.0.2"
        TRANSPORT_PREFIX=32
        UNKNOWN_A="10.99.0.99"
        IP_FLAG=-4
        PING_FLAG=-4
        PCAP_FILTER='ip and udp port 5860'
        ;;
    ipv6)
        NS_A="midr-pms6-a"
        NS_B="midr-pms6-b"
        IF_A="pms6-a"
        IF_B="pms6-b"
        LINK_A="fd00:20::1"
        LINK_B="fd00:20::2"
        LINK_PREFIX=64
        TRANSPORT_A="fd00:99::a"
        TRANSPORT_B="fd00:99::b"
        TRANSPORT_PREFIX=128
        UNKNOWN_A="fd00:dead::1"
        IP_FLAG=-6
        PING_FLAG=-6
        PCAP_FILTER='ip6 and udp port 5860'
        ;;
    *)
        echo "Unsupported address family: $ADDRESS_FAMILY" >&2
        exit 2
        ;;
esac

RUN_ID="$(date -u +%Y%m%d-%H%M%S)-$$"
ARTIFACT_ROOT="$SCRIPT_DIR/artifacts"
ARTIFACT_DIR="$ARTIFACT_ROOT/$RUN_ID-$ADDRESS_FAMILY-smoke"
SMOKE_BIN="$ARTIFACT_DIR/pm-af-smoke"
SERVER_PID=""
TCPDUMP_PID=""
CLEANED_UP=0

export MPLCONFIGDIR="$ARTIFACT_DIR/matplotlib"
mkdir -p "$ARTIFACT_DIR" "$MPLCONFIGDIR"
ln -sfn "$(basename "$ARTIFACT_DIR")" "$ARTIFACT_ROOT/latest-smoke"
exec > >(tee "$ARTIFACT_DIR/test.log") 2>&1

namespace_exists() {
    ip netns list | grep -Eq "^$1([[:space:]]|$)"
}

stop_pid() {
    local pid="$1"
    local signal="${2:-TERM}"
    local attempt
    local process_state

    [[ -n "$pid" ]] || return 0
    kill -0 "$pid" 2>/dev/null || return 0
    kill "-$signal" "$pid" 2>/dev/null || true
    for attempt in {1..30}; do
        if [[ -r "/proc/$pid/stat" ]]; then
            read -r _ _ process_state _ <"/proc/$pid/stat" || true
            if [[ "$process_state" == "Z" ]]; then
                wait "$pid" 2>/dev/null || true
                return 0
            fi
        fi
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
    sleep 0.2
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
    stop_pid "$SERVER_PID"
    stop_pid "$TCPDUMP_PID"
    delete_namespace "$NS_A"
    delete_namespace "$NS_B"
    if [[ -n "${SUDO_UID:-}" && -n "${SUDO_GID:-}" ]]; then
        chown -R "$SUDO_UID:$SUDO_GID" "$ARTIFACT_DIR" 2>/dev/null || true
    fi
    exit "$status"
}
trap cleanup EXIT INT TERM

for command in ip ping tcpdump "$CC_BIN"; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

echo "[pm-smoke] compiling the standalone endpoint"
echo "[pm-smoke] address family: $ADDRESS_FAMILY"
echo "[pm-smoke] python: $PYTHON_BIN"
"$CC_BIN" -std=gnu11 -DHAVE_CONFIG_H -O2 -Wall -Wextra \
    -Wno-unused-parameter -I"$REPO_ROOT" -I"$REPO_ROOT/include" \
    -I"$REPO_ROOT/lib" "$SCRIPT_DIR/pm_ipv6_smoke.c" -o "$SMOKE_BIN"

"$SMOKE_BIN" classify "$ADDRESS_FAMILY" \
    | tee "$ARTIFACT_DIR/classification.log"

delete_namespace "$NS_A"
delete_namespace "$NS_B"
ip netns add "$NS_A"
ip netns add "$NS_B"
ip link add "$IF_A" type veth peer name "$IF_B"
ip link set "$IF_A" netns "$NS_A"
ip link set "$IF_B" netns "$NS_B"

for namespace in "$NS_A" "$NS_B"; do
    if [[ "$ADDRESS_FAMILY" == "ipv6" ]]; then
        ip netns exec "$namespace" sysctl -qw net.ipv6.conf.all.disable_ipv6=0
        ip netns exec "$namespace" sysctl -qw net.ipv6.conf.default.disable_ipv6=0
    else
        ip netns exec "$namespace" sysctl -qw net.ipv6.conf.all.disable_ipv6=1
        ip netns exec "$namespace" sysctl -qw net.ipv6.conf.default.disable_ipv6=1
    fi
    ip -n "$namespace" link set lo up
done

ip -n "$NS_A" link set "$IF_A" up
ip -n "$NS_B" link set "$IF_B" up
ip -n "$NS_A" addr add "$LINK_A/$LINK_PREFIX" dev "$IF_A"
ip -n "$NS_B" addr add "$LINK_B/$LINK_PREFIX" dev "$IF_B"
ip -n "$NS_A" addr add "$TRANSPORT_A/$TRANSPORT_PREFIX" dev lo
ip -n "$NS_B" addr add "$TRANSPORT_B/$TRANSPORT_PREFIX" dev lo
ip -n "$NS_A" addr add "$UNKNOWN_A/$TRANSPORT_PREFIX" dev lo
ip -n "$NS_A" "$IP_FLAG" route add "$TRANSPORT_B/$TRANSPORT_PREFIX" \
    via "$LINK_B" dev "$IF_A"
ip -n "$NS_B" "$IP_FLAG" route add "$TRANSPORT_A/$TRANSPORT_PREFIX" \
    via "$LINK_A" dev "$IF_B"
ip -n "$NS_B" "$IP_FLAG" route add "$UNKNOWN_A/$TRANSPORT_PREFIX" \
    via "$LINK_A" dev "$IF_B"

ip netns exec "$NS_A" ping "$PING_FLAG" -c 2 -W 1 "$TRANSPORT_B" >/dev/null
ip netns exec "$NS_B" ping "$PING_FLAG" -c 2 -W 1 "$TRANSPORT_A" >/dev/null

ip netns exec "$NS_A" tcpdump -U -i "$IF_A" -s 0 \
    -w "$ARTIFACT_DIR/pm-smoke.pcap" "$PCAP_FILTER" \
    >"$ARTIFACT_DIR/tcpdump.log" 2>&1 &
TCPDUMP_PID=$!

ip netns exec "$NS_B" "$SMOKE_BIN" server "$TRANSPORT_B" "$TRANSPORT_A" \
    >"$ARTIFACT_DIR/server.log" 2>&1 &
SERVER_PID=$!
sleep 0.5

ip netns exec "$NS_A" "$SMOKE_BIN" client "$TRANSPORT_A" "$TRANSPORT_B" \
    | tee "$ARTIFACT_DIR/client.log"
ip netns exec "$NS_A" "$SMOKE_BIN" unknown "$UNKNOWN_A" "$TRANSPORT_B" \
    | tee "$ARTIFACT_DIR/unknown.log"

if ! wait "$SERVER_PID"; then
    SERVER_PID=""
    echo "[pm-smoke] FAIL: standalone server rejected the test sequence" >&2
    sed -n '1,120p' "$ARTIFACT_DIR/server.log" >&2
    exit 1
fi
SERVER_PID=""
stop_pid "$TCPDUMP_PID"
TCPDUMP_PID=""

PACKET_COUNT="$(tcpdump -nr "$ARTIFACT_DIR/pm-smoke.pcap" \
    "$PCAP_FILTER" 2>/dev/null | wc -l)"
PACKET_COUNT="${PACKET_COUNT//[[:space:]]/}"

if (( PACKET_COUNT < 3 )); then
    echo "[pm-smoke] FAIL: capture contains only $PACKET_COUNT PM packets" >&2
    exit 1
fi
echo "PASS: capture contains $PACKET_COUNT $ADDRESS_FAMILY PM packets" \
    | tee "$ARTIFACT_DIR/assertions.txt"

if command -v "$PYTHON_BIN" >/dev/null 2>&1 \
    && "$PYTHON_BIN" -c 'import matplotlib' >/dev/null 2>&1; then
    "$PYTHON_BIN" "$SCRIPT_DIR/plot_pm_smoke.py" \
        "$ARTIFACT_DIR/classification.log" \
        "$ARTIFACT_DIR/server.log" \
        "$ARTIFACT_DIR/client.log" \
        "$ARTIFACT_DIR/unknown.log" \
        --packet-count "$PACKET_COUNT" \
        --address-family "$ADDRESS_FAMILY" \
        --output "$ARTIFACT_DIR/pm-$ADDRESS_FAMILY-smoke.png" \
        | tee -a "$ARTIFACT_DIR/assertions.txt"
else
    echo "[pm-smoke] plotting skipped: matplotlib is unavailable in $PYTHON_BIN" \
        | tee -a "$ARTIFACT_DIR/assertions.txt"
fi

echo "[pm-smoke] PASS"
echo "[pm-smoke] results: $ARTIFACT_DIR"
