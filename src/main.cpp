// Artemis — Kalshi arbitrage engine (C++17).
//
//   artemis scan     [-c cfg]                      one-shot scan, prints opportunities
//   artemis run      [-c cfg] [--live] [-d secs]   paper trade (default) or live trade
//   artemis record   [-c cfg] -o file.csv [-d s]   record order books for back-testing
//   artemis backtest <file.csv> [-c cfg] [--latency ms] [--out prefix]
//   artemis balance  [-c cfg]                      verify API keys: balance + positions

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <csignal>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "backtest/Backtester.hpp"
#include "core/Clock.hpp"
#include "core/Config.hpp"
#include "core/FixedPoint.hpp"
#include "engine/Engine.hpp"
#include "engine/Journal.hpp"
#include "engine/Universe.hpp"
#include "net/HttpClient.hpp"
#include "strategy/ArbDetector.hpp"
#include "strategy/BookStore.hpp"
#include "venues/kalshi/KalshiClient.hpp"
#include "venues/polymarket/PolymarketClient.hpp"

using namespace artemis;

namespace {

constexpr const char* kVersion = "2.0.0";

struct Args {
    std::string command;
    std::string config;
    std::vector<std::string> positional;
    std::map<std::string, std::string> opts;
    bool has(const std::string& k) const { return opts.count(k) > 0; }
    std::string get(const std::string& k, const std::string& d = {}) const {
        auto it = opts.find(k);
        return it == opts.end() ? d : it->second;
    }
};

void usage() {
    std::printf(
        "Artemis %s - Kalshi arbitrage engine\n\n"
        "usage:\n"
        "  artemis scan     [-c config.json]\n"
        "  artemis run      [-c config.json] [--live] [-d seconds]\n"
        "  artemis record   [-c config.json] -o data/books.csv [-d seconds]\n"
        "  artemis backtest <books.csv> [-c config.json] [--latency ms] [--leg-latency ms]\n"
        "                   [--out prefix]\n"
        "  artemis balance  [-c config.json]\n\n"
        "Credentials come from KALSHI_API_KEY_ID and KALSHI_PRIVATE_KEY_PATH.\n"
        "Live trading needs BOTH \"mode\": \"live\" in the config AND --live.\n",
        kVersion);
}

Args parse(int argc, char** argv) {
    Args a;
    if (argc > 1) a.command = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string s = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (s == "-c" || s == "--config") a.config = next();
        else if (s == "-o" || s == "--output") a.opts["output"] = next();
        else if (s == "-d" || s == "--duration") a.opts["duration"] = next();
        else if (s == "--latency") a.opts["latency"] = next();
        else if (s == "--leg-latency") a.opts["leg-latency"] = next();
        else if (s == "--out") a.opts["out"] = next();
        else if (s == "--universe") a.opts["universe"] = next();
        else if (s == "--live") a.opts["live"] = "1";
        else if (s == "-v" || s == "--verbose") a.opts["verbose"] = "1";
        else a.positional.push_back(s);
    }
    return a;
}

Config loadConfig(const Args& a) {
    std::string path = a.config;
    if (path.empty() && std::filesystem::exists("config/artemis.json")) path = "config/artemis.json";
    Config c;
    if (!path.empty()) {
        c = Config::load(path);
    } else {
        c.applyEnv();
        c.validate();
    }
    return c;
}

void setupLogging(const Config& c, bool verbose) {
    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    if (!c.output.logFile.empty()) {
        ensureParentDir(c.output.logFile);
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            c.output.logFile, 20 * 1024 * 1024, 5));
    }
    auto logger = std::make_shared<spdlog::logger>("artemis", sinks.begin(), sinks.end());
    logger->set_level(verbose ? spdlog::level::debug : spdlog::level::from_str(c.output.logLevel));
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(logger);
    spdlog::flush_every(std::chrono::seconds(2));
}

void onSignal(int) { Engine::requestStop(); }

std::shared_ptr<KalshiClient> makeKalshi(const Config& cfg) {
    std::shared_ptr<KalshiSigner> signer;
    if (!cfg.kalshi.apiKeyId.empty() && !cfg.kalshi.privateKeyPath.empty())
        signer = KalshiSigner::fromPemFile(cfg.kalshi.apiKeyId, cfg.kalshi.privateKeyPath);
    return std::make_shared<KalshiClient>(
        cfg.kalshi, signer, std::make_shared<RateLimiter>(cfg.kalshi.readRatePerSec, 2.0),
        std::make_shared<RateLimiter>(cfg.kalshi.writeRatePerSec, 1.0));
}

int cmdScan(const Config& cfgIn) {
    Config cfg = cfgIn;
    cfg.strategy.confirmations = 1;
    auto kalshi = makeKalshi(cfg);
    std::vector<MarketPair> pairs;
    if (cfg.polymarket.enabled) pairs = loadMarketMap(cfg.polymarket.marketMapPath);
    UniverseBuilder builder(cfg, kalshi);
    auto uni = builder.build(pairs);

    BookStore store;
    for (auto& b : kalshi->orderbooks(uni->kalshiTickers())) store.put(std::move(b));
    if (!pairs.empty()) {
        PolymarketClient poly(cfg.polymarket,
                              std::make_shared<RateLimiter>(cfg.polymarket.readRatePerSec, 2.0));
        for (auto& b : poly.pairBooks(pairs)) store.put(std::move(b));
    }
    ArbDetector det(cfg.strategy, cfg.feed, cfg.polymarket, cfg.basis);
    std::unordered_map<std::string, FeeModel> fees;
    for (const auto& [t, m] : uni->tickerFeeMultiplier)
        fees[t] = FeeModel::kalshi(cfg.strategy.kalshiTakerRate, m);
    det.setTickerFees(std::move(fees));
    auto opps = det.scan(uni->events, uni->pairs, store, nowUs());

    std::printf("\n%-12s %-34s %8s %10s %9s %9s %7s  legs\n", "kind", "group", "units", "cost",
                "fees", "profit", "roi%");
    for (const auto& o : opps) {
        std::printf("%-12s %-34s %8s %10s %9s %9s %7.2f  %s\n", toString(o.kind),
                    o.groupId.substr(0, 34).c_str(), fp::formatCount(o.units).c_str(),
                    fp::usd(o.cost).c_str(), fp::usd(o.fees).c_str(), fp::usd(o.profit).c_str(),
                    o.roi * 100, legsSummary(o).c_str());
    }
    std::printf("\n%zu opportunities across %zu events / %zu pairs (%zu books)\n", opps.size(),
                uni->events.size(), uni->pairs.size(), store.size());

    // closest misses help tune thresholds
    std::vector<std::pair<Micros, const EventInfo*>> near;
    for (const auto& e : uni->events) near.push_back({noBasketGap(e), &e});
    std::sort(near.begin(), near.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    std::printf("\nclosest events by listing NO-basket gap (positive = gross arb before fees):\n");
    for (std::size_t i = 0; i < std::min<std::size_t>(10, near.size()); ++i) {
        std::printf("  %+7.2fc  %-30s %s\n", static_cast<double>(near[i].first) / kOneCent,
                    near[i].second->eventTicker.c_str(), near[i].second->title.substr(0, 60).c_str());
    }
    return 0;
}

int cmdBalance(const Config& cfg) {
    if (cfg.kalshi.apiKeyId.empty() || cfg.kalshi.privateKeyPath.empty()) {
        std::fprintf(stderr, "set KALSHI_API_KEY_ID and KALSHI_PRIVATE_KEY_PATH first\n");
        return 2;
    }
    auto kalshi = makeKalshi(cfg);
    const ExchangeStatus st = kalshi->exchangeStatus();
    std::printf("exchange_active=%d trading_active=%d (%s)\n", st.exchangeActive, st.tradingActive,
                kalshi->baseUrl().c_str());
    auto bal = kalshi->balance();
    if (!bal) {
        std::fprintf(stderr, "balance request failed - check key id / private key / environment\n");
        return 1;
    }
    std::printf("balance: %s\n", fp::usd(*bal).c_str());
    for (const auto& [t, q] : kalshi->positions())
        std::printf("  %-40s %s %s\n", t.c_str(), q > 0 ? "YES" : "NO",
                    fp::formatCount(q > 0 ? q : -q).c_str());
    return 0;
}

int cmdBacktest(const Args& a, Config cfg) {
    if (a.positional.empty()) {
        std::fprintf(stderr, "backtest needs a recording, e.g. data/sample_books.csv\n");
        return 2;
    }
    if (a.has("latency")) cfg.backtest.latencyMs = std::stoi(a.get("latency"));
    if (a.has("leg-latency")) cfg.backtest.legLatencyMs = std::stoi(a.get("leg-latency"));
    const std::string prefix = a.get("out", cfg.output.dir + "/" + cfg.backtest.resultsPrefix);
    Backtester bt(cfg);
    bt.setOutputPrefix(prefix);
    const BacktestResult r = bt.run(a.positional[0], a.get("universe"));
    bt.writeOutputs(r, prefix);
    std::printf(
        "\n== Artemis backtest: %s ==\n"
        "rows %lld | ticks %lld | latency %d ms (+%d ms per leg)\n"
        "opportunities %lld | executed %lld | completed %lld | leg failures %lld (unwound %lld, "
        "stranded %lld) | not filled %lld | risk-rejected %lld\n"
        "expected $%.2f | locked $%.2f | realized $%.2f | capture %.1f%%\n"
        "equity $%.2f -> $%.2f (%+.2f%%) | Sharpe %.2f | Max DD $%.2f (%.2f%%)\n"
        "tick-to-signal %.1f us mean / %.1f us p99 | %.0f rows/sec\n"
        "outputs: %s_{equity,trades}.csv, %s_summary.json\n",
        a.positional[0].c_str(), r.rows, r.ticks, cfg.backtest.latencyMs,
        cfg.backtest.legLatencyMs, r.opportunities, r.executions, r.completed, r.failures,
        r.unwound, r.stranded, r.notFilled, r.rejected, r.expectedProfit,
        r.lockedProfit, r.realizedPnl, r.captureRatio * 100, r.perf.startEquity, r.perf.endEquity,
        r.perf.returnPct, r.perf.sharpe, r.perf.maxDrawdown, r.perf.maxDrawdownPct, r.evalMeanUs,
        r.evalP99Us, r.rowsPerSec, prefix.c_str(), prefix.c_str());
    for (const auto& [why, count] : r.rejectReasons) std::printf("  risk-rejected %-28s %lld\n", why.c_str(), count);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const Args a = parse(argc, argv);
    if (a.command.empty() || a.command == "help" || a.command == "-h" || a.command == "--help") {
        usage();
        return a.command.empty() ? 1 : 0;
    }
    if (a.command == "version" || a.command == "--version") {
        std::printf("artemis %s\n", kVersion);
        return 0;
    }

    Config cfg;
    try {
        cfg = loadConfig(a);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 2;
    }
    setupLogging(cfg, a.has("verbose"));
    HttpClient::globalInit();
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    int rc = 0;
    try {
        if (a.command == "scan") {
            rc = cmdScan(cfg);
        } else if (a.command == "balance") {
            rc = cmdBalance(cfg);
        } else if (a.command == "backtest") {
            rc = cmdBacktest(a, cfg);
        } else if (a.command == "run" || a.command == "record") {
            EngineOptions o;
            o.durationSec = a.has("duration") ? std::stoi(a.get("duration")) : 0;
            if (a.command == "record") {
                o.execute = false;
                o.recordPath = a.get("output", "data/recording.csv");
            } else {
                o.recordPath = a.get("output");
                const bool wantLive = a.has("live");
                if (wantLive && !cfg.live()) {
                    spdlog::error("--live given but config mode is '{}'; refusing", cfg.mode);
                    return 2;
                }
                if (cfg.live() && !wantLive) {
                    spdlog::warn("config mode is live but --live was not passed: paper trading");
                    cfg.mode = "paper";
                }
                if (cfg.live()) spdlog::warn("LIVE TRADING ENABLED on {}", cfg.kalshi.resolvedBaseUrl());
            }
            Engine engine(cfg, o);
            rc = engine.run();
        } else {
            usage();
            rc = 1;
        }
    } catch (const std::exception& e) {
        spdlog::critical("fatal: {}", e.what());
        rc = 1;
    }
    spdlog::shutdown();
    HttpClient::globalCleanup();
    return rc;
}
