#pragma once
// Exchange-agnostic descriptions of markets, events and orders.

#include <cstdint>
#include <string>
#include <vector>

#include "core/Types.hpp"
#include "venues/Fees.hpp"

namespace artemis {

struct MarketInfo {
    std::string ticker;
    std::string eventTicker;
    std::string title;
    std::string status;          // "active" / "open" are tradable
    std::int64_t closeTsUs = 0;
    // top-of-book as reported by the listing endpoint (may lag the orderbook)
    Micros yesBid = 0, yesAsk = kOneDollar, noBid = 0, noAsk = kOneDollar;

    bool tradable() const { return status == "active" || status == "open"; }
};

struct EventInfo {
    std::string eventTicker;
    std::string seriesTicker;
    std::string title;
    bool mutuallyExclusive = false;  // at most one market resolves YES (MECNET)
    bool exhaustive = false;         // exactly one resolves YES (operator-confirmed)
    double feeMultiplier = 1.0;
    std::string feeType = "quadratic";
    std::vector<MarketInfo> markets;
};

// A request to trade an outcome contract. Prices are in the outcome's own
// terms (a NO contract bought at 0.30 has limitPrice = 300000).
struct OrderRequest {
    Venue venue = Venue::Kalshi;
    std::string marketId;       // Kalshi ticker / Polymarket pair id
    std::string clientOrderId;
    Outcome outcome = Outcome::Yes;
    bool buy = true;
    Micros limitPrice = 0;
    Qty qty = 0;
    std::string timeInForce = "fill_or_kill";  // or "immediate_or_cancel"
    bool reduceOnly = false;
    FeeModel fee;  // used by the simulator to charge fees
};

struct OrderResult {
    bool accepted = false;     // exchange acknowledged the order
    bool ambiguous = false;    // transport failure: state unknown until reconciled
    long httpStatus = 0;
    std::string orderId;
    std::string clientOrderId;
    Qty filled = 0;
    Qty remaining = 0;
    Micros avgPrice = 0;       // outcome terms
    Micros fees = 0;           // total fees for the filled quantity
    std::string error;
};

}  // namespace artemis
