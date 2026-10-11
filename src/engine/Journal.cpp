#include "engine/Journal.hpp"

#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <vector>

#include "core/FixedPoint.hpp"

namespace artemis {

void ensureParentDir(const std::string& path) {
    const std::filesystem::path p(path);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }
}

namespace {
std::ofstream openCsv(const std::string& path, const char* header) {
    ensureParentDir(path);
    const bool exists = std::filesystem::exists(path) && std::filesystem::file_size(path) > 0;
    std::ofstream f(path, std::ios::app);
    if (!exists) f << header << '\n';
    return f;
}

std::string csvSafe(std::string s) {
    for (auto& c : s)
        if (c == ',' || c == '\n' || c == '\r' || c == '"') c = ' ';
    return s;
}
}  // namespace

BookRecorder::BookRecorder(const std::string& path) {
    ensureParentDir(path);
    out_.open(path, std::ios::trunc);
    out_ << header() << '\n';
}

void BookRecorder::write(const BinaryBook& b) {
    // Kalshi asks are implied by the opposite side's bids, so only bids are stored.
    const bool m = b.mirrored();
    out_ << b.tsUs() << ',' << toString(b.venue()) << ',' << b.id() << ','
         << encodeLadder(b.bids(Outcome::Yes)) << ','
         << (m ? std::string() : encodeLadder(b.asks(Outcome::Yes))) << ','
         << encodeLadder(b.bids(Outcome::No)) << ','
         << (m ? std::string() : encodeLadder(b.asks(Outcome::No))) << '\n';
}

bool BookRecorder::parse(const std::string& line, BinaryBook& out) {
    if (line.empty() || line[0] < '0' || line[0] > '9') return false;
    std::vector<std::string> f;
    std::size_t pos = 0;
    while (true) {
        const std::size_t c = line.find(',', pos);
        f.push_back(line.substr(pos, c == std::string::npos ? std::string::npos : c - pos));
        if (c == std::string::npos) break;
        pos = c + 1;
    }
    if (f.size() < 7) return false;
    for (auto& s : f)
        while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) s.pop_back();
    const std::int64_t ts = std::strtoll(f[0].c_str(), nullptr, 10);
    if (f[1] == "kalshi") {
        out = BinaryBook::fromKalshi(f[2], decodeLadder(f[3]), decodeLadder(f[5]), ts);
    } else {
        out = BinaryBook::fromTwoTokens(Venue::Polymarket, f[2], decodeLadder(f[3]),
                                        decodeLadder(f[4]), decodeLadder(f[5]), decodeLadder(f[6]),
                                        ts);
    }
    return true;
}

std::string legsSummary(const Opportunity& o) {
    std::ostringstream ss;
    for (std::size_t i = 0; i < o.legs.size(); ++i) {
        const Leg& l = o.legs[i];
        if (i) ss << ' ';
        ss << (l.venue == Venue::Kalshi ? "K:" : "P:") << l.marketId << ':' << toString(l.outcome)
           << '@' << fp::formatDollars(l.bestPrice, 4);
        if (l.limitPrice != l.bestPrice) ss << '-' << fp::formatDollars(l.limitPrice, 4);
    }
    return ss.str();
}

OpportunityLog::OpportunityLog(const std::string& path)
    : out_(openCsv(path,
                   "ts_us,kind,group,label,legs,units,payout,cost,fees,profit,edge_per_unit,roi,"
                   "confirmations,book_age_ms,decision")) {}

void OpportunityLog::write(const Opportunity& o, const std::string& decision) {
    out_ << o.detectedTsUs << ',' << toString(o.kind) << ',' << o.groupId << ','
         << csvSafe(o.label) << ',' << legsSummary(o) << ',' << fp::formatCount(o.units) << ','
         << fp::formatDollars(o.payout, 4) << ',' << fp::formatDollars(o.cost, 4) << ','
         << fp::formatDollars(o.fees, 4) << ',' << fp::formatDollars(o.profit, 4) << ','
         << fp::formatDollars(o.edgePerUnit, 4) << ',' << o.roi << ',' << o.confirmations << ','
         << (o.detectedTsUs - o.oldestBookTsUs) / 1000 << ',' << csvSafe(decision) << '\n';
    out_.flush();
}

TradeLog::TradeLog(const std::string& path)
    : out_(openCsv(path,
                   "ts_us,kind,group,status,units,cost,fees,guaranteed_payout,locked_profit,"
                   "realized_pnl,stranded_cost,cash_flow,expected_profit,orders,note")) {}

void TradeLog::write(std::int64_t tsUs, const Opportunity& o, const ExecutionReport& r) {
    out_ << tsUs << ',' << toString(o.kind) << ',' << o.groupId << ',' << toString(r.status) << ','
         << fp::formatCount(r.units) << ',' << fp::formatDollars(r.cost, 4) << ','
         << fp::formatDollars(r.fees, 4) << ',' << fp::formatDollars(r.guaranteedPayout, 4) << ','
         << fp::formatDollars(r.lockedProfit, 4) << ',' << fp::formatDollars(r.realizedPnl, 4)
         << ',' << fp::formatDollars(r.strandedCost, 4) << ','
         << fp::formatDollars(r.cashFlow, 4) << ',' << fp::formatDollars(o.profit, 4) << ','
         << r.orders.size() << ',' << csvSafe(r.note) << '\n';
    out_.flush();
}

}  // namespace artemis
