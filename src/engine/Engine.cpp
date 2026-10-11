#include "engine/Engine.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>
#include <thread>
#include <unordered_set>

#include "core/Clock.hpp"
#include "core/FixedPoint.hpp"
#include "engine/Journal.hpp"
#include "exec/BasketExecutor.hpp"
#include "exec/OrderGateway.hpp"
#include "risk/RiskManager.hpp"
#include "strategy/ArbDetector.hpp"
#include "strategy/BookStore.hpp"
#include "venues/kalshi/KalshiClient.hpp"
#include "venues/polymarket/PolymarketClient.hpp"

namespace artemis {

namespace {
std::atomic<bool> gStop{false};

std::shared_ptr<KalshiSigner> loadSigner(const KalshiConfig& k) {
    if (k.apiKeyId.empty() || k.privateKeyPath.empty()) return nullptr;
    return KalshiSigner::fromPemFile(k.apiKeyId, k.privateKeyPath);
}
}  // namespace

void Engine::requestStop() { gStop.store(true); }
bool Engine::stopRequested() { return gStop.load(); }

Engine::Engine(Config cfg, EngineOptions opts)
    : cfg_(std::move(cfg)),
      opts_(std::move(opts)),
      queue_(static_cast<std::size_t>(cfg_.feed.queueCapacity)),
      kalshiRead_(std::make_shared<RateLimiter>(cfg_.kalshi.readRatePerSec,
                                                std::max(1.0, cfg_.kalshi.readRatePerSec / 2))),
      kalshiWrite_(std::make_shared<RateLimiter>(cfg_.kalshi.writeRatePerSec,
                                                 std::max(1.0, cfg_.kalshi.writeRatePerSec / 2))) {}

std::shared_ptr<UniverseSnapshot> Engine::universe() {
    std::lock_guard<std::mutex> lk(uniMu_);
    return universe_;
}

void Engine::publish(std::shared_ptr<UniverseSnapshot> u) {
    std::lock_guard<std::mutex> lk(uniMu_);
    universe_ = std::move(u);
}

int Engine::run() {
    spdlog::info("Artemis engine starting: mode={} execute={} kalshi={} polymarket={}", cfg_.mode,
                 opts_.execute, cfg_.kalshi.resolvedBaseUrl(),
                 cfg_.polymarket.enabled ? cfg_.polymarket.clobUrl : "off");
    const auto started = std::chrono::steady_clock::now();
    std::thread feed([this] { feedLoop(); });
    std::thread strat([this] { strategyLoop(); });
    while (!stopRequested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (opts_.durationSec > 0 &&
            std::chrono::steady_clock::now() - started > std::chrono::seconds(opts_.durationSec)) {
            requestStop();
        }
    }
    feed.join();
    strat.join();
    spdlog::info("Artemis engine stopped");
    return 0;
}

// ------------------------------------------------------------------ feed thread

void Engine::feedLoop() {
    try {
        auto signer = loadSigner(cfg_.kalshi);
        auto kalshi = std::make_shared<KalshiClient>(cfg_.kalshi, signer, kalshiRead_, kalshiWrite_);
        UniverseBuilder builder(cfg_, kalshi);

        std::vector<MarketPair> pairs;
        std::unique_ptr<PolymarketClient> poly;
        if (cfg_.polymarket.enabled) {
            pairs = loadMarketMap(cfg_.polymarket.marketMapPath);
            poly = std::make_unique<PolymarketClient>(
                cfg_.polymarket, std::make_shared<RateLimiter>(cfg_.polymarket.readRatePerSec, 2.0));
            spdlog::info("loaded {} Kalshi<->Polymarket pairs", pairs.size());
        }

        std::shared_ptr<UniverseSnapshot> snap;
        auto nextRefresh = std::chrono::steady_clock::now();
        while (!stopRequested()) {
            const auto t0 = std::chrono::steady_clock::now();
            if (t0 >= nextRefresh || !snap) {
                try {
                    snap = builder.build(pairs);
                    publish(snap);
                } catch (const std::exception& e) {
                    spdlog::error("universe refresh failed: {}", e.what());
                }
                nextRefresh = t0 + std::chrono::seconds(std::max(5, cfg_.universe.refreshSeconds));
            }
            if (snap) {
                auto push = [&](BinaryBook&& b) {
                    while (!queue_.tryPush(std::move(b))) {
                        if (stopRequested()) return;
                        std::this_thread::sleep_for(std::chrono::microseconds(200));
                    }
                    booksFetched_.fetch_add(1, std::memory_order_relaxed);
                };
                for (auto& b : kalshi->orderbooks(snap->kalshiTickers())) push(std::move(b));
                if (poly && !pairs.empty())
                    for (auto& b : poly->pairBooks(pairs)) push(std::move(b));
            }
            const auto dt = std::chrono::steady_clock::now() - t0;
            feedCycleUs_.store(std::chrono::duration_cast<std::chrono::microseconds>(dt).count());
            const auto wait = std::chrono::milliseconds(cfg_.feed.pollIntervalMs) - dt;
            const auto until = std::chrono::steady_clock::now() + wait;
            while (!stopRequested() && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    } catch (const std::exception& e) {
        spdlog::critical("feed thread died: {}", e.what());
        requestStop();
    }
    feedDone_.store(true);
}

// -------------------------------------------------------------- strategy thread

void Engine::strategyLoop() {
    try {
        const bool live = cfg_.live() && opts_.execute;
        BookStore store;
        ArbDetector detector(cfg_.strategy, cfg_.feed, cfg_.polymarket, cfg_.basis);
        RiskManager risk(cfg_.risk);
        OpportunityLog oppLog(cfg_.output.dir + "/opportunities.csv");
        TradeLog tradeLog(cfg_.output.dir + (live ? "/trades_live.csv" : "/trades_paper.csv"));
        std::unique_ptr<BookRecorder> recorder;
        if (!opts_.recordPath.empty()) recorder = std::make_unique<BookRecorder>(opts_.recordPath);

        std::shared_ptr<KalshiClient> tradeClient;
        std::unique_ptr<OrderGateway> gateway;
        if (live) {
            auto signer = loadSigner(cfg_.kalshi);
            if (!signer) throw std::runtime_error("live mode requires Kalshi credentials");
            tradeClient = std::make_shared<KalshiClient>(cfg_.kalshi, signer, kalshiRead_, kalshiWrite_);
            gateway = std::make_unique<KalshiGateway>(tradeClient);
            if (auto bal = tradeClient->balance()) {
                risk.setAvailableBalance(*bal);
                spdlog::info("Kalshi balance {}", fp::usd(*bal));
            } else {
                throw std::runtime_error("could not read Kalshi balance; check API credentials");
            }
        } else {
            gateway = std::make_unique<SimGateway>(store);
            risk.setAvailableBalance(
                static_cast<Micros>(std::llround(cfg_.backtest.startingEquityUsd * kOneDollar)));
        }
        BasketExecutor executor(*gateway, cfg_.execution, cfg_.strategy);

        std::shared_ptr<UniverseSnapshot> uni;
        std::unordered_set<std::string> groups;
        RollingStatistics evalUs(200, 5);
        long long opps = 0, executed = 0;
        Micros locked = 0, realized = 0;
        auto lastStatus = std::chrono::steady_clock::now();
        auto lastBalance = lastStatus;
        auto lastExchangeCheck = lastStatus - std::chrono::minutes(5);
        bool tradingActive = !live;
        bool exchangeChecked = false;

        while (!stopRequested()) {
            // pick up a new universe published by the feed thread
            if (auto u = universe(); u && u != uni) {
                std::unordered_set<std::string> next;
                for (const auto& e : u->events) next.insert(e.eventTicker);
                for (const auto& p : u->pairs) next.insert(p.id);
                for (const auto& g : groups)
                    if (!next.count(g)) risk.releaseGroup(g);
                groups.swap(next);
                std::unordered_map<std::string, FeeModel> fees;
                for (const auto& [t, m] : u->tickerFeeMultiplier)
                    fees[t] = FeeModel::kalshi(cfg_.strategy.kalshiTakerRate, m);
                detector.setTickerFees(std::move(fees));
                uni = std::move(u);
                if (recorder) {
                    std::ofstream uj(opts_.recordPath + ".universe.json");
                    uj << uni->toJson().dump(2);
                }
            }

            std::unordered_set<std::string> dirty;
            int drained = 0;
            while (auto b = queue_.tryPop()) {
                ++drained;
                if (recorder) recorder->write(*b);
                if (uni) {
                    if (b->venue() == Venue::Kalshi) {
                        auto it = uni->tickerToEvent.find(b->id());
                        if (it != uni->tickerToEvent.end()) dirty.insert(it->second);
                        auto pit = uni->tickerToPairs.find(b->id());
                        if (pit != uni->tickerToPairs.end())
                            for (const auto& pid : pit->second) dirty.insert(pid);
                    } else {
                        dirty.insert(b->id());
                    }
                }
                store.put(std::move(*b));
                if (drained >= 4096) break;
            }
            if (drained == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            if (!uni || dirty.empty()) continue;

            const auto nowSteady = std::chrono::steady_clock::now();
            if (live && nowSteady - lastExchangeCheck > std::chrono::seconds(60)) {
                const ExchangeStatus st = tradeClient->exchangeStatus();
                if (!exchangeChecked || st.tradingActive != tradingActive) {
                    if (st.tradingActive) spdlog::info("Kalshi exchange is open for trading");
                    else spdlog::warn("Kalshi trading is paused/closed; executions suspended");
                }
                exchangeChecked = true;
                tradingActive = st.tradingActive;
                lastExchangeCheck = nowSteady;
            }
            if (live && nowSteady - lastBalance > std::chrono::seconds(30)) {
                if (auto bal = tradeClient->balance()) risk.setAvailableBalance(*bal);
                lastBalance = nowSteady;
            }

            const std::int64_t t0 = monoUs();
            const std::int64_t now = nowUs();
            auto found = detector.scan(uni->events, uni->pairs, store, now, &dirty);
            evalUs.update(static_cast<double>(monoUs() - t0));

            for (auto& opp : found) {
                // first sighting at info level, repeats of a persisting edge at debug
                const bool first = opp.confirmations == cfg_.strategy.confirmations;
                if (first) ++opps;
                spdlog::log(first ? spdlog::level::info : spdlog::level::debug,
                            "ARB {} {} | {} | units={} cost={} fees={} profit={} roi={:.2f}%",
                            toString(opp.kind), opp.groupId, legsSummary(opp),
                            fp::formatCount(opp.units), fp::usd(opp.cost), fp::usd(opp.fees),
                            fp::usd(opp.profit), opp.roi * 100);
                if (!opts_.execute) {
                    if (first) oppLog.write(opp, "observe");
                    continue;
                }
                if (live && !tradingActive) {
                    oppLog.write(opp, "skip: exchange not trading");
                    continue;
                }
                if (opp.crossVenue() && !gateway->canTrade(Venue::Polymarket)) {
                    oppLog.write(opp, "signal-only: polymarket leg not routable live");
                    continue;
                }
                const RiskDecision d = risk.check(opp, now);
                if (!d.approved) {
                    if (first || d.reason != "cooldown") oppLog.write(opp, "reject: " + d.reason);
                    continue;
                }
                const Micros perUnit = (opp.cost + opp.fees) * kOneContract / opp.units;
                Qty cap = perUnit > 0 ? (d.maxCost / perUnit) * kOneContract : opp.units;
                cap = std::min(cap, opp.units);
                if (cap < kOneContract) {
                    oppLog.write(opp, "reject: risk budget below one unit");
                    continue;
                }
                if (cap < opp.units) {
                    auto smaller = detector.resize(opp, store, cap);
                    if (!smaller) {
                        oppLog.write(opp, "reject: not profitable at risk-budget size");
                        continue;
                    }
                    opp = std::move(*smaller);
                }
                ExecutionReport rep = executor.execute(opp, opp.units);
                risk.onExecution(opp, rep, now);
                const Micros execLoss = rep.realizedPnl + std::min<Micros>(0, rep.lockedProfit);
                if (execLoss < -static_cast<Micros>(
                                   std::llround(cfg_.execution.maxLegLossUsd * kOneDollar)))
                    risk.halt("single execution loss above max_leg_loss_usd");
                if (rep.status != ExecStatus::Skipped) tradeLog.write(nowUs(), opp, rep);
                oppLog.write(opp, std::string("exec: ") + toString(rep.status));
                if (rep.traded()) ++executed;
                locked += rep.lockedProfit;
                realized += rep.realizedPnl;
                spdlog::info("RESULT {} {} locked={} realized={} {}", toString(rep.status),
                             opp.groupId, fp::usd(rep.lockedProfit), fp::usd(rep.realizedPnl),
                             rep.note);
                if (risk.halted()) spdlog::critical("TRADING HALTED: {}", risk.haltReason());
            }

            if (nowSteady - lastStatus > std::chrono::seconds(15)) {
                lastStatus = nowSteady;
                if (recorder) recorder->flush();
                spdlog::info(
                    "status: books={} fetched={} feed_cycle={}ms eval={:.0f}us opps={} trades={} "
                    "locked={} realized={} exposure={} balance={}{}",
                    store.size(), booksFetched_.load(), feedCycleUs_.load() / 1000, evalUs.mean(),
                    opps, executed, fp::usd(locked), fp::usd(realized), fp::usd(risk.exposure()),
                    fp::usd(risk.availableBalance()),
                    risk.halted() ? " HALTED(" + risk.haltReason() + ")" : std::string());
                for (const auto& [id, bm] : detector.basis()) {
                    if (bm.stats.ready())
                        spdlog::info("basis {} = {:.2f}c (mean {:.2f}c, z {:.2f}) {}", id,
                                     bm.stats.last(), bm.stats.mean(), bm.stats.lastZ(),
                                     toString(bm.last));
                }
            }
        }
        if (recorder) recorder->flush();
    } catch (const std::exception& e) {
        spdlog::critical("strategy thread died: {}", e.what());
        requestStop();
    }
}

}  // namespace artemis
