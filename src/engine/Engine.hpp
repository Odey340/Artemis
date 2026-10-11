#pragma once
// Live / paper trading engine.
//
//   feed thread  --(lock-free SPSC queue of BinaryBook)-->  strategy thread
//
// The feed thread owns all market-data I/O: universe discovery, batched
// Kalshi order-book polling and Polymarket book polling. The strategy thread
// owns every piece of trading state (book store, detector, risk, executor),
// so no locks are needed on the hot path.

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "core/Config.hpp"
#include "core/LockFreeQueue.hpp"
#include "core/OrderBook.hpp"
#include "core/RateLimiter.hpp"
#include "core/RollingStatistics.hpp"
#include "engine/Universe.hpp"

namespace artemis {

struct EngineOptions {
    bool execute = true;        // false => observe only (scan/record)
    std::string recordPath;     // non-empty => record every book to CSV
    int durationSec = 0;        // 0 => run until stopped
};

class Engine {
public:
    Engine(Config cfg, EngineOptions opts);
    int run();

    static void requestStop();
    static bool stopRequested();

private:
    void feedLoop();
    void strategyLoop();
    std::shared_ptr<UniverseSnapshot> universe();
    void publish(std::shared_ptr<UniverseSnapshot> u);

    Config cfg_;
    EngineOptions opts_;
    LockFreeQueue<BinaryBook> queue_;
    // One budget per API key: the feed and the trading client share them.
    std::shared_ptr<RateLimiter> kalshiRead_, kalshiWrite_;
    std::mutex uniMu_;
    std::shared_ptr<UniverseSnapshot> universe_;
    std::atomic<long long> feedCycleUs_{0};
    std::atomic<long long> booksFetched_{0};
    std::atomic<bool> feedDone_{false};
};

}  // namespace artemis
