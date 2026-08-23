#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that the MIDR command reference matches registered VTY syntax."""

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "bgpd" / "bgp_midr_vty.c"
REFERENCE = ROOT / "doc" / "developer" / "MIDR配置命令参考.md"
COMMAND_PATTERN = re.compile(
    r'^\s+"((?:show midr|midr |no midr |neighbor |no neighbor )[^"]+)",$',
    re.MULTILINE,
)


def main():
    source = SOURCE.read_text(encoding="utf-8")
    reference = REFERENCE.read_text(encoding="utf-8")
    commands = sorted(set(COMMAND_PATTERN.findall(source)))
    missing = [command for command in commands if command not in reference]
    if missing:
        print("MIDR command reference is missing registered syntax:", file=sys.stderr)
        for command in missing:
            print(f"  {command}", file=sys.stderr)
        return 1
    required_sections = ("正式生产配置", "只读 Show 与诊断", "仅测试注入")
    if any(section not in reference for section in required_sections):
        print("MIDR command reference is missing a command class", file=sys.stderr)
        return 1
    print(f"MIDR command reference matches {len(commands)} registered commands")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
