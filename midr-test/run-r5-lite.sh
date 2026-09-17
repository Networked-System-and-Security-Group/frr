#!/usr/bin/env bash
# R5-lite: the two integration checks that must not block standalone MIDR
# extraction.  This runner intentionally does not build an image.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd)
RUN_ROOT=${MIDR_R5_LITE_RUN_ROOT:-$(dirname "$ROOT_DIR")/midr-gate-runs/r5-lite}
SCOPE_TEST=${MIDR_SCOPE_TEST:-$ROOT_DIR/tests/bgpd/test_midr_scope}
M5_RUN_ROOT=${MIDR_M5_RUN_ROOT:-$RUN_ROOT/m5}

mkdir -p "$RUN_ROOT"

if [[ ! -x "$SCOPE_TEST" ]]; then
	echo "ERROR: test_midr_scope is not executable: $SCOPE_TEST" >&2
	exit 2
fi

echo "[R5-lite] pure lifetime refresh propagation"
"$SCOPE_TEST" 2>&1 | tee "$RUN_ROOT/pure-refresh.log"
grep -q 'MIDR scope tests passed' "$RUN_ROOT/pure-refresh.log"

echo "[R5-lite] multi-node service/link withdrawal propagation"
MIDR_M5_RUN_DIR="$M5_RUN_ROOT" \
	"$SCRIPT_DIR/run-m5-multinode.sh" withdraw 2>&1 \
	| tee "$RUN_ROOT/withdraw.log"
grep -q '^PASS: MIDR M5 rootless withdraw$' "$RUN_ROOT/withdraw.log"

cat >"$RUN_ROOT/summary.txt" <<EOF
R5-lite result: PASS
refresh: test_midr_scope (7cd2dd979e regression)
withdraw: run-m5-multinode.sh withdraw
EOF
cat "$RUN_ROOT/summary.txt"
