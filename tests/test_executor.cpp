#include <gtest/gtest.h>

#include <set>

#include "TestUtil.hpp"
#include "exec/BasketExecutor.hpp"
#include "strategy/ArbDetector.hpp"

using namespace artemis;
using namespace artemis::test;

namespace {

constexpr std::int64_t kNow = 10'000'000;

// SimGateway that kills specific submissions (1-based call index).
class ScriptedGateway : public SimGateway {
public:
    ScriptedGateway(BookStore& b, std::set<int> kill) : SimGateway(b), kill_(std::move(kill)) {}
    OrderResult submit(const OrderRequest& r) override {
        ++calls;
        submitted.push_back(r);
        if (kill_.count(calls)) {
            OrderResult o;
            o.accepted = true;
            o.clientOrderId = r.clientOrderId;
            o.remaining = r.qty;
            o.error = "killed (scripted)";
            return o;
        }
        return SimGateway::submit(r);
    }
    int calls = 0;
    std::vector<OrderRequest> submitted;

private:
    std::set<int> kill_;
};

EventInfo event(std::vector<std::string> tickers) {
    EventInfo e;
    e.eventTicker = "E";
    e.mutuallyExclusive = true;
    for (auto& t : tickers) {
        MarketInfo m;
        m.ticker = t;
        m.status = "active";
        e.markets.push_back(m);
    }
    return e;
}

StrategyConfig strat() {
    StrategyConfig s;
    s.minProfitUsd = 0.05;
    s.minEdgeCents = 0;
    s.minRoi = 0;
    s.safetyMarginCents = 0;
    s.maxContracts = 1000;
    return s;
}

Opportunity findOne(const BookStore& books, const EventInfo& ev) {
    FeedConfig f;
    ArbDetector d(strat(), f, PolymarketConfig{}, BasisConfig{});
    auto opps = d.evaluateEvent(ev, books, kNow);
    EXPECT_EQ(opps.size(), 1u);
    return opps.empty() ? Opportunity{} : opps[0];
}

void threeLegBook(BookStore& books) {
    books.put(kbook("A", {{40, 10}}, {{55, 50}}, kNow));
    books.put(kbook("B", {{35, 10}}, {{60, 50}}, kNow));
    books.put(kbook("C", {{35, 10}}, {{60, 50}}, kNow));
}

}  // namespace

TEST(BasketExecutor, CompletesBasketAndConsumesLiquidity) {
    BookStore books;
    threeLegBook(books);
    const Opportunity opp = findOne(books, event({"A", "B", "C"}));
    SimGateway gw(books);
    BasketExecutor ex(gw, ExecutionConfig{}, strat());
    const ExecutionReport r = ex.execute(opp, opp.units);
    EXPECT_EQ(r.status, ExecStatus::Completed);
    EXPECT_EQ(r.units, n(10));
    EXPECT_EQ(r.cost, usd(19));
    EXPECT_EQ(r.fees, usd(0.49));
    EXPECT_EQ(r.guaranteedPayout, usd(20));
    EXPECT_EQ(r.lockedProfit, usd(0.51));
    EXPECT_EQ(r.cashFlow, -usd(19.49));
    EXPECT_EQ(r.orders.size(), 3u);
    // liquidity consumed: the YES bids that formed our NO asks are gone
    EXPECT_FALSE(books.get("K:A")->hasAsk(Outcome::No));
}

TEST(BasketExecutor, OrdersUseFillOrKillAtLimit) {
    BookStore books;
    threeLegBook(books);
    const Opportunity opp = findOne(books, event({"A", "B", "C"}));
    ScriptedGateway gw(books, {});
    BasketExecutor ex(gw, ExecutionConfig{}, strat());
    ex.execute(opp, opp.units);
    ASSERT_EQ(gw.submitted.size(), 3u);
    for (const auto& r : gw.submitted) {
        EXPECT_TRUE(r.buy);
        EXPECT_EQ(r.timeInForce, "fill_or_kill");
        EXPECT_EQ(r.outcome, Outcome::No);
        EXPECT_EQ(r.qty, n(10));
        EXPECT_FALSE(r.clientOrderId.empty());
    }
}

TEST(BasketExecutor, LegFailureUnwindsWhenCompletionNotAllowed) {
    BookStore books;
    threeLegBook(books);
    const Opportunity opp = findOne(books, event({"A", "B", "C"}));
    ScriptedGateway gw(books, {2});
    ExecutionConfig e;
    e.maxCompletionRetries = 0;
    BasketExecutor ex(gw, e, strat());
    const ExecutionReport r = ex.execute(opp, opp.units);
    EXPECT_EQ(r.status, ExecStatus::Unwound);
    EXPECT_LT(r.realizedPnl, 0);           // bought at the ask, sold at the bid
    EXPECT_EQ(r.realizedPnl, r.cashFlow);  // flat afterwards: P&L == cash
    EXPECT_EQ(r.strandedCost, 0);
    EXPECT_FALSE(gw.submitted.back().buy);
    EXPECT_TRUE(gw.submitted.back().reduceOnly);
    EXPECT_EQ(gw.submitted.back().timeInForce, "immediate_or_cancel");
}

TEST(BasketExecutor, LegFailureCompletesOnRetry) {
    BookStore books;
    threeLegBook(books);
    const Opportunity opp = findOne(books, event({"A", "B", "C"}));
    ScriptedGateway gw(books, {2});
    ExecutionConfig e;
    e.maxCompletionRetries = 1;
    BasketExecutor ex(gw, e, strat());
    const ExecutionReport r = ex.execute(opp, opp.units);
    EXPECT_EQ(r.status, ExecStatus::Completed);
    EXPECT_EQ(r.lockedProfit, usd(0.51));
    EXPECT_EQ(gw.calls, 4);
}

TEST(BasketExecutor, ShrinksNoBasketToHedgedSubset) {
    BookStore books;
    // A,B: NO @45c (thin, so they go first); C,D: NO @90c (deep)
    books.put(kbook("A", {{55, 10}}, {{40, 50}}, kNow));
    books.put(kbook("B", {{55, 10}}, {{40, 50}}, kNow));
    books.put(kbook("C", {{10, 1000}}, {{85, 50}}, kNow));
    books.put(kbook("D", {{10, 1000}}, {{85, 50}}, kNow));
    const Opportunity opp = findOne(books, event({"A", "B", "C", "D"}));
    ASSERT_EQ(opp.legs.size(), 4u);
    ScriptedGateway gw(books, {3});
    ExecutionConfig e;
    e.maxCompletionRetries = 0;
    BasketExecutor ex(gw, e, strat());
    const ExecutionReport r = ex.execute(opp, opp.units);
    EXPECT_EQ(r.status, ExecStatus::Shrunk);
    EXPECT_EQ(r.guaranteedPayout, usd(10));  // (2 - 1) x 10 units
    EXPECT_EQ(r.cost, usd(9));
    EXPECT_GT(r.lockedProfit, 0);
}

TEST(BasketExecutor, FirstLegKilledIsNotATrade) {
    BookStore books;
    threeLegBook(books);
    const Opportunity opp = findOne(books, event({"A", "B", "C"}));
    ScriptedGateway gw(books, {1});
    ExecutionConfig e;
    e.maxCompletionRetries = 0;
    BasketExecutor ex(gw, e, strat());
    const ExecutionReport r = ex.execute(opp, opp.units);
    EXPECT_EQ(r.status, ExecStatus::NotFilled);
    EXPECT_FALSE(r.traded());
    EXPECT_EQ(r.cashFlow, 0);
}

TEST(BasketExecutor, EdgeVanishedBeforeSending) {
    BookStore books;
    threeLegBook(books);
    const Opportunity opp = findOne(books, event({"A", "B", "C"}));
    books.put(kbook("A", {{20, 10}}, {{55, 50}}, kNow));  // market moved
    ScriptedGateway gw(books, {});
    BasketExecutor ex(gw, ExecutionConfig{}, strat());
    const ExecutionReport r = ex.execute(opp, opp.units);
    EXPECT_EQ(r.status, ExecStatus::NotFilled);
    EXPECT_EQ(gw.calls, 0);
}

TEST(BasketExecutor, BatchModeUsesSameAccounting) {
    BookStore books;
    threeLegBook(books);
    const Opportunity opp = findOne(books, event({"A", "B", "C"}));
    SimGateway gw(books);
    ExecutionConfig e;
    e.legMode = "batch";
    BasketExecutor ex(gw, e, strat());
    const ExecutionReport r = ex.execute(opp, opp.units);
    EXPECT_EQ(r.status, ExecStatus::Completed);
    EXPECT_EQ(r.lockedProfit, usd(0.51));
}

TEST(BasketExecutor, RespectsUnitCap) {
    BookStore books;
    threeLegBook(books);
    const Opportunity opp = findOne(books, event({"A", "B", "C"}));
    SimGateway gw(books);
    BasketExecutor ex(gw, ExecutionConfig{}, strat());
    const ExecutionReport r = ex.execute(opp, n(4));
    EXPECT_EQ(r.status, ExecStatus::Completed);
    EXPECT_EQ(r.units, n(4));
}

TEST(BasketExecutor, LiveGatewaySkipsPolymarketLegs) {
    Opportunity opp;
    opp.kind = ArbKind::CrossVenue;
    opp.units = n(1);
    Leg k;
    k.venue = Venue::Kalshi;
    Leg p;
    p.venue = Venue::Polymarket;
    opp.legs = {k, p};
    KalshiGateway gw(nullptr);
    BasketExecutor ex(gw, ExecutionConfig{}, strat());
    const ExecutionReport r = ex.execute(opp, opp.units);
    EXPECT_EQ(r.status, ExecStatus::Skipped);
}

TEST(ClientOrderId, UniqueUuidShape) {
    std::set<std::string> ids;
    for (int i = 0; i < 1000; ++i) {
        const std::string id = newClientOrderId();
        EXPECT_EQ(id.size(), 36u);
        EXPECT_EQ(id[14], '4');
        ids.insert(id);
    }
    EXPECT_EQ(ids.size(), 1000u);
}
