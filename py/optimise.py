#!/usr/bin/env python3
"""Grid-search strategy thresholds against a recording.

    python3 py/optimise.py data/sample_books.csv [--config config/backtest.sample.json]

Runs `artemis backtest` for every combination of minimum profit, minimum
edge and confirmation count at several latencies, and ranks the settings by
P&L at the *slowest* latency tested (robustness over best case). Results are
written to output/optimisation.csv.

A grid search on one recording will over-fit it; use the output to find
settings that are stable across latencies, then confirm on fresh recordings.
"""

import argparse
import csv
import itertools
import json
import os
import re
import subprocess
import tempfile

GRID = {
    "min_profit_usd": [0.10, 0.25, 0.50],
    "min_edge_cents": [0.0, 0.5, 1.0],
    "confirmations": [1, 2],
}
LATENCIES = [100, 250, 1000]


def load_jsonc(path):
    text = open(path).read()
    text = re.sub(r"(?m)^\s*//.*$", "", text)
    text = re.sub(r"\s//[^\n\"]*$", "", text, flags=re.M)
    return json.loads(text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("recording")
    ap.add_argument("--config", default="config/backtest.sample.json")
    ap.add_argument("--binary", default="build/artemis")
    a = ap.parse_args()

    base = load_jsonc(a.config)
    os.makedirs("output", exist_ok=True)
    rows = []
    keys = list(GRID)
    with tempfile.TemporaryDirectory() as tmp:
        for combo in itertools.product(*(GRID[k] for k in keys)):
            params = dict(zip(keys, combo))
            cfg = json.loads(json.dumps(base))
            cfg.setdefault("strategy", {}).update(params)
            cfg.setdefault("output", {})["log_file"] = ""
            cpath = os.path.join(tmp, "cfg.json")
            json.dump(cfg, open(cpath, "w"))
            result = dict(params)
            for lat in LATENCIES:
                prefix = os.path.join(tmp, f"r{lat}")
                subprocess.run([a.binary, "backtest", a.recording, "-c", cpath, "--latency", str(lat),
                                "--out", prefix], check=True, capture_output=True)
                s = json.load(open(prefix + "_summary.json"))
                result[f"pnl_{lat}ms"] = round(s["total_pnl_usd"], 2)
                result[f"trades_{lat}ms"] = s["executions"]
                result[f"failures_{lat}ms"] = s["failures"]
            rows.append(result)
            print(result, flush=True)

    worst = f"pnl_{max(LATENCIES)}ms"
    rows.sort(key=lambda r: r[worst], reverse=True)
    with open("output/optimisation.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    print(f"\nbest at {max(LATENCIES)} ms latency: {rows[0]}")
    print("full table: output/optimisation.csv")


if __name__ == "__main__":
    main()
