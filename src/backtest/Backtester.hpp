#pragma once
// Deterministic replay of recorded order books through the *same* detector,
// risk manager and basket executor used live, with a configurable
// decision-to-fill latency: orders are filled against the books as they
// stood `latency_ms` after the opportunity was seen.

#include <map>
#include <string>
#include <vector>

#include "backtest/Performance.hpp"
#include "core/Config.hpp"

namespace artemis {

struct BacktestResult {
    PerformanceSummary perf;
    std::vector<EquityPoint> equity;
    long long rows = 0;
    long long ticks = 0;
    long long opportunities = 0;
    long long executions = 0;
    long long completed = 0;
    long long failures = 0;
    long long notFilled = 0;
    long long rejected = 0;
    long long unwound = 0;
    long long stranded = 0;
    std::map<std::string, long long> rejectReasons;
    double expectedProfit = 0.0;  // USD at detection time
    double lockedProfit = 0.0;    // USD
    double realizedPnl = 0.0;     // USD (unwinds)
    double captureRatio = 0.0;    // (locked + realized) / expected over executions
    double evalMeanUs = 0.0;      // tick-to-signal latency
    double evalP99Us = 0.0;
    double wallSeconds = 0.0;
    double rowsPerSec = 0.0;
};

class Backtester {
public:
    explicit Backtester(Config cfg) : cfg_(std::move(cfg)) {}
    // When set, trades are journaled to <prefix>_trades.csv during the run.
    void setOutputPrefix(std::string p) { prefix_ = std::move(p); }
    // `universePath` defaults to csvPath + ".universe.json".
    BacktestResult run(const std::string& csvPath, const std::string& universePath = {});
    // Writes <prefix>_equity.csv, <prefix>_trades.csv and <prefix>_summary.json.
    void writeOutputs(const BacktestResult& r, const std::string& prefix) const;

private:
    Config cfg_;
    std::string prefix_;
};

}  // namespace artemis
