#!/usr/bin/env bash
set -euo pipefail

: "${MIDR_RELEASE_DIR:?set MIDR_RELEASE_DIR}"
: "${MIDR_NODE_NAME:?set MIDR_NODE_NAME}"
: "${MIDR_NODE_ID:?set MIDR_NODE_ID}"
: "${MIDR_EDGE_BGP:?set MIDR_EDGE_BGP}"
[ "$MIDR_EDGE_BGP" = yes ] || { echo 'Prefix Provider is disabled for this internal MIDR' >&2; exit 1; }
[ -x "$MIDR_RELEASE_DIR/vtysh" ] || { echo 'missing matching vtysh' >&2; exit 1; }
[ -r /usr/local/libexec/midr/midr-bgp-prefix-provider.py ] || {
    echo 'missing Prefix Provider script' >&2
    exit 1
}
install -d -m 755 /run/midr /var/log/midr
export LD_LIBRARY_PATH="$MIDR_RELEASE_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
provider_args=(
    /usr/bin/python3 /usr/local/libexec/midr/midr-bgp-prefix-provider.py
    --vtysh "$MIDR_RELEASE_DIR/vtysh"
    --vty-socket "${MIDR_VTY_DIR:-/run/midr}"
    --prefix-socket "${MIDR_PREFIX_SOCKET:-/run/midr/prefix.sock}"
    --originator "$MIDR_NODE_ID"
    --interval "${MIDR_PREFIX_INTERVAL:-5}"
)
if [ -n "${MIDR_PREFIX_EXCLUDE_FILE:-}" ]; then
    provider_args+=(--exclude-prefix-file "$MIDR_PREFIX_EXCLUDE_FILE")
fi
exec "${provider_args[@]}" \
    >>"/var/log/midr/$MIDR_NODE_NAME-prefix-provider.log" 2>&1
