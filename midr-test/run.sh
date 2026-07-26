#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_DIR="$ROOT_DIR/midr-test"
RUN_DIR="${MIDR_TEST_RUN_DIR:-$TEST_DIR/run}"
VTY_DIR="${MIDR_TEST_VTY_DIR:-$RUN_DIR/vty}"
PID_FILE="${MIDR_TEST_PID_FILE:-$RUN_DIR/bgpd.pid}"
CONFIG_FILE="${MIDR_TEST_CONFIG:-$TEST_DIR/bgpd.conf}"
BGPD_BIN="${MIDR_BGPD:-$ROOT_DIR/bgpd/bgpd}"
TIMEOUT_SEC="${MIDR_TEST_TIMEOUT:-10}"
LIMIT_FDS="${MIDR_TEST_LIMIT_FDS:-100000}"

SCENARIOS=(
	smoke
	ipv6
	version
	node-withdraw
	link-withdraw
	ownership
	invalid-link
	peer-session
	ted-not-ready
	sync-status
	eor-config
	router-id-restart
)

usage() {
	cat <<EOF
Usage: $0 <scenario>

Scenarios:
  smoke           IPv4 Node and complete Link upsert
  ipv6            IPv6 transport and Link endpoints
  version         reject an older object version
  node-withdraw   separate active Node state from its tombstone
  link-withdraw   prevent an old upsert from reviving a Link tombstone
  ownership       reject non-local Node and Link ownership
  invalid-link    reject malformed Link measurements and addresses
  peer-session    basic peer session request/release wrapper
  ted-not-ready   verify production TED remains NOT_READY
  sync-status     show bounded input and Provider synchronization state
  eor-config      configure, persist and reset the MIDR EoR timeout
  router-id-restart
                  clear old identity state and queue new input behind the barrier
  all             run every assertion-based scenario
  interactive     start bgpd terminal mode for manual VTY input
  list            list assertion-based scenarios

Environment overrides:
  MIDR_TEST_CONFIG      bgpd config file, default: $CONFIG_FILE
  MIDR_BGPD             bgpd binary, default: $BGPD_BIN
  MIDR_TEST_RUN_DIR     logs and runtime files, default: $RUN_DIR
  MIDR_TEST_VTY_DIR     bgpd VTY socket directory, default: $VTY_DIR
  MIDR_TEST_PID_FILE    bgpd pid file, default: $PID_FILE
  MIDR_TEST_TIMEOUT     scenario timeout, default: $TIMEOUT_SEC
  MIDR_TEST_LIMIT_FDS   bgpd --limit-fds value, default: $LIMIT_FDS
EOF
}

list_scenarios() {
	printf '%s\n' "${SCENARIOS[@]}"
}

check_bgpd() {
	if [[ ! -x "$BGPD_BIN" ]]; then
		echo "bgpd binary not found or not executable: $BGPD_BIN" >&2
		echo "Run: make -j\$(nproc) bgpd/bgpd" >&2
		return 1
	fi
}

prepare_run_dir() {
	mkdir -p "$RUN_DIR" "$VTY_DIR" "$(dirname "$PID_FILE")"
	rm -f "$PID_FILE" "$VTY_DIR/bgpd.vty"
}

assert_log() {
	local scenario="$1"
	local log_file="$2"
	local expect_file="$TEST_DIR/expect/$scenario.expect"
	local expectation
	local needle
	local failed=0

	if [[ ! -f "$expect_file" ]]; then
		echo "Expectation file not found: $expect_file" >&2
		return 1
	fi

	while IFS= read -r expectation || [[ -n "$expectation" ]]; do
		[[ -z "$expectation" || "$expectation" == \#* ]] && continue

		if [[ "$expectation" == \!* ]]; then
			needle="${expectation:1}"
			if grep -F -- "$needle" "$log_file" >/dev/null; then
				echo "Unexpected output in $scenario: $needle" >&2
				failed=1
			fi
		elif ! grep -F -- "$expectation" "$log_file" >/dev/null; then
			echo "Missing output in $scenario: $expectation" >&2
			failed=1
		fi
	done < "$expect_file"

	if grep -E '(% Unknown command|% Ambiguous command|Segmentation fault|Assertion .* failed|core dumped|AddressSanitizer)' \
		"$log_file" >/dev/null; then
		echo "Explicit failure pattern found in $scenario" >&2
		failed=1
	fi

	if ((failed)); then
		echo "Scenario log: $log_file" >&2
		return 1
	fi
}

run_scenario() {
	local scenario="$1"
	local input_file="$TEST_DIR/vty/$scenario.cmd"
	local log_file="$RUN_DIR/$scenario.log"
	local statuses

	if [[ ! -f "$input_file" ]]; then
		echo "Scenario command file not found: $input_file" >&2
		return 1
	fi

	prepare_run_dir
	set +e
	timeout "$TIMEOUT_SEC" "$BGPD_BIN" \
		-S \
		-Z \
		-p 0 \
		-t \
		-f "$CONFIG_FILE" \
		-i "$PID_FILE" \
		--vty_socket "$VTY_DIR" \
		--log stdout \
		--limit-fds "$LIMIT_FDS" \
		< "$input_file" 2>&1 | tee "$log_file"
	statuses=("${PIPESTATUS[@]}")
	set -e

	if ((statuses[0] != 0 || statuses[1] != 0)); then
		echo "Scenario $scenario exited with bgpd=${statuses[0]} tee=${statuses[1]}" >&2
		return 1
	fi

	if ! assert_log "$scenario" "$log_file"; then
		return 1
	fi
	echo "PASS: $scenario"
}

run_all() {
	local scenario
	local failures=0

	for scenario in "${SCENARIOS[@]}"; do
		if ! run_scenario "$scenario"; then
			failures=$((failures + 1))
		fi
	done

	if ((failures)); then
		echo "MIDR scenarios failed: $failures" >&2
		return 1
	fi

	echo "PASS: all MIDR scenarios"
}

run_interactive() {
	prepare_run_dir
	exec "$BGPD_BIN" \
		-S \
		-Z \
		-p 0 \
		-t \
		-f "$CONFIG_FILE" \
		-i "$PID_FILE" \
		--vty_socket "$VTY_DIR" \
		--log stdout \
		--limit-fds "$LIMIT_FDS"
}

scenario="${1:-smoke}"

case "$scenario" in
	-h|--help|help)
		usage
		;;
	list)
		list_scenarios
		;;
	interactive)
		check_bgpd
		run_interactive
		;;
	all)
		check_bgpd
		run_all
		;;
	*)
		if ! printf '%s\n' "${SCENARIOS[@]}" | grep -Fx -- "$scenario" >/dev/null; then
			echo "Unknown MIDR test scenario: $scenario" >&2
			echo "Available scenarios:" >&2
			list_scenarios >&2
			exit 1
		fi
		check_bgpd
		run_scenario "$scenario"
		;;
esac
