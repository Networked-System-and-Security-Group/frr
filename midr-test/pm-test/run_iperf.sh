#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

exec "$SCRIPT_DIR/run_test.sh" \
    --iperf \
    --iperf-start-time 20 \
    --iperf-duration 30 \
    --iperf-bandwidth 10M \
    --duration 120 \
    --output "$SCRIPT_DIR/pm_iperf_results.png"
