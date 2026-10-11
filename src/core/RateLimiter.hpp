#pragma once
// Thread-safe token bucket. Kalshi enforces separate read / write budgets per
// API tier, so each client holds one bucket for each.

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>

namespace artemis {

class RateLimiter {
public:
    using Clock = std::chrono::steady_clock;

    RateLimiter(double ratePerSec, double burst)
        : rate_(std::max(ratePerSec, 0.001)), burst_(std::max(burst, 1.0)), tokens_(burst_),
          last_(Clock::now()) {}

    // Block until `n` tokens are available.
    void acquire(double n = 1.0) {
        for (;;) {
            std::chrono::duration<double> wait{0};
            {
                std::lock_guard<std::mutex> lk(mu_);
                refill();
                if (tokens_ >= n) {
                    tokens_ -= n;
                    return;
                }
                wait = std::chrono::duration<double>((n - tokens_) / rate_);
            }
            std::this_thread::sleep_for(wait);
        }
    }

    bool tryAcquire(double n = 1.0) {
        std::lock_guard<std::mutex> lk(mu_);
        refill();
        if (tokens_ >= n) {
            tokens_ -= n;
            return true;
        }
        return false;
    }

    // Called when the server says we are going too fast (HTTP 429).
    void penalise(double seconds) {
        std::lock_guard<std::mutex> lk(mu_);
        refill();
        tokens_ = std::min(tokens_, 0.0) - seconds * rate_;
    }

    double rate() const { return rate_; }

private:
    void refill() {
        const auto now = Clock::now();
        const double dt = std::chrono::duration<double>(now - last_).count();
        last_ = now;
        tokens_ = std::min(burst_, tokens_ + dt * rate_);
    }

    std::mutex mu_;
    double rate_;
    double burst_;
    double tokens_;
    Clock::time_point last_;
};

}  // namespace artemis
