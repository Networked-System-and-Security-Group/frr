#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
Usage: deploy-node-from-90.sh --node NAME [--mode check|apply]
Run on NetArchLab90. check is read-only on the target. apply stages the
release/configuration and runs the target-side transactional installer.

RELEASE_SRC selects the release directory. The target directory is derived
from its basename, so the node env files do not need to be edited for every
new release.
USAGE
}

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
NODES="$SCRIPT_DIR/nodes-12.csv"
RELEASE_SRC=${RELEASE_SRC:-}
NODE=""
MODE=check

while [ "$#" -gt 0 ]; do
    case "$1" in
        --node) NODE=$2; shift 2 ;;
        --mode) MODE=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

[ -n "$NODE" ] || { usage >&2; exit 2; }
[ -n "$RELEASE_SRC" ] || { echo 'set RELEASE_SRC to a prepared release directory' >&2; exit 2; }
case "$MODE" in check|apply) ;; *) echo 'mode must be check or apply' >&2; exit 2 ;; esac
[ -d "$RELEASE_SRC" ] || { echo "release missing: $RELEASE_SRC" >&2; exit 1; }
[ -f "$RELEASE_SRC/SHA256SUMS" ] || { echo 'release SHA256SUMS missing' >&2; exit 1; }
[ -x "$RELEASE_SRC/midrd" ] || { echo 'release midrd missing or not executable' >&2; exit 1; }
[ -x "$RELEASE_SRC/zebra" ] || { echo 'release zebra missing or not executable' >&2; exit 1; }
[ -x "$RELEASE_SRC/bgpd" ] || { echo 'release bgpd missing or not executable' >&2; exit 1; }
[ -x "$RELEASE_SRC/vtysh" ] || { echo 'release vtysh missing or not executable' >&2; exit 1; }
[ -x "$RELEASE_SRC/run-midrd.sh" ] || { echo 'release run-midrd.sh missing or not executable' >&2; exit 1; }
[ -r "$RELEASE_SRC/midrd-vty.py" ] || { echo 'release midrd-vty.py missing' >&2; exit 1; }
[ -r "$RELEASE_SRC/midr-bgp-prefix-provider.py" ] || { echo 'release Prefix Provider missing' >&2; exit 1; }
[ -d "$RELEASE_SRC/lib" ] || { echo 'release lib directory missing' >&2; exit 1; }

# Reject an incomplete package before opening an SSH connection or staging it.
(cd "$RELEASE_SRC" && sha256sum -c SHA256SUMS)
for binary in midrd zebra bgpd vtysh; do
    # vtysh/vtysh is a shell launcher in this build; ldd is only meaningful
    # for ELF executables.
    if ! file -b "$RELEASE_SRC/$binary" | grep -Fq 'ELF'; then
        continue
    fi
    dependency_output=$(LD_LIBRARY_PATH="$RELEASE_SRC/lib" \
        ldd "$RELEASE_SRC/$binary" 2>&1)
    if printf '%s\n' "$dependency_output" | grep -Fq 'not found'; then
        printf '%s\n' "$dependency_output" >&2
        echo "release has missing shared-library dependencies: $binary" >&2
        exit 1
    fi
done
[ -r "$SCRIPT_DIR/node-configs/$NODE.env" ] || { echo "node env missing: $NODE" >&2; exit 1; }
[ -r "$SCRIPT_DIR/node-configs/$NODE.commands" ] || { echo "node commands missing: $NODE" >&2; exit 1; }

RELEASE_NAME=$(basename -- "$RELEASE_SRC")
case "$RELEASE_NAME" in
    midr-*[!/]*) ;;
    *) echo "release directory must be named midr-*: $RELEASE_NAME" >&2; exit 1 ;;
esac

IFS=, read -r _name host port user node_id locator edge_bgp role group < <(
    awk -F, -v wanted="$NODE" 'NR > 1 && $1 == wanted {print}' "$NODES"
)
[ -n "${port:-}" ] || { echo "node not in nodes-12.csv: $NODE" >&2; exit 1; }

stage=$(mktemp -d "/tmp/midr-stage-$NODE.XXXXXX")
cleanup() { rm -rf "$stage"; }
trap cleanup EXIT
mkdir -p "$stage/release"
awk -v release_dir="/opt/midr-releases/$RELEASE_NAME" '
    BEGIN { replaced = 0 }
    /^MIDR_RELEASE_DIR=/ {
        print "MIDR_RELEASE_DIR=" release_dir
        replaced = 1
        next
    }
    { print }
    END {
        if (!replaced) print "MIDR_RELEASE_DIR=" release_dir
    }
' "$SCRIPT_DIR/node-configs/$NODE.env" >"$stage/node.env"
cp "$SCRIPT_DIR/node-configs/$NODE.commands" "$stage/node.commands"
if [ -r "$SCRIPT_DIR/node-configs/$NODE.bgpd.conf" ]; then
    cp "$SCRIPT_DIR/node-configs/$NODE.bgpd.conf" "$stage/node.bgpd.conf"
fi
cp "$RELEASE_SRC"/SHA256SUMS \
   "$RELEASE_SRC"/midrd "$RELEASE_SRC"/zebra "$RELEASE_SRC"/bgpd \
   "$RELEASE_SRC"/vtysh "$RELEASE_SRC"/midrd-vty.py \
   "$RELEASE_SRC"/midr-bgp-prefix-provider.py \
   "$RELEASE_SRC"/run-midrd.sh "$stage/release/"
for metadata in GIT_COMMIT RELEASE_ID; do
    if [ -f "$RELEASE_SRC/$metadata" ]; then
        cp "$RELEASE_SRC/$metadata" "$stage/release/"
    fi
done
cp -a "$RELEASE_SRC/lib" "$stage/release/"
cp "$SCRIPT_DIR"/start-midr-zebra.sh "$SCRIPT_DIR"/wait-midr-zebra.sh \
   "$SCRIPT_DIR"/start-midr-bgpd.sh "$SCRIPT_DIR"/start-midrd-service.sh \
   "$SCRIPT_DIR"/load-midrd-config.sh \
   "$SCRIPT_DIR"/start-midr-bgp-prefix-provider.sh \
   "$SCRIPT_DIR"/midr-zebra.service "$SCRIPT_DIR"/midr-bgpd.service \
   "$SCRIPT_DIR"/midrd.service "$SCRIPT_DIR"/midr-bgp-prefix-provider.service \
   "$SCRIPT_DIR"/setup-midr-underlay.py \
   "$SCRIPT_DIR"/install-node-local.sh "$stage/"

remote_stage="/tmp/midr-stage-$NODE"
ssh -o BatchMode=yes -o ConnectTimeout=10 -p "$port" "$user@$host" \
    "rm -rf '$remote_stage'; mkdir -p '$remote_stage/release'"
scp -q -P "$port" "$stage/node.env" "$stage/node.commands" "$stage"/*.sh "$stage"/*.service "$stage/setup-midr-underlay.py" \
    "$user@$host:$remote_stage/"
if [ -f "$stage/node.bgpd.conf" ]; then
    scp -q -P "$port" "$stage/node.bgpd.conf" "$user@$host:$remote_stage/"
fi
# Copy release regular files first.  The lib/ entry is a directory and must
# be copied separately with -r; including it in the first scp makes scp fail
# with "not a regular file".
release_files=()
for file in "$stage/release"/*; do
    if [ -f "$file" ]; then
        release_files+=("$file")
    fi
done
[ "${#release_files[@]}" -gt 0 ] || {
    echo "release staging directory contains no regular files: $stage/release" >&2
    exit 1
}
scp -q -P "$port" "${release_files[@]}" \
    "$user@$host:$remote_stage/release/"
scp -q -r -P "$port" "$stage/release/lib" "$user@$host:$remote_stage/release/"
ssh -o BatchMode=yes -o ConnectTimeout=10 -p "$port" "$user@$host" \
    "chmod 0755 '$remote_stage'/*.sh '$remote_stage'/release/run-midrd.sh '$remote_stage'/release/midrd '$remote_stage'/release/zebra '$remote_stage'/release/bgpd '$remote_stage'/release/vtysh; '$remote_stage/install-node-local.sh' '$remote_stage' '$MODE'"
