#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
ADDRESS_FAMILY="all"
PM_DURATION=30

usage() {
    sed -n '/^Usage:/,/^$/p' <<'EOF'
Usage: sudo -E ./midr-test/af-test/run_pre_cl.sh [OPTIONS]
  --address-family FAMILY  ipv4, ipv6, or all (default: all)
  --pm-duration SECONDS    PM baseline duration, at least 30 (default: 30)
  -h, --help               Show this help

EOF
}

while (( $# > 0 )); do
    case "$1" in
        --address-family)
            ADDRESS_FAMILY="$2"
            shift 2
            ;;
        --pm-duration)
            PM_DURATION="$2"
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

case "$ADDRESS_FAMILY" in
    ipv4|ipv6)
        FAMILIES=("$ADDRESS_FAMILY")
        ;;
    all)
        FAMILIES=(ipv4 ipv6)
        ;;
    *)
        echo "Unsupported address family: $ADDRESS_FAMILY" >&2
        exit 2
        ;;
esac
if ! [[ "$PM_DURATION" =~ ^[0-9]+$ ]] || (( PM_DURATION < 30 )); then
    echo "--pm-duration must be at least 30 seconds." >&2
    exit 2
fi
if (( EUID != 0 )); then
    echo "Run this script with sudo -E." >&2
    exit 1
fi

export MIDR_AF_RUN_ID="$(date -u +%Y%m%d-%H%M%S)-$$"
echo "[pre-cl] run id: $MIDR_AF_RUN_ID"
echo "[pre-cl] families: ${FAMILIES[*]}"

for family in "${FAMILIES[@]}"; do
    "$SCRIPT_DIR/nds/run_test.sh" vty --address-family "$family"
    "$SCRIPT_DIR/nds/run_test.sh" control-smoke --address-family "$family"
done

"$REPO_ROOT/midr-test/pm-test/run_ipv6_smoke.sh" \
    --address-family "$ADDRESS_FAMILY"

for family in "${FAMILIES[@]}"; do
    "$REPO_ROOT/midr-test/pm-test/run_ipv6_test.sh" \
        --address-family "$family" --case baseline \
        --duration "$PM_DURATION"
done

echo "[pre-cl] PASS: all NDS and PM pre-CL gates completed"
