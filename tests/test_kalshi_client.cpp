#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "TestUtil.hpp"
#include "venues/kalshi/KalshiClient.hpp"
#include "venues/polymarket/PolymarketClient.hpp"

using namespace artemis;
using namespace artemis::test;
using nlohmann::json;

TEST(KalshiParse, FixedPointOrderbook) {
    const json j = json::parse(R"({"orderbook_fp":{
        "yes_dollars":[["0.4000","10.00"],["0.4100","2.50"]],
        "no_dollars":[["0.5500","7.00"]]}})");
    BinaryBook b = KalshiClient::parseOrderbook("KX-T", j, 42);
    EXPECT_EQ(b.id(), "KX-T");
    EXPECT_EQ(b.tsUs(), 42);
    EXPECT_EQ(b.bestBid(Outcome::Yes), c(41));  // sorted best-first regardless of input order
    EXPECT_EQ(b.bids(Outcome::Yes)[0].qty, 250);
    EXPECT_EQ(b.bestAsk(Outcome::Yes), c(45));
    EXPECT_EQ(b.bestAsk(Outcome::No), c(59));
}

TEST(KalshiParse, LegacyCentsOrderbookAndNulls) {
    const json legacy = json::parse(R"({"orderbook":{"yes":[[40,10]],"no":null}})");
    BinaryBook b = KalshiClient::parseOrderbook("T", legacy, 0);
    EXPECT_EQ(b.bestBid(Outcome::Yes), c(40));
    EXPECT_FALSE(b.hasBid(Outcome::No));
    const json batch = json::parse(R"({"ticker":"T","orderbook_fp":{"yes_dollars":null,"no_dollars":[]}})");
    EXPECT_TRUE(KalshiClient::parseOrderbook("T", batch, 0).empty());
}

TEST(KalshiParse, EventWithNestedMarkets) {
    const json j = json::parse(R"({
      "event_ticker":"KXFED-26DEC","series_ticker":"KXFED","title":"Fed",
      "mutually_exclusive":true,"fee_type_override":"quadratic","fee_multiplier_override":0.5,
      "markets":[
        {"ticker":"KXFED-26DEC-C25","status":"active","yes_sub_title":"Cut 25bp",
         "close_time":"2026-12-09T19:00:00Z","yes_bid_dollars":"0.3100","yes_ask_dollars":"0.3300",
         "no_bid_dollars":"0.6700","no_ask_dollars":"0.6900"},
        {"ticker":"KXFED-26DEC-H0","status":"active","no_ask_dollars":"0.0000"}
      ]})");
    EventInfo e = KalshiClient::parseEvent(j);
    EXPECT_EQ(e.eventTicker, "KXFED-26DEC");
    EXPECT_TRUE(e.mutuallyExclusive);
    EXPECT_DOUBLE_EQ(e.feeMultiplier, 0.5);
    ASSERT_EQ(e.markets.size(), 2u);
    EXPECT_EQ(e.markets[0].title, "Cut 25bp");
    EXPECT_EQ(e.markets[0].yesBid, c(31));
    EXPECT_EQ(e.markets[0].noAsk, c(69));
    EXPECT_EQ(e.markets[0].closeTsUs, 1796842800LL * 1'000'000);
    EXPECT_EQ(e.markets[1].noAsk, kOneDollar);  // "0" ask means no offer
}

TEST(KalshiOrders, V2MappingBuyYesBuyNo) {
    OrderRequest r;
    r.marketId = "T";
    r.clientOrderId = "cid";
    r.qty = n(12);
    r.outcome = Outcome::Yes;
    r.buy = true;
    r.limitPrice = c(42);
    json j = KalshiClient::orderToV2Json(r, 0);
    EXPECT_EQ(j["side"], "bid");
    EXPECT_EQ(j["price"], "0.4200");
    EXPECT_EQ(j["count"], "12.00");
    EXPECT_EQ(j["time_in_force"], "fill_or_kill");
    EXPECT_EQ(j["self_trade_prevention_type"], "taker_at_cross");
    EXPECT_FALSE(j.contains("subaccount"));

    // buying NO at 30c is selling YES at 70c
    r.outcome = Outcome::No;
    r.limitPrice = c(30);
    j = KalshiClient::orderToV2Json(r, 3);
    EXPECT_EQ(j["side"], "ask");
    EXPECT_EQ(j["price"], "0.7000");
    EXPECT_EQ(j["subaccount"], 3);

    // selling NO (unwind) is buying YES at 1 - price, reduce-only
    r.buy = false;
    r.reduceOnly = true;
    r.timeInForce = "immediate_or_cancel";
    j = KalshiClient::orderToV2Json(r, 0);
    EXPECT_EQ(j["side"], "bid");
    EXPECT_EQ(j["price"], "0.7000");
    EXPECT_EQ(j["reduce_only"], true);
}

TEST(KalshiOrders, ParseV2ResponseConvertsNoPrices) {
    OrderRequest r;
    r.outcome = Outcome::No;
    r.clientOrderId = "cid";
    const json j = json::parse(R"({"order_id":"o1","client_order_id":"cid","fill_count":"10.00",
        "remaining_count":"0.00","average_fill_price":"0.7100","average_fee_paid":"0.0150","ts_ms":1})");
    OrderResult o = KalshiClient::parseOrderResponse(j, r);
    EXPECT_TRUE(o.accepted);
    EXPECT_EQ(o.orderId, "o1");
    EXPECT_EQ(o.filled, n(10));
    EXPECT_EQ(o.avgPrice, c(29));  // YES 71c -> NO 29c
    EXPECT_EQ(o.fees, usd(0.15));
}

TEST(KalshiOrders, ParseBatchError) {
    OrderRequest r;
    const json j = json::parse(R"({"client_order_id":"x","error":{"code":"insufficient_balance","message":"no"}})");
    OrderResult o = KalshiClient::parseOrderResponse(j, r);
    EXPECT_FALSE(o.accepted);
    EXPECT_NE(o.error.find("insufficient_balance"), std::string::npos);
}

TEST(Polymarket, ParseAndCombineTokenBooks) {
    const json yes = json::parse(R"({"asset_id":"Y","bids":[{"price":"0.44","size":"100"}],
                                     "asks":[{"price":"0.46","size":"50.5"}]})");
    const json no = json::parse(R"({"asset_id":"N","bids":[{"price":"0.53","size":"80"}],
                                    "asks":[{"price":"0.56","size":"90"}]})");
    PolyTokenBook y = PolymarketClient::parseTokenBook(yes);
    PolyTokenBook nb = PolymarketClient::parseTokenBook(no);
    EXPECT_EQ(y.tokenId, "Y");
    EXPECT_EQ(y.asks[0].qty, 5050);

    MarketPair p;
    p.id = "pair";
    BinaryBook b = PolymarketClient::combine(p, &y, &nb, 0);
    EXPECT_EQ(b.bestAsk(Outcome::Yes), c(46));
    EXPECT_EQ(b.bestAsk(Outcome::No), c(56));

    p.invert = true;  // Polymarket YES == Kalshi NO
    BinaryBook inv = PolymarketClient::combine(p, &y, &nb, 0);
    EXPECT_EQ(inv.bestAsk(Outcome::Yes), c(56));
    EXPECT_EQ(inv.bestAsk(Outcome::No), c(46));

    // missing NO token: derived from the YES book
    p.invert = false;
    BinaryBook derived = PolymarketClient::combine(p, &y, nullptr, 0);
    EXPECT_EQ(derived.bestAsk(Outcome::No), c(56));
    EXPECT_EQ(derived.bestBid(Outcome::No), c(54));
}

TEST(Polymarket, MarketMapParsing) {
    auto pairs = parseMarketMap(R"({
      // comment allowed
      "pairs":[{"id":"a","kalshi_ticker":"K","poly_yes_token":"1","poly_no_token":"2","invert":true,"fee_bps":25}]})");
    ASSERT_EQ(pairs.size(), 1u);
    EXPECT_TRUE(pairs[0].invert);
    EXPECT_DOUBLE_EQ(pairs[0].feeBps.value(), 25.0);
    EXPECT_THROW(parseMarketMap(R"({"pairs":[{"id":"x"}]})"), std::exception);
}

TEST(KalshiOrders, SanitiseFlipsOppositeOutcomePrice) {
    OrderRequest buyNo;
    buyNo.buy = true;
    buyNo.outcome = Outcome::No;
    buyNo.limitPrice = c(30);
    OrderResult r;
    r.filled = n(10);
    r.avgPrice = c(71);  // impossible for a buy limited at 30c; 29c is consistent
    r.fees = usd(0.15);
    KalshiClient::sanitiseFill(r, buyNo);
    EXPECT_EQ(r.avgPrice, c(29));
    EXPECT_EQ(r.fees, usd(0.15));

    OrderResult ok;
    ok.filled = n(10);
    ok.avgPrice = c(29);
    KalshiClient::sanitiseFill(ok, buyNo);
    EXPECT_EQ(ok.avgPrice, c(29));

    // a "per-contract" fee of $1.50 on 10 contracts must have been a total
    OrderResult fee;
    fee.filled = n(10);
    fee.avgPrice = c(29);
    fee.fees = usd(15.0);
    KalshiClient::sanitiseFill(fee, buyNo);
    EXPECT_EQ(fee.fees, usd(1.5));
}
