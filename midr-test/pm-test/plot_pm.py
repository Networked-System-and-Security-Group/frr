#!/usr/bin/env python3
"""
Plot MIDR PM metrics from bgpd debug log.

Usage:
    python3 plot_pm.py /tmp/bgpd-a.log [--output pm_results.png]

Parses two log patterns:
  1. "MIDR PM: reply from X rtt=Yus"        -> raw RTT per probe
  2. "MIDR PM I-5: node=... st_rtt_us=..."  -> EWMA metrics per I-5 push
"""

import re
import sys
import argparse
from datetime import datetime

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec

TS_PAT  = r'(\d{4}/\d{2}/\d{2} \d{2}:\d{2}:\d{2}\.\d+)'
RTT_PAT = re.compile(
    TS_PAT + r'.*MIDR PM: reply from [\d.]+ rtt=(\d+)us')
I5_PAT  = re.compile(
    TS_PAT + r'.*MIDR PM I-5: node=([\d./]+) status=(\d+) failures=(\d+) '
             r'st_rtt_us=(\d+) st_loss=([\d.]+) st_bw=(\d+) '
             r'lt_rtt_us=(\d+) lt_loss=([\d.]+) lt_bw=(\d+)')

def parse_ts(s):
    return datetime.strptime(s, "%Y/%m/%d %H:%M:%S.%f")

def parse_log(path):
    raw_ts, raw_rtt = [], []
    i5_ts = []
    i5_st_rtt, i5_st_loss, i5_st_bw = [], [], []
    i5_lt_rtt, i5_lt_loss, i5_lt_bw = [], [], []
    i5_status, i5_failures = [], []

    with open(path) as f:
        for line in f:
            m = RTT_PAT.search(line)
            if m:
                raw_ts.append(parse_ts(m.group(1)))
                raw_rtt.append(int(m.group(2)) / 1000.0)
                continue
            m = I5_PAT.search(line)
            if m:
                i5_ts.append(parse_ts(m.group(1)))
                # groups: 1=ts 2=node 3=status 4=failures
                #         5=st_rtt_us 6=st_loss 7=st_bw
                #         8=lt_rtt_us 9=lt_loss 10=lt_bw
                i5_status.append(int(m.group(3)))
                i5_failures.append(int(m.group(4)))
                i5_st_rtt.append(int(m.group(5)) / 1000.0)
                i5_st_loss.append(float(m.group(6)) * 100.0)
                i5_st_bw.append(int(m.group(7)))
                i5_lt_rtt.append(int(m.group(8)) / 1000.0)
                i5_lt_loss.append(float(m.group(9)) * 100.0)
                i5_lt_bw.append(int(m.group(10)))

    return {
        "raw_ts": raw_ts, "raw_rtt": raw_rtt,
        "i5_ts": i5_ts,
        "i5_st_rtt": i5_st_rtt, "i5_st_loss": i5_st_loss, "i5_st_bw": i5_st_bw,
        "i5_lt_rtt": i5_lt_rtt, "i5_lt_loss": i5_lt_loss, "i5_lt_bw": i5_lt_bw,
        "i5_status": i5_status, "i5_failures": i5_failures,
    }

def relative_secs(ts_list, t0):
    return [(t - t0).total_seconds() for t in ts_list]

def plot(data, output):
    if not data["raw_ts"] and not data["i5_ts"]:
        print("No MIDR PM data found — check that 'debug bgp midr' is in the config.")
        sys.exit(1)

    t0 = min(
        (data["raw_ts"][0]  if data["raw_ts"]  else data["i5_ts"][0]),
        (data["i5_ts"][0]   if data["i5_ts"]   else data["raw_ts"][0]),
    )

    raw_t   = np.array(relative_secs(data["raw_ts"], t0))
    i5_t    = np.array(relative_secs(data["i5_ts"],  t0))
    raw_rtt = np.array(data["raw_rtt"])

    fig = plt.figure(figsize=(14, 10))
    fig.suptitle("MIDR PM — Link Performance Metrics", fontsize=14, fontweight="bold")
    gs = gridspec.GridSpec(3, 2, figure=fig, hspace=0.45, wspace=0.35)

    ax0 = fig.add_subplot(gs[0, :])
    ax0.scatter(raw_t, raw_rtt, s=4, alpha=0.5, color="steelblue", label="Raw RTT")
    if len(i5_t):
        ax0.plot(i5_t, data["i5_st_rtt"], color="orange", lw=1.5,
                 label="Short-term EWMA (α=0.2)")
        ax0.plot(i5_t, data["i5_lt_rtt"], color="red", lw=1.5, ls="--",
                 label="Long-term EWMA (α=0.05)")
    ax0.set_ylabel("RTT (ms)")
    ax0.set_xlabel("Experiment time (s)")
    ax0.set_title("Round-Trip Time")
    ax0.legend(loc="upper right", fontsize=8)
    ax0.grid(True, alpha=0.3)

    ax1 = fig.add_subplot(gs[1, 0])
    if len(i5_t):
        ax1.plot(i5_t, data["i5_st_loss"], color="orange", lw=1.5,
                 label="Short-term (sliding window)")
        ax1.plot(i5_t, data["i5_lt_loss"], color="red", lw=1.5, ls="--",
                 label="Long-term EWMA")
    ax1.set_ylabel("Loss rate (%)")
    ax1.set_xlabel("Experiment time (s)")
    ax1.set_title("Packet Loss Rate")
    ax1.legend(fontsize=8)
    ax1.grid(True, alpha=0.3)

    ax2 = fig.add_subplot(gs[1, 1])
    if len(i5_t):
        ax2.plot(i5_t, data["i5_st_bw"], color="green", lw=1.5,
                 label="Short-term bw_score")
        ax2.plot(i5_t, data["i5_lt_bw"], color="darkgreen", lw=1.5, ls="--",
                 label="Long-term bw_score")
    ax2.set_ylabel("bw_score (arb.)")
    ax2.set_xlabel("Experiment time (s)")
    ax2.set_title("Bandwidth Score  [√1.5 / (rtt_s · √loss)]")
    ax2.legend(fontsize=8)
    ax2.grid(True, alpha=0.3)

    ax3 = fig.add_subplot(gs[2, 0])
    if len(i5_t):
        ax3.step(i5_t, data["i5_failures"], color="red", where="post", lw=1.2)
        ax3.axhline(3, color="gray", ls=":", lw=1, label="Fast-mode threshold")
    ax3.set_ylabel("Consecutive failures")
    ax3.set_xlabel("Experiment time (s)")
    ax3.set_title("Probe Failures")
    ax3.legend(fontsize=8)
    ax3.grid(True, alpha=0.3)

    ax4 = fig.add_subplot(gs[2, 1])
    status_colors = {0: "green", 1: "orange", 2: "red"}
    status_labels = {0: "UP", 1: "DEGRADED", 2: "DOWN"}
    if len(i5_t) and data["i5_status"]:
        prev_status = data["i5_status"][0]
        seg_start = i5_t[0]
        for i, (t, s) in enumerate(zip(i5_t, data["i5_status"])):
            if s != prev_status or i == len(i5_t) - 1:
                ax4.axvspan(seg_start, t,
                            color=status_colors.get(prev_status, "gray"),
                            alpha=0.4,
                            label=status_labels.get(prev_status, "?"))
                seg_start = t
                prev_status = s
    ax4.set_ylabel("Link status")
    ax4.set_xlabel("Experiment time (s)")
    ax4.set_title("Link Status Timeline")
    ax4.set_yticks([])
    handles, labels = ax4.get_legend_handles_labels()
    by_label = dict(zip(labels, handles))
    ax4.legend(by_label.values(), by_label.keys(), fontsize=8)
    ax4.grid(True, alpha=0.3)

    plt.savefig(output, dpi=150, bbox_inches="tight")
    print(f"Saved: {output}")

    if raw_rtt.size:
        print(f"\nRaw RTT  — n={len(raw_rtt)}  min={raw_rtt.min():.1f}ms  "
              f"median={np.median(raw_rtt):.1f}ms  p99={np.percentile(raw_rtt,99):.1f}ms")
    if data["i5_st_loss"]:
        sl = np.array(data["i5_st_loss"])
        print(f"Loss rate — mean={sl.mean():.2f}%  max={sl.max():.2f}%")
    if data["i5_st_bw"]:
        bw = np.array(data["i5_st_bw"])
        print(f"bw_score  — mean={bw.mean():.0f}  min={bw.min():.0f}")

def main():
    ap = argparse.ArgumentParser(description="Plot MIDR PM metrics from bgpd debug log")
    ap.add_argument("logfile", help="bgpd log file (contains MIDR PM lines)")
    ap.add_argument("--output", default="pm_results.png")
    args = ap.parse_args()
    data = parse_log(args.logfile)
    plot(data, args.output)

if __name__ == "__main__":
    main()
