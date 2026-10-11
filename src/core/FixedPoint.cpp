#include "core/FixedPoint.hpp"

#include <cstdio>
#include <cstdlib>

namespace artemis::fp {

std::optional<std::int64_t> parseScaled(std::string_view s, int scale) {
    // trim whitespace
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    if (s.empty()) return std::nullopt;

    bool neg = false;
    if (s.front() == '-' || s.front() == '+') {
        neg = s.front() == '-';
        s.remove_prefix(1);
        if (s.empty()) return std::nullopt;
    }

    std::int64_t whole = 0;
    std::int64_t frac = 0;
    int fracDigits = 0;
    bool seenDot = false;
    bool seenDigit = false;

    for (char c : s) {
        if (c == '.') {
            if (seenDot) return std::nullopt;
            seenDot = true;
            continue;
        }
        if (c < '0' || c > '9') return std::nullopt;
        seenDigit = true;
        const int d = c - '0';
        if (!seenDot) {
            if (whole > (INT64_MAX - d) / 10) return std::nullopt;  // overflow
            whole = whole * 10 + d;
        } else if (fracDigits < scale) {
            frac = frac * 10 + d;
            ++fracDigits;
        }
        // digits beyond `scale` are truncated
    }
    if (!seenDigit) return std::nullopt;

    std::int64_t mult = 1;
    for (int i = 0; i < scale; ++i) mult *= 10;
    for (int i = fracDigits; i < scale; ++i) frac *= 10;

    if (whole > (INT64_MAX - frac) / mult) return std::nullopt;
    const std::int64_t v = whole * mult + frac;
    return neg ? -v : v;
}

std::optional<Micros> parseDollars(std::string_view s) { return parseScaled(s, 6); }
std::optional<Qty> parseCount(std::string_view s) { return parseScaled(s, 2); }

std::string formatDollars(Micros m, int decimals) {
    if (decimals < 0) decimals = 0;
    if (decimals > 6) decimals = 6;
    const bool neg = m < 0;
    std::int64_t a = neg ? -m : m;
    std::int64_t div = 1;
    for (int i = decimals; i < 6; ++i) div *= 10;
    a /= div;  // truncate to requested precision
    std::int64_t scale = 1;
    for (int i = 0; i < decimals; ++i) scale *= 10;
    char buf[64];
    if (decimals == 0) {
        std::snprintf(buf, sizeof(buf), "%s%lld", neg ? "-" : "", static_cast<long long>(a));
    } else {
        std::snprintf(buf, sizeof(buf), "%s%lld.%0*lld", neg ? "-" : "",
                      static_cast<long long>(a / scale), decimals,
                      static_cast<long long>(a % scale));
    }
    return buf;
}

std::string formatCount(Qty q) {
    const bool neg = q < 0;
    const std::int64_t a = neg ? -q : q;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%s%lld.%02lld", neg ? "-" : "",
                  static_cast<long long>(a / 100), static_cast<long long>(a % 100));
    return buf;
}

std::string usd(Micros m) {
    const bool neg = m < 0;
    std::int64_t a = neg ? -m : m;
    // round half up to the cent for display
    a = (a + kOneCent / 2) / kOneCent;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%s$%lld.%02lld", neg ? "-" : "",
                  static_cast<long long>(a / 100), static_cast<long long>(a % 100));
    return buf;
}

std::string cents(Micros m) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1fc", static_cast<double>(m) / kOneCent);
    return buf;
}

}  // namespace artemis::fp
