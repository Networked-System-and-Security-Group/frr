#!/bin/bash
# Capture read-only MIDR state from the running 10-node CL test.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
VTYSH="$REPO_ROOT/vtysh/.libs/vtysh"
PHASE="${1:-manual}"
RUN_ID="${MIDR_CAPTURE_RUN_ID:-manual-$(date +%Y%m%d-%H%M%S)}"
ARTIFACT_ROOT="${MIDR_CAPTURE_ROOT:-$SCRIPT_DIR/artifacts/$RUN_ID}"
SAFE_PHASE="$(printf '%s' "$PHASE" | tr -c 'A-Za-z0-9._-' '-')"
PHASE_DIR="$ARTIFACT_ROOT/$SAFE_PHASE"
NODES=(g1a g1b g1c g1d g1e g2a g2b g3a g3b newnode)
COMMANDS=(
    "show midr self"
    "show midr join"
    "show midr nodes"
    "show midr neighbors"
    "show midr reps"
    "show midr bootstraps"
    "show midr group2"
    "show midr group2-snapshot"
    "show midr group2-remote"
    "show midr topology sync"
    "show midr topology nodes"
    "show midr topology links"
    "show midr topology tombstones"
    "show midr events"
    "show midr sync"
    "show midr owned"
    "show midr rib summary"
    "show midr rib paths"
    "show midr lsdb summary"
    "show midr lsdb objects"
    "show midr ted summary"
    "show midr ted detail"
    "show midr prefix summary"
    "show midr prefix contributors"
)

mkdir -p "$ARTIFACT_ROOT"
rm -rf "$PHASE_DIR"
mkdir -p "$PHASE_DIR/nodes" "$PHASE_DIR/events"

{
    echo "run_id=$RUN_ID"
    echo "phase=$PHASE"
    echo "captured_at=$(date --iso-8601=seconds)"
    echo "git_commit=$(git -C "$REPO_ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "hostname=$(hostname)"
} >"$PHASE_DIR/metadata.txt"

if [[ ! -x "$VTYSH" ]]; then
    echo "capture_error=vtysh-not-found:$VTYSH" >>"$PHASE_DIR/metadata.txt"
    echo "[capture] vtysh not found: $VTYSH" >&2
    exit 0
fi

capture_node() {
    local node="$1"
    local vty_dir output log command status
    local -a vty_args

    vty_dir="/tmp/midr-cl-vty/$node"
    output="$PHASE_DIR/nodes/$node.txt"
    log="$SCRIPT_DIR/logs/bgpd-$node.log"
    vty_args=(--vty_socket "$vty_dir")

    for command in "${COMMANDS[@]}"; do
        vty_args+=(-c "$command")
    done
    {
        echo "node=$node"
        echo "phase=$PHASE"
        echo "captured_at=$(date --iso-8601=seconds)"
        echo "commands:"
        printf '  %s\n' "${COMMANDS[@]}"
        echo
        timeout 30 "$VTYSH" "${vty_args[@]}" 2>&1
        status=$?
        if [[ $status -ne 0 ]]; then
            echo "[capture-exit-status=$status]"
        fi
    } >"$output"

    if [[ -f "$log" ]]; then
        grep -E \
            'MIDR facts:|MIDR 远端视图:|MIDR 远端视图：|已向第二组注册|MP_REACH|MP_UNREACH|MIDR.*(UPDATE|RIB|LSDB|TED)' \
            "$log" >"$PHASE_DIR/events/$node.log" 2>/dev/null || true
    fi
}

captured=0
pids=()
for node in "${NODES[@]}"; do
    vty_dir="/tmp/midr-cl-vty/$node"

    if [[ ! -d "$vty_dir" ]]; then
        continue
    fi
    captured=$((captured + 1))
    capture_node "$node" &
    pids+=("$!")
done
for pid in "${pids[@]}"; do
    wait "$pid" || true
done

echo "captured_nodes=$captured" >>"$PHASE_DIR/metadata.txt"
echo "[capture] $PHASE: saved $captured node(s) to $PHASE_DIR"
exit 0
