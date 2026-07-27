#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run Gherkin-mapped MIDR input/resync component scenarios."""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path


TEST_ROOT = Path(__file__).resolve().parent
REPO_ROOT = TEST_ROOT.parent
FEATURE = TEST_ROOT / "features" / "m2-input.feature"
MANIFEST = TEST_ROOT / "features" / "m2-input-scenarios.json"
DEFAULT_C_RUNNER = REPO_ROOT / "tests" / "bgpd" / "test_midr_resync"
TAG_PATTERN = re.compile(r"@(?P<id>M2-INPUT-\d{3})\b")


class TestFailure(RuntimeError):
    pass


def feature_ids():
    ids = TAG_PATTERN.findall(FEATURE.read_text(encoding="utf-8"))
    if len(ids) != len(set(ids)):
        raise TestFailure("duplicate @M2-INPUT scenario ID in feature file")
    return set(ids)


def load_manifest():
    entries = json.loads(MANIFEST.read_text(encoding="utf-8"))
    if not isinstance(entries, list):
        raise TestFailure("scenario manifest must be a JSON list")
    by_id = {}
    for entry in entries:
        scenario_id = entry.get("id")
        if not TAG_PATTERN.fullmatch(f"@{scenario_id}"):
            raise TestFailure(f"invalid scenario ID in manifest: {scenario_id}")
        if scenario_id in by_id:
            raise TestFailure(f"duplicate scenario ID in manifest: {scenario_id}")
        by_id[scenario_id] = entry
    ids = feature_ids()
    if ids != set(by_id):
        raise TestFailure(
            "feature/manifest ID mismatch; "
            f"missing={sorted(ids - set(by_id))}, extra={sorted(set(by_id) - ids)}"
        )
    return by_id


def runner_ids(c_runner):
    result = subprocess.run(
        [str(c_runner), "--list"],
        cwd=REPO_ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode:
        raise TestFailure(f"C runner --list failed: {result.stderr.strip()}")
    return {line.strip() for line in result.stdout.splitlines() if line.strip()}


def run_scenario(entry, c_runner):
    result = subprocess.run(
        [str(c_runner), entry["id"]],
        cwd=REPO_ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode:
        raise TestFailure(
            f"{entry['id']} failed: {(result.stderr or result.stdout).strip()}"
        )
    expected = f"PASS @{entry['id']}"
    if expected not in result.stdout:
        raise TestFailure(f"{entry['id']} did not emit {expected!r}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "scenario", nargs="?", default="all", help="all or an ID such as M2-INPUT-001"
    )
    parser.add_argument("--c-runner", type=Path, default=DEFAULT_C_RUNNER)
    args = parser.parse_args()

    try:
        scenarios = load_manifest()
        if not args.c_runner.is_file():
            raise TestFailure(f"C runner not found: {args.c_runner}")
        implemented = runner_ids(args.c_runner)
        if implemented != set(scenarios):
            raise TestFailure(
                "manifest/C runner ID mismatch; "
                f"missing={sorted(set(scenarios) - implemented)}, "
                f"extra={sorted(implemented - set(scenarios))}"
            )

        if args.scenario == "all":
            selected = [scenarios[key] for key in sorted(scenarios)]
        else:
            key = args.scenario.removeprefix("@")
            if key not in scenarios:
                raise TestFailure(f"unknown scenario {args.scenario}")
            selected = [scenarios[key]]

        for entry in selected:
            run_scenario(entry, args.c_runner)
            print(f"PASS @{entry['id']}: {entry['description']}")
    except (OSError, KeyError, TestFailure, json.JSONDecodeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
