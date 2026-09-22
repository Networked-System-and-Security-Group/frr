#!/usr/bin/env python3

import argparse
import json
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--left", required=True, type=Path)
    parser.add_argument("--right", required=True, type=Path)
    return parser.parse_args()


def load_decisions(path):
    payload = json.loads(path.read_text(encoding="utf-8"))
    decisions = payload.get("decisions")
    if not isinstance(decisions, list) or not decisions:
        raise ValueError(f"{path} has no decisions")
    return decisions


def main():
    args = parse_args()
    left = load_decisions(args.left)
    right = load_decisions(args.right)
    if left != right:
        print("FAIL: CL decision sequences differ")
        print(json.dumps({"left": left, "right": right}, indent=2))
        raise SystemExit(1)
    print(f"PASS: {len(left)} CL decisions are address-family equivalent")


if __name__ == "__main__":
    main()
