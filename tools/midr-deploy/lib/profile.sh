# Shared helpers for the MIDR deployment tools.  Source this file; do not run it.
#
# A deployment profile is a directory describing one environment:
#   nodes.csv        one row per MIDR node (header names, see below)
#   node-configs/    <name>.env, <name>.commands and optional <name>.bgpd.conf
#   profile.env      optional environment-wide defaults (shell assignments)
#
# nodes.csv columns, matched by header name:
#   name,ssh_host,ssh_port,ssh_user,node_id,locator,gateway,edge_bgp,role,group
# Lines starting with '#' after the header and blank lines are ignored.
#
# The profile is chosen by --profile DIR, then $MIDR_PROFILE, then the FITI
# profile shipped next to these tools.

MIDR_DEPLOY_DIR=${MIDR_DEPLOY_DIR:-$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)}

midr_profile_load() {
	local dir=${1:-${MIDR_PROFILE:-$MIDR_DEPLOY_DIR/profiles/fiti}}

	if [ ! -d "$dir" ]; then
		echo "profile directory not found: $dir" >&2
		return 1
	fi
	MIDR_PROFILE=$(CDPATH= cd -- "$dir" && pwd -P)
	MIDR_NODES_FILE=$MIDR_PROFILE/nodes.csv
	MIDR_NODE_CONFIGS=$MIDR_PROFILE/node-configs
	if [ ! -r "$MIDR_NODES_FILE" ]; then
		echo "profile has no readable nodes.csv: $MIDR_NODES_FILE" >&2
		return 1
	fi
	if [ -r "$MIDR_PROFILE/profile.env" ]; then
		set -a
		# shellcheck disable=SC1091
		. "$MIDR_PROFILE/profile.env"
		set +a
	fi
	# Extra ssh/scp options for every node connection, e.g. a ProxyJump.
	MIDR_SSH_EXTRA=${MIDR_SSH_EXTRA:-}
	export MIDR_PROFILE MIDR_NODES_FILE MIDR_NODE_CONFIGS MIDR_SSH_EXTRA
}

# midr_nodes_select FIELD... : print the requested columns of every node,
# comma separated, in file order.
midr_nodes_select() {
	awk -F, -v want="$*" '
		function trim(s) { gsub(/^[ \t]+|[ \t\r]+$/, "", s); return s }
		/^[ \t]*$/ { next }
		!header {
			sub(/^#[ \t]*/, "")
			for (i = 1; i <= NF; i++) column[trim($i)] = i
			n = split(want, fields, " ")
			for (i = 1; i <= n; i++)
				if (!(fields[i] in column)) {
					printf "nodes.csv has no column: %s\n", fields[i] > "/dev/stderr"
					exit 2
				}
			header = 1
			next
		}
		/^[ \t]*#/ { next }
		{
			line = ""
			for (i = 1; i <= n; i++)
				line = line (i > 1 ? "," : "") trim($(column[fields[i]]))
			print line
		}
	' "$MIDR_NODES_FILE"
}

# midr_locator_prefix ADDRESS / midr_locator_prefixes : print the prefix
# covering one locator, or every node locator
# (MIDR_LOCATOR_PREFIX_LEN, default /64 for IPv6 and /24 for IPv4), one per
# line, sorted and unique.  Site gateways advertise these prefixes in the
# underlay; MIDR neither imports nor exports them.
midr_locator_prefix() {
	python3 -c '
import ipaddress, sys
address, width = sys.argv[1], sys.argv[2]
width = width or ("64" if ":" in address else "24")
print(ipaddress.ip_interface(address + "/" + width).network)
' "$1" "${MIDR_LOCATOR_PREFIX_LEN:-}"
}

midr_locator_prefixes() {
	local locator
	midr_nodes_select locator | while IFS= read -r locator; do
		[ -n "$locator" ] && midr_locator_prefix "$locator"
	done | sort -u
}

# midr_node_field NAME FIELD : print one column of one node.
midr_node_field() {
	midr_nodes_select name "$2" | awk -F, -v node="$1" '$1 == node { print $2; found = 1 }
		END { exit found ? 0 : 1 }'
}

# SSH to the nodes.
#
# midr_ssh_login opens a per-host OpenSSH master connection (ControlMaster)
# kept for MIDR_SSH_PERSIST (default 15m); every later midr_ssh/midr_scp to
# that host, from any tool, reuses it and needs no further authentication.
# Login tries the public key first (BatchMode).  When the node rejects the
# key and a terminal is available, it falls back to a password prompt.  The
# password is read by ssh itself, or with MIDR_SSH_SHARED_PASSWORD=yes asked
# once per run and handed to ssh through SSH_ASKPASS; it never appears in
# argv or in a log.
#
#   MIDR_SSH_PASSWORD_FALLBACK=no   public key only (unattended runs)
#   MIDR_SSH_SHARED_PASSWORD=yes    one password for every node
#   MIDR_SSH_CONTROL_DIR            master sockets (default /tmp/midr-ssh-$UID)
#
# Close a master early with:
#   ssh -O exit -o ControlPath=/tmp/midr-ssh-$UID/%C -p PORT USER@HOST
MIDR_SSH_CONTROL_DIR=${MIDR_SSH_CONTROL_DIR:-/tmp/midr-ssh-$(id -u)}
MIDR_SSH_PERSIST=${MIDR_SSH_PERSIST:-15m}
export MIDR_SSH_CONTROL_DIR MIDR_SSH_PERSIST

midr_ssh_control_path() {
	install -d -m 700 "$MIDR_SSH_CONTROL_DIR"
	printf '%s\n' "ControlPath=$MIDR_SSH_CONTROL_DIR/%C"
}

# midr_ssh / midr_scp : use the host's master when there is one, otherwise
# connect directly with the public key.  They never start a master, so they
# are safe inside pipes and command substitutions.
midr_ssh() {
	local path
	path=$(midr_ssh_control_path)
	# shellcheck disable=SC2086
	ssh -o BatchMode=yes -o ConnectTimeout=10 -o ControlMaster=no -o "$path" \
		$MIDR_SSH_EXTRA "$@"
}

midr_scp() {
	local path
	path=$(midr_ssh_control_path)
	# shellcheck disable=SC2086
	scp -q -o BatchMode=yes -o ConnectTimeout=10 -o ControlMaster=no -o "$path" \
		$MIDR_SSH_EXTRA "$@"
}

# Ask the shared password once per run; child scripts inherit MIDR_SSH_ASKPASS.
midr_ssh_shared_password() {
	local dir password
	[ -n "${MIDR_SSH_ASKPASS:-}" ] && [ -x "$MIDR_SSH_ASKPASS" ] && return 0
	dir=$(mktemp -d "$MIDR_SSH_CONTROL_DIR/password.XXXXXX")
	IFS= read -r -s -p 'SSH password for the MIDR nodes: ' password </dev/tty
	printf '\n' >/dev/tty
	[ -n "$password" ] || { rmdir "$dir"; echo 'password must not be empty' >&2; return 1; }
	(umask 077; printf '%s\n' "$password" >"$dir/password")
	unset password
	printf '#!/bin/sh\ncat %s\n' "'$dir/password'" >"$dir/askpass"
	chmod 700 "$dir/askpass"
	MIDR_SSH_ASKPASS=$dir/askpass
	MIDR_SSH_ASKPASS_OWNER=$$
	export MIDR_SSH_ASKPASS MIDR_SSH_ASKPASS_OWNER
}

# Remove the shared password file; only the process that asked for it does.
midr_ssh_cleanup() {
	if [ -n "${MIDR_SSH_ASKPASS:-}" ] && [ "${MIDR_SSH_ASKPASS_OWNER:-}" = "$$" ]; then
		rm -rf "$(dirname -- "$MIDR_SSH_ASKPASS")"
		unset MIDR_SSH_ASKPASS MIDR_SSH_ASKPASS_OWNER
	fi
}

# midr_ssh_login PORT USER@HOST : make sure the host has an authenticated
# master, prompting for a password when public-key login is refused.
midr_ssh_login() {
	local port=$1 target=$2 path err rc
	path=$(midr_ssh_control_path)
	# shellcheck disable=SC2086
	if ssh -o "$path" $MIDR_SSH_EXTRA -O check -p "$port" "$target" 2>/dev/null; then
		return 0
	fi
	# The master forks into the background holding its stdio, so its
	# messages go to a file rather than a pipe.
	err=$(mktemp "$MIDR_SSH_CONTROL_DIR/login.XXXXXX")
	local master=(-f -N -o ControlMaster=yes -o "$path"
		-o "ControlPersist=$MIDR_SSH_PERSIST" -o ConnectTimeout=10)
	# shellcheck disable=SC2086
	if ssh "${master[@]}" -o BatchMode=yes $MIDR_SSH_EXTRA -p "$port" "$target" \
		</dev/null >/dev/null 2>"$err"; then
		rm -f "$err"
		return 0
	fi
	if ! grep -Eq 'Permission denied|Too many authentication failures' "$err"; then
		printf 'ssh %s port %s: %s\n' "$target" "$port" "$(tr '\n' ' ' <"$err")" >&2
		rm -f "$err"
		return 1
	fi
	if [ "${MIDR_SSH_PASSWORD_FALLBACK:-yes}" != yes ] || ! { : </dev/tty; } 2>/dev/null; then
		echo "public-key login to $target port $port was refused;" \
			'install the key with install-ssh-keys.sh, or run from a terminal to use a password' >&2
		rm -f "$err"
		return 1
	fi
	echo "public-key login to $target port $port was refused; falling back to a password" >&2
	master+=(-o BatchMode=no -o PubkeyAuthentication=no
		-o PreferredAuthentications=keyboard-interactive,password)
	if [ "${MIDR_SSH_SHARED_PASSWORD:-no}" = yes ]; then
		midr_ssh_shared_password || { rm -f "$err"; return 1; }
		# shellcheck disable=SC2086
		SSH_ASKPASS=$MIDR_SSH_ASKPASS SSH_ASKPASS_REQUIRE=force DISPLAY=${DISPLAY:-midr:0} \
			ssh "${master[@]}" -o NumberOfPasswordPrompts=1 $MIDR_SSH_EXTRA \
			-p "$port" "$target" </dev/null >/dev/null 2>"$err"
	else
		# ssh reads the password from the terminal itself.
		# shellcheck disable=SC2086
		ssh "${master[@]}" -o NumberOfPasswordPrompts=3 $MIDR_SSH_EXTRA \
			-p "$port" "$target" </dev/null >/dev/null 2>"$err"
	fi
	rc=$?
	if [ "$rc" -ne 0 ]; then
		printf 'password login to %s port %s failed: %s\n' "$target" "$port" \
			"$(tr '\n' ' ' <"$err")" >&2
	fi
	rm -f "$err"
	return "$rc"
}

# midr_ssh_login_nodes NAME... : log in to the named profile nodes in turn,
# so any password prompts come first and in order.
midr_ssh_login_nodes() {
	local name row host port user failed=0
	for name in "$@"; do
		row=$(midr_nodes_select name ssh_host ssh_port ssh_user | awk -F, -v n="$name" '$1 == n')
		[ -n "$row" ] || { echo "node not in $MIDR_NODES_FILE: $name" >&2; failed=1; continue; }
		IFS=, read -r _ host port user <<<"$row"
		midr_ssh_login "$port" "$user@$host" || failed=1
	done
	return "$failed"
}
