#include "core/Config.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace artemis {

using nlohmann::json;

namespace {

template <typename T>
void get(const json& j, const char* key, T& out) {
    auto it = j.find(key);
    if (it != j.end() && !it->is_null()) out = it->get<T>();
}

const json& section(const json& j, const char* key) {
    static const json empty = json::object();
    auto it = j.find(key);
    return (it != j.end() && it->is_object()) ? *it : empty;
}

std::string envOr(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}

}  // namespace

std::string KalshiConfig::resolvedBaseUrl() const {
    if (!baseUrl.empty()) return baseUrl;
    if (environment == "demo") return "https://demo-api.kalshi.co/trade-api/v2";
    return "https://api.elections.kalshi.com/trade-api/v2";
}

Config Config::fromJsonString(const std::string& text) {
    Config c;
    const json root = json::parse(text, nullptr, true, /*ignore_comments=*/true);

    get(root, "mode", c.mode);

    const json& k = section(root, "kalshi");
    get(k, "environment", c.kalshi.environment);
    get(k, "base_url", c.kalshi.baseUrl);
    get(k, "api_key_id", c.kalshi.apiKeyId);
    get(k, "private_key_path", c.kalshi.privateKeyPath);
    get(k, "read_rate_per_sec", c.kalshi.readRatePerSec);
    get(k, "write_rate_per_sec", c.kalshi.writeRatePerSec);
    get(k, "timeout_ms", c.kalshi.timeoutMs);
    get(k, "max_retries", c.kalshi.maxRetries);
    get(k, "orderbook_batch_size", c.kalshi.orderbookBatchSize);
    get(k, "subaccount", c.kalshi.subaccount);

    const json& p = section(root, "polymarket");
    get(p, "enabled", c.polymarket.enabled);
    get(p, "clob_url", c.polymarket.clobUrl);
    get(p, "read_rate_per_sec", c.polymarket.readRatePerSec);
    get(p, "timeout_ms", c.polymarket.timeoutMs);
    get(p, "market_map_path", c.polymarket.marketMapPath);
    get(p, "fee_model", c.polymarket.feeModel);
    get(p, "fee_bps", c.polymarket.feeBps);
    get(p, "quadratic_theta", c.polymarket.quadraticTheta);

    const json& u = section(root, "universe");
    get(u, "refresh_seconds", c.universe.refreshSeconds);
    get(u, "series_allow", c.universe.seriesAllow);
    get(u, "series_deny", c.universe.seriesDeny);
    get(u, "event_allow", c.universe.eventAllow);
    get(u, "event_deny", c.universe.eventDeny);
    get(u, "exhaustive_events", c.universe.exhaustiveEvents);
    get(u, "exhaustive_series", c.universe.exhaustiveSeries);
    get(u, "min_markets", c.universe.minMarkets);
    get(u, "max_markets", c.universe.maxMarkets);
    get(u, "max_events_tracked", c.universe.maxEventsTracked);
    get(u, "prefilter_slack_cents", c.universe.prefilterSlackCents);
    get(u, "min_seconds_to_close", c.universe.minSecondsToClose);
    get(u, "max_pages", c.universe.maxPages);

    const json& f = section(root, "feed");
    get(f, "poll_interval_ms", c.feed.pollIntervalMs);
    get(f, "max_book_age_ms", c.feed.maxBookAgeMs);
    get(f, "queue_capacity", c.feed.queueCapacity);

    const json& s = section(root, "strategy");
    get(s, "enable_no_basket", c.strategy.enableNoBasket);
    get(s, "enable_yes_basket", c.strategy.enableYesBasket);
    get(s, "enable_cross_venue", c.strategy.enableCrossVenue);
    get(s, "min_profit_usd", c.strategy.minProfitUsd);
    get(s, "min_edge_cents", c.strategy.minEdgeCents);
    get(s, "min_roi", c.strategy.minRoi);
    get(s, "min_leg_roi", c.strategy.minLegRoi);
    get(s, "max_contracts", c.strategy.maxContracts);
    get(s, "confirmations", c.strategy.confirmations);
    get(s, "kalshi_taker_rate", c.strategy.kalshiTakerRate);
    get(s, "safety_margin_cents", c.strategy.safetyMarginCents);

    const json& b = section(root, "basis");
    get(b, "enabled", c.basis.enabled);
    get(b, "half_life", c.basis.halfLife);
    get(b, "warmup", c.basis.warmup);
    get(b, "entry_z", c.basis.entryZ);
    get(b, "exit_z", c.basis.exitZ);

    const json& r = section(root, "risk");
    get(r, "max_cost_per_trade_usd", c.risk.maxCostPerTradeUsd);
    get(r, "max_total_exposure_usd", c.risk.maxTotalExposureUsd);
    get(r, "max_daily_loss_usd", c.risk.maxDailyLossUsd);
    get(r, "max_trades_per_hour", c.risk.maxTradesPerHour);
    get(r, "cooldown_ms", c.risk.cooldownMs);
    get(r, "max_open_baskets_per_event", c.risk.maxOpenBasketsPerEvent);
    get(r, "balance_utilisation", c.risk.balanceUtilisation);
    get(r, "kill_switch_file", c.risk.killSwitchFile);
    get(r, "max_consecutive_failures", c.risk.maxConsecutiveFailures);

    const json& e = section(root, "execution");
    get(e, "leg_mode", c.execution.legMode);
    get(e, "max_completion_retries", c.execution.maxCompletionRetries);
    get(e, "unwind_on_failure", c.execution.unwindOnFailure);
    get(e, "max_leg_loss_usd", c.execution.maxLegLossUsd);

    const json& bt = section(root, "backtest");
    get(bt, "latency_ms", c.backtest.latencyMs);
    get(bt, "leg_latency_ms", c.backtest.legLatencyMs);
    get(bt, "starting_equity_usd", c.backtest.startingEquityUsd);
    get(bt, "results_prefix", c.backtest.resultsPrefix);

    const json& o = section(root, "output");
    get(o, "dir", c.output.dir);
    get(o, "log_file", c.output.logFile);
    get(o, "log_level", c.output.logLevel);

    return c;
}

Config Config::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open config file: " + path);
    std::stringstream ss;
    ss << in.rdbuf();
    Config c = fromJsonString(ss.str());
    c.applyEnv();
    c.validate();
    return c;
}

void Config::applyEnv() {
    kalshi.apiKeyId = envOr("KALSHI_API_KEY_ID", kalshi.apiKeyId);
    kalshi.privateKeyPath = envOr("KALSHI_PRIVATE_KEY_PATH", kalshi.privateKeyPath);
    kalshi.environment = envOr("KALSHI_ENV", kalshi.environment);
    mode = envOr("ARTEMIS_MODE", mode);
}

void Config::validate() const {
    auto fail = [](const std::string& m) { throw std::invalid_argument("config: " + m); };
    if (mode != "paper" && mode != "live") fail("mode must be 'paper' or 'live'");
    if (kalshi.environment != "prod" && kalshi.environment != "demo")
        fail("kalshi.environment must be 'prod' or 'demo'");
    if (kalshi.readRatePerSec <= 0 || kalshi.writeRatePerSec <= 0) fail("rate limits must be > 0");
    if (kalshi.orderbookBatchSize < 1 || kalshi.orderbookBatchSize > 100)
        fail("kalshi.orderbook_batch_size must be in [1, 100]");
    if (polymarket.feeModel != "bps" && polymarket.feeModel != "quadratic")
        fail("polymarket.fee_model must be 'bps' or 'quadratic'");
    if (feed.pollIntervalMs < 50) fail("feed.poll_interval_ms must be >= 50");
    if (strategy.maxContracts < 1) fail("strategy.max_contracts must be >= 1");
    if (strategy.confirmations < 1) fail("strategy.confirmations must be >= 1");
    if (strategy.kalshiTakerRate < 0 || strategy.kalshiTakerRate > 0.5)
        fail("strategy.kalshi_taker_rate looks wrong");
    if (risk.balanceUtilisation <= 0 || risk.balanceUtilisation > 1)
        fail("risk.balance_utilisation must be in (0, 1]");
    if (execution.legMode != "sequential" && execution.legMode != "batch")
        fail("execution.leg_mode must be 'sequential' or 'batch'");
    if (live() && (kalshi.apiKeyId.empty() || kalshi.privateKeyPath.empty()))
        fail("live mode needs KALSHI_API_KEY_ID and KALSHI_PRIVATE_KEY_PATH");
}

}  // namespace artemis
