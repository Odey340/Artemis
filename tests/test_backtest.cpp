#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "TestUtil.hpp"
#include "backtest/Backtester.hpp"
#include "backtest/Performance.hpp"
#include "engine/Journal.hpp"
#include "engine/Universe.hpp"

using namespace artemis;
using namespace artemis::test;

namespace {

// Writes a 3-outcome event where an arbitrage appears at t=10s and lasts 1s.
std::string writeRecording(const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir);
    const std::string csv = (dir / "rec.csv").string();
    {
        BookRecorder rec(csv);
        for (int s = 0; s < 30; ++s) {
            const std::int64_t ts = 1'000'000'000'000LL + s * 1'000'000LL;
            const bool arb = (s == 10);
            rec.write(kbook("A", {{arb ? 40.0 : 30.0, 10}}, {{60, 50}}, ts));
            rec.write(kbook("B", {{35, 10}}, {{60, 50}}, ts));
            rec.write(kbook("C", {{35, 10}}, {{60, 50}}, ts));
        }
    }
    UniverseSnapshot u;
    EventInfo e;
    e.eventTicker = "E";
    e.seriesTicker = "S";
    e.mutuallyExclusive = true;
    for (auto t : {"A", "B", "C"}) {
        MarketInfo m;
        m.ticker = t;
        m.status = "active";
        e.markets.push_back(m);
    }
    u.events.push_back(e);
    std::ofstream(csv + ".universe.json") << u.toJson().dump();
    return csv;
}

Config cfg(int latencyMs) {
    Config c;
    c.strategy.minProfitUsd = 0.05;
    c.strategy.minEdgeCents = 0;
    c.strategy.safetyMarginCents = 0;
    c.risk.killSwitchFile = "";
    c.backtest.latencyMs = latencyMs;
    c.backtest.startingEquityUsd = 100;
    return c;
}

}  // namespace

TEST(BookRecorder, ParsesWhatItWrites) {
    const auto dir = std::filesystem::temp_directory_path() / "artemis_rec_test";
    std::filesystem::create_directories(dir);
    const std::string path = (dir / "x.csv").string();
    {
        BookRecorder r(path);
        r.write(kbook("K", {{40, 1}}, {{55, 2}}, 7));
        r.write(BinaryBook::fromTwoTokens(Venue::Polymarket, "p", L({{1, 1}}), L({{2, 2}}),
                                          L({{3, 3}}), L({{4, 4}}), 8));
    }
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    BinaryBook b;
    EXPECT_FALSE(BookRecorder::parse(line, b));  // header
    std::getline(in, line);
    ASSERT_TRUE(BookRecorder::parse(line, b));
    EXPECT_EQ(b.venue(), Venue::Kalshi);
    EXPECT_EQ(b.bestAsk(Outcome::Yes), c(45));
    std::getline(in, line);
    ASSERT_TRUE(BookRecorder::parse(line, b));
    EXPECT_EQ(b.venue(), Venue::Polymarket);
    EXPECT_EQ(b.bestAsk(Outcome::No), c(4));
    std::filesystem::remove_all(dir);
}

TEST(Backtester, CapturesArbWithinLatency) {
    const auto dir = std::filesystem::temp_directory_path() / "artemis_bt_test";
    const std::string csv = writeRecording(dir);
    Backtester bt(cfg(250));
    const BacktestResult r = bt.run(csv);
    EXPECT_EQ(r.rows, 90);
    EXPECT_EQ(r.opportunities, 1);
    EXPECT_EQ(r.completed, 1);
    EXPECT_NEAR(r.lockedProfit, 0.51, 1e-9);
    EXPECT_NEAR(r.perf.endEquity, 100.51, 1e-9);
    bt.writeOutputs(r, (dir / "out").string());
    EXPECT_TRUE(std::filesystem::exists(dir / "out_summary.json"));
    EXPECT_TRUE(std::filesystem::exists(dir / "out_equity.csv"));
    std::filesystem::remove_all(dir);
}

TEST(Backtester, MissesArbWhenLatencyExceedsItsLife) {
    const auto dir = std::filesystem::temp_directory_path() / "artemis_bt_test2";
    const std::string csv = writeRecording(dir);
    Backtester bt(cfg(1500));  // by the time orders land, the quote is gone
    const BacktestResult r = bt.run(csv);
    EXPECT_EQ(r.opportunities, 1);
    EXPECT_EQ(r.completed, 0);
    EXPECT_EQ(r.notFilled, 1);
    EXPECT_NEAR(r.perf.endEquity, 100.0, 1e-9);
    std::filesystem::remove_all(dir);
}

TEST(Performance, DrawdownAndSharpe) {
    std::vector<EquityPoint> curve = {
        {0, 100}, {3'600'000'000LL, 110}, {7'200'000'000LL, 99}, {10'800'000'000LL, 120}};
    const auto s = computePerformance(curve, 3600);
    EXPECT_DOUBLE_EQ(s.totalPnl, 20);
    EXPECT_DOUBLE_EQ(s.maxDrawdown, 11);
    EXPECT_NEAR(s.maxDrawdownPct, 10.0, 1e-9);
    EXPECT_EQ(s.buckets, 4);
    EXPECT_GT(s.sharpe, 0);
    EXPECT_DOUBLE_EQ(percentile({1, 2, 3, 4, 5}, 0.5), 3);
    EXPECT_DOUBLE_EQ(percentile({}, 0.5), 0);
}
