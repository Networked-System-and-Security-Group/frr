#!/usr/bin/env python3
"""Create a slide-friendly summary of both backbone NDS join flows."""

import argparse
import re
from collections import defaultdict
from datetime import datetime

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


plt.rcParams.update({
    "font.size": 22,
    "font.sans-serif": ["DejaVu Sans"],
    "axes.titlesize": 26,
    "axes.labelsize": 23,
    "xtick.labelsize": 20,
    "ytick.labelsize": 21,
    "legend.fontsize": 20,
    "figure.titlesize": 30,
    "axes.linewidth": 1.8,
})

RTT_THRESHOLD_MS = 20.0
REPRESENTATIVES = ["r1", "r2"]
GROUP1_MEMBERS = ["r1", "m1a", "m1b"]

N_TO_NAME = {
    101: "b1", 102: "b2", 103: "b3", 104: "b4", 105: "b5",
    111: "r1", 112: "m1a", 113: "m1b",
    121: "r2", 122: "m2a",
    191: "z1", 192: "z2",
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
ANCHOR_DECISION_PAT = re.compile(r"MIDR I-7：ANCHOR ")
ANCHOR_ESTABLISHED_PAT = re.compile(
    r"MIDR 台账：([\d.]+)（原因=CL_ANCHOR）掉出 Established"
)
LIVE_ANCHOR_PAT = re.compile(
    r"PASS: (z[12]) has (\d+) Established anchor session"
)


def parse_ts(value):
    return datetime.strptime(value, "%Y/%m/%d %H:%M:%S.%f")


def node_name(ip):
    match = re.fullmatch(r"10\.(?:99|0)\.0\.(\d+)", ip)
    return N_TO_NAME.get(int(match.group(1)), ip) if match else ip


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
        "anchor_decision": False,
    }
    anchor_candidates = {}
    anchor_established = set()

    try:
        stream = open(path, encoding="utf-8")
    except OSError as error:
        return samples, result, anchor_candidates, anchor_established, str(error)

    with stream:
        for line in stream:
            match = I5_PAT.search(line)
            if match:
                samples[node_name(match.group(2))].append(
                    (parse_ts(match.group(1)), int(match.group(8)) / 1000.0)
                )
                continue

            match = REP_DONE_PAT.search(line)
            if match:
                result["rep_time"] = parse_ts(match.group(1))
                recommendation = RECOMMEND_PAT.search(line)
                if recommendation:
                    result["selected_group"] = int(recommendation.group(1))
                    result["selected_rep"] = node_name(recommendation.group(2))
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
                anchor_candidates[node_name(match.group(1))] = {
                    "name": node_name(match.group(1)),
                    "group": int(match.group(2)),
                    "rtt_ms": int(match.group(3)) / 1000.0,
                }
                continue

            if ANCHOR_DECISION_PAT.search(line):
                result["anchor_decision"] = True
                continue

            match = ANCHOR_ESTABLISHED_PAT.search(line)
            if match:
                anchor_established.add(node_name(match.group(1)))

    return samples, result, anchor_candidates, anchor_established, None


def parse_live_anchor_counts(path):
    counts = {}
    try:
        stream = open(path, encoding="utf-8")
    except OSError:
        return counts
    with stream:
        for line in stream:
            match = LIVE_ANCHOR_PAT.search(line)
            if match:
                counts[match.group(1)] = int(match.group(2))
    return counts


def latest_value(samples, label, cutoff, start=None):
    eligible = [
        value
        for timestamp, value in samples.get(label, [])
        if cutoff is not None
        and timestamp <= cutoff
        and (start is None or timestamp >= start)
    ]
    return eligible[-1] if eligible else None


def add_value_labels(ax, bars, values, limit):
    for bar, value in zip(bars, values):
        inside = value > limit * 0.72
        ax.text(
            value - limit * 0.018 if inside else value + limit * 0.018,
            bar.get_y() + bar.get_height() / 2,
            f"{value:.2f} ms",
            va="center",
            ha="right" if inside else "left",
            fontsize=20,
            fontweight="bold",
            color="white" if inside else "black",
        )


def configure_axis(ax, limit):
    ax.axvline(
        RTT_THRESHOLD_MS,
        color="#c62828",
        linestyle="--",
        linewidth=2.6,
        label="Good-link threshold: 20 ms",
    )
    ax.set_xlim(0, limit)
    ax.set_xlabel("Long-term RTT (ms)")
    ax.grid(axis="x", alpha=0.25)
    ax.set_axisbelow(True)
    ax.legend(loc="upper right", frameon=True)


def draw_bars(ax, labels, values, colors, title, minimum_limit):
    if any(value is None for value in values):
        ax.axis("off")
        ax.text(0.5, 0.5, f"{title}\nIncomplete RTT data", ha="center", va="center",
                fontsize=24, color="#b71c1c", fontweight="bold")
        return False

    limit = max(minimum_limit, max(values) * 1.18)
    bars = ax.barh(labels, values, color=colors, height=0.56)
    ax.invert_yaxis()
    ax.set_title(title, fontweight="bold")
    configure_axis(ax, limit)
    add_value_labels(ax, bars, values, limit)
    return True


def summary_card(ax, x, node, result, candidates, established, live_count,
                 parse_error):
    selected = len(candidates)
    connected = live_count if live_count is not None else len(
        set(candidates) & established
    )
    valid = (
        parse_error is None
        and result["selected_group"] == 1
        and result["selected_rep"] == "r1"
        and result["joined_group"] == 1
        and result["good_links"] == 3
        and result["known_members"] == 3
        and result["anchor_decision"]
        and selected == 2
        and connected == 2
    )
    if parse_error:
        lines = ["LOG ERROR", parse_error]
    else:
        lines = [
            f"RECOMMEND  Group {result['selected_group'] or '?'} via {result['selected_rep'] or '?'}",
            f"JOIN  Group {result['joined_group'] or '?'} — "
            f"{result['good_links'] or 0}/{result['known_members'] or 0} good links",
            f"ANCHOR  Group 2 — {connected}/{selected} Established",
            "PASS" if valid else "INCOMPLETE / FAIL",
        ]
    ax.text(
        x,
        0.50,
        f"{node}\n" + "\n".join(lines),
        transform=ax.transAxes,
        ha="center",
        va="center",
        fontsize=20,
        linespacing=1.45,
        fontweight="bold" if valid else "normal",
        bbox={
            "boxstyle": "round,pad=0.65",
            "facecolor": "#66bb6a" if valid else "#ef9a9a",
            "edgecolor": "#455a64",
            "linewidth": 1.8,
            "alpha": 0.18,
        },
    )
    return valid


def main():
    parser = argparse.ArgumentParser(description="Plot backbone NDS join results")
    parser.add_argument("--z1", default="logs/bgpd-z1.log")
    parser.add_argument("--z2", default="logs/bgpd-z2.log")
    parser.add_argument("--result-log", default="run.log")
    parser.add_argument("--output", default="backbone_join_results.png")
    args = parser.parse_args()

    parsed = {"z1": parse_log(args.z1), "z2": parse_log(args.z2)}
    live_anchor_counts = parse_live_anchor_counts(args.result_log)

    fig = plt.figure(figsize=(20, 14))
    grid = fig.add_gridspec(
        3,
        2,
        height_ratios=[3.2, 3.6, 2.4],
        left=0.11,
        right=0.98,
        top=0.89,
        bottom=0.05,
        hspace=0.60,
        wspace=0.34,
    )
    fig.suptitle("MIDR NDS Zero-Configuration Join Summary", y=0.965,
                 fontweight="bold")

    plot_ok = True
    for column, node in enumerate(("z1", "z2")):
        samples, result, _candidates, _established, error = parsed[node]
        rep_values = [
            latest_value(samples, label, result["rep_time"])
            for label in REPRESENTATIVES
        ]
        member_values = [
            latest_value(samples, label, result["member_time"], result["rep_time"])
            for label in GROUP1_MEMBERS
        ]

        rep_ax = fig.add_subplot(grid[0, column])
        member_ax = fig.add_subplot(grid[1, column])
        plot_ok &= draw_bars(
            rep_ax,
            ["Group 1 — r1", "Group 2 — r2"],
            rep_values,
            ["#2e7d32", "#ef5350"],
            f"{node}: Representative Ranking",
            58.0,
        )
        plot_ok &= draw_bars(
            member_ax,
            ["r1 (rep)", "m1a", "m1b"],
            member_values,
            ["#2e7d32", "#66bb6a", "#66bb6a"],
            f"{node}: Group 1 Member Validation",
            24.0,
        )
        plot_ok &= error is None

    summary_ax = fig.add_subplot(grid[2, :])
    summary_ax.axis("off")
    summary_ok = True
    for x, node in ((0.25, "z1"), (0.75, "z2")):
        _samples, result, candidates, established, error = parsed[node]
        summary_ok &= summary_card(
            summary_ax, x, node, result, candidates, established,
            live_anchor_counts.get(node), error
        )

    fig.text(
        0.5,
        0.015,
        "Both zero-config nodes discover the directory, select Group 1, validate all members, and establish Group 2 anchors.",
        ha="center",
        fontsize=20,
        color="#37474f",
    )
    plt.savefig(args.output, dpi=150, bbox_inches="tight")
    print(f"Saved: {args.output}")
    return 0 if plot_ok and summary_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
