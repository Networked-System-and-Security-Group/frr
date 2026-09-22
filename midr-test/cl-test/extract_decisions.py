#!/usr/bin/env python3

import argparse
import json
import re
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--address-family", required=True,
                        choices=("ipv4", "ipv6"))
    return parser.parse_args()


def extract(log_text):
    decisions = []
    anchor_evidence = []

    for line in log_text.splitlines():
        match = re.search(
            r"REP_PROBE_DONE .*RECOMMEND .*? (\d+) .*? "
            r"([0-9]+(?:\.[0-9]+){3})", line)
        if match:
            decisions.append({
                "type": "RECOMMEND",
                "new_group_id": int(match.group(1)),
                "recommended_node_id": match.group(2),
            })
            continue

        match = re.search(
            r"MIDR CL: I-7 ANCHOR evidence\[(\d+)\]="
            r"([0-9]+(?:\.[0-9]+){3})", line)
        if match:
            index = int(match.group(1))
            if index != len(anchor_evidence):
                raise ValueError("non-contiguous ANCHOR evidence indexes")
            anchor_evidence.append(match.group(2))
            continue

        match = re.search(r"MEMBER_PROBE_DONE .*JOIN .*? (\d+)", line)
        if match:
            decisions.append({
                "type": "JOIN",
                "new_group_id": int(match.group(1)),
            })
            continue

        match = re.search(r"(?:REP|MEMBER)_PROBE_DONE .*CREATE .*? (\d+)", line)
        if match:
            decisions.append({
                "type": "CREATE",
                "new_group_id": int(match.group(1)),
            })
            continue

        if "ANCHOR_PROBE_DONE" in line and "ANCHOR" in line:
            decisions.append({
                "type": "ANCHOR",
                "evidence": anchor_evidence,
            })
            anchor_evidence = []
            continue

        match = re.search(r"PERIODIC_SYNC .*? (\d+) .*LEAVE", line)
        if match:
            decisions.append({
                "type": "LEAVE",
                "old_group_id": int(match.group(1)),
                "new_group_id": 0,
            })
            continue

        match = re.search(r"ISOLATED .*? (\d+) .*RECONNECT", line)
        if match:
            decisions.append({
                "type": "RECONNECT",
                "old_group_id": int(match.group(1)),
                "new_group_id": 0,
            })

    return decisions


def main():
    args = parse_args()
    decisions = extract(args.log.read_text(encoding="utf-8", errors="replace"))
    if not decisions:
        raise SystemExit("No CL decisions found in the log")

    result = {
        "address_family": args.address_family,
        "decisions": decisions,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"[extract-decisions] saved {len(decisions)} decisions to {args.output}")


if __name__ == "__main__":
    main()
