# Artemis: Kalshi arbitrage engine (C++17)

Artemis watches every open Kalshi event, plus any Kalshi markets you map to Polymarket. It looks for baskets of contracts whose guaranteed payout is worth more than their price after fees. It sizes each trade against real order-book depth and executes it with fill-or-kill orders, managing the risk of one leg filling without the others.

This project merges two earlier ones:

* **Artemis** (the original tick engine). From it comes the lock-free single-producer/single-consumer queue between the reader and worker threads, the EWMA rolling-statistics engine, the z-score `SignalGenerator`, the back-tester, Sharpe/drawdown reporting, spdlog, GoogleTest, the CMake build and the Windows scripts.
* **Kalshi ↔ Polymarket arbitrage bot** (feature spec). From it come Kalshi and Polymarket integration, market mapping, fee-aware net edge, risk controls, automated execution, logging, retries and safety controls.

> **Not financial advice.** This is software, not a promise of profit. Read [Before you trade real money](#before-you-trade-real-money) first.

---

## What it trades

| Strategy | Where | Condition (per contract set, after fees) | Guaranteed payout |
|---|---|---|---|
| **NO basket** | Kalshi events flagged `mutually_exclusive` | Σ NO asks over the chosen legs < (legs − 1) | ≥ legs − 1 |
| **YES basket** | Kalshi events **you** declare exhaustive | Σ YES asks over every outcome < 1 | exactly 1 |
| **Cross-venue** | Kalshi market ↔ mapped Polymarket market | YES on one venue + NO on the other < 1 | exactly 1 |

Some notes on the maths:

* **The NO basket does not need the outcome list to be exhaustive.** At most one outcome can resolve YES, so holding NO on a set *S* pays at least |S| − 1. Leg *i* adds `1 − noAsk_i − fee_i` to the margin, so the detector includes exactly the legs where that is positive. Kalshi publishes `mutually_exclusive` for each event, so this is the safest basket.
* **The YES basket is only correct if exactly one outcome must happen.** The API does not tell you that, so it is off unless you list the series or events in `exhaustive_series` / `exhaustive_events`.
* **A single Kalshi market can't be arbitraged against itself.** YES and NO trade on one book (a YES bid at *p* is a NO ask at 1 − *p*), so "YES + NO < $1" on one market is impossible.
* **Fees are rounded up per price level**, so estimates are conservative. Kalshi: `round_up(M × 0.07 × C × P × (1 − P))`, where *M* is the series or event fee multiplier fetched from the API. Polymarket: `bps × C × min(P, 1 − P)` (CLOB) or `θ × C × P × (1 − P)` (Polymarket US).
* **Sizing walks the actual depth** one contract at a time and keeps the size with the highest profit. It stops at the first contract that can't pay for itself.

## Architecture

```
            ┌──────────────────────── feed thread ────────────────────────┐
 Kalshi ───▶│ UniverseBuilder: page /events, keep ME events, rank by       │
 REST v2    │   listing top-of-book "distance to arb", resolve fee mult.   │
            │ batched GET /markets/orderbooks (rate-limited, retried)      │
 Polymarket▶│ POST /books for mapped token pairs                           │
 CLOB       └───────────────┬─────────────────────────────────────────────┘
                            │  LockFreeQueue<BinaryBook>  (SPSC, from Artemis)
            ┌───────────────▼──────────── strategy thread ────────────────┐
            │ BookStore ─▶ ArbDetector (dirty groups only, ~15 µs)         │
            │               ├─ NO / YES baskets, cross-venue               │
            │               ├─ depth-walk sizing, fee model per venue      │
            │               └─ BasisMonitor: EWMA z-score + SignalGenerator│
            │ RiskManager ─▶ BasketExecutor ─▶ OrderGateway                │
            │                                   ├─ KalshiGateway (live)    │
            │                                   └─ SimGateway (paper/bt)   │
            │ journals: opportunities.csv, trades_*.csv, book recordings   │
            └──────────────────────────────────────────────────────────────┘
```

All trading state lives on the strategy thread, so the hot path takes no locks. Paper trading, back-tests and live trading run the same detector, risk manager and executor; only the `OrderGateway` changes.

### Execution and leg risk

1. **Re-check.** Pull fresh books for every leg and re-size. If the edge has gone, nothing is sent.
2. **Fire legs** as `fill_or_kill` orders at the worst price level needed, thinnest leg first, so the leg most likely to fail fails before anything else is bought. A `batch` mode is also available.
3. **If a leg is killed,** price three ways out on fresh books and take the best:
   * **complete:** buy the missing leg at the new price (up to `max_completion_retries`)
   * **shrink:** for NO baskets, keep the fully filled subset, which is still hedged and pays |subset| − 1
   * **unwind:** sell the filled legs back, using `immediate_or_cancel` + `reduce_only` and chasing the bid up to 3 times
4. **Ambiguous results** (the network dropped mid-order) are reconciled through `client_order_id` and never blindly resubmitted. Anything still unknown is reported as *stranded*, and trading halts so a person can check.

### Risk controls

These are all in `config/*.json`:

* per-trade cost cap, total exposure cap, and a balance-utilisation cap based on the live Kalshi balance
* a daily loss limit, which counts unwind losses, locked-in losses and stranded cost
* a per-execution loss limit (`max_leg_loss_usd`)
* a halt after N consecutive failures, or on any stranded position
* a cooldown per opportunity, a maximum number of trades per hour, and a maximum number of open baskets per event
* a kill switch: create a file named `KILL` in the working directory and trading stops
* Kalshi `trading_active` is checked every minute; executions pause while the exchange is closed
* book-age limit (`max_book_age_ms`), optional N-snapshot confirmation, and a safety margin per leg
* live trading needs **both** `"mode": "live"` in the config **and** `--live` on the command line

## Quick start

### 1. Build (Linux / macOS)

```bash
# Ubuntu/Debian: sudo apt install build-essential cmake libssl-dev libcurl4-openssl-dev
# macOS:         brew install cmake openssl curl
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build          # 94 GoogleTest cases
```

spdlog, nlohmann/json and GoogleTest are fetched automatically. On Windows, see [BUILD_WINDOWS.md](BUILD_WINDOWS.md).

### 2. Back-test on the bundled synthetic recording

```bash
python3 py/make_sample_data.py                    # writes data/sample_books.csv (synthetic!)
./build/artemis backtest data/sample_books.csv -c config/backtest.sample.json
python3 py/read_results.py output/results         # summary (+ equity PNG if matplotlib is installed)
python3 py/optimise.py data/sample_books.csv      # threshold grid search across latencies
```

```
rows 68400 | ticks 3600 | latency 250 ms (+50 ms per leg)
opportunities 91 | executed 48 | completed 48 | leg failures 0 (unwound 0, stranded 0) | ...
expected $139.44 | locked $139.44 | realized $0.00 | capture 100.0%
tick-to-signal 15.1 us mean / 90.0 us p99 | 921673 rows/sec
```

Rerun with `--leg-latency 1500` to watch leg failures, completions, unwinds and the loss-limit halt happen. The sample data is **synthetic**: random walks with injected mispricings. It exercises the code; it says nothing about real-world profitability. Sharpe ratios on a curve that only books locked-in arbitrage profit are not meaningful either, because the curve barely ever goes down.

### 3. Connect to Kalshi

1. Create an API key in your Kalshi account settings (use the **demo** environment first) and download the private key `.pem`.
2. Set up the config and credentials:

   ```bash
   cp config/artemis.example.json config/artemis.json      # edit to taste
   export KALSHI_API_KEY_ID=your-key-id
   export KALSHI_PRIVATE_KEY_PATH=$HOME/.kalshi/key.pem
   ./build/artemis balance          # verifies signing: prints balance + positions
   ./build/artemis scan             # one pass over the whole exchange, prints opportunities
   ./build/artemis record -o data/books.csv -d 3600   # record an hour of real books
   ./build/artemis backtest data/books.csv            # replay them
   ./build/artemis run              # paper trading on live data (default)
   ```

3. To go live, set `"mode": "live"` and run `./build/artemis run --live`.

Output goes to `output/`: `artemis.log`, `opportunities.csv`, and `trades_paper.csv` or `trades_live.csv`.

### Dry-run without Kalshi: the mock exchange

`py/mock_kalshi.py` is a local stand-in for the Trade API. It serves an arbitrageable event, verifies every RSA-PSS signature, fills orders against its own books, and can simulate another trader taking your liquidity mid-basket:

```bash
openssl genrsa -out /tmp/k.pem 2048 && openssl rsa -in /tmp/k.pem -pubout -out /tmp/pub.pem
python3 py/mock_kalshi.py --pubkey /tmp/pub.pem --race KXMOCK-26-C --race-mode empty &
# config: "mode": "live", "kalshi": {"base_url": "http://127.0.0.1:8765/trade-api/v2"}
KALSHI_API_KEY_ID=test KALSHI_PRIVATE_KEY_PATH=/tmp/k.pem ./build/artemis run -c my_mock.json --live -d 5
```

## Before you trade real money

* **Start on demo** (`"environment": "demo"`), then paper-trade on prod data, then go live with small caps.
* **Exhaustiveness is your call.** Only list a series in `exhaustive_series` if one of its outcomes always resolves YES. One missing outcome turns a YES basket into a loss.
* **Cross-venue pairs must have identical resolution rules**: same source, same cut-off, same edge cases. "Same question" is not enough. The Polymarket leg is **detected and paper-traded only**. Live Polymarket order routing (EIP-712 wallet signing) is not implemented, and the international Polymarket exchange is not available to US persons. In live mode, cross-venue opportunities are logged as `signal-only`.
* **Capital is locked until settlement.** A basket's profit arrives when the event resolves, which can be weeks away. Size `max_total_exposure_usd` with that in mind.
* **Fees and API details change.** The fee formulas and endpoints here match Kalshi API v3.33 (October 2026). Check `kalshi_taker_rate` against the current fee schedule, and watch the logs for 4xx errors after any Kalshi API change.
* **Rate limits.** Keep `read_rate_per_sec` / `write_rate_per_sec` within your Kalshi API tier. The client backs off on HTTP 429 and honours `Retry-After`.
* **Competition.** Obvious baskets are arbitraged by fast market makers. Expect small, short-lived edges. Latency and fees decide whether you capture them, which is exactly what the back-tester's latency model measures.

## Project layout

```
src/
  core/        Types, FixedPoint (exact decimal parsing), OrderBook, LockFreeQueue,
               RollingStatistics (EWMA), RateLimiter, Config, Clock
  net/         HttpClient (libcurl, keep-alive)
  venues/      Fees; kalshi/ (RSA-PSS & Ed25519 signer, REST client); polymarket/ (CLOB books, market map)
  strategy/    ArbDetector (+ sizing), BookStore, SignalGenerator/BasisMonitor, Opportunity
  risk/        RiskManager
  exec/        BasketExecutor (leg-risk logic), OrderGateway (Kalshi live / simulator)
  engine/      Engine (threads), Universe (discovery & ranking), Journal (CSV)
  backtest/    Backtester (latency-aware replay), Performance (Sharpe, drawdown)
  main.cpp     CLI: scan | run | record | backtest | balance
tests/         GoogleTest suite (fees, sizing, executor paths, signer, parsers, risk, replay, ...)
py/            make_sample_data.py, mock_kalshi.py, read_results.py, optimise.py
config/        artemis.example.json, market_map.example.json, backtest.sample.json
```

## Extending

* **New strategy:** add an `evaluate*` method to `ArbDetector` that builds `LegSpec`s and calls `sizeBasket(...)`. Sizing, risk and execution come for free.
* **New signal on the basis:** inherit from `SignalGenerator` and override `evaluate()`.
* **New venue:** implement an `OrderGateway` (and a book source that pushes `BinaryBook`s into the queue).

## Licence

No licence file has been added yet. Choose one before sharing or accepting contributions.
