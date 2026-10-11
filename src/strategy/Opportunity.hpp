#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/Types.hpp"
#include "venues/Fees.hpp"

namespace artemis {

enum class ArbKind : std::uint8_t {
    NoBasket,    // buy NO on >=2 legs of a mutually-exclusive event: pays >= (legs-1)
    YesBasket,   // buy YES on every leg of an exhaustive event: pays exactly 1
    CrossVenue,  // YES on one venue + NO on the other for the same proposition: pays 1
};

inline const char* toString(ArbKind k) {
    switch (k) {
        case ArbKind::NoBasket: return "no_basket";
        case ArbKind::YesBasket: return "yes_basket";
        case ArbKind::CrossVenue: return "cross_venue";
    }
    return "?";
}

struct Leg {
    Venue venue = Venue::Kalshi;
    std::string marketId;   // Kalshi ticker, or Polymarket pair id
    std::string bookKey;    // key in the BookStore
    Outcome outcome = Outcome::No;
    Qty qty = 0;            // centi-contracts
    Micros limitPrice = 0;  // worst level needed (used as FOK limit)
    Micros bestPrice = 0;
    Micros cost = 0;        // expected cost at current book
    Micros fee = 0;         // expected (conservative) fee
    Micros topDepth = 0;    // contracts available at best price (centi)
    FeeModel feeModel;      // venue fee schedule for this leg
};

struct Opportunity {
    ArbKind kind = ArbKind::NoBasket;
    std::string key;        // unique per (kind, group, legs)
    std::string groupId;    // event ticker or pair id
    std::string label;
    std::vector<Leg> legs;
    Qty units = 0;              // contracts per leg (centi)
    Micros payoutPerUnit = 0;   // guaranteed minimum payout per unit
    Micros payout = 0;          // payoutPerUnit * units
    Micros cost = 0;
    Micros fees = 0;
    Micros profit = 0;          // payout - cost - fees
    Micros edgePerUnit = 0;     // profit per contract-set
    double roi = 0.0;           // profit / (cost + fees)
    std::int64_t detectedTsUs = 0;
    std::int64_t oldestBookTsUs = 0;
    int confirmations = 0;

    bool crossVenue() const { return kind == ArbKind::CrossVenue; }
};

}  // namespace artemis
