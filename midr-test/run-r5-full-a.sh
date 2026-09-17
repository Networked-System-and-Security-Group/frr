#!/usr/bin/env bash
# R5-full-A: record the pre-extraction behaviour of the embedded MIDR path.
#
# This runner deliberately does not build an image and does not mutate the
# source tree.  It uses an already-built independent FRR tree for the
# rootless tests and a caller-selected running containerlab for the read-only
# 15-node checks.  Its output is the behavioural baseline for R6-B-A.
set -uo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd)
RUN_ROOT=${MIDR_R5_FULL_A_RUN_ROOT:-$(dirname "$ROOT_DIR")/midr-gate-runs/r5-full-a}
BUILD_ROOT=${MIDR_R5_FULL_A_BUILD_ROOT:-}
LAB_PREFIX=${MIDR_R5_FULL_A_LAB_PREFIX:-clab-midr-backbone-p6-final}
SOAK_SECONDS=${MIDR_R5_FULL_A_SOAK_SECONDS:-0}
STARTED_AT=$(date --iso-8601=seconds)

PASS=0
FAIL=0
SKIP=0
LAB_IMAGE=unknown
LAB_STARTED_AT=unknown

mkdir -p "$RUN_ROOT"
: >"$RUN_ROOT/results.tsv"

log_case()
{
	local name="$1"
	shift
	local log="$RUN_ROOT/$name.log"
	local status

	printf '\n[R5-full-A] %s\n' "$name" | tee "$log"
	set +e
	"$@" 2>&1 | tee -a "$log"
	status=${PIPESTATUS[0]}
	set -e
	printf '%s=%s\n' "$name" "$status" >>"$RUN_ROOT/results.tsv"
	if [ "$status" -eq 0 ]; then
		PASS=$((PASS + 1))
	else
		FAIL=$((FAIL + 1))
	fi
	return 0
}

skip_case()
{
	local name="$1"
	local reason="$2"
	printf '%s=SKIP:%s\n' "$name" "$reason" >>"$RUN_ROOT/results.tsv"
	printf '[R5-full-A] SKIP %s: %s\n' "$name" "$reason"
	SKIP=$((SKIP + 1))
}

if ! command -v docker >/dev/null 2>&1; then
	skip_case p6-readonly docker-unavailable
else
	if docker inspect "$LAB_PREFIX-r1" >/dev/null 2>&1; then
		LAB_IMAGE=$(docker inspect -f '{{.Config.Image}}@{{.Image}}' \
			"$LAB_PREFIX-r1")
		LAB_STARTED_AT=$(docker inspect -f '{{.State.StartedAt}}' \
			"$LAB_PREFIX-r1")
	fi
	for node in r1 r2 m1a m1b m2a z1 z2; do
		container="$LAB_PREFIX-$node"
		if docker inspect "$container" >/dev/null 2>&1; then
			docker exec "$container" vtysh \
				-c 'show midr self' \
				-c 'show midr rib summary' \
				-c 'show midr lsdb summary' \
				-c 'show midr ted summary' \
				-c 'show midr sync' >"$RUN_ROOT/$node-before.txt" 2>&1 || true
		else
			skip_case "$node-snapshot" "container-not-found:$container"
		fi
	done
	if [ -x "$SCRIPT_DIR/backbone-lab/verify_p6_e2e.sh" ]; then
		log_case p6-readonly env MIDR_LAB_PREFIX="$LAB_PREFIX" \
			"$SCRIPT_DIR/backbone-lab/verify_p6_e2e.sh"
	else
		skip_case p6-readonly verifier-missing
	fi
	if [ "$SOAK_SECONDS" -gt 0 ] 2>/dev/null; then
		printf '[R5-full-A] waiting %ss for refresh baseline\n' "$SOAK_SECONDS" \
			| tee "$RUN_ROOT/soak.log"
		sleep "$SOAK_SECONDS"
		log_case p6-post-refresh env MIDR_LAB_PREFIX="$LAB_PREFIX" \
			"$SCRIPT_DIR/backbone-lab/verify_p6_e2e.sh"
	fi
fi

if [ -n "$BUILD_ROOT" ] && [ -x "$BUILD_ROOT/bgpd/.libs/bgpd" ] &&
	[ -x "$BUILD_ROOT/vtysh/.libs/vtysh" ]; then
		export MIDR_BGPD="$BUILD_ROOT/bgpd/.libs/bgpd"
		export MIDR_VTYSH="$BUILD_ROOT/vtysh/.libs/vtysh"
	export LD_LIBRARY_PATH="$BUILD_ROOT/lib/.libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
	for component in sync owned rib scope; do
		binary="$BUILD_ROOT/tests/bgpd/test_midr_$component"
		if [ -x "$binary" ]; then
			log_case "component-$component" "$binary"
		else
			skip_case "component-$component" test-binary-missing
		fi
	done
	if [ -x "$SCRIPT_DIR/run-m5-multinode.sh" ]; then
		for scenario in line withdraw scope triangle eor-timeout route-refresh \
			prefix prefix-withdraw prefix-takeover peer-reconnect \
			partition-recovery bgpd-restart shutdown; do
			log_case "m5-$scenario" env \
				MIDR_M5_RUN_DIR="$RUN_ROOT/m5-$scenario" \
				"$SCRIPT_DIR/run-m5-multinode.sh" "$scenario"
		done
	else
		skip_case m5-all runner-missing
	fi
else
	skip_case m5-all independent-build-missing
fi

{
	printf 'revision=%s\n' "$(git -C "$ROOT_DIR" rev-parse HEAD 2>/dev/null || echo unknown)"
	printf 'lab_prefix=%s\n' "$LAB_PREFIX"
	printf 'lab_image=%s\n' "$LAB_IMAGE"
	printf 'lab_started_at=%s\n' "$LAB_STARTED_AT"
	printf 'build_root=%s\n' "$BUILD_ROOT"
	printf 'started_at=%s\n' "$STARTED_AT"
	printf 'completed_at=%s\n' "$(date --iso-8601=seconds)"
	printf 'pass=%s\nfail=%s\nskip=%s\n' "$PASS" "$FAIL" "$SKIP"
} >"$RUN_ROOT/summary.txt"
cat "$RUN_ROOT/summary.txt"
printf 'R5-full-A baseline cases: %s passed, %s failed, %s skipped\n' \
	"$PASS" "$FAIL" "$SKIP"

# R5-full-A is a migration gate: missing inputs are incomplete evidence rather
# than a passing baseline.
[ "$FAIL" -eq 0 ] && [ "$SKIP" -eq 0 ]
