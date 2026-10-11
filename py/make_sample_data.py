#!/usr/bin/env python3
"""Generate a synthetic order-book recording for back-testing Artemis.

The output has exactly the format written by `artemis record`:
    data/sample_books.csv                 one row per book per poll
    data/sample_books.csv.universe.json   the events / pairs being tracked

Nothing here is real market data. Fair values follow random walks, market
makers quote around them, and every so often a quote goes stale or an
aggressive order lands, opening a short-lived arbitrage. This is enough to
exercise the detector, sizing, latency model and leg-risk handling end to end.

    python3 py/make_sample_data.py [--hours 1] [--step 3] [--seed 7]
"""

import argparse
import json
import math
import os
import random

ONE = 1_000_000  # micros per dollar
CENT = 10_000


def clamp(x, lo, hi):
    return max(lo, min(hi, x))


def ladder(best_cents, side, rng, levels=3):
    """Bid ladder (side=-1, descending) or ask ladder (side=+1, ascending)."""
    out = []
    for i in range(levels):
        p = best_cents + side * i
        if 1 <= p <= 99:
            out.append((p * CENT, rng.randint(20, 400) * 100))
    return out


def enc(levels):
    return "|".join(f"{p}:{q}" for p, q in levels)


class Event:
    def __init__(self, ticker, series, title, names, exhaustive, rng):
        self.ticker, self.series, self.title = ticker, series, title
        self.names = names
        self.exhaustive = exhaustive
        w = [rng.random() + 0.2 for _ in names]
        mass = 1.0 if exhaustive else 0.97  # non-exhaustive: 3% on "other"
        s = sum(w)
        self.fair = [mass * x / s for x in w]
        self.anomaly = None  # (kind, market index, ticks left, size cents)

    def tickers(self):
        return [f"{self.ticker}-{n}" for n in self.names]

    def step(self, rng):
        # log-space random walk, renormalised to the event's total mass
        mass = sum(self.fair)
        logs = [math.log(max(f, 1e-4)) + rng.gauss(0, 0.01) for f in self.fair]
        ex = [math.exp(x) for x in logs]
        s = sum(ex)
        self.fair = [mass * x / s for x in ex]
        if self.anomaly:
            k, i, left, sz = self.anomaly
            self.anomaly = (k, i, left - 1, sz) if left > 1 else None
        elif rng.random() < 0.012:
            kind = "yes_ask_drop" if (self.exhaustive and rng.random() < 0.5) else "yes_bid_jump"
            self.anomaly = (kind, rng.randrange(len(self.names)), rng.randint(1, 5), rng.randint(6, 14))

    def books(self, rng):
        rows = []
        for i, t in enumerate(self.tickers()):
            f = self.fair[i]
            half = rng.choice([1, 1, 2])
            yb = clamp(int(math.floor(f * 100 - half)), 1, 98)
            nb = clamp(int(math.floor((1 - f) * 100 - half)), 1, 98)
            if yb + nb >= 100:
                nb = 99 - yb
            yes_bids = ladder(yb, -1, rng)
            no_bids = ladder(nb, -1, rng)
            if self.anomaly and self.anomaly[1] == i:
                kind, _, _, sz = self.anomaly
                if kind == "yes_bid_jump":  # someone pays up for YES -> cheap NO
                    p = clamp(yb + sz, 2, 98)
                    if p + nb >= 100:
                        nb = 99 - p
                        no_bids = ladder(nb, -1, rng)
                    yes_bids = [(p * CENT, rng.randint(15, 90) * 100)] + yes_bids
                else:  # a NO bid jumps -> cheap YES ask
                    p = clamp(nb + sz, 2, 98)
                    if p + yb >= 100:
                        yb = 99 - p
                        yes_bids = ladder(yb, -1, rng)
                    no_bids = [(p * CENT, rng.randint(15, 90) * 100)] + no_bids
            rows.append(("kalshi", t, yes_bids, [], no_bids, []))
        return rows


class Pair:
    """A Kalshi market and the same proposition on Polymarket."""

    def __init__(self, pid, kalshi_ticker, rng):
        self.id, self.kalshi = pid, kalshi_ticker
        self.p = 0.45
        self.basis = 0.0
        self.shock = 0  # ticks left
        self.shock_size = 0.0

    def step(self, rng):
        self.p = clamp(self.p + rng.gauss(0, 0.004), 0.05, 0.95)
        self.basis += -0.2 * self.basis + rng.gauss(0, 0.004)
        if self.shock:
            self.shock -= 1
        elif rng.random() < 0.01:
            self.shock = rng.randint(1, 4)
            self.shock_size = rng.choice([-1, 1]) * rng.uniform(0.04, 0.08)

    def books(self, rng):
        k = self.p
        pm = clamp(self.p + self.basis + (self.shock_size if self.shock else 0.0), 0.02, 0.98)
        yb = clamp(int(math.floor(k * 100 - 1)), 1, 98)
        nb = clamp(int(math.floor((1 - k) * 100 - 1)), 1, 98)
        k_yes_bids, k_no_bids = ladder(yb, -1, rng), ladder(nb, -1, rng)
        kalshi = ("kalshi", self.kalshi, k_yes_bids, [], k_no_bids, [])
        # Polymarket quotes in 0.1c ticks; YES and NO token books
        pyb = clamp(round(pm * 1000) - 5, 10, 985)
        pya = pyb + rng.choice([5, 10])
        y_bids = [((pyb - 10 * i) * 1000, rng.randint(50, 900) * 100) for i in range(3)]
        y_asks = [((pya + 10 * i) * 1000, rng.randint(50, 900) * 100) for i in range(3)]
        n_bids = [(ONE - p, q) for p, q in y_asks]
        n_asks = [(ONE - p, q) for p, q in y_bids]
        poly = ("polymarket", self.id, y_bids, y_asks, n_bids, n_asks)
        return [kalshi, poly]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hours", type=float, default=3.0)
    ap.add_argument("--step", type=float, default=3.0, help="seconds between polls")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--out", default="data/sample_books.csv")
    a = ap.parse_args()
    rng = random.Random(a.seed)

    events = [
        Event("KXSAMPLEFED-26DEC", "KXSAMPLEFED", "Fed decision in December (synthetic)",
              ["C50", "C25", "H0", "H25"], True, rng),
        Event("KXSAMPLEAWARD-27", "KXSAMPLEAWARD", "Best Picture winner (synthetic)",
              ["A", "B", "C", "D", "E", "F", "G", "H"], False, rng),
        Event("KXSAMPLECITY-26", "KXSAMPLECITY", "Hottest city tomorrow (synthetic)",
              ["NYC", "CHI", "MIA", "LAX", "DAL"], True, rng),
    ]
    pairs = [Pair("btc-100k-dec", "KXSAMPLEBTC-26DEC-T100000", rng)]

    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    t0 = 1_791_000_000_000_000  # an arbitrary epoch in microseconds
    n = int(a.hours * 3600 / a.step)
    with open(a.out, "w") as f:
        f.write("ts_us,venue,id,yes_bids,yes_asks,no_bids,no_asks\n")
        for k in range(n):
            ts = t0 + int(k * a.step * 1_000_000)
            for e in events:
                e.step(rng)
                for venue, mid, yb, ya, nb, na in e.books(rng):
                    f.write(f"{ts},{venue},{mid},{enc(yb)},{enc(ya)},{enc(nb)},{enc(na)}\n")
            for p in pairs:
                p.step(rng)
                for venue, mid, yb, ya, nb, na in p.books(rng):
                    f.write(f"{ts},{venue},{mid},{enc(yb)},{enc(ya)},{enc(nb)},{enc(na)}\n")

    universe = {
        "built_us": t0,
        "events": [
            {
                "event_ticker": e.ticker,
                "series_ticker": e.series,
                "title": e.title,
                "mutually_exclusive": True,
                "exhaustive": e.exhaustive,
                "fee_type": "quadratic",
                "fee_multiplier": 1.0,
                "markets": [{"ticker": t, "title": nm, "status": "active", "close_ts_us": 0}
                            for t, nm in zip(e.tickers(), e.names)],
            }
            for e in events
        ],
        "pairs": [
            {"id": p.id, "label": "BTC above $100k on Dec 31 (synthetic)",
             "kalshi_ticker": p.kalshi, "poly_yes_token": "SYNTH-YES", "poly_no_token": "SYNTH-NO",
             "invert": False}
            for p in pairs
        ],
    }
    with open(a.out + ".universe.json", "w") as f:
        json.dump(universe, f, indent=2)
    print(f"wrote {n} polls to {a.out} (+ .universe.json)")


if __name__ == "__main__":
    main()
