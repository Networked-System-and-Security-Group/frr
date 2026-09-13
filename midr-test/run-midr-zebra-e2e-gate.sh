#!/usr/bin/env bash
# Run test_midr_zebra_e2e in a dedicated privileged container with a live
# zebra. This is intentionally separate from the non-privileged component gate.
set -uo pipefail

if [ "${1:-}" = __inside ]; then
	ROOT=${2:-/home/frr/frr}
	usermod -a -G frrvty root
	mkdir -p /var/run/frr /etc/frr /var/log/frr /var/lib/frr
	printf 'frr version 10.7\nfrr defaults traditional\nhostname zebra\n' \
		>/etc/frr/zebra.conf
	setsid /usr/lib/frr/zebra -f /etc/frr/zebra.conf \
		-i /var/run/frr/zebra.pid -z /var/run/frr/zserv.api \
		-u root -g root -d >/tmp/midr-zebra-start.log 2>&1 </dev/null
	ready=no
	for _attempt in $(seq 1 50); do
		if [ -S /var/run/frr/zserv.api ]; then
			ready=yes
			break
		fi
		sleep 0.2
	done
	if [ "$ready" != yes ]; then
		printf 'ERROR: zebra socket was not created\n' >&2
		sed -n '1,200p' /tmp/midr-zebra-start.log >&2
		exit 1
	fi
	cd "$ROOT" || exit 2
	timeout "${MIDR_ZEBRA_E2E_TIMEOUT_SECONDS:-180}" \
		./tests/bgpd/test_midr_zebra_e2e
	exit $?
fi

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
MODE=${1:-image}
IMAGE=${MIDR_GATE_IMAGE:-frr-midr-p6:f9beedd0d653}
RUN_ID=${MIDR_GATE_RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}
OUT_DIR=${MIDR_GATE_OUT:-$(dirname "$REPO_ROOT")/midr-gate-runs/$RUN_ID-zebra-image}
LOG="$OUT_DIR/zebra-e2e.log"

[ "$MODE" = image ] || {
	printf 'ERROR: usage: %s [image]\n' "$0" >&2
	exit 2
}
command -v docker >/dev/null 2>&1 || {
	printf 'ERROR: docker is unavailable\n' >&2
	exit 2
}
docker info >/dev/null 2>&1 || {
	printf 'ERROR: Docker daemon is unavailable\n' >&2
	exit 2
	}
docker image inspect "$IMAGE" >/dev/null 2>&1 || {
	printf 'ERROR: image is unavailable: %s\n' "$IMAGE" >&2
	exit 2
}
mkdir -p "$OUT_DIR"

{
	printf '# MIDR privileged Zebra E2E gate\n'
	printf '# image: %s\n' "$IMAGE"
	docker image inspect "$IMAGE" \
		--format '# image-id: {{.Id}}\n# image-created: {{.Created}}'
	printf '# run-at: %s\n' "$(date --iso-8601=seconds)"
} >"$LOG"

docker run --user 0:0 --privileged --rm \
	-v "$SCRIPT_DIR:/midr-gate:ro" \
	-e "MIDR_ZEBRA_E2E_TIMEOUT_SECONDS=${MIDR_ZEBRA_E2E_TIMEOUT_SECONDS:-180}" \
	"$IMAGE" bash /midr-gate/run-midr-zebra-e2e-gate.sh \
	__inside /home/frr/frr 2>&1 | tee -a "$LOG"
run_rc=${PIPESTATUS[0]}
if [ "$run_rc" -eq 0 ] && grep -q '^=== ALL CHECKS PASSED ===$' "$LOG"; then
	printf 'ZEBRA_GATE_RESULT=0\n' | tee -a "$LOG"
else
	[ "$run_rc" -ne 0 ] || run_rc=1
	printf 'ZEBRA_GATE_RESULT=%d\n' "$run_rc" | tee -a "$LOG"
fi
printf 'Evidence: %s\n' "$OUT_DIR"
exit "$run_rc"
