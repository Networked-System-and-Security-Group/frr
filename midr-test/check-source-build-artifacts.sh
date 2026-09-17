#!/usr/bin/env bash
# Reject compiled artifacts left in the source worktree. Configuration and
# compilation must happen in an independent directory or disposable container.
set -uo pipefail

ROOT=${1:-}
[ -n "$ROOT" ] || ROOT=$(cd -- "$(dirname -- "$0")/.." && pwd)
[ -e "$ROOT/.git" ] || {
	printf 'ERROR: not a Git worktree: %s\n' "$ROOT" >&2
	exit 2
}

TMP_FILE=$(mktemp)
trap 'rm -f "$TMP_FILE"' EXIT

find "$ROOT" -path "$ROOT/.git" -prune -o \
	-type f \( -name '*.o' -o -name '*.lo' -o -name '*.la' \
		-o -name '*.xref' -o -name '.dirstamp' \) -print \
	>>"$TMP_FILE"
find "$ROOT" -path "$ROOT/.git" -prune -o \
	-type d \( -name '.deps' -o -name '.libs' \) -print \
	>>"$TMP_FILE"
for executable in "$ROOT/bgpd/bgpd" "$ROOT/zebra/zebra"; do
	if [ -f "$executable" ]; then
		printf '%s\n' "$executable" >>"$TMP_FILE"
	fi
done
find "$ROOT/tests/bgpd" -maxdepth 1 -type f -name 'test_midr_*' \
	-perm /111 -print >>"$TMP_FILE"

sort -u -o "$TMP_FILE" "$TMP_FILE"
if [ -s "$TMP_FILE" ]; then
	printf 'ERROR: in-source build artifacts found under %s:\n' "$ROOT" >&2
	sed "s|^$ROOT/|  |" "$TMP_FILE" >&2
	exit 1
fi

printf 'SOURCE_ARTIFACT_SCAN=clean root=%s\n' "$ROOT"
