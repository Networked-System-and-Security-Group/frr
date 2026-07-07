#!/bin/bash
# run_test.sh — launch the MIDR PM Rattan test with optional iperf3 background load.
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
#   -c, --config FILE         Rattan TOML config (default: midr-pm-test.toml)
#   -h, --help                Show this help

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# midr-pm-test.toml uses paths relative to this dir (packet_log, start-node-*.sh),
# so rattan must be launched with SCRIPT_DIR as its cwd.
cd "$SCRIPT_DIR"

CONFIG="$SCRIPT_DIR/midr-pm-test.toml"
IPERF_ENABLE=0
IPERF_START_TIME=5
IPERF_DURATION=10
IPERF_BANDWIDTH=10M

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
        -c|--config)
            CONFIG="$2"
            shift 2 ;;
        -h|--help)
            grep '^#' "$0" | sed 's/^# \?//'
            exit 0 ;;
        *)
            echo "Unknown option: $1" >&2
            exit 1 ;;
    esac
done

export IPERF_ENABLE IPERF_START_TIME IPERF_DURATION IPERF_BANDWIDTH

if [ "$IPERF_ENABLE" = "1" ]; then
    echo "[run_test] iperf3 UDP ON: client starts at t+${IPERF_START_TIME}s, runs for ${IPERF_DURATION}s at ${IPERF_BANDWIDTH}bps"
else
    echo "[run_test] iperf3 OFF"
fi
echo "[run_test] config: $CONFIG"
echo

rattan run -c "$CONFIG" --left-stdout --right-stdout 2>&1 | tee "$SCRIPT_DIR/rattan.log"
