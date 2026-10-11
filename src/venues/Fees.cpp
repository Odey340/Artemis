#include "venues/Fees.hpp"

#include <algorithm>
#include <cmath>

namespace artemis {

Micros ceilToCent(long double micros) {
    if (micros <= 0) return 0;
    // snap away binary noise (e.g. 1.75000000001 cents worth of error)
    const long double cents = micros / static_cast<long double>(kOneCent);
    const long double snapped = std::nearbyint(cents * 1e6L) / 1e6L;
    return static_cast<Micros>(std::ceil(snapped)) * kOneCent;
}

Micros FeeModel::takerFee(Micros price, Qty qty) const {
    if (qty <= 0 || price <= 0 || price >= kOneDollar || coef_ <= 0.0) return 0;
    const long double p = static_cast<long double>(price) / kOneDollar;
    const long double contracts = static_cast<long double>(qty) / kOneContract;
    long double dollars = 0;
    switch (kind_) {
        case Kind::KalshiQuadratic:
        case Kind::PolyQuadratic:
            dollars = coef_ * contracts * p * (1.0L - p);
            break;
        case Kind::PolyBps:
            dollars = coef_ * contracts * std::min(p, 1.0L - p);
            break;
        case Kind::Zero:
            return 0;
    }
    return ceilToCent(dollars * kOneDollar);
}

}  // namespace artemis
