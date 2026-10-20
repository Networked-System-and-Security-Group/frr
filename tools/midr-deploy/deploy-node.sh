#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
Usage: deploy-node.sh --node NAME [--mode check|apply] [--profile DIR]

Stage a release and the node's configuration on one MIDR node and run the
target-side installer.  check is read-only on the target: it only validates
the host, the release and the configuration.  apply backs up the node, stops
the old routing daemons and starts the matching MIDR services.

RELEASE_SRC must point to a release directory built by build-release.sh.  The
target directory is derived from its basename, so node env files do not need
to be edited for each release.

The profile (default: profiles/fiti, or $MIDR_PROFILE) provides nodes.csv,
node-configs/ and profile.env.

SSH uses the public key; when the node refuses it, the script asks for the
password on the terminal (see lib/profile.sh, MIDR_SSH_PASSWORD_FALLBACK and
MIDR_SSH_SHARED_PASSWORD).
USAGE
}

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
# shellcheck source=lib/profile.sh
. "$SCRIPT_DIR/lib/profile.sh"
RELEASE_SRC=${RELEASE_SRC:-}
NODE=""
MODE=check
PROFILE_DIR=""

need_value() {
    [ "$2" -ge 2 ] || { echo "$1 requires a value" >&2; usage >&2; exit 2; }
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --node) need_value "$1" "$#"; NODE=$2; shift 2 ;;
        --mode) need_value "$1" "$#"; MODE=$2; shift 2 ;;
        --profile) need_value "$1" "$#"; PROFILE_DIR=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

[ -n "$NODE" ] || { usage >&2; exit 2; }
[ -n "$RELEASE_SRC" ] || { echo 'set RELEASE_SRC to a prepared release directory' >&2; exit 2; }
case "$MODE" in check|apply) ;; *) echo 'mode must be check or apply' >&2; exit 2 ;; esac
midr_profile_load "$PROFILE_DIR"

[ -d "$RELEASE_SRC" ] || { echo "release missing: $RELEASE_SRC" >&2; exit 1; }
[ -f "$RELEASE_SRC/SHA256SUMS" ] || { echo 'release SHA256SUMS missing' >&2; exit 1; }
for binary in midrd zebra bgpd vtysh run-midrd.sh; do
    [ -x "$RELEASE_SRC/$binary" ] || { echo "release $binary missing or not executable" >&2; exit 1; }
done
[ -r "$RELEASE_SRC/midrd-vty.py" ] || { echo 'release midrd-vty.py missing' >&2; exit 1; }
[ -r "$RELEASE_SRC/midr-bgp-prefix-provider.py" ] || { echo 'release Prefix Provider missing' >&2; exit 1; }
[ -d "$RELEASE_SRC/lib" ] || { echo 'release lib directory missing' >&2; exit 1; }

# Reject an incomplete package before opening an SSH connection or staging it.
(cd "$RELEASE_SRC" && sha256sum -c --quiet SHA256SUMS)
for binary in midrd zebra bgpd vtysh; do
    if ! file -b "$RELEASE_SRC/$binary" | grep -Fq 'ELF'; then
        echo "release $binary is not an ELF executable (libtool wrapper?)" >&2
        exit 1
    fi
    dependency_output=$(LD_LIBRARY_PATH="$RELEASE_SRC/lib" \
        ldd "$RELEASE_SRC/$binary" 2>&1)
    if printf '%s\n' "$dependency_output" | grep -Fq 'not found'; then
        printf '%s\n' "$dependency_output" >&2
        echo "release has missing shared-library dependencies: $binary" >&2
        exit 1
    fi
done
[ -r "$MIDR_NODE_CONFIGS/$NODE.env" ] || { echo "node env missing: $MIDR_NODE_CONFIGS/$NODE.env" >&2; exit 1; }
[ -r "$MIDR_NODE_CONFIGS/$NODE.commands" ] || { echo "node commands missing: $MIDR_NODE_CONFIGS/$NODE.commands" >&2; exit 1; }

RELEASE_NAME=$(basename -- "$RELEASE_SRC")
case "$RELEASE_NAME" in
    midr-?*) ;;
    *) echo "release directory must be named midr-*: $RELEASE_NAME" >&2; exit 1 ;;
esac

row=$(midr_nodes_select name ssh_host ssh_port ssh_user | awk -F, -v node="$NODE" '$1 == node')
[ -n "$row" ] || { echo "node not in $MIDR_NODES_FILE: $NODE" >&2; exit 1; }
IFS=, read -r _name host port user <<<"$row"
target="$user@$host"
midr_ssh_login "$port" "$target" || exit 1

stage=$(mktemp -d "${TMPDIR:-/tmp}/midr-stage-$NODE.XXXXXX")
cleanup() { rm -rf "$stage"; midr_ssh_cleanup; }
trap cleanup EXIT
mkdir -p "$stage/release"

# node.env: the node's own settings, the release directory chosen here, and
# the profile-wide defaults the node does not override.
{
    grep -v '^MIDR_RELEASE_DIR=' "$MIDR_NODE_CONFIGS/$NODE.env"
    printf 'MIDR_RELEASE_DIR=%s\n' "/opt/midr-releases/$RELEASE_NAME"
    for var in MIDR_UNDERLAY_INTERFACE MIDR_PROTECTED_NETPLAN; do
        if [ -n "${!var:-}" ] && ! grep -q "^$var=" "$MIDR_NODE_CONFIGS/$NODE.env"; then
            # Double quotes are understood by both bash and systemd's
            # EnvironmentFile parser; %q escaping is not.
            printf '%s="%s"\n' "$var" "${!var}"
        fi
    done
} >"$stage/node.env"
cp "$MIDR_NODE_CONFIGS/$NODE.commands" "$stage/node.commands"
midr_locator_prefixes >"$stage/node.locators"
if [ -r "$MIDR_NODE_CONFIGS/$NODE.bgpd.conf" ]; then
    cp "$MIDR_NODE_CONFIGS/$NODE.bgpd.conf" "$stage/node.bgpd.conf"
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
cp "$SCRIPT_DIR"/node/*.sh "$SCRIPT_DIR"/node/*.service "$SCRIPT_DIR"/node/*.timer \
   "$SCRIPT_DIR"/node/setup-midr-underlay.py \
   "$SCRIPT_DIR"/node/midr-logrotate.conf "$SCRIPT_DIR"/node/needrestart-midr.conf \
   "$SCRIPT_DIR"/install-node-local.sh "$stage/"

remote_stage="/tmp/midr-stage-$NODE"
midr_ssh -p "$port" "$target" "rm -rf '$remote_stage'; mkdir -p '$remote_stage/release'"
stage_files=()
for file in "$stage"/*; do
    [ -f "$file" ] && stage_files+=("$file")
done
midr_scp -P "$port" "${stage_files[@]}" "$target:$remote_stage/"
# Copy release regular files first.  lib/ is a directory and must be copied
# separately with -r; mixing it into the first scp fails with "not a regular
# file".
release_files=()
for file in "$stage/release"/*; do
    [ -f "$file" ] && release_files+=("$file")
done
midr_scp -P "$port" "${release_files[@]}" "$target:$remote_stage/release/"
midr_scp -r -P "$port" "$stage/release/lib" "$target:$remote_stage/release/"
midr_ssh -p "$port" "$target" \
    "chmod 0755 '$remote_stage'/*.sh '$remote_stage'/release/run-midrd.sh '$remote_stage'/release/midrd '$remote_stage'/release/zebra '$remote_stage'/release/bgpd '$remote_stage'/release/vtysh; '$remote_stage/install-node-local.sh' '$remote_stage' '$MODE'"
