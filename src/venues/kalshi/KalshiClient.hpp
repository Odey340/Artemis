#pragma once
// Kalshi Trade API v2 REST client.
//
// Read endpoints are rate limited and retried with exponential back-off.
// Order creation is *never* blindly retried: an ambiguous failure is
// reconciled by looking the order up through its client_order_id.

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "core/Config.hpp"
#include "core/OrderBook.hpp"
#include "core/RateLimiter.hpp"
#include "net/HttpClient.hpp"
#include "venues/MarketTypes.hpp"
#include "venues/kalshi/KalshiSigner.hpp"

namespace artemis {

struct ExchangeStatus {
    bool exchangeActive = false;
    bool tradingActive = false;
};

struct SeriesFee {
    std::string feeType = "quadratic";
    double multiplier = 1.0;
};

class KalshiClient {
public:
    // Rate limiters are shared between the clients owned by different threads.
    KalshiClient(const KalshiConfig& cfg, std::shared_ptr<KalshiSigner> signer,
                 std::shared_ptr<RateLimiter> readLimiter,
                 std::shared_ptr<RateLimiter> writeLimiter);

    bool authenticated() const { return signer_ != nullptr; }

    ExchangeStatus exchangeStatus();
    // One page of events (with nested markets). Returns the next cursor ("" when done).
    std::string eventsPage(const std::string& cursor, std::vector<EventInfo>& out,
                           const std::string& seriesTicker = {});
    std::optional<EventInfo> event(const std::string& eventTicker);
    SeriesFee seriesFee(const std::string& seriesTicker);

    std::optional<BinaryBook> orderbook(const std::string& ticker);
    std::vector<BinaryBook> orderbooks(const std::vector<std::string>& tickers);

    // --- authenticated ---
    std::optional<Micros> balance();
    std::map<std::string, Qty> positions();  // signed: +YES / -NO
    OrderResult createOrder(const OrderRequest& req);
    std::vector<OrderResult> batchCreateOrders(const std::vector<OrderRequest>& reqs);
    std::optional<OrderResult> findOrderByClientId(const std::string& ticker,
                                                   const std::string& clientOrderId,
                                                   long long minTsSec);
    bool cancelOrder(const std::string& orderId);

    // Parsers exposed for unit tests.
    static BinaryBook parseOrderbook(const std::string& ticker, const nlohmann::json& ob,
                                     std::int64_t tsUs);
    static EventInfo parseEvent(const nlohmann::json& ev);
    static nlohmann::json orderToV2Json(const OrderRequest& req, int subaccount);
    static OrderResult parseOrderResponse(const nlohmann::json& j, const OrderRequest& req);
    // Defensive normalisation of reported fill price / fee units.
    static void sanitiseFill(OrderResult& r, const OrderRequest& req);

    const std::string& baseUrl() const { return baseUrl_; }
    double lastLatencyMs() const { return lastLatencyMs_; }

private:
    HttpResponse send(const std::string& method, const std::string& pathAndQuery,
                      const std::string& body, bool auth, bool write, int maxRetries);
    std::optional<nlohmann::json> getJson(const std::string& pathAndQuery, bool auth);

    KalshiConfig cfg_;
    std::string baseUrl_;
    std::string basePath_;  // "/trade-api/v2" — part of the signed path
    std::shared_ptr<KalshiSigner> signer_;
    std::shared_ptr<RateLimiter> readLimiter_, writeLimiter_;
    HttpClient http_;
    double lastLatencyMs_ = 0.0;
};

}  // namespace artemis
