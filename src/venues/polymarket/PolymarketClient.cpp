#include "venues/polymarket/PolymarketClient.hpp"

#include <spdlog/spdlog.h>

#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include "core/Clock.hpp"
#include "core/FixedPoint.hpp"

namespace artemis {

using nlohmann::json;

std::vector<MarketPair> parseMarketMap(const std::string& text) {
    std::vector<MarketPair> out;
    const json j = json::parse(text, nullptr, true, true);
    const json& arr = j.is_array() ? j : j.at("pairs");
    for (const auto& p : arr) {
        MarketPair m;
        m.kalshiTicker = p.at("kalshi_ticker").get<std::string>();
        m.polyYesToken = p.at("poly_yes_token").get<std::string>();
        m.polyNoToken = p.value("poly_no_token", std::string());
        m.id = p.value("id", m.kalshiTicker);
        m.label = p.value("label", m.id);
        m.invert = p.value("invert", false);
        if (p.contains("fee_bps") && p["fee_bps"].is_number()) m.feeBps = p["fee_bps"].get<double>();
        out.push_back(std::move(m));
    }
    return out;
}

std::vector<MarketPair> loadMarketMap(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open market map: " + path);
    std::stringstream ss;
    ss << in.rdbuf();
    return parseMarketMap(ss.str());
}

PolymarketClient::PolymarketClient(const PolymarketConfig& cfg,
                                   std::shared_ptr<RateLimiter> limiter)
    : cfg_(cfg), limiter_(std::move(limiter)), http_(cfg.timeoutMs) {}

PolyTokenBook PolymarketClient::parseTokenBook(const json& j) {
    PolyTokenBook b;
    b.tokenId = j.value("asset_id", std::string());
    auto side = [](const json& arr) {
        Ladder l;
        if (!arr.is_array()) return l;
        for (const auto& x : arr) {
            const auto price = fp::parseDollars(x.value("price", std::string("0")));
            const auto size = fp::parseCount(x.value("size", std::string("0")));
            if (price && size) l.push_back({*price, *size});
        }
        return l;
    };
    b.bids = side(j.value("bids", json::array()));
    b.asks = side(j.value("asks", json::array()));
    return b;
}

std::vector<PolyTokenBook> PolymarketClient::tokenBooks(const std::vector<std::string>& ids) {
    std::vector<PolyTokenBook> out;
    if (ids.empty()) return out;
    json body = json::array();
    for (const auto& id : ids) body.push_back({{"token_id", id}});
    limiter_->acquire();
    HttpResponse r = http_.request("POST", cfg_.clobUrl + "/books", {}, body.dump());
    if (r.ok()) {
        try {
            const json j = json::parse(r.body);
            for (const auto& b : j) out.push_back(parseTokenBook(b));
            return out;
        } catch (const std::exception& e) {
            spdlog::warn("polymarket /books bad JSON: {}", e.what());
        }
    } else {
        spdlog::warn("polymarket /books -> {} {}; falling back to /book", r.status, r.error);
    }
    for (const auto& id : ids) {
        limiter_->acquire();
        HttpResponse one = http_.get(cfg_.clobUrl + "/book?token_id=" + HttpClient::escape(id));
        if (!one.ok()) continue;
        try {
            out.push_back(parseTokenBook(json::parse(one.body)));
        } catch (...) {
        }
    }
    return out;
}

BinaryBook PolymarketClient::combine(const MarketPair& pair, const PolyTokenBook* yes,
                                     const PolyTokenBook* no, std::int64_t tsUs) {
    // Re-orient to Kalshi's YES/NO: when inverted, Polymarket YES is Kalshi NO.
    const PolyTokenBook* kYes = pair.invert ? no : yes;
    const PolyTokenBook* kNo = pair.invert ? yes : no;
    Ladder yb, ya, nb, na;
    if (kYes) {
        yb = kYes->bids;
        ya = kYes->asks;
    }
    if (kNo) {
        nb = kNo->bids;
        na = kNo->asks;
    }
    // If one token is missing, derive it from its complement (CLOB complement matching).
    auto reflect = [](const Ladder& l) {
        Ladder o;
        for (const auto& x : l) o.push_back({kOneDollar - x.price, x.qty});
        return o;
    };
    if (!kNo && kYes) {
        nb = reflect(ya);
        na = reflect(yb);
    }
    if (!kYes && kNo) {
        yb = reflect(na);
        ya = reflect(nb);
    }
    return BinaryBook::fromTwoTokens(Venue::Polymarket, pair.id, yb, ya, nb, na, tsUs);
}

std::vector<BinaryBook> PolymarketClient::pairBooks(const std::vector<MarketPair>& pairs) {
    std::vector<std::string> ids;
    for (const auto& p : pairs) {
        ids.push_back(p.polyYesToken);
        if (!p.polyNoToken.empty()) ids.push_back(p.polyNoToken);
    }
    std::vector<BinaryBook> out;
    const std::int64_t ts = nowUs();
    std::unordered_map<std::string, PolyTokenBook> byId;
    for (std::size_t i = 0; i < ids.size(); i += 40) {
        std::vector<std::string> chunk(ids.begin() + static_cast<long>(i),
                                       ids.begin() + static_cast<long>(std::min(ids.size(), i + 40)));
        for (auto& b : tokenBooks(chunk)) byId[b.tokenId] = std::move(b);
    }
    for (const auto& p : pairs) {
        auto y = byId.find(p.polyYesToken);
        auto n = p.polyNoToken.empty() ? byId.end() : byId.find(p.polyNoToken);
        if (y == byId.end() && n == byId.end()) continue;
        out.push_back(combine(p, y == byId.end() ? nullptr : &y->second,
                              n == byId.end() ? nullptr : &n->second, ts));
    }
    return out;
}

}  // namespace artemis
