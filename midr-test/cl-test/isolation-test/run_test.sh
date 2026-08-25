#!/bin/bash
# run_test.sh — Isolation / RECONNECT test (validates MIDR_TRIGGER_ISOLATED +
# MIDR_DECISION_RECONNECT in bgp_midr.c / bgp_midr_cl.c).
#
# Reproduces the reported scenario in miniature: a bootstrap node (a) that
# stays up the whole time, two singleton-group reps (d, e) that a joining
# node f ends up depending on for *all* of its connectivity, and a kill of
# both d and e partway through -- leaving f with zero established sessions
# even though a is still perfectly reachable. Before this fix, nothing would
# ever notice; f would sit isolated forever.
#
# f's rep directory (served by a) lists only d and e -- d has lower delay so
# REP_PROBE_DONE ranks it as the primary (JOIN) target, e is the sole
# runner-up (ANCHOR) candidate. After the initial join, f's established
# sessions are exactly {d, e}, nothing else -- so killing both is a clean,
# total isolation, not a partial degradation.
#
# Sequence:
#   t=0      a, d, e start. f starts a few seconds later (bootstrap fires
#            immediately).
#   t≈125    f has JOINed d's group and ANCHOR-connected to e.
#   (kill)   d and e are both killed.
#   +≤150s   MIDR_TRIGGER_ISOLATED fires (2 x 30s debounce + margin) and CL
#            emits MIDR_DECISION_RECONNECT.
#   +≤30s    NDS executes RECONNECT: group cleared, fresh REP_LIST_REQ sent
#            to the bootstrap (a, still alive).
#   +≤150s   Since d/e are both dead, the fresh REP_PROBE_DONE finds no live
#            rep and CL falls back to CREATE -- confirming f is genuinely
#            un-stuck, not just that it tried once and gave up.
#
# Usage: sudo ./run_test.sh [--no-setup] [--join-timeout SECS]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BGPD="$REPO_ROOT/bgpd/.libs/bgpd"
VTYSH="$REPO_ROOT/vtysh/.libs/vtysh"
# 合栈（对接轮 4）后必需，理由同 growth-test/run_test.sh 同处注释：宿主系统的旧
# libfrr 缺第二组新加的符号，不指过去 bgpd 起不来、而脚本会 grep 到旧日志报假阳性。
export LD_LIBRARY_PATH="$REPO_ROOT/lib/.libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
TESTDIR="$SCRIPT_DIR"
JOIN_TIMEOUT=150
ISOLATION_TIMEOUT=150
REJOIN_TIMEOUT=30
FALLBACK_TIMEOUT=150
DO_SETUP=1
PYTHON_BIN="${MIDR_PYTHON_BIN:-python3}"
declare -A BG_PIDS=()
declare -A EXPECTED_STOPS=()
fault_rc=0

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

wait_for_nodes() {
    local failed=0 node rc
    for node in "${!BG_PIDS[@]}"; do
        if wait "${BG_PIDS[$node]}"; then
            continue
        else
            rc=$?
            if [[ "${EXPECTED_STOPS[$node]:-0}" -eq 1 && "$rc" -eq 143 ]]; then
                continue
            fi
            echo "[iso-run_test] $node exited abnormally (status $rc)." >&2
            failed=1
        fi
    done
    BG_PIDS=()
    EXPECTED_STOPS=()
    return "$failed"
}

cleanup() {
    local rc=$?
    trap - EXIT
    bash "$TESTDIR/teardown.sh" || true
    wait_for_nodes || true
    chmod -R a+rX "$TESTDIR/logs" 2>/dev/null || true
    chmod a+r "$TESTDIR/run.log" 2>/dev/null || true
    chmod a+r "$TESTDIR/isolation_results.png" 2>/dev/null || true
    exit "$rc"
}
trap cleanup EXIT
trap 'echo "[iso-run_test] Interrupted."; exit 130' INT TERM

echo "[iso-run_test] Checking for stale bgpd instances from a previous run..."
shopt -s nullglob
for pidfile in /tmp/bgpd-iso-*.pid; do
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
    echo "[iso-run_test] Removing stale isolation-test processes and namespaces..."
    bash "$TESTDIR/teardown.sh"
    echo "[iso-run_test] Setting up network namespaces..."
    bash "$TESTDIR/setup.sh"
fi

mkdir -p "$TESTDIR/logs"
rm -f "$TESTDIR/logs"/*.log \
    "$TESTDIR/isolation_results.png"
rm -rf /tmp/midr-iso-vty && mkdir -p /tmp/midr-iso-vty

start_node() {
    local node="$1"
    mkdir -p "/tmp/midr-iso-vty/$node"
    ip netns exec "ns-iso-$node" "$BGPD" \
        -f "$TESTDIR/configs/bgpd-${node}.conf" \
        -Z -S \
        -i "/tmp/bgpd-iso-${node}.pid" \
        --vty_socket "/tmp/midr-iso-vty/$node" \
        --log-level debug &
    BG_PIDS[$node]="$!"
    echo "  started bgpd for $node (bg pid $!)"
}

# Only matches lines added to f's log *after* $since_line -- several of the
# patterns we wait for (REP_LIST_REQ, CREATE) also legitimately appear once
# during the very first join, so an unscoped grep would false-positive on
# that old line instead of proving a fresh one showed up post-recovery.
wait_for_log() {
    local pattern="$1" timeout="$2" label="$3" since_line="$4"
    local log="$TESTDIR/logs/bgpd-f.log"
    local elapsed=0
    while [[ $elapsed -lt $timeout ]]; do
        if [[ -f "$log" ]] && tail -n "+${since_line}" "$log" | grep -q "$pattern" 2>/dev/null; then
            echo ""
            echo "[iso-run_test] ✓ $label detected at t≈${elapsed}s!"
            return 0
        fi
        printf "\r  elapsed: %3ds / %ds" "$elapsed" "$timeout"
        sleep 5
        elapsed=$((elapsed + 5))
    done
    echo ""
    echo "[iso-run_test] ✗ $label not seen within ${timeout}s."
    return 1
}

log_lines_now() {
    local log="$TESTDIR/logs/bgpd-f.log"
    [[ -f "$log" ]] && wc -l < "$log" || echo 0
}

# 起引导 a，等它把远端视图回调注册上再起代表。件②（对接轮 4）之后 a 的代表目录
# 只能由第二组回调来灌，而该回调挂在 bgp_config_end 上 —— 裸 bgpd（-f 读配置）不
# 触发这个钩子（它由 vtysh 下发配置时发的 XFRR_end_configuration 触发），只能靠
# periodic_sync 每 30s 兜底重试补上。原先这里死等 10s 短于 30s，f 必然撞空目录、
# CREATE 自建群并自任代表，于是 Phase 1/2 全废（自任代表后 PERIODIC_SYNC 直接
# "本节点是群代表，跳过退群判定"，孤岛检测一行都执行不到）。
# 判据一律换成轮询真实状态，照 docs/midr-backbone运行手册.md §3.0 的规矩。
wait_bootstrap_ready() {
    local node="$1" timeout=90 elapsed=0
    echo "[iso-run_test] Waiting for $node's remote-view callback registration..."
    while [[ $elapsed -lt $timeout ]]; do
        if grep -q "已向第二组注册 node/link 回调" \
             "$TESTDIR/logs/bgpd-${node}.log" 2>/dev/null; then
            echo "  ✓ $node ready at t=${elapsed}s"
            return 0
        fi
        sleep 3
        elapsed=$((elapsed + 3))
    done
    echo "  ✗ $node never registered its remote-view callback within ${timeout}s"
    return 1
}

# 等 a 的目录里真的出现 d、e 两个代表 —— f 能 JOIN 而不是 CREATE 的前置条件。
wait_rep_directory() {
    local node="$1" want="$2" timeout=90 elapsed=0 got=0
    echo "[iso-run_test] Waiting for $node's rep directory to list $want rep(s)..."
    while [[ $elapsed -lt $timeout ]]; do
        got=$("$VTYSH" --vty_socket "/tmp/midr-iso-vty/$node" \
                  -c 'show midr reps' 2>/dev/null | grep -c '^10\.0\.' || true)
        if [[ "$got" -ge "$want" ]]; then
            echo "  ✓ $node's directory lists $got rep(s) at t=${elapsed}s"
            return 0
        fi
        sleep 5
        elapsed=$((elapsed + 5))
    done
    echo "  ✗ $node's directory still lists $got/$want rep(s) after ${timeout}s"
    return 1
}

echo "[iso-run_test] Starting a (bootstrap)..."
start_node a
wait_bootstrap_ready a

echo "[iso-run_test] Starting d and e (singleton reps)..."
start_node d
start_node e
wait_rep_directory a 2

echo "[iso-run_test] Starting f (join flow begins automatically)..."
start_node f
sleep 2  # let the log file get created before we start tailing it from line 1

wait_for_log "MIDR CL: MEMBER_PROBE_DONE → JOIN 群" "$JOIN_TIMEOUT" "JOIN" 1
wait_for_log "MIDR I-7：ANCHOR " 30 "ANCHOR" 1

echo "[iso-run_test] Waiting 5s for sessions to settle before killing d and e..."
sleep 5

echo "[iso-run_test] Killing d and e -- f should now have zero established sessions..."
kill_marker=$(( $(log_lines_now) + 1 ))
kill_time=$(date '+%Y/%m/%d %H:%M:%S.%3N')
# 落盘给 check_result.sh：kill 之后才算数的判据要按它卡行号
printf '%s\n%s\n' "$kill_marker" "$kill_time" > "$TESTDIR/logs/kill_marker.log"
for node in d e; do
    pidfile="/tmp/bgpd-iso-${node}.pid"
    pid=$(cat "$pidfile" 2>/dev/null || true)
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
        EXPECTED_STOPS[$node]=1
        kill "$pid" 2>/dev/null || true
        echo "  killed $node (pid $pid)"
    else
        echo "  ✗ no running pid for $node -- was it already dead?"
        fault_rc=1
    fi
done

wait_for_log "MIDR CL: ISOLATED" "$ISOLATION_TIMEOUT" "ISOLATED trigger / RECONNECT decision" "$kill_marker" || true
wait_for_log "MIDR JOIN: sent REP_LIST_REQ to bootstrap" "$REJOIN_TIMEOUT" "fresh REP_LIST_REQ to bootstrap (rejoin attempt)" "$kill_marker" || true
wait_for_log "CREATE 新群" "$FALLBACK_TIMEOUT" "fallback CREATE (d/e both dead, no live rep to join)" "$kill_marker" || true

chmod -R a+rX "$TESTDIR/logs" 2>/dev/null || true

echo ""
result_rc=0
bash "$TESTDIR/check_result.sh" || result_rc=$?

echo "[iso-run_test] Stopping isolation-test processes before plotting..."
shutdown_rc=0
if ! bash "$TESTDIR/teardown.sh"; then
    echo "[iso-run_test] Failed to stop the testbed; retrying through the exit trap." >&2
    shutdown_rc=1
fi
wait_for_nodes || shutdown_rc=1
trap - EXIT

plot_rc=0
MPLCONFIGDIR=/tmp/midr-matplotlib "$PYTHON_BIN" "$TESTDIR/plot_isolation.py" \
    "$TESTDIR/logs/bgpd-f.log" --since-line "$kill_marker" \
    --kill-time "$kill_time" \
    --output "$TESTDIR/isolation_results.png" || plot_rc=$?
chmod a+r "$TESTDIR/isolation_results.png" 2>/dev/null || true
chmod a+r "$TESTDIR/run.log" 2>/dev/null || true

if [[ "$result_rc" -ne 0 || "$plot_rc" -ne 0 || "$shutdown_rc" -ne 0 || "$fault_rc" -ne 0 ]]; then
    exit 1
fi
