#pragma once
// CSV journals: order-book recordings (replayable by the back-tester),
// detected opportunities and executed trades.

#include <fstream>
#include <mutex>
#include <string>

#include "core/OrderBook.hpp"
#include "exec/ExecutionReport.hpp"
#include "strategy/Opportunity.hpp"

namespace artemis {

class BookRecorder {
public:
    explicit BookRecorder(const std::string& path);
    void write(const BinaryBook& b);
    void flush() { out_.flush(); }
    static const char* header() { return "ts_us,venue,id,yes_bids,yes_asks,no_bids,no_asks"; }

    // Parse one CSV row. Returns false for the header / malformed rows.
    static bool parse(const std::string& line, BinaryBook& out);

private:
    std::ofstream out_;
};

class OpportunityLog {
public:
    explicit OpportunityLog(const std::string& path);
    void write(const Opportunity& o, const std::string& decision);

private:
    std::ofstream out_;
};

class TradeLog {
public:
    explicit TradeLog(const std::string& path);
    void write(std::int64_t tsUs, const Opportunity& o, const ExecutionReport& r);

private:
    std::ofstream out_;
};

std::string legsSummary(const Opportunity& o);
void ensureParentDir(const std::string& path);

}  // namespace artemis
