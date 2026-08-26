#!/bin/bash
# test_session_exclude.sh — Exercise `no midr session` / `midr session` persistent
# exclusion semantics (B1-Q2: excluded nodes must not be silently reconnected by
# the next automatic reconvergence; an explicit `midr session ... remote-as`
# must always be able to override the exclusion).
#
# Prerequisite: run_test.sh has already been run against this same topology and
# newnode has JOINed group 1 (bgpd instances for all nodes are still running in
# their namespaces — this script does not start or stop bgpd).
#
# What it does, against newnode (group 1, members g1a-g1e):
#   1. `no midr session <g1b>`      → session torn down + added to exclude list.
#   2. Force a reconvergence (`midr group-id 0` then `midr group-id 1` — a
#      no-op group membership round trip that still calls the same
#      connect_group() reconvergence path a real LEAVE/rejoin or manual
#      `midr group-id` change would) → g1b must stay disconnected.
#   3. `midr session <g1b> remote-as <ASN>` → exclusion lifted, session rebuilt.
#
# Usage: sudo ./test_session_exclude.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
VTYSH="$REPO_ROOT/vtysh/.libs/vtysh"
LOG="$SCRIPT_DIR/logs/bgpd-newnode.log"
VTY="/tmp/midr-cl-vty/newnode"

# Target: g1b, an ordinary group-1 member (not the rep, to keep this test
# independent of REP_LIST/MEMBER_LIST re-serving behavior).
TARGET_TRANSPORT="10.10.12.2"
TARGET_ASN="65012"
TARGET_RID="10.0.12.1"

fail() { echo "[test_session_exclude] ✗ $1"; exit 1; }
pass() { echo "[test_session_exclude] ✓ $1"; }

# A bare `vtysh` resolves to whatever's in PATH — on a machine with a system
# FRR package installed, that binary has no MIDR commands and fails outright.
# Use the repo-built one explicitly.
[[ -x "$VTYSH" ]] || fail "$VTYSH not found or not executable — build it first (make vtysh/vtysh)"

[[ -S "$VTY/bgpd.vty" ]] || fail "no vty socket at $VTY — run run_test.sh first and leave it running"
[[ -f "$LOG" ]] || fail "$LOG not found — run run_test.sh first"
grep -q "MIDR CL: MEMBER_PROBE_DONE → JOIN 群" "$LOG" || \
    fail "newnode hasn't JOINed a group yet — run run_test.sh to completion first"

vty() { "$VTYSH" --vty_socket "$VTY" "$@"; }

# Baseline: confirm g1b is currently connected before we start excluding it.
vty -c "show midr neighbors" | grep -q "$TARGET_TRANSPORT" || \
    echo "[test_session_exclude] warning: $TARGET_TRANSPORT not seen in 'show midr neighbors' yet — proceeding anyway"

lines_before=$(wc -l < "$LOG")

echo "[test_session_exclude] Step 1: no midr session $TARGET_TRANSPORT"
vty -c "configure terminal" -c "router bgp 65099" -c "no midr session $TARGET_TRANSPORT"
sleep 1
# 判据按 rid 认人：排除名单自 2026-08-11（保底轮 2 批 2）起以 router-id 为键
# （原为 transport 地址——运维改了对端 transport 排除就静默失效），日志随之打
# 的是 `MIDR 会话排除：router-id <rid> 加入排除名单`。命令入参仍是 transport。
tail -n +"$lines_before" "$LOG" | grep -q "MIDR 会话排除：router-id ${TARGET_RID} 加入排除名单" \
    && pass "exclusion recorded for $TARGET_TRANSPORT (rid $TARGET_RID)" \
    || fail "no '加入排除名单' log line found for rid $TARGET_RID"

lines_before=$(wc -l < "$LOG")

echo "[test_session_exclude] Step 2: forcing a reconvergence (group-id 0 -> 1)..."
vty -c "configure terminal" -c "router bgp 65099" -c "midr group-id 0"
sleep 2
vty -c "configure terminal" -c "router bgp 65099" -c "midr group-id 1"
sleep 3

# 群内重收敛走 midr_discovery_should_peer()，它命中排除名单时打这条 info（08-18 新增；
# 在那之前这条路径是静默的，原判据 grep 的是 midr_ctrl_connect() 里那行"跳过自动建连"——
# 那行只覆盖不过 should_peer 的路径[群间 ANCHOR/骨干/挂靠]，重收敛永远碰不到，故必然误报）。
# 接收侧那条（对端来敲门被拒 + PEER_REJECT）也算数，但不能只靠它：双侧对称执行
# `no midr session` 时对端根本不会来敲门。
if tail -n +"$lines_before" "$LOG" | grep -qE "MIDR 会话排除：router-id ${TARGET_RID} 在排除名单中，群内自动建连跳过它|MIDR ctrl: 拒绝 PEER_REQUEST —— 发起方 rid ${TARGET_RID} 在会话排除名单中"; then
    pass "reconvergence kept $TARGET_RID excluded (should_peer skip, or receive-side reject)"
else
    fail "no exclusion evidence for $TARGET_RID after reconvergence — exclusion did not survive"
fi

# ⚠ 判据查 **Established**，不查"出现在表里"：`show midr neighbors` 会把 Active/Connect
# 等未建成的状态一并列出（表尾那句"State 列为其他状态的行也在表内"就是提醒这个）。被排除的
# 对端仍会不断发起连接、在本机留下一个 Active 的 peer 对象——那恰恰是"没连上"的证据，
# 拿"表里有这一行"当失败会误报（08-18 实撞）。排除生效 = 建不成会话，不是查无此行。
if vty -c "show midr neighbors" | grep "$TARGET_TRANSPORT" | grep -q "Established"; then
    fail "$TARGET_TRANSPORT is Established in 'show midr neighbors' — it got reconnected despite exclusion"
else
    pass "$TARGET_TRANSPORT not Established — stays disconnected (an Active/retrying entry is expected)"
fi

lines_before=$(wc -l < "$LOG")

echo "[test_session_exclude] Step 3: midr session $TARGET_TRANSPORT remote-as $TARGET_ASN (explicit override)"
vty -c "configure terminal" -c "router bgp 65099" -c "midr session $TARGET_TRANSPORT remote-as $TARGET_ASN"
sleep 1
tail -n +"$lines_before" "$LOG" | grep -q "MIDR 会话排除：router-id ${TARGET_RID} 移出排除名单" \
    && pass "exclusion lifted for $TARGET_TRANSPORT (rid $TARGET_RID)" \
    || fail "no '移出排除名单' log line found for rid $TARGET_RID"

echo "[test_session_exclude] Waiting up to 15s for the session to re-establish..."
for _ in $(seq 1 5); do
    if vty -c "show midr neighbors" | grep -q "$TARGET_TRANSPORT"; then
        pass "$TARGET_TRANSPORT reconnected after explicit override"
        echo ""
        echo "[test_session_exclude] All checks passed."
        exit 0
    fi
    sleep 3
done

fail "$TARGET_TRANSPORT never reappeared in 'show midr neighbors' after explicit override"
