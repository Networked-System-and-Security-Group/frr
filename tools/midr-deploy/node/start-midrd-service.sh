#!/usr/bin/env bash
set -euo pipefail

: "${MIDR_RELEASE_DIR:?set MIDR_RELEASE_DIR in /etc/midr/node.env}"
: "${MIDR_NODE_ID:?set MIDR_NODE_ID in /etc/midr/node.env}"
: "${MIDR_NODE_NAME:?set MIDR_NODE_NAME in /etc/midr/node.env}"
: "${MIDR_LOCATOR:?set MIDR_LOCATOR in /etc/midr/node.env}"

MIDR_CONFIG=${MIDR_CONFIG:-/etc/midr/node.commands}
MIDR_VTY_DIR=${MIDR_VTY_DIR:-/run/midr}
MIDR_ZSERV_PATH=${MIDR_ZSERV_PATH:-/run/midr/zserv.api}
MIDR_SEQUENCE_DIR=${MIDR_SEQUENCE_DIR:-/var/lib/midr}
MIDR_LOG_DIR=${MIDR_LOG_DIR:-/var/log/midr}
MIDR_PREFIX_SOCKET=${MIDR_PREFIX_SOCKET:-/run/midr/prefix.sock}
MIDR_PREFIX_MODE=${MIDR_PREFIX_MODE:-none}

case "$MIDR_NODE_ID" in
    ''|*[!0-9]*) echo 'MIDR_NODE_ID must be an integer' >&2; exit 1 ;;
esac
[ "$MIDR_NODE_ID" -gt 0 ] && [ "$MIDR_NODE_ID" -le 4294967295 ] || {
    echo 'MIDR_NODE_ID is out of range' >&2
    exit 1
}
[ -x "$MIDR_RELEASE_DIR/run-midrd.sh" ] && \
[ -x "$MIDR_RELEASE_DIR/midrd" ] && \
[ -x "$MIDR_RELEASE_DIR/zebra" ] && \
[ -r "$MIDR_RELEASE_DIR/midrd-vty.py" ] || {
    echo "release is incomplete: $MIDR_RELEASE_DIR" >&2
    exit 1
}
[ -s "$MIDR_CONFIG" ] || { echo "missing configuration: $MIDR_CONFIG" >&2; exit 1; }
[ -S "$MIDR_ZSERV_PATH" ] || { echo "Zebra socket is not ready: $MIDR_ZSERV_PATH" >&2; exit 1; }

# A host has one midrd and one Zebra managing the default Linux FIB.
if pgrep -x midrd >/dev/null; then
    echo 'midrd is already running; stop the previous process before migration' >&2
    exit 1
fi
mapfile -t zebra_pids < <(pgrep -x zebra || true)
[ "${#zebra_pids[@]}" -eq 1 ] || {
    echo 'expected exactly one running Zebra' >&2
    exit 1
}
running_zebra=$(readlink -f "/proc/${zebra_pids[0]}/exe")
release_zebra=$(readlink -f "$MIDR_RELEASE_DIR/zebra")
[ "$running_zebra" = "$release_zebra" ] || {
    echo "running Zebra does not match the release: $running_zebra" >&2
    exit 1
}

install -d -m 755 "$MIDR_VTY_DIR" "$MIDR_SEQUENCE_DIR" "$MIDR_LOG_DIR"
args=(
    --node-id "$MIDR_NODE_ID"
    --listen "[$MIDR_LOCATOR]:5871"
    --zserv-path "$MIDR_ZSERV_PATH"
    --vty_socket "$MIDR_VTY_DIR"
    --sequence-file "$MIDR_SEQUENCE_DIR/$MIDR_NODE_ID.seq"
    --pidfile "$MIDR_VTY_DIR/midrd-$MIDR_NODE_ID.pid"
    --takeover-delay 1500
    --lifetime 30000
    --hold-time 10000
)
# Only override midrd's built-in default (199) when explicitly configured, so
# this script also starts releases that predate --route-tag.  The Prefix
# Provider reads the same MIDR_ROUTE_TAG.
if [ -n "${MIDR_ROUTE_TAG:-}" ]; then
    args+=(--route-tag "$MIDR_ROUTE_TAG")
fi
case "$MIDR_PREFIX_MODE" in
    none) ;;
    socket)
        args+=(--prefix-socket "$MIDR_PREFIX_SOCKET")
        ;;
    *) echo 'MIDR_PREFIX_MODE must be none or socket' >&2; exit 1 ;;
esac
exec "$MIDR_RELEASE_DIR/run-midrd.sh" \
    "${args[@]}" \
    >>"$MIDR_LOG_DIR/$MIDR_NODE_NAME-midrd.log" 2>&1
