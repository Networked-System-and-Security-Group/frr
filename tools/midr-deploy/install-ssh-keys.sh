#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: install-ssh-keys.sh [--profile DIR | --inventory FILE] [--key PUBLIC_KEY]
                           [--shared-password]

Install one management host public key on every host of the profile
(default profiles/fiti).
Default: prompt for each host's password when installation is needed.
--shared-password: prompt once and reuse it for hosts sharing a password.
Existing authorized keys are preserved. Every host is verified with SSH.
--inventory takes a legacy file with columns name,host,port,user,...
EOF
}

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
# shellcheck source=lib/profile.sh
. "$SCRIPT_DIR/lib/profile.sh"
INVENTORY=""
PROFILE_DIR=""
PROFILE_INVENTORY=""
PUBLIC_KEY="$HOME/.ssh/id_ed25519.pub"
SHARED_PASSWORD=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --inventory|--key|--profile)
            [ "$#" -ge 2 ] || { usage >&2; exit 2; }
            case "$1" in
                --inventory) INVENTORY=$2 ;;
                --key) PUBLIC_KEY=$2 ;;
                --profile) PROFILE_DIR=$2 ;;
            esac
            shift 2
            ;;
        --shared-password)
            SHARED_PASSWORD=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
done

for command in ssh ssh-copy-id ssh-keygen; do
    command -v "$command" >/dev/null || { echo "Missing command: $command" >&2; exit 1; }
done
if [ -z "$INVENTORY" ]; then
    midr_profile_load "$PROFILE_DIR"
    PROFILE_INVENTORY=$(mktemp)
    INVENTORY=$PROFILE_INVENTORY
    midr_nodes_select name ssh_host ssh_port ssh_user locator node_id role group >"$INVENTORY"
fi
[ -r "$INVENTORY" ] || { echo "Inventory is not readable: $INVENTORY" >&2; exit 1; }
case "$PUBLIC_KEY" in
    *.pub) PRIVATE_KEY=${PUBLIC_KEY%.pub} ;;
    *) echo "--key must name a .pub public key" >&2; exit 2 ;;
esac
[ -r "$PUBLIC_KEY" ] || { echo "Public key is not readable: $PUBLIC_KEY" >&2; exit 1; }
[ -r "$PRIVATE_KEY" ] || { echo "Private key is not readable: $PRIVATE_KEY" >&2; exit 1; }
ssh-keygen -l -f "$PUBLIC_KEY" >/dev/null

# Validate all destinations before editing any authorized_keys file.
node_count=0
while IFS=, read -r name host port user transport node_id role group_id; do
    name=${name%$'\r'}
    case "$name" in ''|'#'*) continue ;; esac
    [[ "$name" =~ ^[A-Za-z0-9_.-]+$ ]] &&
    [[ "$host" =~ ^[A-Za-z0-9][A-Za-z0-9.:-]*$ ]] &&
    [[ "$user" =~ ^[A-Za-z_][A-Za-z0-9_.-]*$ ]] &&
    [[ "$port" =~ ^[0-9]{1,5}$ ]] &&
    (( 10#$port >= 1 && 10#$port <= 65535 )) ||
        { echo "Invalid inventory row: $name" >&2; exit 2; }
    node_count=$((node_count + 1))
done < "$INVENTORY"
[ "$node_count" -gt 0 ] || { echo "Inventory contains no hosts" >&2; exit 2; }

password_dir=""
cleanup() {
    if [ -n "$PROFILE_INVENTORY" ]; then
        rm -f "$PROFILE_INVENTORY"
    fi
    if [ -n "$password_dir" ]; then
        rm -f "$password_dir/password" "$password_dir/askpass"
        rmdir "$password_dir"
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
umask 077

prepare_shared_password() {
    [ -n "$password_dir" ] && return
    password_dir=$(mktemp -d "${TMPDIR:-/tmp}/fiti-ssh-password.XXXXXXXX")
    # OpenSSH reads the password through askpass; never put it in argv.
    IFS= read -r -s -p 'SSH password (only hosts needing key installation): ' shared_password
    printf '\n' >&2
    [ -n "$shared_password" ] || { echo "Password must not be empty" >&2; exit 2; }
    printf '%s\n' "$shared_password" >"$password_dir/password"
    unset shared_password
    cat >"$password_dir/askpass" <<'ASKPASS'
#!/bin/sh
case "$1" in
    *assword*) cat "$FITI_SSH_PASSWORD_FILE" ;;
    *) exit 1 ;;
esac
ASKPASS
    chmod 0700 "$password_dir/askpass"
}

verify_key() {
    ssh -n -i "$PRIVATE_KEY" -o IdentitiesOnly=yes \
        -o BatchMode=yes -o PreferredAuthentications=publickey \
        -o StrictHostKeyChecking=accept-new -o ConnectTimeout=10 \
        -o ConnectionAttempts=1 -p "$2" "$3@$1" hostname
}

success=0
failed=0
exec 3<"$INVENTORY"
while IFS=, read -r name host port user transport node_id role group_id <&3; do
    name=${name%$'\r'}
    case "$name" in ''|'#'*) continue ;; esac
    printf '\nCHECK %s (%s:%s)\n' "$name" "$host" "$port"
    if verify_key "$host" "$port" "$user" >/dev/null 2>&1; then
        printf 'KEY_OK %s (public-key login verified; no password needed)\n' "$name"
        success=$((success + 1))
        continue
    fi

    if [ "$SHARED_PASSWORD" -eq 1 ]; then
        printf 'KEY_MISSING %s (public-key login failed; password will be requested for installation)\n' "$name"
    else
        printf 'KEY_MISSING %s (public-key login failed; attempting interactive password installation)\n' "$name"
    fi

    copy_options=(-i "$PUBLIC_KEY" -p "$port"
        -o ConnectTimeout=10 -o ConnectionAttempts=1
        -o StrictHostKeyChecking=accept-new
        -o NumberOfPasswordPrompts=1)
    if [ "$SHARED_PASSWORD" -eq 1 ]; then
        prepare_shared_password
        if ! env SSH_ASKPASS="$password_dir/askpass" \
            SSH_ASKPASS_REQUIRE=force DISPLAY="${DISPLAY:-fiti:0}" \
            FITI_SSH_PASSWORD_FILE="$password_dir/password" \
            ssh-copy-id "${copy_options[@]}" "$user@$host" </dev/null; then
            printf 'FAILED %s (public key installation)\n' "$name"
            failed=$((failed + 1))
            continue
        fi
    elif ! ssh-copy-id "${copy_options[@]}" "$user@$host" </dev/null; then
        printf 'FAILED %s (public key installation)\n' "$name"
        failed=$((failed + 1))
        continue
    fi

    if verify_key "$host" "$port" "$user"; then
        printf 'INSTALLED_AND_VERIFIED %s\n' "$name"
        success=$((success + 1))
    else
        printf 'FAILED %s (key installed, but passwordless login not verified)\n' "$name"
        failed=$((failed + 1))
    fi
done
exec 3<&-
printf '\nverified=%d failed=%d total=%d\n' "$success" "$failed" "$node_count"
[ "$failed" -eq 0 ]
