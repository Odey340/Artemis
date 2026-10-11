#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "TestUtil.hpp"
#include "engine/Universe.hpp"

using namespace artemis;
using namespace artemis::test;

namespace {
EventInfo ev(const std::string& t, const std::string& series, std::vector<double> yesBidCents,
             bool me = true) {
    EventInfo e;
    e.eventTicker = t;
    e.seriesTicker = series;
    e.mutuallyExclusive = me;
    int i = 0;
    for (double yb : yesBidCents) {
        MarketInfo m;
        m.ticker = t + "-" + std::to_string(i++);
        m.status = "active";
        m.yesBid = c(yb);
        m.noAsk = kOneDollar - c(yb);
        m.yesAsk = c(yb + 2);
        e.markets.push_back(m);
    }
    return e;
}
}  // namespace

TEST(Universe, Gaps) {
    EXPECT_EQ(noBasketGap(ev("E", "S", {40, 35, 35})), c(10));
    EXPECT_EQ(noBasketGap(ev("E", "S", {30, 30, 30})), -c(10));
    EXPECT_EQ(noBasketGap(ev("E", "S", {90})), -kOneDollar);
    // yes asks 42+37+37 = 116
    EXPECT_EQ(yesBasketGap(ev("E", "S", {40, 35, 35})), -c(16));
}

TEST(Universe, PatternMatching) {
    EXPECT_TRUE(matchesAny("KXFED", {"KXFED"}));
    EXPECT_TRUE(matchesAny("KXFED-26", {"KXFED*"}));
    EXPECT_FALSE(matchesAny("KXCPI", {"KXFED*", ""}));
    EXPECT_TRUE(matchesAny("anything", {"*"}));
}

TEST(Universe, SelectFiltersAndRanks) {
    Config cfg;
    cfg.universe.prefilterSlackCents = 5;
    cfg.universe.maxEventsTracked = 10;
    cfg.universe.seriesDeny = {"BAD*"};
    cfg.universe.exhaustiveSeries = {"EXH"};
    UniverseBuilder b(cfg, nullptr);
    std::vector<EventInfo> all = {
        ev("NEAR", "S", {33, 33, 32}),         // gap -2c  -> kept
        ev("FAR", "S", {20, 20, 20}),          // gap -40c -> dropped by slack
        ev("BEST", "S", {40, 40, 30}),         // gap +10c -> ranked first
        ev("NOTME", "S", {50, 50, 50}, false), // not mutually exclusive
        ev("DENY", "BADSERIES", {50, 50, 50}),
        ev("ONE", "S", {60}),                  // single market
        ev("EX", "EXH", {33, 33, 33}),         // exhaustive flag from config
    };
    auto out = b.select(all, 0);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].eventTicker, "BEST");
    EXPECT_FALSE(out[0].exhaustive);
    bool sawEx = false;
    for (const auto& e : out)
        if (e.eventTicker == "EX") sawEx = e.exhaustive;
    EXPECT_TRUE(sawEx);
}

TEST(Universe, PinnedEventsBypassRankingAndCloseGuard) {
    Config cfg;
    cfg.universe.eventAllow = {"FAR"};
    cfg.universe.minSecondsToClose = 600;
    UniverseBuilder b(cfg, nullptr);
    auto far = ev("FAR", "S", {20, 20, 20});
    auto closing = ev("SOON", "S", {40, 40, 40});
    for (auto& m : closing.markets) m.closeTsUs = 100'000'000;  // closes 100s after t=0
    auto out = b.select({far, closing}, 0);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].eventTicker, "FAR");
}

TEST(Universe, SnapshotJsonRoundTrip) {
    UniverseSnapshot u;
    u.events.push_back(ev("E", "S", {40, 35}));
    u.events[0].exhaustive = true;
    u.events[0].feeMultiplier = 0.5;
    MarketPair p;
    p.id = "pair";
    p.kalshiTicker = "E-0";
    p.polyYesToken = "y";
    p.feeBps = 10;
    u.pairs.push_back(p);
    u.index();
    UniverseSnapshot r = UniverseSnapshot::fromJson(nlohmann::json::parse(u.toJson().dump()));
    ASSERT_EQ(r.events.size(), 1u);
    EXPECT_TRUE(r.events[0].exhaustive);
    EXPECT_DOUBLE_EQ(r.events[0].feeMultiplier, 0.5);
    EXPECT_EQ(r.tickerToEvent.at("E-1"), "E");
    EXPECT_EQ(r.tickerToPairs.at("E-0")[0], "pair");
    EXPECT_DOUBLE_EQ(r.pairs[0].feeBps.value(), 10.0);
    auto tickers = r.kalshiTickers();
    EXPECT_EQ(tickers.size(), 2u);  // E-0 shared by event and pair is listed once
}
