#!/usr/bin/env bash
# Build and deploy an isolated 15-node group1+group2 integration lab.

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
GIT=(git -c "safe.directory=$REPO_ROOT" -C "$REPO_ROOT")
REV=$("${GIT[@]}" rev-parse --short=12 HEAD)
LAB_NAME=${MIDR_LAB_NAME:-midr-backbone-yhy}
LAB_PREFIX="clab-$LAB_NAME"
IMAGE=${MIDR_LAB_IMAGE:-frr-midr-g12:$REV}
UBUNTU_VERSION=${MIDR_UBUNTU_VERSION:-22.04}
RUN_ROOT=${MIDR_LAB_RUN_ROOT:-"$(dirname "$REPO_ROOT")/midr-lab-runs/$LAB_NAME"}
TOPOLOGY="$RUN_ROOT/midr-backbone.clab.yaml"
ACCEPTANCE="$REPO_ROOT/midr-test/backbone-group2/run_group1_group2_demo_check.sh"
CAPTURE="$REPO_ROOT/midr-test/backbone-group2/capture_group1_group2_state.sh"
VERIFY="$REPO_ROOT/midr-test/backbone-group2/verify_group1_group2_evidence.py"
MODE=${1:-all}

die()
{
	printf 'ERROR: %s\n' "$*" >&2
	exit 1
}

require_command()
{
	command -v "$1" >/dev/null 2>&1 || die "required command is unavailable: $1"
}

ensure_root()
{
	[ "$(id -u)" -eq 0 ] ||
		die "this mode needs root; run: sudo -E $0 $MODE"
}

check_lab_name()
{
	case "$LAB_NAME" in
		*[!a-zA-Z0-9_-]*|'')
			die "MIDR_LAB_NAME may contain only letters, digits, '_' and '-'"
			;;
	esac
}

check_existing_lab()
{
	local existing

	existing=$(docker ps -a --format '{{.Names}}' |
		grep -E "^${LAB_PREFIX}-" || true)
	[ -z "$existing" ] || {
		printf 'An existing lab uses prefix %s:\n%s\n' "$LAB_PREFIX" "$existing" >&2
		die "refusing to replace or destroy an existing lab"
	}
}

preflight()
{
	require_command git
	require_command docker
	require_command containerlab
	require_command python3
	require_command sed
	require_command timeout
	check_lab_name

	docker info >/dev/null 2>&1 || die "Docker daemon is unavailable to the current user"
	containerlab version
	"${GIT[@]}" diff --check
	bash -n "$ACCEPTANCE"
	bash -n "$CAPTURE"
	python3 -c 'import ast, pathlib, sys; ast.parse(pathlib.Path(sys.argv[1]).read_text())' "$VERIFY"
	[ -f "$SCRIPT_DIR/midr-backbone.clab.yaml" ] || die "15-node topology is missing"
	[ "$(grep -c 'image: frr-ubuntu20:latest' "$SCRIPT_DIR/midr-backbone.clab.yaml")" -eq 15 ] ||
		die "the checked-in topology does not contain exactly 15 FRR nodes"
	[ "$(grep -c 'endpoints:' "$SCRIPT_DIR/midr-backbone.clab.yaml")" -eq 15 ] ||
		die "the checked-in topology does not contain exactly 15 links"

	printf 'Repository : %s\n' "$REPO_ROOT"
	printf 'Revision   : %s%s\n' "$REV" "$("${GIT[@]}" diff --quiet && "${GIT[@]}" diff --cached --quiet || printf ' + local changes')"
	printf 'Image      : %s\n' "$IMAGE"
	printf 'Ubuntu     : %s\n' "$UBUNTU_VERSION"
	printf 'Lab name   : %s\n' "$LAB_NAME"
	printf 'Run root   : %s\n' "$RUN_ROOT"
	df -h "$REPO_ROOT" | tail -n 1
}

prepare_run_root()
{
	if [ -e "$RUN_ROOT" ]; then
		[ -f "$RUN_ROOT/build-metadata.txt" ] ||
			die "run directory already exists and was not prepared by this script: $RUN_ROOT"
		grep -qx "revision=$REV" "$RUN_ROOT/build-metadata.txt" &&
			grep -qx "image=$IMAGE" "$RUN_ROOT/build-metadata.txt" &&
			grep -qx "lab_name=$LAB_NAME" "$RUN_ROOT/build-metadata.txt" ||
			die "existing run directory belongs to a different build: $RUN_ROOT"
		if grep -q '^ubuntu_version=' "$RUN_ROOT/build-metadata.txt"; then
			grep -qx "ubuntu_version=$UBUNTU_VERSION" "$RUN_ROOT/build-metadata.txt" ||
				die "existing run directory uses a different Ubuntu version: $RUN_ROOT"
		else
			printf 'ubuntu_version=%s\n' "$UBUNTU_VERSION" >>"$RUN_ROOT/build-metadata.txt"
		fi
		printf 'Reusing prepared run directory: %s\n' "$RUN_ROOT"
		return
	fi
	mkdir -p "$RUN_ROOT"
	cp -a "$SCRIPT_DIR/configs-backbone" "$RUN_ROOT/configs-backbone"
	mkdir -p "$RUN_ROOT/logs-backbone"
	for node in b1 b2 b3 b4 b5 t1 t2 t3 r1 r2 m1a m1b m2a z1 z2; do
		mkdir -p "$RUN_ROOT/logs-backbone/$node"
	done
	sed -e "s/^name: .*/name: $LAB_NAME/" \
		-e "s|image: frr-ubuntu20:latest|image: $IMAGE|g" \
		"$SCRIPT_DIR/midr-backbone.clab.yaml" >"$TOPOLOGY"
	{
		printf 'repository=%s\n' "$REPO_ROOT"
		printf 'revision=%s\n' "$REV"
		printf 'image=%s\n' "$IMAGE"
		printf 'ubuntu_version=%s\n' "$UBUNTU_VERSION"
		printf 'lab_name=%s\n' "$LAB_NAME"
		printf 'lab_prefix=%s\n' "$LAB_PREFIX"
		printf 'prepared_at=%s\n' "$(date --iso-8601=seconds)"
		"${GIT[@]}" status --short
	} >"$RUN_ROOT/build-metadata.txt"
}

build_image()
{
	local build_cpuset=${MIDR_BUILD_CPUSET:-0-15}

	if [ "${MIDR_SKIP_IMAGE_BUILD:-0}" = 1 ]; then
		docker image inspect "$IMAGE" >/dev/null 2>&1 ||
			die "MIDR_SKIP_IMAGE_BUILD=1 but image does not exist: $IMAGE"
		return
	fi

	DOCKER_BUILDKIT=0 docker build \
		--cpuset-cpus "$build_cpuset" \
		--force-rm \
		--network host \
		--tag "$IMAGE" \
		--build-arg UBUNTU_VERSION="$UBUNTU_VERSION" \
		--build-arg FRR_SOURCE_REV="$REV" \
		--file "$REPO_ROOT/docker/ubuntu-ci/Dockerfile" \
		"$REPO_ROOT" 2>&1 | tee "$RUN_ROOT/image-build.log"
	docker run --rm "$IMAGE" /usr/lib/frr/bgpd --version |
		tee "$RUN_ROOT/image-smoke.log"
}

deploy_lab()
{
	check_existing_lab
	containerlab deploy --topo "$TOPOLOGY" 2>&1 | tee "$RUN_ROOT/deploy.log"
	local running
	running=$(docker ps --format '{{.Names}}' | grep -cE "^${LAB_PREFIX}-" || true)
	[ "$running" -eq 15 ] || die "expected 15 running containers, found $running"
	containerlab inspect --topo "$TOPOLOGY" | tee "$RUN_ROOT/inspect.log"
}

run_acceptance()
{
	local acceptance_status capture_status

	set +e
	MIDR_LAB_PREFIX="$LAB_PREFIX" \
		MIDR_DEMO_WAIT_SECONDS="${MIDR_DEMO_WAIT_SECONDS:-600}" \
		MIDR_DEMO_POLL_SECONDS="${MIDR_DEMO_POLL_SECONDS:-10}" \
		"$ACCEPTANCE" 2>&1 | tee "$RUN_ROOT/acceptance.log"
	acceptance_status=${PIPESTATUS[0]}
	run_capture post-convergence
	capture_status=$?
	set -e
	[ "$acceptance_status" -eq 0 ] && [ "$capture_status" -eq 0 ]
}

run_capture()
{
	local phase=${1:-manual}

	MIDR_LAB_PREFIX="$LAB_PREFIX" \
		MIDR_LAB_RUN_ROOT="$RUN_ROOT" \
		"$CAPTURE" "$phase" 2>&1 | tee "$RUN_ROOT/capture-$phase.log"
	return "${PIPESTATUS[0]}"
}

case "$MODE" in
	preflight)
		preflight
		;;
	build)
		preflight
		check_existing_lab
		prepare_run_root
		build_image
		;;
	deploy)
		ensure_root
		preflight
		[ -f "$TOPOLOGY" ] || die "prepared topology is missing: $TOPOLOGY"
		deploy_lab
		;;
	all)
		ensure_root
		preflight
		check_existing_lab
		prepare_run_root
		build_image
		deploy_lab
		run_acceptance
		;;
	check)
		preflight
		[ -f "$TOPOLOGY" ] || die "prepared topology is missing: $TOPOLOGY"
		run_acceptance
		;;
	capture)
		preflight
		run_capture manual
		;;
	*)
		die "usage: $0 [preflight|build|deploy|all|check|capture]"
		;;
esac
