#pragma once
// The set of Kalshi events and cross-venue pairs the engine watches.
//
// Discovery pages through every open event once per refresh, keeps the
// mutually-exclusive ones, and ranks them by how close their *listing*
// top-of-book is to an arbitrage. Only the closest `max_events_tracked`
// events get full order-book polling, which keeps the bot inside Kalshi's
// rate limits while still covering the whole exchange.

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "core/Config.hpp"
#include "venues/MarketTypes.hpp"
#include "venues/polymarket/PolymarketClient.hpp"

namespace artemis {

class KalshiClient;

struct UniverseSnapshot {
    std::vector<EventInfo> events;
    std::vector<MarketPair> pairs;
    std::unordered_map<std::string, std::string> tickerToEvent;
    std::unordered_map<std::string, std::vector<std::string>> tickerToPairs;
    std::unordered_map<std::string, double> tickerFeeMultiplier;
    std::int64_t builtUs = 0;

    std::vector<std::string> kalshiTickers() const;  // every ticker that needs a book
    void index();

    nlohmann::json toJson() const;
    static UniverseSnapshot fromJson(const nlohmann::json& j);
};

// Listing-based "distance to arbitrage" in micros (positive = arb on top of book).
Micros noBasketGap(const EventInfo& ev);
Micros yesBasketGap(const EventInfo& ev);

bool matchesAny(const std::string& value, const std::vector<std::string>& patterns);

class UniverseBuilder {
public:
    UniverseBuilder(const Config& cfg, std::shared_ptr<KalshiClient> client);
    std::shared_ptr<UniverseSnapshot> build(const std::vector<MarketPair>& pairs);

    // Filtering/ranking without network access (unit-testable).
    std::vector<EventInfo> select(std::vector<EventInfo> all, std::int64_t nowUs) const;

private:
    void resolveFees(std::vector<EventInfo>& events);

    Config cfg_;
    std::shared_ptr<KalshiClient> client_;
    std::unordered_map<std::string, std::pair<std::string, double>> seriesFeeCache_;
};

}  // namespace artemis
