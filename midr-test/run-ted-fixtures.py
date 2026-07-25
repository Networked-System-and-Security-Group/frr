#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run Gherkin-mapped MIDR TED fixtures through the test-only provider."""

import argparse
import difflib
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path


TEST_ROOT = Path(__file__).resolve().parent
REPO_ROOT = TEST_ROOT.parent
FEATURE = TEST_ROOT / "features" / "m1-ted.feature"
MANIFEST = TEST_ROOT / "features" / "m1-ted-scenarios.json"
NORMALIZER = TEST_ROOT / "ted-fixture-normalize.py"
DEFAULT_C_RUNNER = REPO_ROOT / "tests" / "bgpd" / "test_midr_ted_fixture"
TAG_PATTERN = re.compile(r"@(?P<id>M1-TED-\d{3})\b")


class TestFailure(RuntimeError):
    pass


def feature_ids():
    text = FEATURE.read_text(encoding="utf-8")
    ids = TAG_PATTERN.findall(text)
    if len(ids) != len(set(ids)):
        raise TestFailure("duplicate @M1-TED scenario ID in feature file")
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
    if feature_ids() != set(by_id):
        missing = sorted(feature_ids() - set(by_id))
        extra = sorted(set(by_id) - feature_ids())
        raise TestFailure(
            f"feature/manifest ID mismatch; missing={missing}, extra={extra}"
        )
    return by_id


def run_command(command):
    return subprocess.run(
        command,
        cwd=REPO_ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def assert_valid(entry, c_runner):
    fixture = TEST_ROOT / "path-fixtures" / entry["fixture"]
    expected_path = TEST_ROOT / "path-fixtures" / "expected" / entry["expected"]
    with tempfile.TemporaryDirectory(prefix="midr-ted-") as directory:
        normalized = Path(directory) / "fixture.json"
        result = run_command(
            [sys.executable, str(NORMALIZER), str(fixture), "-o", str(normalized)]
        )
        if result.returncode:
            raise TestFailure(
                f"normalizer rejected {fixture.name}: {result.stderr.strip()}"
            )
        result = run_command([str(c_runner), str(normalized)])
        if result.returncode:
            raise TestFailure(
                f"C provider rejected {fixture.name}: {result.stderr.strip()}"
            )
        try:
            actual = json.loads(result.stdout)
            expected = json.loads(expected_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise TestFailure(f"invalid expected/actual JSON: {error}") from error
        if actual != expected:
            actual_text = json.dumps(actual, indent=2, sort_keys=True).splitlines()
            expected_text = json.dumps(
                expected, indent=2, sort_keys=True
            ).splitlines()
            difference = "\n".join(
                difflib.unified_diff(
                    expected_text,
                    actual_text,
                    fromfile=f"expected/{entry['expected']}",
                    tofile=f"actual/{entry['fixture']}",
                    lineterm="",
                )
            )
            raise TestFailure(f"snapshot mismatch:\n{difference}")


def assert_invalid(entry):
    fixture = TEST_ROOT / "path-fixtures" / "invalid" / entry["fixture"]
    result = run_command([sys.executable, str(NORMALIZER), str(fixture)])
    if result.returncode == 0:
        raise TestFailure(f"normalizer accepted invalid fixture {fixture.name}")
    expected_error = entry["error_contains"]
    if expected_error not in result.stderr:
        raise TestFailure(
            f"{fixture.name} error did not contain {expected_error!r}: "
            f"{result.stderr.strip()}"
        )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "scenario",
        nargs="?",
        default="all",
        help="all or an ID such as M1-TED-001",
    )
    parser.add_argument("--c-runner", type=Path, default=DEFAULT_C_RUNNER)
    args = parser.parse_args()

    try:
        scenarios = load_manifest()
        if args.scenario == "all":
            selected = [scenarios[key] for key in sorted(scenarios)]
        else:
            key = args.scenario.removeprefix("@")
            if key not in scenarios:
                raise TestFailure(f"unknown scenario {args.scenario}")
            selected = [scenarios[key]]
        if any(entry["kind"] == "valid" for entry in selected):
            if not args.c_runner.is_file():
                raise TestFailure(f"C fixture runner not found: {args.c_runner}")

        for entry in selected:
            if entry["kind"] == "valid":
                assert_valid(entry, args.c_runner)
            elif entry["kind"] == "invalid":
                assert_invalid(entry)
            else:
                raise TestFailure(
                    f"{entry['id']} has unknown kind {entry['kind']!r}"
                )
            print(f"PASS @{entry['id']}: {entry['description']}")
    except (OSError, KeyError, TestFailure) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
