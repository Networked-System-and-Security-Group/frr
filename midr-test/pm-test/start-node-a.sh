#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BGPD="$REPO_ROOT/bgpd/.libs/bgpd"

# bgpd-a.conf uses a log path relative to this directory.
cd "$SCRIPT_DIR"

# Keep the transport loopback separate from the physical Rattan link address.
ip addr add 10.0.0.1/32 dev lo 2>/dev/null || true
ip link set lo up
ip route replace 10.0.0.2/32 via 10.2.1.1

# Rattan already configured the physical veth address (10.1.1.1).
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

# Wait for remote-view callback registration before creating the overlay.
VTYSH="$REPO_ROOT/vtysh/.libs/vtysh"
for i in $(seq 1 30); do
    grep -q "已向第二组注册 node/link 回调" "$SCRIPT_DIR/bgpd-a.log" 2>/dev/null && break
    sleep 2
done
if ! grep -q "已向第二组注册 node/link 回调" "$SCRIPT_DIR/bgpd-a.log" 2>/dev/null; then
    echo "[node-a] ERROR: remote-view callback registration timed out." >&2
    exit 1
fi
echo "[node-a] remote-view callback ready, bringing up MIDR overlay to 10.0.0.2..."
"$VTYSH" --vty_socket /tmp/midr-pm-vty-a -c 'configure terminal' \
         -c 'router bgp 65001' -c 'midr session 10.0.0.2 remote-as 65002'

if [ "${IPERF_ENABLE:-0}" = "1" ]; then
    START=${IPERF_START_TIME:-5}
    DURATION=${IPERF_DURATION:-10}
    BW=${IPERF_BANDWIDTH:-10M}
    echo "[node-a] iperf3 UDP client will start in ${START}s and run for ${DURATION}s at ${BW}bps..."
    sleep "$START"
    printf '%s MIDR PM TEST: iperf START\n' "$(date '+%Y/%m/%d %H:%M:%S.%3N')" >> "$SCRIPT_DIR/bgpd-a.log"
    echo "[node-a] starting iperf3 UDP client -> 10.2.1.1:5201"
    iperf3 -c 10.2.1.1 -p 5201 -u -b "$BW" -t "$DURATION"
    printf '%s MIDR PM TEST: iperf END\n' "$(date '+%Y/%m/%d %H:%M:%S.%3N')" >> "$SCRIPT_DIR/bgpd-a.log"
    echo "[node-a] iperf3 UDP client done"
fi

wait $BGPD_PID
