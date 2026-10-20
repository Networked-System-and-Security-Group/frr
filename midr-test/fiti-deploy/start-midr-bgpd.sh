#!/usr/bin/env bash
set -euo pipefail
: "${MIDR_RELEASE_DIR:?set MIDR_RELEASE_DIR}"
: "${MIDR_NODE_NAME:?set MIDR_NODE_NAME}"
[ "${MIDR_EDGE_BGP:-no}" = yes ] || { echo 'BGP is disabled for this internal MIDR' >&2; exit 1; }
[ -S /run/midr/zserv.api ] || { echo 'Zebra is not ready' >&2; exit 1; }
if pgrep -x bgpd >/dev/null; then
    echo 'a bgpd is already running; complete the service migration first' >&2
    exit 1
fi
export LD_LIBRARY_PATH="$MIDR_RELEASE_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$MIDR_RELEASE_DIR/bgpd" -u root -g root -S \
    -f /etc/midr/bgpd.conf -i /run/midr/bgpd.pid \
    -z /run/midr/zserv.api --vty_socket /run/midr \
    -A 127.0.0.1 --log stdout \
    >>"/var/log/midr/$MIDR_NODE_NAME-bgpd.log" 2>&1
