#include <gtest/gtest.h>

#include "TestUtil.hpp"
#include "venues/Fees.hpp"

using namespace artemis;
using namespace artemis::test;

TEST(Fees, KalshiGeneralScheduleExamples) {
    const FeeModel k = FeeModel::kalshi();
    // 0.07 * 100 * 0.5 * 0.5 = 1.75 exactly -> $1.75 (no spurious round-up)
    EXPECT_EQ(k.takerFee(c(50), n(100)), usd(1.75));
    // 0.07 * 1 * 0.25 = 0.0175 -> rounds up to $0.02
    EXPECT_EQ(k.takerFee(c(50), n(1)), usd(0.02));
    // 0.07 * 10 * 0.1 * 0.9 = 0.063 -> $0.07
    EXPECT_EQ(k.takerFee(c(10), n(10)), usd(0.07));
    // 0.07 * 1 * 0.01 * 0.99 = 0.000693 -> $0.01
    EXPECT_EQ(k.takerFee(c(1), n(1)), usd(0.01));
    // 0.07 * 200 * 0.35 * 0.65 = 3.185 -> $3.19
    EXPECT_EQ(k.takerFee(c(35), n(200)), usd(3.19));
}

TEST(Fees, KalshiMultiplier) {
    const FeeModel half = FeeModel::kalshi(0.07, 0.5);
    EXPECT_EQ(half.takerFee(c(50), n(100)), usd(0.88));  // 0.875 -> 0.88
    const FeeModel zero = FeeModel::kalshi(0.07, 0.0);
    EXPECT_EQ(zero.takerFee(c(50), n(100)), 0);
}

TEST(Fees, SymmetricInPrice) {
    const FeeModel k = FeeModel::kalshi();
    EXPECT_EQ(k.takerFee(c(30), n(37)), k.takerFee(c(70), n(37)));
}

TEST(Fees, Polymarket) {
    EXPECT_EQ(FeeModel::polymarketBps(0).takerFee(c(40), n(100)), 0);
    // 100 bps * 100 * min(0.4, 0.6) = 0.40
    EXPECT_EQ(FeeModel::polymarketBps(100).takerFee(c(40), n(100)), usd(0.40));
    // 0.0695 * 1000 * 0.25 = 17.375 -> 17.38
    EXPECT_EQ(FeeModel::polymarketQuadratic(0.0695).takerFee(c(50), n(1000)), usd(17.38));
}

TEST(Fees, DegenerateInputs) {
    const FeeModel k = FeeModel::kalshi();
    EXPECT_EQ(k.takerFee(0, n(10)), 0);
    EXPECT_EQ(k.takerFee(kOneDollar, n(10)), 0);
    EXPECT_EQ(k.takerFee(c(50), 0), 0);
    EXPECT_EQ(FeeModel::zero().takerFee(c(50), n(10)), 0);
    EXPECT_EQ(ceilToCent(0.0L), 0);
    EXPECT_EQ(ceilToCent(1.0L), kOneCent);
}
