#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: check-transport-matrix.sh [--inventory FILE] [--output DIR]

Read-only IPv6 route lookup for TCP 5871 from each host's transport-address
to every listed endpoint. This checks the local FIB, not end-to-end delivery.
SSH must already work with public-key BatchMode=yes.
EOF
}

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
INVENTORY="$SCRIPT_DIR/inventory-12.csv"
OUTPUT=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --inventory)
            [ "$#" -ge 2 ] || { usage >&2; exit 2; }
            INVENTORY=$2
            shift 2
            ;;
        --output)
            [ "$#" -ge 2 ] || { usage >&2; exit 2; }
            OUTPUT=$2
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
done

[ -r "$INVENTORY" ] || { echo "inventory is not readable: $INVENTORY" >&2; exit 1; }
[ -n "$OUTPUT" ] || OUTPUT="$PWD/fiti-transport-matrix-$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$OUTPUT/raw" "$OUTPUT/status"

declare -a NAMES HOSTS PORTS USERS TARGETS
while IFS=, read -r name host port user transport node_id role group_id; do
    case "$name" in
        ''|'#'*) continue ;;
    esac
    NAMES+=("$name")
    HOSTS+=("$host")
    PORTS+=("$port")
    USERS+=("$user")
    TARGETS+=("$transport")
done < "$INVENTORY"

[ "${#TARGETS[@]}" -gt 0 ] || { echo 'inventory has no nodes' >&2; exit 1; }

target_script='set +e
printf "%s\\n" "=== identity ==="
hostname
printf "%s\\n" "=== default route ==="
ip -6 route show default
printf "%s\\n" "=== transport routes ==="
'
for target in "${TARGETS[@]}"; do
    target_script+="printf '%s\\n' '--- $target ---'; ip -6 route get '$target' from \"\$transport_source\" ipproto tcp dport 5871 || printf '%s\\n' 'ROUTE_LOOKUP_FAILED';"$'\n'
done
target_script+=$'exit 0\n'

run_one() {
    local index=$1
    local name=${NAMES[$index]}
    local raw="$OUTPUT/raw/$name.txt"
    local status="$OUTPUT/status/$name.txt"
    local remote_script="transport_source='${TARGETS[$index]}'"$'\n'"$target_script"

    if ssh -n -o BatchMode=yes \
        -o PreferredAuthentications=publickey \
        -o PubkeyAuthentication=yes \
        -o PasswordAuthentication=no \
        -o KbdInteractiveAuthentication=no \
        -o ConnectTimeout=10 \
        -p "${PORTS[$index]}" "${USERS[$index]}@${HOSTS[$index]}" \
        "$remote_script" >"$raw" 2>&1; then
        if grep -Eq 'ROUTE_LOOKUP_FAILED|Network is unreachable|RTNETLINK answers:|unreachable ' "$raw"; then
            printf 'FAIL %s route-unreachable\n' "$name" >"$status"
        else
            printf 'PASS %s\n' "$name" >"$status"
        fi
    else
        printf 'FAIL %s ssh\n' "$name" >"$status"
    fi
}

for ((i = 0; i < ${#NAMES[@]}; i++)); do
    run_one "$i" &
done
wait

pass_count=0
fail_count=0
for status in "$OUTPUT"/status/*.txt; do
    [ -e "$status" ] || continue
    if grep -q '^PASS ' "$status"; then
        pass_count=$((pass_count + 1))
    else
        fail_count=$((fail_count + 1))
    fi
done

printf 'checked=%d failed=%d output=%s\n' \
    "$pass_count" "$fail_count" "$OUTPUT"
[ "$fail_count" -eq 0 ]
