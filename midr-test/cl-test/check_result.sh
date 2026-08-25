#!/bin/bash
# check_result.sh — Parse bgpd logs and print a summary of the CL test outcome.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"   # 同 run_test.sh，锚点判据要用仓库 vtysh
LOGDIR="$SCRIPT_DIR/logs"

echo "=== MIDR CL Test Result Summary ==="
echo ""

newnode_log="$LOGDIR/bgpd-newnode.log"

if [[ ! -f "$newnode_log" ]]; then
    echo "  ERROR: $newnode_log not found."
    exit 1
fi

# Key log lines to extract
echo "--- REP probe phase ---"
grep -E "REP_LIST_REQ|REP_LIST_RESP|I-1.*群代表|REP_PROBE_DONE|RECOMMEND" "$newnode_log" 2>/dev/null | tail -10 || true

echo ""
echo "--- MEMBER probe phase ---"
grep -E "MEMBER_LIST_REQ|MEMBER_LIST_RESP|I-1.*成员|MEMBER_PROBE_DONE" "$newnode_log" 2>/dev/null | tail -10 || true

echo ""
echo "--- CL decisions ---"
grep -E "MIDR CL:|JOIN|CREATE|RECOMMEND" "$newnode_log" 2>/dev/null | tail -15 || true

echo ""
echo "--- Anchor-connection phase (group-3 and group-2 runner-up groups) ---"
grep -E "锚点|ANCHOR" "$newnode_log" 2>/dev/null | tail -20 || true

echo ""
echo "--- PM I-5 metric samples (last 5 per node) ---"
for node in g1a g2a g1b g1c g1d g1e; do
    echo "  Node $node:"
    grep "MIDR PM I-5" "$newnode_log" 2>/dev/null | grep -oP "(10\.10\.[0-9]+\.2)" | sort -u | while read -r ip; do
        grep "MIDR PM I-5" "$newnode_log" | grep "$ip" | tail -3 | sed 's/^/    /'
    done || true
done
grep "MIDR PM I-5" "$newnode_log" 2>/dev/null | tail -10 | sed 's/^/  /' || true

echo ""
echo "--- Final verdict ---"
if grep -qE "MIDR CL: MEMBER_PROBE_DONE → JOIN 群|JOIN group" "$newnode_log" 2>/dev/null; then
    group=$(grep -oP "JOIN 群 \K[0-9]+" "$newnode_log" 2>/dev/null | tail -1 || echo "?")
    echo "  PASS: newnode JOIN group $group  ✓"
elif grep -qE "MIDR CL:.*CREATE 新群" "$newnode_log" 2>/dev/null; then
    group=$(grep -oP "CREATE 新群 \K[0-9]+" "$newnode_log" 2>/dev/null | tail -1 || echo "?")
    echo "  INFO: newnode CREATE new group $group (EWMA 未收敛，或好链路数不到阈值——阈值自 JOIN 封顶起是 min(5, 已知成员数)，不再是固定 5)"
else
    echo "  PENDING: no CL decision yet (increase --timeout or check logs)"
fi

# 锚点建连的判据：查**真的 Established**，不查"发起了几条"。
# 原判据抓的是日志里的「尝试建连 N」（= attempted）却打印 "established"，
# 于是跨群闸门把 PEER_REQUEST 全拒掉、一条都没建成时照样报 PASS
# （保底轮 2 核对档问题③点名要修，批 6 落地）。
anchor_line=$(grep "MIDR I-7：ANCHOR " "$newnode_log" 2>/dev/null | tail -1 || true)
if [[ -z "$anchor_line" ]]; then
    echo "  PENDING: no ANCHOR decision yet (increase --timeout or check logs)"
else
    attempted=$(echo "$anchor_line" | grep -oP "尝试建连 \K[0-9]+" || echo "?")
    # 用仓库自己的 vtysh（裸 vtysh 会命中系统 apt 装的官方 FRR，没有 show midr）
    VTYSH="$REPO_ROOT/vtysh/.libs/vtysh"
    VTY_DIR="/tmp/midr-cl-vty/newnode"
    established="?"
    if [[ -x "$VTYSH" && -d "$VTY_DIR" ]]; then
        # 列序：Neighbor ASN State LS Group-ID Origin Capabilities —— Origin 是第 6 列。
        # 原先注释漏了 LS 列、按 $5 取，恒不匹配 → 锚点全 Established 也报 0
        # （2026-08-25 实撞：4 条 CL_ANCHOR 全 Established，判据仍 FAIL）。
        # 不按列号数，直接认字段，免得再随列变动失效。
        established=$("$VTYSH" --vty_socket "$VTY_DIR" \
            -c "show midr neighbors" 2>/dev/null \
            | awk '/Established/ && /CL_ANCHOR/' | wc -l)
    fi

    if [[ "$established" == "?" ]]; then
        # bgpd 已退出或 vtysh 不可用：只能报"发起了多少"，且**不许说 established**
        echo "  INFO: ANCHOR decision seen (attempted $attempted); could not verify"
        echo "        established sessions — bgpd/vtysh not reachable at $VTY_DIR"
    elif [[ "$established" -gt 0 && "$established" == "$attempted" ]]; then
        echo "  PASS: $established/$attempted anchor sessions Established (verified via vtysh)  ✓"
    elif [[ "$established" -gt 0 ]]; then
        echo "  INFO: only $established/$attempted anchor sessions Established — check logs"
    else
        echo "  FAIL: ANCHOR attempted $attempted but 0 sessions Established"
        echo "        (跨群闸门没放行？看对端日志有无 ignoring/拒绝 PEER_REQUEST)"
    fi
fi

echo ""
echo "Logs are in: $LOGDIR/"
