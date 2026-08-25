#!/usr/bin/env python3
"""
Plot the join timeline (candidate long-term RTT + REP/MEMBER_PROBE_DONE
decisions) for both zero-config joiners (z1, z2) in the backbone-test
15-node topology, from their own bgpd logs.

Usage:
    python3 plot_backbone_join.py --z1 logs/bgpd-z1.log --z2 logs/bgpd-z2.log \
        --output backbone_join_results.png

Same parsing approach as midr-test/cl-test/plot_cl.py (phase 1 probes a rep
by its transport address, phase 2 probes members by router-id -- both IDs
for the same physical node are folded onto one legend label), adapted for
this topology's addressing (transport 10.99.0.<n>, router-id 10.0.0.<n>,
see CLAUDE.md's backbone-test section for the full n-to-name table).
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

plt.rcParams.update({
    "font.size": 26,
    "axes.titlesize": 28,
    "axes.labelsize": 26,
    "xtick.labelsize": 22,
    "ytick.labelsize": 22,
    "legend.fontsize": 20,
    "figure.titlesize": 32,
    "axes.linewidth": 1.8,
    "lines.linewidth": 2.6,
})

RTT_THRESHOLD_MS = 20.0

# n -> name (see CLAUDE.md's backbone-test addressing table)
N_TO_NAME = {
    101: "b1", 102: "b2", 103: "b3", 104: "b4", 105: "b5",
    111: "r1", 112: "m1a", 113: "m1b",
    121: "r2", 122: "m2a",
    191: "z1", 192: "z2",
}


def name_for(ip):
    """10.99.0.<n> (transport, phase 1) or 10.0.0.<n> (router-id, phase 2) -> name."""
    m = re.match(r'10\.(?:99|0)\.0\.(\d+)$', ip)
    if not m:
        return ip
    return N_TO_NAME.get(int(m.group(1)), ip)


TS_PAT = r'(\d{4}/\d{2}/\d{2} \d{2}:\d{2}:\d{2}\.\d+)'
I5_PAT = re.compile(
    TS_PAT + r'.*MIDR PM I-5: node=([\d.]+)/\d+ status=(\d+) failures=(\d+) '
             r'st_rtt_us=(\d+) st_loss=([\d.]+) st_bw=(\d+) '
             r'lt_rtt_us=(\d+) lt_loss=([\d.]+) lt_bw=(\d+)')
REP_DONE_PAT = re.compile(TS_PAT + r'.*MIDR CL: REP_PROBE_DONE')
MEMBER_DONE_PAT = re.compile(TS_PAT + r'.*MIDR CL: MEMBER_PROBE_DONE')
RECOMMEND_PAT = re.compile(r'RECOMMEND 群 (\d+) 代表 ([\d.]+)')
JOIN_PAT = re.compile(r'JOIN 群 (\d+)（(\d+)(?:/\d+)? 条好链路')
CREATE_PAT = re.compile(r'无可用群代表，CREATE 新群 (\d+)|CREATE 新群 (\d+)')
ANCHOR_PAT = re.compile(TS_PAT + r'.*MIDR I-7：ANCHOR ')


def parse_ts(s):
    return datetime.strptime(s, "%Y/%m/%d %H:%M:%S.%f")


def parse_log(path):
    series = defaultdict(lambda: {"t": [], "lt_rtt": []})
    events = []
    try:
        f = open(path)
    except FileNotFoundError:
        print(f"  (missing: {path})")
        return series, events
    with f:
        for line in f:
            m = I5_PAT.search(line)
            if m:
                ts = parse_ts(m.group(1))
                label = name_for(m.group(2))
                series[label]["t"].append(ts)
                series[label]["lt_rtt"].append(int(m.group(8)) / 1000.0)
                continue
            m = REP_DONE_PAT.search(line)
            if m:
                ts = parse_ts(m.group(1))
                rec = RECOMMEND_PAT.search(line)
                detail = f"RECOMMEND g{rec.group(1)}@{name_for(rec.group(2))}" if rec else "REP_PROBE_DONE"
                events.append((ts, "rep_done", detail))
                continue
            m = MEMBER_DONE_PAT.search(line)
            if m:
                ts = parse_ts(m.group(1))
                j = JOIN_PAT.search(line)
                c = CREATE_PAT.search(line)
                if j:
                    detail = f"JOIN g{j.group(1)} ({j.group(2)} good links)"
                elif c:
                    detail = f"CREATE g{c.group(1) or c.group(2)}"
                else:
                    detail = "MEMBER_PROBE_DONE"
                events.append((ts, "member_done", detail))
                continue
            m = ANCHOR_PAT.search(line)
            if m:
                events.append((parse_ts(m.group(1)), "anchor", "ANCHOR"))
    return series, events


MARKER_STYLE = {"rep_done": ("purple", "-."), "member_done": ("darkgreen", "-"), "anchor": ("brown", ":")}


def plot_one(ax, path, title):
    series, events = parse_log(path)
    if not series:
        ax.set_title(f"{title} (no data)")
        return False
    all_ts = [t for s in series.values() for t in s["t"]] + [e[0] for e in events]
    t0 = min(all_ts)

    colors = plt.cm.tab10.colors
    for i, (label, s) in enumerate(sorted(series.items())):
        t_rel = np.array([(t - t0).total_seconds() for t in s["t"]])
        rtt = np.array(s["lt_rtt"])
        ax.plot(t_rel, rtt, color=colors[i % len(colors)], label=label, marker="o", ms=5, alpha=0.9)

    ax.axhline(RTT_THRESHOLD_MS, color="black", ls=":", lw=2.4, label=f"threshold {RTT_THRESHOLD_MS:.0f}ms")
    ax.set_ylabel("Long-term RTT (ms)")
    ax.set_xlabel("Experiment time (s)")
    ax.set_title(title)
    ax.grid(True, alpha=0.3)

    ymin, ymax = ax.get_ylim()
    for t, kind, detail in events:
        t_rel = (t - t0).total_seconds()
        color, ls = MARKER_STYLE.get(kind, ("gray", ":"))
        ax.axvline(t_rel, color=color, ls=ls, lw=2.2, alpha=0.8)
        ax.text(t_rel, ymax * 0.95, f" {detail}", rotation=90, va="top", ha="left",
                fontsize=20, color=color, fontweight="bold")

    ax.legend(loc="upper right", ncol=2, fontsize=20)
    return True


def main():
    ap = argparse.ArgumentParser(description="Plot backbone-test join timelines for z1/z2")
    ap.add_argument("--z1", default="logs/bgpd-z1.log")
    ap.add_argument("--z2", default="logs/bgpd-z2.log")
    ap.add_argument("--output", default="backbone_join_results.png")
    args = ap.parse_args()

    fig, (ax0, ax1) = plt.subplots(2, 1, figsize=(20, 16))
    fig.suptitle("MIDR NDS/CL — Backbone-Topology Join Timeline (z1, z2)",
                 fontsize=32, fontweight="bold")
    z1_ok = plot_one(ax0, args.z1, "z1 (attached to t3, near group 2's hub)")
    z2_ok = plot_one(ax1, args.z2, "z2 (attached to t1, near group 1's hub)")

    plt.tight_layout(rect=[0, 0, 1, 0.95])
    plt.savefig(args.output, dpi=150, bbox_inches="tight")
    print(f"Saved: {args.output}")
    return 0 if z1_ok and z2_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
