#!/bin/bash
# run_test.sh — Main CL integration test.
#
# Topology: 10 nodes in separate Linux network namespaces, all connected via
# an ns-hub L3 router.  tc-netem adds one-way delay on hub→node interfaces
# (see setup.sh for why intra-group RTT is double a node's own delay):
#   Group 1 (g1a-g1e): 3 ms → RTT from newnode ≈  3 ms, intra-group ≈  6 ms — best, chosen
#   Group 3 (g3a-g3b): 6 ms → RTT from newnode ≈  6 ms, intra-group ≈ 12 ms — 2nd, 1st anchor group
#   Group 2 (g2a-g2b): 50 ms → RTT from newnode ≈ 50 ms, intra-group ≈ 100 ms — 3rd, 2nd anchor group
#
# Expected result: newnode JOINs group 1 after ≈125 seconds, and — as a side
# effect of the same RECOMMEND event (doc/change-reply.md B2/疑2) — anchor-
# connects to the best 2 nodes in each of the two runner-up groups (3 and 2),
# completing a few seconds after JOIN.
# Timing breakdown (MIDR_JOIN_PROBE_WAIT_SECS = 60):
#   t=0    All nodes start; newnode sends REP_LIST_REQ immediately.
#   t≈5    BGP-LS sessions establish; g1a builds full group-1 member list and
#          learns g2a/g3a (cross-group BGP-LS neighbors) for its rep directory.
#   t≈3    g1a replies to REP_LIST_REQ (or retry at t≈3 if g1a not ready yet).
#   t≈63   REP_PROBE_DONE fires (60 s EWMA warm-up): ranks g1a(≈2.9ms) <
#          g3a(≈5.8ms) < g2a(≈48ms) → CL RECOMMEND group 1, anchor_reps=[g3a,g2a].
#   t≈63   NDS sends MEMBER_LIST_REQ to g1a (main) and to g3a/g2a (anchors).
#   t≈123  MEMBER_PROBE_DONE fires: 5 group-1 members × RTT < 20 ms → CL JOIN group 1.
#   t≈123  ANCHOR_PROBE_DONE fires (~same time, own independent timer): CL
#          picks the best 2 nodes in group 3 and the best 2 in group 2 → I-7
#          ANCHOR → NDS connects to all 4.
#
# Usage: sudo ./run_test.sh [--no-setup] [--timeout SECS]

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BGPD="$REPO_ROOT/bgpd/.libs/bgpd"
VTYSH="$REPO_ROOT/vtysh/.libs/vtysh"
LIBDIR="$REPO_ROOT/lib/.libs"
TESTDIR="$SCRIPT_DIR"
TIMEOUT=150   # seconds to wait for JOIN decision
DO_SETUP=1
ADDRESS_FAMILY="ipv4"
ANCHOR_ESTABLISH_TIMEOUT="${MIDR_ANCHOR_ESTABLISH_TIMEOUT:-60}"
POST_CONVERGENCE_TIMEOUT="${MIDR_POST_CONVERGENCE_TIMEOUT:-120}"
CAPTURE_RUN_ID="${MIDR_CAPTURE_RUN_ID:-$(date +%Y%m%d-%H%M%S)-$$}"
CAPTURE_ROOT="${MIDR_CAPTURE_ROOT:-$TESTDIR/artifacts/$CAPTURE_RUN_ID}"
CONFIG_DIR="$CAPTURE_ROOT/configs"
export MIDR_CAPTURE_RUN_ID="$CAPTURE_RUN_ID"
export MIDR_CAPTURE_ROOT="$CAPTURE_ROOT"

capture_state() {
    local phase="$1"

    bash "$TESTDIR/capture_state.sh" "$phase" ||
        echo "[run_test] State capture failed for phase '$phase'; continuing." >&2
}

archive_logs() {
    mkdir -p "$CAPTURE_ROOT/logs"
    cp -f "$TESTDIR"/logs/*.log "$CAPTURE_ROOT/logs/" 2>/dev/null || true
    chmod -R a+rX "$CAPTURE_ROOT" 2>/dev/null || true
    if [[ -n "${SUDO_UID:-}" && -n "${SUDO_GID:-}" ]]; then
        chown -R "$SUDO_UID:$SUDO_GID" "$CAPTURE_ROOT" 2>/dev/null || true
    fi
}

while (( $# > 0 )); do
    case "$1" in
        --address-family)
            ADDRESS_FAMILY="${2:-}"
            shift 2
            ;;
        --no-setup)
            DO_SETUP=0
            shift
            ;;
        --timeout)
            TIMEOUT="${2:-}"
            shift 2
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 2
            ;;
    esac
done
if [[ "$ADDRESS_FAMILY" != "ipv4" && "$ADDRESS_FAMILY" != "ipv6" ]]; then
    echo "--address-family must be ipv4 or ipv6." >&2
    exit 2
fi

if [[ $EUID -ne 0 ]]; then
    echo "[run_test] This test must run as root: sudo ./run_test.sh" >&2
    exit 2
fi
if [[ ! -x "$BGPD" || ! -x "$VTYSH" || ! -r "$LIBDIR/libfrr.so.0" ]]; then
    echo "[run_test] Build artifacts are missing; build bgpd, vtysh, and libfrr first." >&2
    exit 2
fi

# sudo clears the caller's LD_LIBRARY_PATH.  The test launches uninstalled
# .libs binaries directly, so point the dynamic linker at the matching libfrr.
export LD_LIBRARY_PATH="$LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
MISSING_LIBS=$(ldd "$BGPD" 2>&1 | awk '/not found/ {print}')
if [[ -n "$MISSING_LIBS" ]]; then
    echo "[run_test] bgpd has unresolved runtime libraries:" >&2
    echo "$MISSING_LIBS" >&2
    exit 2
fi
if ! strings "$VTYSH" | grep -qF 'show midr self' ||
   ! strings "$VTYSH" | grep -qF 'show midr ted detail'; then
    echo "[run_test] vtysh has a stale command table; run: make -j\$(nproc) vtysh/vtysh" >&2
    exit 2
fi

# bgpd config files use log paths relative to TESTDIR (e.g. "logs/bgpd-g1a.log"),
# so bgpd must be launched with TESTDIR as its cwd.
cd "$TESTDIR"
mkdir -p "$CAPTURE_ROOT"
exec > >(tee "$CAPTURE_ROOT/console.log") 2>&1

cleanup() {
    status=$?
    trap - EXIT INT TERM
    archive_logs
    bash "$TESTDIR/teardown.sh" || true
    exit "$status"
}
trap cleanup EXIT INT TERM

# ---- 0. Reap any stale bgpd instances left running by a previous, ----------
#         incomplete run (timed out, Ctrl-C'd, or teardown.sh skipped).
# `ip netns del` does NOT kill processes still running inside a namespace —
# the namespace just stays alive, invisible to `ip netns list`, for as long
# as that orphan process holds it open. The orphan keeps its old
# /tmp/bgpd-cl-<node>.pid locked, so this run's freshly started bgpd for that
# same node silently fails at "Could not lock pid_file ... exiting" and the
# node never comes up — with no obvious error at the run_test.sh level.
echo "[run_test] Checking for stale bgpd instances from a previous run..."
shopt -s nullglob
for pidfile in /tmp/bgpd-cl-*.pid; do
    pid=$(cat "$pidfile" 2>/dev/null || true)
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
        echo "  killing stale $(basename "$pidfile") (pid $pid)"
        kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$pidfile"
done
shopt -u nullglob
sleep 1

# ---- 1. Network setup -------------------------------------------------------
if [[ "$DO_SETUP" -eq 1 ]]; then
    echo "[run_test] Setting up network namespaces..."
    bash "$TESTDIR/setup.sh" --address-family "$ADDRESS_FAMILY"
fi

# ---- 2. Create log and VTY dirs ---------------------------------------------
mkdir -p "$TESTDIR/logs"
# bgpd appends to configured log files.  Old JOIN/ANCHOR lines would make a
# rerun satisfy the log-based waits immediately, so start each run with empty
# live logs; the previous run is already preserved under artifacts/<run-id>/.
shopt -s nullglob
for logfile in "$TESTDIR"/logs/bgpd-*.log; do
    : >"$logfile"
done
shopt -u nullglob
rm -rf /tmp/midr-cl-vty && mkdir -p /tmp/midr-cl-vty
mkdir -p "$CAPTURE_ROOT"
bash "$TESTDIR/render_configs.sh" "$ADDRESS_FAMILY" "$CONFIG_DIR"
{
    echo "run_id=$CAPTURE_RUN_ID"
    echo "started_at=$(date --iso-8601=seconds)"
    echo "git_commit=$(git -C "$REPO_ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "address_family=$ADDRESS_FAMILY"
    echo "timeout_seconds=$TIMEOUT"
    echo "anchor_establish_timeout_seconds=$ANCHOR_ESTABLISH_TIMEOUT"
    echo "post_convergence_timeout_seconds=$POST_CONVERGENCE_TIMEOUT"
} >"$CAPTURE_ROOT/run-metadata.txt"

# ---- 3. 分阶段启动 ----------------------------------------------------------
# 按依赖链 引导 → 代表 → 成员 → 新节点（docs/midr-backbone运行手册.md §3.2），
# 每级之间等**真实收敛判据**、不等固定秒数。原先 9 台齐起 + 死等 15s 两条都违反：
# 引导的代表目录由第二组回调灌，而该回调挂在 bgp_config_end 上、裸 bgpd 不触发
# （靠 periodic_sync 每 30s 兜底），15s 内目录必空 → 人人 CREATE 自建群、群号乱套。
start_node() {
    local node="$1"
    local pid

    mkdir -p "/tmp/midr-cl-vty/$node"
    ip netns exec "ns-$node" "$BGPD" \
        -f "$CONFIG_DIR/bgpd-${node}.conf" \
        -Z -S \
        -i "/tmp/bgpd-cl-${node}.pid" \
        --vty_socket "/tmp/midr-cl-vty/$node" \
        --log-level debug &
    pid=$!
    echo "  started bgpd for $node (bg pid $pid)"
    sleep 1
    if ! kill -0 "$pid" 2>/dev/null; then
        wait "$pid" || true
        echo "[run_test] bgpd for $node exited during startup; aborting." >&2
        return 1
    fi
}

wait_bootstrap_ready() {
    local node="$1" timeout=90 elapsed=0
    echo "[run_test] Waiting for $node's remote-view callback registration..."
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

wait_rep_directory() {
    local node="$1" want="$2" timeout=90 elapsed=0 got=0
    echo "[run_test] Waiting for $node's rep directory to list $want rep(s)..."
    while [[ $elapsed -lt $timeout ]]; do
        got=$("$VTYSH" --vty_socket "/tmp/midr-cl-vty/$node" \
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

# 等各群代表认齐本群成员——newnode 问它要成员表时拿到几条、CL 的
# min(5, 已知成员数) 阈值算成几，全看这个数。
# timeout 要盖住成员自己那趟 join：REP 60s + MEMBER 60s 两段 EWMA 热身 ≈125s，
# 再留出并发起动的余量。设 120s 会差十几秒（2026-08-25 实测卡在 2/4）。
wait_group_members() {
    local node="$1" gid="$2" want="$3" timeout=200 elapsed=0 got=0
    echo "[run_test] Waiting for $node to know $want group-$gid member(s)..."
    while [[ $elapsed -lt $timeout ]]; do
        got=$("$VTYSH" --vty_socket "/tmp/midr-cl-vty/$node" \
                  -c 'show midr nodes' 2>/dev/null \
                  | awk -v g="$gid" '$1 ~ /^10\.0\./ && $4 == g' | wc -l)
        if [[ "$got" -ge "$want" ]]; then
            echo "  ✓ $node knows $got group-$gid member(s) at t=${elapsed}s"
            return 0
        fi
        sleep 5
        elapsed=$((elapsed + 5))
    done
    echo "  ✗ $node still knows only $got/$want group-$gid member(s) after ${timeout}s"
    return 1
}

vty_node() {
    local node="$1"
    local command="$2"

    "$VTYSH" --vty_socket "/tmp/midr-cl-vty/$node" \
        -c "$command" 2>/dev/null
}

anchor_established_count() {
    vty_node newnode "show midr neighbors" \
        | awk '/Established/ && /CL_ANCHOR/' | wc -l
}

wait_anchor_sessions() {
    local want="$1" timeout="$2" elapsed=0 got=0

    echo "[run_test] Waiting up to ${timeout}s for $want/$want Anchor sessions to establish..."
    while [[ $elapsed -lt $timeout ]]; do
        got=$(anchor_established_count || true)
        if [[ "$got" -ge "$want" ]]; then
            echo "  ✓ $got/$want Anchor sessions Established at t=${elapsed}s"
            return 0
        fi
        sleep 2
        elapsed=$((elapsed + 2))
    done
    echo "  ✗ only $got/$want Anchor sessions Established after ${timeout}s"
    return 1
}

post_convergence_status() {
    local new_lsdb new_ted g1b_objects

    new_lsdb=$(vty_node newnode "show midr lsdb summary" || true)
    new_ted=$(vty_node newnode "show midr ted detail" || true)
    g1b_objects=$(vty_node g1b "show midr lsdb objects" || true)

    POST_ANCHORS=$(anchor_established_count || true)
    POST_MEMBERSHIPS=$(awk '/^  memberships:/ {print $2; exit}' <<<"$new_lsdb")
    POST_PENDING=$(awk '/^  pending:/ {print $2; exit}' <<<"$new_lsdb")
    POST_TED_NODES=$(sed -n 's/^nodes (\([0-9][0-9]*\)):.*/\1/p' \
        <<<"$new_ted" | head -1)
    if grep -q \
        'object type=MEMBERSHIP origin=10.0.99.1 .*usable=yes' \
        <<<"$g1b_objects"; then
        POST_G1B_SEES_NEWNODE=yes
    else
        POST_G1B_SEES_NEWNODE=no
    fi

    POST_MEMBERSHIPS=${POST_MEMBERSHIPS:-0}
    POST_PENDING=${POST_PENDING:-unknown}
    POST_TED_NODES=${POST_TED_NODES:-0}
}

wait_post_convergence() {
    local timeout="$1" elapsed=0 stable=0

    echo "[run_test] Waiting up to ${timeout}s for Membership/LSDB/TED convergence..."
    while [[ $elapsed -lt $timeout ]]; do
        post_convergence_status
        if [[ "$POST_ANCHORS" -ge 4 && "$POST_MEMBERSHIPS" -ge 9 &&
              "$POST_PENDING" == 0 && "$POST_TED_NODES" -ge 5 &&
              "$POST_G1B_SEES_NEWNODE" == yes ]]; then
            stable=$((stable + 1))
            if [[ $stable -ge 2 ]]; then
                echo "  ✓ stable: anchors=$POST_ANCHORS memberships=$POST_MEMBERSHIPS" \
                     "pending=$POST_PENDING ted-nodes=$POST_TED_NODES" \
                     "g1b-sees-newnode=$POST_G1B_SEES_NEWNODE"
                return 0
            fi
        else
            stable=0
        fi
        if (( elapsed % 10 == 0 )); then
            echo "  elapsed=${elapsed}s anchors=$POST_ANCHORS" \
                 "memberships=$POST_MEMBERSHIPS pending=$POST_PENDING" \
                 "ted-nodes=$POST_TED_NODES" \
                 "g1b-sees-newnode=$POST_G1B_SEES_NEWNODE"
        fi
        sleep 3
        elapsed=$((elapsed + 3))
    done
    post_convergence_status
    echo "  ✗ not converged after ${timeout}s: anchors=$POST_ANCHORS" \
         "memberships=$POST_MEMBERSHIPS pending=$POST_PENDING" \
         "ted-nodes=$POST_TED_NODES" \
         "g1b-sees-newnode=$POST_G1B_SEES_NEWNODE"
    return 1
}

echo "[run_test] Stage 1/4: bootstrap (g1a)..."
start_node g1a || exit 1
wait_bootstrap_ready g1a || true
capture_state after-bootstrap

echo "[run_test] Stage 2/4: group representatives (g1b, g2a, g3a)..."
for node in g1b g2a g3a; do start_node "$node" || exit 1; sleep 1; done
wait_rep_directory g1a 3 || true
capture_state after-representatives

echo "[run_test] Stage 3/4: members (g1c g1d g1e g2b g3b)..."
for node in g1c g1d g1e g2b g3b; do start_node "$node" || exit 1; sleep 1; done
# 群 1 该有 4 台（g1b 代表 + g1c/g1d/g1e）；群 2/3 各 2 台
wait_group_members g1b 1 4 || true
capture_state after-members

# ---- 4. Start newnode (bootstrap command in config fires immediately) --------
echo "[run_test] Stage 4/4: newnode (join flow will begin automatically)..."
start_node newnode || exit 1
capture_state after-newnode-start

# ---- 5. Wait for JOIN decision in newnode's log -----------------------------
LOG="$TESTDIR/logs/bgpd-newnode.log"
echo "[run_test] Waiting up to ${TIMEOUT}s for 'JOIN' decision in newnode log..."
elapsed=0
join_detected=0
while [[ $elapsed -lt $TIMEOUT ]]; do
    if [[ -f "$LOG" ]] && grep -q "MIDR CL: MEMBER_PROBE_DONE → JOIN 群" "$LOG" 2>/dev/null; then
        echo ""
        echo "[run_test] ✓ JOIN decision detected at t=${elapsed}s!"
        join_detected=1
        break
    fi
    printf "\r  elapsed: %3ds / %ds" "$elapsed" "$TIMEOUT"
    sleep 5
    elapsed=$((elapsed + 5))
done
echo ""

if [[ $elapsed -ge $TIMEOUT ]]; then
    echo "[run_test] ✗ Timed out — no JOIN decision in ${TIMEOUT}s."
fi
if [[ $join_detected -eq 1 ]]; then
    capture_state after-join
else
    capture_state join-timeout
fi

# ---- 5b. Wait a bit more for the anchor-connection side effect --------------
# ANCHOR_PROBE_DONE runs on its own independent timer (restarted whenever
# either runner-up group's member list arrives), so it can land a few seconds
# after JOIN. Give it up to 30 s before declaring it missing.
anchor_decision_detected=0
if [[ $elapsed -lt $TIMEOUT ]]; then
    echo "[run_test] Waiting up to 30s for the ANCHOR decision (group-3/group-2 anchor connections)..."
    anchor_elapsed=0
    while [[ $anchor_elapsed -lt 30 ]]; do
        if grep -q "MIDR I-7：ANCHOR " "$LOG" 2>/dev/null; then
            echo "[run_test] ✓ ANCHOR decision detected at t≈$((elapsed + anchor_elapsed))s!"
            anchor_decision_detected=1
            break
        fi
        sleep 3
        anchor_elapsed=$((anchor_elapsed + 3))
    done
    if [[ $anchor_elapsed -ge 30 ]]; then
        echo "[run_test] ✗ No ANCHOR decision seen within 30s of JOIN — check logs."
    fi
fi

if [[ $join_detected -eq 1 && $anchor_decision_detected -eq 1 ]]; then
    if wait_anchor_sessions 4 "$ANCHOR_ESTABLISH_TIMEOUT"; then
        capture_state after-anchor-sessions
    else
        capture_state anchor-session-timeout
    fi

    if wait_post_convergence "$POST_CONVERGENCE_TIMEOUT"; then
        capture_state post-convergence
    else
        capture_state post-convergence-timeout
    fi
else
    echo "[run_test] Skipping convergence wait because JOIN/ANCHOR decision is missing."
    capture_state convergence-not-started
fi

# ---- 6. Show result summary -------------------------------------------------
echo ""
bash "$TESTDIR/check_result.sh"
capture_state final
archive_logs
python3 "$TESTDIR/extract_decisions.py" \
    --log "$LOG" \
    --output "$CAPTURE_ROOT/decisions.json" \
    --address-family "$ADDRESS_FAMILY"
ln -sfn "$CAPTURE_ROOT" "$TESTDIR/artifacts/latest-$ADDRESS_FAMILY"
echo "[run_test] State artifacts: $CAPTURE_ROOT"
