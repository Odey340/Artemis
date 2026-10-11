#pragma once
// Helpers for building books in tests. Prices are given in cents, sizes in contracts.

#include <initializer_list>
#include <string>
#include <utility>

#include "core/OrderBook.hpp"
#include "core/Types.hpp"

namespace artemis::test {

inline Ladder L(std::initializer_list<std::pair<double, double>> lv) {
    Ladder l;
    for (const auto& [c, n] : lv)
        l.push_back({static_cast<Micros>(c * kOneCent + 0.5), static_cast<Qty>(n * kOneContract + 0.5)});
    return l;
}

// Kalshi-style book from YES bids and NO bids.
inline BinaryBook kbook(const std::string& t, std::initializer_list<std::pair<double, double>> yesBids,
                        std::initializer_list<std::pair<double, double>> noBids,
                        std::int64_t ts = 1'000'000) {
    return BinaryBook::fromKalshi(t, L(yesBids), L(noBids), ts);
}

constexpr Micros c(double cents) { return static_cast<Micros>(cents * kOneCent + 0.5); }
constexpr Micros usd(double d) { return static_cast<Micros>(d * kOneDollar + 0.5); }
constexpr Qty n(double contracts) { return static_cast<Qty>(contracts * kOneContract + 0.5); }

}  // namespace artemis::test
