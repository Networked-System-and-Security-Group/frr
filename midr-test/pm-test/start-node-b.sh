#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BGPD="$REPO_ROOT/bgpd/.libs/bgpd"

# bgpd-b.conf uses a log path relative to this dir ("bgpd-b.log").
cd "$SCRIPT_DIR"

# transport 专用 loopback + 到对端 transport 的路由，理由同 start-node-a.sh
ip addr add 10.0.0.2/32 dev lo 2>/dev/null || true
ip link set lo up
ip route replace 10.0.0.1/32 via 10.1.1.1

echo "[node-b] === network state ==="
ip addr show
ip route show
echo "[node-b] RATTAN_BASE=$RATTAN_BASE"

if [ "${IPERF_ENABLE:-0}" = "1" ]; then
    echo "[node-b] starting iperf3 server on port 5201..."
    iperf3 -s -p 5201 &
    IPERF_PID=$!
fi

mkdir -p /tmp/midr-pm-vty-b
echo "[node-b] starting bgpd..."
"$BGPD" \
  -f "$SCRIPT_DIR/bgpd-b.conf" \
  -Z \
  -S \
  -i /tmp/bgpd-b.pid \
  --vty_socket /tmp/midr-pm-vty-b \
  --log-level debug &
BGPD_PID=$!

# 双端对称建 overlay，理由与时机同 start-node-a.sh
VTYSH="$REPO_ROOT/vtysh/.libs/vtysh"
for i in $(seq 1 30); do
    grep -q "已向第二组注册 node/link 回调" "$SCRIPT_DIR/bgpd-b.log" 2>/dev/null && break
    sleep 2
done
echo "[node-b] remote-view callback ready, bringing up MIDR overlay to 10.0.0.1..."
"$VTYSH" --vty_socket /tmp/midr-pm-vty-b -c 'configure terminal' \
         -c 'router bgp 65002' -c 'midr session 10.0.0.1 remote-as 65001' || true

wait $BGPD_PID ${IPERF_PID:-}
