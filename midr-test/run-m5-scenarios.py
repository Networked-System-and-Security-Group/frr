#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate and run Gherkin-mapped MIDR M5 propagation scenarios."""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path


TEST_ROOT = Path(__file__).resolve().parent
FEATURE = TEST_ROOT / "features" / "m5-propagation.feature"
MANIFEST = TEST_ROOT / "features" / "m5-propagation-scenarios.json"
RUNNER = TEST_ROOT / "run-m5-multinode.sh"
TAG_PATTERN = re.compile(r"@(?P<id>M5-PROP-\d{3})\b")


class TestFailure(RuntimeError):
    pass


def load_scenarios():
    feature_ids = TAG_PATTERN.findall(FEATURE.read_text(encoding="utf-8"))
    entries = json.loads(MANIFEST.read_text(encoding="utf-8"))
    if len(feature_ids) != len(set(feature_ids)):
        raise TestFailure("duplicate M5 scenario ID in feature file")
    if not isinstance(entries, list):
        raise TestFailure("scenario manifest must be a JSON list")

    scenarios = {}
    for entry in entries:
        scenario_id = entry.get("id")
        scenario = entry.get("scenario")
        if not TAG_PATTERN.fullmatch(f"@{scenario_id}"):
            raise TestFailure(f"invalid scenario ID: {scenario_id}")
        if scenario_id in scenarios:
            raise TestFailure(f"duplicate scenario ID: {scenario_id}")
        if not isinstance(scenario, str) or not scenario:
            raise TestFailure(f"{scenario_id} has no shell scenario")
        scenarios[scenario_id] = entry
    if set(feature_ids) != set(scenarios):
        raise TestFailure("feature and manifest scenario IDs differ")
    return scenarios


def run_scenario(entry):
    result = subprocess.run(
        [str(RUNNER), entry["scenario"]],
        cwd=TEST_ROOT.parent,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode:
        detail = (result.stderr or result.stdout).strip()
        raise TestFailure(f"{entry['id']} failed: {detail}")
    if f"PASS: MIDR M5 rootless {entry['scenario']}" not in result.stdout:
        raise TestFailure(f"{entry['id']} emitted no success assertion")


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
