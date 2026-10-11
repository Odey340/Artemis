#include "core/RollingStatistics.hpp"

#include <algorithm>

namespace artemis {

RollingStatistics::RollingStatistics(double halfLife, std::uint32_t warmup)
    : alpha_(1.0 - std::pow(2.0, -1.0 / std::max(halfLife, 1e-9))), warmup_(warmup) {}

void RollingStatistics::update(double x) {
    if (!std::isfinite(x)) return;
    if (n_ == 0) {
        mean_ = x;
        var_ = 0.0;
    } else {
        // West's incremental EWMA (numerically stable, single pass)
        const double diff = x - mean_;
        const double incr = alpha_ * diff;
        mean_ += incr;
        var_ = (1.0 - alpha_) * (var_ + diff * incr);
    }
    ++n_;
    last_ = x;
    lastZ_ = zscore(x);
}

void RollingStatistics::reset() {
    n_ = 0;
    mean_ = var_ = last_ = lastZ_ = 0.0;
}

double RollingStatistics::zscore(double x) const {
    if (!ready()) return 0.0;
    const double sd = stddev();
    if (sd < 1e-12) return 0.0;
    return (x - mean_) / sd;
}

}  // namespace artemis
