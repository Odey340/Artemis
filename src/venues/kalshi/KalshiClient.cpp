#include "venues/kalshi/KalshiClient.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <nlohmann/json.hpp>
#include <random>
#include <thread>

#include "core/Clock.hpp"
#include "core/FixedPoint.hpp"

namespace artemis {

using nlohmann::json;

namespace {

std::string str(const json& j, const char* key, const std::string& def = {}) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    if (it->is_string()) return it->get<std::string>();
    return it->dump();
}

Micros dollars(const json& j, const char* key, Micros def) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    if (it->is_string()) return fp::parseDollars(it->get<std::string>()).value_or(def);
    if (it->is_number()) return static_cast<Micros>(it->get<double>() * kOneDollar + 0.5);
    return def;
}

Qty count(const json& j, const char* key, Qty def = 0) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    if (it->is_string()) return fp::parseCount(it->get<std::string>()).value_or(def);
    if (it->is_number_integer()) return it->get<std::int64_t>() * kOneContract;
    if (it->is_number()) return static_cast<Qty>(it->get<double>() * kOneContract + 0.5);
    return def;
}

// [["0.5600","10.00"], ...]  (dollar strings)  or legacy [[56, 10], ...] (cents, ints)
Ladder parseLevels(const json& arr, bool legacyCents) {
    Ladder l;
    if (!arr.is_array()) return l;
    for (const auto& lv : arr) {
        if (!lv.is_array() || lv.size() < 2) continue;
        Level x;
        if (legacyCents) {
            x.price = lv[0].get<std::int64_t>() * kOneCent;
            x.qty = lv[1].get<std::int64_t>() * kOneContract;
        } else {
            x.price = lv[0].is_string() ? fp::parseDollars(lv[0].get<std::string>()).value_or(0)
                                        : static_cast<Micros>(lv[0].get<double>() * kOneDollar);
            x.qty = lv[1].is_string() ? fp::parseCount(lv[1].get<std::string>()).value_or(0)
                                      : static_cast<Qty>(lv[1].get<double>() * kOneContract);
        }
        l.push_back(x);
    }
    return l;
}

}  // namespace

KalshiClient::KalshiClient(const KalshiConfig& cfg, std::shared_ptr<KalshiSigner> signer,
                           std::shared_ptr<RateLimiter> readLimiter,
                           std::shared_ptr<RateLimiter> writeLimiter)
    : cfg_(cfg),
      baseUrl_(cfg.resolvedBaseUrl()),
      signer_(std::move(signer)),
      readLimiter_(std::move(readLimiter)),
      writeLimiter_(std::move(writeLimiter)),
      http_(cfg.timeoutMs) {
    // base path = everything after the host, e.g. "/trade-api/v2"
    const auto scheme = baseUrl_.find("://");
    const auto slash = baseUrl_.find('/', scheme == std::string::npos ? 0 : scheme + 3);
    basePath_ = slash == std::string::npos ? std::string() : baseUrl_.substr(slash);
    while (!basePath_.empty() && basePath_.back() == '/') basePath_.pop_back();
    while (!baseUrl_.empty() && baseUrl_.back() == '/') baseUrl_.pop_back();
}

HttpResponse KalshiClient::send(const std::string& method, const std::string& pathAndQuery,
                                const std::string& body, bool auth, bool write,
                                int maxRetries) {
    static thread_local std::mt19937 rng{std::random_device{}()};
    HttpResponse r;
    for (int attempt = 0; attempt <= maxRetries; ++attempt) {
        (write ? writeLimiter_ : readLimiter_)->acquire();
        std::vector<std::string> headers;
        if (auth) {
            if (!signer_) {
                r.status = 401;
                r.error = "no Kalshi credentials configured";
                return r;
            }
            headers = signer_->headers(method, basePath_ + pathAndQuery, nowMs());
        }
        r = http_.request(method, baseUrl_ + pathAndQuery, headers, body);
        lastLatencyMs_ = r.elapsedMs;
        if (!r.retryable() || attempt == maxRetries) break;

        double backoff = 0.2 * std::pow(2.0, attempt);
        if (r.status == 429) {
            backoff = std::max(backoff, r.retryAfterSec > 0 ? r.retryAfterSec : 1.0);
            (write ? writeLimiter_ : readLimiter_)->penalise(backoff);
        }
        std::uniform_real_distribution<double> jitter(0.0, backoff * 0.25);
        spdlog::warn("kalshi {} {} -> {} {} (attempt {}/{}), retrying in {:.2f}s", method,
                     pathAndQuery, r.status, r.error, attempt + 1, maxRetries + 1, backoff);
        std::this_thread::sleep_for(std::chrono::duration<double>(backoff + jitter(rng)));
    }
    return r;
}

std::optional<json> KalshiClient::getJson(const std::string& pathAndQuery, bool auth) {
    HttpResponse r = send("GET", pathAndQuery, {}, auth, false, cfg_.maxRetries);
    if (!r.ok()) {
        spdlog::error("kalshi GET {} failed: {} {} {}", pathAndQuery, r.status, r.error,
                      r.body.substr(0, 300));
        return std::nullopt;
    }
    try {
        return json::parse(r.body);
    } catch (const std::exception& e) {
        spdlog::error("kalshi GET {}: bad JSON: {}", pathAndQuery, e.what());
        return std::nullopt;
    }
}

ExchangeStatus KalshiClient::exchangeStatus() {
    ExchangeStatus s;
    if (auto j = getJson("/exchange/status", false)) {
        s.exchangeActive = j->value("exchange_active", false);
        s.tradingActive = j->value("trading_active", false);
    }
    return s;
}

EventInfo KalshiClient::parseEvent(const json& ev) {
    EventInfo e;
    e.eventTicker = str(ev, "event_ticker");
    e.seriesTicker = str(ev, "series_ticker");
    e.title = str(ev, "title");
    e.mutuallyExclusive = ev.value("mutually_exclusive", false);
    if (ev.contains("fee_type_override") && ev["fee_type_override"].is_string() &&
        !ev["fee_type_override"].get<std::string>().empty()) {
        e.feeType = ev["fee_type_override"].get<std::string>();
        if (ev.contains("fee_multiplier_override") && ev["fee_multiplier_override"].is_number())
            e.feeMultiplier = ev["fee_multiplier_override"].get<double>();
    }
    if (ev.contains("markets") && ev["markets"].is_array()) {
        for (const auto& m : ev["markets"]) {
            MarketInfo mi;
            mi.ticker = str(m, "ticker");
            mi.eventTicker = str(m, "event_ticker", e.eventTicker);
            mi.title = str(m, "yes_sub_title", str(m, "title"));
            mi.status = str(m, "status");
            mi.closeTsUs = parseRfc3339Us(str(m, "close_time"));
            mi.yesBid = dollars(m, "yes_bid_dollars", 0);
            mi.yesAsk = dollars(m, "yes_ask_dollars", kOneDollar);
            mi.noBid = dollars(m, "no_bid_dollars", 0);
            mi.noAsk = dollars(m, "no_ask_dollars", kOneDollar);
            // a zero ask means "no offer" in the listing payload
            if (mi.yesAsk <= 0) mi.yesAsk = kOneDollar;
            if (mi.noAsk <= 0) mi.noAsk = kOneDollar;
            if (!mi.ticker.empty()) e.markets.push_back(std::move(mi));
        }
    }
    return e;
}

std::string KalshiClient::eventsPage(const std::string& cursor, std::vector<EventInfo>& out,
                                     const std::string& seriesTicker) {
    std::string q = "/events?status=open&with_nested_markets=true&limit=200";
    if (!cursor.empty()) q += "&cursor=" + HttpClient::escape(cursor);
    if (!seriesTicker.empty()) q += "&series_ticker=" + HttpClient::escape(seriesTicker);
    auto j = getJson(q, false);
    if (!j) return {};
    if (j->contains("events") && (*j)["events"].is_array()) {
        for (const auto& ev : (*j)["events"]) out.push_back(parseEvent(ev));
    }
    return str(*j, "cursor");
}

std::optional<EventInfo> KalshiClient::event(const std::string& eventTicker) {
    auto j = getJson("/events/" + HttpClient::escape(eventTicker) + "?with_nested_markets=true",
                     false);
    if (!j || !j->contains("event")) return std::nullopt;
    EventInfo e = parseEvent((*j)["event"]);
    // some API versions return markets beside the event rather than nested
    if (e.markets.empty() && j->contains("markets")) {
        json ev = (*j)["event"];
        ev["markets"] = (*j)["markets"];
        e = parseEvent(ev);
    }
    return e;
}

SeriesFee KalshiClient::seriesFee(const std::string& seriesTicker) {
    SeriesFee f;
    auto j = getJson("/series/" + HttpClient::escape(seriesTicker), false);
    if (j && j->contains("series")) {
        const auto& s = (*j)["series"];
        f.feeType = str(s, "fee_type", "quadratic");
        if (s.contains("fee_multiplier") && s["fee_multiplier"].is_number())
            f.multiplier = s["fee_multiplier"].get<double>();
    }
    return f;
}

BinaryBook KalshiClient::parseOrderbook(const std::string& ticker, const json& body,
                                        std::int64_t tsUs) {
    // Accept {"orderbook_fp": {...}}, a bare {"yes_dollars":..}, or legacy {"orderbook": {...}}
    if (body.contains("orderbook_fp") && body["orderbook_fp"].is_object()) {
        const auto& ob = body["orderbook_fp"];
        return BinaryBook::fromKalshi(ticker, parseLevels(ob.value("yes_dollars", json()), false),
                                      parseLevels(ob.value("no_dollars", json()), false), tsUs);
    }
    if (body.contains("yes_dollars") || body.contains("no_dollars")) {
        return BinaryBook::fromKalshi(ticker,
                                      parseLevels(body.value("yes_dollars", json()), false),
                                      parseLevels(body.value("no_dollars", json()), false), tsUs);
    }
    if (body.contains("orderbook") && body["orderbook"].is_object()) {
        const auto& ob = body["orderbook"];
        if (ob.contains("yes_dollars") || ob.contains("no_dollars")) {
            return BinaryBook::fromKalshi(ticker,
                                          parseLevels(ob.value("yes_dollars", json()), false),
                                          parseLevels(ob.value("no_dollars", json()), false),
                                          tsUs);
        }
        return BinaryBook::fromKalshi(ticker, parseLevels(ob.value("yes", json()), true),
                                      parseLevels(ob.value("no", json()), true), tsUs);
    }
    return BinaryBook::fromKalshi(ticker, {}, {}, tsUs);
}

std::optional<BinaryBook> KalshiClient::orderbook(const std::string& ticker) {
    auto j = getJson("/markets/" + HttpClient::escape(ticker) + "/orderbook", false);
    if (!j) return std::nullopt;
    return parseOrderbook(ticker, *j, nowUs());
}

std::vector<BinaryBook> KalshiClient::orderbooks(const std::vector<std::string>& tickers) {
    std::vector<BinaryBook> out;
    const std::size_t batch = static_cast<std::size_t>(std::max(1, cfg_.orderbookBatchSize));
    for (std::size_t i = 0; i < tickers.size(); i += batch) {
        std::string q = "/markets/orderbooks?";
        const std::size_t end = std::min(tickers.size(), i + batch);
        for (std::size_t k = i; k < end; ++k) {
            if (k > i) q += '&';
            q += "tickers=" + HttpClient::escape(tickers[k]);
        }
        auto j = getJson(q, false);
        const std::int64_t ts = nowUs();
        if (!j) {
            // fall back to single-book requests for this chunk
            for (std::size_t k = i; k < end; ++k)
                if (auto b = orderbook(tickers[k])) out.push_back(std::move(*b));
            continue;
        }
        if (j->contains("orderbooks") && (*j)["orderbooks"].is_array()) {
            for (const auto& ob : (*j)["orderbooks"]) {
                out.push_back(parseOrderbook(str(ob, "ticker"), ob, ts));
            }
        }
    }
    return out;
}

std::optional<Micros> KalshiClient::balance() {
    auto j = getJson("/portfolio/balance", true);
    if (!j) return std::nullopt;
    if (j->contains("balance_dollars")) return dollars(*j, "balance_dollars", 0);
    if (j->contains("balance") && (*j)["balance"].is_number_integer())
        return (*j)["balance"].get<std::int64_t>() * kOneCent;
    return std::nullopt;
}

std::map<std::string, Qty> KalshiClient::positions() {
    std::map<std::string, Qty> out;
    std::string cursor;
    for (int page = 0; page < 50; ++page) {
        std::string q = "/portfolio/positions?limit=200&count_filter=position";
        if (!cursor.empty()) q += "&cursor=" + HttpClient::escape(cursor);
        auto j = getJson(q, true);
        if (!j) break;
        if (j->contains("market_positions") && (*j)["market_positions"].is_array()) {
            for (const auto& p : (*j)["market_positions"]) {
                Qty pos = count(p, "position_fp", 0);
                if (pos == 0 && p.contains("position") && p["position"].is_number_integer())
                    pos = p["position"].get<std::int64_t>() * kOneContract;
                if (pos != 0) out[str(p, "ticker")] = pos;
            }
        }
        cursor = str(*j, "cursor");
        if (cursor.empty()) break;
    }
    return out;
}

json KalshiClient::orderToV2Json(const OrderRequest& req, int subaccount) {
    // V2 orders are quoted on the YES book:
    //   buy YES @p  -> bid p        sell YES @p -> ask p
    //   buy NO  @q  -> ask (1-q)    sell NO  @q -> bid (1-q)
    const bool yes = req.outcome == Outcome::Yes;
    const bool bid = (yes && req.buy) || (!yes && !req.buy);
    const Micros yesPrice = yes ? req.limitPrice : kOneDollar - req.limitPrice;
    json j = {
        {"ticker", req.marketId},
        {"client_order_id", req.clientOrderId},
        {"side", bid ? "bid" : "ask"},
        {"count", fp::formatCount(req.qty)},
        {"price", fp::formatDollars(yesPrice, 4)},
        {"time_in_force", req.timeInForce},
        {"self_trade_prevention_type", "taker_at_cross"},
    };
    if (req.reduceOnly) j["reduce_only"] = true;
    if (subaccount > 0) j["subaccount"] = subaccount;
    return j;
}

OrderResult KalshiClient::parseOrderResponse(const json& raw, const OrderRequest& req) {
    const json& j = raw.contains("order") && raw["order"].is_object() ? raw["order"] : raw;
    OrderResult r;
    r.accepted = true;
    r.orderId = str(j, "order_id");
    r.clientOrderId = str(j, "client_order_id", req.clientOrderId);
    r.filled = count(j, "fill_count", count(j, "fill_count_fp", 0));
    r.remaining = count(j, "remaining_count", count(j, "remaining_count_fp", 0));
    Micros avgYes = dollars(j, "average_fill_price", -1);
    if (avgYes >= 0) {
        r.avgPrice = req.outcome == Outcome::Yes ? avgYes : kOneDollar - avgYes;
    } else if (r.filled > 0) {
        // fall back to fill cost when available (Order object)
        const Micros cost = dollars(j, "taker_fill_cost_dollars", 0) +
                            dollars(j, "maker_fill_cost_dollars", 0);
        r.avgPrice = cost > 0 ? cost * kOneContract / r.filled : req.limitPrice;
    }
    const Micros avgFee = dollars(j, "average_fee_paid", -1);
    if (avgFee >= 0) {
        r.fees = notional(avgFee, r.filled);
    } else {
        r.fees = dollars(j, "taker_fees_dollars", 0) + dollars(j, "maker_fees_dollars", 0);
    }
    if (j.contains("error") && j["error"].is_object()) {
        r.accepted = false;
        r.error = str(j["error"], "code") + ": " + str(j["error"], "message");
    }
    return r;
}

void KalshiClient::sanitiseFill(OrderResult& r, const OrderRequest& req) {
    if (r.filled <= 0) return;
    // A fill-or-kill/IOC fill can never be worse than its limit. If the average
    // price only satisfies that when read from the other side of the book, the
    // API reported it in the other outcome's terms: flip it.
    const Micros flipped = kOneDollar - r.avgPrice;
    const bool bad = req.buy ? r.avgPrice > req.limitPrice : r.avgPrice < req.limitPrice;
    const bool flippedOk = req.buy ? flipped <= req.limitPrice : flipped >= req.limitPrice;
    if (req.limitPrice > 0 && bad && flippedOk) {
        spdlog::warn("order {}: average price {} reported in opposite-outcome terms; using {}",
                     r.clientOrderId, fp::formatDollars(r.avgPrice), fp::formatDollars(flipped));
        r.avgPrice = flipped;
    }
    // Kalshi's taker fee is at most ~1.75c per contract (x multiplier). A
    // per-contract "average" above 5c must be a total.
    const Micros perContract = r.fees * kOneContract / r.filled;
    if (r.filled > kOneContract && perContract > 5 * kOneCent) {
        r.fees = r.fees * kOneContract / r.filled;
    }
}

OrderResult KalshiClient::createOrder(const OrderRequest& req) {
    const std::string body = orderToV2Json(req, cfg_.subaccount).dump();
    // maxRetries = 0: never resubmit an order automatically
    HttpResponse r = send("POST", "/portfolio/events/orders", body, true, true, 0);
    OrderResult out;
    out.clientOrderId = req.clientOrderId;
    out.httpStatus = r.status;
    if (r.transportError() || r.status >= 500) {
        out.ambiguous = true;
        out.error = r.error.empty() ? r.body.substr(0, 300) : r.error;
        return out;
    }
    if (!r.ok()) {
        out.error = r.body.substr(0, 300);
        return out;
    }
    try {
        out = parseOrderResponse(json::parse(r.body), req);
        out.httpStatus = r.status;
        sanitiseFill(out, req);
    } catch (const std::exception& e) {
        out.ambiguous = true;
        out.error = std::string("unparseable order response: ") + e.what();
    }
    return out;
}

std::vector<OrderResult> KalshiClient::batchCreateOrders(const std::vector<OrderRequest>& reqs) {
    json arr = json::array();
    for (const auto& r : reqs) arr.push_back(orderToV2Json(r, cfg_.subaccount));
    const std::string body = json{{"orders", arr}}.dump();
    HttpResponse r = send("POST", "/portfolio/events/orders/batched", body, true, true, 0);
    std::vector<OrderResult> out(reqs.size());
    for (std::size_t i = 0; i < reqs.size(); ++i) {
        out[i].clientOrderId = reqs[i].clientOrderId;
        out[i].httpStatus = r.status;
    }
    if (r.transportError() || r.status >= 500) {
        for (auto& o : out) {
            o.ambiguous = true;
            o.error = r.error;
        }
        return out;
    }
    if (!r.ok()) {
        for (auto& o : out) o.error = r.body.substr(0, 300);
        return out;
    }
    try {
        const json j = json::parse(r.body);
        const auto& orders = j.at("orders");
        for (std::size_t i = 0; i < reqs.size() && i < orders.size(); ++i) {
            // match by client_order_id when present, else by position
            std::size_t idx = i;
            const std::string cid = str(orders[i], "client_order_id");
            for (std::size_t k = 0; k < reqs.size(); ++k)
                if (!cid.empty() && reqs[k].clientOrderId == cid) idx = k;
            out[idx] = parseOrderResponse(orders[i], reqs[idx]);
            out[idx].httpStatus = r.status;
            sanitiseFill(out[idx], reqs[idx]);
        }
    } catch (const std::exception& e) {
        for (auto& o : out) {
            o.ambiguous = true;
            o.error = std::string("unparseable batch response: ") + e.what();
        }
    }
    return out;
}

std::optional<OrderResult> KalshiClient::findOrderByClientId(const std::string& ticker,
                                                             const std::string& clientOrderId,
                                                             long long minTsSec) {
    std::string q = "/portfolio/orders?limit=200&ticker=" + HttpClient::escape(ticker) +
                    "&min_ts=" + std::to_string(minTsSec);
    auto j = getJson(q, true);
    if (!j || !j->contains("orders")) return std::nullopt;
    for (const auto& o : (*j)["orders"]) {
        if (str(o, "client_order_id") != clientOrderId) continue;
        OrderRequest req;
        req.clientOrderId = clientOrderId;
        const std::string outcome = str(o, "outcome_side", str(o, "side"));
        req.outcome = outcome == "no" ? Outcome::No : Outcome::Yes;
        OrderResult r = parseOrderResponse(o, req);
        // Order objects give outcome-specific prices directly
        if (r.filled > 0) {
            const Micros cost = dollars(o, "taker_fill_cost_dollars", 0) +
                                dollars(o, "maker_fill_cost_dollars", 0);
            if (cost > 0) r.avgPrice = cost * kOneContract / r.filled;
        }
        return r;
    }
    return std::nullopt;
}

bool KalshiClient::cancelOrder(const std::string& orderId) {
    HttpResponse r = send("DELETE", "/portfolio/events/orders/" + HttpClient::escape(orderId), {},
                          true, true, cfg_.maxRetries);
    return r.ok();
}

}  // namespace artemis
