#pragma once
// Exponentially-weighted rolling mean / variance / z-score (Artemis EWMA engine).
//
// O(1) per update, no allocation, no locks. Used for the cross-venue basis
// monitor, feed latency tracking and opportunity persistence statistics.

#include <cmath>
#include <cstdint>

namespace artemis {

class RollingStatistics {
public:
    // `halfLife` is expressed in samples; alpha = 1 - 2^(-1/halfLife).
    explicit RollingStatistics(double halfLife = 50.0, std::uint32_t warmup = 20);

    void update(double x);
    void reset();

    double mean() const { return mean_; }
    double variance() const { return var_; }
    double stddev() const { return std::sqrt(var_ > 0.0 ? var_ : 0.0); }
    // z-score of `x` against the current distribution (0 while warming up).
    double zscore(double x) const;
    // z-score of the most recent observation.
    double lastZ() const { return lastZ_; }
    double last() const { return last_; }
    std::uint64_t count() const { return n_; }
    bool ready() const { return n_ >= warmup_; }
    double alpha() const { return alpha_; }

private:
    double alpha_;
    std::uint32_t warmup_;
    std::uint64_t n_ = 0;
    double mean_ = 0.0;
    double var_ = 0.0;
    double last_ = 0.0;
    double lastZ_ = 0.0;
};

}  // namespace artemis
