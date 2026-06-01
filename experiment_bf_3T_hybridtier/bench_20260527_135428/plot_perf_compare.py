#!/usr/bin/env python3
"""
Plot 4-system × 7-workload throughput comparison from bench_20260527_135428.
two_level (ours) vs bf-tree / 3T / hybridtier.

Key change vs bench_20260526_150208:
  - bf-tree now uses fair memory budget: BP=0.125 + mini-page=0.375 (total 0.5 GiB).
    Previous round gave bf-tree BP=0.5 (2× our DRAM).
"""

import os
from datetime import datetime
import matplotlib.pyplot as plt
import numpy as np
from PIL import Image

SYSTEMS = ["two_level", "bf-tree", "3T", "hybridtier"]
SYSTEM_LABELS = {
    "two_level": "Ours (two-level)",
    "bf-tree": "bf-tree",
    "3T": "3T (tiered-indexing)",
    "hybridtier": "hybridtier",
}
SYSTEM_COLORS = {
    "two_level": "#d62728",
    "bf-tree": "#1f77b4",
    "3T": "#2ca02c",
    "hybridtier": "#9467bd",
}

YCSB_DATA = {
    "YCSB-A\n(R 50% / U 50%, Zipf 0.99)": {
        "two_level": 1.165, "bf-tree": 2.347, "3T": 0.645, "hybridtier": 1.633,
    },
    "YCSB-B\n(R 95% / U 5%)": {
        "two_level": 2.563, "bf-tree": 2.490, "3T": 0.898, "hybridtier": 2.239,
    },
    "YCSB-C\n(R 100%)": {
        "two_level": 2.406, "bf-tree": 2.443, "3T": 1.881, "hybridtier": 2.395,
    },
    "YCSB-D\n(read-latest 95% / I 5%)": {
        "two_level": 0.663, "bf-tree": 0.281, "3T": 0.046, "hybridtier": 0.568,
    },
    "YCSB-E\n(scan 95% / I 5%)": {
        "two_level": 0.279, "bf-tree": 0.067, "3T": 0.069, "hybridtier": 0.280,
    },
    "YCSB-F\n(R 50% / RMW 50%)": {
        "two_level": 1.080, "bf-tree": 2.348, "3T": 0.621, "hybridtier": 1.461,
    },
}

TPCC_DATA = {
    "two_level": 70806, "bf-tree": 46924, "3T": 55317, "hybridtier": 74011,
}

TIMESTAMP = datetime.now().strftime("%Y%m%d_%H%M%S")
OUT_DIR = os.path.dirname(os.path.abspath(__file__))


def draw_bars(ax, values, ylabel, annotate_fmt="{:.3f}", title=None):
    xs = np.arange(len(SYSTEMS))
    bars = ax.bar(
        xs,
        [values[s] for s in SYSTEMS],
        color=[SYSTEM_COLORS[s] for s in SYSTEMS],
        edgecolor="black",
        linewidth=0.6,
        width=0.7,
    )
    bars[0].set_hatch("//")
    bars[0].set_edgecolor("black")

    ax.set_xticks(xs)
    ax.set_xticklabels([SYSTEM_LABELS[s] for s in SYSTEMS], fontsize=8, rotation=15)
    ax.set_ylabel(ylabel, fontsize=9)
    ax.grid(axis="y", linestyle=":", alpha=0.4)
    ax.set_axisbelow(True)
    if title is not None:
        ax.set_title(title, fontsize=10)

    ymax = max(values[s] for s in SYSTEMS)
    for bar, s in zip(bars, SYSTEMS):
        v = values[s]
        ax.text(
            bar.get_x() + bar.get_width() / 2,
            v + ymax * 0.02,
            annotate_fmt.format(v),
            ha="center",
            va="bottom",
            fontsize=7.5,
        )
    ax.set_ylim(0, ymax * 1.18)


def main():
    fig = plt.figure(figsize=(10, 13))
    gs = fig.add_gridspec(
        nrows=4, ncols=2, hspace=0.85, wspace=0.28,
        top=0.86, bottom=0.04, left=0.07, right=0.97,
    )

    ycsb_keys = list(YCSB_DATA.keys())
    for idx, key in enumerate(ycsb_keys):
        row, col = divmod(idx, 2)
        ax = fig.add_subplot(gs[row, col])
        draw_bars(ax, YCSB_DATA[key], ylabel="Throughput (Mqps)", title=key)

    ax_tpcc = fig.add_subplot(gs[3, :])
    draw_bars(
        ax_tpcc,
        TPCC_DATA,
        ylabel="Throughput (TPS)",
        annotate_fmt="{:,.0f}",
        title="TPC-C (10 warehouses, 8 workers, warmup 20s + measure 40s)",
    )

    handles = [
        plt.Rectangle(
            (0, 0), 1, 1,
            facecolor=SYSTEM_COLORS[s],
            edgecolor="black",
            hatch=("//" if s == "two_level" else None),
        )
        for s in SYSTEMS
    ]
    labels = [SYSTEM_LABELS[s] for s in SYSTEMS]
    fig.legend(
        handles, labels,
        loc="upper center",
        bbox_to_anchor=(0.5, 0.915),
        ncol=4,
        fontsize=10,
        frameon=False,
    )

    fig.suptitle(
        "Throughput comparison: ours (two-level) vs three baselines\n"
        "bench_20260527_135428  ·  workingset=2.0 GiB, CXL=2.0 GiB, DRAM=0.5 GiB, theta=0.99, payload=100B\n"
        "bf-tree fair budget: BP=0.125+MP=0.375 (was BP=0.5 in prev round)",
        fontsize=10,
        y=0.975,
    )

    pdf_path = os.path.join(OUT_DIR, f"perf_compare_{TIMESTAMP}.pdf")
    png_path = os.path.join(OUT_DIR, f"perf_compare_{TIMESTAMP}.png")
    svg_path = os.path.join(OUT_DIR, f"perf_compare_{TIMESTAMP}.svg")
    fig.set_facecolor("white")
    for ax in fig.get_axes():
        ax.set_facecolor("white")
    fig.savefig(pdf_path, facecolor="white")
    fig.savefig(png_path, dpi=150, facecolor="white", transparent=False)
    fig.savefig(svg_path, facecolor="white")
    print(f"[OK] wrote {svg_path}")

    with Image.open(png_path) as im:
        if im.mode != "RGB":
            im.convert("RGB").save(png_path, "PNG", optimize=True)
    print(f"[OK] wrote {pdf_path}")
    print(f"[OK] wrote {png_path}")


if __name__ == "__main__":
    main()
