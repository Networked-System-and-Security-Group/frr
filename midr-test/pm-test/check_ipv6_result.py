#!/usr/bin/env python3

import argparse
import re
import subprocess
import sys
from pathlib import Path


I5_PATTERN = re.compile(
    r"MIDR PM I-5: node=(\S+) status=(\d+) failures=(\d+) "
    r"st_rtt_us=(\d+) st_loss=([\d.]+)")
TARGET_PATTERN = re.compile(r"MIDR PM I-1: start probing \S+ -> (\S+)")


def require(condition, message, failures):
    if condition:
        print(f"PASS: {message}")
    else:
        print(f"FAIL: {message}")
        failures.append(message)


def packet_count(capture, expression):
    result = subprocess.run(
        ["tcpdump", "-nr", str(capture), expression],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    return len([line for line in result.stdout.splitlines() if line.strip()])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--case", choices=("baseline", "impairment",
                                           "invalid-source"), required=True)
    parser.add_argument("--log-a", type=Path, required=True)
    parser.add_argument("--log-b", type=Path, required=True)
    parser.add_argument("--socket-a", type=Path, required=True)
    parser.add_argument("--socket-b", type=Path, required=True)
    parser.add_argument("--capture", type=Path, required=True)
    args = parser.parse_args()

    log_a = args.log_a.read_text(errors="replace")
    log_b = args.log_b.read_text(errors="replace")
    socket_a = args.socket_a.read_text(errors="replace")
    socket_b = args.socket_b.read_text(errors="replace")
    combined = log_a + "\n" + log_b
    failures = []

    require("probe socket ready on fd00:99::a:5860" in log_a,
            "node-a binds UDP 5860 to its IPv6 transport", failures)
    require("probe socket ready on fd00:99::b:5860" in log_b,
            "node-b binds UDP 5860 to its IPv6 transport", failures)
    require("[fd00:99::a]:5860" in socket_a,
            "node-a socket table shows the exact IPv6 bind", failures)
    require("[fd00:99::b]:5860" in socket_b,
            "node-b socket table shows the exact IPv6 bind", failures)
    require("MIDR PM: reply from fd00:99::b" in log_a,
            "node-a receives IPv6 PM replies", failures)
    require("MIDR PM: reply from fd00:99::a" in log_b,
            "node-b receives IPv6 PM replies", failures)

    targets = TARGET_PATTERN.findall(combined)
    require(bool(targets) and all(":" in target for target in targets),
            "all probe contexts use IPv6 transport locators", failures)
    require(len(I5_PATTERN.findall(log_a)) >= 5
            and len(I5_PATTERN.findall(log_b)) >= 5,
            "both nodes continuously publish I-5 metrics", failures)

    ipv6_packets = packet_count(args.capture, "ip6 and udp port 5860")
    ipv4_packets = packet_count(args.capture, "ip and udp port 5860")
    require(ipv6_packets >= 10, "capture contains IPv6 PM traffic", failures)
    require(ipv4_packets == 0, "capture contains no IPv4 PM traffic", failures)

    if args.case == "impairment":
        measurements = [
            (int(status), int(count), int(rtt), float(loss))
            for _, status, count, rtt, loss in I5_PATTERN.findall(combined)
        ]
        require(any(count >= 3 for _, count, _, _ in measurements),
                "three consecutive timeouts are reported", failures)
        require("entered fast-probe mode" in combined,
                "PM enters FAST mode", failures)
        require("returning to normal probe rate" in combined,
                "PM returns to NORMAL after recovery", failures)
        require(any(loss > 0 for _, _, _, loss in measurements),
                "I-5 reports non-zero loss during impairment", failures)

    if args.case == "invalid-source":
        require("packet from unknown transport fd00:dead::1, dropped" in log_b,
                "unknown IPv6 source is rejected", failures)
        require("bad magic 0xdeadbeef from fd00:99::a, dropped" in log_b,
                "bad PM magic is rejected", failures)
        require(("seqno mismatch from fd00:99::a" in log_b
                 or "late/duplicate reply from fd00:99::a" in log_b),
                "unexpected reply cannot update a probe context", failures)

    if failures:
        print(f"\n{len(failures)} assertion(s) failed.")
        return 1

    print("\nAll IPv6 PM assertions passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
