#!/usr/bin/env python3
"""
Plot MIDR group-id-allocation collision prevention/repair test results.

Usage:
    python3 plot_group_alloc.py --prevent-dir logs-prevent --repair-dir logs-repair \
        --output group_alloc_results.png

Reads x/y's bgpd logs from both scenarios (run_test.sh --scenario both saves
each scenario's logs/ into logs-prevent/ and logs-repair/ automatically) and
draws a two-panel event timeline:
  top    = "prevent" scenario: both bootstraps reachable, x/y each get a
           distinct group id from the elected allocator -- no collision.
  bottom = "repair" scenario: the allocator bootstrap is deliberately not
           started, x/y's allocation requests fall back to an identical
           local estimate, they collide (both self-appoint the same group),
           and the collision is detected + repaired (loser reconnects and
           re-settles, converging with the winner's group).
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
    "ytick.labelsize": 26,
    "legend.fontsize": 22,
    "figure.titlesize": 32,
    "axes.linewidth": 1.8,
    "lines.linewidth": 2.6,
})

TS_PAT = r'(\d{4}/\d{2}/\d{2} \d{2}:\d{2}:\d{2}\.\d+)'
SELF_APPOINT_PAT = re.compile(TS_PAT + r'.*MIDR 加入落定：群 (\d+) 目前只有本节点，自任首任代表')
SETTLE_PAT = re.compile(TS_PAT + r'.*MIDR I-7：CREATE 落定群 (\d+)')
COLLISION_PAT = re.compile(TS_PAT + r'.*MIDR CL: GROUP_ID_COLLISION')
RECONNECT_PAT = re.compile(TS_PAT + r'.*MIDR I-7：RECONNECT 群 \d+ 因失联触发')
JOIN_PAT = re.compile(TS_PAT + r'.*MIDR CL: MEMBER_PROBE_DONE → JOIN 群 (\d+)')


def parse_ts(s):
    return datetime.strptime(s, "%Y/%m/%d %H:%M:%S.%f")


def parse_node_log(path):
    """Returns a list of (ts, kind, label) events for one node's log."""
    events = []
    try:
        with open(path) as f:
            lines = f.readlines()
    except FileNotFoundError:
        print(f"  (missing: {path})")
        return events

    for line in lines:
        m = SELF_APPOINT_PAT.search(line)
        if m:
            events.append((parse_ts(m.group(1)), "self-appoint", f"self-appoint group {m.group(2)}"))
            continue
        m = COLLISION_PAT.search(line)
        if m:
            events.append((parse_ts(m.group(1)), "collision", "collision detected, yielding"))
            continue
        m = RECONNECT_PAT.search(line)
        if m:
            events.append((parse_ts(m.group(1)), "reconnect", "RECONNECT"))
            continue
        m = JOIN_PAT.search(line)
        if m:
            events.append((parse_ts(m.group(1)), "join", f"JOIN group {m.group(2)}"))
            continue
        m = SETTLE_PAT.search(line)
        if m:
            events.append((parse_ts(m.group(1)), "settle", f"CREATE settle group {m.group(2)}"))
    return events


KIND_STYLE = {
    "settle": ("tab:blue", "s"),
    "collision": ("tab:red", "X"),
    "reconnect": ("tab:orange", "^"),
    "join": ("tab:green", "*"),
}


def plot_prevention(ax, logdir):
    values = []
    for node in ("x", "y"):
        events = parse_node_log(f"{logdir}/bgpd-{node}.log")
        groups = [int(label.rsplit(" ", 1)[1]) for _, kind, label in events
                  if kind == "settle"]
        values.append(groups[-1] if groups else 0)

    if not all(values):
        ax.set_title(f"Scenario A: Collision Prevention (no data found in {logdir})")
        return

    colors = ["steelblue", "darkorange"] if values[0] != values[1] else ["firebrick"] * 2
    bars = ax.bar(["x", "y"], values, color=colors, width=0.55)
    ax.bar_label(bars, labels=[f"Group {value}" for value in values],
                 fontsize=22, padding=6, fontweight="bold")
    verdict = "Distinct IDs allocated — collision prevented" if values[0] != values[1] else "Collision"
    ax.text(0.5, 0.92, verdict, transform=ax.transAxes, ha="center", va="top",
            fontsize=22, color="darkgreen" if values[0] != values[1] else "firebrick",
            fontweight="bold")
    ax.set_ylabel("Allocated group ID")
    ax.set_xlabel("Simultaneous zero-config node")
    ax.set_ylim(0, max(values) * 1.35 + 0.5)
    ax.set_title("Scenario A: Prevention (allocator online)")
    ax.grid(True, axis="y", alpha=0.3)


def plot_scenario(ax, logdir, title):
    x_events = [event for event in parse_node_log(f"{logdir}/bgpd-x.log")
                if event[1] != "self-appoint"]
    y_events = [event for event in parse_node_log(f"{logdir}/bgpd-y.log")
                if event[1] != "self-appoint"]

    all_ts = [e[0] for e in x_events + y_events]
    if not all_ts:
        ax.set_title(f"{title} (no data found in {logdir})")
        return
    t0 = min(all_ts)

    rows = {"x (router-id 10.0.5.11)": (x_events, 1), "y (router-id 10.0.5.12)": (y_events, 0)}
    for label, (events, y) in rows.items():
        ax.axhline(y, color="lightgray", lw=1, zorder=0)
        # Stagger annotations that land close together in time (self-appoint
        # and settle are logged in the same breath, milliseconds apart) --
        # a fixed offset makes consecutive labels print on top of each
        # other and become unreadable.
        last_t = None
        stack = 0
        for ts, kind, detail in events:
            t_rel = (ts - t0).total_seconds()
            if last_t is not None and abs(t_rel - last_t) < max(3.0, 0.02 * max(t_rel, 1)):
                stack += 1
            else:
                stack = 0
            last_t = t_rel
            color, marker = KIND_STYLE[kind]
            ax.scatter([t_rel], [y], color=color, marker=marker, s=260, zorder=3,
                       edgecolors="black", linewidths=0.8)
            dy = (18 + stack * 60) if y == 1 else -(30 + stack * 60)
            dx = 10 + stack * 130
            ax.annotate(f"{detail}\n(t={t_rel:.0f}s)", (t_rel, y), xytext=(dx, dy),
                        textcoords="offset points", fontsize=20, color=color,
                        fontweight="bold", ha="left",
                        arrowprops=dict(arrowstyle="-", color=color, lw=1, alpha=0.6))

    ax.set_yticks([0, 1])
    ax.set_yticklabels([
        "y (router-id 10.0.5.12)",
        "x (router-id 10.0.5.11)",
    ])
    ax.set_xlabel("Experiment time (s)")
    ax.set_title(title)
    ax.set_ylim(-1.1, 2.1)
    ax.margins(x=0.08)
    ax.grid(True, axis="x", alpha=0.3)

    handles = [plt.Line2D([0], [0], marker=m, color="w", markerfacecolor=c,
                           markeredgecolor="black", markersize=16, label=k)
               for k, (c, m) in KIND_STYLE.items()]
    ax.legend(handles=handles, loc="upper right", ncol=3, fontsize=20)


def main():
    ap = argparse.ArgumentParser(description="Plot MIDR group-id-allocation collision test results")
    ap.add_argument("--prevent-dir", default="logs-prevent")
    ap.add_argument("--repair-dir", default="logs-repair")
    ap.add_argument("--output", default="group_alloc_results.png")
    args = ap.parse_args()

    fig, (ax0, ax1) = plt.subplots(2, 1, figsize=(20, 14))
    fig.suptitle("MIDR NDS — Group-ID Allocation: Collision Prevention & Repair",
                 fontsize=32, fontweight="bold")

    plot_prevention(ax0, args.prevent_dir)
    plot_scenario(ax1, args.repair_dir,
                  "Scenario B: repair (allocator down -- collision forced, then detected & repaired)")

    plt.tight_layout(rect=[0, 0, 1, 0.96])
    plt.savefig(args.output, dpi=150, bbox_inches="tight")
    print(f"Saved: {args.output}")


if __name__ == "__main__":
    main()
