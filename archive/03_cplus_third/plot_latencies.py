#!/usr/bin/env python3
"""
plot_latencies.py

Generates latency-analysis plots from the raw_latencies.csv produced by
benchmark.cpp (columns: phase, workload, op_index, op_type, latency_ns).

Usage:
    python3 plot_latencies.py [--raw raw_latencies.csv] [--summary summary_stats.csv] [--out plots]

Requires: pandas, matplotlib, numpy
"""

import argparse
import os
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt

PHASE_ORDER = ["phase1_priority_queue", "phase2_tick_vector", "phase3_map_list"]
PHASE_LABELS = {
    "phase1_priority_queue": "Phase 1\n(priority_queue)",
    "phase2_tick_vector":    "Phase 2\n(tick vector)",
    "phase3_map_list":       "Phase 3\n(map + list)",
}
PHASE_COLORS = {
    "phase1_priority_queue": "#4C72B0",
    "phase2_tick_vector":    "#DD8452",
    "phase3_map_list":       "#55A868",
}
WORKLOAD_ORDER = ["dense", "sparse"]


def iqr_filter(x, fence=3.0):
    """Same IQR-fence outlier filter used in benchmark.cpp, for apples-to-apples stats."""
    if len(x) == 0:
        return x
    q1, q3 = np.percentile(x, [25, 75])
    iqr = q3 - q1
    lo, hi = q1 - fence * iqr, q3 + fence * iqr
    return x[(x >= lo) & (x <= hi)]


def load_raw(path):
    print(f"Loading {path} ...")
    df = pd.read_csv(path)
    df["phase"] = pd.Categorical(df["phase"], categories=PHASE_ORDER, ordered=True)
    df["workload"] = pd.Categorical(df["workload"], categories=WORKLOAD_ORDER, ordered=True)
    print(f"  {len(df):,} rows loaded")
    return df


def plot_percentile_bars(df, out_dir):
    """Grouped bar chart of mean/P50/P90/P99 per phase, one subplot per workload."""
    metrics = ["mean", "p50", "p90", "p99"]
    fig, axes = plt.subplots(1, len(WORKLOAD_ORDER), figsize=(13, 5), sharey=False)

    for ax, workload in zip(axes, WORKLOAD_ORDER):
        wl_df = df[df["workload"] == workload]
        rows = []
        for phase in PHASE_ORDER:
            vals = wl_df.loc[wl_df["phase"] == phase, "latency_ns"].to_numpy()
            filt = iqr_filter(vals)
            rows.append({
                "phase": phase,
                "mean": filt.mean() if len(filt) else 0,
                "p50": np.percentile(filt, 50) if len(filt) else 0,
                "p90": np.percentile(filt, 90) if len(filt) else 0,
                "p99": np.percentile(filt, 99) if len(filt) else 0,
            })
        stats = pd.DataFrame(rows)

        x = np.arange(len(metrics))
        width = 0.25
        for i, phase in enumerate(PHASE_ORDER):
            vals = stats.loc[stats["phase"] == phase, metrics].values.flatten()
            ax.bar(x + (i - 1) * width, vals, width,
                   label=PHASE_LABELS[phase], color=PHASE_COLORS[phase])

        ax.set_xticks(x)
        ax.set_xticklabels([m.upper() for m in metrics])
        ax.set_ylabel("Latency (ns)")
        ax.set_title(f"{workload.capitalize()} workload")
        if workload == "sparse":
            ax.set_yscale("log")
        ax.grid(axis="y", alpha=0.3)

    axes[0].legend(loc="upper left", fontsize=8)
    fig.suptitle("Latency percentiles by phase (IQR-filtered)", fontsize=13)
    fig.tight_layout()
    path = os.path.join(out_dir, "percentile_comparison.png")
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"  wrote {path}")


def plot_histograms(df, out_dir):
    """Latency distribution histograms, one figure per workload, phases overlaid."""
    for workload in WORKLOAD_ORDER:
        fig, ax = plt.subplots(figsize=(8, 5))
        wl_df = df[df["workload"] == workload]

        for phase in PHASE_ORDER:
            vals = wl_df.loc[wl_df["phase"] == phase, "latency_ns"].to_numpy()
            filt = iqr_filter(vals)
            if len(filt) == 0:
                continue
            bins = np.logspace(np.log10(max(filt.min(), 1)), np.log10(filt.max()), 60)
            ax.hist(filt, bins=bins, alpha=0.5, label=PHASE_LABELS[phase].replace("\n", " "),
                    color=PHASE_COLORS[phase])

        ax.set_xscale("log")
        ax.set_xlabel("Latency (ns, log scale)")
        ax.set_ylabel("Count")
        ax.set_title(f"Latency distribution — {workload} workload (IQR-filtered)")
        ax.legend(fontsize=8)
        ax.grid(alpha=0.3)
        fig.tight_layout()
        path = os.path.join(out_dir, f"histogram_{workload}.png")
        fig.savefig(path, dpi=150)
        plt.close(fig)
        print(f"  wrote {path}")


def plot_cdf(df, out_dir):
    """Empirical CDF per phase, one figure per workload."""
    for workload in WORKLOAD_ORDER:
        fig, ax = plt.subplots(figsize=(8, 5))
        wl_df = df[df["workload"] == workload]

        for phase in PHASE_ORDER:
            vals = np.sort(wl_df.loc[wl_df["phase"] == phase, "latency_ns"].to_numpy())
            if len(vals) == 0:
                continue
            y = np.arange(1, len(vals) + 1) / len(vals)
            ax.plot(vals, y, label=PHASE_LABELS[phase].replace("\n", " "),
                     color=PHASE_COLORS[phase], linewidth=1.5)

        ax.set_xscale("log")
        ax.set_xlabel("Latency (ns, log scale)")
        ax.set_ylabel("Cumulative probability")
        ax.set_title(f"Latency CDF — {workload} workload (unfiltered, all samples)")
        ax.legend(fontsize=8)
        ax.grid(alpha=0.3)
        fig.tight_layout()
        path = os.path.join(out_dir, f"cdf_{workload}.png")
        fig.savefig(path, dpi=150)
        plt.close(fig)
        print(f"  wrote {path}")


def plot_timeseries(df, out_dir, sample_every=20):
    """
    Latency vs operation index, one figure per workload. This is the plot that
    makes Phase 2's sparse-book scan spikes visible over the course of a run.
    Downsampled (every Nth point) purely for plotting — does not affect the
    underlying stats, just keeps the PNG file size and render time reasonable.
    """
    for workload in WORKLOAD_ORDER:
        fig, ax = plt.subplots(figsize=(11, 5))
        wl_df = df[df["workload"] == workload]

        for phase in PHASE_ORDER:
            sub = wl_df[wl_df["phase"] == phase].iloc[::sample_every]
            ax.plot(sub["op_index"], sub["latency_ns"], '.', markersize=1.5,
                     alpha=0.6, label=PHASE_LABELS[phase].replace("\n", " "),
                     color=PHASE_COLORS[phase])

        ax.set_yscale("log")
        ax.set_xlabel("Operation index (post-warmup)")
        ax.set_ylabel("Latency (ns, log scale)")
        ax.set_title(f"Latency over time — {workload} workload "
                      f"(every {sample_every}th sample, unfiltered)")
        ax.legend(fontsize=8, markerscale=4)
        ax.grid(alpha=0.3)
        fig.tight_layout()
        path = os.path.join(out_dir, f"timeseries_{workload}.png")
        fig.savefig(path, dpi=150)
        plt.close(fig)
        print(f"  wrote {path}")


def plot_add_vs_cancel(df, out_dir):
    """Compare add vs cancel latency per phase, dense workload only (op_type breakdown)."""
    workload = "dense"
    wl_df = df[df["workload"] == workload]

    fig, ax = plt.subplots(figsize=(8, 5))
    x = np.arange(len(PHASE_ORDER))
    width = 0.35

    for i, op in enumerate(["add", "cancel"]):
        means = []
        for phase in PHASE_ORDER:
            vals = wl_df.loc[(wl_df["phase"] == phase) & (wl_df["op_type"] == op),
                              "latency_ns"].to_numpy()
            filt = iqr_filter(vals)
            means.append(filt.mean() if len(filt) else 0)
        ax.bar(x + (i - 0.5) * width, means, width, label=op.capitalize())

    ax.set_xticks(x)
    ax.set_xticklabels([PHASE_LABELS[p].replace("\n", " ") for p in PHASE_ORDER])
    ax.set_ylabel("Mean latency (ns, IQR-filtered)")
    ax.set_title(f"Add vs cancel latency by phase — {workload} workload")
    ax.legend()
    ax.grid(axis="y", alpha=0.3)
    fig.tight_layout()
    path = os.path.join(out_dir, "add_vs_cancel_dense.png")
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"  wrote {path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--raw", default="raw_latencies.csv", help="path to raw_latencies.csv")
    parser.add_argument("--out", default="plots", help="output directory for PNGs")
    args = parser.parse_args()

    os.makedirs(args.out, exist_ok=True)
    df = load_raw(args.raw)

    print("Generating plots...")
    plot_percentile_bars(df, args.out)
    plot_histograms(df, args.out)
    plot_cdf(df, args.out)
    plot_timeseries(df, args.out)
    plot_add_vs_cancel(df, args.out)
    print(f"\nDone. Plots written to: {os.path.abspath(args.out)}")


if __name__ == "__main__":
    main()