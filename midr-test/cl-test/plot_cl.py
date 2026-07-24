#!/usr/bin/env python3
"""
Plot MIDR CL (clustering) test results from the joining node's bgpd log.

Usage:
    python3 plot_cl.py logs/bgpd-newnode.log [--output cl_results.png]

Parses:
  1. "MIDR PM I-5: node=<ip>/32 ... lt_rtt_us=... lt_loss=..."
     -> long-term RTT per probed candidate (rep or member) over time
  2. "MIDR CL: REP_PROBE_DONE -> RECOMMEND ..." / "MIDR CL: MEMBER_PROBE_DONE -> JOIN/CREATE ..."
     -> decision markers

Produces a single plot: long-term RTT of every probed candidate node vs. the
MIDR_CL_JOIN_RTT_THRESHOLD_US (20 ms) line, with REP_PROBE_DONE /
MEMBER_PROBE_DONE decision instants marked. Phase 1 (REP probing) shows g1a
and g2a probed simultaneously, with g2a's RTT sitting well above the
threshold; phase 2 (MEMBER probing) shows g1a-g1e probed simultaneously, all
converging below the threshold, ending in JOIN.

Node IDs differ between phases (this test's fixed 10-node topology,
midr-test/cl-test/configs/*.conf): phase 1 probes a rep by its transport
address (e.g. g1a=10.10.11.2, g2a=10.10.21.2); phase 2 probes members by
their router-id (e.g. g1a=10.0.11.1, g1b=10.0.12.1, ...). Both IDs for the
same physical node are mapped to one label (e.g. "g1a") below so the legend
shows node names instead of IP addresses that "don't line up" across phases.

Note: lt_loss (and st_loss) in the log are already one-way loss estimates —
bgpd's probe is a round trip, so it converts the measured bidirectional
loss rate L to a one-way rate p = 1 - sqrt(1-L) (assuming both directions
are equally lossy) before logging/using it. Not plotted here (RTT only),
but relevant if this script is extended to show loss.
"""

import re
import sys
import argparse
from datetime import datetime
from collections import defaultdict

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# ── Slide-friendly font scale (matches midr-test/pm-test/plot_pm.py) ────────
plt.rcParams.update({
    "font.size": 28,
    "font.sans-serif": ["WenQuanYi Zen Hei", "Noto Sans CJK SC", "DejaVu Sans"],
    "axes.unicode_minus": False,
    "axes.titlesize": 32,
    "axes.labelsize": 30,
    "xtick.labelsize": 28,
    "ytick.labelsize": 28,
    "legend.fontsize": 24,
    "figure.titlesize": 32,
    "axes.linewidth": 1.8,
    "lines.linewidth": 2.8,
})

RTT_THRESHOLD_MS = 20.0

# Fixed topology of midr-test/cl-test (see configs/*.conf): map both the
# transport-address ID used while probing a rep (phase 1) and the router-id
# used while probing a member (phase 2) to the same node label.
IP_TO_NAME = {
    "10.10.11.2": "g1a",  # group-1 rep, probed by transport addr (phase 1)
    "10.10.21.2": "g2a",  # group-2 rep, probed by transport addr (phase 1)
    "10.10.31.2": "g3a",  # group-3 rep, probed by transport addr (phase 1)
    "10.0.11.1":  "g1a",  # group-1 member, probed by router-id (phase 2)
    "10.0.12.1":  "g1b",
    "10.0.13.1":  "g1c",
    "10.0.14.1":  "g1d",
    "10.0.15.1":  "g1e",
    "10.0.21.1":  "g2a",
    "10.0.22.1":  "g2b",
    "10.0.31.1":  "g3a",  # group-3 anchor candidate, probed by router-id (anchor phase)
    "10.0.32.1":  "g3b",
}

TS_PAT = r'(\d{4}/\d{2}/\d{2} \d{2}:\d{2}:\d{2}\.\d+)'
I5_PAT = re.compile(
    TS_PAT + r'.*MIDR PM I-5: node=([\d.]+)/\d+ status=(\d+) failures=(\d+) '
             r'st_rtt_us=(\d+) st_loss=([\d.]+) st_bw=(\d+) '
             r'lt_rtt_us=(\d+) lt_loss=([\d.]+) lt_bw=(\d+)')
REP_DONE_PAT = re.compile(
    TS_PAT + r'.*MIDR CL: REP_PROBE_DONE'
)
MEMBER_DONE_PAT = re.compile(
    TS_PAT + r'.*MIDR CL: MEMBER_PROBE_DONE'
)
RECOMMEND_PAT = re.compile(r'RECOMMEND 群 (\d+) 代表 ([\d.]+)')
JOIN_PAT = re.compile(r'JOIN 群 (\d+)（(\d+) 条好链路')
CREATE_PAT = re.compile(r'CREATE 新群 (\d+)')


def parse_ts(s):
    return datetime.strptime(s, "%Y/%m/%d %H:%M:%S.%f")


def node_label(ip):
    return IP_TO_NAME.get(ip, ip)


def parse_log(path):
    series = defaultdict(lambda: {"t": [], "lt_rtt": []})
    events = []  # (t, kind, detail_str)

    with open(path) as f:
        for line in f:
            m = I5_PAT.search(line)
            if m:
                ts = parse_ts(m.group(1))
                label = node_label(m.group(2))
                lt_rtt_ms = int(m.group(8)) / 1000.0
                series[label]["t"].append(ts)
                series[label]["lt_rtt"].append(lt_rtt_ms)
                continue

            m = REP_DONE_PAT.search(line)
            if m:
                ts = parse_ts(m.group(1))
                rec = RECOMMEND_PAT.search(line)
                detail = f"RECOMMEND 群{rec.group(1)}@{node_label(rec.group(2))}" if rec else line.strip()
                events.append((ts, "REP_PROBE_DONE", detail))
                continue

            m = MEMBER_DONE_PAT.search(line)
            if m:
                ts = parse_ts(m.group(1))
                j = JOIN_PAT.search(line)
                c = CREATE_PAT.search(line)
                if j:
                    detail = f"JOIN 群{j.group(1)}（{j.group(2)} 条好链路）"
                elif c:
                    detail = f"CREATE 新群{c.group(1)}"
                else:
                    detail = line.strip()
                events.append((ts, "MEMBER_PROBE_DONE", detail))

    return series, events


def plot(series, events, output):
    if not series:
        print("No MIDR PM/CL data found in log.")
        sys.exit(1)

    all_ts = [t for s in series.values() for t in s["t"]] + [e[0] for e in events]
    t0 = min(all_ts)

    fig, ax0 = plt.subplots(figsize=(20, 11))
    fig.suptitle("MIDR CL — 分群决策测试结果（候选节点长期 RTT）", fontsize=32, fontweight="bold")

    colors = plt.cm.tab10.colors
    for i, (label, s) in enumerate(sorted(series.items())):
        t_rel = np.array([(t - t0).total_seconds() for t in s["t"]])
        rtt = np.array(s["lt_rtt"])
        color = colors[i % len(colors)]
        ax0.plot(t_rel, rtt, color=color, label=label, marker="o", ms=6, alpha=0.9)

    ax0.axhline(RTT_THRESHOLD_MS, color="black", ls=":", lw=2.6,
                label=f"入群阈值 {RTT_THRESHOLD_MS:.0f} ms")
    ax0.set_ylabel("长期 RTT (ms)")
    ax0.set_xlabel("实验时间 (s)")
    ax0.set_title("阶段一：探测 g1a/g2a（群代表）；阶段二：探测 g1a–g1e（群成员）")
    ax0.grid(True, alpha=0.3)

    # ── decision markers ──
    marker_style = {"REP_PROBE_DONE": ("purple", "-."), "MEMBER_PROBE_DONE": ("darkgreen", "-")}
    for t, kind, detail in events:
        t_rel = (t - t0).total_seconds()
        color, ls = marker_style.get(kind, ("gray", ":"))
        ax0.axvline(t_rel, color=color, ls=ls, lw=2.6, alpha=0.85)

    ymin, ymax = ax0.get_ylim()
    for t, kind, detail in events:
        t_rel = (t - t0).total_seconds()
        color, _ = marker_style.get(kind, ("gray", ":"))
        ax0.text(t_rel, ymax * 0.95, f" {detail}", rotation=90, va="top",
                  ha="left", fontsize=18, color=color, fontweight="bold")

    ax0.legend(loc="upper left", ncol=2)

    plt.savefig(output, dpi=150, bbox_inches="tight")
    print(f"Saved: {output}")

    for label, s in sorted(series.items()):
        if s["lt_rtt"]:
            print(f"  {label}: last lt_rtt={s['lt_rtt'][-1]:.2f}ms  n_samples={len(s['lt_rtt'])}")
    for t, kind, detail in events:
        print(f"  [{kind}] {detail}")


def main():
    ap = argparse.ArgumentParser(description="Plot MIDR CL test results")
    ap.add_argument("logfile", help="joining node's bgpd log (contains MIDR PM I-5 / MIDR CL lines)")
    ap.add_argument("--output", default="cl_results.png")
    args = ap.parse_args()
    series, events = parse_log(args.logfile)
    plot(series, events, args.output)


if __name__ == "__main__":
    main()
