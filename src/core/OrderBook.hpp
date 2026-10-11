#pragma once
// Normalised binary-outcome order book.
//
// Every venue is mapped onto the same four ladders so that the strategy layer
// never needs to know where a quote came from:
//
//            bids (best = highest first)   asks (best = lowest first)
//   YES      yesBids                       yesAsks
//   NO       noBids                        noAsks
//
// Kalshi publishes only bids for each side because a YES bid at p *is* a NO
// ask at (1 - p). `fromKalshi` performs that reflection and marks the book as
// `mirrored`, so consuming liquidity from an ask also removes the matching bid.

#include <functional>
#include <string>

#include "core/Types.hpp"

namespace artemis {

struct WalkResult {
    Qty    filled = 0;       // centi-contracts actually available up to `qty`
    Micros cost = 0;         // total cost of those contracts
    Micros worstPrice = 0;   // price of the last (worst) level touched
    Micros bestPrice = 0;    // price of the first level touched
    int    levels = 0;
};

class BinaryBook {
public:
    BinaryBook() = default;
    BinaryBook(Venue venue, std::string id) : venue_(venue), id_(std::move(id)) {}

    // Kalshi: yes_dollars / no_dollars are the bid ladders for each outcome.
    static BinaryBook fromKalshi(const std::string& ticker, Ladder yesBids, Ladder noBids,
                                 std::int64_t tsUs);
    // Polymarket: YES token and NO token books (bids & asks each).
    static BinaryBook fromTwoTokens(Venue venue, const std::string& id, Ladder yesBids,
                                    Ladder yesAsks, Ladder noBids, Ladder noAsks,
                                    std::int64_t tsUs);

    const Ladder& bids(Outcome o) const { return o == Outcome::Yes ? yesBids_ : noBids_; }
    const Ladder& asks(Outcome o) const { return o == Outcome::Yes ? yesAsks_ : noAsks_; }

    bool hasAsk(Outcome o) const { return !asks(o).empty(); }
    bool hasBid(Outcome o) const { return !bids(o).empty(); }
    Micros bestAsk(Outcome o) const { return asks(o).empty() ? kOneDollar : asks(o).front().price; }
    Micros bestBid(Outcome o) const { return bids(o).empty() ? 0 : bids(o).front().price; }
    // Mid of YES in micros; falls back to whichever side exists.
    Micros yesMid() const;

    // Walk the ask ladder buying up to `qty`; `fn(price, qty)` receives each fill.
    WalkResult walkAsks(Outcome o, Qty qty,
                        const std::function<void(Micros, Qty)>& fn = nullptr) const;
    // Walk the bid ladder selling up to `qty`.
    WalkResult walkBids(Outcome o, Qty qty,
                        const std::function<void(Micros, Qty)>& fn = nullptr) const;
    // Total contracts on the ask side priced at or better than `limit`.
    Qty askDepthAtOrBetter(Outcome o, Micros limit) const;
    Qty bidDepthAtOrBetter(Outcome o, Micros limit) const;

    // Remove liquidity after a (simulated) fill. Keeps mirrored ladders in sync.
    void consumeAsks(Outcome o, Qty qty);
    void consumeBids(Outcome o, Qty qty);
    // Remove up to `qty` from the single level at `price` (ask or bid side).
    // Returns how much was removed (0 if that level no longer exists).
    Qty takeAt(bool askSide, Outcome o, Micros price, Qty qty);

    Venue venue() const { return venue_; }
    const std::string& id() const { return id_; }
    std::int64_t tsUs() const { return tsUs_; }
    void setTsUs(std::int64_t t) { tsUs_ = t; }
    bool mirrored() const { return mirrored_; }
    bool empty() const {
        return yesBids_.empty() && yesAsks_.empty() && noBids_.empty() && noAsks_.empty();
    }

    // Sort/clean all ladders: drop non-positive quantities and out-of-range prices,
    // merge duplicate price levels, order best-first.
    void normalise();

private:
    static void consume(Ladder& l, Qty qty);

    Venue venue_ = Venue::Kalshi;
    std::string id_;
    std::int64_t tsUs_ = 0;
    bool mirrored_ = false;
    Ladder yesBids_, yesAsks_, noBids_, noAsks_;
};

// Serialise a ladder as "price:qty|price:qty" using raw integer units.
std::string encodeLadder(const Ladder& l);
Ladder decodeLadder(const std::string& s);

}  // namespace artemis
