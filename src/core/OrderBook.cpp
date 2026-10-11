#include "core/OrderBook.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace artemis {

namespace {

Ladder cleanLadder(Ladder l, bool descending) {
    l.erase(std::remove_if(l.begin(), l.end(),
                           [](const Level& x) {
                               return x.qty <= 0 || x.price <= 0 || x.price >= kOneDollar;
                           }),
            l.end());
    std::sort(l.begin(), l.end(), [descending](const Level& a, const Level& b) {
        return descending ? a.price > b.price : a.price < b.price;
    });
    Ladder out;
    out.reserve(l.size());
    for (const auto& x : l) {
        if (!out.empty() && out.back().price == x.price) {
            out.back().qty += x.qty;
        } else {
            out.push_back(x);
        }
    }
    return out;
}

Ladder reflect(const Ladder& bids) {
    Ladder asks;
    asks.reserve(bids.size());
    for (const auto& b : bids) asks.push_back({kOneDollar - b.price, b.qty});
    return asks;
}

WalkResult walk(const Ladder& l, Qty qty, const std::function<void(Micros, Qty)>& fn) {
    WalkResult r;
    for (const auto& lvl : l) {
        if (r.filled >= qty) break;
        const Qty take = std::min(lvl.qty, qty - r.filled);
        if (take <= 0) continue;
        if (r.levels == 0) r.bestPrice = lvl.price;
        r.filled += take;
        r.cost += notional(lvl.price, take);
        r.worstPrice = lvl.price;
        ++r.levels;
        if (fn) fn(lvl.price, take);
    }
    return r;
}

}  // namespace

BinaryBook BinaryBook::fromKalshi(const std::string& ticker, Ladder yesBids, Ladder noBids,
                                  std::int64_t tsUs) {
    BinaryBook b(Venue::Kalshi, ticker);
    b.tsUs_ = tsUs;
    b.mirrored_ = true;
    b.yesBids_ = std::move(yesBids);
    b.noBids_ = std::move(noBids);
    b.normalise();  // sorts bids and re-derives asks
    return b;
}

BinaryBook BinaryBook::fromTwoTokens(Venue venue, const std::string& id, Ladder yesBids,
                                     Ladder yesAsks, Ladder noBids, Ladder noAsks,
                                     std::int64_t tsUs) {
    BinaryBook b(venue, id);
    b.tsUs_ = tsUs;
    b.mirrored_ = false;
    b.yesBids_ = std::move(yesBids);
    b.yesAsks_ = std::move(yesAsks);
    b.noBids_ = std::move(noBids);
    b.noAsks_ = std::move(noAsks);
    b.normalise();
    return b;
}

void BinaryBook::normalise() {
    yesBids_ = cleanLadder(std::move(yesBids_), true);
    noBids_ = cleanLadder(std::move(noBids_), true);
    if (mirrored_) {
        yesAsks_ = reflect(noBids_);   // already ascending because noBids descending
        noAsks_ = reflect(yesBids_);
    } else {
        yesAsks_ = cleanLadder(std::move(yesAsks_), false);
        noAsks_ = cleanLadder(std::move(noAsks_), false);
    }
}

Micros BinaryBook::yesMid() const {
    const bool hb = hasBid(Outcome::Yes);
    const bool ha = hasAsk(Outcome::Yes);
    if (hb && ha) return (bestBid(Outcome::Yes) + bestAsk(Outcome::Yes)) / 2;
    if (hb) return bestBid(Outcome::Yes);
    if (ha) return bestAsk(Outcome::Yes);
    return kOneDollar / 2;
}

WalkResult BinaryBook::walkAsks(Outcome o, Qty qty,
                                const std::function<void(Micros, Qty)>& fn) const {
    return walk(asks(o), qty, fn);
}

WalkResult BinaryBook::walkBids(Outcome o, Qty qty,
                                const std::function<void(Micros, Qty)>& fn) const {
    return walk(bids(o), qty, fn);
}

Qty BinaryBook::askDepthAtOrBetter(Outcome o, Micros limit) const {
    Qty q = 0;
    for (const auto& l : asks(o)) {
        if (l.price > limit) break;
        q += l.qty;
    }
    return q;
}

Qty BinaryBook::bidDepthAtOrBetter(Outcome o, Micros limit) const {
    Qty q = 0;
    for (const auto& l : bids(o)) {
        if (l.price < limit) break;
        q += l.qty;
    }
    return q;
}

void BinaryBook::consume(Ladder& l, Qty qty) {
    auto it = l.begin();
    while (qty > 0 && it != l.end()) {
        const Qty take = std::min(qty, it->qty);
        it->qty -= take;
        qty -= take;
        if (it->qty == 0) {
            it = l.erase(it);
        } else {
            ++it;
        }
    }
}

void BinaryBook::consumeAsks(Outcome o, Qty qty) {
    if (mirrored_) {
        // a YES ask is a resting NO bid (and vice versa)
        consume(o == Outcome::Yes ? noBids_ : yesBids_, qty);
        normalise();
    } else {
        consume(o == Outcome::Yes ? yesAsks_ : noAsks_, qty);
    }
}

void BinaryBook::consumeBids(Outcome o, Qty qty) {
    consume(o == Outcome::Yes ? yesBids_ : noBids_, qty);
    if (mirrored_) normalise();
}

Qty BinaryBook::takeAt(bool askSide, Outcome o, Micros price, Qty qty) {
    Ladder* l = nullptr;
    Micros px = price;
    if (mirrored_ && askSide) {
        // a YES ask at p is a resting NO bid at (1 - p)
        l = (o == Outcome::Yes) ? &noBids_ : &yesBids_;
        px = kOneDollar - price;
    } else if (askSide) {
        l = (o == Outcome::Yes) ? &yesAsks_ : &noAsks_;
    } else {
        l = (o == Outcome::Yes) ? &yesBids_ : &noBids_;
    }
    for (auto it = l->begin(); it != l->end(); ++it) {
        if (it->price != px) continue;
        const Qty t = std::min(qty, it->qty);
        it->qty -= t;
        if (it->qty == 0) l->erase(it);
        if (mirrored_) normalise();
        return t;
    }
    return 0;
}

std::string encodeLadder(const Ladder& l) {
    std::string s;
    s.reserve(l.size() * 14);
    for (std::size_t i = 0; i < l.size(); ++i) {
        if (i) s += '|';
        s += std::to_string(l[i].price);
        s += ':';
        s += std::to_string(l[i].qty);
    }
    return s;
}

Ladder decodeLadder(const std::string& s) {
    Ladder l;
    std::size_t pos = 0;
    while (pos < s.size()) {
        std::size_t end = s.find('|', pos);
        if (end == std::string::npos) end = s.size();
        const std::size_t colon = s.find(':', pos);
        if (colon != std::string::npos && colon < end) {
            Level lvl;
            lvl.price = std::strtoll(s.c_str() + pos, nullptr, 10);
            lvl.qty = std::strtoll(s.c_str() + colon + 1, nullptr, 10);
            l.push_back(lvl);
        }
        pos = end + 1;
    }
    return l;
}

}  // namespace artemis
