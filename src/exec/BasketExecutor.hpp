#pragma once
// Multi-leg execution with leg-risk management.
//
//  1. Re-validate: pull fresh books for every leg and re-size the basket. If
//     the edge is gone, nothing is sent.
//  2. Fire legs as fill-or-kill orders, thinnest leg first (so the leg most
//     likely to fail fails before anything else is bought), or all at once in
//     "batch" mode.
//  3. If a leg is killed, price three ways out on fresh books and take the
//     best one:
//        A. complete  - buy the missing legs at the new prices
//        B. shrink    - (NO baskets) keep the fully-filled subset, which is
//                       still a hedged basket paying (|subset| - 1)
//        C. unwind    - sell everything back at the bid
//     Completion is retried up to `max_completion_retries` times.
//  4. Ambiguous order states (network failure mid-request) are reconciled by
//     client_order_id; anything still unknown is reported as stranded so the
//     risk manager halts trading for a human to look.

#include "core/Config.hpp"
#include "exec/ExecutionReport.hpp"
#include "exec/OrderGateway.hpp"
#include "strategy/Opportunity.hpp"

namespace artemis {

class BasketExecutor {
public:
    BasketExecutor(OrderGateway& gw, const ExecutionConfig& e, const StrategyConfig& s)
        : gw_(gw), e_(e), s_(s) {}

    // `maxUnits` caps the size (risk budget); the executor may trade less.
    ExecutionReport execute(const Opportunity& opp, Qty maxUnits);

private:
    struct LegState {
        Leg leg;
        BinaryBook book;
        Qty held = 0;
        Micros heldCost = 0;
        Micros buyFees = 0;
        bool uncertain = false;
    };

    void applyBuy(LegState& s, const OrderResult& r, ExecutionReport& rep);
    // Sell everything held on a leg (IOC). Returns false if some could not be sold.
    bool unwindLeg(LegState& s, ExecutionReport& rep);
    bool buyLeg(LegState& s, Qty qty, Micros limit, ExecutionReport& rep);
    void refreshBooks(std::vector<LegState>& st);
    ExecutionReport finalize(const Opportunity& opp, std::vector<LegState>& st, Qty units,
                             ExecutionReport rep, ExecStatus status);

    OrderGateway& gw_;
    ExecutionConfig e_;
    StrategyConfig s_;
};

}  // namespace artemis
