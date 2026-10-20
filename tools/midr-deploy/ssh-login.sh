#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: ssh-login.sh [--profile DIR] [--shared-password] [NODE...]

Log in to the MIDR nodes of the profile (default: all of them) and keep one
SSH master connection per node for MIDR_SSH_PERSIST (default 15m).  The
other tools reuse these connections, so after this no node asks again.

Each node is tried with the public key first; a node that refuses it asks
for its password on the terminal.  --shared-password asks one password for
all such nodes.  install-ssh-keys.sh installs the key permanently instead.
EOF
}

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
# shellcheck source=lib/profile.sh
. "$SCRIPT_DIR/lib/profile.sh"
PROFILE_DIR=""
nodes=()

while [ "$#" -gt 0 ]; do
    case "$1" in
        --profile)
            [ "$#" -ge 2 ] || { echo '--profile requires a value' >&2; exit 2; }
            PROFILE_DIR=$2; shift 2 ;;
        --shared-password) export MIDR_SSH_SHARED_PASSWORD=yes; shift ;;
        -h|--help) usage; exit 0 ;;
        -*) usage >&2; exit 2 ;;
        *) nodes+=("$1"); shift ;;
    esac
done
midr_profile_load "$PROFILE_DIR"
trap midr_ssh_cleanup EXIT
[ "${#nodes[@]}" -gt 0 ] || mapfile -t nodes < <(midr_nodes_select name)

failed=0
for node in "${nodes[@]}"; do
    if midr_ssh_login_nodes "$node"; then
        echo "LOGGED IN $node"
    else
        echo "FAILED $node"
        failed=$((failed + 1))
    fi
done
echo "logged-in=$((${#nodes[@]} - failed)) failed=$failed"
[ "$failed" -eq 0 ]
