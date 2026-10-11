#include <gtest/gtest.h>

#include <cmath>
#include <random>

#include "core/RollingStatistics.hpp"

using namespace artemis;

TEST(RollingStatistics, FirstSampleSetsMean) {
    RollingStatistics s(10, 1);
    s.update(5.0);
    EXPECT_DOUBLE_EQ(s.mean(), 5.0);
    EXPECT_DOUBLE_EQ(s.variance(), 0.0);
    EXPECT_EQ(s.count(), 1u);
}

TEST(RollingStatistics, ConvergesToStationaryMoments) {
    RollingStatistics s(500, 10);
    std::mt19937 rng(42);
    std::normal_distribution<double> d(3.0, 2.0);
    for (int i = 0; i < 50000; ++i) s.update(d(rng));
    EXPECT_NEAR(s.mean(), 3.0, 0.25);
    EXPECT_NEAR(s.stddev(), 2.0, 0.25);
}

TEST(RollingStatistics, HalfLifeControlsDecay) {
    RollingStatistics s(1.0, 1);  // alpha = 0.5
    EXPECT_NEAR(s.alpha(), 0.5, 1e-12);
    s.update(0.0);
    s.update(10.0);
    EXPECT_NEAR(s.mean(), 5.0, 1e-12);
}

TEST(RollingStatistics, ZScoreZeroDuringWarmup) {
    RollingStatistics s(10, 5);
    for (int i = 0; i < 4; ++i) s.update(i);
    EXPECT_FALSE(s.ready());
    EXPECT_EQ(s.zscore(100.0), 0.0);
    s.update(1.0);
    EXPECT_TRUE(s.ready());
    EXPECT_GT(s.zscore(100.0), 3.0);
}

TEST(RollingStatistics, IgnoresNonFinite) {
    RollingStatistics s(10, 1);
    s.update(1.0);
    s.update(std::nan(""));
    s.update(INFINITY);
    EXPECT_EQ(s.count(), 1u);
    EXPECT_DOUBLE_EQ(s.mean(), 1.0);
}

TEST(RollingStatistics, Reset) {
    RollingStatistics s(10, 1);
    s.update(3);
    s.reset();
    EXPECT_EQ(s.count(), 0u);
    EXPECT_EQ(s.mean(), 0.0);
}
