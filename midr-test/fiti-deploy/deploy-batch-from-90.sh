#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
Usage: deploy-batch-from-90.sh --phase bootstrap|representatives|members \
       [--mode check|apply] [--continue]

Run on NetArchLab90. Nodes are processed in the order in nodes-12.csv. The
default is to stop at the first failed node; --continue records the failure
and proceeds with the remaining nodes in this phase.

RELEASE_SRC selects the release directory, for example:
  RELEASE_SRC=/tmp/midr-5228cff9b3be-20261001T154159Z
USAGE
}

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
NODES="$SCRIPT_DIR/nodes-12.csv"
RELEASE_SRC=${RELEASE_SRC:-}
PHASE=""
MODE=check
CONTINUE=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --phase) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; PHASE=$2; shift 2 ;;
        --mode) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; MODE=$2; shift 2 ;;
        --continue) CONTINUE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

case "$PHASE" in
    bootstrap|representatives|members) ;;
    *) echo '--phase must be bootstrap, representatives, or members' >&2; usage >&2; exit 2 ;;
esac
case "$MODE" in check|apply) ;; *) echo '--mode must be check or apply' >&2; exit 2 ;; esac
[ -n "$RELEASE_SRC" ] || { echo 'set RELEASE_SRC to a prepared release directory' >&2; exit 2; }
[ -r "$NODES" ] || { echo "nodes file missing: $NODES" >&2; exit 1; }
[ -d "$RELEASE_SRC" ] || { echo "release missing: $RELEASE_SRC" >&2; exit 1; }

case "$PHASE" in
    bootstrap) wanted_role=bootstrap ;;
    representatives) wanted_role=group-rep ;;
    members) wanted_role=member ;;
esac

mapfile -t selected < <(
    awk -F, -v wanted="$wanted_role" '
        NR > 1 && $1 !~ /^[[:space:]]*#/ && $8 == wanted { print $1 }
    ' "$NODES"
)
[ "${#selected[@]}" -gt 0 ] || { echo "no nodes for phase $PHASE" >&2; exit 1; }

run_root=${RUN_ROOT:-$PWD/fiti-deploy-$(date -u +%Y%m%dT%H%M%SZ)}
mkdir -p "$run_root/$PHASE"
failed=0
for node in "${selected[@]}"; do
    log="$run_root/$PHASE/$node.log"
    echo "=== $PHASE $MODE $node ==="
    if bash "$SCRIPT_DIR/deploy-node-from-90.sh" \
        --node "$node" --mode "$MODE" 2>&1 | tee "$log"; then
        echo "PASS $node"
    else
        failed=$((failed + 1))
        echo "FAIL $node (log: $log)" >&2
        if [ "$CONTINUE" -ne 1 ]; then
            echo "phase stopped after failure; use --continue only after reviewing the log" >&2
            exit 1
        fi
    fi
done

printf 'phase=%s mode=%s passed=%d failed=%d logs=%s\n' \
    "$PHASE" "$MODE" "$((${#selected[@]} - failed))" "$failed" "$run_root/$PHASE"
[ "$failed" -eq 0 ]
