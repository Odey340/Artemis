#include <gtest/gtest.h>

#include "core/FixedPoint.hpp"

using namespace artemis;

TEST(FixedPoint, ParsesDollarStringsExactly) {
    EXPECT_EQ(fp::parseDollars("0.56").value(), 560000);
    EXPECT_EQ(fp::parseDollars("0.5600").value(), 560000);
    EXPECT_EQ(fp::parseDollars("0.123456").value(), 123456);
    EXPECT_EQ(fp::parseDollars("1").value(), 1000000);
    EXPECT_EQ(fp::parseDollars("12.5").value(), 12500000);
    EXPECT_EQ(fp::parseDollars("-0.01").value(), -10000);
    EXPECT_EQ(fp::parseDollars(" 0.07 ").value(), 70000);
}

TEST(FixedPoint, TruncatesExtraPrecision) {
    EXPECT_EQ(fp::parseDollars("0.1234569").value(), 123456);
    EXPECT_EQ(fp::parseCount("10.009").value(), 1000);
}

TEST(FixedPoint, ParsesCounts) {
    EXPECT_EQ(fp::parseCount("10.00").value(), 1000);
    EXPECT_EQ(fp::parseCount("2.5").value(), 250);
    EXPECT_EQ(fp::parseCount("0").value(), 0);
}

TEST(FixedPoint, RejectsGarbage) {
    EXPECT_FALSE(fp::parseDollars("").has_value());
    EXPECT_FALSE(fp::parseDollars("abc").has_value());
    EXPECT_FALSE(fp::parseDollars("1.2.3").has_value());
    EXPECT_FALSE(fp::parseDollars("-").has_value());
    EXPECT_FALSE(fp::parseDollars(".").has_value());
    EXPECT_FALSE(fp::parseDollars("99999999999999999999").has_value());
}

TEST(FixedPoint, Formats) {
    EXPECT_EQ(fp::formatDollars(560000), "0.5600");
    EXPECT_EQ(fp::formatDollars(560000, 2), "0.56");
    EXPECT_EQ(fp::formatDollars(-10000, 2), "-0.01");
    EXPECT_EQ(fp::formatDollars(1000000, 0), "1");
    EXPECT_EQ(fp::formatCount(1250), "12.50");
    EXPECT_EQ(fp::formatCount(5), "0.05");
    EXPECT_EQ(fp::usd(1234567), "$1.23");
    EXPECT_EQ(fp::usd(-5000), "-$0.01");
    EXPECT_EQ(fp::cents(563000), "56.3c");
}

TEST(FixedPoint, RoundTrip) {
    for (Micros m : {0LL, 10000LL, 990000LL, 123400LL, 5000LL}) {
        EXPECT_EQ(fp::parseDollars(fp::formatDollars(m, 6)).value(), m);
    }
}
