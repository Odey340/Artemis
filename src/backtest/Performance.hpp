#pragma once
// Equity-curve statistics (Artemis performance module).

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace artemis {

struct EquityPoint {
    std::int64_t tsUs = 0;
    double equity = 0.0;  // USD
};

struct PerformanceSummary {
    double startEquity = 0.0;
    double endEquity = 0.0;
    double totalPnl = 0.0;
    double returnPct = 0.0;
    double sharpe = 0.0;          // annualised from bucketed returns
    double maxDrawdown = 0.0;     // USD
    double maxDrawdownPct = 0.0;  // of running peak
    int buckets = 0;
    double bucketSeconds = 3600.0;
};

// Buckets the curve into `bucketSeconds` intervals, computes per-bucket
// returns and annualises: sharpe = mean/std * sqrt(seconds_per_year / bucket).
PerformanceSummary computePerformance(const std::vector<EquityPoint>& curve,
                                      double bucketSeconds = 3600.0);

double percentile(std::vector<double> v, double p);

}  // namespace artemis
