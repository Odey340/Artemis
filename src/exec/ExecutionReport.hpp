#pragma once

#include <string>
#include <vector>

#include "core/Types.hpp"
#include "venues/MarketTypes.hpp"

namespace artemis {

enum class ExecStatus {
    Completed,   // every leg filled: payoff locked in
    Shrunk,      // NO basket finished on a subset of legs (still hedged)
    Unwound,     // a leg failed; filled legs were sold back
    Stranded,    // a leg failed and some exposure could not be unwound
    NotFilled,   // nothing traded (first leg killed / edge vanished)
    Skipped,     // not executed (venue not tradable live, dry run, ...)
    Error,
};

inline const char* toString(ExecStatus s) {
    switch (s) {
        case ExecStatus::Completed: return "completed";
        case ExecStatus::Shrunk: return "shrunk";
        case ExecStatus::Unwound: return "unwound";
        case ExecStatus::Stranded: return "stranded";
        case ExecStatus::NotFilled: return "not_filled";
        case ExecStatus::Skipped: return "skipped";
        case ExecStatus::Error: return "error";
    }
    return "?";
}

struct ExecutionReport {
    ExecStatus status = ExecStatus::NotFilled;
    Qty units = 0;
    Micros cost = 0;           // total spent on legs still held
    Micros fees = 0;           // all fees paid (incl. unwind)
    Micros guaranteedPayout = 0;
    Micros lockedProfit = 0;   // guaranteedPayout - cost - fees for hedged holdings
    Micros realizedPnl = 0;    // cash P&L already realised (unwinds)
    Micros strandedCost = 0;   // cost of unhedged legs left open
    Micros cashFlow = 0;       // net cash: sell proceeds - buy cost - all fees
    std::vector<OrderResult> orders;
    std::string note;

    bool traded() const {
        return status == ExecStatus::Completed || status == ExecStatus::Shrunk ||
               status == ExecStatus::Unwound || status == ExecStatus::Stranded;
    }
    bool failure() const {
        return status == ExecStatus::Unwound || status == ExecStatus::Stranded ||
               status == ExecStatus::Error;
    }
};

}  // namespace artemis
