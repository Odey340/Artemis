#include "exec/OrderGateway.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdio>
#include <random>
#include <thread>
#include <unordered_map>

#include "core/Clock.hpp"
#include "core/FixedPoint.hpp"
#include "venues/kalshi/KalshiClient.hpp"

namespace artemis {

std::string newClientOrderId() {
    static thread_local std::mt19937_64 rng{std::random_device{}() ^
                                            static_cast<std::uint64_t>(nowUs())};
    const std::uint64_t a = rng(), b = rng();
    char buf[40];
    // RFC 4122 v4 layout
    std::snprintf(buf, sizeof(buf), "%08x-%04x-4%03x-%04x-%012llx",
                  static_cast<unsigned>(a >> 32), static_cast<unsigned>((a >> 16) & 0xffff),
                  static_cast<unsigned>(a & 0x0fff),
                  static_cast<unsigned>(((b >> 48) & 0x3fff) | 0x8000),
                  static_cast<unsigned long long>(b & 0xffffffffffffULL));
    return buf;
}

// ---------------------------------------------------------------- SimGateway

OrderResult SimGateway::submit(const OrderRequest& req) {
    if (beforeSubmit_) beforeSubmit_();
    OrderResult r;
    r.clientOrderId = req.clientOrderId;
    r.orderId = "sim-" + std::to_string(++seq_);
    if (failNext_ > 0) {
        --failNext_;
        r.accepted = true;
        r.remaining = req.qty;
        r.error = "simulated kill";
        return r;
    }
    const std::string key = req.venue == Venue::Kalshi ? BookStore::kalshiKey(req.marketId)
                                                       : BookStore::polyKey(req.marketId);
    BinaryBook* book = books_.mut(key);
    if (!book) {
        r.error = "no book for " + key;
        return r;
    }
    r.accepted = true;

    const bool fok = req.timeInForce == "fill_or_kill";
    const Qty avail = req.buy ? book->askDepthAtOrBetter(req.outcome, req.limitPrice)
                              : book->bidDepthAtOrBetter(req.outcome, req.limitPrice);
    Qty fillQty = std::min(avail, req.qty);
    if (fok && fillQty < req.qty) fillQty = 0;
    if (fillQty <= 0) {
        r.remaining = req.qty;
        return r;
    }

    Micros fees = 0;
    std::vector<Level> taken;
    auto onFill = [&](Micros price, Qty q) {
        fees += req.fee.takerFee(price, q);
        taken.push_back({price, q});
    };
    const WalkResult w = req.buy ? book->walkAsks(req.outcome, fillQty, onFill)
                                 : book->walkBids(req.outcome, fillQty, onFill);
    if (req.buy) {
        book->consumeAsks(req.outcome, w.filled);
    } else {
        book->consumeBids(req.outcome, w.filled);
    }
    for (const auto& t : taken)
        books_.recordTake(key, req.buy, req.outcome, t.price, t.qty, book->tsUs());
    r.filled = w.filled;
    r.remaining = req.qty - w.filled;
    r.avgPrice = w.filled ? w.cost * kOneContract / w.filled : 0;
    r.fees = fees;
    return r;
}

std::optional<BinaryBook> SimGateway::refresh(const Leg& leg) {
    if (const BinaryBook* b = books_.get(leg.bookKey)) return *b;
    return std::nullopt;
}

// ------------------------------------------------------------- KalshiGateway

OrderResult KalshiGateway::reconcile(const OrderRequest& req, OrderResult r,
                                     long long submittedSec) {
    if (!r.ambiguous) return r;
    spdlog::warn("order {} ambiguous ({}); reconciling", req.clientOrderId, r.error);
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300 * (attempt + 1)));
        if (auto found = client_->findOrderByClientId(req.marketId, req.clientOrderId,
                                                      submittedSec - 60)) {
            spdlog::info("reconciled {}: filled {}", req.clientOrderId,
                         fp::formatCount(found->filled));
            return *found;
        }
    }
    // Not found after several looks: the order most likely never reached the
    // matching engine. Keep the ambiguous flag so the executor treats it with care.
    r.error += " (not found during reconciliation)";
    return r;
}

OrderResult KalshiGateway::submit(const OrderRequest& req) {
    const long long t = nowMs() / 1000;
    OrderResult r = client_->createOrder(req);
    return reconcile(req, std::move(r), t);
}

std::vector<OrderResult> KalshiGateway::submitBatch(const std::vector<OrderRequest>& reqs) {
    const long long t = nowMs() / 1000;
    std::vector<OrderResult> out = client_->batchCreateOrders(reqs);
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = reconcile(reqs[i], std::move(out[i]), t);
    return out;
}

std::optional<BinaryBook> KalshiGateway::refresh(const Leg& leg) {
    if (leg.venue != Venue::Kalshi) return std::nullopt;
    return client_->orderbook(leg.marketId);
}

std::vector<std::optional<BinaryBook>> KalshiGateway::refreshAll(const std::vector<Leg>& legs) {
    std::vector<std::string> tickers;
    for (const auto& l : legs)
        if (l.venue == Venue::Kalshi) tickers.push_back(l.marketId);
    std::unordered_map<std::string, BinaryBook> byTicker;
    for (auto& b : client_->orderbooks(tickers)) {
        std::string id = b.id();
        byTicker[std::move(id)] = std::move(b);
    }
    std::vector<std::optional<BinaryBook>> out;
    for (const auto& l : legs) {
        auto it = byTicker.find(l.marketId);
        if (l.venue == Venue::Kalshi && it != byTicker.end()) {
            out.emplace_back(it->second);
        } else {
            out.emplace_back(std::nullopt);
        }
    }
    return out;
}

}  // namespace artemis
