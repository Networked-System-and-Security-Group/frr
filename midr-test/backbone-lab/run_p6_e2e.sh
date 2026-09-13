#!/usr/bin/env bash
# P6: full Group 1 -> Group 2 -> Group 3 end-to-end acceptance.
set -uo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
BASE="$SCRIPT_DIR/run_group1_group2_lab.sh"
VERIFY="$SCRIPT_DIR/verify_p6_e2e.sh"
REV=$(git -C "$REPO_ROOT" rev-parse --short=12 HEAD)
LAB_NAME=${MIDR_LAB_NAME:-}
[ -n "$LAB_NAME" ] || LAB_NAME=midr-backbone-p6
IMAGE=${MIDR_LAB_IMAGE:-}
[ -n "$IMAGE" ] || IMAGE=frr-midr-p6:$REV
RUN_ROOT=${MIDR_LAB_RUN_ROOT:-}
[ -n "$RUN_ROOT" ] || RUN_ROOT=$(dirname "$REPO_ROOT")/midr-lab-runs/$LAB_NAME
RUN_ID=${MIDR_EVIDENCE_RUN_ID:-}
[ -n "$RUN_ID" ] || RUN_ID=$(date +%Y%m%d-%H%M%S)
MODE=${1:-check}
[ -n "$MODE" ] || MODE=check
SOAK_SECONDS=${MIDR_P6_SOAK_SECONDS:-330}
[ -n "$SOAK_SECONDS" ] || SOAK_SECONDS=330

export MIDR_LAB_NAME="$LAB_NAME"
export MIDR_LAB_IMAGE="$IMAGE"
export MIDR_LAB_RUN_ROOT="$RUN_ROOT"
export MIDR_LAB_PREFIX=clab-$LAB_NAME
export MIDR_EVIDENCE_RUN_ID="$RUN_ID"

die() { printf 'ERROR: %s\n' "$*" >&2; exit 2; }
preflight()
{
	bash -n "$BASE"
	bash -n "$VERIFY"
	command -v docker >/dev/null 2>&1 || die "docker is unavailable"
	command -v git >/dev/null 2>&1 || die "git is unavailable"
	docker info >/dev/null 2>&1 || die "Docker daemon is unavailable"
	printf 'Repository : %s\nRevision   : %s\nImage      : %s\nLab name   : %s\nRun root   : %s\n' "$REPO_ROOT" "$REV" "$IMAGE" "$LAB_NAME" "$RUN_ROOT"
}
run_base() { "$BASE" "$1"; }
run_verify()
{
	local verify_status=0
	local -a pipeline_status

	"$VERIFY" 2>&1 | tee "$RUN_ROOT/p6-verification.log" || {
		pipeline_status=("${PIPESTATUS[@]}")
		verify_status=${pipeline_status[0]}
		[ "$verify_status" -eq 0 ] && verify_status=${pipeline_status[1]}
	}
	grep '^P6 result:' "$RUN_ROOT/p6-verification.log" >"$RUN_ROOT/p6-summary.txt" || true
	return "$verify_status"
}
run_check()
{
	local base_status verify_status
	set +e
	run_base check
	base_status=$?
	run_verify
	verify_status=$?
	set -e
	[ "$base_status" -eq 0 ] && [ "$verify_status" -eq 0 ]
}
run_all()
{
	local base_status verify_status
	set +e
	run_base all
	base_status=$?
	run_verify
	verify_status=$?
	set -e
	[ "$base_status" -eq 0 ] && [ "$verify_status" -eq 0 ]
}
run_soak()
{
	local status

	export MIDR_EVIDENCE_RUN_ID="$RUN_ID-pre-soak"
	run_check
	status=$?
	cp "$RUN_ROOT/p6-verification.log" \
		"$RUN_ROOT/p6-verification-pre-soak.log"
	[ "$status" -eq 0 ] || return "$status"
	printf 'Waiting %s seconds to cross one owner refresh interval...\n' \
		"$SOAK_SECONDS"
	sleep "$SOAK_SECONDS"
	export MIDR_EVIDENCE_RUN_ID="$RUN_ID-post-soak"
	run_check
	status=$?
	cp "$RUN_ROOT/p6-verification.log" \
		"$RUN_ROOT/p6-verification-post-soak.log"
	return "$status"
}
case "$MODE" in
	preflight) preflight ;;
	build) preflight; run_base build ;;
	deploy) preflight; run_base deploy ;;
	check) preflight; run_check ;;
	all) preflight; run_all ;;
	soak) preflight; run_soak ;;
	*) die "usage: $0 [preflight|build|deploy|check|all|soak]" ;;
esac
