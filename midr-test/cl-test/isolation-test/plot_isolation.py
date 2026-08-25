#!/usr/bin/env python3
"""
Plot MIDR NDS isolation self-healing (debounce -> ISOLATED -> RECONNECT ->
re-join) test results, from the joining node f's bgpd log.

Usage:
    python3 plot_isolation.py logs/bgpd-f.log --output isolation_results.png
"""

import re
import sys
import argparse
from datetime import datetime

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

plt.rcParams.update({
    "font.size": 26,
    "axes.titlesize": 30,
    "axes.labelsize": 28,
    "xtick.labelsize": 24,
    "ytick.labelsize": 24,
    "legend.fontsize": 22,
    "figure.titlesize": 32,
    "axes.linewidth": 1.8,
    "lines.linewidth": 2.6,
})

TS_PAT = r'(\d{4}/\d{2}/\d{2} \d{2}:\d{2}:\d{2}\.\d+)'
ZERO_SESS_PAT = re.compile(TS_PAT + r'.*MIDR: 0 个已建立会话（第 (\d+)/(\d+) 次探测')
ISOLATED_PAT = re.compile(TS_PAT + r'.*MIDR:.*判定孤岛，通知 CL')
RECONNECT_DECISION_PAT = re.compile(TS_PAT + r'.*MIDR CL: ISOLATED')
RECONNECT_EXEC_PAT = re.compile(TS_PAT + r'.*MIDR I-7：RECONNECT 群 \d+ 因失联触发')
# "加入流程结束" is the stable suffix across log-message revisions (older logs
# say "CREATE 自建群 N（...），加入流程结束（回稳态）"; current ones say
# "CREATE 落定群 N，加入流程结束（回稳态）") -- match on that instead of the
# part of the message that changed.
SETTLE_PAT = re.compile(TS_PAT + r'.*MIDR I-7：(?:CREATE|JOIN)[^\n]*加入流程结束')
JOIN_GID_PAT = re.compile(r'自建群 (\d+)|落定群 (\d+)|JOIN 群 (\d+)')


def parse_ts(s):
    return datetime.strptime(s, "%Y/%m/%d %H:%M:%S.%f")


def parse_log(path, since_line):
    events = []
    debounce = []  # (t, attempt, total)
    with open(path) as f:
        for line_number, line in enumerate(f, start=1):
            if line_number < since_line:
                continue
            m = ZERO_SESS_PAT.search(line)
            if m:
                ts = parse_ts(m.group(1))
                debounce.append((ts, int(m.group(2)), int(m.group(3))))
                continue
            m = ISOLATED_PAT.search(line)
            if m:
                events.append((parse_ts(m.group(1)), "isolated", "debounce threshold reached\n-> ISOLATED"))
                continue
            m = RECONNECT_EXEC_PAT.search(line)
            if m:
                events.append((parse_ts(m.group(1)), "reconnect", "RECONNECT executed\n(restart join flow)"))
                continue
            m = SETTLE_PAT.search(line)
            if m:
                g = JOIN_GID_PAT.search(line)
                gid = next((x for x in (g.groups() if g else ()) if x), "?")
                events.append((parse_ts(m.group(1)), "settle", f"re-settled: group {gid}"))
    return debounce, events


KIND_STYLE = {
    "isolated": ("tab:red", "X"),
    "reconnect": ("tab:orange", "^"),
    "settle": ("tab:green", "s"),
}


def plot(debounce, events, output):
    all_ts = [d[0] for d in debounce] + [e[0] for e in events]
    if not all_ts:
        print("No MIDR isolation/reconnect data found in log.")
        sys.exit(1)
    t0 = min(all_ts)

    fig, ax = plt.subplots(figsize=(20, 10))
    fig.suptitle("MIDR NDS — Isolation Self-Healing (debounce -> ISOLATED -> RECONNECT)",
                 fontsize=30, fontweight="bold")

    # Debounce counter as a step line (0 established sessions, ticking up
    # each periodic-sync pass until it crosses the threshold).
    if debounce:
        dt = [(t - t0).total_seconds() for t, _, _ in debounce]
        dc = [a for _, a, _ in debounce]
        total = debounce[0][2]
        ax.step(dt, dc, where="post", color="steelblue", label="consecutive zero-session ticks", zorder=2)
        ax.axhline(total, color="gray", ls=":", lw=2.4, label=f"debounce threshold ({total})")

    ymax = max([d[1] for d in debounce] + [1]) + 1
    for ts, kind, detail in events:
        t_rel = (ts - t0).total_seconds()
        color, marker = KIND_STYLE[kind]
        ax.axvline(t_rel, color=color, ls="--", lw=2, alpha=0.6, zorder=1)
        ax.scatter([t_rel], [ymax * 0.85], color=color, marker=marker, s=300,
                   zorder=3, edgecolors="black", linewidths=0.8)
        ax.annotate(f"{detail}\n(t={t_rel:.0f}s)", (t_rel, ymax * 0.85),
                    xytext=(10, 0), textcoords="offset points", fontsize=22,
                    color=color, fontweight="bold", va="center")

    ax.set_xlabel("Experiment time (s)")
    ax.set_ylabel("Consecutive zero-established-session ticks")
    ax.set_ylim(0, ymax * 1.15)
    ax.grid(True, alpha=0.3)
    ax.legend(loc="upper left")

    plt.tight_layout(rect=[0, 0, 1, 0.94])
    plt.savefig(output, dpi=150, bbox_inches="tight")
    print(f"Saved: {output}")
    for ts, kind, detail in events:
        print(f"  [{kind}] t={(ts - t0).total_seconds():.1f}s: {detail}")


def main():
    ap = argparse.ArgumentParser(description="Plot MIDR isolation self-healing test results")
    ap.add_argument("logfile", help="joining node f's bgpd log")
    ap.add_argument("--output", default="isolation_results.png")
    ap.add_argument("--since-line", type=int, default=1,
                    help="ignore log lines before this one")
    args = ap.parse_args()
    debounce, events = parse_log(args.logfile, args.since_line)
    plot(debounce, events, args.output)


if __name__ == "__main__":
    main()
