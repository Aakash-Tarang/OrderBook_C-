#!/usr/bin/env python3
"""
plot_bench.py – Generates the report figures from the benchmark outputs.

Inputs (produced by ob_bench / matching_engine):
  results/bench_raw.csv       engine,workload,op_index,op_type,latency_ns
  results/bench_summary.csv   engine,workload,op,n,mean_ns,p50_ns,...
  results/pipeline_raw.csv    op_index,op_latency_ns,tick_to_trade_ns

Outputs (committed to the repo, used by docs/report/report.tex):
  docs/report/figures/bench-percentiles.png   per-workload percentile bars
  docs/report/figures/bench-cdf.png           latency CDFs, log-x
  docs/report/figures/pipeline-latencies.png  op vs tick-to-trade CDF

No outlier filtering anywhere – raw samples only (report §7).
"""

import argparse
from pathlib import Path

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

COLORS = {"fast": "#2A6F97", "ref": "#BC4749"}
WORKLOADS = ["dense", "sparse"]


def pct(sorted_arr, p):
    return float(np.percentile(sorted_arr, p))


def load_raw(path):
    """bench_raw.csv: engine,workload,op_index,op_type,latency_ns"""
    data = {}
    with open(path) as fh:
        fh.readline()
        for line in fh:
            engine, workload, _idx, _type, ns = line.strip().split(",")
            data.setdefault((engine, workload), []).append(float(ns))
    return {k: np.array(v) for k, v in data.items()}


def load_summary(path):
    rows = {}
    with open(path) as fh:
        header = fh.readline().strip().split(",")
        for line in fh:
            vals = line.strip().split(",")
            row = dict(zip(header, vals))
            rows[(row["engine"], row["workload"], row["op"])] = row
    return rows


def fig_percentiles(raw, summary, out):
    """Grouped percentile bars: fast vs reference, dense vs sparse."""
    pcts = [50, 90, 99, 99.9]
    engines = sorted({k[0] for k in raw})
    workloads = [w for w in WORKLOADS if any(k[1] == w for k in raw)]

    fig, axes = plt.subplots(1, len(workloads), figsize=(6.4 * len(workloads), 4.2))
    if len(workloads) == 1:
        axes = [axes]

    for ax, wl in zip(axes, workloads):
        width = 0.8 / len(engines)
        xs = np.arange(len(pcts))
        for i, eng in enumerate(engines):
            try:
                s = np.sort(raw[(eng, wl)])
            except KeyError:
                continue
            vals = [pct(s, p) for p in pcts]
            label = "this engine (ob::OrderBook)" if eng == "fast" else "naive reference (std::map)"
            ax.bar(xs + i * width, vals, width * 0.92, label=label,
                   color=COLORS.get(eng, "#777"))
        ax.set_xticks(xs + width * (len(engines) - 1) / 2)
        ax.set_xticklabels([f"P{p:g}" for p in pcts])
        ax.set_ylabel("latency (ns, raw – unfiltered)")
        ax.set_title(f"{wl} workload")
        ax.set_yscale("log")
        ax.grid(axis="y", alpha=0.3, which="both")
        ax.legend(fontsize=8)

    fig.suptitle("OrderBook operation latency percentiles (same machine, same workloads)")
    fig.tight_layout()
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"wrote {out}")


def fig_cdf(raw, out):
    fig, axes = plt.subplots(1, 2, figsize=(12.8, 4.2))
    for ax, wl in zip(axes, WORKLOADS):
        for eng in sorted({k[0] for k in raw}):
            if (eng, wl) not in raw:
                continue
            s = np.sort(raw[(eng, wl)])
            cdf = np.arange(1, len(s) + 1) / len(s)
            label = "this engine" if eng == "fast" else "naive reference"
            ax.plot(s, cdf, label=label, color=COLORS.get(eng, "#777"), linewidth=1.4)
        ax.set_xscale("log")
        ax.set_xlabel("latency (ns, log scale)")
        ax.set_ylabel("CDF")
        ax.set_title(f"{wl} workload")
        ax.grid(alpha=0.3, which="both")
        ax.legend(fontsize=8)
    fig.suptitle("Latency distribution (raw samples, no outlier removal)")
    fig.tight_layout()
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"wrote {out}")


def fig_pipeline(raw_path, out):
    """CDFs of engine-op latency vs tick-to-trade in the SPSC pipeline."""
    op, ttt = [], []
    with open(raw_path) as fh:
        fh.readline()
        for line in fh:
            _i, op_ns, ttt_ns = line.strip().split(",")
            op.append(float(op_ns))
            ttt.append(float(ttt_ns))
    if not op:
        print("no pipeline samples; skipping")
        return

    fig, ax = plt.subplots(figsize=(6.8, 4.2))
    for arr, label, color in ((op, "engine op latency (pop \u2192 done)", "#2A6F97"),
                              (ttt, "tick-to-trade (enqueue \u2192 done)", "#E76F51")):
        s = np.sort(arr)
        cdf = np.arange(1, len(s) + 1) / len(s)
        ax.plot(s, cdf, label=label, color=color, linewidth=1.4)

    ax.set_xscale("log")
    ax.set_xlabel("latency (ns, log scale)")
    ax.set_ylabel("CDF")
    ax.set_title("SPSC pipeline: op latency vs end-to-end tick-to-trade")
    ax.grid(alpha=0.3, which="both")
    ax.legend(fontsize=9)
    fig.tight_layout()
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print(f"wrote {out}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bench-raw", default="results/bench_raw.csv")
    ap.add_argument("--bench-summary", default="results/bench_summary.csv")
    ap.add_argument("--pipeline-raw", default="results/pipeline_raw.csv")
    ap.add_argument("--figures", default="docs/report/figures")
    args = ap.parse_args()

    figdir = Path(args.figures)
    figdir.mkdir(parents=True, exist_ok=True)

    raw = {}
    if Path(args.bench_raw).exists():
        raw = load_raw(args.bench_raw)
        summary = load_summary(args.bench_summary) if Path(args.bench_summary).exists() else {}
        fig_percentiles(raw, summary, figdir / "bench-percentiles.png")
        fig_cdf(raw, figdir / "bench-cdf.png")
    else:
        print(f"missing {args.bench_raw}; run ob_bench first")

    if Path(args.pipeline_raw).exists():
        fig_pipeline(args.pipeline_raw, figdir / "pipeline-latencies.png")
    else:
        print(f"missing {args.pipeline_raw}; run matching_engine first")


if __name__ == "__main__":
    main()
