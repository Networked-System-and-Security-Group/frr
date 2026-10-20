#!/usr/bin/env bash
set -euo pipefail
for ((attempt = 0; attempt < 30; attempt++)); do
    [ -S /run/midr/zserv.api ] && exit 0
    sleep 1
done
echo 'matching Zebra ZAPI socket was not created' >&2
exit 1
