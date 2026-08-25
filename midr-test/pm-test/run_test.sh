#!/bin/bash
# Launch the MIDR PM Rattan test with optional iperf3 background load.
#
# Usage:
#   sudo ./run_test.sh [OPTIONS]
#
# Options:
#   --iperf                   Enable iperf3 UDP background flow
#   --iperf-start-time N      Seconds after experiment start before iperf3 client
#                             connects (default: 5)
#   --iperf-duration N        Duration of the iperf3 flow in seconds (default: 10)
#   --iperf-bandwidth BW      Target UDP send rate, e.g. 10M, 100M (default: 10M)
#   -d, --duration N          Stop the experiment automatically after N seconds
#                             (default: unlimited — press Ctrl-C to stop)
#   -c, --config FILE         Rattan TOML config (default: midr-pm-test.toml)
#   -o, --output FILE         Output image path (default depends on mode)
#   -h, --help                Show this help

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RATTAN_BIN="${MIDR_RATTAN_BIN:-$REPO_ROOT/../rattan/target/release/rattan}"

# midr-pm-test.toml uses paths relative to this dir (packet_log, start-node-*.sh),
# so rattan must be launched with SCRIPT_DIR as its cwd.
cd "$SCRIPT_DIR"

CONFIG="$SCRIPT_DIR/midr-pm-test.toml"
IPERF_ENABLE=0
IPERF_START_TIME=5
IPERF_DURATION=10
IPERF_BANDWIDTH=10M
DURATION=
OUTPUT=

while [[ $# -gt 0 ]]; do
    case "$1" in
        --iperf)
            IPERF_ENABLE=1
            shift ;;
        --iperf-start-time)
            IPERF_START_TIME="$2"
            shift 2 ;;
        --iperf-duration)
            IPERF_DURATION="$2"
            shift 2 ;;
        --iperf-bandwidth)
            IPERF_BANDWIDTH="$2"
            shift 2 ;;
        -d|--duration)
            DURATION="$2"
            shift 2 ;;
        -c|--config)
            CONFIG="$2"
            shift 2 ;;
        -o|--output)
            OUTPUT="$2"
            shift 2 ;;
        -h|--help)
            sed -n '2,/^$/p' "$SCRIPT_DIR/run_test.sh" | sed 's/^# \?//'
            exit 0 ;;
        *)
            echo "Unknown option: $1" >&2
            exit 1 ;;
    esac
done

export IPERF_ENABLE IPERF_START_TIME IPERF_DURATION IPERF_BANDWIDTH
export LD_LIBRARY_PATH="$REPO_ROOT/lib/.libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

if [ "$EUID" -ne 0 ]; then
    echo "ERROR: run this test as root (sudo $0 ...)." >&2
    exit 1
fi

for binary in "$REPO_ROOT/bgpd/.libs/bgpd" "$REPO_ROOT/vtysh/.libs/vtysh" "$RATTAN_BIN"; do
    if [ ! -x "$binary" ]; then
        echo "ERROR: required executable not found: $binary" >&2
        exit 1
    fi
done
for command_name in ip python3 timeout tee; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        echo "ERROR: required command not found: $command_name" >&2
        exit 1
    fi
done
if [ "$IPERF_ENABLE" = "1" ] && ! command -v iperf3 >/dev/null 2>&1; then
    echo "ERROR: iperf3 is required for the background-load scenario." >&2
    exit 1
fi

export MPLCONFIGDIR="${MIDR_MPLCONFIGDIR:-/tmp/midr-pm-matplotlib-$EUID}"
mkdir -p "$MPLCONFIGDIR"

if [ -z "$OUTPUT" ]; then
    if [ "$IPERF_ENABLE" = "1" ]; then
        OUTPUT="$SCRIPT_DIR/pm_iperf_results.png"
    else
        OUTPUT="$SCRIPT_DIR/pm_baseline_results.png"
    fi
fi

if [ "$IPERF_ENABLE" = "1" ]; then
    SCENARIO=iperf
else
    SCENARIO=baseline
fi
LOG_DIR="$SCRIPT_DIR/logs/$SCENARIO"
mkdir -p "$LOG_DIR"
find "$LOG_DIR" -mindepth 1 -maxdepth 1 -type f -delete

RUNTIME_ARTIFACTS=(
    bgpd-a.log
    bgpd-b.log
    iperf-client.log
    iperf-server.log
    midr-pm-test.flow
    midr-pm-test.rtl
)
for artifact in "${RUNTIME_ARTIFACTS[@]}"; do
    rm -f "$SCRIPT_DIR/$artifact"
done
rm -f "$OUTPUT"

archive_runtime_artifacts() {
    local artifact
    for artifact in "${RUNTIME_ARTIFACTS[@]}"; do
        if [ -e "$SCRIPT_DIR/$artifact" ]; then
            mv -f "$SCRIPT_DIR/$artifact" "$LOG_DIR/$artifact"
        fi
    done
    chmod -R a+rX "$LOG_DIR"
}

if [ "$IPERF_ENABLE" = "1" ]; then
    echo "[run_test] iperf3 UDP ON: client starts at t+${IPERF_START_TIME}s, runs for ${IPERF_DURATION}s at ${IPERF_BANDWIDTH}bps"
else
    echo "[run_test] iperf3 OFF"
fi
echo "[run_test] config: $CONFIG"
echo "[run_test] rattan: $RATTAN_BIN"
echo "[run_test] logs: $LOG_DIR"
runner_status=0
tee_status=0
set +e
if [ -n "$DURATION" ]; then
    echo "[run_test] duration: ${DURATION}s (auto-stop)"
else
    echo "[run_test] duration: unlimited (press Ctrl-C to stop)"
fi
echo

if [ -n "$DURATION" ]; then
    # Send SIGINT first (same as a manual Ctrl-C) so rattan's own cleanup
    # (tearing down namespaces, stopping bgpd) runs normally; force-kill
    # only if it hasn't exited 10s after that.
    timeout --signal=INT --kill-after=10 "$DURATION" \
        "$RATTAN_BIN" run -c "$CONFIG" --left-stdout --right-stdout \
        --left-stderr --right-stderr 2>&1 | tee "$LOG_DIR/rattan.log"
    pipeline_status=("${PIPESTATUS[@]}")
    runner_status=${pipeline_status[0]}
    tee_status=${pipeline_status[1]}
else
    "$RATTAN_BIN" run -c "$CONFIG" --left-stdout --right-stdout \
        --left-stderr --right-stderr 2>&1 | tee "$LOG_DIR/rattan.log"
    pipeline_status=("${PIPESTATUS[@]}")
    runner_status=${pipeline_status[0]}
    tee_status=${pipeline_status[1]}
fi
set -e
archive_runtime_artifacts

if [ "$tee_status" -ne 0 ]; then
    echo "ERROR: failed to save Rattan output (status $tee_status); logs: $LOG_DIR" >&2
    exit "$tee_status"
fi
if [ "$runner_status" -ne 0 ] && [ "$runner_status" -ne 124 ]; then
    echo "ERROR: Rattan failed with status $runner_status; logs: $LOG_DIR" >&2
    exit "$runner_status"
fi

check_args=("$LOG_DIR/bgpd-a.log")
plot_args=("$LOG_DIR/bgpd-a.log" --output "$OUTPUT")
if [ "$IPERF_ENABLE" = "1" ]; then
    check_args+=(--iperf)
    plot_args+=(--iperf-start "$IPERF_START_TIME" --iperf-duration "$IPERF_DURATION")
fi

python3 "$SCRIPT_DIR/check_result.py" "${check_args[@]}"
python3 "$SCRIPT_DIR/plot_pm.py" "${plot_args[@]}"
chmod a+r "$OUTPUT"
echo "[run_test] PASS: $OUTPUT"
echo "[run_test] logs: $LOG_DIR"
