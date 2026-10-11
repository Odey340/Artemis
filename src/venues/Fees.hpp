#pragma once
// Venue fee models. Every fee is rounded *up* to the cent so that edge
// estimates are conservative: a trade that looks profitable here is
// profitable at the exchange.
//
// Kalshi (General Trading Fees Table):
//     fee = round_up( M * 0.07 * C * P * (1 - P) )            (taker)
//   where M is the series/event fee multiplier (1.0 for most series).
// Polymarket CLOB:   fee = bps/10000 * C * min(P, 1 - P)
// Polymarket US:     fee = theta * C * P * (1 - P)            (theta = 0.0695)

#include "core/Types.hpp"

namespace artemis {

class FeeModel {
public:
    enum class Kind { KalshiQuadratic, PolyBps, PolyQuadratic, Zero };

    static FeeModel kalshi(double rate = 0.07, double multiplier = 1.0) {
        return FeeModel(Kind::KalshiQuadratic, rate * multiplier);
    }
    static FeeModel polymarketBps(double bps) { return FeeModel(Kind::PolyBps, bps / 10000.0); }
    static FeeModel polymarketQuadratic(double theta) {
        return FeeModel(Kind::PolyQuadratic, theta);
    }
    static FeeModel zero() { return FeeModel(Kind::Zero, 0.0); }

    FeeModel() = default;

    // Taker fee for one fill of `qty` centi-contracts at `price` micros.
    Micros takerFee(Micros price, Qty qty) const;
    Kind kind() const { return kind_; }
    double coefficient() const { return coef_; }

private:
    FeeModel(Kind k, double coef) : kind_(k), coef_(coef) {}
    Kind kind_ = Kind::Zero;
    double coef_ = 0.0;
};

// Round micro-dollars up to the next whole cent.
Micros ceilToCent(long double micros);

}  // namespace artemis
