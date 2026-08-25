#!/usr/bin/env bash
# Run the staged 15-node backbone experiment and produce PPT-ready results.

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TOPOLOGY="$SCRIPT_DIR/midr-backbone.clab.yaml"
CHECK_SCRIPT="$REPO_ROOT/midr-test/backbone-group2/run_dual_source_check.sh"
PYTHON_BIN="${MIDR_PYTHON_BIN:-${CONDA_PREFIX:-}/bin/python}"
OUTPUT_UID="${SUDO_UID:-$(stat -c %u "$REPO_ROOT")}"
OUTPUT_GID="${SUDO_GID:-$(stat -c %g "$REPO_ROOT")}"
BASE_TIMEOUT=360
JOIN_TIMEOUT=420
PHASE=group2
PREFIX=clab-midr-backbone
NODES=(t1 t2 t3 b1 b2 b3 b4 b5 r1 r2 m1a m1b m2a z1 z2)
REVERSE_NODES=(z2 z1 m2a m1b m1a r2 r1 b5 b4 b3 b2 b1 t3 t2 t1)
BASE_NODES=(t1 t2 t3 b1 b2 b3 b4 b5 r1 r2 m1a m1b m2a)
MEMBERS=(r1 r2 m1a m1b m2a z1 z2)
LINK_NODES=(r1 m1a m1b)
SAMPLER_PID=""
STARTED=0

usage() {
    echo "Usage: sudo env MIDR_PYTHON_BIN=/path/to/python $0 [options]"
    echo "  --phase group2       Run the Part 2.2 Group2 integration test"
    echo "  --base-timeout SEC   Base convergence timeout (default: $BASE_TIMEOUT)"
    echo "  --join-timeout SEC   Final convergence timeout (default: $JOIN_TIMEOUT)"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --phase)
            PHASE="${2:-}"
            shift 2
            ;;
        --base-timeout)
            BASE_TIMEOUT="${2:-}"
            shift 2
            ;;
        --join-timeout)
            JOIN_TIMEOUT="${2:-}"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ "$PHASE" != "group2" ]]; then
    echo "Only --phase group2 is available in this test step." >&2
    exit 2
fi
if [[ "$EUID" -ne 0 ]]; then
    echo "Run this script with sudo; it manages containerlab and Docker." >&2
    exit 2
fi
if [[ -z "$PYTHON_BIN" || ! -x "$PYTHON_BIN" ]]; then
    PYTHON_BIN="$(command -v python3 || true)"
fi

for command_name in docker clab timeout tee; do
    command -v "$command_name" >/dev/null 2>&1 || {
        echo "Missing command: $command_name" >&2
        exit 2
    }
done
for file in \
    "$REPO_ROOT/lib/.libs/libfrr.so.0.0.0" \
    "$REPO_ROOT/zebra/.libs/zebra" \
    "$REPO_ROOT/bgpd/.libs/bgpd" \
    "$REPO_ROOT/staticd/.libs/staticd" \
    "$REPO_ROOT/mgmtd/.libs/mgmtd" \
    "$REPO_ROOT/watchfrr/.libs/watchfrr" \
    "$REPO_ROOT/vtysh/.libs/vtysh" \
    /usr/lib/x86_64-linux-gnu/libunwind.so.8.0.1 \
    /usr/lib/x86_64-linux-gnu/libyang.so.2.41.0 \
    "$CHECK_SCRIPT" \
    "$SCRIPT_DIR/plot_group2_health.py"; do
    [[ -e "$file" ]] || {
        echo "Missing required artifact: $file" >&2
        exit 2
    }
done
mkdir -p /tmp/midr-matplotlib
if ! MPLCONFIGDIR=/tmp/midr-matplotlib "$PYTHON_BIN" -c \
    'import matplotlib, numpy' >/dev/null 2>&1; then
    echo "The selected Python lacks matplotlib or numpy: $PYTHON_BIN" >&2
    exit 2
fi

container_exists() {
    docker inspect "$PREFIX-$1" >/dev/null 2>&1
}

force_stop_node() {
    local node="$1"
    container_exists "$node" || return 0
    timeout 12s docker exec -u root "$PREFIX-$node" sh -c \
        'pkill -9 -x watchfrr 2>/dev/null || true
         pkill -9 -x bgpd 2>/dev/null || true
         pkill -9 -x zebra 2>/dev/null || true
         pkill -9 -x staticd 2>/dev/null || true
         pkill -9 -x mgmtd 2>/dev/null || true
         rm -f /var/run/frr/*.pid /var/run/frr/*.vty' >/dev/null 2>&1 || true
}

stop_all() {
    local node
    echo "[backbone] Stopping all FRR daemons..."
    for node in "${REVERSE_NODES[@]}"; do
        if container_exists "$node"; then
            timeout 5s docker exec -u root "$PREFIX-$node" \
                pkill -STOP -x bgpd >/dev/null 2>&1 || true
        fi
    done
    for node in "${REVERSE_NODES[@]}"; do
        force_stop_node "$node"
    done
}

restore_host_permissions() {
    find "$SCRIPT_DIR/configs-backbone" -name frr.conf -exec \
        chown "$OUTPUT_UID:$OUTPUT_GID" {} + 2>/dev/null || true
    find "$SCRIPT_DIR/configs-backbone" -name frr.conf -exec \
        chmod 0644 {} + 2>/dev/null || true
    chown -R "$OUTPUT_UID:$OUTPUT_GID" "$SCRIPT_DIR/logs-backbone" \
        2>/dev/null || true
}

stop_sampler() {
    if [[ -n "$SAMPLER_PID" ]] && kill -0 "$SAMPLER_PID" 2>/dev/null; then
        kill "$SAMPLER_PID" 2>/dev/null || true
        wait "$SAMPLER_PID" 2>/dev/null || true
    fi
    SAMPLER_PID=""
}

cleanup() {
    local rc=$?
    trap - EXIT
    stop_sampler
    if [[ "$STARTED" -eq 1 ]]; then
        stop_all
    fi
    restore_host_permissions
    if [[ -s "$SCRIPT_DIR/group2_health.log" && \
          ! -f "$SCRIPT_DIR/group2_results.png" ]]; then
        MPLCONFIGDIR=/tmp/midr-matplotlib "$PYTHON_BIN" \
            "$SCRIPT_DIR/plot_group2_health.py" \
            --samples "$SCRIPT_DIR/group2_health.log" \
            --check-log "$SCRIPT_DIR/group2_check.log" \
            --output "$SCRIPT_DIR/group2_results.png" || true
    fi
    chmod -R a+rX "$SCRIPT_DIR/logs-backbone" 2>/dev/null || true
    chmod a+r "$SCRIPT_DIR/run-group2.log" \
        "$SCRIPT_DIR/group2_check.log" \
        "$SCRIPT_DIR/group2_health.log" \
        "$SCRIPT_DIR/group2_results.png" 2>/dev/null || true
    chown "$OUTPUT_UID:$OUTPUT_GID" "$SCRIPT_DIR/run-group2.log" \
        "$SCRIPT_DIR/group2_check.log" \
        "$SCRIPT_DIR/group2_health.log" \
        "$SCRIPT_DIR/group2_results.png" 2>/dev/null || true
    exit "$rc"
}
trap cleanup EXIT
trap 'echo "[backbone] Interrupted; stopping this experiment now."; exit 130' INT TERM

cd "$SCRIPT_DIR"
rm -f run-group2.log group2_check.log group2_health.log group2_results.png
exec > >(tee run-group2.log) 2>&1

echo "[backbone] Part 2.2: Group2 reporting and accounting integration"
echo "[backbone] Python: $PYTHON_BIN"

STARTED=1
present=0
for node in "${NODES[@]}"; do
    container_exists "$node" && present=$((present + 1))
done
if [[ "$present" -ne "${#NODES[@]}" ]]; then
    if [[ "$present" -gt 0 ]]; then
        echo "[backbone] Found an incomplete lab ($present/${#NODES[@]}); rebuilding it."
        clab destroy -t "$TOPOLOGY" --cleanup || true
    else
        echo "[backbone] No existing lab found; deploying it."
    fi
    clab deploy -t "$TOPOLOGY"
fi

stop_all

echo "[backbone] Verifying a clean process state..."
for node in "${NODES[@]}"; do
    for daemon in watchfrr bgpd zebra staticd mgmtd; do
        if docker exec "$PREFIX-$node" pgrep -x "$daemon" >/dev/null 2>&1; then
            echo "Residual process: $node/$daemon" >&2
            exit 1
        fi
    done
done

echo "[backbone] Installing the complete local FRR build in all containers..."
for node in "${NODES[@]}"; do
    container="$PREFIX-$node"
    docker cp /usr/lib/x86_64-linux-gnu/libunwind.so.8.0.1 \
        "$container:/lib/x86_64-linux-gnu/" >/dev/null
    docker cp /usr/lib/x86_64-linux-gnu/libyang.so.2.41.0 \
        "$container:/lib/x86_64-linux-gnu/" >/dev/null
    docker cp "$REPO_ROOT/lib/.libs/libfrr.so.0.0.0" \
        "$container:/lib/libfrr.so.0.0.0" >/dev/null
    docker exec -u root "$container" sh -c \
        'ln -sf libunwind.so.8.0.1 /lib/x86_64-linux-gnu/libunwind.so.8
         ln -sf libyang.so.2.41.0 /lib/x86_64-linux-gnu/libyang.so.2
         touch /etc/frr/vtysh.conf
         chown frr:frr /etc/frr/vtysh.conf
         chmod 0640 /etc/frr/vtysh.conf
         ldconfig' >/dev/null
    for daemon in zebra bgpd staticd mgmtd watchfrr; do
        docker cp "$REPO_ROOT/$daemon/.libs/$daemon" \
            "$container:/usr/lib/frr/$daemon" >/dev/null
    done
    docker cp "$REPO_ROOT/vtysh/.libs/vtysh" \
        "$container:/usr/bin/vtysh" >/dev/null
    docker exec -u root "$container" sh -c ': > /etc/frr/logs/frr.log' >/dev/null
done

vty() {
    docker exec "$PREFIX-$1" vtysh -c "$2" 2>/dev/null
}

number_or_missing() {
    local value="$1"
    [[ "$value" =~ ^[0-9]+$ ]] && echo "$value" || echo -1
}

sample_once() {
    local elapsed node output node_reported node_pending link_reported owned group
    elapsed=$(( $(date +%s) - SAMPLE_START ))
    for node in "${MEMBERS[@]}"; do
        output="$(vty "$node" 'show midr group2' || true)"
        node_reported="$(sed -n 's/.*node.*reported=\([0-9][0-9]*\).*/\1/p' <<<"$output" | head -1)"
        node_pending="$(sed -n 's/.*node.*pending=\([0-9][0-9]*\).*/\1/p' <<<"$output" | head -1)"
        link_reported="$(sed -n 's/.*link.*reported \([0-9][0-9]*\).*/\1/p' <<<"$output" | head -1)"
        owned="$(vty "$node" 'show midr owned' 2>/dev/null | sed -n 's/^[[:space:]]*links:[[:space:]]*\([0-9][0-9]*\).*/\1/p' | head -1 || true)"
        group="$(vty "$node" 'show midr self' 2>/dev/null | awk -F: '/^Group-ID/{gsub(/[[:space:]]/,"",$2); print $2}' || true)"
        printf '%s,%s,%s,%s,%s,%s,%s\n' \
            "$elapsed" "$node" \
            "$(number_or_missing "$node_reported")" \
            "$(number_or_missing "$node_pending")" \
            "$(number_or_missing "$link_reported")" \
            "$(number_or_missing "$owned")" \
            "$(number_or_missing "$group")" >> group2_health.log
    done
}

sample_loop() {
    while true; do
        sample_once
        sleep 10
    done
}

echo 'elapsed,node,node_reported,node_pending,link_reported,owned_links,group_id' \
    > group2_health.log
: > group2_check.log
SAMPLE_START=$(date +%s)
export SAMPLE_START
sample_loop &
SAMPLER_PID=$!

wait_for_bgpd() {
    local node="$1" elapsed=0
    while [[ "$elapsed" -lt 45 ]]; do
        if docker exec "$PREFIX-$node" pgrep -x bgpd >/dev/null 2>&1 && \
           vty "$node" 'show bgp summary' >/dev/null 2>&1; then
            return 0
        fi
        sleep 2
        elapsed=$((elapsed + 2))
    done
    echo "bgpd did not become ready on $node" >&2
    return 1
}

start_node() {
    local node="$1"
    echo "  starting $node"
    docker exec -u root "$PREFIX-$node" /usr/lib/frr/frrinit.sh start >/dev/null
    wait_for_bgpd "$node"
    sleep 2
}

echo "[backbone] Stage 1: starting underlay, bootstraps, representatives, then members..."
for node in "${BASE_NODES[@]}"; do
    start_node "$node"
done

echo "[backbone] Waiting for r2 to learn m2a before starting zero-config nodes..."
elapsed=0
while [[ "$elapsed" -lt "$BASE_TIMEOUT" ]]; do
    if vty r2 'show midr nodes' | grep -q '10\.0\.0\.122'; then
        echo "[backbone] Base converged at +${elapsed}s."
        break
    fi
    sleep 10
    elapsed=$((elapsed + 10))
done
if [[ "$elapsed" -ge "$BASE_TIMEOUT" ]]; then
    echo "Base convergence timed out after ${BASE_TIMEOUT}s." >&2
    exit 1
fi

echo "[backbone] Stage 2: starting z1 and z2..."
start_node z1
start_node z2

group_id() {
    vty "$1" 'show midr self' | awk -F: '/^Group-ID/{gsub(/[[:space:]]/,"",$2); print $2}'
}

health_ready() {
    local node output fact owned
    for node in "${MEMBERS[@]}"; do
        output="$(vty "$node" 'show midr group2' || true)"
        grep -q '回调注册        : 已注册' <<<"$output" || return 1
        grep 'node            :' <<<"$output" | grep -q 'reported=1' || return 1
        grep 'node            :' <<<"$output" | grep -q 'pending=0' || return 1
    done
    [[ "$(group_id z1 || true)" =~ ^[1-9][0-9]*$ ]] || return 1
    [[ "$(group_id z2 || true)" =~ ^[1-9][0-9]*$ ]] || return 1
    for node in "${LINK_NODES[@]}"; do
        output="$(vty "$node" 'show midr group2' || true)"
        fact="$(grep -oP 'link            : 条目 [0-9]+（reported \K[0-9]+' <<<"$output" | head -1)"
        owned="$(vty "$node" 'show midr owned' | grep -oP '^\s+links:\s+\K[0-9]+' | head -1)"
        [[ "$fact" =~ ^[1-9][0-9]*$ ]] || return 1
        [[ "$fact" == "$owned" ]] || return 1
    done
}

echo "[backbone] Waiting for Group2 reporting and link accounts to converge..."
elapsed=0
while [[ "$elapsed" -lt "$JOIN_TIMEOUT" ]]; do
    if health_ready; then
        echo "[backbone] Group2 health converged at +${elapsed}s after stage 2."
        break
    fi
    sleep 10
    elapsed=$((elapsed + 10))
done
if [[ "$elapsed" -ge "$JOIN_TIMEOUT" ]]; then
    echo "Final Group2 convergence timed out after ${JOIN_TIMEOUT}s." >&2
    exit 1
fi

stop_sampler
sample_once
echo "[backbone] Running G1-G8 integration criteria..."
set +e
GROUP2_HEALTH_LOG="$SCRIPT_DIR/group2_health.log" \
    bash "$CHECK_SCRIPT" | tee group2_check.log
check_rc=${PIPESTATUS[0]}
set -e

echo "[backbone] Rendering the PPT result figure..."
MPLCONFIGDIR=/tmp/midr-matplotlib "$PYTHON_BIN" \
    "$SCRIPT_DIR/plot_group2_health.py" \
    --samples group2_health.log \
    --check-log group2_check.log \
    --output group2_results.png

if [[ "$check_rc" -ne 0 ]]; then
    echo "[backbone] Group2 criteria failed; see group2_check.log." >&2
    exit "$check_rc"
fi
echo "[backbone] PASS: Group2 integration criteria contain no failures."
echo "[backbone] Figure: $SCRIPT_DIR/group2_results.png"
