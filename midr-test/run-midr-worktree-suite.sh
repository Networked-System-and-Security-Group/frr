#!/usr/bin/env bash
# Build the current worktree in an isolated container copy, then execute the
# component suite. The host source tree is mounted read-only by the caller.
set -euo pipefail

WORKSPACE=${1:-}
GATE_DIR=${2:-}
TOOLCHAIN_ROOT=${MIDR_GATE_TOOLCHAIN_ROOT:-/home/frr/frr}
JOBS=${MIDR_GATE_JOBS:-8}

die()
{
	printf 'ERROR: %s\n' "$*" >&2
	exit 2
}

[ -n "$WORKSPACE" ] || die "workspace mount is required"
[ -n "$GATE_DIR" ] || die "gate script mount is required"
[ -x "$TOOLCHAIN_ROOT/config.status" ] || \
	die "toolchain image has no configured FRR tree at $TOOLCHAIN_ROOT"

TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT
SOURCE_DIR="$TMP_DIR/source"
PATCH_FILE="$TMP_DIR/worktree.patch"
UNTRACKED_FILE="$TMP_DIR/untracked"

WORKTREE_GIT_DIR=$(sed -n 's/^gitdir: //p' "$WORKSPACE/.git")
git config --global --add safe.directory "$WORKSPACE"
if [ -n "$WORKTREE_GIT_DIR" ]; then
	git config --global --add safe.directory "$WORKTREE_GIT_DIR"
fi
git clone --quiet --no-local "$WORKSPACE" "$SOURCE_DIR"

git -C "$WORKSPACE" diff --binary HEAD -- . >"$PATCH_FILE"
if [ -s "$PATCH_FILE" ]; then
	git -C "$SOURCE_DIR" apply --binary "$PATCH_FILE"
fi
git -C "$WORKSPACE" ls-files --others --exclude-standard -z \
	>"$UNTRACKED_FILE"
if [ -s "$UNTRACKED_FILE" ]; then
	tar --null -C "$WORKSPACE" -T "$UNTRACKED_FILE" -cf - \
		| tar -C "$SOURCE_DIR" -xf -
fi

printf 'WORKTREE_BUILD revision=%s dirty=%s jobs=%s\n' \
	"$(git -C "$WORKSPACE" rev-parse --short=12 HEAD)" \
	"$([ -s "$PATCH_FILE" ] || [ -s "$UNTRACKED_FILE" ] && printf yes || printf no)" \
	"$JOBS"

cd "$SOURCE_DIR"
./bootstrap.sh
read -r -a configure_args <<<"$($TOOLCHAIN_ROOT/config.status --config)"
./configure "${configure_args[@]}"

mapfile -t programs < <(sed -e '/^[[:space:]]*#/d' \
	-e '/^[[:space:]]*$/d' midr-test/midr-component-tests.txt)
targets=()
for program in "${programs[@]}"; do
	targets+=("tests/bgpd/$program")
done
targets+=("tests/bgpd/test_midr_ted_fixture")
make -j"$JOBS" "${targets[@]}"

bash midr-test/run-midr-component-suite.sh \
	"$SOURCE_DIR" "$SOURCE_DIR/midr-test/midr-component-tests.txt"
