#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build real FRR modules and run related tests in a disposable Linux copy."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def main():
    if os.name != "posix":
        raise SystemExit("Run this optional validation in Linux/WSL.")
    output = HERE / "artifacts" / "linux-validation"
    output.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix="midr-cl-validation-"))
    source = stage / "source"
    steps = [
        ("bootstrap", ["sh", "bootstrap.sh"]),
        ("configure", ["./configure", "--disable-doc", "--disable-snmp",
                       "--disable-rpki", "--disable-zeromq"]),
        ("build", ["make", f"-j{min(os.cpu_count() or 2, 4)}", "midrd/midrd",
                   "bgpd/bgpd", "midrd/group1/test_midr_group1",
                   "tests/bgpd/test_midr_admission"]),
        ("group1-linked", ["bash", "midrd/group1/verify-group1-linked.sh", "midrd/midrd"]),
        ("group1-test", ["midrd/group1/test_midr_group1"]),
        ("admission-test", ["tests/bgpd/test_midr_admission"]),
        ("candidate-test", ["python3", "midr-test/cl-test/candidate-fallback/run_test.py"]),
    ]
    try:
        print("Copying sources into a disposable Linux build directory...", flush=True)
        shutil.copytree(ROOT, source, ignore=shutil.ignore_patterns(
            ".git", ".codex", ".agents", ".aws", "artifacts", "__pycache__", ".deps", ".libs"))
        # Windows export line endings only need normalization in build scripts.
        for path in source.rglob("*"):
            if path.is_file() and path.suffix in (".sh", ".am", ".ac", ".m4", ".py"):
                data = path.read_bytes()
                if b"\r\n" in data:
                    path.write_bytes(data.replace(b"\r\n", b"\n"))
            # Exported Windows timestamps can be ahead of the Linux clock.
            if path.is_file():
                os.utime(path, None)
        with (output / "summary.txt").open("w", encoding="utf-8") as summary:
            for name, command in steps:
                print(f"Running {name}...", flush=True)
                log = output / f"{name}.log"
                with log.open("w", encoding="utf-8") as stream:
                    result = subprocess.run(command, cwd=source, stdout=stream,
                                            stderr=subprocess.STDOUT, check=False)
                summary.write(f"{name}: exit={result.returncode}\n")
                summary.flush()
                if result.returncode:
                    print(f"FAIL {name}; last diagnostic lines:", flush=True)
                    print("\n".join(log.read_text(encoding="utf-8", errors="replace").splitlines()[-45:]),
                          flush=True)
                    return result.returncode
                print(f"PASS {name}", flush=True)
        return 0
    finally:
        # Deletion stays within the exact temporary directory created above.
        if stage.resolve().parent != Path(tempfile.gettempdir()).resolve():
            raise RuntimeError("Unexpected temporary build path")
        shutil.rmtree(stage)


if __name__ == "__main__":
    raise SystemExit(main())
