#pragma once
// Exact decimal-string <-> integer conversions for exchange payloads.
// Exchanges send prices as strings like "0.5600" and counts like "12.50";
// parsing through double would introduce rounding error, so we never do.

#include <optional>
#include <string>
#include <string_view>

#include "core/Types.hpp"

namespace artemis::fp {

// Parse a non-negative or negative decimal string into an integer scaled by
// 10^scale. Extra fractional digits beyond `scale` are truncated toward zero.
// Returns nullopt on malformed input.
std::optional<std::int64_t> parseScaled(std::string_view s, int scale);

// "0.5600" -> 560000 micros
std::optional<Micros> parseDollars(std::string_view s);
// "12.50" -> 1250 centi-contracts
std::optional<Qty> parseCount(std::string_view s);

// 560000 -> "0.5600" (4 decimals, the precision Kalshi accepts on requests)
std::string formatDollars(Micros m, int decimals = 4);
// 1250 -> "12.50"
std::string formatCount(Qty q);

// Human-friendly "$1,234.56" style (2 decimals, sign aware) for logs.
std::string usd(Micros m);
// Price in cents with one decimal: 563000 -> "56.3c"
std::string cents(Micros m);

}  // namespace artemis::fp
