#pragma once
// Fee-aware, depth-aware arbitrage detection.
//
// Three structures are scanned:
//
//  1. NO basket (Kalshi, mutually-exclusive events). Buying NO on a set S of
//     outcomes of which at most one can resolve YES pays at least |S|-1.
//     Leg i adds (1 - noAsk_i - fee_i) to the margin, so the optimal set is
//     every leg where that is positive. Works without assuming the outcome
//     list is exhaustive.
//  2. YES basket (Kalshi, events the operator has confirmed are exhaustive).
//     Buying YES on every outcome pays exactly 1.
//  3. Cross-venue (Kalshi <-> Polymarket, mapped propositions). YES on one
//     venue + NO on the other pays exactly 1.
//
// Each candidate is sized by walking the real order-book depth one contract
// at a time and keeping the size that maximises profit after (rounded-up)
// fees and an optional per-leg safety margin.

#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/Config.hpp"
#include "strategy/BookStore.hpp"
#include "strategy/Opportunity.hpp"
#include "strategy/SignalGenerator.hpp"
#include "venues/Fees.hpp"
#include "venues/MarketTypes.hpp"
#include "venues/polymarket/PolymarketClient.hpp"

namespace artemis {

struct LegSpec {
    const BinaryBook* book = nullptr;
    std::string bookKey;
    Venue venue = Venue::Kalshi;
    std::string marketId;
    Outcome outcome = Outcome::No;
    FeeModel fee;
};

struct SizingResult {
    bool ok = false;
    Qty units = 0;
    std::vector<Leg> legs;
    Micros cost = 0;
    Micros fees = 0;
    Micros profit = 0;
};

// Pure sizing routine (exposed for tests and for re-validation before execution).
SizingResult sizeBasket(const std::vector<LegSpec>& legs, Micros payoutPerUnit, Qty maxUnits,
                        Micros minProfit, Micros minEdgePerUnit, double minRoi,
                        Micros safetyPerLegPerUnit);

// Fee + cost of buying exactly `qty` along the ask ladder (nullopt if not enough depth).
struct LegQuote {
    Micros cost = 0;
    Micros fee = 0;
    Micros worst = 0;
    Micros best = 0;
};
std::optional<LegQuote> quoteBuy(const BinaryBook& book, Outcome o, Qty qty, const FeeModel& fee);

class ArbDetector {
public:
    ArbDetector(const StrategyConfig& s, const FeedConfig& f, const PolymarketConfig& p,
                const BasisConfig& b);

    // Evaluate one Kalshi event (NO basket + YES basket if eligible).
    std::vector<Opportunity> evaluateEvent(const EventInfo& ev, const BookStore& books,
                                           std::int64_t nowUs) const;
    // Evaluate one cross-venue pair (both directions).
    std::vector<Opportunity> evaluatePair(const MarketPair& pair, const BookStore& books,
                                          std::int64_t nowUs) const;

    // Evaluate everything (or only `dirty` group ids if given) and apply the
    // confirmation filter. Also updates the per-pair basis monitors.
    std::vector<Opportunity> scan(const std::vector<EventInfo>& events,
                                  const std::vector<MarketPair>& pairs, const BookStore& books,
                                  std::int64_t nowUs,
                                  const std::unordered_set<std::string>* dirty = nullptr);

    // Re-size an opportunity on the current books with a tighter unit cap
    // (e.g. to fit the risk budget). nullopt if it is no longer worth trading.
    std::optional<Opportunity> resize(const Opportunity& o, const BookStore& books,
                                      Qty maxUnits) const;

    FeeModel kalshiFee(const EventInfo& ev) const;
    FeeModel kalshiFeeForTicker(const std::string& ticker) const;
    FeeModel polyFee(const MarketPair& pair) const;
    void setTickerFees(std::unordered_map<std::string, FeeModel> m) { tickerFees_ = std::move(m); }

    const std::map<std::string, BasisMonitor>& basis() const { return basis_; }

private:
    bool fresh(const BinaryBook& b, std::int64_t nowUs) const;
    Opportunity finish(ArbKind kind, const std::string& group, const std::string& label,
                       SizingResult&& r, Micros payoutPerUnit, std::int64_t nowUs) const;
    void updateBasis(const MarketPair& pair, const BookStore& books, std::int64_t nowUs);

    StrategyConfig s_;
    FeedConfig f_;
    PolymarketConfig p_;
    BasisConfig b_;
    std::unordered_map<std::string, FeeModel> tickerFees_;
    std::unordered_map<std::string, std::pair<std::string, int>> streak_;  // key -> (group, n)
    std::map<std::string, BasisMonitor> basis_;
};

}  // namespace artemis
