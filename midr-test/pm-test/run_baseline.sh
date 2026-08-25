#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

exec "$SCRIPT_DIR/run_test.sh" \
    --duration 90 \
    --output "$SCRIPT_DIR/pm_baseline_results.png"
