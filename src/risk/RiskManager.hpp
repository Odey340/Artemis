#pragma once
// Pre-trade limits and post-trade bookkeeping.
//
// The risk manager never sizes trades up; it tells the engine the largest
// cost it may commit for an opportunity (0 = rejected) and the engine
// re-sizes the basket to fit.

#include <deque>
#include <map>
#include <string>
#include <unordered_map>

#include "core/Config.hpp"
#include "core/Types.hpp"
#include "strategy/Opportunity.hpp"

namespace artemis {

struct ExecutionReport;

struct RiskDecision {
    bool approved = false;
    Micros maxCost = 0;  // cost + fees budget for this trade
    std::string reason;
};

class RiskManager {
public:
    explicit RiskManager(const RiskConfig& cfg);

    RiskDecision check(const Opportunity& opp, std::int64_t nowUs);
    void onExecution(const Opportunity& opp, const ExecutionReport& rep, std::int64_t nowUs);
    // Release capital tied up in a group once its markets have settled/closed.
    void releaseGroup(const std::string& groupId);

    void setAvailableBalance(Micros b) { balance_ = b; hasBalance_ = true; }
    Micros availableBalance() const { return balance_; }

    bool halted() const { return !haltReason_.empty(); }
    const std::string& haltReason() const { return haltReason_; }
    void halt(const std::string& why) { if (haltReason_.empty()) haltReason_ = why; }
    void resume() { haltReason_.clear(); consecutiveFailures_ = 0; }

    Micros exposure() const { return exposure_; }
    Micros realizedToday(std::int64_t nowUs) const;
    int openBaskets(const std::string& group) const;

private:
    bool killSwitch() const;

    RiskConfig cfg_;
    Micros balance_ = 0;
    bool hasBalance_ = false;
    Micros exposure_ = 0;
    std::unordered_map<std::string, Micros> groupExposure_;
    std::unordered_map<std::string, int> groupBaskets_;
    std::unordered_map<std::string, std::int64_t> lastTradeByKey_;
    std::deque<std::int64_t> tradeTimes_;
    std::map<std::int64_t, Micros> dailyPnl_;  // utc day -> realised pnl
    int consecutiveFailures_ = 0;
    std::string haltReason_;
};

}  // namespace artemis
