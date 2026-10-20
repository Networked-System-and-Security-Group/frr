#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
Usage: deploy-batch.sh --phase bootstrap|representatives|members
       [--mode check|apply] [--profile DIR] [--wait-joined SECS] [--continue]

Run deploy-node.sh for every node of one role, in nodes.csv order.  Deploy
the phases in order: bootstrap, representatives, members.

--wait-joined SECS  After applying a non-bootstrap node, wait up to SECS for
                    it to report a non-zero group (show midr join) before the
                    next node.  Default: 300 for apply, 0 (no wait) for check.
                    Restarting members back to back without waiting lets a
                    node pick a group whose representative is restarting too.
--continue          Record a failed node and carry on with the rest of the
                    phase instead of stopping at the first failure.

RELEASE_SRC selects the release directory built by build-release.sh.
USAGE
}

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
# shellcheck source=lib/profile.sh
. "$SCRIPT_DIR/lib/profile.sh"
RELEASE_SRC=${RELEASE_SRC:-}
PHASE=""
MODE=check
CONTINUE=0
PROFILE_DIR=""
WAIT_JOINED=""

need_value() {
    [ "$2" -ge 2 ] || { echo "$1 requires a value" >&2; usage >&2; exit 2; }
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --phase) need_value "$1" "$#"; PHASE=$2; shift 2 ;;
        --mode) need_value "$1" "$#"; MODE=$2; shift 2 ;;
        --profile) need_value "$1" "$#"; PROFILE_DIR=$2; shift 2 ;;
        --wait-joined) need_value "$1" "$#"; WAIT_JOINED=$2; shift 2 ;;
        --continue) CONTINUE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

case "$PHASE" in
    bootstrap) wanted_role=bootstrap ;;
    representatives) wanted_role=group-rep ;;
    members) wanted_role=member ;;
    *) echo '--phase must be bootstrap, representatives, or members' >&2; usage >&2; exit 2 ;;
esac
case "$MODE" in check|apply) ;; *) echo '--mode must be check or apply' >&2; exit 2 ;; esac
if [ -z "$WAIT_JOINED" ]; then
    if [ "$MODE" = apply ]; then WAIT_JOINED=300; else WAIT_JOINED=0; fi
fi
[[ "$WAIT_JOINED" =~ ^[0-9]+$ ]] || { echo '--wait-joined must be a number of seconds' >&2; exit 2; }
[ -n "$RELEASE_SRC" ] || { echo 'set RELEASE_SRC to a prepared release directory' >&2; exit 2; }
[ -d "$RELEASE_SRC" ] || { echo "release missing: $RELEASE_SRC" >&2; exit 1; }
midr_profile_load "$PROFILE_DIR"

mapfile -t selected < <(midr_nodes_select name role | awk -F, -v wanted="$wanted_role" '$2 == wanted { print $1 }')
[ "${#selected[@]}" -gt 0 ] || { echo "no nodes with role $wanted_role in $MIDR_NODES_FILE" >&2; exit 1; }

# Poll the node's join state until it reports a non-zero local group.
wait_joined() {
    local node=$1 row host port user deadline
    row=$(midr_nodes_select name ssh_host ssh_port ssh_user | awk -F, -v n="$node" '$1 == n')
    IFS=, read -r _ host port user <<<"$row"
    deadline=$((SECONDS + WAIT_JOINED))
    while [ "$SECONDS" -lt "$deadline" ]; do
        if midr_ssh -n -p "$port" "$user@$host" \
            'python3 /opt/midr-current/midrd-vty.py --socket /run/midr/midrd.vty -c "show midr join"' \
            2>/dev/null | grep -Eq 'Local group-id *: *[1-9]'; then
            echo "JOINED $node after $((WAIT_JOINED - (deadline - SECONDS)))s"
            return 0
        fi
        sleep 5
    done
    echo "NOT JOINED $node within ${WAIT_JOINED}s" >&2
    return 1
}

run_root=${RUN_ROOT:-$PWD/midr-deploy-$(date -u +%Y%m%dT%H%M%SZ)}
mkdir -p "$run_root/$PHASE"
failed=0
for node in "${selected[@]}"; do
    log="$run_root/$PHASE/$node.log"
    echo "=== $PHASE $MODE $node ==="
    ok=1
    if ! bash "$SCRIPT_DIR/deploy-node.sh" --profile "$MIDR_PROFILE" \
        --node "$node" --mode "$MODE" 2>&1 | tee "$log"; then
        ok=0
    elif [ "$MODE" = apply ] && [ "$wanted_role" != bootstrap ] && [ "$WAIT_JOINED" -gt 0 ]; then
        wait_joined "$node" 2>&1 | tee -a "$log" || ok=0
    fi
    if [ "$ok" -eq 1 ]; then
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
