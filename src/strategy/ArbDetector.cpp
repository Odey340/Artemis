#include "strategy/ArbDetector.hpp"

#include <algorithm>
#include <cmath>

#include "core/FixedPoint.hpp"

namespace artemis {

namespace {

struct Cursor {
    const Ladder* ladder = nullptr;
    std::size_t idx = 0;
    Qty used = 0;          // consumed from ladder[idx]
    Ladder fills;          // aggregated by price level
    Micros cost = 0;
    Micros fee = 0;
};

// Take `q` centi-contracts from the cursor. Returns the cost of this slice, or
// -1 (leaving the cursor untouched) if depth is insufficient.
Micros take(Cursor& c, Qty q, const FeeModel& fm) {
    // depth check first so a failed step leaves no partial state
    Qty avail = 0;
    for (std::size_t i = c.idx; i < c.ladder->size() && avail < q; ++i)
        avail += (*c.ladder)[i].qty - (i == c.idx ? c.used : 0);
    if (avail < q) return -1;

    Micros slice = 0;
    while (q > 0) {
        const Level& lvl = (*c.ladder)[c.idx];
        const Qty t = std::min(q, lvl.qty - c.used);
        if (c.fills.empty() || c.fills.back().price != lvl.price) c.fills.push_back({lvl.price, 0});
        Level& f = c.fills.back();
        const Micros oldFee = fm.takerFee(f.price, f.qty);
        f.qty += t;
        c.fee += fm.takerFee(f.price, f.qty) - oldFee;
        const Micros n = notional(lvl.price, t);
        c.cost += n;
        slice += n;
        c.used += t;
        q -= t;
        if (c.used == lvl.qty) {
            ++c.idx;
            c.used = 0;
        }
    }
    return slice;
}

}  // namespace

std::optional<LegQuote> quoteBuy(const BinaryBook& book, Outcome o, Qty qty, const FeeModel& fm) {
    LegQuote q;
    bool first = true;
    const WalkResult w = book.walkAsks(o, qty, [&](Micros price, Qty t) {
        q.fee += fm.takerFee(price, t);
        if (first) {
            q.best = price;
            first = false;
        }
    });
    if (w.filled < qty) return std::nullopt;
    q.cost = w.cost;
    q.worst = w.worstPrice;
    return q;
}

SizingResult sizeBasket(const std::vector<LegSpec>& legs, Micros payoutPerUnit, Qty maxUnits,
                        Micros minProfit, Micros minEdgePerUnit, double minRoi,
                        Micros safetyPerLegPerUnit) {
    SizingResult best;
    if (legs.empty() || maxUnits < kOneContract) return best;

    std::vector<Cursor> cur(legs.size());
    for (std::size_t i = 0; i < legs.size(); ++i) {
        if (!legs[i].book) return best;
        cur[i].ladder = &legs[i].book->asks(legs[i].outcome);
    }

    std::vector<Cursor> bestCur;
    Qty units = 0;
    const Micros safety = safetyPerLegPerUnit * static_cast<Micros>(legs.size());

    while (units + kOneContract <= maxUnits) {
        std::vector<Cursor> next = cur;
        Micros marginal = 0;
        bool ok = true;
        for (std::size_t i = 0; i < legs.size(); ++i) {
            const Micros slice = take(next[i], kOneContract, legs[i].fee);
            if (slice < 0) {
                ok = false;
                break;
            }
            marginal += slice;
        }
        if (!ok) break;                                     // out of depth on some leg
        if (marginal + safety >= payoutPerUnit) break;      // next unit cannot add value

        cur.swap(next);
        units += kOneContract;
        Micros cost = 0, fees = 0;
        for (const auto& c : cur) {
            cost += c.cost;
            fees += c.fee;
        }
        const Micros payout = notional(payoutPerUnit, units);
        const Micros profit = payout - cost - fees - notional(safety, units);
        const Micros perUnit = profit * kOneContract / units;
        const double roi = (cost + fees) > 0 ? static_cast<double>(profit) / (cost + fees) : 0.0;
        const bool passes =
            profit > 0 && profit >= minProfit && perUnit >= minEdgePerUnit && roi >= minRoi;
        if (passes && (!best.ok || profit > best.profit)) {
            best.ok = true;
            best.units = units;
            best.cost = cost;
            best.fees = fees;
            best.profit = profit;
            bestCur = cur;
        }
    }

    if (!best.ok) return best;
    for (std::size_t i = 0; i < legs.size(); ++i) {
        const Cursor& c = bestCur[i];
        Leg l;
        l.venue = legs[i].venue;
        l.marketId = legs[i].marketId;
        l.bookKey = legs[i].bookKey;
        l.outcome = legs[i].outcome;
        l.qty = best.units;
        l.cost = c.cost;
        l.fee = c.fee;
        l.bestPrice = c.fills.empty() ? 0 : c.fills.front().price;
        l.limitPrice = c.fills.empty() ? 0 : c.fills.back().price;
        l.topDepth = c.ladder->empty() ? 0 : c.ladder->front().qty;
        l.feeModel = legs[i].fee;
        best.legs.push_back(std::move(l));
    }
    return best;
}

ArbDetector::ArbDetector(const StrategyConfig& s, const FeedConfig& f, const PolymarketConfig& p,
                         const BasisConfig& b)
    : s_(s), f_(f), p_(p), b_(b) {}

FeeModel ArbDetector::kalshiFee(const EventInfo& ev) const {
    return FeeModel::kalshi(s_.kalshiTakerRate, ev.feeMultiplier);
}

FeeModel ArbDetector::kalshiFeeForTicker(const std::string& ticker) const {
    auto it = tickerFees_.find(ticker);
    return it != tickerFees_.end() ? it->second : FeeModel::kalshi(s_.kalshiTakerRate, 1.0);
}

FeeModel ArbDetector::polyFee(const MarketPair& pair) const {
    if (p_.feeModel == "quadratic") return FeeModel::polymarketQuadratic(p_.quadraticTheta);
    return FeeModel::polymarketBps(pair.feeBps.value_or(p_.feeBps));
}

bool ArbDetector::fresh(const BinaryBook& b, std::int64_t nowUs) const {
    return nowUs - b.tsUs() <= static_cast<std::int64_t>(f_.maxBookAgeMs) * 1000;
}

Opportunity ArbDetector::finish(ArbKind kind, const std::string& group, const std::string& label,
                                SizingResult&& r, Micros payoutPerUnit,
                                std::int64_t nowUs) const {
    Opportunity o;
    o.kind = kind;
    o.groupId = group;
    o.label = label;
    o.key = std::string(toString(kind)) + ":" + group;
    o.units = r.units;
    o.payoutPerUnit = payoutPerUnit;
    o.payout = notional(payoutPerUnit, r.units);
    o.cost = r.cost;
    o.fees = r.fees;
    o.profit = r.profit;
    o.edgePerUnit = r.units ? r.profit * kOneContract / r.units : 0;
    o.roi = (r.cost + r.fees) > 0 ? static_cast<double>(r.profit) / (r.cost + r.fees) : 0.0;
    o.legs = std::move(r.legs);
    o.detectedTsUs = nowUs;
    o.oldestBookTsUs = nowUs;
    return o;
}

std::vector<Opportunity> ArbDetector::evaluateEvent(const EventInfo& ev, const BookStore& books,
                                                    std::int64_t nowUs) const {
    std::vector<Opportunity> out;
    if (!ev.mutuallyExclusive) return out;
    if (ev.feeType == "flat") return out;  // Specific Trading Fees Table: not modelled

    const FeeModel fee = kalshiFee(ev);
    const Micros safety = static_cast<Micros>(std::llround(s_.safetyMarginCents * kOneCent));
    const Micros minProfit = static_cast<Micros>(std::llround(s_.minProfitUsd * kOneDollar));
    const Micros minEdge = static_cast<Micros>(std::llround(s_.minEdgeCents * kOneCent));
    const Qty maxUnits = static_cast<Qty>(s_.maxContracts) * kOneContract;

    // ---- NO basket ------------------------------------------------------
    if (s_.enableNoBasket) {
        std::vector<LegSpec> legs;
        std::int64_t oldest = nowUs;
        for (const auto& m : ev.markets) {
            if (!m.tradable()) continue;
            const std::string key = BookStore::kalshiKey(m.ticker);
            const BinaryBook* b = books.get(key);
            if (!b || !fresh(*b, nowUs) || !b->hasAsk(Outcome::No)) continue;
            const Micros ask = b->bestAsk(Outcome::No);
            // per-contract fee at size (rounding is accounted for by sizeBasket)
            const Micros feePer = fee.takerFee(ask, 100 * kOneContract) / 100;
            const Micros value = kOneDollar - ask - feePer - safety;
            // A leg must add value, and enough of it per dollar tied up: NO at
            // 99c adds <1c for 99c of capital locked until settlement.
            if (value <= 0 || static_cast<double>(value) < s_.minLegRoi * static_cast<double>(ask))
                continue;
            legs.push_back({b, key, Venue::Kalshi, m.ticker, Outcome::No, fee});
            oldest = std::min(oldest, b->tsUs());
        }
        if (legs.size() >= 2) {
            const Micros payout = static_cast<Micros>(legs.size() - 1) * kOneDollar;
            SizingResult r = sizeBasket(legs, payout, maxUnits, minProfit, minEdge, s_.minRoi, safety);
            if (r.ok) {
                Opportunity o = finish(ArbKind::NoBasket, ev.eventTicker, ev.title, std::move(r),
                                       payout, nowUs);
                o.oldestBookTsUs = oldest;
                out.push_back(std::move(o));
            }
        }
    }

    // ---- YES basket (exhaustive events only) ------------------------------
    if (s_.enableYesBasket && ev.exhaustive) {
        std::vector<LegSpec> legs;
        bool complete = !ev.markets.empty();
        std::int64_t oldest = nowUs;
        for (const auto& m : ev.markets) {
            if (m.status == "settled" || m.status == "finalized") {
                complete = false;  // resolution in progress: payout no longer certain to be 1
                break;
            }
            const std::string key = BookStore::kalshiKey(m.ticker);
            const BinaryBook* b = books.get(key);
            if (!m.tradable() || !b || !fresh(*b, nowUs) || !b->hasAsk(Outcome::Yes)) {
                complete = false;
                break;
            }
            legs.push_back({b, key, Venue::Kalshi, m.ticker, Outcome::Yes, fee});
            oldest = std::min(oldest, b->tsUs());
        }
        if (complete && legs.size() >= 2) {
            SizingResult r =
                sizeBasket(legs, kOneDollar, maxUnits, minProfit, minEdge, s_.minRoi, safety);
            if (r.ok) {
                Opportunity o = finish(ArbKind::YesBasket, ev.eventTicker, ev.title, std::move(r),
                                       kOneDollar, nowUs);
                o.oldestBookTsUs = oldest;
                out.push_back(std::move(o));
            }
        }
    }
    return out;
}

std::vector<Opportunity> ArbDetector::evaluatePair(const MarketPair& pair, const BookStore& books,
                                                   std::int64_t nowUs) const {
    std::vector<Opportunity> out;
    if (!s_.enableCrossVenue) return out;
    const std::string kKey = BookStore::kalshiKey(pair.kalshiTicker);
    const std::string pKey = BookStore::polyKey(pair.id);
    const BinaryBook* kb = books.get(kKey);
    const BinaryBook* pb = books.get(pKey);
    if (!kb || !pb || !fresh(*kb, nowUs) || !fresh(*pb, nowUs)) return out;

    const FeeModel kf = kalshiFeeForTicker(pair.kalshiTicker);
    const FeeModel pf = polyFee(pair);
    const Micros safety = static_cast<Micros>(std::llround(s_.safetyMarginCents * kOneCent));
    const Micros minProfit = static_cast<Micros>(std::llround(s_.minProfitUsd * kOneDollar));
    const Micros minEdge = static_cast<Micros>(std::llround(s_.minEdgeCents * kOneCent));
    const Qty maxUnits = static_cast<Qty>(s_.maxContracts) * kOneContract;

    for (Outcome kOutcome : {Outcome::Yes, Outcome::No}) {
        const Outcome pOutcome = opposite(kOutcome);
        if (!kb->hasAsk(kOutcome) || !pb->hasAsk(pOutcome)) continue;
        if (kb->bestAsk(kOutcome) + pb->bestAsk(pOutcome) >= kOneDollar) continue;  // fast reject
        std::vector<LegSpec> legs = {
            {kb, kKey, Venue::Kalshi, pair.kalshiTicker, kOutcome, kf},
            {pb, pKey, Venue::Polymarket, pair.id, pOutcome, pf},
        };
        SizingResult r = sizeBasket(legs, kOneDollar, maxUnits, minProfit, minEdge, s_.minRoi, safety);
        if (!r.ok) continue;
        Opportunity o = finish(ArbKind::CrossVenue, pair.id, pair.label, std::move(r), kOneDollar,
                               nowUs);
        o.key += kOutcome == Outcome::Yes ? ":kYES" : ":kNO";
        o.oldestBookTsUs = std::min(kb->tsUs(), pb->tsUs());
        out.push_back(std::move(o));
    }
    return out;
}

std::optional<Opportunity> ArbDetector::resize(const Opportunity& o, const BookStore& books,
                                               Qty maxUnits) const {
    std::vector<LegSpec> legs;
    for (const auto& l : o.legs) {
        const BinaryBook* b = books.get(l.bookKey);
        if (!b) return std::nullopt;
        legs.push_back({b, l.bookKey, l.venue, l.marketId, l.outcome, l.feeModel});
    }
    const Micros safety = static_cast<Micros>(std::llround(s_.safetyMarginCents * kOneCent));
    const Micros minProfit = static_cast<Micros>(std::llround(s_.minProfitUsd * kOneDollar));
    const Micros minEdge = static_cast<Micros>(std::llround(s_.minEdgeCents * kOneCent));
    SizingResult r = sizeBasket(legs, o.payoutPerUnit, maxUnits, minProfit, minEdge, s_.minRoi, safety);
    if (!r.ok) return std::nullopt;
    Opportunity out = finish(o.kind, o.groupId, o.label, std::move(r), o.payoutPerUnit, o.detectedTsUs);
    out.key = o.key;
    out.confirmations = o.confirmations;
    out.oldestBookTsUs = o.oldestBookTsUs;
    return out;
}

void ArbDetector::updateBasis(const MarketPair& pair, const BookStore& books, std::int64_t nowUs) {
    if (!b_.enabled) return;
    const BinaryBook* kb = books.get(BookStore::kalshiKey(pair.kalshiTicker));
    const BinaryBook* pb = books.get(BookStore::polyKey(pair.id));
    if (!kb || !pb || !fresh(*kb, nowUs) || !fresh(*pb, nowUs)) return;
    auto it = basis_.find(pair.id);
    if (it == basis_.end()) {
        it = basis_.emplace(pair.id, BasisMonitor(b_.halfLife, b_.warmup, b_.entryZ, b_.exitZ)).first;
    }
    const double basisCents = static_cast<double>(kb->yesMid() - pb->yesMid()) / kOneCent;
    it->second.update(basisCents);
}

std::vector<Opportunity> ArbDetector::scan(const std::vector<EventInfo>& events,
                                           const std::vector<MarketPair>& pairs,
                                           const BookStore& books, std::int64_t nowUs,
                                           const std::unordered_set<std::string>* dirty) {
    std::vector<Opportunity> found;
    std::unordered_set<std::string> evaluatedGroups;

    for (const auto& ev : events) {
        if (dirty && !dirty->count(ev.eventTicker)) continue;
        evaluatedGroups.insert(ev.eventTicker);
        for (auto& o : evaluateEvent(ev, books, nowUs)) found.push_back(std::move(o));
    }
    for (const auto& p : pairs) {
        if (dirty && !dirty->count(p.id)) continue;
        evaluatedGroups.insert(p.id);
        updateBasis(p, books, nowUs);
        for (auto& o : evaluatePair(p, books, nowUs)) found.push_back(std::move(o));
    }

    // confirmation streaks: reset keys of evaluated groups that did not fire
    std::unordered_set<std::string> fired;
    for (const auto& o : found) fired.insert(o.key);
    for (auto it = streak_.begin(); it != streak_.end();) {
        if (evaluatedGroups.count(it->second.first) && !fired.count(it->first)) {
            it = streak_.erase(it);
        } else {
            ++it;
        }
    }

    std::vector<Opportunity> out;
    for (auto& o : found) {
        auto& st = streak_[o.key];
        st.first = o.groupId;
        const int n = ++st.second;
        o.confirmations = n;
        if (n >= s_.confirmations) out.push_back(std::move(o));
    }
    std::sort(out.begin(), out.end(),
              [](const Opportunity& a, const Opportunity& b) { return a.profit > b.profit; });
    return out;
}

}  // namespace artemis
