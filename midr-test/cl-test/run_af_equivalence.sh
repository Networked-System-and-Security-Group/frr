#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TIMEOUT=150

while (( $# > 0 )); do
    case "$1" in
        --timeout)
            TIMEOUT="${2:-}"
            shift 2
            ;;
        -h|--help)
            echo "Usage: sudo -E $0 [--timeout SECONDS]"
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

for family in ipv4 ipv6; do
    "$SCRIPT_DIR/run_test.sh" \
        --address-family "$family" \
        --timeout "$TIMEOUT"
done

python3 "$SCRIPT_DIR/compare_decisions.py" \
    --left "$SCRIPT_DIR/artifacts/latest-ipv4/decisions.json" \
    --right "$SCRIPT_DIR/artifacts/latest-ipv6/decisions.json"

echo "[cl-af] PASS: IPv4-only and IPv6-only CL decisions are equivalent"
