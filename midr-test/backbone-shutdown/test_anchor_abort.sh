#!/bin/bash
# test_anchor_abort.sh —— 判据 8：join 中止后，锚点热身定时器不再迟到触发（骨干台子版）。
#
# 配套一（anchor 残留清理 helper）的反证实验。窗口很窄：
#   join 中 RECOMMEND 之后，次优群成员表到达 → 武装 t_anchor_probe_done（60s 热身）
#     日志："MIDR 锚点：收到次优群 N 成员列表，ANCHOR_PROBE_DONE 将在 60 秒后触发"
#   60s 后到点 → "MIDR 锚点：ANCHOR_PROBE_DONE 定时器触发，通知 CL"
# 在这两点之间中止 join：旧码不 cancel 定时器 → 到点照样拿**过期备选群**通知 CL；
# 新码在中止路径上清上下文 + cancel，定时器不该再响。
#
# 中止手段选 `no midr bootstrap`（清空候选）而不是 `midr shutdown`：shutdown 另有
# notify_cl 静默守卫，会把"定时器响了但 CL 没收到"也算成通过，测不出锚点清理本身。
#
# 被测：z1（无配置群号、靠引导 join，join 时有 2 个锚点候选群）。
# 用法：sudo ./test_anchor_abort.sh
set -u

Z1=clab-midr-backbone-z1
ASN=65191
LOG=/etc/frr/logs/frr.log

PASS=0; FAIL=0
pass() { echo "  ✓ $1"; PASS=$((PASS+1)); }
fail() { echo "  ✗ $1"; FAIL=$((FAIL+1)); }

v()        { docker exec $Z1 vtysh -c "$1" 2>/dev/null; }
loglines() { docker exec $Z1 sh -c "wc -l < $LOG" 2>/dev/null | tr -d ' '; }
logtail()  { docker exec $Z1 sh -c "tail -n +$1 $LOG" 2>/dev/null; }

echo "=============================================================="
echo " 判据 8：join 中止后锚点定时器不迟到（配套一反证 / 骨干台子）"
echo "=============================================================="

# ---- 重启 z1 的 FRR，拿一轮全新的 join --------------------------------------
echo "[准备] 重启 z1 的 FRR（重新走一轮 join，才有锚点热身窗口）"
docker exec -u root $Z1 /usr/lib/frr/frrinit.sh restart >/dev/null 2>&1
sleep 8
# 重启后 debug 频道要重开（不进 conf）
docker exec $Z1 vtysh -c "configure terminal" -c "log file $LOG debugging" \
    -c "debug bgp midr" -c "debug bgp midr discovery" >/dev/null 2>&1
MARK0=$(loglines)

# ---- 等"锚点定时器武装"那一行 -----------------------------------------------
armed=0
for i in $(seq 1 40); do   # 最多 200s
    if logtail "$MARK0" | grep -q "ANCHOR_PROBE_DONE 将在"; then
        armed=1
        echo "  t≈$((i*5))s 锚点定时器已武装："
        logtail "$MARK0" | grep -m1 "ANCHOR_PROBE_DONE 将在" | sed 's/.*MIDR/    MIDR/'
        break
    fi
    sleep 5
done

if [[ "$armed" -eq 0 ]]; then
    echo "  ⚠ 200s 内没等到锚点定时器武装 —— 判据 8 **拿不到正样本**，不算通过。"
    echo "    （沉默 ≠ 通过：没武装过的定时器，'没触发'什么也证明不了）"
    echo "    z1 当前 join 状态："
    v "show midr join" | sed 's/^/      /' | head -12
    exit 3
fi

# ---- 中止 join ---------------------------------------------------------------
MARK=$(loglines)
echo ""
echo "[中止] no midr bootstrap（清空候选 = 中止加入）"
docker exec $Z1 vtysh -c "configure terminal" -c "router bgp $ASN" \
    -c "no midr bootstrap" >/dev/null 2>&1

logtail "$MARK" | grep -q "MIDR 锚点：清理评估上下文" \
    && pass "中止时清了锚点评估上下文（helper 有正样本）" \
    || fail "没看到锚点清理日志（配套一没挂上，或当时无残留可清）"

# ---- 等过定时器本该到点的时刻 ------------------------------------------------
echo "  等 90s（覆盖 60s 热身窗）看定时器还响不响..."
sleep 90

if logtail "$MARK" | grep -q "ANCHOR_PROBE_DONE 定时器触发"; then
    fail "定时器仍到点触发 —— 中止后拿过期备选群通知了 CL（旧码行为）"
else
    pass "定时器没有再触发（迟到的 ANCHOR_PROBE_DONE 被掐掉）"
fi

if logtail "$MARK" | grep -q "MIDR CL: ANCHOR_PROBE_DONE"; then
    fail "CL 仍收到了 ANCHOR_PROBE_DONE"
else
    pass "CL 全程没收到 ANCHOR_PROBE_DONE"
fi

# ---- 收尾：把候选清单恢复回来（重启重灌 frr.conf）---------------------------
echo ""
echo "[收尾] 重启 z1 恢复 frr.conf 里的引导候选"
docker exec -u root $Z1 /usr/lib/frr/frrinit.sh restart >/dev/null 2>&1

echo ""
echo " 小结：通过 $PASS 条，失败 $FAIL 条"
[[ "$FAIL" -eq 0 ]]
