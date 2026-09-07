#!/usr/bin/env python3

import argparse
import re
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


plt.rcParams.update({
    "font.size": 26,
    "font.sans-serif": ["WenQuanYi Zen Hei", "Noto Sans CJK SC",
                        "DejaVu Sans"],
    "axes.unicode_minus": False,
    "axes.titlesize": 32,
    "xtick.labelsize": 24,
    "ytick.labelsize": 26,
})


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--packet-count", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    content = "\n".join(path.read_text(errors="replace")
                        for path in args.logs)
    checks = [
        ("精确绑定 transport", "PASS exact-bind" in content),
        ("仅接收 IPv6", "PASS ipv6-v6only" in content),
        ("20 字节请求与应答", "PASS request-echoed" in content
         and "PASS request-reply" in content),
        ("未知来源拒绝", "PASS unknown-source-rejected" in content),
        ("mapped 地址拒绝", "PASS mapped-source-rejected" in content),
        ("link-local / 多播拒绝", "PASS link-local-source-rejected" in content
         and "PASS multicast-source-rejected" in content),
        ("跨族 / 错误端口拒绝", "PASS cross-family-source-rejected" in content
         and "PASS wrong-port-rejected" in content),
        ("IPv6 抓包可见", args.packet_count >= 3),
    ]

    for label, passed in checks:
        print(f"{'PASS' if passed else 'FAIL'}: {label}")

    labels = [label for label, _ in checks]
    values = [1 if passed else 0 for _, passed in checks]
    colors = ["#42a66c" if passed else "#d9534f" for _, passed in checks]
    figure, axis = plt.subplots(figsize=(16, 11))
    bars = axis.barh(labels, values, color=colors, height=0.62)
    axis.invert_yaxis()
    axis.set_xlim(0, 1.22)
    axis.set_xticks([])
    axis.set_title("PM IPv6 独立冒烟测试", pad=24, fontweight="bold")
    axis.spines[["top", "right", "bottom", "left"]].set_visible(False)

    for bar, passed in zip(bars, values):
        axis.text(1.04, bar.get_y() + bar.get_height() / 2,
                  "通过" if passed else "失败", va="center",
                  fontsize=26, fontweight="bold",
                  color="#267349" if passed else "#a62f2b")

    rtt_match = re.search(r"RTT_US=(\d+)", content)
    if rtt_match:
        figure.text(0.5, 0.035,
                    f"本机 namespace 往返时延：{int(rtt_match.group(1)) / 1000:.3f} ms",
                    ha="center", fontsize=25)

    figure.tight_layout(rect=(0.04, 0.08, 0.98, 0.96))
    figure.savefig(args.output, dpi=180, bbox_inches="tight")
    print(f"Saved: {args.output}")
    return 0 if all(values) else 1


if __name__ == "__main__":
    sys.exit(main())
