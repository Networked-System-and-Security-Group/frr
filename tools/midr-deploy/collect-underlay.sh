#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: collect-underlay.sh [--inventory FILE] [--output DIR] [--interactive]

Collect read-only state from every FITI MIDR host listed in FILE.
By default SSH must already work with BatchMode=yes; --interactive prompts
for authentication one node at a time. This script never changes a node.
EOF
}

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
INVENTORY="$SCRIPT_DIR/inventory-12.csv"
OUTPUT=""
INTERACTIVE=0

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
        --interactive)
            INTERACTIVE=1
            shift
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
if [ -z "$OUTPUT" ]; then
    OUTPUT="$PWD/fiti-underlay-$(date -u +%Y%m%dT%H%M%SZ)"
fi
mkdir -p "$OUTPUT/raw" "$OUTPUT/status"

remote_script=$(cat <<'REMOTE'
set +e
printf '%s\n' '=== identity ==='
hostname
date -u
printf '%s\n' '=== addresses ==='
ip -6 addr show
printf '%s\n' '=== routes ==='
ip -6 route show table main
printf '%s\n' '=== transport policy ==='
ip -6 rule show
ip -6 route show table 1990 2>&1 || true
printf '%s\n' '=== processes ==='
systemctl is-active frr 2>&1 || true
pgrep -af 'zebra|bgpd|ospf6d|ospfd|midrd' || true
printf '%s\n' '=== sockets ==='
ss -ltnp 2>&1 || true
printf '%s\n' '=== bgp summary ==='
vtysh -d bgpd -c 'show bgp summary' 2>&1 || true
printf '%s\n' '=== bgp ipv6 summary ==='
vtysh -d bgpd -c 'show bgp ipv6 unicast summary' 2>&1 || true
printf '%s\n' '=== ospf6 neighbors ==='
vtysh -d ospf6d -c 'show ipv6 ospf6 neighbor' 2>&1 || true
printf '%s\n' '=== ospf6 routes ==='
vtysh -d ospf6d -c 'show ipv6 ospf6 route' 2>&1 || true
printf '%s\n' '=== daemon configuration ==='
for file in /etc/frr/daemons /etc/frr/frr.conf /etc/frr/zebra.conf; do
    if [ -r "$file" ]; then
        printf '%s\n' "--- $file ---"
        sed -n '1,260p' "$file"
    fi
done
REMOTE
)

run_one() {
    local name=$1 host=$2 port=$3 user=$4
    local raw="$OUTPUT/raw/$name.txt"
    local status="$OUTPUT/status/$name.txt"
    if [ "$INTERACTIVE" -eq 1 ]; then
        if ssh -o BatchMode=no \
            -o PreferredAuthentications=password,keyboard-interactive \
            -o PubkeyAuthentication=no -o ConnectTimeout=10 -p "$port" \
            "$user@$host" "$remote_script" | tee "$raw"; then
            printf 'PASS %s\n' "$name" >"$status"
        else
            printf 'FAIL %s\n' "$name" >"$status"
        fi
    elif ssh -n -o BatchMode=yes \
        -o PreferredAuthentications=publickey \
        -o PubkeyAuthentication=yes \
        -o PasswordAuthentication=no \
        -o KbdInteractiveAuthentication=no \
        -o ConnectTimeout=10 -p "$port" \
        "$user@$host" "$remote_script" >"$raw" 2>&1; then
        printf 'PASS %s\n' "$name" >"$status"
    else
        printf 'FAIL %s\n' "$name" >"$status"
    fi
}

if [ "$INTERACTIVE" -eq 1 ]; then
    while IFS=, read -r name host port user transport node_id role group_id; do
        case "$name" in
            ''|'#'*) continue ;;
        esac
        printf 'Authenticating %s (%s:%s)\n' "$name" "$host" "$port" >&2
        run_one "$name" "$host" "$port" "$user"
    done < "$INVENTORY"
else
    while IFS=, read -r name host port user transport node_id role group_id; do
        case "$name" in
            ''|'#'*) continue ;;
        esac
        run_one "$name" "$host" "$port" "$user" &
    done < "$INVENTORY"
    wait
fi

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
printf 'collected=%d failed=%d output=%s\n' "$pass_count" "$fail_count" "$OUTPUT"
[ "$fail_count" -eq 0 ]
