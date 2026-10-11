#include "exec/BasketExecutor.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <numeric>

#include "core/FixedPoint.hpp"
#include "strategy/ArbDetector.hpp"

namespace artemis {

namespace {

Micros cents(double c) { return static_cast<Micros>(std::llround(c * kOneCent)); }
Micros usd(double d) { return static_cast<Micros>(std::llround(d * kOneDollar)); }

struct SellQuote {
    Micros proceeds = 0;
    Micros fee = 0;
    Micros worst = 0;
    Qty filled = 0;
};

SellQuote quoteSell(const BinaryBook& b, Outcome o, Qty qty, const FeeModel& fm) {
    SellQuote q;
    const WalkResult w = b.walkBids(o, qty, [&](Micros p, Qty t) { q.fee += fm.takerFee(p, t); });
    q.proceeds = w.cost;
    q.worst = w.worstPrice;
    q.filled = w.filled;
    return q;
}

}  // namespace

void BasketExecutor::applyBuy(LegState& s, const OrderResult& r, ExecutionReport& rep) {
    rep.orders.push_back(r);
    if (r.ambiguous) s.uncertain = true;
    if (r.filled <= 0) return;
    const Micros cost = notional(r.avgPrice, r.filled);
    s.held += r.filled;
    s.heldCost += cost;
    s.buyFees += r.fees;
    rep.fees += r.fees;
    rep.cashFlow -= cost + r.fees;
}

bool BasketExecutor::buyLeg(LegState& s, Qty qty, Micros limit, ExecutionReport& rep) {
    OrderRequest req;
    req.venue = s.leg.venue;
    req.marketId = s.leg.marketId;
    req.clientOrderId = newClientOrderId();
    req.outcome = s.leg.outcome;
    req.buy = true;
    req.limitPrice = limit;
    req.qty = qty;
    req.timeInForce = "fill_or_kill";
    req.fee = s.leg.feeModel;
    const Qty before = s.held;
    const OrderResult r = gw_.submit(req);
    applyBuy(s, r, rep);
    spdlog::info("  BUY  {} {} {} x{} @<= {} -> filled {} {}", toString(s.leg.venue),
                 s.leg.marketId, toString(s.leg.outcome), fp::formatCount(qty), fp::cents(limit),
                 fp::formatCount(r.filled), r.error);
    return s.held - before >= qty;
}

bool BasketExecutor::unwindLeg(LegState& s, ExecutionReport& rep) {
    // Up to three IOC passes, re-reading the book each time: if the market
    // moves between our quote and our order, chase it rather than leaving the
    // position unhedged.
    for (int attempt = 0; attempt < 3 && s.held > 0; ++attempt) {
        if (attempt > 0) {
            if (auto b = gw_.refresh(s.leg)) s.book = std::move(*b);
        }
        const SellQuote q = quoteSell(s.book, s.leg.outcome, s.held, s.leg.feeModel);
        if (q.filled <= 0) continue;
        OrderRequest req;
        req.venue = s.leg.venue;
        req.marketId = s.leg.marketId;
        req.clientOrderId = newClientOrderId();
        req.outcome = s.leg.outcome;
        req.buy = false;
        req.limitPrice = q.worst;
        req.qty = std::min(s.held, q.filled);
        req.timeInForce = "immediate_or_cancel";
        req.reduceOnly = true;
        req.fee = s.leg.feeModel;
        const OrderResult r = gw_.submit(req);
        rep.orders.push_back(r);
        if (r.ambiguous) s.uncertain = true;
        spdlog::info("  SELL {} {} {} x{} @>= {} -> filled {} {}", toString(s.leg.venue),
                     s.leg.marketId, toString(s.leg.outcome), fp::formatCount(req.qty),
                     fp::cents(req.limitPrice), fp::formatCount(r.filled), r.error);
        if (r.filled > 0) {
            const Micros proceeds = notional(r.avgPrice, r.filled);
            const Micros costOut = s.heldCost * r.filled / s.held;
            const Micros feeOut = s.buyFees * r.filled / s.held;
            rep.realizedPnl += proceeds - r.fees - costOut - feeOut;
            rep.cashFlow += proceeds - r.fees;
            rep.fees += r.fees;
            s.heldCost -= costOut;
            s.buyFees -= feeOut;
            s.held -= r.filled;
        }
    }
    return s.held == 0;
}

void BasketExecutor::refreshBooks(std::vector<LegState>& st) {
    std::vector<Leg> legs;
    for (const auto& s : st) legs.push_back(s.leg);
    auto books = gw_.refreshAll(legs);
    for (std::size_t i = 0; i < st.size() && i < books.size(); ++i)
        if (books[i]) st[i].book = std::move(*books[i]);
}

ExecutionReport BasketExecutor::finalize(const Opportunity& opp, std::vector<LegState>& st,
                                         Qty units, ExecutionReport rep, ExecStatus status) {
    const bool uncertain =
        std::any_of(st.begin(), st.end(), [](const LegState& s) { return s.uncertain; });
    rep.units = units;
    if (status == ExecStatus::Completed || status == ExecStatus::Shrunk) {
        int hedged = 0;
        Micros cost = 0, fees = 0;
        for (const auto& s : st) {
            if (s.held >= units) {
                ++hedged;
                cost += s.heldCost;
                fees += s.buyFees;
            }
        }
        const Micros perUnit = status == ExecStatus::Completed
                                   ? opp.payoutPerUnit
                                   : static_cast<Micros>(hedged - 1) * kOneDollar;
        rep.cost = cost;
        rep.guaranteedPayout = notional(perUnit, units);
        rep.lockedProfit = rep.guaranteedPayout - cost - fees;
    } else {
        Micros left = 0;
        for (const auto& s : st) left += s.heldCost;
        rep.strandedCost = left;
        if (left > 0) status = ExecStatus::Stranded;
    }
    if (uncertain && status != ExecStatus::Stranded) {
        rep.note += " [ambiguous order state: verify positions manually]";
        status = ExecStatus::Stranded;
    }
    rep.status = status;
    return rep;
}

ExecutionReport BasketExecutor::execute(const Opportunity& opp, Qty maxUnits) {
    ExecutionReport rep;
    for (const auto& l : opp.legs) {
        if (!gw_.canTrade(l.venue)) {
            rep.status = ExecStatus::Skipped;
            rep.note = std::string(toString(l.venue)) + " legs are not routable via " + gw_.name();
            return rep;
        }
    }

    // ---- 1. re-validate on fresh books --------------------------------------
    std::vector<LegState> st(opp.legs.size());
    std::vector<LegSpec> specs;
    auto fresh = gw_.refreshAll(opp.legs);
    for (std::size_t i = 0; i < opp.legs.size(); ++i) {
        st[i].leg = opp.legs[i];
        if (i >= fresh.size() || !fresh[i]) {
            rep.status = ExecStatus::NotFilled;
            rep.note = "could not refresh book for " + opp.legs[i].marketId;
            return rep;
        }
        st[i].book = std::move(*fresh[i]);
    }
    for (auto& s : st) {
        specs.push_back({&s.book, s.leg.bookKey, s.leg.venue, s.leg.marketId, s.leg.outcome,
                         s.leg.feeModel});
    }
    const Micros minProfit = usd(s_.minProfitUsd);
    const Micros minEdge = cents(s_.minEdgeCents);
    const Micros safety = cents(s_.safetyMarginCents);
    SizingResult r = sizeBasket(specs, opp.payoutPerUnit, std::min(opp.units, maxUnits), minProfit,
                                minEdge, s_.minRoi, safety);
    if (!r.ok) {
        rep.status = ExecStatus::NotFilled;
        rep.note = "edge vanished on re-check";
        return rep;
    }
    const Qty U = r.units;
    for (std::size_t i = 0; i < st.size(); ++i) st[i].leg = r.legs[i];

    // thinnest leg first: least depth at the limit relative to what we need
    std::vector<std::size_t> order(st.size());
    std::iota(order.begin(), order.end(), 0);
    auto depthRatio = [&](std::size_t i) {
        const Qty d = st[i].book.askDepthAtOrBetter(st[i].leg.outcome, st[i].leg.limitPrice);
        return static_cast<double>(d) / static_cast<double>(std::max<Qty>(U, 1));
    };
    std::sort(order.begin(), order.end(),
              [&](std::size_t a, std::size_t b) { return depthRatio(a) < depthRatio(b); });

    spdlog::info("EXEC {} {} units={} expected profit={} via {}", toString(opp.kind), opp.groupId,
                 fp::formatCount(U), fp::usd(r.profit), gw_.name());

    // ---- 2. fire legs ------------------------------------------------------
    if (e_.legMode == "batch") {
        std::vector<OrderRequest> reqs;
        for (std::size_t i : order) {
            OrderRequest q;
            q.venue = st[i].leg.venue;
            q.marketId = st[i].leg.marketId;
            q.clientOrderId = newClientOrderId();
            q.outcome = st[i].leg.outcome;
            q.buy = true;
            q.limitPrice = st[i].leg.limitPrice;
            q.qty = U;
            q.timeInForce = "fill_or_kill";
            q.fee = st[i].leg.feeModel;
            reqs.push_back(std::move(q));
        }
        const auto results = gw_.submitBatch(reqs);
        for (std::size_t k = 0; k < order.size() && k < results.size(); ++k)
            applyBuy(st[order[k]], results[k], rep);
    } else {
        for (std::size_t i : order) {
            if (!buyLeg(st[i], U, st[i].leg.limitPrice, rep)) break;
        }
    }

    // ---- 3. resolve leg failures ---------------------------------------------
    int retries = 0;
    for (;;) {
        std::vector<std::size_t> incomplete;
        for (std::size_t i = 0; i < st.size(); ++i)
            if (st[i].held < U) incomplete.push_back(i);
        if (incomplete.empty()) return finalize(opp, st, U, rep, ExecStatus::Completed);

        const bool anyHeld =
            std::any_of(st.begin(), st.end(), [](const LegState& s) { return s.held > 0; });
        refreshBooks(st);

        Micros sunk = 0;
        for (const auto& s : st) sunk += s.heldCost + s.buyFees;

        // A: complete the basket at fresh prices
        bool feasibleA = retries < e_.maxCompletionRetries;
        Micros completion = 0;
        if (feasibleA) {
            for (std::size_t i : incomplete) {
                auto q = quoteBuy(st[i].book, st[i].leg.outcome, U - st[i].held, st[i].leg.feeModel);
                if (!q) {
                    feasibleA = false;
                    break;
                }
                completion += q->cost + q->fee + notional(safety, U - st[i].held);
            }
        }
        const Micros pnlA = notional(opp.payoutPerUnit, U) - sunk - completion;

        // C: unwind everything
        Micros unwindValue = 0;
        bool unwindAll = true;
        for (const auto& s : st) {
            if (s.held <= 0) continue;
            const SellQuote q = quoteSell(s.book, s.leg.outcome, s.held, s.leg.feeModel);
            unwindValue += q.proceeds - q.fee;
            if (q.filled < s.held) unwindAll = false;
        }
        const Micros pnlC = unwindValue - sunk;

        // B: shrink a NO basket to its fully-filled subset
        bool feasibleB = false;
        Micros pnlB = INT64_MIN;
        if (opp.kind == ArbKind::NoBasket) {
            int full = 0;
            Micros keepCost = 0, partialValue = 0, partialSunk = 0;
            for (const auto& s : st) {
                if (s.held >= U) {
                    ++full;
                    keepCost += s.heldCost + s.buyFees;
                } else if (s.held > 0) {
                    const SellQuote q = quoteSell(s.book, s.leg.outcome, s.held, s.leg.feeModel);
                    partialValue += q.proceeds - q.fee;
                    partialSunk += s.heldCost + s.buyFees;
                }
            }
            if (full >= 2) {
                feasibleB = true;
                pnlB = notional(static_cast<Micros>(full - 1) * kOneDollar, U) - keepCost +
                       partialValue - partialSunk;
            }
        }

        if (!anyHeld) {
            // Nothing bought yet: only re-try if the full edge is still there.
            if (feasibleA && pnlA >= minProfit) {
                ++retries;
                spdlog::info("  retry {} : nothing filled, edge still {}", retries, fp::usd(pnlA));
                for (std::size_t i : order) {
                    auto q = quoteBuy(st[i].book, st[i].leg.outcome, U, st[i].leg.feeModel);
                    if (!q || !buyLeg(st[i], U, q->worst, rep)) break;
                }
                continue;
            }
            rep.note = "first leg not filled; edge gone";
            return finalize(opp, st, U, rep, ExecStatus::NotFilled);
        }

        spdlog::warn("  leg failure, options: complete {} | shrink {} | unwind {}{}",
                     feasibleA ? fp::usd(pnlA) : "n/a", feasibleB ? fp::usd(pnlB) : "n/a",
                     fp::usd(pnlC), unwindAll ? "" : " (partial: thin bids)");

        if (feasibleA && pnlA >= pnlC && (!feasibleB || pnlA >= pnlB)) {
            ++retries;
            bool allDone = true;
            for (std::size_t i : order) {
                if (st[i].held >= U) continue;
                auto q = quoteBuy(st[i].book, st[i].leg.outcome, U - st[i].held, st[i].leg.feeModel);
                if (!q || !buyLeg(st[i], U - st[i].held, q->worst, rep)) {
                    allDone = false;
                    break;
                }
            }
            if (allDone) {
                rep.note = "completed after leg retry";
                return finalize(opp, st, U, rep, ExecStatus::Completed);
            }
            continue;
        }

        if (feasibleB && pnlB >= pnlC) {
            bool clean = true;
            for (auto& s : st)
                if (s.held > 0 && s.held < U) clean &= unwindLeg(s, rep);
            rep.note = "shrunk to fully-filled NO legs";
            if (!clean) return finalize(opp, st, U, rep, ExecStatus::Stranded);
            return finalize(opp, st, U, rep, ExecStatus::Shrunk);
        }

        if (!e_.unwindOnFailure) {
            rep.note = "leg failed; unwind disabled";
            return finalize(opp, st, U, rep, ExecStatus::Stranded);
        }
        bool clean = true;
        for (auto& s : st) clean &= unwindLeg(s, rep);
        rep.note = clean ? "leg failed; unwound" : "leg failed; partial unwind";
        return finalize(opp, st, U, rep, clean ? ExecStatus::Unwound : ExecStatus::Stranded);
    }
}

}  // namespace artemis
