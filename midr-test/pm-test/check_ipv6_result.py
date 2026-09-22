#!/usr/bin/env python3

import argparse
import csv
import ipaddress
import re
import subprocess
import sys
from datetime import datetime
from pathlib import Path
from statistics import median


I5_PATTERN = re.compile(
    r"MIDR PM I-5: node=(\S+) status=(\d+) failures=(\d+) "
    r"st_rtt_us=(\d+) st_loss=([\d.]+)")
TARGET_PATTERN = re.compile(r"MIDR PM I-1: start probing \S+ -> (\S+)")
RTT_PATTERN = re.compile(
    r"(\d{4}/\d{2}/\d{2} \d{2}:\d{2}:\d{2}\.\d+).*"
    r"MIDR PM: reply from \S+ rtt=(\d+)us")


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


def parse_timestamp(value):
    return datetime.strptime(value, "%Y/%m/%d %H:%M:%S.%f")


def read_events(path):
    with path.open(newline="") as event_file:
        return {
            row["event"]: parse_timestamp(row["timestamp"])
            for row in csv.DictReader(event_file)
        }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--address-family", choices=("ipv4", "ipv6"),
                        required=True)
    parser.add_argument("--case", choices=("baseline", "impairment",
                                           "invalid-source"), required=True)
    parser.add_argument("--log-a", type=Path, required=True)
    parser.add_argument("--log-b", type=Path, required=True)
    parser.add_argument("--socket-a", type=Path, required=True)
    parser.add_argument("--socket-b", type=Path, required=True)
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--events", type=Path, required=True)
    parser.add_argument("--transport-a", required=True)
    parser.add_argument("--transport-b", required=True)
    parser.add_argument("--unknown-source", required=True)
    parser.add_argument("--delay-ms", type=float, default=30.0)
    args = parser.parse_args()

    log_a = args.log_a.read_text(errors="replace")
    log_b = args.log_b.read_text(errors="replace")
    socket_a = args.socket_a.read_text(errors="replace")
    socket_b = args.socket_b.read_text(errors="replace")
    combined = log_a + "\n" + log_b
    failures = []
    expected_version = 4 if args.address_family == "ipv4" else 6
    other_family = "ipv6" if args.address_family == "ipv4" else "ipv4"
    selected_filter = "ip" if args.address_family == "ipv4" else "ip6"
    other_filter = "ip6" if args.address_family == "ipv4" else "ip"

    require(f"probe socket ready on {args.transport_a}:5860" in log_a,
            f"node-a binds UDP 5860 to its {args.address_family} transport",
            failures)
    require(f"probe socket ready on {args.transport_b}:5860" in log_b,
            f"node-b binds UDP 5860 to its {args.address_family} transport",
            failures)
    require(args.transport_a in socket_a and ":5860" in socket_a,
            f"node-a socket table shows the exact {args.address_family} bind",
            failures)
    require(args.transport_b in socket_b and ":5860" in socket_b,
            f"node-b socket table shows the exact {args.address_family} bind",
            failures)
    require(f"MIDR PM: reply from {args.transport_b}" in log_a,
            f"node-a receives {args.address_family} PM replies", failures)
    require(f"MIDR PM: reply from {args.transport_a}" in log_b,
            f"node-b receives {args.address_family} PM replies", failures)

    targets = TARGET_PATTERN.findall(combined)
    require(bool(targets) and all(
        ipaddress.ip_address(target).version == expected_version
        for target in targets),
        f"all probe contexts use {args.address_family} transport locators",
        failures)
    require(len(I5_PATTERN.findall(log_a)) >= 5
            and len(I5_PATTERN.findall(log_b)) >= 5,
            "both nodes continuously publish I-5 metrics", failures)

    selected_packets = packet_count(
        args.capture, f"{selected_filter} and udp port 5860")
    other_packets = packet_count(
        args.capture, f"{other_filter} and udp port 5860")
    require(selected_packets >= 10,
            f"capture contains {args.address_family} PM traffic", failures)
    require(other_packets == 0,
            f"capture contains no {other_family} PM traffic", failures)

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

        events = read_events(args.events)
        rtts = [
            (parse_timestamp(timestamp), int(rtt_us) / 1000.0)
            for timestamp, rtt_us in RTT_PATTERN.findall(combined)
        ]
        baseline_rtts = [
            rtt for timestamp, rtt in rtts
            if events["baseline-start"] <= timestamp
            < events["forced-loss-start"]
        ]
        impaired_rtts = [
            rtt for timestamp, rtt in rtts
            if events["impairment-start"] <= timestamp
            < events["recovery-start"]
        ]
        recovered_rtts = [
            rtt for timestamp, rtt in rtts
            if timestamp >= events["recovery-start"]
        ]
        require(bool(baseline_rtts) and bool(impaired_rtts)
                and median(impaired_rtts) >= median(baseline_rtts)
                + args.delay_ms,
                "RTT rises during the delayed phase", failures)
        require(bool(impaired_rtts) and bool(recovered_rtts)
                and median(recovered_rtts) + args.delay_ms
                <= median(impaired_rtts),
                "RTT returns near baseline after recovery", failures)

    if args.case == "invalid-source":
        require((f"packet from unknown transport {args.unknown_source}, "
                 "dropped") in log_b,
                f"unknown {args.address_family} source is rejected", failures)
        require((f"bad magic 0xdeadbeef from {args.transport_a}, "
                 "dropped") in log_b,
                "bad PM magic is rejected", failures)
        require((f"invalid packet type 9 from {args.transport_a}, "
                 "dropped") in log_b,
                "unknown PM packet type is rejected", failures)
        require((f"{args.transport_a}:5861 has invalid source port") in log_b,
                "unexpected PM source port is rejected", failures)
        require((f"invalid packet length (8 B) from {args.transport_a}, "
                 "dropped") in log_b,
                "invalid PM packet length is rejected", failures)
        require((f"seqno mismatch from {args.transport_a}" in log_b
                 or f"late/duplicate reply from {args.transport_a}" in log_b),
                "unexpected reply cannot update a probe context", failures)

    if failures:
        print(f"\n{len(failures)} assertion(s) failed.")
        return 1

    print(f"\nAll {args.address_family} PM assertions passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
