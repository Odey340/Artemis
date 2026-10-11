#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "TestUtil.hpp"
#include "exec/ExecutionReport.hpp"
#include "risk/RiskManager.hpp"

using namespace artemis;
using namespace artemis::test;

namespace {
Opportunity opp(const std::string& group = "E") {
    Opportunity o;
    o.key = "no_basket:" + group;
    o.groupId = group;
    o.units = n(10);
    o.cost = usd(9);
    o.fees = usd(0.3);
    o.profit = usd(0.7);
    return o;
}
ExecutionReport completed(Micros cost = usd(9)) {
    ExecutionReport r;
    r.status = ExecStatus::Completed;
    r.units = n(10);
    r.cost = cost;
    r.fees = usd(0.3);
    r.cashFlow = -(cost + usd(0.3));
    r.lockedProfit = usd(0.7);
    return r;
}
RiskConfig cfg() {
    RiskConfig c;
    c.killSwitchFile = "";
    return c;
}
constexpr std::int64_t T = 1'800'000'000'000'000;
}  // namespace

TEST(RiskManager, ApprovesWithinBudget) {
    RiskConfig c = cfg();
    c.maxCostPerTradeUsd = 50;
    RiskManager r(c);
    auto d = r.check(opp(), T);
    EXPECT_TRUE(d.approved);
    EXPECT_EQ(d.maxCost, usd(50));
}

TEST(RiskManager, BudgetLimitedByExposureAndBalance) {
    RiskConfig c = cfg();
    c.maxCostPerTradeUsd = 100;
    c.maxTotalExposureUsd = 20;
    c.balanceUtilisation = 0.5;
    RiskManager r(c);
    r.setAvailableBalance(usd(30));
    EXPECT_EQ(r.check(opp(), T).maxCost, usd(15));  // 50% of $30 balance
    r.setAvailableBalance(usd(1000));
    r.onExecution(opp("X"), completed(usd(12)), T);
    EXPECT_EQ(r.exposure(), usd(12));
    EXPECT_EQ(r.check(opp("Y"), T).maxCost, usd(8));  // $20 cap - $12 used
    r.releaseGroup("X");
    EXPECT_EQ(r.exposure(), 0);
}

TEST(RiskManager, BalanceTracksCashFlow) {
    RiskManager r(cfg());
    r.setAvailableBalance(usd(100));
    r.onExecution(opp(), completed(), T);
    EXPECT_EQ(r.availableBalance(), usd(100 - 9.3));
}

TEST(RiskManager, CooldownPerOpportunity) {
    RiskConfig c = cfg();
    c.cooldownMs = 1000;
    RiskManager r(c);
    r.onExecution(opp(), completed(), T);
    EXPECT_EQ(r.check(opp(), T + 500'000).reason, "cooldown");
    EXPECT_TRUE(r.check(opp("other"), T + 500'000).approved);
    EXPECT_TRUE(r.check(opp(), T + 1'500'000).approved);
}

TEST(RiskManager, TradesPerHour) {
    RiskConfig c = cfg();
    c.maxTradesPerHour = 2;
    c.cooldownMs = 0;
    RiskManager r(c);
    r.onExecution(opp("a"), completed(), T);
    r.onExecution(opp("b"), completed(), T + 1);
    EXPECT_EQ(r.check(opp("c"), T + 2).reason, "max trades per hour");
    EXPECT_TRUE(r.check(opp("c"), T + 3'700'000'000LL).approved);
}

TEST(RiskManager, MaxOpenBasketsPerEvent) {
    RiskConfig c = cfg();
    c.maxOpenBasketsPerEvent = 1;
    c.cooldownMs = 0;
    RiskManager r(c);
    r.onExecution(opp(), completed(), T);
    EXPECT_FALSE(r.check(opp(), T + 10).approved);
}

TEST(RiskManager, DailyLossHalts) {
    RiskConfig c = cfg();
    c.maxDailyLossUsd = 1;
    RiskManager r(c);
    ExecutionReport loss;
    loss.status = ExecStatus::Unwound;
    loss.realizedPnl = -usd(1.5);
    r.onExecution(opp(), loss, T);
    EXPECT_TRUE(r.halted());
    EXPECT_FALSE(r.check(opp("z"), T).approved);
    r.resume();
    // still over the limit for today -> halts again on next check
    EXPECT_FALSE(r.check(opp("z"), T).approved);
}

TEST(RiskManager, StrandedAndConsecutiveFailuresHalt) {
    RiskConfig c = cfg();
    c.maxConsecutiveFailures = 2;
    c.maxDailyLossUsd = 1000;
    RiskManager r(c);
    ExecutionReport s;
    s.status = ExecStatus::Stranded;
    s.strandedCost = usd(5);
    r.onExecution(opp(), s, T);
    EXPECT_TRUE(r.halted());

    RiskManager r2(c);
    ExecutionReport u;
    u.status = ExecStatus::Unwound;
    u.realizedPnl = -usd(0.1);
    r2.onExecution(opp("a"), u, T);
    EXPECT_FALSE(r2.halted());
    r2.onExecution(opp("b"), u, T);
    EXPECT_TRUE(r2.halted());
}

TEST(RiskManager, KillSwitchFile) {
    const std::string path = "artemis_test_kill_switch";
    RiskConfig c = cfg();
    c.killSwitchFile = path;
    RiskManager r(c);
    EXPECT_TRUE(r.check(opp(), T).approved);
    { std::ofstream(path) << "stop"; }
    EXPECT_FALSE(r.check(opp("q"), T).approved);
    EXPECT_TRUE(r.halted());
    std::filesystem::remove(path);
}
