#!/usr/bin/env bash
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BIN=${MIDRD_BIN:-"$ROOT/build/midrd"}
BASE_PORT=${MIDRD_RESTART_BASE_PORT:-44300}
[[ -x "$BIN" ]] || { echo "missing midrd binary: $BIN" >&2; exit 2; }

RUN=$(mktemp -d "${TMPDIR:-/tmp}/midrd-r7-restart.XXXXXX")
OWNER_LOG="$RUN/owner.log"
OWNER_RESTART_LOG="$RUN/owner-restart.log"
PEER_LOG="$RUN/peer.log"
SEQ_FILE="$RUN/owner.seq"
owner_pid= peer_pid= owner_restart_pid=

cleanup() {
	for pid in "$owner_restart_pid" "$owner_pid" "$peer_pid"; do
		if [[ -n "$pid" ]]; then
			kill "$pid" 2>/dev/null || true
		fi
	done
}
trap cleanup EXIT

"$BIN" --node-id 902 --listen "127.0.0.1:$((BASE_PORT + 2))" \
	--peer "127.0.0.1:$((BASE_PORT + 1))" --group 1 \
	--prefix 192.0.2.2/32 --sequence-file "$RUN/peer.seq" \
	--lifetime 3000 --runtime 8 --pidfile "$RUN/peer.pid" \
	>"$PEER_LOG" 2>&1 &
peer_pid=$!
"$BIN" --node-id 901 --listen "127.0.0.1:$((BASE_PORT + 1))" \
	--peer "127.0.0.1:$((BASE_PORT + 2))" --group 1 \
	--prefix 192.0.2.1/32 --sequence-file "$SEQ_FILE" \
	--lifetime 3000 --runtime 20 --pidfile "$RUN/owner.pid" \
	>"$OWNER_LOG" 2>&1 &
owner_pid=$!

sleep 1
kill -KILL "$owner_pid" 2>/dev/null || true
wait "$owner_pid" 2>/dev/null || true
owner_pid=
sleep 0.3
"$BIN" --node-id 901 --listen "127.0.0.1:$((BASE_PORT + 1))" \
	--peer "127.0.0.1:$((BASE_PORT + 2))" --group 1 \
	--prefix 192.0.2.1/32 --sequence-file "$SEQ_FILE" \
	--lifetime 3000 --runtime 4 --pidfile "$RUN/owner.pid" \
	>"$OWNER_RESTART_LOG" 2>&1 &
owner_restart_pid=$!

wait "$owner_restart_pid"
owner_restart_pid=
wait "$peer_pid"
peer_pid=

awk '/node=902 event state=1 / {
	for (i = 1; i <= NF; i++)
		if ($i ~ /^seq=/) {
			split($i, fields, "=")
			if ((fields[2] + 0) >= 2)
				found=1
		}
     }
     END { exit found ? 0 : 1 }' "$PEER_LOG"
[[ -s "$SEQ_FILE" ]]
grep -q 'shutdown=' "$OWNER_RESTART_LOG"
echo "r7 owner restart smoke: PASS (logs: $RUN)"
