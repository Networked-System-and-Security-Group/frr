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

# midr_node_field NAME FIELD : print one column of one node.
midr_node_field() {
	midr_nodes_select name "$2" | awk -F, -v node="$1" '$1 == node { print $2; found = 1 }
		END { exit found ? 0 : 1 }'
}

# midr_ssh / midr_scp : connect with the profile's extra options.
midr_ssh() {
	# shellcheck disable=SC2086
	ssh -o BatchMode=yes -o ConnectTimeout=10 $MIDR_SSH_EXTRA "$@"
}

midr_scp() {
	# shellcheck disable=SC2086
	scp -q -o BatchMode=yes -o ConnectTimeout=10 $MIDR_SSH_EXTRA "$@"
}
