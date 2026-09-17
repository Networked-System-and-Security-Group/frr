#!/usr/bin/env bash
# MIDR scale evidence gate: run test_midr_scale across an objects x peers
# matrix in an isolated container build of the current worktree, storing
# one evidence directory per matrix point.  Not part of the component
# gate (too slow for per-commit regression); invoked explicitly for
# FOLLOW-05 capacity evidence.
set -uo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
BASELINE_IMAGE=frr-midr-p6:f9beedd0d653
TOOLCHAIN_IMAGE=${MIDR_GATE_TOOLCHAIN_IMAGE:-$BASELINE_IMAGE}
RUN_ID=${MIDR_GATE_RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}
OUT_ROOT=${MIDR_GATE_OUT:-$(dirname "$REPO_ROOT")/midr-gate-runs/$RUN_ID-scale}
# objects x peers matrix; keep total upserts bounded for gate runtime
MATRIX=${MIDR_SCALE_MATRIX:-"100 2; 100 8; 1000 2; 1000 8; 5000 4"}
CYCLES=${MIDR_SCALE_CYCLES:-2}
TARGET=${MIDR_SCALE_TARGET:-0}

die() { printf 'ERROR: %s\n' "$*" >&2; exit 2; }

command -v docker >/dev/null 2>&1 || die "docker is unavailable"
docker info >/dev/null 2>&1 || die "Docker daemon is unavailable"

case "$CYCLES" in
	*[!0-9]*|"") die "MIDR_SCALE_CYCLES must be a positive integer" ;;
esac
[ "$CYCLES" -gt 0 ] || die "MIDR_SCALE_CYCLES must be positive"

mkdir -p "$OUT_ROOT" || die "cannot create output directory"

{
	printf '# MIDR scale evidence gate\n'
	printf '# repository=%s\n' "$REPO_ROOT"
	printf '# revision=%s\n' "$(git -C "$REPO_ROOT" rev-parse --short=12 HEAD)"
	printf '# matrix=%s cycles=%s\n' "$MATRIX" "$CYCLES"
	printf '# run-at=%s\n' "$(date --iso-8601=seconds)"
	git -C "$REPO_ROOT" status --porcelain
} >"$OUT_ROOT/scale-gate.log"

IFS=';' read -r -a MATRIX_LIST <<< "$MATRIX"
points=0
for raw_entry in "${MATRIX_LIST[@]}"; do
	entry=$(printf '%s' "$raw_entry" | awk '{$1=$1; print}')
	objects=$(printf '%s\n' "$entry" | awk '{print $1}')
	peers=$(printf '%s\n' "$entry" | awk '{print $2}')
	fields=$(printf '%s\n' "$entry" | awk '{print NF}')
	[ "$fields" -eq 2 ] || die "matrix point must be 'objects peers': $entry"
	[ -n "$objects" ] && [ -n "$peers" ] || die "invalid empty matrix point: $raw_entry"
	case "$objects:$peers" in
		*[!0-9:]*|*:*:*) die "invalid matrix point: $entry" ;;
	esac
	[ "$objects" -gt 0 ] && [ "$peers" -gt 0 ] || die "matrix values must be positive: $entry"
	points=$((points + 1))
	tag="obj${objects}-peer${peers}"
	OUT="$OUT_ROOT/$tag"
	mkdir -p "$OUT"

	printf '=== %s ===\n' "$tag" | tee -a "$OUT_ROOT/scale-gate.log"
	docker run --rm \
		--user 0:0 \
		-v "$REPO_ROOT:/src:ro" \
		-w /src \
		"$TOOLCHAIN_IMAGE" \
		bash -c "
			set -e
			cp -a /src /build
			cd /build
			./configure --enable-vtysh --enable-multipath=256 \
				--enable-user=frr --enable-group=frr \
				--enable-vty-group=frrvty >/dev/null 2>&1
			make -j\$(nproc) tests/bgpd/test_midr_scale >/dev/null 2>&1
			MIDR_SCALE_OBJECTS=$objects \\
			MIDR_SCALE_PEERS=$peers \\
			MIDR_SCALE_CYCLES=$CYCLES \\
			timeout 1200 ./tests/bgpd/test_midr_scale
		" 2>&1 | tee "$OUT/scale.log"
	rc=${PIPESTATUS[0]}
	printf 'RESULT\t%s\t%d\n' "$tag" "$rc" | tee -a "$OUT_ROOT/scale-gate.log"
	[ "$rc" -eq 0 ] || die "scale point $tag failed"
	rows=$(awk -F '\t' '$1 == "SCALE" && $2 != "phase" {count++} END {print count + 0}' "$OUT/scale.log")
	expected_rows=$((3 * CYCLES))
	[ "$rows" -eq "$expected_rows" ] ||
		die "scale point $tag emitted $rows data rows, expected $expected_rows"
done

if [ "$TARGET" = 1 ]; then
	objects=65536
	peers=1
	points=$((points + 1))
	tag="obj${objects}-peer${peers}"
	OUT="$OUT_ROOT/$tag"
	mkdir -p "$OUT"
	printf '=== %s (target limit) ===\n' "$tag" | tee -a "$OUT_ROOT/scale-gate.log"
	docker run --rm \
		--user 0:0 \
		-v "$REPO_ROOT:/src:ro" \
		-w /src \
		"$TOOLCHAIN_IMAGE" \
		bash -c "
			set -e
			cp -a /src /build
			cd /build
			./configure --enable-vtysh --enable-multipath=256 \
				--enable-user=frr --enable-group=frr \
				--enable-vty-group=frrvty >/dev/null 2>&1
			make -j\$(nproc) tests/bgpd/test_midr_scale >/dev/null 2>&1
			MIDR_SCALE_OBJECTS=$objects \
			MIDR_SCALE_PEERS=$peers \
			MIDR_SCALE_CYCLES=1 \
			timeout 1800 ./tests/bgpd/test_midr_scale
		" 2>&1 | tee "$OUT/scale.log"
	rc=${PIPESTATUS[0]}
	printf 'RESULT\t%s\t%d\n' "$tag" "$rc" | tee -a "$OUT_ROOT/scale-gate.log"
	[ "$rc" -eq 0 ] || die "target scale point failed"
	rows=$(awk -F '\t' '$1 == "SCALE" && $2 != "phase" {count++} END {print count + 0}' "$OUT/scale.log")
	[ "$rows" -eq 3 ] || die "target scale point emitted $rows data rows, expected 3"
fi

[ "$points" -gt 0 ] || die "scale matrix executed zero points"

printf 'SCALE_GATE_RESULT=0\n' | tee -a "$OUT_ROOT/scale-gate.log"
printf 'Evidence: %s\n' "$OUT_ROOT"
