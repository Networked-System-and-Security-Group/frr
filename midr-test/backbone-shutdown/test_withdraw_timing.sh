#!/bin/bash
# test_withdraw_timing.sh —— 退网时那一发 LEAVE（Node NLRI 撤销）到底发没发出去？
#
# 起因（2026-08-21 用户提问）：退网顺序是"① 置位 → ② 发撤销 → ③ 拆会话"，撤销排在
# 拆会话之前。但 BGP 撤销是**异步**的——`bgp_ls_withdraw()` 末尾调 `bgp_process()`，
# 只把路由挂进工作队列等重算并生成 UPDATE，真正写到 TCP 要等事件循环下一轮；而步③在
# 同一个函数里紧接着就 `peer_delete`。悬念：那份 UPDATE 是赶在会话被拆前发出去了，
# 还是随会话一起没了？后者的话，"优雅退网"实际退化成"干净地挂断电话"——全网仍即时
# 收敛，但靠的是对端看到会话断、各自作废我方 NLRI 再接力撤销，不是我方主动播报。
#
# 【场景怎么造对】必须有一条**退网拆不掉、且载 BGP-LS** 的会话。
#   ✗ 第一次尝试（已废弃）：拿 m1a 的 transport 地址配"静态"邻居 —— 那地址上本来就有
#     MIDR 会话，同地址即同一个 peer 对象，等于改了既有 MIDR peer，照样被拆，测不出东西。
#   ✓ 现在的做法：用 z1 **frr.conf 里那条真实的静态过境邻居 t3**（10.10.31.2），运行时
#     给它加上 link-state 族。这个地址不在 MIDR 台账里、peer 不带 OVERLAY 标记，退网
#     碰不到它；而且这正是这道守卫要防的真实场景（运维自己在静态邻居上开了 LS）。
#     再让 t3 朝 m2a 也激活 LS，t3 就成了 z1 的 LS 上游，有持续的 NLRI 喂进来。
#
# 【判别点】t3 的 LS 库里，z1 的 Node NLRI 只可能是**从 z1 直接学来的**（t3 的 LS 邻居
# 只有 z1 和 m2a，而 m2a 那边的 z1 NLRI 也要经 t3 才到得了 t3……故直接来源唯有 z1）。
# 所以退网后若 t3 的库里 z1 的 NLRI 没了、而 z1↔t3 会话仍 Established，那就只能是
# **显式撤销通过这条幸存会话送达了**。反之则说明那一发 LEAVE 被拆会话吃掉了。
#
# ⚠ 纯测试脚手架：不改代码、不落盘（运行时配，测完拆掉，容器重启即恢复）。
# ⚠ 单独一轮跑，不混进主判据：这条会话激活了 LS，`show midr neighbors` 会列它，
#   混进去会把判据 2 的"MIDR 边已拆净"顶成误报。
#
# 用法：sudo ./test_withdraw_timing.sh
set -u

Z1=clab-midr-backbone-z1;   Z1_ASN=65191;  Z1_RID=10.0.0.191
T3=clab-midr-backbone-t3;   T3_ASN=65203
M2A=clab-midr-backbone-m2a; M2A_ASN=65122
Z1_LINK=10.10.31.1   # z1 的 eth1（t3 眼里的 z1）
T3_TO_Z1=10.10.31.2  # t3 的 eth（z1 眼里的 t3）—— z1 frr.conf 里的静态邻居
M2A_LINK=10.10.25.1  # m2a 的 eth（t3 眼里的 m2a）
T3_TO_M2A=10.10.25.2 # t3 的 eth（m2a 眼里的 t3）
LOG=/etc/frr/logs/frr.log

PASS=0; FAIL=0
pass() { echo "  ✓ $1"; PASS=$((PASS+1)); }
fail() { echo "  ✗ $1"; FAIL=$((FAIL+1)); }

v()        { docker exec "$1" vtysh -c "$2" 2>/dev/null; }
conf()     { docker exec "$1" vtysh -c "configure terminal" -c "router bgp $2" -c "$3" 2>/dev/null; }
loglines() { docker exec "$1" sh -c "wc -l < $LOG" 2>/dev/null | tr -d ' '; }
logtail()  { docker exec "$1" sh -c "tail -n +$2 $LOG" 2>/dev/null; }
sess_up()  { v "$1" "show bgp summary" | grep "^$2 " | grep -qvE "Idle|Active|Connect"; }
# 只数 z1 的 **Node** NLRI（[V] 前缀）。⚠ 别拿"含 rid 的行数"当判据：那会把一大堆
# Link NLRI（[E]，以 z1 为端点的链路，由 z1 与各对端分别 originate）一起数进去，
# 撤销与否被淹没在里面（08-21 首跑实撞：44->31，看着像"没撤"，其实 Node 那条已撤）。
z1_node_in_t3() { v "$T3" "show bgp link-state link-state" | grep "\[V\]" | grep -c "q$Z1_RID"; }

echo "=============================================================="
echo " 撤销时序验证：退网那一发 LEAVE 到底发没发出去"
echo "=============================================================="

docker exec $Z1 vtysh -c "configure terminal" -c "log file $LOG debugging" \
    -c "debug bgp midr" -c "debug bgp link-state" >/dev/null 2>&1
GID=$(v $Z1 "show midr self" | awk -F: '/^Group-ID/{gsub(/ /,"",$2);print $2}')
[[ "$GID" =~ ^[1-9] ]] || { echo "  z1 还没入群（$GID），先等它 join"; exit 2; }
v $Z1 "show midr self" | grep -q "^Shutdown *: *no" || { echo "  z1 处于退网态，先 no midr shutdown"; exit 2; }

# ---- 把静态过境邻居升级成载 LS ----------------------------------------------
echo ""
echo "[准备] 给已有的静态过境邻居加 link-state 族：z1<->t3<->m2a"
docker exec $Z1 vtysh -c "configure terminal" -c "router bgp $Z1_ASN" \
    -c "address-family link-state link-state" -c "neighbor $T3_TO_Z1 activate" >/dev/null 2>&1
docker exec $T3 vtysh -c "configure terminal" -c "router bgp $T3_ASN" \
    -c "address-family link-state link-state" \
    -c "neighbor $Z1_LINK activate" -c "neighbor $M2A_LINK activate" >/dev/null 2>&1
docker exec $M2A vtysh -c "configure terminal" -c "router bgp $M2A_ASN" \
    -c "address-family link-state link-state" -c "neighbor $T3_TO_M2A activate" >/dev/null 2>&1

echo "  等 LS 会话就绪并传播（最多 60s）..."
ok=0
for _ in $(seq 1 12); do
    if sess_up $Z1 "$T3_TO_Z1" && [[ "$(z1_node_in_t3)" -gt 0 ]]; then
        ok=1; break
    fi
    sleep 5
done
[[ "$ok" -eq 1 ]] && pass "z1↔t3 静态 LS 会话已建立，且 t3 已从 z1 学到 z1 的 Node NLRI" \
                  || { fail "场景没造起来（会话或 NLRI 未就绪），实验无从进行"
                       echo "    z1↔t3: $(v $Z1 "show bgp summary" | grep "^$T3_TO_Z1 " || echo 无此行)"
                       echo "    t3 LS 库里 z1 的 Node NLRI: $(z1_node_in_t3) 条"; }

T3_HAS_BEFORE=$(z1_node_in_t3)
echo "  退网前：t3 的 LS 库里 z1 的 Node NLRI $T3_HAS_BEFORE 条"

# ---- 退网 -------------------------------------------------------------------
MARK_Z1=$(loglines $Z1)
echo ""
echo "[执行] z1: midr shutdown（此刻 z1↔t3 这条静态 LS 会话应当幸存）"
conf $Z1 $Z1_ASN "midr shutdown" >/dev/null
sleep 10

# ---- 判别 -------------------------------------------------------------------
echo ""
echo "[结果]"
if sess_up $Z1 "$T3_TO_Z1"; then
    pass "z1↔t3 静态会话在退网后仍 Established（退网确实没碰运维配的会话）"
    SESS_ALIVE=1
else
    fail "z1↔t3 静态会话也断了 —— 无法区分'撤销没发'与'会话没了'，判别失效"
    SESS_ALIVE=0
fi

logtail $Z1 "$MARK_Z1" | grep -q "Withdrew local BGP Node NLRI" \
    && pass "z1 本地确实执行了 Node NLRI 撤销动作" \
    || fail "z1 连本地撤销动作都没执行"

T3_HAS_AFTER=$(z1_node_in_t3)
echo "  退网后：t3 的 LS 库里 z1 的 Node NLRI $T3_HAS_AFTER 条（退网前 $T3_HAS_BEFORE 条）"
if [[ "$SESS_ALIVE" -eq 1 ]]; then
    if [[ "$T3_HAS_AFTER" -eq 0 && "$T3_HAS_BEFORE" -gt 0 ]]; then
        pass "**结论：撤销确实发出去了** —— 会话还活着，而 t3 库里 z1 的 NLRI 已消失，只可能是收到了显式撤销"
    else
        fail "**结论：那一发 LEAVE 没送达** —— 会话活着但 t3 仍持有 z1 的 NLRI，说明撤销被拆会话吃掉了"
    fi
fi

# ---- 顺带：收包守卫的正样本（此刻 z1 仍有 LS 输入源）------------------------
echo ""
echo "[顺带] 判据 4 的收包守卫正样本（此刻 z1 经 t3 仍在收 LS）"
DROP=$(logtail $Z1 "$MARK_Z1" | grep -c "MIDR 退网：丢弃收到的远端 Node NLRI")
[[ "$DROP" -gt 0 ]] && pass "收包守卫有正样本：丢弃 $DROP 份远端 NLRI" \
                    || fail "没抓到收包守卫日志（此刻可能确实没有 NLRI 送达）"
NODES=$(v $Z1 "show midr nodes" | grep -E "^10\." | grep -vc "^$Z1_RID")
[[ "$NODES" -eq 0 ]] && pass "有 LS 输入源的情况下节点表仍为空（守卫真的挡住了回灌）" \
                     || fail "节点表被灌回 $NODES 条"

# ---- 收尾 -------------------------------------------------------------------
echo ""
echo "[收尾] 撤掉 link-state 族激活，恢复 z1"
docker exec $Z1 vtysh -c "configure terminal" -c "router bgp $Z1_ASN" \
    -c "address-family link-state link-state" -c "no neighbor $T3_TO_Z1 activate" >/dev/null 2>&1
docker exec $T3 vtysh -c "configure terminal" -c "router bgp $T3_ASN" \
    -c "address-family link-state link-state" \
    -c "no neighbor $Z1_LINK activate" -c "no neighbor $M2A_LINK activate" >/dev/null 2>&1
docker exec $M2A vtysh -c "configure terminal" -c "router bgp $M2A_ASN" \
    -c "address-family link-state link-state" -c "no neighbor $T3_TO_M2A activate" >/dev/null 2>&1
conf $Z1 $Z1_ASN "no midr shutdown" >/dev/null

echo ""
echo " 小结：通过 $PASS 条，失败 $FAIL 条（本脚本判的是**事实**，失败条目即事实本身）"
