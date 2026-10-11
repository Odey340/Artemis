#include <gtest/gtest.h>

#include "strategy/SignalGenerator.hpp"

using namespace artemis;

TEST(SignalGenerator, EntersAndExitsWithHysteresis) {
    SignalGenerator g(2.0, 0.5);
    EXPECT_EQ(g.evaluate(1.0), Signal::Flat);
    EXPECT_EQ(g.evaluate(-2.1), Signal::Long);
    EXPECT_EQ(g.evaluate(-1.0), Signal::Long);  // still beyond exit band
    EXPECT_EQ(g.evaluate(-0.4), Signal::Flat);
    EXPECT_EQ(g.evaluate(2.5), Signal::Short);
    EXPECT_EQ(g.evaluate(0.6), Signal::Short);
    EXPECT_EQ(g.evaluate(0.5), Signal::Flat);
}

TEST(SignalGenerator, Reset) {
    SignalGenerator g(1.0, 0.1);
    g.evaluate(5);
    EXPECT_EQ(g.state(), Signal::Short);
    g.reset();
    EXPECT_EQ(g.state(), Signal::Flat);
}

struct AlwaysLong : SignalGenerator {
    AlwaysLong() : SignalGenerator(0, 0) {}
    Signal evaluate(double) override { return state_ = Signal::Long; }
};

TEST(SignalGenerator, Overridable) {
    AlwaysLong g;
    SignalGenerator& base = g;
    EXPECT_EQ(base.evaluate(0.0), Signal::Long);
}

TEST(BasisMonitor, FlagsDislocation) {
    BasisMonitor m(50, 20, 2.5, 0.5);
    for (int i = 0; i < 200; ++i) m.update((i % 2) ? 0.5 : -0.5);
    EXPECT_EQ(m.last, Signal::Flat);
    EXPECT_EQ(m.update(6.0), Signal::Short);  // Kalshi rich vs Polymarket
}
