#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: build-release.sh [--build-dir DIR] [--out-dir DIR] [--tag NAME]
                        [--no-make] [--allow-dirty]

Build midrd, zebra, bgpd and vtysh in a configured FRR build tree and
assemble a relocatable MIDR release directory:

  OUT_DIR/midr-<sha12>[-tag][-dirty]-<UTC>/
    midrd zebra bgpd vtysh run-midrd.sh midrd-vty.py
    midr-bgp-prefix-provider.py lib/ RELEASE_ID GIT_COMMIT SHA256SUMS

--build-dir   configured FRR tree (default: the repository holding this script)
--out-dir     parent directory of the release (default: /tmp)
--tag         short label added to the release id, e.g. g1rejoin
--no-make     package the existing build without running make
--allow-dirty package a tree with uncommitted tracked changes; the release
              id is marked -dirty

Extra system libraries copied into lib/ are listed in MIDR_RELEASE_EXTRA_LIBS
(space separated sonames, default "libyang.so.3"); their paths are resolved
with ldd against the built zebra.
EOF
}

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
BUILD_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd -P)
OUT_DIR=/tmp
TAG=""
RUN_MAKE=1
ALLOW_DIRTY=0
EXTRA_LIBS=${MIDR_RELEASE_EXTRA_LIBS-libyang.so.3}

need_value() {
    [ "$#" -ge 2 ] && [ -n "$2" ] || { echo "$1 requires a value" >&2; exit 2; }
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --build-dir) need_value "$@"; BUILD_DIR=$2; shift 2 ;;
        --out-dir) need_value "$@"; OUT_DIR=$2; shift 2 ;;
        --tag) need_value "$@"; TAG=$2; shift 2 ;;
        --no-make) RUN_MAKE=0; shift ;;
        --allow-dirty) ALLOW_DIRTY=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

[[ -z "$TAG" || "$TAG" =~ ^[A-Za-z0-9._]+$ ]] ||
    { echo "--tag may contain only letters, digits, '.' and '_'" >&2; exit 2; }
BUILD_DIR=$(CDPATH= cd -- "$BUILD_DIR" && pwd -P)
[ -f "$BUILD_DIR/Makefile" ] ||
    { echo "not a configured FRR build tree (no Makefile): $BUILD_DIR" >&2; exit 1; }
[ -d "$OUT_DIR" ] || { echo "output directory does not exist: $OUT_DIR" >&2; exit 1; }

commit=$(git -C "$BUILD_DIR" rev-parse HEAD)
dirty=""
if [ -n "$(git -C "$BUILD_DIR" status --porcelain --untracked-files=no)" ]; then
    if [ "$ALLOW_DIRTY" -ne 1 ]; then
        echo "build tree has uncommitted changes; commit them or pass --allow-dirty" >&2
        git -C "$BUILD_DIR" status --short --untracked-files=no >&2
        exit 1
    fi
    dirty=-dirty
fi

if [ "$RUN_MAKE" -eq 1 ]; then
    make -C "$BUILD_DIR" -j"$(nproc)" midrd/midrd zebra/zebra bgpd/bgpd vtysh/vtysh
fi

release_id=${commit:0:12}${TAG:+-$TAG}$dirty-$(date -u +%Y%m%dT%H%M%SZ)
release=$OUT_DIR/midr-$release_id
[ ! -e "$release" ] || { echo "release already exists: $release" >&2; exit 1; }
work=$(mktemp -d "$OUT_DIR/.midr-release.XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir "$work/lib"

# The top-level midrd, zebra, ... are libtool wrapper scripts; package the
# real executables from .libs.
for daemon in midrd zebra bgpd vtysh; do
    binary=$BUILD_DIR/$daemon/.libs/$daemon
    case "$(head -c 4 "$binary" 2>/dev/null | od -An -c | tr -d ' ')" in
        177ELF) ;;
        *) echo "missing ELF executable: $binary" >&2; exit 1 ;;
    esac
    install -m 0755 "$binary" "$work/$daemon"
done
install -m 0755 "$SCRIPT_DIR/node/midrd-vty.py" "$work/midrd-vty.py"
install -m 0755 "$SCRIPT_DIR/node/midr-bgp-prefix-provider.py" \
    "$work/midr-bgp-prefix-provider.py"
cat >"$work/run-midrd.sh" <<'EOF'
#!/bin/sh
set -eu
BASE_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
export LD_LIBRARY_PATH="$BASE_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$BASE_DIR/midrd" "$@"
EOF
chmod 0755 "$work/run-midrd.sh"

cp -L "$BUILD_DIR/lib/.libs/libfrr.so.0" "$work/lib/libfrr.so.0"
cp -L "$BUILD_DIR/mlag/.libs/libmlag_pb.so.0" "$work/lib/libmlag_pb.so.0"
for soname in $EXTRA_LIBS; do
    path=$(LD_LIBRARY_PATH="$work/lib" ldd "$work/zebra" |
        awk -v lib="$soname" '$1 == lib && $3 ~ /^\// { print $3; exit }')
    [ -n "$path" ] || { echo "cannot resolve $soname with ldd" >&2; exit 1; }
    install -m 0644 "$path" "$work/lib/$soname"
done

printf '%s\n' "$release_id" >"$work/RELEASE_ID"
printf '%s\n' "$commit" >"$work/GIT_COMMIT"
(cd "$work" && find . -type f ! -name SHA256SUMS ! -name RELEASE_ID ! -name GIT_COMMIT |
    sed 's|^\./||' | LC_ALL=C sort | xargs sha256sum >SHA256SUMS)

# Self-check: every packaged library must come from lib/, nothing missing.
failed=0
for daemon in midrd zebra bgpd vtysh; do
    deps=$(LD_LIBRARY_PATH="$work/lib" ldd "$work/$daemon")
    if printf '%s\n' "$deps" | grep -q 'not found'; then
        echo "unresolved libraries in $daemon:" >&2
        printf '%s\n' "$deps" | grep 'not found' >&2
        failed=1
    fi
    for soname in libfrr.so.0 $EXTRA_LIBS; do
        resolved=$(printf '%s\n' "$deps" | awk -v lib="$soname" '$1 == lib { print $3 }')
        if [ -n "$resolved" ] && [ "$resolved" != "$work/lib/$soname" ]; then
            echo "$daemon resolves $soname to $resolved instead of lib/" >&2
            failed=1
        fi
    done
done
(cd "$work" && sha256sum --quiet -c SHA256SUMS) || failed=1
[ "$failed" -eq 0 ] || { echo "release self-check failed" >&2; exit 1; }

chmod 0755 "$work"
mv "$work" "$release"
trap - EXIT
echo "release=$release"
cat "$release/SHA256SUMS"
