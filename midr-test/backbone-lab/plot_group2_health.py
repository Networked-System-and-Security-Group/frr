#!/usr/bin/env python3
"""Plot Group2 convergence and criterion results for presentation slides."""

import argparse
import csv
import re
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


plt.rcParams.update({
    "font.size": 22,
    "axes.titlesize": 25,
    "axes.labelsize": 23,
    "xtick.labelsize": 20,
    "ytick.labelsize": 20,
    "legend.fontsize": 19,
    "figure.titlesize": 29,
    "axes.linewidth": 1.8,
})

MEMBERS = {"r1", "r2", "m1a", "m1b", "m2a", "z1", "z2"}
LINK_NODES = {"r1", "m1a", "m1b"}
CRITERIA = [
    ("G1", "Callback registration"),
    ("G2", "Independent-source check"),
    ("G3", "Node reporting"),
    ("G4", "Bootstrap suppression"),
    ("G5", "Link accounting"),
    ("G6", "Placeholder visibility"),
    ("G7", "False-withdraw probe"),
    ("G8", "Zero-group join"),
]


def read_samples(path):
    rows = []
    with open(path, encoding="utf-8", errors="replace") as stream:
        for row in csv.DictReader(stream):
            try:
                rows.append({
                    "elapsed": int(row["elapsed"]),
                    "node": row["node"],
                    "reported": int(row["node_reported"]),
                    "pending": int(row["node_pending"]),
                    "link_reported": int(row["link_reported"]),
                    "owned": int(row["owned_links"]),
                })
            except (KeyError, ValueError):
                continue
    return rows


def aggregate(rows, nodes, fields):
    points = {}
    for row in rows:
        if row["node"] not in nodes:
            continue
        bucket = points.setdefault(row["elapsed"], {field: [] for field in fields})
        for field in fields:
            if row[field] >= 0:
                bucket[field].append(row[field])

    elapsed = sorted(points)
    series = {}
    for field in fields:
        series[field] = np.array([
            sum(points[t][field]) if points[t][field] else np.nan for t in elapsed
        ])
    return np.array(elapsed), series


def read_status(path):
    status = {key: "NO SAMPLE" for key, _ in CRITERIA}
    status["G7"] = "OBSERVED"
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    for line in text.splitlines():
        match = re.search(r"[判据]*G([1-8])", line)
        if not match:
            continue
        key = f"G{match.group(1)}"
        if "✓" in line:
            status[key] = "PASS"
        elif "✗" in line:
            status[key] = "FAIL"
        elif "⚠" in line:
            status[key] = "NO SAMPLE"
    return status


def draw_line(ax, x, y, label, color, marker):
    ax.plot(x, y, linewidth=3.4, marker=marker, markersize=7,
            markevery=max(1, len(x) // 12), label=label, color=color)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--samples", default="group2_health.log")
    parser.add_argument("--check-log", default="group2_check.log")
    parser.add_argument("--output", default="group2_results.png")
    args = parser.parse_args()

    rows = read_samples(args.samples)
    if not rows:
        raise SystemExit("No Group2 health samples were collected")
    statuses = read_status(args.check_log)

    fig = plt.figure(figsize=(24, 13.5))
    grid = fig.add_gridspec(2, 2, width_ratios=(1.25, 1.25), hspace=0.34,
                           wspace=0.24)
    node_ax = fig.add_subplot(grid[0, 0])
    link_ax = fig.add_subplot(grid[1, 0])
    table_ax = fig.add_subplot(grid[:, 1])
    fig.suptitle("MIDR Group2 Integration — Reporting Convergence and Checks",
                 fontweight="bold", y=0.985)

    elapsed, node_data = aggregate(rows, MEMBERS, ("reported", "pending"))
    minutes = elapsed / 60.0
    draw_line(node_ax, minutes, node_data["reported"],
              "Reported nodes", "#1677b8", "o")
    draw_line(node_ax, minutes, node_data["pending"],
              "Pending nodes", "#d95f02", "s")
    node_ax.axhline(7, linestyle="--", linewidth=2.5, color="#238b45",
                    label="Expected members (7)")
    node_ax.set_title("Node-reporting convergence", fontweight="bold")
    node_ax.set_ylabel("Member count")
    node_ax.set_ylim(bottom=-0.2)
    node_ax.grid(alpha=0.28)
    node_ax.legend(loc="best", ncol=2)

    link_elapsed, link_data = aggregate(
        rows, LINK_NODES, ("link_reported", "owned"))
    link_minutes = link_elapsed / 60.0
    exact_match = np.allclose(link_data["link_reported"], link_data["owned"],
                              equal_nan=True)
    if exact_match:
        draw_line(link_ax, link_minutes, link_data["link_reported"],
                  "Reported = Group2-owned", "#238b45", "o")
        valid = link_data["link_reported"][~np.isnan(link_data["link_reported"])]
        if valid.size:
            link_ax.text(0.97, 0.10, f"Final account: {int(valid[-1])} = {int(valid[-1])}",
                         transform=link_ax.transAxes, ha="right", va="bottom",
                         fontsize=21, color="#176b38", fontweight="bold",
                         bbox={"boxstyle": "round,pad=0.35", "facecolor": "#d8f3dc",
                               "edgecolor": "#238b45"})
    else:
        draw_line(link_ax, link_minutes, link_data["link_reported"],
                  "Locally reported links", "#6a3d9a", "o")
        draw_line(link_ax, link_minutes, link_data["owned"],
                  "Group2-owned links", "#33a02c", "s")
    link_ax.set_title("Link-account convergence (r1, m1a, m1b)",
                      fontweight="bold")
    link_ax.set_xlabel("Elapsed time (minutes)")
    link_ax.set_ylabel("Aggregated link count")
    link_ax.set_ylim(bottom=-0.2)
    link_ax.grid(alpha=0.28)
    link_ax.legend(loc="best")

    table_ax.axis("off")
    table_ax.set_title("Integration criteria", fontweight="bold", pad=16)
    cell_text = [[key, label, statuses[key]] for key, label in CRITERIA]
    table = table_ax.table(
        cellText=cell_text,
        colLabels=["ID", "Criterion", "Result"],
        colWidths=[0.12, 0.57, 0.31],
        cellLoc="left",
        loc="center",
    )
    table.auto_set_font_size(False)
    table.set_fontsize(20)
    table.scale(1, 2.45)
    colors = {
        "PASS": "#b7e4c7",
        "FAIL": "#ffb3b3",
        "NO SAMPLE": "#ffe8a1",
        "OBSERVED": "#cfe8ff",
    }
    for col in range(3):
        table[(0, col)].set_facecolor("#334e68")
        table[(0, col)].set_text_props(color="white", weight="bold")
    for row_idx, (key, _, _) in enumerate(cell_text, start=1):
        table[(row_idx, 2)].set_facecolor(colors[statuses[key]])
        table[(row_idx, 2)].set_text_props(weight="bold")
        if row_idx % 2 == 0:
            table[(row_idx, 0)].set_facecolor("#eef2f5")
            table[(row_idx, 1)].set_facecolor("#eef2f5")

    fig.text(0.02, 0.015,
             "Expected steady state: 7 reported nodes, 0 pending nodes, "
             "and equal reported/owned link counts.", fontsize=20)
    plt.savefig(args.output, dpi=180, bbox_inches="tight")
    print(f"Saved: {args.output}")


if __name__ == "__main__":
    main()
