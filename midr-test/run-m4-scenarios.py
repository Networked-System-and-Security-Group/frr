#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run Gherkin-mapped MIDR M4 component scenarios."""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path


TEST_ROOT = Path(__file__).resolve().parent
REPO_ROOT = TEST_ROOT.parent
FEATURE = TEST_ROOT / "features" / "m4-rib-lsdb.feature"
MANIFEST = TEST_ROOT / "features" / "m4-rib-lsdb-scenarios.json"
TAG_PATTERN = re.compile(r"@(?P<id>M4-(?:RIB|LSDB)-\d{3})\b")


class TestFailure(RuntimeError):
    pass


def load_scenarios():
    feature_ids = TAG_PATTERN.findall(FEATURE.read_text(encoding="utf-8"))
    if len(feature_ids) != len(set(feature_ids)):
        raise TestFailure("duplicate M4 scenario ID in feature file")
    entries = json.loads(MANIFEST.read_text(encoding="utf-8"))
    if not isinstance(entries, list):
        raise TestFailure("scenario manifest must be a JSON list")
    scenarios = {}
    for entry in entries:
        scenario_id = entry.get("id")
        runner = entry.get("runner")
        if not TAG_PATTERN.fullmatch(f"@{scenario_id}"):
            raise TestFailure(f"invalid scenario ID: {scenario_id}")
        if scenario_id in scenarios:
            raise TestFailure(f"duplicate scenario ID: {scenario_id}")
        if not isinstance(runner, str) or not runner:
            raise TestFailure(f"{scenario_id} has no component runner")
        scenarios[scenario_id] = entry
    if set(feature_ids) != set(scenarios):
        raise TestFailure(
            "feature/manifest ID mismatch; "
            f"missing={sorted(set(feature_ids) - set(scenarios))}, "
            f"extra={sorted(set(scenarios) - set(feature_ids))}"
        )
    return scenarios


def run_scenario(entry):
    runner = REPO_ROOT / entry["runner"]
    if not runner.is_file():
        raise TestFailure(f"runner not found: {runner}")
    result = subprocess.run(
        [str(runner)],
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
    if "tests passed" not in result.stdout:
        raise TestFailure(f"{entry['id']} runner emitted no success assertion")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("scenario", nargs="?", default="all")
    args = parser.parse_args()

    try:
        scenarios = load_scenarios()
        if args.scenario == "all":
            selected = [scenarios[key] for key in sorted(scenarios)]
        else:
            key = args.scenario.removeprefix("@")
            if key not in scenarios:
                raise TestFailure(f"unknown scenario {args.scenario}")
            selected = [scenarios[key]]
        for entry in selected:
            run_scenario(entry)
            print(f"PASS @{entry['id']}: {entry['description']}")
    except (OSError, KeyError, TestFailure, json.JSONDecodeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
