#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile unchanged CL sources with test-only dependencies and run assertions."""

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
MODULES = {
    "midrd": ROOT / "midrd/group1/midr_cl.c",
    "bgpd": ROOT / "bgpd/bgp_midr_cl.c",
}
# Only these dependencies are replaced. New dependencies require an explicit
# harness update, rather than silently becoming empty headers.
SHIM_HEADERS = (
    "zebra.h", "log.h", "memory.h", "prefix.h", "linklist.h", "monotime.h",
    "midrd/group1/midr_g1.h", "midrd/group1/midr_nds.h",
    "midrd/group1/midr_admission.h", "midrd/group1/midr_cl.h",
    "bgpd/bgpd.h", "bgpd/bgp_midr_nds.h",
    "bgpd/bgp_midr_admission.h", "bgpd/bgp_midr_cl.h",
)


def function_source(source, name):
    """Read a complete column-zero C function, with no body rewriting."""
    match = re.search(r"(?m)^(?:static )?(?:bool|void|struct midr_rep_entry \*)\s*"
                      + re.escape(name) + r"\s*\([^;{]*\)\s*\n\{", source)
    if not match:
        raise ValueError(f"Missing function: {name}")
    end = source.index("\n}", match.end()) + 2
    body = source[match.start():end]
    if "{" not in body or ");" in body.split("{", 1)[0]:
        raise ValueError(f"Expected function definition: {name}")
    return body


def nds_excerpts(module):
    """Compile the real RECOMMEND consumer and request timer dependencies.

    The outer switch wrapper replaces unrelated JOIN/LEAVE/ANCHOR consumers;
    the selected case and function bodies are copied verbatim from production.
    """
    directory, prefix = (ROOT / "midrd/group1", "midr_") if module == "midrd" else (
        ROOT / "bgpd", "bgp_midr_")
    nds = (directory / (prefix + "nds.c")).read_text(encoding="utf-8")
    ctrl = (directory / (prefix + "ctrl.c")).read_text(encoding="utf-8")
    start = nds.index("\tcase MIDR_DECISION_RECOMMEND: {")
    end = nds.index("\tcase MIDR_DECISION_JOIN:", start)
    owner, state = ("midr_g1", "midr_nds") if module == "midrd" else ("bgp", "bgp_midr_nds")
    arg = "g1" if module == "midrd" else "bgp"
    wrapper = (f"static void test_apply_recommend(struct {owner} *{arg},\n"
               "    const struct midr_cluster_decision *decision)\n{\n"
               f"    struct {state} *mi = {arg}->midr_nds_info;\n"
               "    switch (decision->decision_type) {\n" + nds[start:end] +
               "    default: assert(false);\n    }\n}\n")
    return "\n\n".join((
        function_source(ctrl, "midr_ctrl_member_probe_done_cb"),
        function_source(ctrl, "midr_ctrl_arm_member_probe_done"),
        function_source(ctrl, "midr_ctrl_in_join_batch"),
        function_source(ctrl, "midr_ctrl_send_member_request"),
        function_source(nds, "midr_rep_dir_find_group"),
        function_source(nds, "midr_rep_identity_resolve"),
        function_source(nds, "midr_join_round_start"),
        wrapper,
    ))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=("all", "baseline", "regression"),
                        default="all")
    parser.add_argument("--module", choices=("both", *MODULES), default="both")
    parser.add_argument("--cc", default=os.environ.get("CC"),
                        help="C compiler executable or path (default: CC/cc/gcc/clang)")
    args = parser.parse_args()
    compiler = args.cc or next((shutil.which(c) for c in ("cc", "gcc", "clang")
                               if shutil.which(c)), None)
    if not compiler:
        parser.error("No C compiler found; set --cc or CC to gcc/clang.")

    modules = MODULES if args.module == "both" else {args.module: MODULES[args.module]}
    before = {name: hashlib.sha256(path.read_bytes()).hexdigest()
              for name, path in modules.items()}
    failed = False
    # Keep every generated header, translation unit and binary out of the source
    # tree. No configure/make, daemon, network namespace or root is needed.
    with tempfile.TemporaryDirectory(prefix="midr-cl-fallback-") as temp:
        build = Path(temp)
        for header in SHIM_HEADERS:
            target = build / header
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("/* Test dependency; declarations are in cl_test_support.h. */\n",
                              encoding="utf-8")
        for name, source in modules.items():
            print(f"\n=== {name}: {source.relative_to(ROOT).as_posix()} ===", flush=True)
            unit = build / f"{name}_test.c"
            excerpt = build / f"{name}_nds_excerpts.h"
            excerpt.write_text(nds_excerpts(name), encoding="utf-8")
            unit.write_text(
                f'#define CL_TEST_{name.upper()} 1\n'
                f'#include "{(HERE / "cl_test_support.h").as_posix()}"\n'
                f'#include "{source.as_posix()}"\n'
                f'#include "{excerpt.as_posix()}"\n'
                f'#include "{(HERE / "test_candidate_fallback.c").as_posix()}"\n',
                encoding="utf-8")
            binary = build / (name + "_cl_test" + (".exe" if os.name == "nt" else ""))
            command = [compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                       "-I", str(build), str(unit), "-o", str(binary)]
            if os.name == "nt":
                command.append("-lws2_32")
            compilation = subprocess.run(command, check=False)
            if compilation.returncode:
                return 2  # A build error is not a reproduced regression.
            result = subprocess.run([str(binary), args.suite], check=False)
            if result.returncode not in (0, 1):
                print(f"ERROR: test process exited unexpectedly: {result.returncode}", flush=True)
                return 2
            failed |= result.returncode == 1

    for name, source in modules.items():
        if hashlib.sha256(source.read_bytes()).hexdigest() != before[name]:
            raise RuntimeError(f"Production source changed during test: {source}")
    print("\nProduction CL source hashes unchanged.", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
