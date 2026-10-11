#include <gtest/gtest.h>

#include "TestUtil.hpp"
#include "core/OrderBook.hpp"

using namespace artemis;
using namespace artemis::test;

TEST(OrderBook, KalshiBidsAreReflectedIntoAsks) {
    // YES bids 40c x10, 39c x5 ; NO bids 55c x7, 54c x3  (unsorted input on purpose)
    auto b = kbook("T", {{39, 5}, {40, 10}}, {{54, 3}, {55, 7}});
    EXPECT_TRUE(b.mirrored());
    EXPECT_EQ(b.bestBid(Outcome::Yes), c(40));
    EXPECT_EQ(b.bestBid(Outcome::No), c(55));
    // YES ask = 1 - best NO bid
    EXPECT_EQ(b.bestAsk(Outcome::Yes), c(45));
    EXPECT_EQ(b.asks(Outcome::Yes)[0].qty, n(7));
    EXPECT_EQ(b.asks(Outcome::Yes)[1].price, c(46));
    // NO ask = 1 - best YES bid
    EXPECT_EQ(b.bestAsk(Outcome::No), c(60));
    EXPECT_EQ(b.yesMid(), c(42.5));
}

TEST(OrderBook, NormaliseDropsJunkAndMergesLevels) {
    auto b = BinaryBook::fromTwoTokens(Venue::Polymarket, "p", L({{40, 1}, {40, 2}, {0, 5}, {41, 0}}),
                                       L({{50, 1}, {100, 3}, {49, 2}}), {}, {}, 0);
    ASSERT_EQ(b.bids(Outcome::Yes).size(), 1u);
    EXPECT_EQ(b.bids(Outcome::Yes)[0].qty, n(3));
    ASSERT_EQ(b.asks(Outcome::Yes).size(), 2u);
    EXPECT_EQ(b.bestAsk(Outcome::Yes), c(49));
}

TEST(OrderBook, WalkAsksAcrossLevels) {
    auto b = kbook("T", {}, {{60, 5}, {58, 10}});  // YES asks 40c x5, 42c x10
    int calls = 0;
    WalkResult w = b.walkAsks(Outcome::Yes, n(8), [&](Micros, Qty) { ++calls; });
    EXPECT_EQ(w.filled, n(8));
    EXPECT_EQ(w.cost, usd(5 * 0.40 + 3 * 0.42));
    EXPECT_EQ(w.bestPrice, c(40));
    EXPECT_EQ(w.worstPrice, c(42));
    EXPECT_EQ(calls, 2);
    WalkResult all = b.walkAsks(Outcome::Yes, n(100));
    EXPECT_EQ(all.filled, n(15));
}

TEST(OrderBook, DepthAtOrBetter) {
    auto b = kbook("T", {{30, 4}, {29, 6}}, {{60, 5}, {58, 10}});
    EXPECT_EQ(b.askDepthAtOrBetter(Outcome::Yes, c(40)), n(5));
    EXPECT_EQ(b.askDepthAtOrBetter(Outcome::Yes, c(42)), n(15));
    EXPECT_EQ(b.bidDepthAtOrBetter(Outcome::Yes, c(29)), n(10));
    EXPECT_EQ(b.bidDepthAtOrBetter(Outcome::Yes, c(31)), 0);
}

TEST(OrderBook, ConsumingMirroredAsksRemovesOppositeBids) {
    auto b = kbook("T", {{30, 4}}, {{60, 5}, {58, 10}});
    b.consumeAsks(Outcome::Yes, n(7));  // takes all of 40c and 2 of 42c
    EXPECT_EQ(b.bestAsk(Outcome::Yes), c(42));
    EXPECT_EQ(b.asks(Outcome::Yes)[0].qty, n(8));
    EXPECT_EQ(b.bestBid(Outcome::No), c(58));
    EXPECT_EQ(b.bids(Outcome::No)[0].qty, n(8));
    b.consumeBids(Outcome::Yes, n(4));
    EXPECT_FALSE(b.hasBid(Outcome::Yes));
    EXPECT_FALSE(b.hasAsk(Outcome::No));
}

TEST(OrderBook, LadderEncodingRoundTrips) {
    Ladder l = L({{12.5, 3}, {13, 0.5}});
    EXPECT_EQ(decodeLadder(encodeLadder(l)), l);
    EXPECT_TRUE(decodeLadder("").empty());
}

#include "strategy/BookStore.hpp"

TEST(OrderBook, TakeAtSingleLevel) {
    auto b = kbook("T", {{30, 4}}, {{60, 5}, {58, 10}});
    EXPECT_EQ(b.takeAt(true, Outcome::Yes, c(42), n(3)), n(3));  // YES ask 42c == NO bid 58c
    EXPECT_EQ(b.asks(Outcome::Yes)[1].qty, n(7));
    EXPECT_EQ(b.takeAt(true, Outcome::Yes, c(41), n(3)), 0);     // no such level
    EXPECT_EQ(b.takeAt(false, Outcome::Yes, c(30), n(10)), n(4));
    EXPECT_FALSE(b.hasBid(Outcome::Yes));
}

TEST(BookStore, SimulatedTakesPersistAcrossSnapshots) {
    BookStore s;
    s.setTakeTtlUs(10'000'000);
    s.put(kbook("T", {}, {{60, 5}}, 1'000'000));
    s.recordTake("K:T", true, Outcome::Yes, c(40), n(3), 1'000'000);
    // a fresh snapshot still showing the same resting order: our 3 are gone from it
    s.put(kbook("T", {}, {{60, 5}}, 2'000'000));
    EXPECT_EQ(s.get("K:T")->asks(Outcome::Yes)[0].qty, n(2));
    EXPECT_EQ(s.pendingTakes(), 1u);
    // level disappears -> take forgotten
    s.put(kbook("T", {}, {{59, 5}}, 3'000'000));
    EXPECT_EQ(s.pendingTakes(), 0u);
    // expiry
    s.recordTake("K:T", true, Outcome::Yes, c(41), n(1), 3'000'000);
    s.put(kbook("T", {}, {{59, 5}}, 20'000'000));
    EXPECT_EQ(s.get("K:T")->asks(Outcome::Yes)[0].qty, n(5));
    EXPECT_EQ(s.pendingTakes(), 0u);
}
