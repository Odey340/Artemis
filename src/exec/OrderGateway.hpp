#pragma once
// Where orders go. The basket executor is written against this interface so
// the exact same leg-risk logic runs live (KalshiGateway), in paper trading
// and in back-tests (SimGateway).

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/OrderBook.hpp"
#include "strategy/BookStore.hpp"
#include "strategy/Opportunity.hpp"
#include "venues/MarketTypes.hpp"

namespace artemis {

class KalshiClient;

class OrderGateway {
public:
    virtual ~OrderGateway() = default;
    virtual std::string name() const = 0;
    virtual bool canTrade(Venue v) const = 0;
    virtual OrderResult submit(const OrderRequest& req) = 0;
    virtual std::vector<OrderResult> submitBatch(const std::vector<OrderRequest>& reqs) {
        std::vector<OrderResult> out;
        for (const auto& r : reqs) out.push_back(submit(r));
        return out;
    }
    // Most recent book for a leg's market (nullopt if unavailable).
    virtual std::optional<BinaryBook> refresh(const Leg& leg) = 0;
    // Books for several legs at once (one round-trip where the venue allows it).
    virtual std::vector<std::optional<BinaryBook>> refreshAll(const std::vector<Leg>& legs) {
        std::vector<std::optional<BinaryBook>> out;
        for (const auto& l : legs) out.push_back(refresh(l));
        return out;
    }
};

// Fills orders against the BookStore and removes the liquidity it takes.
class SimGateway : public OrderGateway {
public:
    explicit SimGateway(BookStore& books) : books_(books) {}
    std::string name() const override { return "sim"; }
    bool canTrade(Venue) const override { return true; }
    OrderResult submit(const OrderRequest& req) override;
    std::optional<BinaryBook> refresh(const Leg& leg) override;

    // Optional: force the next N submits to be rejected (tests / chaos drills).
    void failNext(int n) { failNext_ = n; }
    // Called before every order is matched; the back-tester uses it to let the
    // market move between legs (per-order latency).
    void setBeforeSubmit(std::function<void()> f) { beforeSubmit_ = std::move(f); }

private:
    BookStore& books_;
    std::function<void()> beforeSubmit_;
    int failNext_ = 0;
    long seq_ = 0;
};

// Live Kalshi routing. Polymarket legs are not routable live.
class KalshiGateway : public OrderGateway {
public:
    explicit KalshiGateway(std::shared_ptr<KalshiClient> client) : client_(std::move(client)) {}
    std::string name() const override { return "kalshi-live"; }
    bool canTrade(Venue v) const override { return v == Venue::Kalshi; }
    OrderResult submit(const OrderRequest& req) override;
    std::vector<OrderResult> submitBatch(const std::vector<OrderRequest>& reqs) override;
    std::optional<BinaryBook> refresh(const Leg& leg) override;
    std::vector<std::optional<BinaryBook>> refreshAll(const std::vector<Leg>& legs) override;

private:
    OrderResult reconcile(const OrderRequest& req, OrderResult r, long long submittedSec);
    std::shared_ptr<KalshiClient> client_;
};

std::string newClientOrderId();

}  // namespace artemis
