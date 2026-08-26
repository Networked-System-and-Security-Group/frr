#!/bin/bash
# run_test.sh — Organic group growth test (validates the MEMBER_PROBE_DONE
# JOIN-threshold cap fix in bgp_midr_cl.c).
#
# Before the fix, cl_handle_member_probe_done() required an uncapped,
# absolute good_links >= MIDR_CL_MIN_GOOD_LINKS (5) to JOIN. Group 1 here
# starts with only its rep (r) — 1 member — so under the OLD code every
# joiner would see far fewer than 5 known members, fail the bar, and CREATE
# its own singleton group instead of ever joining group 1. This is the exact
# bug shape: a group smaller than 5 can never gain a first member.
#
# With the fix (threshold = min(MIDR_CL_MIN_GOOD_LINKS, total known members),
# plus total_known > 0), joiners started one at a time should JOIN group 1
# even though the group has only 1, then 2, then 3 members at each step —
# demonstrating the threshold adapting as the group actually grows:
#   j1 sees total_known=1 (just r)        -> needs good_links >= 1 -> JOIN
#   j2 sees total_known=2 (r, j1)         -> needs good_links >= 2 -> JOIN
#   j3 sees total_known=3 (r, j1, j2)     -> needs good_links >= 3 -> JOIN
#
# j2/j3 are only started after the previous joiner's JOIN decision has
# actually fired — starting them earlier wouldn't exercise the growing
# total_known count, since r would not yet know about the previous joiner.
#
# Usage: sudo ./run_test.sh [--no-setup] [--timeout SECS]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BGPD="$REPO_ROOT/bgpd/.libs/bgpd"
# 合栈（对接轮 4）后必需：第二组给 lib/libfrr.c 加了新符号（frr_daemon_state_load_status
# 等），而宿主系统装的是旧 libfrr —— 不指过去，bgpd 起来就 `undefined symbol` 直接死，
# 而脚本会因为 grep 到上一轮的旧日志报出**假阳性通过**。clab 容器那边是整套换过产物才没事。
export LD_LIBRARY_PATH="$REPO_ROOT/lib/.libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
TESTDIR="$SCRIPT_DIR"
TIMEOUT=150   # seconds to wait for each joiner's JOIN decision
DO_SETUP=1

cd "$TESTDIR"

for arg in "$@"; do
    case "$arg" in
        --no-setup) DO_SETUP=0 ;;
        --timeout)  shift; TIMEOUT="$1" ;;
    esac
done

trap 'echo "[growth-run_test] Interrupted."; exit 1' INT TERM

echo "[growth-run_test] Checking for stale bgpd instances from a previous run..."
shopt -s nullglob
for pidfile in /tmp/bgpd-gr-*.pid; do
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
    echo "[growth-run_test] Setting up network namespaces..."
    bash "$TESTDIR/setup.sh"
fi

mkdir -p "$TESTDIR/logs"
rm -rf /tmp/midr-gr-vty && mkdir -p /tmp/midr-gr-vty

start_node() {
    local node="$1"
    mkdir -p "/tmp/midr-gr-vty/$node"
    ip netns exec "ns-gr-$node" "$BGPD" \
        -f "$TESTDIR/configs/bgpd-${node}.conf" \
        -Z -S \
        -i "/tmp/bgpd-gr-${node}.pid" \
        --vty_socket "/tmp/midr-gr-vty/$node" \
        --log-level debug &
    echo "  started bgpd for $node (bg pid $!)"
}

wait_for_join() {
    local node="$1"
    local log="$TESTDIR/logs/bgpd-${node}.log"
    local elapsed=0
    echo "[growth-run_test] Waiting up to ${TIMEOUT}s for $node's JOIN decision..."
    while [[ $elapsed -lt $TIMEOUT ]]; do
        if [[ -f "$log" ]] && grep -q "MIDR CL: MEMBER_PROBE_DONE → JOIN 群" "$log" 2>/dev/null; then
            echo ""
            echo "[growth-run_test] ✓ $node JOIN detected at t=${elapsed}s!"
            return 0
        fi
        printf "\r  elapsed: %3ds / %ds" "$elapsed" "$TIMEOUT"
        sleep 5
        elapsed=$((elapsed + 5))
    done
    echo ""
    echo "[growth-run_test] ✗ $node timed out — no JOIN decision in ${TIMEOUT}s."
    return 1
}

# 等引导把远端视图回调注册上。件②（对接轮 4）之后引导的代表目录只能由第二组的
# 回调来灌，而该回调挂在 bgp_config_end 上 —— 裸 bgpd（-f 读配置）不触发这个钩子
# （它由 vtysh 下发配置时发的 XFRR_end_configuration 触发），只能靠 periodic_sync
# 每 30s 兜底重试补上。原先这里死等 15s 短于 30s，joiner 必然撞空目录、CREATE
# 自建群。判据一律换成轮询真实状态，照 docs/midr-backbone运行手册.md §3.0 的规矩。
wait_bootstrap_ready() {
    local node="$1" timeout=90 elapsed=0
    echo "[growth-run_test] Waiting for $node's remote-view callback registration..."
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

# 等引导目录里真的出现 N 个代表 —— 这才是 joiner 能 JOIN 而不是 CREATE 的前置条件。
wait_rep_directory() {
    local node="$1" want="$2" timeout=90 elapsed=0 got=0
    echo "[growth-run_test] Waiting for $node's rep directory to list $want rep(s)..."
    while [[ $elapsed -lt $timeout ]]; do
        got=$("$REPO_ROOT/vtysh/.libs/vtysh" --vty_socket "/tmp/midr-gr-vty/$node" \
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

# 等群代表的节点表里真的涨到 N 个本群成员（含它自己）。下一个 joiner 问它要成员表
# 时拿到几条、CL 的 min(5, 已知成员数) 阈值算成几，全看这个数——所以等它，而不是
# 等一个"应该够了吧"的秒数。
wait_group_members() {
    local node="$1" gid="$2" want="$3" timeout=90 elapsed=0 got=0
    echo "[growth-run_test] Waiting for $node to know $want group-$gid member(s)..."
    while [[ $elapsed -lt $timeout ]]; do
        got=$("$REPO_ROOT/vtysh/.libs/vtysh" --vty_socket "/tmp/midr-gr-vty/$node" \
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

# 【我方适配】原版只有 4 个节点、r 兼任引导；本仓库引导与群代表硬互斥，故先起
# 专职引导 b，再起 r。r 是 GROUP_REP，挂靠钩子会自动把它挂到 b 上（活引导只有
# 一台，K=2 不足会降级运行并告警，属预期）；b 由此经第二组回调学到 r 的
# GROUP_REP 位，推导出代表目录，才能应答 joiner 的 REP_LIST_REQ。
echo "[growth-run_test] Starting b (dedicated bootstrap)..."
start_node b
wait_bootstrap_ready b || true

echo "[growth-run_test] Starting r (group-1 rep, 1 member so far)..."
start_node r
wait_rep_directory b 1 || true

echo "[growth-run_test] Starting j1 (group 1 has 1 known member: r)..."
start_node j1
wait_for_join j1 || true

wait_group_members r 1 2 || true

echo "[growth-run_test] Starting j2 (group 1 should now have 2 known members: r, j1)..."
start_node j2
wait_for_join j2 || true

wait_group_members r 1 3 || true

echo "[growth-run_test] Starting j3 (group 1 should now have 3 known members: r, j1, j2)..."
start_node j3
wait_for_join j3 || true

# Logs were written as root; open them up so a non-root reader can inspect
# results afterward without an extra manual chmod step.
chmod -R a+rX "$TESTDIR/logs" 2>/dev/null || true

echo ""
bash "$TESTDIR/check_result.sh"
