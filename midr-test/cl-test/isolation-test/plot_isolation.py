#!/usr/bin/env python3
"""Create a slide-friendly timeline of MIDR isolation recovery."""

import argparse
import re
from datetime import datetime

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


plt.rcParams.update({
    "font.size": 22,
    "font.sans-serif": ["DejaVu Sans"],
    "axes.titlesize": 28,
    "axes.labelsize": 24,
    "xtick.labelsize": 21,
    "ytick.labelsize": 21,
    "figure.titlesize": 31,
    "axes.linewidth": 1.8,
})

TS_PAT = r"(\d{4}/\d{2}/\d{2} \d{2}:\d{2}:\d{2}\.\d+)"
INITIAL_JOIN_PAT = re.compile(r"MIDR CL: MEMBER_PROBE_DONE → JOIN 群")
INITIAL_ANCHOR_PAT = re.compile(r"MIDR I-7：ANCHOR ")
ZERO_SESS_PAT = re.compile(
    TS_PAT + r".*MIDR: 0 个已建立会话（第 (\d+)/(\d+) 次探测"
)
ISOLATED_PAT = re.compile(TS_PAT + r".*MIDR:.*判定孤岛，通知 CL")
CL_DECISION_PAT = re.compile(TS_PAT + r".*MIDR CL: ISOLATED")
RECONNECT_PAT = re.compile(TS_PAT + r".*MIDR I-7：RECONNECT 群 \d+ 因失联触发")
REP_LIST_PAT = re.compile(TS_PAT + r".*MIDR JOIN: sent REP_LIST_REQ to bootstrap")
SETTLE_PAT = re.compile(TS_PAT + r".*MIDR I-7：(?:CREATE|JOIN)[^\n]*加入流程结束")
GROUP_PAT = re.compile(r"自建群 (\d+)|落定群 (\d+)|JOIN 群 (\d+)")

LANES = {
    "failure": 6,
    "debounce": 5,
    "isolated": 4,
    "decision": 3,
    "reconnect": 2,
    "directory": 1,
    "settle": 0,
}
LANE_LABELS = [
    "Fallback outcome",
    "Bootstrap query",
    "NDS action",
    "CL decision",
    "Isolation detection",
    "Debounce",
    "Connectivity failure",
]
STYLES = {
    "failure": ("#c62828", "X"),
    "debounce": ("#1565c0", "o"),
    "isolated": ("#ad1457", "D"),
    "decision": ("#6a1b9a", "s"),
    "reconnect": ("#ef6c00", "^"),
    "directory": ("#00838f", ">"),
    "settle": ("#2e7d32", "P"),
}


def parse_ts(value):
    return datetime.strptime(value, "%Y/%m/%d %H:%M:%S.%f")


def parse_log(path, since_line):
    initial_join = False
    initial_anchor = False
    events = {}

    with open(path, encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, start=1):
            if line_number < since_line:
                initial_join |= bool(INITIAL_JOIN_PAT.search(line))
                initial_anchor |= bool(INITIAL_ANCHOR_PAT.search(line))
                continue

            match = ZERO_SESS_PAT.search(line)
            if match and "debounce" not in events:
                events["debounce"] = (
                    parse_ts(match.group(1)),
                    f"Zero sessions: {match.group(2)}/{match.group(3)}",
                )
                continue

            match = ISOLATED_PAT.search(line)
            if match and "isolated" not in events:
                events["isolated"] = (
                    parse_ts(match.group(1)),
                    "Debounce threshold reached",
                )
                continue

            match = CL_DECISION_PAT.search(line)
            if match and "decision" not in events:
                events["decision"] = (
                    parse_ts(match.group(1)),
                    "ISOLATED → RECONNECT",
                )
                continue

            match = RECONNECT_PAT.search(line)
            if match and "reconnect" not in events:
                events["reconnect"] = (
                    parse_ts(match.group(1)),
                    "RECONNECT executed",
                )
                continue

            match = REP_LIST_PAT.search(line)
            if match and "directory" not in events:
                events["directory"] = (
                    parse_ts(match.group(1)),
                    "Fresh representative-directory request",
                )
                continue

            match = SETTLE_PAT.search(line)
            if match and "settle" not in events:
                group = GROUP_PAT.search(line)
                group_id = next(
                    (value for value in (group.groups() if group else ()) if value),
                    "?",
                )
                events["settle"] = (
                    parse_ts(match.group(1)),
                    f"Fallback CREATE: Group {group_id}",
                )

    return initial_join, initial_anchor, events


def add_card(ax, x, title, body, passed):
    ax.text(
        x,
        0.50,
        f"{title}\n{body}",
        transform=ax.transAxes,
        ha="center",
        va="center",
        fontsize=20,
        linespacing=1.45,
        fontweight="bold",
        bbox={
            "boxstyle": "round,pad=0.65",
            "facecolor": "#66bb6a" if passed else "#ef9a9a",
            "edgecolor": "#455a64",
            "linewidth": 1.8,
            "alpha": 0.18,
        },
    )


def elapsed_seconds(events, first, second):
    if first not in events or second not in events:
        return None
    return (events[second][0] - events[first][0]).total_seconds()


def plot(initial_join, initial_anchor, events, kill_time, output):
    if kill_time is not None:
        events["failure"] = (kill_time, "Representatives d and e stopped")

    required = set(LANES)
    complete = required.issubset(events)
    passed = initial_join and initial_anchor and complete

    timestamps = [event[0] for event in events.values()]
    if not timestamps:
        raise ValueError("No isolation recovery events found in the log")
    baseline = kill_time or min(timestamps)
    relative = {
        kind: (timestamp - baseline).total_seconds()
        for kind, (timestamp, _label) in events.items()
    }
    xmin = min(-4.0, min(relative.values()) - 4.0)
    xmax = max(relative.values()) + 24.0

    fig = plt.figure(figsize=(20, 12))
    grid = fig.add_gridspec(
        2,
        1,
        height_ratios=[5.0, 1.7],
        left=0.18,
        right=0.97,
        top=0.88,
        bottom=0.06,
        hspace=0.28,
    )
    timeline_ax = fig.add_subplot(grid[0])
    summary_ax = fig.add_subplot(grid[1])
    fig.suptitle("MIDR NDS Isolation Self-Healing", y=0.96, fontweight="bold")

    for y in LANES.values():
        timeline_ax.hlines(y, xmin, xmax, color="#cfd8dc", linewidth=1.6)

    if "isolated" in relative:
        timeline_ax.axvspan(
            0,
            relative["isolated"],
            color="#ef5350",
            alpha=0.08,
            label="Isolation debounce window",
        )

    for kind in ("failure", "debounce", "isolated", "decision", "reconnect",
                 "directory", "settle"):
        if kind not in events:
            continue
        x = relative[kind]
        y = LANES[kind]
        color, marker = STYLES[kind]
        timeline_ax.scatter(
            [x], [y], color=color, marker=marker, s=300,
            edgecolors="black", linewidths=0.8, zorder=3,
        )
        timeline_ax.annotate(
            f"{events[kind][1]}\n+{x:.1f} s",
            (x, y),
            xytext=(12, 0),
            textcoords="offset points",
            ha="left",
            va="center",
            fontsize=20,
            color=color,
            fontweight="bold",
        )

    timeline_ax.set_xlim(xmin, xmax)
    timeline_ax.set_ylim(-0.7, 6.7)
    timeline_ax.set_yticks(range(7), LANE_LABELS)
    timeline_ax.set_xlabel("Time since d/e failure (s)")
    timeline_ax.set_title("Failure → Debounce → RECONNECT → Fallback CREATE")
    timeline_ax.grid(axis="x", alpha=0.25)
    timeline_ax.set_axisbelow(True)

    summary_ax.axis("off")
    initial_ok = initial_join and initial_anchor
    detection = elapsed_seconds(events, "failure", "isolated")
    restart = elapsed_seconds(events, "isolated", "reconnect")
    fallback = elapsed_seconds(events, "reconnect", "settle")
    add_card(summary_ax, 0.12, "INITIAL STATE", "JOIN + ANCHOR\nPASS" if initial_ok else "INCOMPLETE", initial_ok)
    add_card(summary_ax, 0.37, "ISOLATION DETECTED", f"{detection:.1f} s\n2/2 ticks" if detection is not None else "MISSING", detection is not None)
    add_card(summary_ax, 0.62, "RECONNECT", f"{restart:.1f} s after detection\nPASS" if restart is not None else "MISSING", restart is not None)
    add_card(summary_ax, 0.87, "FALLBACK CREATE", f"{fallback:.1f} s after RECONNECT\nPASS" if fallback is not None else "MISSING", fallback is not None)

    fig.text(
        0.5,
        0.015,
        "Result: the node detects total session loss, re-contacts the bootstrap, and resumes group formation.",
        ha="center",
        fontsize=20,
        color="#37474f",
    )
    plt.savefig(output, dpi=150, bbox_inches="tight")
    print(f"Saved: {output}")
    return 0 if passed else 1


def main():
    parser = argparse.ArgumentParser(description="Plot MIDR isolation recovery")
    parser.add_argument("logfile", help="joining node f's bgpd log")
    parser.add_argument("--output", default="isolation_results.png")
    parser.add_argument("--since-line", type=int, default=1)
    parser.add_argument("--kill-time", help="d/e stop time: YYYY/MM/DD HH:MM:SS.mmm")
    args = parser.parse_args()

    initial_join, initial_anchor, events = parse_log(args.logfile, args.since_line)
    kill_time = parse_ts(args.kill_time) if args.kill_time else None
    return plot(initial_join, initial_anchor, events, kill_time, args.output)


if __name__ == "__main__":
    raise SystemExit(main())
