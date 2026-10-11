#pragma once
// Core value types shared by every Artemis module.
//
// All money and prices are integers so that edge calculations are exact:
//   * Price / Money : micro-dollars (1'000'000 == $1.00)
//   * Qty           : centi-contracts (100 == 1 contract), matching the
//                     2-decimal fixed-point counts used by the Kalshi API.

#include <cstdint>
#include <string>
#include <vector>

namespace artemis {

using Micros = std::int64_t;   // money / price in 1e-6 USD
using Qty    = std::int64_t;   // contracts * 100

constexpr Micros kOneDollar = 1'000'000;
constexpr Micros kOneCent   = 10'000;
constexpr Qty    kOneContract = 100;

enum class Venue : std::uint8_t { Kalshi = 0, Polymarket = 1 };
enum class Outcome : std::uint8_t { Yes = 0, No = 1 };

inline const char* toString(Venue v) { return v == Venue::Kalshi ? "kalshi" : "polymarket"; }
inline const char* toString(Outcome o) { return o == Outcome::Yes ? "YES" : "NO"; }
inline Outcome opposite(Outcome o) { return o == Outcome::Yes ? Outcome::No : Outcome::Yes; }

// Cost (micros) of `qty` centi-contracts at `price` micros per contract.
inline Micros notional(Micros price, Qty qty) { return (price * qty) / kOneContract; }

struct Level {
    Micros price = 0;
    Qty    qty   = 0;
    bool operator==(const Level& o) const { return price == o.price && qty == o.qty; }
};

using Ladder = std::vector<Level>;

}  // namespace artemis
