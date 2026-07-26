#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate and run Gherkin-mapped MIDR M6 and M7 scenarios."""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
TEST_ROOT = ROOT / "midr-test"
SUITES = (
    (
        TEST_ROOT / "features" / "m6-prefix.feature",
        TEST_ROOT / "features" / "m6-prefix-scenarios.json",
    ),
    (
        TEST_ROOT / "features" / "m7-ted.feature",
        TEST_ROOT / "features" / "m7-ted-scenarios.json",
    ),
)
TAG_PATTERN = re.compile(r"@(?P<id>M[67]-(?:PFX|TED)-\d{3})\b")


class TestFailure(RuntimeError):
    pass


def load_scenarios():
    scenarios = {}
    for feature, manifest in SUITES:
        feature_ids = TAG_PATTERN.findall(feature.read_text(encoding="utf-8"))
        entries = json.loads(manifest.read_text(encoding="utf-8"))
        if len(feature_ids) != len(set(feature_ids)):
            raise TestFailure(f"duplicate scenario ID in {feature.name}")
        if not isinstance(entries, list):
            raise TestFailure(f"{manifest.name} must contain a JSON list")
        manifest_ids = set()
        for entry in entries:
            scenario_id = entry.get("id")
            command = entry.get("command")
            if not TAG_PATTERN.fullmatch(f"@{scenario_id}"):
                raise TestFailure(f"invalid scenario ID: {scenario_id}")
            if scenario_id in scenarios:
                raise TestFailure(f"duplicate scenario ID: {scenario_id}")
            if not isinstance(command, list) or not command or not all(
                isinstance(item, str) and item for item in command
            ):
                raise TestFailure(f"{scenario_id} has no executable command")
            if not isinstance(entry.get("expect"), str) or not entry["expect"]:
                raise TestFailure(f"{scenario_id} has no success assertion")
            scenarios[scenario_id] = entry
            manifest_ids.add(scenario_id)
        if set(feature_ids) != manifest_ids:
            raise TestFailure(
                f"{feature.name} and {manifest.name} scenario IDs differ"
            )
    return scenarios


def run_entry(entry, cache):
    command = tuple(entry["command"])
    if command not in cache:
        executable = ROOT / command[0]
        result = subprocess.run(
            [str(executable), *command[1:]],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        cache[command] = result
    result = cache[command]
    if result.returncode:
        detail = (result.stderr or result.stdout).strip()
        raise TestFailure(f"{entry['id']} failed: {detail}")
    if entry["expect"] not in result.stdout:
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
        cache = {}
        for entry in selected:
            run_entry(entry, cache)
            print(f"PASS @{entry['id']}: {entry['description']}")
    except (OSError, KeyError, TestFailure, json.JSONDecodeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
