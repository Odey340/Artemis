#include "engine/Universe.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <unordered_set>

#include "core/Clock.hpp"
#include "venues/kalshi/KalshiClient.hpp"

namespace artemis {

using nlohmann::json;

bool matchesAny(const std::string& value, const std::vector<std::string>& patterns) {
    for (const auto& p : patterns) {
        if (p.empty()) continue;
        if (p.back() == '*') {
            if (value.compare(0, p.size() - 1, p, 0, p.size() - 1) == 0) return true;
        } else if (value == p) {
            return true;
        }
    }
    return false;
}

Micros noBasketGap(const EventInfo& ev) {
    // sum over legs of max(0, 1 - noAsk) minus the 1 payout we give up
    Micros sum = 0;
    int legs = 0;
    for (const auto& m : ev.markets) {
        if (!m.tradable()) continue;
        const Micros v = kOneDollar - m.noAsk;
        if (v > 0) {
            sum += v;
            ++legs;
        }
    }
    return legs >= 2 ? sum - kOneDollar : -kOneDollar;
}

Micros yesBasketGap(const EventInfo& ev) {
    Micros sum = 0;
    for (const auto& m : ev.markets) {
        if (!m.tradable()) return -kOneDollar;
        sum += m.yesAsk;
    }
    return ev.markets.size() >= 2 ? kOneDollar - sum : -kOneDollar;
}

std::vector<std::string> UniverseSnapshot::kalshiTickers() const {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (const auto& e : events)
        for (const auto& m : e.markets)
            if (m.tradable() && seen.insert(m.ticker).second) out.push_back(m.ticker);
    for (const auto& p : pairs)
        if (seen.insert(p.kalshiTicker).second) out.push_back(p.kalshiTicker);
    return out;
}

void UniverseSnapshot::index() {
    tickerToEvent.clear();
    tickerToPairs.clear();
    for (const auto& e : events) {
        for (const auto& m : e.markets) {
            tickerToEvent[m.ticker] = e.eventTicker;
            tickerFeeMultiplier[m.ticker] = e.feeMultiplier;
        }
    }
    for (const auto& p : pairs) tickerToPairs[p.kalshiTicker].push_back(p.id);
}

json UniverseSnapshot::toJson() const {
    json evs = json::array();
    for (const auto& e : events) {
        json ms = json::array();
        for (const auto& m : e.markets) {
            ms.push_back({{"ticker", m.ticker},
                          {"title", m.title},
                          {"status", m.status},
                          {"close_ts_us", m.closeTsUs}});
        }
        evs.push_back({{"event_ticker", e.eventTicker},
                       {"series_ticker", e.seriesTicker},
                       {"title", e.title},
                       {"mutually_exclusive", e.mutuallyExclusive},
                       {"exhaustive", e.exhaustive},
                       {"fee_type", e.feeType},
                       {"fee_multiplier", e.feeMultiplier},
                       {"markets", ms}});
    }
    json ps = json::array();
    for (const auto& p : pairs) {
        json pj = {{"id", p.id},
                   {"label", p.label},
                   {"kalshi_ticker", p.kalshiTicker},
                   {"poly_yes_token", p.polyYesToken},
                   {"poly_no_token", p.polyNoToken},
                   {"invert", p.invert}};
        if (p.feeBps) pj["fee_bps"] = *p.feeBps;
        ps.push_back(pj);
    }
    return {{"built_us", builtUs}, {"events", evs}, {"pairs", ps}};
}

UniverseSnapshot UniverseSnapshot::fromJson(const json& j) {
    UniverseSnapshot u;
    u.builtUs = j.value("built_us", 0LL);
    for (const auto& e : j.value("events", json::array())) {
        EventInfo ev;
        ev.eventTicker = e.at("event_ticker").get<std::string>();
        ev.seriesTicker = e.value("series_ticker", std::string());
        ev.title = e.value("title", ev.eventTicker);
        ev.mutuallyExclusive = e.value("mutually_exclusive", false);
        ev.exhaustive = e.value("exhaustive", false);
        ev.feeType = e.value("fee_type", std::string("quadratic"));
        ev.feeMultiplier = e.value("fee_multiplier", 1.0);
        for (const auto& m : e.value("markets", json::array())) {
            MarketInfo mi;
            mi.ticker = m.at("ticker").get<std::string>();
            mi.eventTicker = ev.eventTicker;
            mi.title = m.value("title", mi.ticker);
            mi.status = m.value("status", std::string("active"));
            mi.closeTsUs = m.value("close_ts_us", 0LL);
            ev.markets.push_back(std::move(mi));
        }
        u.events.push_back(std::move(ev));
    }
    if (j.contains("pairs")) u.pairs = parseMarketMap(j["pairs"].dump());
    u.index();
    return u;
}

UniverseBuilder::UniverseBuilder(const Config& cfg, std::shared_ptr<KalshiClient> client)
    : cfg_(cfg), client_(std::move(client)) {}

std::vector<EventInfo> UniverseBuilder::select(std::vector<EventInfo> all,
                                               std::int64_t nowUs) const {
    const auto& u = cfg_.universe;
    const Micros slack = static_cast<Micros>(std::llround(u.prefilterSlackCents * kOneCent));
    const std::int64_t closeGuard = static_cast<std::int64_t>(u.minSecondsToClose) * 1'000'000;

    struct Ranked {
        Micros gap;
        bool pinned;
        EventInfo ev;
    };
    std::vector<Ranked> ranked;
    for (auto& ev : all) {
        if (!ev.mutuallyExclusive) continue;
        if (matchesAny(ev.eventTicker, u.eventDeny) || matchesAny(ev.seriesTicker, u.seriesDeny))
            continue;
        const bool pinned = matchesAny(ev.eventTicker, u.eventAllow);
        if (!u.seriesAllow.empty() && !matchesAny(ev.seriesTicker, u.seriesAllow) && !pinned)
            continue;

        ev.exhaustive = matchesAny(ev.eventTicker, u.exhaustiveEvents) ||
                        matchesAny(ev.seriesTicker, u.exhaustiveSeries);

        // drop markets that close too soon to trade safely
        if (closeGuard > 0) {
            for (auto& m : ev.markets)
                if (m.closeTsUs > 0 && m.closeTsUs - nowUs < closeGuard) m.status = "closing";
        }
        int tradable = 0;
        for (const auto& m : ev.markets) tradable += m.tradable() ? 1 : 0;
        if (tradable < u.minMarkets || tradable > u.maxMarkets) continue;

        Micros gap = noBasketGap(ev);
        if (ev.exhaustive) gap = std::max(gap, yesBasketGap(ev));
        if (!pinned && gap < -slack) continue;
        ranked.push_back({gap, pinned, std::move(ev)});
    }
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
        if (a.pinned != b.pinned) return a.pinned;
        return a.gap > b.gap;
    });
    std::vector<EventInfo> out;
    for (auto& r : ranked) {
        if (static_cast<int>(out.size()) >= u.maxEventsTracked) break;
        out.push_back(std::move(r.ev));
    }
    return out;
}

void UniverseBuilder::resolveFees(std::vector<EventInfo>& events) {
    for (auto& ev : events) {
        if (ev.feeType != "quadratic" || ev.feeMultiplier != 1.0) continue;  // event override set
        if (ev.seriesTicker.empty()) continue;
        auto it = seriesFeeCache_.find(ev.seriesTicker);
        if (it == seriesFeeCache_.end()) {
            const SeriesFee f = client_->seriesFee(ev.seriesTicker);
            it = seriesFeeCache_.emplace(ev.seriesTicker, std::make_pair(f.feeType, f.multiplier)).first;
        }
        ev.feeType = it->second.first;
        ev.feeMultiplier = it->second.second;
    }
}

std::shared_ptr<UniverseSnapshot> UniverseBuilder::build(const std::vector<MarketPair>& pairs) {
    auto snap = std::make_shared<UniverseSnapshot>();
    snap->builtUs = nowUs();
    std::vector<EventInfo> all;
    std::string cursor;
    int pages = 0;
    do {
        cursor = client_->eventsPage(cursor, all);
        ++pages;
    } while (!cursor.empty() && pages < cfg_.universe.maxPages);

    // pinned events that the listing did not return (e.g. beyond max_pages)
    std::unordered_set<std::string> have;
    for (const auto& e : all) have.insert(e.eventTicker);
    for (const auto& p : cfg_.universe.eventAllow) {
        if (p.empty() || p.back() == '*' || have.count(p)) continue;
        if (auto e = client_->event(p)) all.push_back(std::move(*e));
    }

    const std::size_t total = all.size();
    snap->events = select(std::move(all), snap->builtUs);
    resolveFees(snap->events);
    snap->pairs = pairs;
    snap->index();
    spdlog::info("universe: {} open events scanned over {} pages, {} tracked ({} markets), {} pairs",
                 total, pages, snap->events.size(), snap->kalshiTickers().size(), pairs.size());
    return snap;
}

}  // namespace artemis
