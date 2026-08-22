#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BGPD="$REPO_ROOT/bgpd/.libs/bgpd"

# bgpd-b.conf uses a log path relative to this dir ("bgpd-b.log").
cd "$SCRIPT_DIR"

echo "[node-b] === network state ==="
ip addr show
ip route show
echo "[node-b] RATTAN_BASE=$RATTAN_BASE"

if [ "${IPERF_ENABLE:-0}" = "1" ]; then
    echo "[node-b] starting iperf3 server on port 5201..."
    iperf3 -s -p 5201 &
    IPERF_PID=$!
fi

echo "[node-b] starting bgpd..."
"$BGPD" \
  -f "$SCRIPT_DIR/bgpd-b.conf" \
  -Z \
  -S \
  -i /tmp/bgpd-b.pid \
  --vty_socket /tmp \
  --log-level debug &
BGPD_PID=$!

wait $BGPD_PID ${IPERF_PID:-}
