#pragma once
// Polymarket CLOB public market-data client and the Kalshi <-> Polymarket
// market map. Only public, unauthenticated endpoints are used: the Polymarket
// leg of a cross-venue opportunity is detected, sized and paper-traded, but
// live Polymarket order routing is intentionally not implemented here (it
// needs an EIP-712 wallet signer and is unavailable to US persons on the
// international exchange).

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

namespace artemis {

struct MarketPair {
    std::string id;            // stable key, e.g. "fed-dec-cut"
    std::string label;
    std::string kalshiTicker;
    std::string polyYesToken;  // CLOB token id of the Polymarket YES outcome
    std::string polyNoToken;   // CLOB token id of the Polymarket NO outcome
    bool invert = false;       // true if Polymarket YES == Kalshi NO
    std::optional<double> feeBps;  // per-pair override
};

std::vector<MarketPair> loadMarketMap(const std::string& path);
std::vector<MarketPair> parseMarketMap(const std::string& json);

struct PolyTokenBook {
    std::string tokenId;
    Ladder bids, asks;
};

class PolymarketClient {
public:
    PolymarketClient(const PolymarketConfig& cfg, std::shared_ptr<RateLimiter> limiter);

    std::vector<PolyTokenBook> tokenBooks(const std::vector<std::string>& tokenIds);
    // Builds one normalised BinaryBook per pair (in Kalshi YES/NO orientation).
    std::vector<BinaryBook> pairBooks(const std::vector<MarketPair>& pairs);

    static PolyTokenBook parseTokenBook(const nlohmann::json& j);
    static BinaryBook combine(const MarketPair& pair, const PolyTokenBook* yes,
                              const PolyTokenBook* no, std::int64_t tsUs);

private:
    PolymarketConfig cfg_;
    std::shared_ptr<RateLimiter> limiter_;
    HttpClient http_;
};

}  // namespace artemis
