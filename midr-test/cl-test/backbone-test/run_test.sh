#!/bin/bash
# run_test.sh — Backbone-topology migration test (validates that the
# migrated containerlab `midr-backbone` graph behaves correctly under our
# netns + tc-netem environment).
#
# Staggered start in three stages, gated on real confirmation rather than
# fixed sleeps -- a blind sleep can't actually fix the race this guards
# against (tried 100s, then 160s; both still failed the same way, since the
# race is about whether a bad thing happens even *once*, not about total
# elapsed time -- see the wait_for_attach() comment below for the mechanism).
#   t=0     t1-t3 (transit fabric) start first -- b1-b5's underlay eBGP
#           sessions peer directly with them, so the transit core must
#           already be up. t1-t3 run no MIDR config at all (see setup.sh's
#           header comment).
#   +5s     b1-b5 start. `midr session`/`midr bootstrap` are static config,
#           not join-flow-gated, so the 5-way backbone mesh forms fast.
#   +15s    r1, r2 (reps only) start. Wait for BOTH to show a real
#           Established session to some bootstrap (wait_for_attach) before
#           moving on -- not a timer guess.
#   (confirmed) m1a, m1b, m2a (members) start.
#   +140s   z1, z2 (zero-config joiners) start, once both groups have had
#           time to settle (REP_PROBE_DONE + MEMBER_PROBE_DONE, ~120s + margin).
#
# Expected outcome (see setup.sh's link table): both z1 and z2 JOIN group 1
# (r1's access link is fast; r2's is deliberately 25ms) and ANCHOR-connect
# to group 2 (the only runner-up -- group 3 doesn't exist in this topology).
#
# Usage: sudo ./run_test.sh [--no-setup] [--join-timeout SECS]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BGPD="$REPO_ROOT/bgpd/.libs/bgpd"
VTYSH="$REPO_ROOT/vtysh/.libs/vtysh"
TESTDIR="$SCRIPT_DIR"
JOIN_TIMEOUT=150
DO_SETUP=1
PYTHON_BIN="${MIDR_PYTHON_BIN:-python3}"

if [[ "$EUID" -ne 0 ]]; then
    echo "Run this test with sudo: sudo ./run_test.sh" >&2
    exit 2
fi
for binary in "$BGPD" "$VTYSH"; do
    if [[ ! -x "$binary" ]]; then
        echo "Missing executable: $binary" >&2
        exit 2
    fi
done
for command_name in ip tc tee "$PYTHON_BIN"; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        echo "Missing command: $command_name" >&2
        exit 2
    fi
done
mkdir -p /tmp/midr-matplotlib
if ! MPLCONFIGDIR=/tmp/midr-matplotlib "$PYTHON_BIN" -c \
    'import matplotlib' >/dev/null 2>&1; then
    echo "Missing Python package: matplotlib is required." >&2
    exit 2
fi

# bgpd/.libs/bgpd is the real binary (bgpd/bgpd is just a libtool wrapper
# script that sets this up automatically -- we bypass it to run the binary
# directly under `ip netns exec`, so we must set this ourselves). Without
# it, the dynamic loader falls back to the system search path and can pick
# up a stale, previously-`make install`-ed libfrr.so.0 instead of the one
# actually built from this source tree, causing spurious
# "undefined symbol" crashes on start that look nothing like a real bug.
export LD_LIBRARY_PATH="$REPO_ROOT/lib/.libs:${LD_LIBRARY_PATH:-}"

cd "$TESTDIR"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-setup)
            DO_SETUP=0
            shift
            ;;
        --join-timeout)
            if [[ $# -lt 2 || ! "$2" =~ ^[1-9][0-9]*$ ]]; then
                echo "--join-timeout requires a positive integer." >&2
                exit 2
            fi
            JOIN_TIMEOUT="$2"
            shift 2
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

rm -f "$TESTDIR/run.log"
exec > >(tee "$TESTDIR/run.log") 2>&1

cleanup() {
    local rc=$?
    trap - EXIT
    bash "$TESTDIR/teardown.sh" || true
    chmod -R a+rX "$TESTDIR/logs" 2>/dev/null || true
    chmod a+r "$TESTDIR/run.log" 2>/dev/null || true
    chmod a+r "$TESTDIR/backbone_join_results.png" 2>/dev/null || true
    exit "$rc"
}
trap cleanup EXIT
trap 'echo "[bb-run_test] Interrupted."; exit 130' INT TERM

echo "[bb-run_test] Checking for stale bgpd instances from a previous run..."
shopt -s nullglob
for pidfile in /tmp/bgpd-bb-*.pid; do
    pid=$(cat "$pidfile" 2>/dev/null || true)
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
        echo "  killing stale $(basename "$pidfile") (pid $pid)"
        kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$pidfile"
done
shopt -u nullglob
sleep 1

if [[ "$DO_SETUP" -eq 1 ]]; then
    echo "[bb-run_test] Removing stale backbone-test processes and namespaces..."
    bash "$TESTDIR/teardown.sh"
    echo "[bb-run_test] Setting up network namespaces (15 nodes, 15 links)..."
    bash "$TESTDIR/setup.sh"
fi

mkdir -p "$TESTDIR/logs"
rm -f "$TESTDIR/logs"/*.log "$TESTDIR/backbone_join_results.png"
rm -rf /tmp/midr-bb-vty && mkdir -p /tmp/midr-bb-vty

start_node() {
    local node="$1"
    mkdir -p "/tmp/midr-bb-vty/$node"
    ip netns exec "ns-bb-$node" "$BGPD" \
        -f "$TESTDIR/configs/bgpd-${node}.conf" \
        -Z -S \
        -i "/tmp/bgpd-bb-${node}.pid" \
        --vty_socket "/tmp/midr-bb-vty/$node" \
        --log-level debug &
    echo "  started bgpd for $node (bg pid $!)"
}

# Only matches lines added *after* $since_line -- teardown.sh deliberately
# doesn't delete logs/*.log (so you can inspect a prior run after tearing
# down), and FRR's `log file` directive appends rather than truncates on a
# fresh process start. An unscoped grep would false-positive on a leftover
# line from an earlier run instead of proving this run actually succeeded
# (confirmed: saw exactly this -- "JOIN detected at t≈0s" -- before adding
# the marker).
wait_for_log() {
    local node="$1" pattern="$2" timeout="$3" label="$4" since_line="$5"
    local log="$TESTDIR/logs/bgpd-${node}.log"
    local elapsed=0
    while [[ $elapsed -lt $timeout ]]; do
        if [[ -f "$log" ]] && tail -n "+${since_line}" "$log" | grep -q "$pattern" 2>/dev/null; then
            echo ""
            echo "[bb-run_test] ✓ $node: $label detected at t≈${elapsed}s!"
            return 0
        fi
        printf "\r  elapsed: %3ds / %ds" "$elapsed" "$timeout"
        sleep 5
        elapsed=$((elapsed + 5))
    done
    echo ""
    echo "[bb-run_test] ✗ $node: $label not seen within ${timeout}s."
    return 1
}

log_lines_now() {
    local node="$1" log="$TESTDIR/logs/bgpd-${node}.log"
    [[ -f "$log" ]] && wc -l < "$log" || echo 0
}

wait_callback_ready() {
    local node="$1" timeout=90 elapsed=0
    echo "[bb-run_test] Waiting for $node's remote-view callback registration..."
    while [[ $elapsed -lt $timeout ]]; do
        if grep -q "已向第二组注册 node/link 回调" \
             "$TESTDIR/logs/bgpd-${node}.log" 2>/dev/null; then
            echo "  $node ready at t=${elapsed}s"
            return 0
        fi
        sleep 3
        elapsed=$((elapsed + 3))
    done
    echo "  $node did not register its remote-view callback within ${timeout}s"
    return 1
}

wait_group_members() {
    local node="$1" gid="$2" want="$3" timeout=240 elapsed=0 got=0
    echo "[bb-run_test] Waiting for $node to know $want group-$gid members..."
    while [[ $elapsed -lt $timeout ]]; do
        got=$("$VTYSH" --vty_socket "/tmp/midr-bb-vty/$node" \
                  -c 'show midr nodes' 2>/dev/null \
                  | awk -v g="$gid" '$1 ~ /^10\.0\.0\./ && $4 == g' | wc -l)
        if [[ "$got" -ge "$want" ]]; then
            echo "  $node knows $got group-$gid members at t=${elapsed}s"
            return 0
        fi
        sleep 5
        elapsed=$((elapsed + 5))
    done
    echo "  $node knows only $got/$want group-$gid members after ${timeout}s"
    return 1
}

# Waits for $node to show a real Established session to *any* bootstrap,
# queried live via its own vty (show midr neighbors) rather than guessed from
# a fixed sleep. First attempt confirmed r1/r2 had a real Established attach
# session before starting members and STILL hit the self-appointment race --
# because "r1/r2 attached to *some* bootstrap" isn't the same thing as "b1
# specifically knows about them". Every node's bootstrap candidate list is
# ordered b1..b5 (config file order), so b1 is who everyone actually asks
# first; r1/r2 pick their OWN attach target via hash(router-id) % 5, which
# is frequently a *different* bootstrap, requiring bootstrap-to-bootstrap
# gossip (the b1..b5 mutual mesh) to reach b1 -- a second propagation delay
# wait_for_attach() never accounted for. Fix: query b1 directly (`show midr
# reps`, filtered by the GROUP_REP capability bit) instead of asking r1/r2
# about themselves.
wait_for_reps_known_to_b1() {
    local timeout="$1"
    local elapsed=0
    while [[ $elapsed -lt $timeout ]]; do
        local out
        out=$("$VTYSH" --vty_socket "/tmp/midr-bb-vty/b1" -c "show midr reps" 2>/dev/null)
        if echo "$out" | grep -q "10.0.0.111" && echo "$out" | grep -q "10.0.0.121"; then
            echo ""
            echo "[bb-run_test] ✓ b1 knows about both r1 and r2 as reps at t≈${elapsed}s!"
            return 0
        fi
        printf "\r  elapsed: %3ds / %ds" "$elapsed" "$timeout"
        sleep 5
        elapsed=$((elapsed + 5))
    done
    echo ""
    echo "[bb-run_test] ✗ b1 never learned about both r1 and r2 within ${timeout}s."
    return 1
}

echo "[bb-run_test] Starting t1-t3 (transit fabric, underlay-only eBGP)..."
for t in t1 t2 t3; do start_node "$t"; done
echo "[bb-run_test] Waiting 5s for the transit core to establish..."
sleep 5

echo "[bb-run_test] Starting b1-b5 (bootstrap backbone mesh)..."
for b in b1 b2 b3 b4 b5; do start_node "$b"; done
for b in b1 b2 b3 b4 b5; do wait_callback_ready "$b"; done
sleep 5

echo "[bb-run_test] Starting r1 (group 1 rep) and r2 (group 2 rep)..."
r1_marker=$(log_lines_now r1); r2_marker=$(log_lines_now r2)
start_node r1
start_node r2
wait_callback_ready r1
wait_callback_ready r2
echo "[bb-run_test] Waiting for b1 to learn about both r1 and r2 as reps..."
wait_for_reps_known_to_b1 240
sleep 5

echo "[bb-run_test] Starting m1a, m1b (group 1) and m2a (group 2)..."
m1a_marker=$(log_lines_now m1a); m1b_marker=$(log_lines_now m1b); m2a_marker=$(log_lines_now m2a)
start_node m1a
start_node m1b
start_node m2a
wait_group_members r1 1 3
wait_group_members r2 2 2

echo "[bb-run_test] Starting z1 and z2 (zero-config join-flow test nodes)..."
z1_marker=$(log_lines_now z1); z2_marker=$(log_lines_now z2)
start_node z1
start_node z2

wait_for_log z1 "MIDR CL: MEMBER_PROBE_DONE → JOIN 群" "$JOIN_TIMEOUT" "JOIN" "$z1_marker"
wait_for_log z1 "MIDR I-7：ANCHOR " 30 "ANCHOR" "$z1_marker"
wait_for_log z2 "MIDR CL: MEMBER_PROBE_DONE → JOIN 群" "$JOIN_TIMEOUT" "JOIN" "$z2_marker"
wait_for_log z2 "MIDR I-7：ANCHOR " 30 "ANCHOR" "$z2_marker"

chmod -R a+rX "$TESTDIR/logs" 2>/dev/null || true

echo ""
result_rc=0
bash "$TESTDIR/check_result.sh" || result_rc=$?

echo "[bb-run_test] Stopping backbone-test processes before plotting..."
if ! bash "$TESTDIR/teardown.sh"; then
    echo "[bb-run_test] Failed to stop the testbed; retrying through the exit trap." >&2
    exit 1
fi
trap - EXIT

plot_rc=0
MPLCONFIGDIR=/tmp/midr-matplotlib "$PYTHON_BIN" "$TESTDIR/plot_backbone_join.py" \
    --z1 "$TESTDIR/logs/bgpd-z1.log" --z2 "$TESTDIR/logs/bgpd-z2.log" \
    --output "$TESTDIR/backbone_join_results.png" || plot_rc=$?
chmod a+r "$TESTDIR/backbone_join_results.png" 2>/dev/null || true
chmod a+r "$TESTDIR/run.log" 2>/dev/null || true

if [[ "$result_rc" -ne 0 || "$plot_rc" -ne 0 ]]; then
    exit 1
fi
