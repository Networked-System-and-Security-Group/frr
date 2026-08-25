#!/usr/bin/env python3
"""Plot discovery-chain criteria and RTT-to-JOIN timelines."""

import argparse
import re
from datetime import datetime
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


plt.rcParams.update({
    "font.size": 21,
    "axes.titlesize": 25,
    "axes.labelsize": 23,
    "xtick.labelsize": 20,
    "ytick.labelsize": 20,
    "legend.fontsize": 18,
    "figure.titlesize": 29,
    "axes.linewidth": 1.8,
})

CRITERIA = [
    ("9-a", "Shutdown guard"),
    ("9-b", "No dead session"),
    ("9-c", "Expiry cleanup"),
    ("1-a", "Peering-back path"),
    ("1-b", "Same-group ledger"),
    ("2-a", "Probe startup"),
    ("2-b", "No false LEAVE"),
    ("3", "Nudge brake"),
    ("4-a", "r1 attachments"),
    ("4-b", "r2 attachments"),
    ("4-c", "z1 anchors"),
    ("7-a", "Overlay session"),
    ("7-b", "Underlay session"),
    ("8", "LS-layer separation"),
    ("S1", "Group-1 full mesh"),
    ("S2-a", "Dead-end cleanup"),
    ("S2-b", "Fallback recovery"),
    ("S3", "Stable fallback"),
]

EVENTS = [
    ("Callback ready", "已向第二组注册 node/link 回调", "#4c78a8", "o"),
    ("Representative decision", "MIDR CL: REP_PROBE_DONE", "#f58518", "s"),
    ("Member decision", "MIDR CL: MEMBER_PROBE_DONE", "#e45756", "D"),
    ("JOIN complete", "MIDR I-7：JOIN 群", "#2a9d55", "*"),
    ("Anchor evaluation", "MIDR I-7：ANCHOR", "#7950a3", "P"),
]


def parse_status(path):
    status = {key: "NO SAMPLE" for key, _ in CRITERIA}
    aliases = {"补1": "S1", "补2-a": "S2-a", "补2-b": "S2-b", "补3": "S3"}
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    pattern = re.compile(r"判据(补[123](?:-[ab])?|[0-9]+(?:-[abc])?)")
    for line in text.splitlines():
        match = pattern.search(line)
        if not match:
            continue
        key = aliases.get(match.group(1), match.group(1))
        if key not in status:
            continue
        if "✓" in line:
            status[key] = "PASS"
        elif "✗" in line:
            status[key] = "FAIL"
        elif "⚠" in line:
            status[key] = "NO SAMPLE"
    return status


def timestamp(line):
    match = re.match(r"(\d{4}/\d{2}/\d{2} \d{2}:\d{2}:\d{2})", line)
    if not match:
        return None
    return datetime.strptime(match.group(1), "%Y/%m/%d %H:%M:%S")


def parse_cycles(path, node):
    cycles = []
    current = None
    with open(path, encoding="utf-8", errors="replace") as stream:
        for line in stream:
            stamp = timestamp(line)
            if stamp is None:
                continue
            if "BGP-LS: Module initialized" in line:
                current = {"start": stamp, "events": {}}
                cycles.append(current)
            if current is None:
                continue
            for label, needle, _, _ in EVENTS:
                if needle in line and label not in current["events"]:
                    current["events"][label] = (stamp - current["start"]).total_seconds()

    completed = [cycle for cycle in cycles if "JOIN complete" in cycle["events"]]
    labels = []
    for index, cycle in enumerate(completed, start=1):
        suffix = "initial" if index == 1 else f"rejoin {index - 1}"
        labels.append((f"{node} {suffix}", cycle))
    return labels


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--log-dir", default="logs-backbone")
    parser.add_argument("--check-log", default="discovery_check.log")
    parser.add_argument("--output", default="discovery_results.png")
    args = parser.parse_args()

    statuses = parse_status(args.check_log)
    cycles = []
    for node in ("z1", "z2"):
        cycles.extend(parse_cycles(Path(args.log_dir) / node / "frr.log", node))
    if not cycles:
        raise SystemExit("No completed JOIN cycle found in z1/z2 logs")

    fig = plt.figure(figsize=(26, 15.5))
    grid = fig.add_gridspec(1, 2, width_ratios=(1.18, 1), wspace=0.20)
    timeline_ax = fig.add_subplot(grid[0, 0])
    table_ax = fig.add_subplot(grid[0, 1])
    fig.suptitle("MIDR Discovery Chain — Real Multi-hop Integration Results",
                 fontweight="bold", y=0.985)

    y_positions = list(range(len(cycles)))[::-1]
    max_time = 1.0
    for y, (cycle_label, cycle) in zip(y_positions, cycles):
        join_time = cycle["events"]["JOIN complete"]
        max_time = max(max_time, join_time)
        timeline_ax.hlines(y, 0, join_time, color="#9aa5b1", linewidth=4,
                           alpha=0.7)
        for label, _, color, marker in EVENTS:
            if label not in cycle["events"]:
                continue
            elapsed = cycle["events"][label]
            timeline_ax.scatter(elapsed, y, s=230 if marker != "*" else 360,
                                color=color, marker=marker, zorder=3,
                                label=label)
        timeline_ax.text(join_time + 3, y, f"JOIN {int(join_time)}s",
                         va="center", fontsize=20, color="#176b38",
                         fontweight="bold")

    handles, labels = timeline_ax.get_legend_handles_labels()
    unique = dict(zip(labels, handles))
    timeline_ax.legend(unique.values(), unique.keys(), loc="upper left",
                       ncol=2)
    timeline_ax.set_yticks(y_positions, [label for label, _ in cycles])
    timeline_ax.set_xlim(-3, max_time + 38)
    timeline_ax.set_ylim(-0.7, len(cycles) - 0.3)
    timeline_ax.set_xlabel("Seconds since bgpd startup")
    timeline_ax.set_title("RTT-to-JOIN timeline", fontweight="bold", pad=18)
    timeline_ax.grid(axis="x", alpha=0.28)

    table_ax.axis("off")
    passed = sum(value == "PASS" for value in statuses.values())
    failed = sum(value == "FAIL" for value in statuses.values())
    no_sample = sum(value == "NO SAMPLE" for value in statuses.values())
    table_ax.set_title(
        f"Discovery criteria — {passed} pass, {failed} fail, {no_sample} no sample",
        fontweight="bold", pad=18)
    rows = [[key, label, statuses[key]] for key, label in CRITERIA]
    table = table_ax.table(
        cellText=rows,
        colLabels=["ID", "Criterion", "Result"],
        colWidths=[0.15, 0.57, 0.28],
        cellLoc="left",
        loc="center",
    )
    table.auto_set_font_size(False)
    table.set_fontsize(20)
    table.scale(1, 1.70)
    colors = {"PASS": "#b7e4c7", "FAIL": "#ffb3b3", "NO SAMPLE": "#ffe8a1"}
    for column in range(3):
        table[(0, column)].set_facecolor("#334e68")
        table[(0, column)].set_text_props(color="white", weight="bold")
    for row_index, (key, _, _) in enumerate(rows, start=1):
        table[(row_index, 2)].set_facecolor(colors[statuses[key]])
        table[(row_index, 2)].set_text_props(weight="bold")
        if row_index % 2 == 0:
            table[(row_index, 0)].set_facecolor("#eef2f5")
            table[(row_index, 1)].set_facecolor("#eef2f5")

    fig.text(0.02, 0.016,
             "The timeline uses real container timestamps; representative and member decisions "
             "follow independent RTT evaluation windows.", fontsize=20)
    plt.savefig(args.output, dpi=180, bbox_inches="tight")
    print(f"Saved: {args.output}")


if __name__ == "__main__":
    main()
