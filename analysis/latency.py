"""Latency analysis for the matchbook benchmark, using pandas.

    ./build/bench -n 1000000 --csv build/latency.csv
    python3 analysis/latency.py build/latency.csv [--plot build/latency.png]

Prints per-operation percentiles, how often each op type blows a latency
budget, and how the p99 drifts over the run. Spikes that hit every op type at
once usually come from the OS (timer interrupts, context switches, page
faults), not from the matching code, which is why the tail is reported
separately from the median.
"""
from __future__ import annotations

import argparse
import sys

import pandas as pd

PERCENTILES = [0.5, 0.9, 0.99, 0.999]


def summarize(df: pd.DataFrame) -> pd.DataFrame:
    grouped = df.groupby("kind", observed=True)["latency_ns"]
    table = grouped.quantile(PERCENTILES).unstack()
    table.columns = [f"p{q * 100:g}" for q in PERCENTILES]
    table.insert(0, "ops", grouped.size())
    table.insert(1, "mean", grouped.mean().round(1))
    table["max"] = grouped.max()
    overall = df["latency_ns"]
    table.loc["all"] = [len(df), round(overall.mean(), 1), *overall.quantile(PERCENTILES), overall.max()]
    return table.astype({"ops": int})


def budget_misses(df: pd.DataFrame, budgets_ns: list[int]) -> pd.DataFrame:
    out = pd.DataFrame({f"> {b} ns": df.groupby("kind", observed=True)["latency_ns"].apply(lambda s, b=b: (s > b).mean() * 100) for b in budgets_ns})
    return out.map(lambda pct: f"{pct:.3f}%")


def rolling_p99(df: pd.DataFrame, window: int) -> pd.Series:
    return df["latency_ns"].rolling(window, min_periods=window).quantile(0.99).iloc[window - 1 :: window]


def plot(df: pd.DataFrame, rolling: pd.Series, path: str) -> None:
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not installed; skipping plot (pip install -r analysis/requirements.txt)", file=sys.stderr)
        return

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 4.5))
    cap = df["latency_ns"].quantile(0.999)
    for kind, group in df.groupby("kind", observed=True):
        ax1.hist(group["latency_ns"].clip(upper=cap), bins=80, histtype="step", label=kind, log=True)
    ax1.set_xlabel("latency (ns, clipped at p99.9)")
    ax1.set_ylabel("ops (log scale)")
    ax1.set_title("Latency distribution by operation")
    ax1.legend()

    ax2.plot(rolling.index, rolling.values)
    ax2.set_xlabel("operation #")
    ax2.set_ylabel("p99 latency (ns)")
    ax2.set_title("Rolling p99")
    fig.tight_layout()
    fig.savefig(path, dpi=120)
    print(f"wrote {path}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("csv")
    parser.add_argument("--plot", help="write a PNG histogram + rolling p99 chart")
    parser.add_argument("--window", type=int, default=50_000, help="ops per rolling-p99 window")
    args = parser.parse_args()

    df = pd.read_csv(args.csv, dtype={"kind": "category", "side": "category"})
    pd.set_option("display.width", 120)

    print("Latency by operation (ns)\n")
    print(summarize(df).to_string(float_format=lambda v: f"{v:,.0f}"))

    print("\nShare of ops over latency budget\n")
    print(budget_misses(df, [250, 1_000, 10_000]).to_string())

    rolling = rolling_p99(df, args.window)
    print(f"\nRolling p99 over {args.window:,}-op windows: "
          f"min {rolling.min():,.0f} ns, median {rolling.median():,.0f} ns, max {rolling.max():,.0f} ns")

    worst = df.nlargest(5, "latency_ns")[["seq", "kind", "latency_ns"]]
    print("\nSlowest 5 operations\n")
    print(worst.to_string(index=False))

    if args.plot:
        plot(df, rolling, args.plot)
    return 0


if __name__ == "__main__":
    sys.exit(main())
