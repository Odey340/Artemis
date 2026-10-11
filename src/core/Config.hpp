#pragma once
// Runtime configuration. Loaded from JSON (see config/artemis.example.json);
// secrets are taken from environment variables so they never live in the repo.

#include <string>
#include <vector>

namespace artemis {

struct KalshiConfig {
    std::string environment = "prod";  // "prod" | "demo"
    std::string baseUrl;                // optional override
    std::string apiKeyId;               // env KALSHI_API_KEY_ID
    std::string privateKeyPath;         // env KALSHI_PRIVATE_KEY_PATH
    double readRatePerSec = 15.0;       // stay under the Basic tier budget
    double writeRatePerSec = 8.0;
    int timeoutMs = 5000;
    int maxRetries = 3;
    int orderbookBatchSize = 50;
    int subaccount = 0;

    std::string resolvedBaseUrl() const;
};

struct PolymarketConfig {
    bool enabled = false;
    std::string clobUrl = "https://clob.polymarket.com";
    double readRatePerSec = 10.0;
    int timeoutMs = 5000;
    std::string marketMapPath = "config/market_map.json";
    // Fee model for the Polymarket leg: "bps" -> bps * C * min(p, 1-p) (CLOB),
    // "quadratic" -> theta * C * p * (1-p) (Polymarket US schedule).
    std::string feeModel = "bps";
    double feeBps = 0.0;
    double quadraticTheta = 0.0695;
};

struct UniverseConfig {
    int refreshSeconds = 60;
    std::vector<std::string> seriesAllow, seriesDeny, eventAllow, eventDeny;
    // Events/series whose outcomes are known to be collectively exhaustive.
    // Only these are eligible for YES-basket arbitrage.
    std::vector<std::string> exhaustiveEvents, exhaustiveSeries;
    int minMarkets = 2;
    int maxMarkets = 80;
    int maxEventsTracked = 250;
    double prefilterSlackCents = 4.0;  // poll books for events within this of break-even
    int minSecondsToClose = 0;
    int maxPages = 50;
};

struct FeedConfig {
    int pollIntervalMs = 1000;
    int maxBookAgeMs = 3000;
    int queueCapacity = 4096;
};

struct StrategyConfig {
    bool enableNoBasket = true;
    bool enableYesBasket = true;
    bool enableCrossVenue = true;
    double minProfitUsd = 0.10;    // per opportunity, after fees
    double minEdgeCents = 0.5;     // per basket unit (per contract of each leg), after fees
    double minRoi = 0.0;           // profit / cost
    double minLegRoi = 0.02;       // NO basket: a leg's own edge / its price to be included
    int maxContracts = 250;        // per leg
    int confirmations = 1;         // consecutive snapshots that must show the edge
    double kalshiTakerRate = 0.07;
    double safetyMarginCents = 0.0;  // extra per-leg cost buffer for slippage
};

struct BasisConfig {
    bool enabled = true;
    double halfLife = 120.0;
    int warmup = 30;
    double entryZ = 2.5;
    double exitZ = 0.5;
};

struct RiskConfig {
    double maxCostPerTradeUsd = 100.0;
    double maxTotalExposureUsd = 1000.0;
    double maxDailyLossUsd = 50.0;
    int maxTradesPerHour = 60;
    int cooldownMs = 5000;
    int maxOpenBasketsPerEvent = 3;
    double balanceUtilisation = 0.8;
    std::string killSwitchFile = "KILL";
    int maxConsecutiveFailures = 3;
};

struct ExecutionConfig {
    std::string legMode = "sequential";  // "sequential" | "batch"
    int maxCompletionRetries = 1;
    bool unwindOnFailure = true;
    double maxLegLossUsd = 5.0;  // accept completing a basket at up to this loss vs unwinding
};

struct BacktestConfig {
    int latencyMs = 250;     // detection -> first order reaches the exchange
    int legLatencyMs = 50;   // additional delay before each order (legs are sequential)
    double startingEquityUsd = 1000.0;
    std::string resultsPrefix = "results";
};

struct OutputConfig {
    std::string dir = "output";
    std::string logFile = "output/artemis.log";
    std::string logLevel = "info";
};

struct Config {
    std::string mode = "paper";  // "paper" | "live"
    KalshiConfig kalshi;
    PolymarketConfig polymarket;
    UniverseConfig universe;
    FeedConfig feed;
    StrategyConfig strategy;
    BasisConfig basis;
    RiskConfig risk;
    ExecutionConfig execution;
    BacktestConfig backtest;
    OutputConfig output;

    bool live() const { return mode == "live"; }

    // Load from a JSON file (missing keys keep defaults), then apply env overrides.
    static Config load(const std::string& path);
    static Config fromJsonString(const std::string& json);
    void applyEnv();
    // Throws std::invalid_argument describing the first invalid setting.
    void validate() const;
};

}  // namespace artemis
