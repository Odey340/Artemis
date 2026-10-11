#include "backtest/Backtester.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#include "core/Clock.hpp"
#include "core/FixedPoint.hpp"
#include "engine/Journal.hpp"
#include "engine/Universe.hpp"
#include "exec/BasketExecutor.hpp"
#include "exec/OrderGateway.hpp"
#include "risk/RiskManager.hpp"
#include "strategy/ArbDetector.hpp"
#include "strategy/BookStore.hpp"

namespace artemis {

namespace {
double toUsd(Micros m) { return static_cast<double>(m) / kOneDollar; }
}  // namespace

BacktestResult Backtester::run(const std::string& csvPath, const std::string& universePathIn) {
    const std::string universePath =
        universePathIn.empty() ? csvPath + ".universe.json" : universePathIn;
    std::ifstream uf(universePath);
    if (!uf) throw std::runtime_error("cannot open universe file: " + universePath);
    std::stringstream us;
    us << uf.rdbuf();
    UniverseSnapshot uni = UniverseSnapshot::fromJson(nlohmann::json::parse(us.str()));
    // apply operator configuration that is not part of the recording
    for (auto& e : uni.events) {
        if (matchesAny(e.eventTicker, cfg_.universe.exhaustiveEvents) ||
            matchesAny(e.seriesTicker, cfg_.universe.exhaustiveSeries))
            e.exhaustive = true;
    }

    std::ifstream in(csvPath);
    if (!in) throw std::runtime_error("cannot open recording: " + csvPath);
    std::vector<BinaryBook> rows;
    std::string line;
    while (std::getline(in, line)) {
        BinaryBook b;
        if (BookRecorder::parse(line, b)) rows.push_back(std::move(b));
    }
    std::stable_sort(rows.begin(), rows.end(), [](const BinaryBook& a, const BinaryBook& b) {
        return a.tsUs() < b.tsUs();
    });

    BacktestResult res;
    res.rows = static_cast<long long>(rows.size());
    if (rows.empty()) return res;

    BookStore store;
    ArbDetector detector(cfg_.strategy, cfg_.feed, cfg_.polymarket, cfg_.basis);
    std::unordered_map<std::string, FeeModel> fees;
    for (const auto& [t, m] : uni.tickerFeeMultiplier)
        fees[t] = FeeModel::kalshi(cfg_.strategy.kalshiTakerRate, m);
    detector.setTickerFees(std::move(fees));
    RiskManager risk(cfg_.risk);
    const Micros startEq =
        static_cast<Micros>(std::llround(cfg_.backtest.startingEquityUsd * kOneDollar));
    risk.setAvailableBalance(startEq);
    SimGateway sim(store);
    BasketExecutor exec(sim, cfg_.execution, cfg_.strategy);

    std::unique_ptr<TradeLog> tradeLog;
    if (!prefix_.empty()) {
        const std::string p = prefix_ + "_trades.csv";
        std::error_code ec;
        std::filesystem::remove(p, ec);
        tradeLog = std::make_unique<TradeLog>(p);
    }

    Micros equity = startEq;
    res.equity.push_back({rows.front().tsUs(), toUsd(equity)});
    std::vector<double> evalSamples;
    const std::int64_t latencyUs = static_cast<std::int64_t>(cfg_.backtest.latencyMs) * 1000;

    auto markDirty = [&](const BinaryBook& b, std::unordered_set<std::string>& dirty) {
        if (b.venue() == Venue::Kalshi) {
            auto it = uni.tickerToEvent.find(b.id());
            if (it != uni.tickerToEvent.end()) dirty.insert(it->second);
            auto pit = uni.tickerToPairs.find(b.id());
            if (pit != uni.tickerToPairs.end())
                for (const auto& id : pit->second) dirty.insert(id);
        } else {
            dirty.insert(b.id());
        }
    };

    const auto wall0 = std::chrono::steady_clock::now();
    std::size_t i = 0;
    std::unordered_set<std::string> carry;  // groups touched while "busy" executing
    std::int64_t clockUs = rows.front().tsUs();
    // advance the replay (apply every row up to `untilUs`) without evaluating
    auto advanceTo = [&](std::int64_t untilUs) {
        clockUs = std::max(clockUs, untilUs);
        while (i < rows.size() && rows[i].tsUs() <= clockUs) {
            markDirty(rows[i], carry);
            store.put(rows[i]);
            ++i;
        }
    };
    const std::int64_t legLatencyUs = static_cast<std::int64_t>(cfg_.backtest.legLatencyMs) * 1000;
    bool firstOrder = true;
    sim.setBeforeSubmit([&] {
        // the first order of a basket already paid the decision latency
        if (!firstOrder) advanceTo(clockUs + legLatencyUs);
        firstOrder = false;
    });
    while (i < rows.size()) {
        const std::int64_t ts = rows[i].tsUs();
        clockUs = std::max(clockUs, ts);
        std::unordered_set<std::string> dirty;
        dirty.swap(carry);
        while (i < rows.size() && rows[i].tsUs() == ts) {
            markDirty(rows[i], dirty);
            store.put(rows[i]);
            ++i;
        }
        ++res.ticks;
        if (dirty.empty()) continue;

        const std::int64_t t0 = monoUs();
        auto found = detector.scan(uni.events, uni.pairs, store, ts, &dirty);
        evalSamples.push_back(static_cast<double>(monoUs() - t0));

        for (auto& opp : found) {
            ++res.opportunities;
            const RiskDecision d = risk.check(opp, clockUs);
            if (!d.approved) {
                ++res.rejected;
                ++res.rejectReasons[d.reason.substr(0, d.reason.find(" for "))];
                continue;
            }
            const Micros perUnit = (opp.cost + opp.fees) * kOneContract / opp.units;
            Qty cap = perUnit > 0 ? (d.maxCost / perUnit) * kOneContract : opp.units;
            cap = std::min(cap, opp.units);
            if (cap < kOneContract) {
                ++res.rejected;
                ++res.rejectReasons["budget below one unit"];
                continue;
            }
            if (cap < opp.units) {
                auto smaller = detector.resize(opp, store, cap);
                if (!smaller) {
                    ++res.rejected;
                    ++res.rejectReasons["unprofitable at budget size"];
                    continue;
                }
                opp = std::move(*smaller);
            }

            // latency: the market keeps moving while our orders are in flight
            advanceTo(clockUs + latencyUs);
            firstOrder = true;
            const ExecutionReport rep = exec.execute(opp, opp.units);
            const std::int64_t fillTs = clockUs;
            risk.onExecution(opp, rep, fillTs);
            if (tradeLog && rep.status != ExecStatus::Skipped) tradeLog->write(fillTs, opp, rep);
            if (rep.status == ExecStatus::NotFilled) {
                ++res.notFilled;
                continue;
            }
            if (!rep.traded()) continue;
            ++res.executions;
            if (rep.status == ExecStatus::Completed || rep.status == ExecStatus::Shrunk)
                ++res.completed;
            if (rep.failure()) ++res.failures;
            if (rep.status == ExecStatus::Unwound) ++res.unwound;
            if (rep.status == ExecStatus::Stranded) ++res.stranded;
            // profit the detector expected (at decision time, for the size attempted)
            res.expectedProfit += toUsd(opp.profit);
            res.lockedProfit += toUsd(rep.lockedProfit);
            res.realizedPnl += toUsd(rep.realizedPnl);
            // mark-to-guaranteed-settlement equity
            equity += rep.lockedProfit + rep.realizedPnl - rep.strandedCost;
            res.equity.push_back({fillTs, toUsd(equity)});
            const Micros execLoss = rep.realizedPnl + std::min<Micros>(0, rep.lockedProfit);
            if (execLoss < -static_cast<Micros>(
                               std::llround(cfg_.execution.maxLegLossUsd * kOneDollar)))
                risk.halt("single execution loss above max_leg_loss_usd");
            if (risk.halted()) {
                spdlog::warn("backtest: risk halted at {}: {}", fillTs, risk.haltReason());
            }
        }
    }
    res.wallSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
    res.equity.push_back({rows.back().tsUs(), toUsd(equity)});
    res.rowsPerSec = res.wallSeconds > 0 ? static_cast<double>(res.rows) / res.wallSeconds : 0.0;
    res.perf = computePerformance(res.equity, 900.0);  // 15-minute buckets
    if (!evalSamples.empty()) {
        double sum = 0;
        for (double v : evalSamples) sum += v;
        res.evalMeanUs = sum / static_cast<double>(evalSamples.size());
        res.evalP99Us = percentile(evalSamples, 0.99);
    }
    res.captureRatio =
        res.expectedProfit > 0 ? (res.lockedProfit + res.realizedPnl) / res.expectedProfit : 0.0;
    return res;
}

void Backtester::writeOutputs(const BacktestResult& r, const std::string& prefix) const {
    ensureParentDir(prefix + "_equity.csv");
    {
        std::ofstream f(prefix + "_equity.csv");
        f << "timestamp_us,equity\n";
        f.setf(std::ios::fixed);
        f.precision(4);
        for (const auto& p : r.equity) f << p.tsUs << ',' << p.equity << '\n';
    }
    nlohmann::json j = {
        {"rows", r.rows},
        {"ticks", r.ticks},
        {"opportunities", r.opportunities},
        {"executions", r.executions},
        {"completed", r.completed},
        {"failures", r.failures},
        {"not_filled", r.notFilled},
        {"rejected_by_risk", r.rejected},
        {"reject_reasons", r.rejectReasons},
        {"unwound", r.unwound},
        {"stranded", r.stranded},
        {"leg_latency_ms", cfg_.backtest.legLatencyMs},
        {"expected_profit_usd", r.expectedProfit},
        {"locked_profit_usd", r.lockedProfit},
        {"realized_pnl_usd", r.realizedPnl},
        {"capture_ratio", r.captureRatio},
        {"start_equity_usd", r.perf.startEquity},
        {"end_equity_usd", r.perf.endEquity},
        {"total_pnl_usd", r.perf.totalPnl},
        {"return_pct", r.perf.returnPct},
        {"sharpe_annualised_15min_buckets", r.perf.sharpe},
        {"max_drawdown_usd", r.perf.maxDrawdown},
        {"max_drawdown_pct", r.perf.maxDrawdownPct},
        {"eval_mean_us", r.evalMeanUs},
        {"eval_p99_us", r.evalP99Us},
        {"rows_per_sec", r.rowsPerSec},
        {"latency_ms", cfg_.backtest.latencyMs},
    };
    std::ofstream(prefix + "_summary.json") << j.dump(2) << '\n';
}

}  // namespace artemis
