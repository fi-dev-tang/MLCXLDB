#!/usr/bin/env python3
"""
Plot 4-system × 7-workload throughput comparison from bench_20260526_150208.
two_level (ours) vs bf-tree / 3T / hybridtier.

Notes vs bench_20260525_233324:
  - two_level now uses per-workload DRAM split (BP=0.375+RC=0.125 for D/E,
    BP=0.125+RC=0.375 for A/B/C/F/TPC-C).
  - hybridtier is strictly page-grain (no record-grain, no CMS); the
    in-memory leaks present in yesterday's reproduction have been removed.
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

# Mqps for YCSB, TPS for TPC-C. Source: bench_20260526_150208/*.log
YCSB_DATA = {
    "YCSB-A\n(R 50% / U 50%, Zipf 0.99)": {
        "two_level": 1.039, "bf-tree": 2.186, "3T": 0.679, "hybridtier": 1.594,
    },
    "YCSB-B\n(R 95% / U 5%)": {
        "two_level": 2.404, "bf-tree": 2.938, "3T": 0.876, "hybridtier": 2.713,
    },
    "YCSB-C\n(R 100%)": {
        "two_level": 2.192, "bf-tree": 3.320, "3T": 1.816, "hybridtier": 2.573,
    },
    "YCSB-D\n(read-latest 95% / I 5%)": {
        "two_level": 0.609, "bf-tree": 0.251, "3T": 0.043, "hybridtier": 0.566,
    },
    "YCSB-E\n(scan 95% / I 5%)": {
        "two_level": 0.288, "bf-tree": 0.083, "3T": 0.073, "hybridtier": 0.274,
    },
    "YCSB-F\n(R 50% / RMW 50%)": {
        "two_level": 0.963, "bf-tree": 2.082, "3T": 0.577, "hybridtier": 1.381,
    },
}

TPCC_DATA = {
    "two_level": 67775, "bf-tree": 31516, "3T": 58873, "hybridtier": 63411,
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
    # Highlight our system with a hatch
    bars[0].set_hatch("//")
    bars[0].set_edgecolor("black")

    ax.set_xticks(xs)
    ax.set_xticklabels([SYSTEM_LABELS[s] for s in SYSTEMS], fontsize=8, rotation=15)
    ax.set_ylabel(ylabel, fontsize=9)
    ax.grid(axis="y", linestyle=":", alpha=0.4)
    ax.set_axisbelow(True)
    if title is not None:
        ax.set_title(title, fontsize=10)

    # Annotate values on top
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

    # 6 YCSB subplots in 3x2
    ycsb_keys = list(YCSB_DATA.keys())
    for idx, key in enumerate(ycsb_keys):
        row, col = divmod(idx, 2)
        ax = fig.add_subplot(gs[row, col])
        draw_bars(ax, YCSB_DATA[key], ylabel="Throughput (Mqps)", title=key)

    # TPC-C spanning the full bottom row
    ax_tpcc = fig.add_subplot(gs[3, :])
    draw_bars(
        ax_tpcc,
        TPCC_DATA,
        ylabel="Throughput (TPS)",
        annotate_fmt="{:,.0f}",
        title="TPC-C (10 warehouses, 8 workers, warmup 20s + measure 40s)",
    )

    # Figure-level legend
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
        "bench_20260526_150208  ·  workingset=2.0 GiB, CXL=2.0 GiB, DRAM=0.5 GiB, theta=0.99, payload=100B\n"
        "two_level uses per-workload BP/RC split (D/E: BP=0.375+RC=0.125; others: BP=0.125+RC=0.375)",
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

    # Force RGB (drop alpha) so picky viewers (e.g. some Windows preview) open it.
    with Image.open(png_path) as im:
        if im.mode != "RGB":
            im.convert("RGB").save(png_path, "PNG", optimize=True)
    print(f"[OK] wrote {pdf_path}")
    print(f"[OK] wrote {png_path}")


if __name__ == "__main__":
    main()
