#!/usr/bin/env bash
set -euo pipefail

: "${MIDR_RELEASE_DIR:?set MIDR_RELEASE_DIR in /etc/midr/node.env}"
MIDR_CONFIG=${MIDR_CONFIG:-/etc/midr/node.commands}
MIDR_VTY_DIR=${MIDR_VTY_DIR:-/run/midr}

[ -s "$MIDR_CONFIG" ] || { echo "missing configuration: $MIDR_CONFIG" >&2; exit 1; }
[ -r "$MIDR_RELEASE_DIR/midrd-vty.py" ] || {
    echo 'release does not contain midrd-vty.py' >&2
    exit 1
}
for ((attempt = 0; attempt < 30; attempt++)); do
    # A socket left by the previous process is not proof that the new VTY
    # listener is ready. Probe before loading any configuration commands.
    if [ -S "$MIDR_VTY_DIR/midrd.vty" ] && \
       python3 "$MIDR_RELEASE_DIR/midrd-vty.py" \
         --socket "$MIDR_VTY_DIR/midrd.vty" -c 'show midr self' \
         >/dev/null 2>&1; then
        break
    fi
    sleep 1
done
[ "$attempt" -lt 30 ] || {
    echo "midrd VTY socket is not ready: $MIDR_VTY_DIR/midrd.vty" >&2
    exit 1
}
python3 "$MIDR_RELEASE_DIR/midrd-vty.py" \
    --socket "$MIDR_VTY_DIR/midrd.vty" \
    --commands-file "$MIDR_CONFIG"
python3 "$MIDR_RELEASE_DIR/midrd-vty.py" \
    --socket "$MIDR_VTY_DIR/midrd.vty" \
    -c 'show midr self'
