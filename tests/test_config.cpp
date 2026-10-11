#include <gtest/gtest.h>

#include <cstdlib>

#include "core/Config.hpp"

using namespace artemis;

TEST(Config, DefaultsAreValid) {
    Config c;
    EXPECT_NO_THROW(c.validate());
    EXPECT_FALSE(c.live());
    EXPECT_EQ(c.kalshi.resolvedBaseUrl(), "https://api.elections.kalshi.com/trade-api/v2");
    c.kalshi.environment = "demo";
    EXPECT_EQ(c.kalshi.resolvedBaseUrl(), "https://demo-api.kalshi.co/trade-api/v2");
    c.kalshi.baseUrl = "http://localhost:9/trade-api/v2";
    EXPECT_EQ(c.kalshi.resolvedBaseUrl(), "http://localhost:9/trade-api/v2");
}

TEST(Config, ParsesJsonWithComments) {
    Config c = Config::fromJsonString(R"({
        // comment
        "mode": "paper",
        "kalshi": {"environment": "demo", "orderbook_batch_size": 25},
        "strategy": {"min_profit_usd": 1.5, "confirmations": 2},
        "universe": {"exhaustive_series": ["KXFED"]},
        "risk": {"max_daily_loss_usd": 7}
    })");
    EXPECT_EQ(c.kalshi.environment, "demo");
    EXPECT_EQ(c.kalshi.orderbookBatchSize, 25);
    EXPECT_DOUBLE_EQ(c.strategy.minProfitUsd, 1.5);
    EXPECT_EQ(c.strategy.confirmations, 2);
    ASSERT_EQ(c.universe.exhaustiveSeries.size(), 1u);
    EXPECT_DOUBLE_EQ(c.risk.maxDailyLossUsd, 7.0);
    EXPECT_EQ(c.feed.pollIntervalMs, 1000);  // untouched default
}

TEST(Config, ValidationCatchesMistakes) {
    Config c;
    c.mode = "yolo";
    EXPECT_THROW(c.validate(), std::invalid_argument);
    c = Config{};
    c.execution.legMode = "parallel";
    EXPECT_THROW(c.validate(), std::invalid_argument);
    c = Config{};
    c.kalshi.orderbookBatchSize = 500;
    EXPECT_THROW(c.validate(), std::invalid_argument);
    c = Config{};
    c.mode = "live";  // no credentials
    EXPECT_THROW(c.validate(), std::invalid_argument);
    c.kalshi.apiKeyId = "id";
    c.kalshi.privateKeyPath = "key.pem";
    EXPECT_NO_THROW(c.validate());
}

TEST(Config, EnvironmentOverrides) {
#ifdef _WIN32
    _putenv_s("KALSHI_API_KEY_ID", "from-env");
#else
    setenv("KALSHI_API_KEY_ID", "from-env", 1);
#endif
    Config c;
    c.applyEnv();
    EXPECT_EQ(c.kalshi.apiKeyId, "from-env");
#ifdef _WIN32
    _putenv_s("KALSHI_API_KEY_ID", "");
#else
    unsetenv("KALSHI_API_KEY_ID");
#endif
}

#include <filesystem>

#include "venues/polymarket/PolymarketClient.hpp"

TEST(Config, BundledExampleFilesParse) {
    if (!std::filesystem::exists("config/artemis.example.json")) GTEST_SKIP() << "run from repo root";
    Config ex = Config::load("config/artemis.example.json");
    EXPECT_EQ(ex.kalshi.environment, "demo");
    EXPECT_FALSE(ex.live());
    Config bt = Config::load("config/backtest.sample.json");
    EXPECT_FALSE(bt.universe.exhaustiveSeries.empty());
    EXPECT_EQ(loadMarketMap("config/market_map.example.json").size(), 1u);
}
