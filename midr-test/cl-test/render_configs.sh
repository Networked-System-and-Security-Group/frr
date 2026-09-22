#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FAMILY="${1:-}"
OUTPUT_DIR="${2:-}"

if [[ "$FAMILY" != "ipv4" && "$FAMILY" != "ipv6" ]] ||
   [[ -z "$OUTPUT_DIR" ]]; then
    echo "Usage: $0 <ipv4|ipv6> <output-directory>" >&2
    exit 2
fi

mkdir -p "$OUTPUT_DIR"
rm -f "$OUTPUT_DIR"/bgpd-*.conf

node_ids=(11 12 13 14 15 21 22 31 32 99)
sed_args=()

for node_id in "${node_ids[@]}"; do
    if [[ "$FAMILY" == "ipv4" ]]; then
        transport="172.31.${node_id}.1"
    else
        transport="fd00:0:${node_id}::1"
        underlay="fd10:10:${node_id}::2"
        sed_args+=(-e "s|10\.10\.${node_id}\.2|${underlay}|g")
    fi
    sed_args+=(
        -e "s|midr transport-address 10\.0\.${node_id}\.1|midr transport-address ${transport}|g"
    )
done

if [[ "$FAMILY" == "ipv4" ]]; then
    bootstrap="172.31.11.1"
else
    bootstrap="fd00:0:11::1"
fi
sed_args+=(
    -e "s|midr bootstrap 10\.0\.11\.1|midr bootstrap ${bootstrap}|g"
)

for source in "$SCRIPT_DIR"/configs/bgpd-*.conf; do
    destination="$OUTPUT_DIR/$(basename "$source")"
    sed "${sed_args[@]}" "$source" >"$destination"
done

legacy_pattern='^[[:space:]]+midr (transport-address|bootstrap) 10\.0\.'
if [[ "$FAMILY" == "ipv6" ]]; then
    legacy_pattern='^[[:space:]]+(neighbor 10\.10\.|midr (transport-address|bootstrap) 10\.0\.)'
fi
if grep -REq "$legacy_pattern" "$OUTPUT_DIR"; then
    echo "Generated configuration still contains a legacy locator." >&2
    exit 1
fi

echo "[render-configs] generated $FAMILY configs in $OUTPUT_DIR"
