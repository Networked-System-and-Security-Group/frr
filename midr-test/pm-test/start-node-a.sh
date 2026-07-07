#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BGPD="$REPO_ROOT/bgpd/.libs/bgpd"

# bgpd-a.conf uses a log path relative to this dir ("bgpd-a.log").
cd "$SCRIPT_DIR"

# Rattan already set up the veth (10.1.1.1). Dump net info for debugging.
echo "[node-a] === network state ==="
ip addr show
ip route show
echo "[node-a] RATTAN_BASE=$RATTAN_BASE"

echo "[node-a] starting bgpd..."
"$BGPD" \
  -f "$SCRIPT_DIR/bgpd-a.conf" \
  -Z \
  -S \
  -i /tmp/bgpd-a.pid \
  --vty_socket /tmp \
  --log-level debug &
BGPD_PID=$!

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
