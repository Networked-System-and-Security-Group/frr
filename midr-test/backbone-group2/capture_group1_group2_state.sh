#!/usr/bin/env bash
# Capture reviewable group1-to-group2 state from the 15-node containerlab.

set -uo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
LAB_PREFIX=${MIDR_LAB_PREFIX:-clab-midr-backbone}
RUN_ROOT=${MIDR_LAB_RUN_ROOT:-/tmp/midr-group1-group2}
PHASE=${1:-manual}
SAFE_PHASE=$(printf '%s' "$PHASE" | tr -c 'A-Za-z0-9._-' '-')
RUN_ID=${MIDR_EVIDENCE_RUN_ID:-$(date +%Y%m%d-%H%M%S)}
EVIDENCE_ROOT=${MIDR_EVIDENCE_ROOT:-"$RUN_ROOT/evidence"}
PHASE_DIR="$EVIDENCE_ROOT/$RUN_ID/$SAFE_PHASE"
VERIFY="$SCRIPT_DIR/verify_group1_group2_evidence.py"

MEMBERS="r1 r2 m1a m1b m2a z1 z2"
BOOTSTRAPS="b1 b2 b3 b4 b5"
TRANSIT="t1 t2 t3"
ALL_NODES="$TRANSIT $BOOTSTRAPS $MEMBERS"

COMMANDS=(
	"midr-self|show midr self"
	"midr-join|show midr join"
	"midr-nodes|show midr nodes"
	"midr-neighbors|show midr neighbors"
	"midr-reps|show midr reps"
	"midr-bootstraps|show midr bootstraps"
	"group2-interface|show midr group2"
	"group2-provider-snapshot|show midr group2-snapshot"
	"group2-remote-view|show midr group2-remote"
	"input-sync|show midr topology sync"
	"local-node-facts|show midr topology nodes"
	"local-link-facts|show midr topology links"
	"local-tombstones|show midr topology tombstones"
	"input-events|show midr events"
	"eor-sync|show midr sync"
	"owned-objects|show midr owned"
	"rib-summary|show midr rib summary"
	"rib-paths|show midr rib paths"
	"lsdb-summary|show midr lsdb summary"
	"lsdb-objects|show midr lsdb objects"
	"ted-summary|show midr ted summary"
	"ted-detail|show midr ted detail"
	"spf-summary|show midr spf summary"
	"spf-routes|show midr spf routes"
	"prefix-summary|show midr prefix summary"
	"prefix-contributors|show midr prefix contributors"
)

container()
{
	printf '%s-%s' "$LAB_PREFIX" "$1"
}

capture_node()
{
	local node=$1
	local c node_dir report spec slug command status

	c=$(container "$node")
	node_dir="$PHASE_DIR/nodes/$node"
	report="$node_dir/full-report.txt"
	mkdir -p "$node_dir"

	if ! docker inspect "$c" >/dev/null 2>&1; then
		printf 'container=%s\nstatus=missing\n' "$c" >"$node_dir/container.txt"
		return
	fi

	{
		printf 'node=%s\n' "$node"
		printf 'container=%s\n' "$c"
		docker inspect -f 'running={{.State.Running}} started={{.State.StartedAt}} image={{.Config.Image}}' "$c"
	} >"$node_dir/container.txt" 2>&1

	: >"$report"
	for spec in "${COMMANDS[@]}"; do
		IFS='|' read -r slug command <<<"$spec"
		{
			printf '===== %s =====\n' "$command"
			printf 'file: %s.txt\n\n' "$slug"
		} >>"$report"
		if timeout 30 docker exec "$c" vtysh -c "$command" \
			>"$node_dir/$slug.txt" 2>"$node_dir/$slug.stderr"; then
			status=0
		else
			status=$?
		fi
		grep -Ev \
			"^% Can't open configuration file /etc/frr/vtysh.conf|^Configuration file\[/etc/frr/frr.conf\] processing failure: 11|^profiling:.*\.gcda:Cannot open$" \
			"$node_dir/$slug.stderr" >"$node_dir/$slug.stderr.filtered" || true
		mv "$node_dir/$slug.stderr.filtered" "$node_dir/$slug.stderr"
		cat "$node_dir/$slug.txt" >>"$report"
		if [ -s "$node_dir/$slug.stderr" ]; then
			printf '\n[stderr]\n' >>"$report"
			cat "$node_dir/$slug.stderr" >>"$report"
		else
			rm -f "$node_dir/$slug.stderr"
		fi
		[ "$status" -eq 0 ] || printf '\n[exit-status=%d]\n' "$status" >>"$report"
		printf '\n' >>"$report"
	done

	docker logs "$c" >"$node_dir/container.log" 2>&1 || true
	docker exec "$c" sh -c 'cat /etc/frr/logs/frr.log 2>/dev/null' \
		>"$node_dir/frr.log" 2>/dev/null || true
	grep -E \
		'MIDR|MP_REACH|MP_UNREACH|snapshot|Snapshot|RIB|LSDB|TED|owned|withdraw|UPDATE' \
		"$node_dir/frr.log" >"$node_dir/midr-events.log" 2>/dev/null || true
}

if ! command -v docker >/dev/null 2>&1; then
	echo 'docker is unavailable' >&2
	exit 2
fi

rm -rf "$PHASE_DIR"
mkdir -p "$PHASE_DIR/nodes"
{
	printf 'run_id=%s\n' "$RUN_ID"
	printf 'phase=%s\n' "$PHASE"
	printf 'captured_at=%s\n' "$(date --iso-8601=seconds)"
	printf 'hostname=%s\n' "$(hostname)"
	printf 'lab_prefix=%s\n' "$LAB_PREFIX"
	printf 'members=%s\n' "$MEMBERS"
	printf 'bootstraps=%s\n' "$BOOTSTRAPS"
	printf 'transit=%s\n' "$TRANSIT"
} >"$PHASE_DIR/metadata.txt"

cat >"$PHASE_DIR/README.txt" <<'EOF'
MIDR group1-to-group2 evidence

summary.tsv              One row per MIDR member with cross-layer counts.
summary.md               Compact table suitable for review or recording.
semantic-digests.tsv     Normalized object-set counts and SHA-256 digests.
verification.log         Automated interface, propagation, LSDB and TED checks.
nodes/<node>/*.txt       One read-only VTY command per file.
nodes/<node>/full-report.txt
                         All VTY outputs for convenient browsing or recording.
nodes/<node>/frr.log     Complete FRR daemon log captured from the container.
nodes/<node>/midr-events.log
                         MIDR-related daemon log lines when logging is enabled.

The evidence contains no path-computation or route-installation assertions.
EOF

pids=()
for node in $ALL_NODES; do
	capture_node "$node" &
	pids+=("$!")
done
for pid in "${pids[@]}"; do
	wait "$pid" || true
done

captured=$(find "$PHASE_DIR/nodes" -mindepth 1 -maxdepth 1 -type d | wc -l)
printf 'captured_nodes=%s\n' "$captured" >>"$PHASE_DIR/metadata.txt"
printf 'Captured %s node(s).\n' "$captured"
printf 'EVIDENCE_DIR=%s\n' "$PHASE_DIR"

if [ "${MIDR_EVIDENCE_SKIP_VERIFY:-0}" = 1 ]; then
	exit 0
fi
if [ ! -f "$VERIFY" ]; then
	echo "Verifier is missing: $VERIFY" >&2
	exit 2
fi

python3 "$VERIFY" "$PHASE_DIR" 2>&1 | tee "$PHASE_DIR/verification.log"
exit "${PIPESTATUS[0]}"
