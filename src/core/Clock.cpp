#include "core/Clock.hpp"

#include <cstdio>
#include <cstdlib>

namespace artemis {

namespace {
// days from civil (Howard Hinnant's algorithm) — portable timegm replacement
std::int64_t daysFromCivil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}
}  // namespace

std::int64_t parseRfc3339Us(const std::string& s) {
    int Y = 0, M = 0, D = 0, h = 0, mi = 0, sec = 0;
    if (s.size() < 19) return 0;
    if (std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &h, &mi, &sec) != 6) {
        return 0;
    }
    std::size_t pos = 19;
    std::int64_t fracUs = 0;
    if (pos < s.size() && s[pos] == '.') {
        ++pos;
        int digits = 0;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
            if (digits < 6) {
                fracUs = fracUs * 10 + (s[pos] - '0');
                ++digits;
            }
            ++pos;
        }
        for (; digits < 6; ++digits) fracUs *= 10;
    }
    std::int64_t offsetSec = 0;
    if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) {
        int oh = 0, om = 0;
        std::sscanf(s.c_str() + pos + 1, "%2d:%2d", &oh, &om);
        offsetSec = (oh * 3600 + om * 60) * (s[pos] == '+' ? 1 : -1);
    }
    const std::int64_t days = daysFromCivil(Y, static_cast<unsigned>(M), static_cast<unsigned>(D));
    const std::int64_t secs = days * 86400 + h * 3600 + mi * 60 + sec - offsetSec;
    return secs * 1'000'000 + fracUs;
}

}  // namespace artemis
