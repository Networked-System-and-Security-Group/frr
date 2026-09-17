#!/usr/bin/env bash
# Execute the registered MIDR component programs and TED fixtures in an
# already-built source tree. This script is called by the outer gate runner.
set -uo pipefail

ROOT=${1:-}
MANIFEST=${2:-}
PROGRAM_TIMEOUT=${MIDR_COMPONENT_TIMEOUT_SECONDS:-180}
FIXTURE_TIMEOUT=${MIDR_FIXTURE_TIMEOUT_SECONDS:-300}

die()
{
	printf 'ERROR: %s\n' "$*" >&2
	exit 2
}

[ -n "$ROOT" ] || die "source root is required"
[ -n "$MANIFEST" ] || MANIFEST="$ROOT/midr-test/midr-component-tests.txt"
[ -d "$ROOT/tests/bgpd" ] || die "missing tests/bgpd under $ROOT"
[ -f "$MANIFEST" ] || die "missing component manifest: $MANIFEST"
[ -f "$ROOT/tests/bgpd/subdir.am" ] || die "missing tests/bgpd/subdir.am"

TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT
MANIFEST_CLEAN="$TMP_DIR/manifest"
REGISTERED="$TMP_DIR/registered"
MANIFEST_SORTED="$TMP_DIR/manifest-sorted"

sed -e '/^[[:space:]]*#/d' -e '/^[[:space:]]*$/d' \
	"$MANIFEST" >"$MANIFEST_CLEAN"
sed -n 's|^check_PROGRAMS += tests/bgpd/\(test_midr_[A-Za-z0-9_]*\)$|\1|p' \
	"$ROOT/tests/bgpd/subdir.am" \
	| grep -v -E '^(test_midr_ted_fixture|test_midr_zebra_e2e)$' \
	| sort -u >"$REGISTERED"
sed -n 's|^check_PROGRAMS += tests/bgpd/\(test_capability\)$|\1|p' \
	"$ROOT/tests/bgpd/subdir.am" >>"$REGISTERED"
sort -u "$REGISTERED" -o "$REGISTERED"
sort -u "$MANIFEST_CLEAN" >"$MANIFEST_SORTED"

if ! diff -u "$REGISTERED" "$MANIFEST_SORTED"; then
	die "component manifest does not match registered non-privileged MIDR tests"
fi

cd "$ROOT" || die "cannot enter source root"
program_passed=0
program_failed=0
overall=0

while IFS= read -r test_name; do
	test_path="./tests/bgpd/$test_name"
	out="$TMP_DIR/$test_name.log"
	if [ ! -x "$test_path" ]; then
		rc=127
		printf 'missing executable: %s\n' "$test_path" >"$out"
	else
		timeout "$PROGRAM_TIMEOUT" "$test_path" >"$out" 2>&1
		rc=$?
	fi
	last_line=$(tail -n 1 "$out" 2>/dev/null \
		| tr '\t\r\n' '   ' | cut -c1-300)
	printf 'RESULT\t%s\t%d\t%s\n' "$test_name" "$rc" "$last_line"
	if [ "$rc" -eq 0 ]; then
		program_passed=$((program_passed + 1))
	else
		program_failed=$((program_failed + 1))
		overall=1
		sed 's/^/  | /' "$out"
	fi
done <"$MANIFEST_CLEAN"

fixture_out="$TMP_DIR/ted-fixtures.log"
timeout "$FIXTURE_TIMEOUT" python3 midr-test/run-ted-fixtures.py \
	>"$fixture_out" 2>&1
fixture_rc=$?
sed -n '1,240p' "$fixture_out"
printf 'RESULT\t%s\t%d\t%s\n' "test_midr_ted_fixtures(py)" \
	"$fixture_rc" "run-ted-fixtures.py"
if [ "$fixture_rc" -ne 0 ]; then
	overall=1
fi

printf 'COMPONENT_SUMMARY programs_passed=%d programs_failed=%d fixtures=%s\n' \
	"$program_passed" "$program_failed" \
	"$([ "$fixture_rc" -eq 0 ] && printf passed || printf failed)"
printf 'COMPONENT_RESULT=%d\n' "$overall"
exit "$overall"
