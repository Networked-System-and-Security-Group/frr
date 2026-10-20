#!/usr/bin/env bash
set -euo pipefail
: "${MIDR_RELEASE_DIR:?set MIDR_RELEASE_DIR}"
: "${MIDR_NODE_NAME:?set MIDR_NODE_NAME}"
[ -x "$MIDR_RELEASE_DIR/zebra" ] || { echo 'missing matching Zebra' >&2; exit 1; }
if pgrep -x zebra >/dev/null; then
    echo 'a Zebra is already running; complete the service migration first' >&2
    exit 1
fi
install -d -m 755 /run/midr /var/log/midr
export LD_LIBRARY_PATH="$MIDR_RELEASE_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$MIDR_RELEASE_DIR/zebra" -u root -g root \
    -f /etc/midr/zebra.conf -i /run/midr/zebra.pid \
    -z /run/midr/zserv.api --vty_socket /run/midr --log stdout \
    >>"/var/log/midr/$MIDR_NODE_NAME-zebra.log" 2>&1
