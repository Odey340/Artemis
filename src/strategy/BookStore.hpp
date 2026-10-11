#pragma once
// Latest normalised book per market. Owned by the strategy thread only.
//
// Simulated fills (paper trading and back-tests) are remembered as "takes":
// when the next snapshot of that market arrives, the liquidity we already
// took is removed from it again, so the simulator cannot trade the same
// resting order twice. A take is dropped once its price level disappears or
// after `takeTtlUs`.

#include <string>
#include <unordered_map>
#include <vector>

#include "core/OrderBook.hpp"

namespace artemis {

class BookStore {
public:
    static std::string kalshiKey(const std::string& ticker) { return "K:" + ticker; }
    static std::string polyKey(const std::string& pairId) { return "P:" + pairId; }
    static std::string keyFor(const BinaryBook& b) {
        return b.venue() == Venue::Kalshi ? kalshiKey(b.id()) : polyKey(b.id());
    }

    void put(BinaryBook b) {
        std::string k = keyFor(b);
        auto t = takes_.find(k);
        if (t != takes_.end()) {
            auto& v = t->second;
            for (auto it = v.begin(); it != v.end();) {
                const bool expired = b.tsUs() > it->expiresUs;
                const Qty got = expired ? 0 : b.takeAt(it->ask, it->outcome, it->price, it->qty);
                it = (expired || got == 0) ? v.erase(it) : it + 1;
            }
            if (v.empty()) takes_.erase(t);
        }
        books_[std::move(k)] = std::move(b);
    }

    void recordTake(const std::string& key, bool askSide, Outcome o, Micros price, Qty qty,
                    std::int64_t nowUs) {
        takes_[key].push_back({askSide, o, price, qty, nowUs + takeTtlUs_});
    }
    void setTakeTtlUs(std::int64_t t) { takeTtlUs_ = t; }
    std::size_t pendingTakes() const {
        std::size_t n = 0;
        for (const auto& [k, v] : takes_) n += v.size();
        return n;
    }
    const BinaryBook* get(const std::string& key) const {
        auto it = books_.find(key);
        return it == books_.end() ? nullptr : &it->second;
    }
    BinaryBook* mut(const std::string& key) {
        auto it = books_.find(key);
        return it == books_.end() ? nullptr : &it->second;
    }
    std::size_t size() const { return books_.size(); }
    void clear() {
        books_.clear();
        takes_.clear();
    }

private:
    struct Take {
        bool ask;
        Outcome outcome;
        Micros price;
        Qty qty;
        std::int64_t expiresUs;
    };
    std::unordered_map<std::string, BinaryBook> books_;
    std::unordered_map<std::string, std::vector<Take>> takes_;
    std::int64_t takeTtlUs_ = 60'000'000;
};

}  // namespace artemis
