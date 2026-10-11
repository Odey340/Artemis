#!/usr/bin/env python3
"""Summarise (and optionally plot) an Artemis back-test.

    python3 py/read_results.py [output/results]        # prefix used by `artemis backtest`

Prints the summary JSON and a per-kind trade breakdown. If matplotlib is
installed, also saves <prefix>_equity.png.
"""

import csv
import json
import sys
from collections import defaultdict


def main():
    prefix = sys.argv[1] if len(sys.argv) > 1 else "output/results"
    with open(prefix + "_summary.json") as f:
        summary = json.load(f)
    print(json.dumps(summary, indent=2))

    by_kind = defaultdict(lambda: {"n": 0, "locked": 0.0, "realized": 0.0, "status": defaultdict(int)})
    try:
        with open(prefix + "_trades.csv") as f:
            for row in csv.DictReader(f):
                k = by_kind[row["kind"]]
                k["n"] += 1
                k["locked"] += float(row["locked_profit"])
                k["realized"] += float(row["realized_pnl"])
                k["status"][row["status"]] += 1
    except FileNotFoundError:
        pass
    if by_kind:
        print(f"\n{'kind':<12} {'trades':>6} {'locked $':>10} {'realized $':>11}  statuses")
        for kind, k in sorted(by_kind.items()):
            st = ", ".join(f"{s}={n}" for s, n in sorted(k["status"].items()))
            print(f"{kind:<12} {k['n']:>6} {k['locked']:>10.2f} {k['realized']:>11.2f}  {st}")

    ts, eq = [], []
    with open(prefix + "_equity.csv") as f:
        for row in csv.DictReader(f):
            ts.append(int(row["timestamp_us"]) / 1e6)
            eq.append(float(row["equity"]))
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        from datetime import datetime, timezone

        x = [datetime.fromtimestamp(t, tz=timezone.utc) for t in ts]
        fig, ax = plt.subplots(figsize=(10, 4))
        ax.step(x, eq, where="post", color="#2563eb", linewidth=1.5)
        ax.set_title("Artemis equity (marked to guaranteed settlement value)")
        ax.set_ylabel("USD")
        ax.grid(alpha=0.3)
        fig.autofmt_xdate()
        fig.tight_layout()
        fig.savefig(prefix + "_equity.png", dpi=120)
        print(f"\nsaved {prefix}_equity.png")
    except ImportError:
        print("\n(matplotlib not installed: skipping plot)")


if __name__ == "__main__":
    main()
