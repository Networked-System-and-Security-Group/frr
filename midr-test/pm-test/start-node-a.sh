#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BGPD="$REPO_ROOT/bgpd/.libs/bgpd"

# bgpd-a.conf uses a log path relative to this dir ("bgpd-a.log").
cd "$SCRIPT_DIR"

# transport 专用 loopback + 到对端 transport 的路由（走 rattan 链路）。
# 与静态邻居地址分开，否则 overlay 撞⑦归属守卫建不起来，两端互不认识。
ip addr add 10.0.0.1/32 dev lo 2>/dev/null || true
ip link set lo up
ip route replace 10.0.0.2/32 via 10.2.1.1

# Rattan already set up the veth (10.1.1.1). Dump net info for debugging.
echo "[node-a] === network state ==="
ip addr show
ip route show
echo "[node-a] RATTAN_BASE=$RATTAN_BASE"

mkdir -p /tmp/midr-pm-vty-a
echo "[node-a] starting bgpd..."
"$BGPD" \
  -f "$SCRIPT_DIR/bgpd-a.conf" \
  -Z \
  -S \
  -i /tmp/bgpd-a.pid \
  --vty_socket /tmp/midr-pm-vty-a \
  --log-level debug &
BGPD_PID=$!

# 等远端视图回调注册好再建 overlay，否则对端身份到达时无人接收、被丢弃且不重放。
# 裸 bgpd 不触发 bgp_config_end，注册靠 periodic_sync 30s 兜底，故这里要等。
VTYSH="$REPO_ROOT/vtysh/.libs/vtysh"
for i in $(seq 1 30); do
    grep -q "已向第二组注册 node/link 回调" "$SCRIPT_DIR/bgpd-a.log" 2>/dev/null && break
    sleep 2
done
echo "[node-a] remote-view callback ready, bringing up MIDR overlay to 10.0.0.2..."
"$VTYSH" --vty_socket /tmp/midr-pm-vty-a -c 'configure terminal' \
         -c 'router bgp 65001' -c 'midr session 10.0.0.2 remote-as 65002' || true

if [ "${IPERF_ENABLE:-0}" = "1" ]; then
    START=${IPERF_START_TIME:-5}
    DURATION=${IPERF_DURATION:-10}
    BW=${IPERF_BANDWIDTH:-10M}
    echo "[node-a] iperf3 UDP client will start in ${START}s and run for ${DURATION}s at ${BW}bps..."
    sleep "$START"
    echo "[node-a] starting iperf3 UDP client -> 10.2.1.1:5201"
    iperf3 -c 10.2.1.1 -p 5201 -u -b "$BW" -t "$DURATION" || true
    echo "[node-a] iperf3 UDP client done"
fi

wait $BGPD_PID
