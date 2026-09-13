#!/usr/bin/env bash
# Run the complete non-privileged MIDR component gate either against the
# current worktree (isolated container build) or an existing image.
set -uo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
MODE=${1:-worktree}
BASELINE_IMAGE=frr-midr-p6:f9beedd0d653
IMAGE=${MIDR_GATE_IMAGE:-$BASELINE_IMAGE}
TOOLCHAIN_IMAGE=${MIDR_GATE_TOOLCHAIN_IMAGE:-$BASELINE_IMAGE}
RUN_ID=${MIDR_GATE_RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}
OUT_DIR=${MIDR_GATE_OUT:-$(dirname "$REPO_ROOT")/midr-gate-runs/$RUN_ID-$MODE}
LOG="$OUT_DIR/component-tests.log"
TSV="$OUT_DIR/component-tests.tsv"

die()
{
	printf 'ERROR: %s\n' "$*" >&2
	exit 2
}

case "$MODE" in
	worktree|image) ;;
	*) die "usage: $0 [worktree|image]" ;;
esac
command -v docker >/dev/null 2>&1 || die "docker is unavailable"
docker info >/dev/null 2>&1 || die "Docker daemon is unavailable"
mkdir -p "$OUT_DIR" || die "cannot create output directory: $OUT_DIR"

revision=$(git -C "$REPO_ROOT" rev-parse --short=12 HEAD) || \
	die "cannot resolve repository revision"
dirty=no
if [ -n "$(git -C "$REPO_ROOT" status --porcelain=v1)" ]; then
	dirty=yes
fi

{
	printf '# MIDR component regression gate\n'
	printf '# mode: %s\n' "$MODE"
	printf '# repository: %s\n' "$REPO_ROOT"
	printf '# revision: %s\n' "$revision"
	printf '# worktree-dirty: %s\n' "$dirty"
	printf '# run-at: %s\n' "$(date --iso-8601=seconds)"
} >"$LOG"

if [ "$MODE" = image ]; then
	docker image inspect "$IMAGE" >/dev/null 2>&1 || \
		die "image is unavailable: $IMAGE"
	{
		printf '# image: %s\n' "$IMAGE"
		docker image inspect "$IMAGE" \
			--format '# image-id: {{.Id}}\n# image-created: {{.Created}}'
	} >>"$LOG"
	command=(docker run --rm
		-v "$SCRIPT_DIR:/midr-gate:ro"
		-e "MIDR_COMPONENT_TIMEOUT_SECONDS=${MIDR_COMPONENT_TIMEOUT_SECONDS:-180}"
		-e "MIDR_FIXTURE_TIMEOUT_SECONDS=${MIDR_FIXTURE_TIMEOUT_SECONDS:-300}"
		"$IMAGE" bash /midr-gate/run-midr-component-suite.sh
		/home/frr/frr /midr-gate/midr-component-tests.txt)
else
	"$SCRIPT_DIR/check-source-build-artifacts.sh" "$REPO_ROOT" \
		|| die "source worktree contains build artifacts"
	GIT_COMMON_DIR=$(git -C "$REPO_ROOT" rev-parse \
		--path-format=absolute --git-common-dir) || \
		die "cannot resolve Git common directory"
	docker image inspect "$TOOLCHAIN_IMAGE" >/dev/null 2>&1 || \
		die "toolchain image is unavailable: $TOOLCHAIN_IMAGE"
	{
		printf '# toolchain-image: %s\n' "$TOOLCHAIN_IMAGE"
		docker image inspect "$TOOLCHAIN_IMAGE" \
			--format '# toolchain-image-id: {{.Id}}\n# toolchain-image-created: {{.Created}}'
	} >>"$LOG"
	command=(docker run --rm
		-v "$REPO_ROOT:$REPO_ROOT:ro"
		-v "$GIT_COMMON_DIR:$GIT_COMMON_DIR:ro"
		-v "$SCRIPT_DIR:/midr-gate:ro"
		-e "MIDR_COMPONENT_TIMEOUT_SECONDS=${MIDR_COMPONENT_TIMEOUT_SECONDS:-180}"
		-e "MIDR_FIXTURE_TIMEOUT_SECONDS=${MIDR_FIXTURE_TIMEOUT_SECONDS:-300}"
		-e "MIDR_GATE_JOBS=${MIDR_GATE_JOBS:-8}"
		"$TOOLCHAIN_IMAGE" bash /midr-gate/run-midr-worktree-suite.sh
		"$REPO_ROOT" /midr-gate)
fi

"${command[@]}" 2>&1 | tee -a "$LOG"
run_rc=${PIPESTATUS[0]}
printf 'test\texit\n' >"$TSV"
awk -F '\t' '$1 == "RESULT" {print $2 "\t" $3}' "$LOG" >>"$TSV"

if [ "$run_rc" -eq 0 ] && grep -q '^COMPONENT_RESULT=0$' "$LOG"; then
	printf 'GATE_RESULT=0\n' | tee -a "$LOG"
else
	[ "$run_rc" -ne 0 ] || run_rc=1
	printf 'GATE_RESULT=%d\n' "$run_rc" | tee -a "$LOG"
fi
printf 'Evidence: %s\n' "$OUT_DIR"
exit "$run_rc"
