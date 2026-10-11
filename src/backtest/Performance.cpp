#include "backtest/Performance.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace artemis {

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double idx = std::clamp(p, 0.0, 1.0) * static_cast<double>(v.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(std::floor(idx));
    const std::size_t hi = static_cast<std::size_t>(std::ceil(idx));
    const double w = idx - static_cast<double>(lo);
    return v[lo] * (1.0 - w) + v[hi] * w;
}

PerformanceSummary computePerformance(const std::vector<EquityPoint>& curve,
                                      double bucketSeconds) {
    PerformanceSummary s;
    s.bucketSeconds = bucketSeconds;
    if (curve.empty()) return s;
    s.startEquity = curve.front().equity;
    s.endEquity = curve.back().equity;
    s.totalPnl = s.endEquity - s.startEquity;
    s.returnPct = s.startEquity != 0.0 ? 100.0 * s.totalPnl / s.startEquity : 0.0;

    double peak = curve.front().equity;
    for (const auto& p : curve) {
        peak = std::max(peak, p.equity);
        const double dd = peak - p.equity;
        if (dd > s.maxDrawdown) {
            s.maxDrawdown = dd;
            s.maxDrawdownPct = peak > 0 ? 100.0 * dd / peak : 0.0;
        }
    }

    // last equity value in each bucket, forward-filled across empty buckets
    const std::int64_t bucketUs = static_cast<std::int64_t>(bucketSeconds * 1e6);
    if (bucketUs <= 0) return s;
    const std::int64_t first = curve.front().tsUs / bucketUs;
    const std::int64_t last = curve.back().tsUs / bucketUs;
    std::map<std::int64_t, double> closeByBucket;
    for (const auto& p : curve) closeByBucket[p.tsUs / bucketUs] = p.equity;
    std::vector<double> closes;
    double prev = curve.front().equity;
    for (std::int64_t b = first; b <= last && b - first < 10'000'000; ++b) {
        auto it = closeByBucket.find(b);
        if (it != closeByBucket.end()) prev = it->second;
        closes.push_back(prev);
    }
    std::vector<double> rets;
    double open = curve.front().equity;
    for (double c : closes) {
        if (open != 0.0) rets.push_back((c - open) / open);
        open = c;
    }
    s.buckets = static_cast<int>(rets.size());
    if (rets.size() >= 2) {
        double mean = 0;
        for (double r : rets) mean += r;
        mean /= static_cast<double>(rets.size());
        double var = 0;
        for (double r : rets) var += (r - mean) * (r - mean);
        var /= static_cast<double>(rets.size() - 1);
        const double sd = std::sqrt(var);
        if (sd > 1e-15) s.sharpe = mean / sd * std::sqrt(365.25 * 86400.0 / bucketSeconds);
    }
    return s;
}

}  // namespace artemis
