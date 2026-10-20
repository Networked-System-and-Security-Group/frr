#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: install-midr-default-gateways.sh [--inventory FILE] [--dry-run|--apply]

Default mode is dry-run. --apply backs up both Netplan files, validates
61-l3.yaml, and installs the default route immediately without netplan apply.
EOF
}

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
INVENTORY="$SCRIPT_DIR/inventory-12.csv"
MODE=dry-run

while [ "$#" -gt 0 ]; do
    case "$1" in
        --inventory) INVENTORY=$2; shift 2 ;;
        --dry-run) MODE=dry-run; shift ;;
        --apply) MODE=apply; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

[ -r "$INVENTORY" ] || { echo "inventory is not readable: $INVENTORY" >&2; exit 1; }

run_one() {
    local name=$1 host=$2 port=$3 user=$4 transport=$5 gateway=$6
    if [ -z "$gateway" ]; then
        case "$transport" in
            *::2) gateway=$(printf '%s' "$transport" | sed 's/::2$/::1/') ;;
            *) echo "FAIL $name missing explicit gateway for $transport"; return 1 ;;
        esac
    fi
    echo "$name gateway=$gateway mode=$MODE"

    if [ "$MODE" = dry-run ]; then
        ssh -n -o BatchMode=yes -o ConnectTimeout=10 -p "$port" "$user@$host" \
            "hostname; ip -6 route show default; grep -F '$gateway' /etc/netplan/61-l3.yaml || true"
        return
    fi

    ssh -o BatchMode=yes -o ConnectTimeout=10 -p "$port" "$user@$host" \
        "MIDR_GATEWAY='$gateway' bash -s" <<'REMOTE'
set -eu
stamp=$(date -u +%Y%m%dT%H%M%SZ)
backup=/root/midr-gateway-backups/$stamp
install -d -m 700 "$backup"
cp -a /etc/netplan/60-ens4.yaml "$backup/"
cp -a /etc/netplan/61-l3.yaml "$backup/"
before=$(sha256sum /etc/netplan/60-ens4.yaml)
export MIDR_GATEWAY
python3 - <<'PY'
from pathlib import Path
import os
p = Path('/etc/netplan/61-l3.yaml')
s = p.read_text()
g = os.environ['MIDR_GATEWAY']
if g in s:
    raise SystemExit(0)
if '      routes:' in s:
    raise SystemExit('existing ens5 routes require manual review')
marker = '    ens4:\n'
route = '      routes:\n        - to: "::/0"\n          via: "' + g + '"\n          metric: 1024\n'
if marker in s:
    s = s.replace(marker, route + marker, 1)
else:
    s = s.rstrip() + '\n' + route
p.write_text(s)
PY
netplan generate
[ "$before" = "$(sha256sum /etc/netplan/60-ens4.yaml)" ]
ip -6 route replace default via "$MIDR_GATEWAY" dev ens5 metric 1024
ip -6 route show default
REMOTE
}

failed=0
while IFS=, read -r name host port user transport node_id role group_id gateway; do
    case "$name" in ''|'#'*) continue ;; esac
    run_one "$name" "$host" "$port" "$user" "$transport" "$gateway" || failed=$((failed + 1))
done < "$INVENTORY"

echo "failed=$failed mode=$MODE"
[ "$failed" -eq 0 ]
