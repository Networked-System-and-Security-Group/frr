#!/usr/bin/env python3
"""Validate MIDR PM baseline and background-load experiment logs."""

import argparse
import statistics
import sys

from plot_pm import parse_log


def fail(message):
    print(f"FAIL: {message}", file=sys.stderr)
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logfile")
    parser.add_argument("--iperf", action="store_true")
    args = parser.parse_args()

    data = parse_log(args.logfile)
    raw_rtt = data["raw_rtt"]
    statuses = data["i5_status"]
    ok = True

    if len(raw_rtt) < 20:
        ok = fail(f"only {len(raw_rtt)} successful PM probes were recorded") and ok
    if len(statuses) < 20:
        ok = fail(f"only {len(statuses)} I-5 updates were recorded") and ok

    if raw_rtt:
        median_rtt = statistics.median(raw_rtt)
        upper_bound = 1000.0 if args.iperf else 80.0
        if not 30.0 <= median_rtt <= upper_bound:
            ok = fail(
                f"median RTT {median_rtt:.1f} ms is outside 30.0-{upper_bound:.1f} ms"
            ) and ok

    if statuses and 2 in statuses[-5:]:
        ok = fail("the link remained DOWN at the end of the experiment") and ok

    if args.iperf:
        markers = data["iperf_markers"]
        if "start" not in markers or "end" not in markers:
            ok = fail("the complete iperf START/END window was not recorded") and ok
        elif data["i5_ts"]:
            before = sum(t < markers["start"] for t in data["i5_ts"])
            during = sum(markers["start"] <= t <= markers["end"] for t in data["i5_ts"])
            after = sum(t > markers["end"] for t in data["i5_ts"])
            if min(before, during, after) < 5:
                ok = fail(
                    "fewer than five I-5 samples were recorded in a load-test window "
                    f"(before={before}, during={during}, after={after})"
                ) and ok
    elif data["i5_st_loss"]:
        tail = data["i5_st_loss"][-min(20, len(data["i5_st_loss"])):]
        mean_tail_loss = statistics.mean(tail)
        if mean_tail_loss > 15.0:
            ok = fail(
                f"steady one-way loss {mean_tail_loss:.1f}% exceeds the 15.0% limit"
            ) and ok

    if not ok:
        return 1

    mode = "iperf background-load" if args.iperf else "baseline"
    median_text = f", median RTT={statistics.median(raw_rtt):.1f} ms" if raw_rtt else ""
    print(f"PASS: MIDR PM {mode}, probes={len(raw_rtt)}, I-5 updates={len(statuses)}{median_text}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
