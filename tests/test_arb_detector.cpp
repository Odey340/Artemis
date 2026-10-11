#include <gtest/gtest.h>

#include "TestUtil.hpp"
#include "strategy/ArbDetector.hpp"

using namespace artemis;
using namespace artemis::test;

namespace {

constexpr std::int64_t kNow = 10'000'000;

EventInfo event(const std::string& id, std::vector<std::string> tickers, bool exhaustive = false) {
    EventInfo e;
    e.eventTicker = id;
    e.seriesTicker = "S";
    e.title = id;
    e.mutuallyExclusive = true;
    e.exhaustive = exhaustive;
    for (auto& t : tickers) {
        MarketInfo m;
        m.ticker = t;
        m.eventTicker = id;
        m.status = "active";
        e.markets.push_back(m);
    }
    return e;
}

StrategyConfig strat() {
    StrategyConfig s;
    s.minProfitUsd = 0.10;
    s.minEdgeCents = 0.0;
    s.minRoi = 0.0;
    s.safetyMarginCents = 0.0;
    s.maxContracts = 1000;
    return s;
}

ArbDetector detector(StrategyConfig s = strat()) {
    FeedConfig f;
    f.maxBookAgeMs = 3000;
    return ArbDetector(s, f, PolymarketConfig{}, BasisConfig{});
}

}  // namespace

TEST(ArbDetector, NoBasketExactProfit) {
    BookStore books;
    // YES bids 40/35/35 sum to 110c -> NO asks 60/65/65 cost 190c for a 200c payout.
    books.put(kbook("A", {{40, 10}, {30, 50}}, {{55, 10}}, kNow));
    books.put(kbook("B", {{35, 10}, {30, 50}}, {{60, 10}}, kNow));
    books.put(kbook("C", {{35, 10}, {30, 50}}, {{60, 10}}, kNow));
    auto d = detector();
    auto opps = d.evaluateEvent(event("E", {"A", "B", "C"}), books, kNow);
    ASSERT_EQ(opps.size(), 1u);
    const Opportunity& o = opps[0];
    EXPECT_EQ(o.kind, ArbKind::NoBasket);
    EXPECT_EQ(o.legs.size(), 3u);
    EXPECT_EQ(o.payoutPerUnit, usd(2));
    EXPECT_EQ(o.units, n(10));  // deeper levels (NO @70c) are not profitable
    EXPECT_EQ(o.cost, usd(19.00));
    // fees: 0.07*10*.6*.4 = .168 -> .17 ; 0.07*10*.65*.35 = .15925 -> .16 (x2)
    EXPECT_EQ(o.fees, usd(0.49));
    EXPECT_EQ(o.profit, usd(0.51));
    for (const auto& l : o.legs) EXPECT_EQ(l.outcome, Outcome::No);
}

TEST(ArbDetector, NoBasketDropsLegsThatDoNotPayForTheirFee) {
    BookStore books;
    books.put(kbook("A", {{40, 10}}, {}, kNow));
    books.put(kbook("B", {{35, 10}}, {}, kNow));
    books.put(kbook("C", {{35, 10}}, {}, kNow));
    books.put(kbook("D", {{1, 1000}}, {}, kNow));  // NO @99c: <1c edge on 99c of capital
    books.put(kbook("X", {}, {{50, 10}}, kNow));    // no NO ask at all
    auto d = detector();
    auto opps = d.evaluateEvent(event("E", {"A", "B", "C", "D", "X"}), books, kNow);
    ASSERT_EQ(opps.size(), 1u);
    EXPECT_EQ(opps[0].legs.size(), 3u);
    EXPECT_EQ(opps[0].payoutPerUnit, usd(2));

    // with the capital-efficiency filter off, the 99c leg is worth adding
    StrategyConfig s = strat();
    s.minLegRoi = 0.0;
    auto d2 = detector(s);
    auto all = d2.evaluateEvent(event("E", {"A", "B", "C", "D", "X"}), books, kNow);
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].legs.size(), 4u);
    EXPECT_EQ(all[0].payoutPerUnit, usd(3));
    EXPECT_GT(all[0].profit, opps[0].profit);
}

TEST(ArbDetector, FairBooksProduceNothing) {
    BookStore books;
    books.put(kbook("A", {{30, 100}}, {{68, 100}}, kNow));
    books.put(kbook("B", {{30, 100}}, {{68, 100}}, kNow));
    books.put(kbook("C", {{30, 100}}, {{68, 100}}, kNow));
    auto d = detector();
    EXPECT_TRUE(d.evaluateEvent(event("E", {"A", "B", "C"}, true), books, kNow).empty());
}

TEST(ArbDetector, FeesKillThinGrossEdge) {
    BookStore books;
    // 34+34+34 = 102c of YES bids: 2c gross per unit, ~6c of fees
    for (auto t : {"A", "B", "C"}) books.put(kbook(t, {{34, 100}}, {}, kNow));
    auto d = detector();
    EXPECT_TRUE(d.evaluateEvent(event("E", {"A", "B", "C"}), books, kNow).empty());
}

TEST(ArbDetector, YesBasketRequiresExhaustiveEvent) {
    BookStore books;
    // NO bids at 70c -> YES asks at 30c each; 3 x 30c = 90c for a guaranteed $1
    for (auto t : {"A", "B", "C"}) books.put(kbook(t, {}, {{70, 20}}, kNow));
    auto d = detector();
    EXPECT_TRUE(d.evaluateEvent(event("E", {"A", "B", "C"}, false), books, kNow).empty());
    auto opps = d.evaluateEvent(event("E", {"A", "B", "C"}, true), books, kNow);
    ASSERT_EQ(opps.size(), 1u);
    EXPECT_EQ(opps[0].kind, ArbKind::YesBasket);
    EXPECT_EQ(opps[0].units, n(20));
    EXPECT_EQ(opps[0].cost, usd(18));
    // 0.07*20*.3*.7 = .294 -> .30 per leg
    EXPECT_EQ(opps[0].fees, usd(0.90));
    EXPECT_EQ(opps[0].profit, usd(20 - 18 - 0.90));
}

TEST(ArbDetector, YesBasketNeedsEveryLeg) {
    BookStore books;
    books.put(kbook("A", {}, {{70, 20}}, kNow));
    books.put(kbook("B", {}, {{70, 20}}, kNow));
    books.put(kbook("C", {{10, 5}}, {}, kNow));  // no YES ask on C
    auto d = detector();
    auto opps = d.evaluateEvent(event("E", {"A", "B", "C"}, true), books, kNow);
    for (const auto& o : opps) EXPECT_NE(o.kind, ArbKind::YesBasket);
}

TEST(ArbDetector, IgnoresNonMutuallyExclusiveAndFlatFeeEvents) {
    BookStore books;
    for (auto t : {"A", "B", "C"}) books.put(kbook(t, {{40, 10}}, {}, kNow));
    auto d = detector();
    EventInfo e = event("E", {"A", "B", "C"});
    e.mutuallyExclusive = false;
    EXPECT_TRUE(d.evaluateEvent(e, books, kNow).empty());
    e.mutuallyExclusive = true;
    e.feeType = "flat";
    EXPECT_TRUE(d.evaluateEvent(e, books, kNow).empty());
}

TEST(ArbDetector, StaleBooksAreIgnored) {
    BookStore books;
    for (auto t : {"A", "B", "C"}) books.put(kbook(t, {{40, 10}}, {}, kNow - 5'000'000));
    auto d = detector();
    EXPECT_TRUE(d.evaluateEvent(event("E", {"A", "B", "C"}), books, kNow).empty());
}

TEST(ArbDetector, CrossVenueBothDirections) {
    BookStore books;
    // Kalshi YES ask 40c x50 (NO bid 60c); Polymarket NO ask 55c x100
    books.put(kbook("K", {{30, 50}}, {{60, 50}}, kNow));
    books.put(BinaryBook::fromTwoTokens(Venue::Polymarket, "pair", L({{44, 100}}), L({{45, 100}}),
                                        L({{54, 100}}), L({{55, 100}}), kNow));
    MarketPair p;
    p.id = "pair";
    p.kalshiTicker = "K";
    auto d = detector();
    auto opps = d.evaluatePair(p, books, kNow);
    ASSERT_EQ(opps.size(), 1u);
    const auto& o = opps[0];
    EXPECT_EQ(o.kind, ArbKind::CrossVenue);
    EXPECT_EQ(o.units, n(50));
    EXPECT_EQ(o.legs[0].venue, Venue::Kalshi);
    EXPECT_EQ(o.legs[0].outcome, Outcome::Yes);
    EXPECT_EQ(o.legs[1].venue, Venue::Polymarket);
    EXPECT_EQ(o.legs[1].outcome, Outcome::No);
    EXPECT_EQ(o.cost, usd(50 * 0.95));
    EXPECT_EQ(o.fees, usd(0.84));  // 0.07*50*.4*.6 = .84 on Kalshi, 0 on Polymarket
    EXPECT_EQ(o.profit, usd(50 - 47.5 - 0.84));

    // Flip: Polymarket YES cheap vs Kalshi NO
    BookStore b2;
    b2.put(kbook("K", {{58, 30}}, {{30, 30}}, kNow));  // Kalshi NO ask 42c
    b2.put(BinaryBook::fromTwoTokens(Venue::Polymarket, "pair", L({{49, 100}}), L({{50, 100}}),
                                     L({{49, 100}}), L({{51, 100}}), kNow));
    auto o2 = d.evaluatePair(p, b2, kNow);
    ASSERT_EQ(o2.size(), 1u);
    EXPECT_EQ(o2[0].legs[0].outcome, Outcome::No);
    EXPECT_EQ(o2[0].legs[1].outcome, Outcome::Yes);
}

TEST(ArbDetector, PolymarketFeeModelApplies) {
    BookStore books;
    books.put(kbook("K", {{30, 50}}, {{60, 50}}, kNow));
    books.put(BinaryBook::fromTwoTokens(Venue::Polymarket, "pair", {}, {}, {}, L({{55, 100}}), kNow));
    MarketPair p;
    p.id = "pair";
    p.kalshiTicker = "K";
    p.feeBps = 1000;  // 10% * min(p,1-p): 50*0.45*0.1 = 2.25 -> kills the 2.5 edge
    auto d = detector();
    EXPECT_TRUE(d.evaluatePair(p, books, kNow).empty());
}

TEST(ArbDetector, ConfirmationsRequireConsecutiveSightings) {
    BookStore books;
    for (auto t : {"A", "B", "C"}) books.put(kbook(t, {{40, 10}}, {}, kNow));
    StrategyConfig s = strat();
    s.confirmations = 2;
    auto d = detector(s);
    std::vector<EventInfo> evs = {event("E", {"A", "B", "C"})};
    EXPECT_TRUE(d.scan(evs, {}, books, kNow).empty());
    auto second = d.scan(evs, {}, books, kNow);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(second[0].confirmations, 2);

    // edge disappears -> streak resets
    for (auto t : {"A", "B", "C"}) books.put(kbook(t, {{20, 10}}, {}, kNow));
    EXPECT_TRUE(d.scan(evs, {}, books, kNow).empty());
    for (auto t : {"A", "B", "C"}) books.put(kbook(t, {{40, 10}}, {}, kNow));
    EXPECT_TRUE(d.scan(evs, {}, books, kNow).empty());
}

TEST(ArbDetector, ResizeToRiskBudget) {
    BookStore books;
    for (auto t : {"A", "B", "C"}) books.put(kbook(t, {{40, 100}}, {}, kNow));
    auto d = detector();
    auto opps = d.evaluateEvent(event("E", {"A", "B", "C"}), books, kNow);
    ASSERT_EQ(opps.size(), 1u);
    EXPECT_EQ(opps[0].units, n(100));
    auto small = d.resize(opps[0], books, n(10));
    ASSERT_TRUE(small.has_value());
    EXPECT_EQ(small->units, n(10));
    EXPECT_EQ(small->cost, usd(18));
    EXPECT_EQ(small->key, opps[0].key);
}

TEST(SizeBasket, ThresholdsAndDepth) {
    // Two legs, payout 1: YES 45c x3 then 52c; NO 50c x10
    BinaryBook a = BinaryBook::fromTwoTokens(Venue::Polymarket, "a", {}, L({{45, 3}, {52, 10}}), {}, {}, 0);
    BinaryBook b = BinaryBook::fromTwoTokens(Venue::Polymarket, "b", {}, {}, {}, L({{50, 10}}), 0);
    std::vector<LegSpec> legs = {{&a, "P:a", Venue::Polymarket, "a", Outcome::Yes, FeeModel::zero()},
                                 {&b, "P:b", Venue::Polymarket, "b", Outcome::No, FeeModel::zero()}};
    auto r = sizeBasket(legs, kOneDollar, n(100), 0, 0, 0.0, 0);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.units, n(3));  // 4th unit would cost 52+50 > 100
    EXPECT_EQ(r.profit, usd(0.15));
    // min profit above what is available
    EXPECT_FALSE(sizeBasket(legs, kOneDollar, n(100), usd(0.20), 0, 0.0, 0).ok);
    // safety margin of 3c/leg/unit eats the whole 5c edge
    EXPECT_FALSE(sizeBasket(legs, kOneDollar, n(100), 0, 0, 0.0, c(3)).ok);
    // unit cap
    EXPECT_EQ(sizeBasket(legs, kOneDollar, n(2), 0, 0, 0.0, 0).units, n(2));
}
