#!/usr/bin/env python3
"""Create a slide-friendly summary of the main MIDR CL experiment."""

import argparse
import re
import sys
from collections import defaultdict
from datetime import datetime

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


plt.rcParams.update({
    "font.size": 22,
    "font.sans-serif": ["DejaVu Sans"],
    "axes.unicode_minus": False,
    "axes.titlesize": 27,
    "axes.labelsize": 25,
    "xtick.labelsize": 21,
    "ytick.labelsize": 22,
    "legend.fontsize": 20,
    "figure.titlesize": 30,
    "axes.linewidth": 1.8,
})

RTT_THRESHOLD_MS = 20.0
REPRESENTATIVES = ["g1b", "g3a", "g2a"]
GROUP1_MEMBERS = ["g1b", "g1c", "g1d", "g1e"]

IP_TO_NAME = {
    "10.0.11.1": "g1a",
    "10.0.12.1": "g1b",
    "10.0.13.1": "g1c",
    "10.0.14.1": "g1d",
    "10.0.15.1": "g1e",
    "10.0.21.1": "g2a",
    "10.0.22.1": "g2b",
    "10.0.31.1": "g3a",
    "10.0.32.1": "g3b",
}

TS_PAT = r"(\d{4}/\d{2}/\d{2} \d{2}:\d{2}:\d{2}\.\d+)"
I5_PAT = re.compile(
    TS_PAT
    + r".*MIDR PM I-5: node=([\d.]+)/\d+ status=(\d+) failures=(\d+) "
      r"st_rtt_us=(\d+) st_loss=([\d.]+) st_bw=(\d+) "
      r"lt_rtt_us=(\d+) lt_loss=([\d.]+) lt_bw=(\d+)"
)
REP_DONE_PAT = re.compile(TS_PAT + r".*MIDR CL: REP_PROBE_DONE")
MEMBER_DONE_PAT = re.compile(TS_PAT + r".*MIDR CL: MEMBER_PROBE_DONE")
RECOMMEND_PAT = re.compile(r"RECOMMEND 群 (\d+) 代表 ([\d.]+)")
JOIN_PAT = re.compile(r"JOIN 群 (\d+)（(\d+)(?:/(\d+))? 条好链路")
ANCHOR_CANDIDATE_PAT = re.compile(
    r"锚点候选 ([\d.]+)/32 group=(\d+).*rtt=(\d+)"
)
ANCHOR_ESTABLISHED_PAT = re.compile(
    r"MIDR 台账：([\d.]+)（原因=CL_ANCHOR）掉出 Established"
)


def parse_ts(value):
    return datetime.strptime(value, "%Y/%m/%d %H:%M:%S.%f")


def node_label(ip):
    return IP_TO_NAME.get(ip, ip)


def parse_log(path):
    samples = defaultdict(list)
    result = {
        "rep_time": None,
        "selected_group": None,
        "selected_rep": None,
        "member_time": None,
        "joined_group": None,
        "good_links": None,
        "known_members": None,
    }
    anchor_candidates = {}
    anchor_established = set()

    with open(path, encoding="utf-8") as stream:
        for line in stream:
            match = I5_PAT.search(line)
            if match:
                samples[node_label(match.group(2))].append(
                    (parse_ts(match.group(1)), int(match.group(8)) / 1000.0)
                )
                continue

            match = REP_DONE_PAT.search(line)
            if match:
                result["rep_time"] = parse_ts(match.group(1))
                recommend = RECOMMEND_PAT.search(line)
                if recommend:
                    result["selected_group"] = int(recommend.group(1))
                    result["selected_rep"] = node_label(recommend.group(2))
                continue

            match = MEMBER_DONE_PAT.search(line)
            if match:
                result["member_time"] = parse_ts(match.group(1))
                joined = JOIN_PAT.search(line)
                if joined:
                    result["joined_group"] = int(joined.group(1))
                    result["good_links"] = int(joined.group(2))
                    result["known_members"] = int(joined.group(3) or joined.group(2))
                continue

            match = ANCHOR_CANDIDATE_PAT.search(line)
            if match:
                ip = match.group(1)
                anchor_candidates[ip] = {
                    "name": node_label(ip),
                    "group": int(match.group(2)),
                    "rtt_ms": int(match.group(3)) / 1000.0,
                }
                continue

            match = ANCHOR_ESTABLISHED_PAT.search(line)
            if match:
                anchor_established.add(match.group(1))

    return samples, result, anchor_candidates, anchor_established


def latest_value(samples, label, cutoff, start=None):
    eligible = [
        value
        for timestamp, value in samples.get(label, [])
        if (cutoff is None or timestamp <= cutoff)
        and (start is None or timestamp >= start)
    ]
    return eligible[-1] if eligible else None


def add_value_labels(ax, bars, values, limit):
    for bar, value in zip(bars, values):
        long_bar = value > limit * 0.72
        ax.text(
            value - limit * 0.018 if long_bar else value + limit * 0.018,
            bar.get_y() + bar.get_height() / 2,
            f"{value:.2f} ms",
            va="center",
            ha="right" if long_bar else "left",
            fontsize=21,
            fontweight="bold",
            color="white" if long_bar else "black",
        )


def configure_rtt_axis(ax, limit):
    ax.axvline(
        RTT_THRESHOLD_MS,
        color="#c62828",
        linestyle="--",
        linewidth=2.8,
        label="Good-link threshold: 20 ms",
    )
    ax.set_xlim(0, limit)
    ax.set_xlabel("Long-term RTT (ms)")
    ax.grid(axis="x", alpha=0.25)
    ax.set_axisbelow(True)
    ax.legend(loc="upper right", frameon=True)


def summary_card(ax, x, title, body, color):
    ax.text(
        x,
        0.53,
        f"{title}\n{body}",
        transform=ax.transAxes,
        ha="center",
        va="center",
        fontsize=21,
        linespacing=1.45,
        bbox={
            "boxstyle": "round,pad=0.65",
            "facecolor": color,
            "edgecolor": "#455a64",
            "linewidth": 1.8,
            "alpha": 0.16,
        },
    )


def plot(samples, result, anchor_candidates, anchor_established, output):
    if not samples or not result["rep_time"] or not result["member_time"]:
        print("Incomplete MIDR PM/CL data in log.")
        sys.exit(1)

    rep_values = [
        latest_value(samples, label, result["rep_time"])
        for label in REPRESENTATIVES
    ]
    member_values = [
        latest_value(
            samples,
            label,
            result["member_time"],
            result["rep_time"],
        )
        for label in GROUP1_MEMBERS
    ]
    if any(value is None for value in rep_values + member_values):
        print("Missing representative or group-1 member RTT samples.")
        sys.exit(1)

    fig = plt.figure(figsize=(20, 11))
    grid = fig.add_gridspec(
        2,
        2,
        height_ratios=[4.2, 1.45],
        left=0.12,
        right=0.98,
        top=0.86,
        bottom=0.06,
        hspace=0.42,
        wspace=0.32,
    )
    rep_ax = fig.add_subplot(grid[0, 0])
    member_ax = fig.add_subplot(grid[0, 1])
    summary_ax = fig.add_subplot(grid[1, :])
    fig.suptitle("MIDR CL Decision Summary", y=0.96, fontweight="bold")

    rep_labels = ["Group 1 — g1b", "Group 3 — g3a", "Group 2 — g2a"]
    rep_colors = ["#2e7d32", "#42a5f5", "#ef5350"]
    rep_limit = max(58.0, max(rep_values) * 1.18)
    rep_bars = rep_ax.barh(rep_labels, rep_values, color=rep_colors, height=0.56)
    rep_ax.invert_yaxis()
    rep_ax.set_title("1. Representative Ranking")
    configure_rtt_axis(rep_ax, rep_limit)
    add_value_labels(rep_ax, rep_bars, rep_values, rep_limit)

    member_labels = ["g1b (rep)", "g1c", "g1d", "g1e"]
    member_limit = 24.0
    member_bars = member_ax.barh(
        member_labels,
        member_values,
        color=["#2e7d32", "#66bb6a", "#66bb6a", "#66bb6a"],
        height=0.56,
    )
    member_ax.invert_yaxis()
    member_ax.set_title("2. Group 1 Member Validation")
    configure_rtt_axis(member_ax, member_limit)
    add_value_labels(member_ax, member_bars, member_values, member_limit)

    summary_ax.axis("off")
    selected_rep = result["selected_rep"] or "unknown"
    selected_group = result["selected_group"] or "?"
    joined_group = result["joined_group"] or "?"
    good_links = result["good_links"] or 0
    known_members = result["known_members"] or 0

    anchors_by_group = defaultdict(list)
    for candidate in anchor_candidates.values():
        anchors_by_group[candidate["group"]].append(candidate["name"])
    for names in anchors_by_group.values():
        names.sort()
    established = len(set(anchor_candidates) & anchor_established)
    selected = len(anchor_candidates)
    anchor_lines = []
    for group in sorted(anchors_by_group, reverse=True):
        anchor_lines.append(f"Group {group}: {', '.join(anchors_by_group[group])}")
    anchor_lines.append(f"{established}/{selected} Established")

    summary_card(
        summary_ax,
        0.17,
        "RECOMMEND",
        f"Group {selected_group} via {selected_rep}",
        "#ce93d8",
    )
    summary_card(
        summary_ax,
        0.50,
        "JOIN",
        f"Group {joined_group}\n{good_links}/{known_members} good links",
        "#81c784",
    )
    summary_card(
        summary_ax,
        0.83,
        "CROSS-GROUP ANCHORS",
        "\n".join(anchor_lines),
        "#64b5f6",
    )

    fig.savefig(output, dpi=150)
    print(f"Saved: {output}")
    print("Representative RTT:")
    for label, value in zip(REPRESENTATIVES, rep_values):
        print(f"  {label}: {value:.2f} ms")
    print("Group 1 member RTT:")
    for label, value in zip(GROUP1_MEMBERS, member_values):
        print(f"  {label}: {value:.2f} ms")
    print(f"Decision: RECOMMEND group {selected_group}; JOIN group {joined_group}")
    print(f"Anchors: {established}/{selected} Established")


def main():
    parser = argparse.ArgumentParser(description="Plot the main MIDR CL test result")
    parser.add_argument("logfile", help="Joining node bgpd log")
    parser.add_argument("--output", default="cl_results.png")
    args = parser.parse_args()
    plot(*parse_log(args.logfile), args.output)


if __name__ == "__main__":
    main()
