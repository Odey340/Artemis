#include "risk/RiskManager.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>

#include "core/Clock.hpp"
#include "core/FixedPoint.hpp"
#include "exec/ExecutionReport.hpp"

namespace artemis {

namespace {
Micros usdToMicros(double usd) { return static_cast<Micros>(std::llround(usd * kOneDollar)); }
}  // namespace

RiskManager::RiskManager(const RiskConfig& cfg) : cfg_(cfg) {}

bool RiskManager::killSwitch() const {
    if (cfg_.killSwitchFile.empty()) return false;
    std::ifstream f(cfg_.killSwitchFile);
    return f.good();
}

Micros RiskManager::realizedToday(std::int64_t nowUs) const {
    auto it = dailyPnl_.find(utcDay(nowUs));
    return it == dailyPnl_.end() ? 0 : it->second;
}

int RiskManager::openBaskets(const std::string& group) const {
    auto it = groupBaskets_.find(group);
    return it == groupBaskets_.end() ? 0 : it->second;
}

RiskDecision RiskManager::check(const Opportunity& opp, std::int64_t nowUs) {
    RiskDecision d;
    auto reject = [&](std::string why) {
        d.approved = false;
        d.reason = std::move(why);
        return d;
    };

    if (killSwitch()) halt("kill switch file '" + cfg_.killSwitchFile + "' present");
    if (halted()) return reject("halted: " + haltReason_);

    if (-realizedToday(nowUs) >= usdToMicros(cfg_.maxDailyLossUsd)) {
        halt("daily loss limit reached");
        return reject("daily loss limit");
    }

    auto last = lastTradeByKey_.find(opp.key);
    if (last != lastTradeByKey_.end() &&
        nowUs - last->second < static_cast<std::int64_t>(cfg_.cooldownMs) * 1000) {
        return reject("cooldown");
    }

    while (!tradeTimes_.empty() && nowUs - tradeTimes_.front() > 3'600'000'000LL)
        tradeTimes_.pop_front();
    if (static_cast<int>(tradeTimes_.size()) >= cfg_.maxTradesPerHour)
        return reject("max trades per hour");

    if (openBaskets(opp.groupId) >= cfg_.maxOpenBasketsPerEvent)
        return reject("max open baskets for " + opp.groupId);

    Micros budget = usdToMicros(cfg_.maxCostPerTradeUsd);
    budget = std::min(budget, usdToMicros(cfg_.maxTotalExposureUsd) - exposure_);
    if (hasBalance_) {
        budget = std::min(budget, static_cast<Micros>(static_cast<double>(balance_) *
                                                      cfg_.balanceUtilisation));
    }
    if (budget <= 0) return reject("no risk budget (exposure/balance)");

    d.approved = true;
    d.maxCost = budget;
    return d;
}

void RiskManager::onExecution(const Opportunity& opp, const ExecutionReport& rep,
                              std::int64_t nowUs) {
    if (rep.status == ExecStatus::Skipped) return;
    lastTradeByKey_[opp.key] = nowUs;
    if (rep.traded()) tradeTimes_.push_back(nowUs);

    const Micros held = rep.cost + rep.strandedCost;
    if (held > 0) {
        exposure_ += held;
        groupExposure_[opp.groupId] += held;
        if (rep.status == ExecStatus::Completed || rep.status == ExecStatus::Shrunk)
            ++groupBaskets_[opp.groupId];
    }
    if (hasBalance_) balance_ += rep.cashFlow;

    // Loss accounting: realised P&L from unwinds, plus any *locked-in* loss
    // (a basket completed above its payout after a leg had to be re-priced).
    // Locked gains are not counted until settlement.
    dailyPnl_[utcDay(nowUs)] += rep.realizedPnl + std::min<Micros>(0, rep.lockedProfit) -
                                rep.strandedCost;

    if (rep.failure()) {
        ++consecutiveFailures_;
        if (rep.status == ExecStatus::Stranded) halt("stranded exposure in " + opp.groupId);
        if (consecutiveFailures_ >= cfg_.maxConsecutiveFailures)
            halt("too many consecutive execution failures");
    } else if (rep.traded()) {
        consecutiveFailures_ = 0;
    }
    if (-realizedToday(nowUs) >= usdToMicros(cfg_.maxDailyLossUsd)) halt("daily loss limit reached");
}

void RiskManager::releaseGroup(const std::string& groupId) {
    auto it = groupExposure_.find(groupId);
    if (it != groupExposure_.end()) {
        exposure_ -= it->second;
        groupExposure_.erase(it);
    }
    groupBaskets_.erase(groupId);
}

}  // namespace artemis
