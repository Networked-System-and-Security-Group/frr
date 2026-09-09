#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BGPD="$REPO_ROOT/bgpd/.libs/bgpd"
VTYSH="$REPO_ROOT/vtysh/.libs/vtysh"

if [[ -z "${PYTHON_BIN:-}" ]]; then
    if [[ -n "${CONDA_PREFIX:-}" && -x "$CONDA_PREFIX/bin/python3" ]]; then
        PYTHON_BIN="$CONDA_PREFIX/bin/python3"
    else
        PYTHON_BIN="python3"
    fi
fi

CASE="baseline"
ADDRESS_FAMILY="ipv6"
DURATION=""
DELAY_MS=30
LOSS_PERCENT=20
IPERF_ENABLE=0
IPERF_START_TIME=10
IPERF_DURATION=30
IPERF_BANDWIDTH=10M

NS_A=""
NS_B=""
IF_A=""
IF_B=""
TRANSPORT_A=""
TRANSPORT_B=""
UNKNOWN_SOURCE=""
LINK_A=""
LINK_B=""
LINK_PREFIX=""
TRANSPORT_PREFIX=""
IP_FAMILY_FLAG=""
SS_FAMILY_FLAG=""
CAPTURE_FILTER=""
CAPTURE_FILE=""
ADDR_FLAGS=()

RUN_ID=""
ARTIFACT_ROOT=""
ARTIFACT_DIR=""
VTY_A=""
VTY_B=""

PID_A=""
PID_B=""
TCPDUMP_PID=""
IPERF_SERVER_PID=""
IPERF_CLIENT_PID=""
CLEANED_UP=0

usage() {
    sed -n '/^Usage:/,/^$/p' <<'EOF'
Usage: sudo -E ./midr-test/pm-test/run_ipv6_test.sh [OPTIONS]
  --address-family FAMILY  ipv4 or ipv6 (default: ipv6)
  --case CASE              baseline, impairment, or invalid-source
  --duration SECONDS       Measurement duration
  --delay-ms MS            Per-direction netem delay (default: 30)
  --loss-percent PERCENT   Per-direction netem loss (default: 20)
  --iperf                  Run same-family UDP background traffic
  --iperf-start-time SEC   Delay before iperf starts (default: 10)
  --iperf-duration SEC     iperf duration (default: 30)
  --iperf-bandwidth RATE   iperf UDP rate (default: 10M)
  -h, --help               Show this help

EOF
}

while (( $# > 0 )); do
    case "$1" in
        --address-family)
            ADDRESS_FAMILY="$2"
            shift 2
            ;;
        --case)
            CASE="$2"
            shift 2
            ;;
        --duration)
            DURATION="$2"
            shift 2
            ;;
        --delay-ms)
            DELAY_MS="$2"
            shift 2
            ;;
        --loss-percent)
            LOSS_PERCENT="$2"
            shift 2
            ;;
        --iperf)
            IPERF_ENABLE=1
            shift
            ;;
        --iperf-start-time)
            IPERF_START_TIME="$2"
            shift 2
            ;;
        --iperf-duration)
            IPERF_DURATION="$2"
            shift 2
            ;;
        --iperf-bandwidth)
            IPERF_BANDWIDTH="$2"
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

case "$CASE" in
    baseline)
        DURATION="${DURATION:-120}"
        MIN_DURATION=30
        ;;
    impairment)
        DURATION="${DURATION:-180}"
        MIN_DURATION=70
        ;;
    invalid-source)
        DURATION="${DURATION:-90}"
        MIN_DURATION=30
        ;;
    *)
        echo "Unsupported case: $CASE" >&2
        exit 2
        ;;
esac

case "$ADDRESS_FAMILY" in
    ipv4)
        NS_A="midr-pm4-a"
        NS_B="midr-pm4-b"
        IF_A="pm4-a"
        IF_B="pm4-b"
        TRANSPORT_A="10.99.0.1"
        TRANSPORT_B="10.99.0.2"
        UNKNOWN_SOURCE="10.222.0.1"
        LINK_A="10.10.1.1"
        LINK_B="10.10.1.2"
        LINK_PREFIX="30"
        TRANSPORT_PREFIX="32"
        IP_FAMILY_FLAG="-4"
        SS_FAMILY_FLAG="-4"
        CAPTURE_FILTER="ip and udp port 5860"
        CAPTURE_FILE="pm-ipv4.pcap"
        ;;
    ipv6)
        NS_A="midr-pm6-a"
        NS_B="midr-pm6-b"
        IF_A="pm6-a"
        IF_B="pm6-b"
        TRANSPORT_A="fd00:99::a"
        TRANSPORT_B="fd00:99::b"
        UNKNOWN_SOURCE="fd00:dead::1"
        LINK_A="fd00:10:1::1"
        LINK_B="fd00:10:1::2"
        LINK_PREFIX="64"
        TRANSPORT_PREFIX="128"
        IP_FAMILY_FLAG="-6"
        SS_FAMILY_FLAG="-6"
        CAPTURE_FILTER="ip6 and udp port 5860"
        CAPTURE_FILE="pm-ipv6.pcap"
        ADDR_FLAGS=(nodad)
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
if ! [[ "$DURATION" =~ ^[0-9]+$ ]] || (( DURATION < MIN_DURATION )); then
    echo "$CASE requires --duration >= $MIN_DURATION." >&2
    exit 2
fi

RUN_ID="$(date -u +%Y%m%d-%H%M%S)-$$"
ARTIFACT_ROOT="$SCRIPT_DIR/artifacts"
ARTIFACT_DIR="$ARTIFACT_ROOT/$RUN_ID-$ADDRESS_FAMILY-$CASE"
VTY_A="$ARTIFACT_DIR/vty-a"
VTY_B="$ARTIFACT_DIR/vty-b"
export MPLCONFIGDIR="$ARTIFACT_DIR/matplotlib"
mkdir -p "$ARTIFACT_DIR" "$VTY_A" "$VTY_B" "$MPLCONFIGDIR"
ln -sfn "$(basename "$ARTIFACT_DIR")" \
    "$ARTIFACT_ROOT/latest-$ADDRESS_FAMILY"
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
    stop_pid "$IPERF_CLIENT_PID"
    stop_pid "$IPERF_SERVER_PID"
    stop_pid "$TCPDUMP_PID"
    stop_pid "$PID_A"
    stop_pid "$PID_B"
    delete_namespace "$NS_A"
    delete_namespace "$NS_B"
    if [[ -n "${SUDO_UID:-}" && -n "${SUDO_GID:-}" ]]; then
        chown -R "$SUDO_UID:$SUDO_GID" "$ARTIFACT_DIR" 2>/dev/null || true
    fi
    exit "$status"
}
trap cleanup EXIT INT TERM

for command in ip tc ss tcpdump ping "$PYTHON_BIN"; do
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
if (( IPERF_ENABLE == 1 )); then
    command -v iperf3 >/dev/null || {
        echo "Missing command: iperf3" >&2
        exit 1
    }
fi

echo "[run_ipv6_test] address family: $ADDRESS_FAMILY"
echo "[run_ipv6_test] case: $CASE"
echo "[run_ipv6_test] python: $PYTHON_BIN"
echo "[run_ipv6_test] duration: ${DURATION}s"
echo "[run_ipv6_test] artifacts: $ARTIFACT_DIR"
echo "[run_ipv6_test] transport: $TRANSPORT_A <-> $TRANSPORT_B"
echo "[run_ipv6_test] underlay: $LINK_A <-> $LINK_B"

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
    else
        ip netns exec "$namespace" sysctl -qw net.ipv6.conf.all.disable_ipv6=1
    fi
    ip -n "$namespace" link set lo up
done

ip -n "$NS_A" link set "$IF_A" up
ip -n "$NS_B" link set "$IF_B" up
ip -n "$NS_A" addr add "$LINK_A/$LINK_PREFIX" dev "$IF_A" \
    "${ADDR_FLAGS[@]}"
ip -n "$NS_B" addr add "$LINK_B/$LINK_PREFIX" dev "$IF_B" \
    "${ADDR_FLAGS[@]}"
ip -n "$NS_A" addr add "$TRANSPORT_A/$TRANSPORT_PREFIX" dev lo \
    "${ADDR_FLAGS[@]}"
ip -n "$NS_B" addr add "$TRANSPORT_B/$TRANSPORT_PREFIX" dev lo \
    "${ADDR_FLAGS[@]}"
ip -n "$NS_A" "$IP_FAMILY_FLAG" route add \
    "$TRANSPORT_B/$TRANSPORT_PREFIX" via "$LINK_B" dev "$IF_A"
ip -n "$NS_B" "$IP_FAMILY_FLAG" route add \
    "$TRANSPORT_A/$TRANSPORT_PREFIX" via "$LINK_A" dev "$IF_B"

ip netns exec "$NS_A" ping "$IP_FAMILY_FLAG" -c 2 -W 1 \
    "$TRANSPORT_B" >/dev/null
ip netns exec "$NS_B" ping "$IP_FAMILY_FLAG" -c 2 -W 1 \
    "$TRANSPORT_A" >/dev/null

cat >"$ARTIFACT_DIR/bgpd-a.conf" <<EOF
frr version 10.0
hostname node-a
log file $ARTIFACT_DIR/bgpd-a.log debugging
log timestamp precision 3

debug bgp midr
debug bgp link-state

router bgp 65001
  bgp router-id 10.255.0.1
  no bgp ebgp-requires-policy
  no bgp network import-check
  neighbor $LINK_B remote-as 65002
  neighbor $LINK_B ebgp-multihop 5
  neighbor $LINK_B update-source $LINK_A
  address-family link-state link-state
    distribute bgp-fabric-link-state
    neighbor $LINK_B activate
  exit-address-family
  midr transport-address $TRANSPORT_A
  midr group-id 1
  midr role group-rep
!
EOF

cat >"$ARTIFACT_DIR/bgpd-b.conf" <<EOF
frr version 10.0
hostname node-b
log file $ARTIFACT_DIR/bgpd-b.log debugging
log timestamp precision 3

debug bgp midr
debug bgp link-state

router bgp 65002
  bgp router-id 10.255.0.2
  no bgp ebgp-requires-policy
  no bgp network import-check
  neighbor $LINK_A remote-as 65001
  neighbor $LINK_A ebgp-multihop 5
  neighbor $LINK_A update-source $LINK_B
  address-family link-state link-state
    distribute bgp-fabric-link-state
    neighbor $LINK_A activate
  exit-address-family
  midr transport-address $TRANSPORT_B
  midr group-id 1
!
EOF

ip netns exec "$NS_A" tcpdump -U -i "$IF_A" -s 0 -w \
    "$ARTIFACT_DIR/$CAPTURE_FILE" "$CAPTURE_FILTER" \
    >"$ARTIFACT_DIR/tcpdump.log" 2>&1 &
TCPDUMP_PID=$!

ip netns exec "$NS_A" "$BGPD" -f "$ARTIFACT_DIR/bgpd-a.conf" -Z -S \
    -i "$ARTIFACT_DIR/bgpd-a.pid" --vty_socket "$VTY_A" \
    --log-level debug >"$ARTIFACT_DIR/bgpd-a.console.log" 2>&1 &
PID_A=$!
ip netns exec "$NS_B" "$BGPD" -f "$ARTIFACT_DIR/bgpd-b.conf" -Z -S \
    -i "$ARTIFACT_DIR/bgpd-b.pid" --vty_socket "$VTY_B" \
    --log-level debug >"$ARTIFACT_DIR/bgpd-b.console.log" 2>&1 &
PID_B=$!

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
    local namespace="$1"
    local socket_dir="$2"
    shift 2
    ip netns exec "$namespace" "$VTYSH" --vty_socket "$socket_dir" "$@"
}

wait_for_file "$VTY_A/bgpd.vty" 20
wait_for_file "$VTY_B/bgpd.vty" 20
wait_for_pattern "$ARTIFACT_DIR/bgpd-a.log" \
    'registered node/link callbacks|已向第二组注册 node/link 回调' 75
wait_for_pattern "$ARTIFACT_DIR/bgpd-b.log" \
    'registered node/link callbacks|已向第二组注册 node/link 回调' 75

echo "[run_ipv6_test] creating symmetric $ADDRESS_FAMILY MIDR overlay sessions"
run_vty "$NS_A" "$VTY_A" -c 'configure terminal' -c 'router bgp 65001' \
    -c "midr session $TRANSPORT_B remote-as 65002"
run_vty "$NS_B" "$VTY_B" -c 'configure terminal' -c 'router bgp 65002' \
    -c "midr session $TRANSPORT_A remote-as 65001"

wait_for_pattern "$ARTIFACT_DIR/bgpd-a.log" \
    "MIDR PM I-1: start probing .* -> $TRANSPORT_B" 90
wait_for_pattern "$ARTIFACT_DIR/bgpd-b.log" \
    "MIDR PM I-1: start probing .* -> $TRANSPORT_A" 90
wait_for_pattern "$ARTIFACT_DIR/bgpd-a.log" \
    "MIDR PM: reply from $TRANSPORT_B" 20
wait_for_pattern "$ARTIFACT_DIR/bgpd-b.log" \
    "MIDR PM: reply from $TRANSPORT_A" 20

ip netns exec "$NS_A" ss "$SS_FAMILY_FLAG" -u -l -n -p \
    >"$ARTIFACT_DIR/ss-node-a.txt"
ip netns exec "$NS_B" ss "$SS_FAMILY_FLAG" -u -l -n -p \
    >"$ARTIFACT_DIR/ss-node-b.txt"

if (( IPERF_ENABLE == 1 )); then
    IPERF_FAMILY_ARGS=()
    if [[ "$ADDRESS_FAMILY" == "ipv6" ]]; then
        IPERF_FAMILY_ARGS=(-6)
    fi
    ip netns exec "$NS_B" iperf3 "${IPERF_FAMILY_ARGS[@]}" \
        -s -1 -B "$LINK_B" -p 5201 \
        >"$ARTIFACT_DIR/iperf-server.log" 2>&1 &
    IPERF_SERVER_PID=$!
    (
        sleep "$IPERF_START_TIME"
        ip netns exec "$NS_A" iperf3 "${IPERF_FAMILY_ARGS[@]}" \
            -c "$LINK_B" -B "$LINK_A" \
            -p 5201 -u -b "$IPERF_BANDWIDTH" -t "$IPERF_DURATION"
    ) >"$ARTIFACT_DIR/iperf-client.log" 2>&1 &
    IPERF_CLIENT_PID=$!
fi

EVENTS="$ARTIFACT_DIR/events.csv"
echo "timestamp,event" >"$EVENTS"
echo "$(date '+%Y/%m/%d %H:%M:%S.%3N'),baseline-start" >>"$EVENTS"

case "$CASE" in
    baseline)
        sleep "$DURATION"
        ;;
    impairment)
        sleep 20
        echo "$(date '+%Y/%m/%d %H:%M:%S.%3N'),forced-loss-start" >>"$EVENTS"
        ip netns exec "$NS_A" tc qdisc replace dev "$IF_A" root netem loss 100%
        ip netns exec "$NS_B" tc qdisc replace dev "$IF_B" root netem loss 100%
        sleep 4
        echo "$(date '+%Y/%m/%d %H:%M:%S.%3N'),impairment-start" >>"$EVENTS"
        ip netns exec "$NS_A" tc qdisc replace dev "$IF_A" root netem \
            delay "${DELAY_MS}ms" loss "${LOSS_PERCENT}%"
        ip netns exec "$NS_B" tc qdisc replace dev "$IF_B" root netem \
            delay "${DELAY_MS}ms" loss "${LOSS_PERCENT}%"
        sleep "$((DURATION - 49))"
        echo "$(date '+%Y/%m/%d %H:%M:%S.%3N'),recovery-start" >>"$EVENTS"
        ip netns exec "$NS_A" tc qdisc delete dev "$IF_A" root
        ip netns exec "$NS_B" tc qdisc delete dev "$IF_B" root
        sleep 25
        ;;
    invalid-source)
        sleep 15
        echo "$(date '+%Y/%m/%d %H:%M:%S.%3N'),injection-start" >>"$EVENTS"
        ip netns exec "$NS_A" "$PYTHON_BIN" "$SCRIPT_DIR/inject_invalid_pm.py" \
            --address-family "$ADDRESS_FAMILY" \
            --destination "$TRANSPORT_B" --known-source "$TRANSPORT_A" \
            --unknown-source "$UNKNOWN_SOURCE" \
            >"$ARTIFACT_DIR/injection.log" 2>&1
        echo "$(date '+%Y/%m/%d %H:%M:%S.%3N'),injection-end" >>"$EVENTS"
        sleep "$((DURATION - 15))"
        ;;
esac

stop_pid "$IPERF_CLIENT_PID"
stop_pid "$IPERF_SERVER_PID"
stop_pid "$TCPDUMP_PID"
TCPDUMP_PID=""
stop_pid "$PID_A"
stop_pid "$PID_B"
PID_A=""
PID_B=""

"$PYTHON_BIN" "$SCRIPT_DIR/check_ipv6_result.py" \
    --address-family "$ADDRESS_FAMILY" \
    --case "$CASE" \
    --log-a "$ARTIFACT_DIR/bgpd-a.log" \
    --log-b "$ARTIFACT_DIR/bgpd-b.log" \
    --socket-a "$ARTIFACT_DIR/ss-node-a.txt" \
    --socket-b "$ARTIFACT_DIR/ss-node-b.txt" \
    --capture "$ARTIFACT_DIR/$CAPTURE_FILE" \
    --transport-a "$TRANSPORT_A" \
    --transport-b "$TRANSPORT_B" \
    --unknown-source "$UNKNOWN_SOURCE" \
    --events "$EVENTS" \
    --delay-ms "$DELAY_MS" \
    | tee "$ARTIFACT_DIR/assertions.txt"

if "$PYTHON_BIN" -c 'import matplotlib, numpy' >/dev/null 2>&1; then
    PLOT_ARGS=("$ARTIFACT_DIR/bgpd-a.log"
               --output "$ARTIFACT_DIR/pm-$ADDRESS_FAMILY-results.png"
               --events-file "$EVENTS")
    if (( IPERF_ENABLE == 1 )); then
        PLOT_ARGS+=(--iperf-start "$IPERF_START_TIME"
                    --iperf-duration "$IPERF_DURATION")
    fi
    "$PYTHON_BIN" "$SCRIPT_DIR/plot_pm.py" "${PLOT_ARGS[@]}"
else
    echo "[run_ipv6_test] plotting skipped: matplotlib or numpy is unavailable in $PYTHON_BIN" \
        | tee -a "$ARTIFACT_DIR/assertions.txt"
fi

echo "[run_ipv6_test] PASS"
echo "[run_ipv6_test] results: $ARTIFACT_DIR"
