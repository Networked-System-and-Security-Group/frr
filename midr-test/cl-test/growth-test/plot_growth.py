#!/usr/bin/env python3
"""Plot the adaptive JOIN threshold observed by sequential joiners."""

import argparse
import re
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


plt.rcParams.update({
    "font.size": 24,
    "axes.titlesize": 28,
    "axes.labelsize": 26,
    "xtick.labelsize": 24,
    "ytick.labelsize": 24,
    "legend.fontsize": 22,
    "figure.titlesize": 30,
    "axes.linewidth": 1.8,
})

JOIN_PATTERN = re.compile(
    r"MIDR CL: MEMBER_PROBE_DONE .* JOIN .*"
    r"(\d+)/(\d+).*good links|"
    r"MIDR CL: MEMBER_PROBE_DONE .* JOIN .*"
    r"（(\d+)/(\d+) 条好链路，认识 (\d+) 个成员）"
)


def parse_join(path):
    with open(path, encoding="utf-8", errors="replace") as stream:
        for line in stream:
            match = JOIN_PATTERN.search(line)
            if not match:
                continue
            if match.group(1):
                good = int(match.group(1))
                threshold = int(match.group(2))
                return good, threshold, threshold
            return int(match.group(3)), int(match.group(4)), int(match.group(5))
    return None


def main():
    parser = argparse.ArgumentParser(description="Plot MIDR organic group growth")
    parser.add_argument("--log-dir", default="logs")
    parser.add_argument("--output", default="growth_results.png")
    args = parser.parse_args()

    joiners = ["j1", "j2", "j3"]
    rows = [parse_join(f"{args.log_dir}/bgpd-{node}.log") for node in joiners]
    if any(row is None for row in rows):
        missing = [node for node, row in zip(joiners, rows) if row is None]
        print(f"Missing JOIN evidence for: {', '.join(missing)}", file=sys.stderr)
        return 1

    good = np.array([row[0] for row in rows])
    threshold = np.array([row[1] for row in rows])
    known = np.array([row[2] for row in rows])
    positions = np.arange(len(joiners))
    width = 0.24

    fig, ax = plt.subplots(figsize=(18, 10))
    fig.suptitle("MIDR CL — Organic Group Growth with an Adaptive JOIN Threshold",
                 fontweight="bold")
    bars_known = ax.bar(positions - width, known, width, label="Known members",
                        color="steelblue")
    bars_threshold = ax.bar(positions, threshold, width, label="JOIN threshold",
                            color="darkorange")
    bars_good = ax.bar(positions + width, good, width, label="Good links",
                       color="seagreen")

    ax.axhline(5, color="firebrick", linestyle="--", linewidth=2.5,
               label="Old fixed threshold (5)")
    ax.set_xticks(positions, ["j1 joins", "j2 joins", "j3 joins"])
    ax.set_ylabel("Node / link count")
    ax.set_xlabel("Sequential join event")
    ax.set_ylim(0, 6.2)
    ax.grid(axis="y", alpha=0.3)
    ax.legend(loc="upper left", ncol=2)

    for bars in (bars_known, bars_threshold, bars_good):
        ax.bar_label(bars, fontsize=22, padding=5)
    for position, value in zip(positions, good):
        ax.text(position, value + 0.7, "JOIN", ha="center", va="bottom",
                fontsize=22, color="darkgreen", fontweight="bold")

    plt.tight_layout(rect=[0, 0, 1, 0.94])
    plt.savefig(args.output, dpi=180, bbox_inches="tight")
    print(f"Saved: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
